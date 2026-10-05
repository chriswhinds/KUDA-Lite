#!/usr/bin/env bash
# Copyright 2026 Christopher Hinds, Stratum Labs
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Deploys and operates a whole KUDA-Lite cluster over SSH, from your development machine.
#
#   deploy/deploy-cluster.sh [-i INVENTORY] [-c CONTROLLER_ADDR] [-p] COMMAND
#
# Commands:
#   install    copy this source tree to every node and run deploy/install.sh for its role(s)
#              (controller first, then workers, then the dashboard). Also used to upgrade.
#   status     service state on every node + cluster view from the controller (cl-info)
#   verify     run the acceptance tests on the controller (cl-test-memory, cl-test-matmul)
#   restart    restart the controller, then workers, then the dashboard
#   stop       stop every KUDA-Lite service
#   logs HOST  follow the KUDA-Lite logs of one node (Ctrl-C to stop)
#   render     write each node's config files and systemd units, exactly as install would create
#              them, into deploy/rendered/<host>/ for review (touches no node; needs no SSH)
#
# Options:
#   -i FILE    inventory (default deploy/cluster.inventory; see cluster.inventory.example)
#   -c ADDR    address workers use to reach the controller (default: its inventory host)
#   -p         install workers in parallel (needs passwordless sudo on the workers)
#   -x ARGS    extra arguments for install.sh, e.g. -x "--tune-network --install-node"
#   -o DIR     output directory for render (default deploy/rendered)
#
# Needs: ssh key access to every node, and sudo there. Every node needs rsync (install.sh's
# package step installs it, but the first copy needs it too).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$(cd "$here/.." && pwd)"
inventory="$here/cluster.inventory"
controller_addr=""
parallel=0
extra=""
render_dir="$here/rendered"
prefix="/usr/local"  # must match install.sh --prefix
remote_dir="kudalite-src"  # relative to the SSH user's home

log() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
usage() { sed -n '/^# limitations under the License\./,/^set -euo/p' "$0" | sed '1,2d; $d; s/^# \{0,1\}//'; exit "${1:-0}"; }

while getopts ":i:c:px:o:h" opt; do
  case "$opt" in
    i) inventory="$OPTARG" ;;
    c) controller_addr="$OPTARG" ;;
    p) parallel=1 ;;
    x) extra="$OPTARG" ;;
    o) render_dir="$OPTARG" ;;
    h) usage 0 ;;
    *) usage 64 ;;
  esac
done
shift $((OPTIND - 1))
command="${1:-}"
[[ -n "$command" ]] || usage 64

[[ -f "$inventory" ]] || die "inventory $inventory not found (cp deploy/cluster.inventory.example deploy/cluster.inventory and edit it)"

# ---- Inventory ----------------------------------------------------------------------------------

# Plain arrays only: this runs on macOS's bash 3.2 as well as on Linux.
controller_host=""
workers=() dashboards=() known_hosts=() known_users=()
while read -r role host user _ || [[ -n "${role:-}" ]]; do
  [[ -z "${role:-}" || "$role" == \#* ]] && continue
  [[ -n "${host:-}" ]] || die "inventory line for role '$role' has no host"
  known_hosts+=("$host")
  known_users+=("${user:-$USER}")
  case "$role" in
    controller)
      [[ -z "$controller_host" ]] || die "inventory has more than one controller"
      controller_host="$host" ;;
    worker) workers+=("$host") ;;
    dashboard) dashboards+=("$host") ;;
    *) die "unknown role '$role' in $inventory" ;;
  esac
done <"$inventory"
[[ -n "$controller_host" ]] || die "inventory has no controller"
[[ -n "$controller_addr" ]] || controller_addr="$controller_host"

# "${arr[@]+...}" expands to nothing for an empty array under set -u, even on bash 3.2.
each() { eval "printf '%s\n' \${$1[@]+\"\${$1[@]}\"}"; }

user_of() {
  local i
  for ((i = 0; i < ${#known_hosts[@]}; i++)); do
    if [[ "${known_hosts[$i]}" == "$1" ]]; then echo "${known_users[$i]}"; return 0; fi
  done
  return 1
}
target() { echo "$(user_of "$1")@$1"; }
remote() { local host="$1"; shift; ssh -o ConnectTimeout=10 "$(target "$host")" "$@"; }
remote_tty() { local host="$1"; shift; ssh -t -o ConnectTimeout=10 "$(target "$host")" "$@"; }

all_hosts() { { echo "$controller_host"; each workers; each dashboards; } | awk 'NF && !seen[$0]++'; }

units_of() {  # services that run on a host, according to the inventory
  local host="$1" u=""
  if [[ "$host" == "$controller_host" ]]; then u+=" kudalite-controller"; fi
  if grep -qxF "$host" <<<"$(each workers)"; then u+=" kudalite-worker"; fi
  if grep -qxF "$host" <<<"$(each dashboards)"; then u+=" kudalite-dashboard-api kudalite-dashboard-web"; fi
  echo "$u"
}

# ---- Commands -----------------------------------------------------------------------------------

sync_source() {
  log "copying source to $1:~/$remote_dir"
  rsync -az --delete \
    --exclude .git --exclude build/ --exclude node_modules --exclude .next --exclude .venv \
    --exclude __pycache__ --exclude deploy/cluster.inventory \
    "$src/" "$(target "$1"):$remote_dir/"
}

install_role() {  # host role
  local host="$1" role="$2"
  log "installing $role on $host"
  # shellcheck disable=SC2086  # $extra is intentionally word-split into options
  remote_tty "$host" "sudo bash ~/$remote_dir/deploy/install.sh $role --controller $controller_addr $extra"
}

synced=" "
sync_once() {
  if [[ "$synced" != *" $1 "* ]]; then
    sync_source "$1"
    synced+="$1 "
  fi
}

cmd_install() {
  sync_once "$controller_host"
  install_role "$controller_host" controller

  if [[ $parallel == 1 && ${#workers[@]} -gt 1 ]]; then
    local logdir pids=() failed=0 w
    logdir="$(mktemp -d)"
    for w in $(each workers); do
      sync_once "$w"
      (ssh -o BatchMode=yes "$(target "$w")" \
        "sudo -n bash ~/$remote_dir/deploy/install.sh worker --controller $controller_addr $extra" \
        >"$logdir/$w.log" 2>&1) &
      pids+=("$!:$w")
    done
    for p in "${pids[@]}"; do
      if wait "${p%%:*}"; then log "worker ${p#*:}: installed"; else
        echo "worker ${p#*:} FAILED; log: $logdir/${p#*:}.log" >&2; failed=1; fi
    done
    [[ $failed == 0 ]] || die "some workers failed to install"
  else
    for w in $(each workers); do
      sync_once "$w"
      install_role "$w" worker
    done
  fi

  for d in $(each dashboards); do
    sync_once "$d"
    local addr="$controller_addr"
    [[ "$d" == "$controller_host" ]] && addr=127.0.0.1
    log "installing dashboard on $d"
    # shellcheck disable=SC2086
    remote_tty "$d" "sudo bash ~/$remote_dir/deploy/install.sh dashboard --controller $addr $extra"
  done
  log "install complete"
  cmd_status
}

cmd_status() {
  for h in $(all_hosts); do
    local units
    units="$(units_of "$h")"
    # shellcheck disable=SC2086
    printf '%-20s ' "$h"
    remote "$h" "for u in $units; do printf '%s=%s  ' \$u \$(systemctl is-active \$u 2>/dev/null || true); done; echo" ||
      echo "(unreachable)"
  done
  echo
  remote "$controller_host" "PATH=/usr/local/bin:\$PATH cl-info 127.0.0.1" || true
  for d in $(each dashboards); do echo "dashboard: http://$d:3000/"; done
}

cmd_verify() {
  log "acceptance tests on $controller_host"
  remote "$controller_host" "PATH=/usr/local/bin:\$PATH; cl-info 127.0.0.1 && cl-test-memory --controller 127.0.0.1 &&
    cl-test-matmul --controller 127.0.0.1 --sizes 64,257x129x300,512,1024,2048"
}

cmd_restart() {
  remote_tty "$controller_host" "sudo systemctl restart kudalite-controller"
  for w in $(each workers); do remote_tty "$w" "sudo systemctl restart kudalite-worker"; done
  for d in $(each dashboards); do remote_tty "$d" "sudo systemctl restart kudalite-dashboard-api kudalite-dashboard-web"; done
  cmd_status
}

cmd_stop() {
  for d in $(each dashboards); do remote_tty "$d" "sudo systemctl stop kudalite-dashboard-web kudalite-dashboard-api"; done
  for w in $(each workers); do remote_tty "$w" "sudo systemctl stop kudalite-worker"; done
  remote_tty "$controller_host" "sudo systemctl stop kudalite-controller"
}

cmd_logs() {
  local host="${1:?usage: logs HOST}"
  user_of "$host" >/dev/null || die "$host is not in the inventory"
  local args=""
  for u in $(units_of "$host"); do args+=" -u $u"; done
  remote_tty "$host" "journalctl -f -n 100 $args"
}

# Mirrors the substitutions install.sh and CMake make, so what you review is what gets installed.
render_file() {  # template output controller-address
  mkdir -p "$(dirname "$2")"
  sed -e "s#@CONTROLLER@#$3#g" \
    -e "s#@KUDALITE_BINDIR@#$prefix/bin#g" \
    -e "s#@KUDALITE_DOCDIR@#$prefix/share/doc/kudalite#g" \
    -e "s#@DASHBOARD_DIR@#/opt/kudalite/dashboard#g" \
    -e "s#@NODE@#/usr/bin/node#g" \
    "$1" >"$2"
}

cmd_render() {
  rm -rf "$render_dir"
  local h addr out
  for h in $(all_hosts); do
    out="$render_dir/$h"
    addr="$controller_addr"
    if [[ "$h" == "$controller_host" ]]; then
      render_file "$src/deploy/config/controller.conf" "$out/etc/kudalite/controller.conf" "$addr"
      render_file "$src/deploy/systemd/kudalite-controller.service.in" \
        "$out/etc/systemd/system/kudalite-controller.service" "$addr"
    fi
    if grep -qxF "$h" <<<"$(each workers)"; then
      render_file "$src/deploy/config/worker.conf" "$out/etc/kudalite/worker.conf" "$addr"
      render_file "$src/deploy/systemd/kudalite-worker.service.in" \
        "$out/etc/systemd/system/kudalite-worker.service" "$addr"
    fi
    if grep -qxF "$h" <<<"$(each dashboards)"; then
      [[ "$h" == "$controller_host" ]] && addr=127.0.0.1
      render_file "$src/deploy/config/dashboard.env" "$out/etc/kudalite/dashboard.env" "$addr"
      for u in kudalite-dashboard-api kudalite-dashboard-web; do
        render_file "$src/deploy/systemd/$u.service.in" "$out/etc/systemd/system/$u.service" "$addr"
      done
    fi
    printf '%-20s %s\n' "$h" "$(cd "$out" && find . -type f | sed 's#^\./#/#' | sort | tr '\n' ' ')"
  done
  cat >"$render_dir/README.txt" <<EOF_README
Rendered by deploy/deploy-cluster.sh render from $(basename "$inventory") on $(date -u +%Y-%m-%dT%H:%MZ).
Each <host>/ directory mirrors the files deploy/install.sh writes on that node (prefix $prefix).
Workers reach the controller at: $controller_addr
Note: the dashboard web unit shows /usr/bin/node; install.sh substitutes the Node.js it actually
finds or installs (e.g. /opt/kudalite/node/bin/node with --install-node).
To deploy: deploy/deploy-cluster.sh install
EOF_README
  log "rendered configuration for $(all_hosts | wc -l | tr -d ' ') node(s) into $render_dir"
}

case "$command" in
  install | upgrade) cmd_install ;;
  render) cmd_render ;;
  status) cmd_status ;;
  verify) cmd_verify ;;
  restart) cmd_restart ;;
  stop) cmd_stop ;;
  logs) cmd_logs "${2:-}" ;;
  *) usage 64 ;;
esac
