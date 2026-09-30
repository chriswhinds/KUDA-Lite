#!/usr/bin/env bash
# Installs one KUDA-Lite role on this machine: builds from this source tree, installs the
# binaries, config, and systemd service, and starts it. Safe to re-run (that is how you upgrade):
# existing config files are never overwritten.
#
#   sudo deploy/install.sh controller
#   sudo deploy/install.sh worker    --controller pi5-ctl
#   sudo deploy/install.sh dashboard --controller 127.0.0.1 [--install-node]
#   sudo deploy/install.sh host                       # library + tools only, no services
#
# Options:
#   --controller HOST   controller address (required for worker; dashboard default 127.0.0.1)
#   --prefix DIR        install prefix for binaries/library (default /usr/local)
#   --dashboard-dir DIR where the dashboard is installed (default /opt/kudalite/dashboard)
#   --install-node      dashboard: download Node.js 22 LTS from nodejs.org if the system's is < 20.9
#   --tune-network      also install deploy/sysctl/90-kudalite.conf
#   --no-deps           do not install OS packages (you already have them)
#   --no-start          install files only; do not touch systemd (e.g. building an SD-card image)
#   --jobs N            parallel build jobs (default: number of CPUs)
#
# Works on Debian / Raspberry Pi OS / Ubuntu (apt), Fedora / RHEL (dnf), Arch (pacman),
# openSUSE (zypper) and Alpine (apk). Services need systemd; without it the script prints the
# commands to run instead. See docs/DEPLOYMENT.md.
set -euo pipefail

src="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
role=""
controller=""
prefix="/usr/local"
dashboard_dir="/opt/kudalite/dashboard"
install_node=0
tune_network=0
with_deps=1
start_services=1
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

log() { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

usage() { sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit "${1:-0}"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    controller | worker | dashboard | host) role="$1" ;;
    --controller) controller="${2:?--controller needs a value}"; shift ;;
    --prefix) prefix="${2:?}"; shift ;;
    --dashboard-dir) dashboard_dir="${2:?}"; shift ;;
    --install-node) install_node=1 ;;
    --tune-network) tune_network=1 ;;
    --no-deps) with_deps=0 ;;
    --no-start) start_services=0 ;;
    --jobs) jobs="${2:?}"; shift ;;
    -h | --help) usage 0 ;;
    *) warn "unknown argument: $1"; usage 64 ;;
  esac
  shift
done

[[ -n "$role" ]] || usage 64
[[ "$(uname -s)" == Linux ]] || die "install.sh is for Linux nodes. On macOS build the host tools with: cmake --preset host && cmake --build --preset host"
[[ $EUID -eq 0 ]] || die "run as root (sudo $0 ...)"
if [[ "$role" == worker && -z "$controller" ]]; then die "worker needs --controller HOST"; fi
if [[ "$role" == dashboard && -z "$controller" ]]; then controller="127.0.0.1"; fi

have() { command -v "$1" >/dev/null 2>&1; }
has_systemd() { have systemctl && [[ -d /run/systemd/system ]]; }

arch="$(uname -m)"
case "$arch" in
  aarch64 | x86_64) ;;
  *) warn "untested architecture '$arch'; KUDA-Lite targets 64-bit Linux (use a 64-bit OS on the Pi)" ;;
esac

is_pi5() { [[ -r /proc/device-tree/model ]] && tr -d '\0' </proc/device-tree/model | grep -q "Raspberry Pi 5"; }

# ---- OS packages --------------------------------------------------------------------------------

install_packages() {
  local want_python=0
  [[ "$role" == dashboard ]] && want_python=1
  if have apt-get; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -q
    apt-get install -y -q build-essential cmake rsync
    if ((want_python)); then apt-get install -y -q python3 python3-venv python3-pip curl xz-utils; fi
  elif have dnf; then
    dnf install -y gcc-c++ make cmake rsync
    if ((want_python)); then dnf install -y python3 python3-pip curl xz; fi
  elif have pacman; then
    pacman -Sy --needed --noconfirm base-devel cmake rsync
    if ((want_python)); then pacman -S --needed --noconfirm python python-pip curl xz; fi
  elif have zypper; then
    zypper --non-interactive install gcc-c++ make cmake rsync
    if ((want_python)); then zypper --non-interactive install python3 python3-pip curl xz; fi
  elif have apk; then
    apk add --no-cache build-base cmake linux-headers rsync bash
    if ((want_python)); then apk add --no-cache python3 py3-pip curl xz; fi
  else
    die "no supported package manager found; install a C++17 compiler, cmake >= 3.21 and rsync, then re-run with --no-deps"
  fi
}

check_cmake() {
  have cmake || die "cmake not found"
  local v
  v="$(cmake --version | awk 'NR==1 {print $3}')"
  local major="${v%%.*}" rest="${v#*.}"
  local minor="${rest%%.*}"
  if ((major < 3 || (major == 3 && minor < 21))); then
    die "cmake $v is too old (need >= 3.21). Install a newer one (e.g. 'pip install cmake') and re-run with --no-deps"
  fi
}

# ---- C++ build ----------------------------------------------------------------------------------

build_and_install() {
  check_cmake
  local cpu=generic controller_on=OFF worker_on=OFF
  if is_pi5; then cpu=pi5; fi
  case "$role" in
    controller) controller_on=ON ;;
    worker) worker_on=ON ;;
  esac
  local build="/var/tmp/kudalite-build-$role"
  log "building KUDA-Lite ($role, CPU tuning: $cpu) in $build"
  cmake -S "$src" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DKUDALITE_CPU="$cpu" \
    -DKUDALITE_BUILD_CONTROLLER="$controller_on" \
    -DKUDALITE_BUILD_WORKER="$worker_on" \
    -DKUDALITE_BUILD_TOOLS=ON \
    -DKUDALITE_BUILD_TESTS=ON \
    -DKUDALITE_BUILD_EXAMPLES=OFF >/dev/null
  cmake --build "$build" --parallel "$jobs"
  log "running unit tests"
  ctest --test-dir "$build" -L unit --output-on-failure
  log "installing to $prefix"
  cmake --install "$build" >/dev/null
}

# ---- Users, config, services --------------------------------------------------------------------

ensure_user() {
  if id kudalite >/dev/null 2>&1; then return; fi
  log "creating system user 'kudalite'"
  if have useradd; then
    useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin kudalite 2>/dev/null ||
      useradd --system --no-create-home --shell /sbin/nologin kudalite
  else
    addgroup -S kudalite && adduser -S -D -H -G kudalite -s /sbin/nologin kudalite  # BusyBox / Alpine
  fi
}

# Installs $1 as $2 unless $2 exists; then writes $2.new and says so (your edits are kept).
install_config() {
  local from="$1" to="$2" tmp
  tmp="$(mktemp)"
  sed "s/@CONTROLLER@/${controller:-127.0.0.1}/g" "$from" >"$tmp"
  install -d -m 0755 /etc/kudalite
  if [[ -e "$to" ]]; then
    if ! cmp -s "$tmp" "$to"; then
      install -m 0644 "$tmp" "$to.new"
      warn "$to exists and was kept; the new template is $to.new"
    fi
  else
    install -m 0644 "$tmp" "$to"
    log "wrote $to"
  fi
  rm -f "$tmp"
}

install_unit() {  # $1 = unit file path, generated with the right paths
  local name
  name="$(basename "$1")"
  if has_systemd; then
    install -m 0644 "$1" "/etc/systemd/system/$name"
  fi
}

activate() {  # $@ = unit names
  if ! has_systemd; then
    warn "systemd not found: services were not installed. Start the daemons yourself, e.g."
    for u in "$@"; do grep '^ExecStart=' "$prefix/share/kudalite/systemd/$u.service" 2>/dev/null |
      sed 's/^ExecStart=/    /' || true; done
    return
  fi
  if [[ $start_services == 1 ]]; then
    systemctl daemon-reload
    for u in "$@"; do
      systemctl enable "$u" >/dev/null 2>&1
      systemctl restart "$u"
      log "$u: $(systemctl is-active "$u")"
    done
  else
    log "installed, not started. To start: systemctl daemon-reload && systemctl enable --now $*"
  fi
}

# ---- Dashboard ----------------------------------------------------------------------------------

node_bin=""

node_ok() {  # $1 = node binary
  local v
  v="$("$1" --version 2>/dev/null)" || return 1
  v="${v#v}"
  local major="${v%%.*}" rest="${v#*.}"
  local minor="${rest%%.*}"
  ((major > 20 || (major == 20 && minor >= 9)))
}

download_node() {
  local narch
  case "$arch" in aarch64) narch=arm64 ;; x86_64) narch=x64 ;; *) die "no Node.js build for $arch" ;; esac
  local base="https://nodejs.org/dist/latest-v22.x" line file sum
  log "downloading Node.js 22 LTS ($narch) from nodejs.org"
  line="$(curl -fsSL "$base/SHASUMS256.txt" | grep " node-v.*-linux-$narch.tar.xz\$")" || die "cannot list Node.js releases"
  sum="${line%% *}"
  file="${line##* }"
  curl -fsSL -o "/var/tmp/$file" "$base/$file"
  echo "$sum  /var/tmp/$file" | sha256sum -c --quiet - || die "Node.js download failed its checksum"
  rm -rf /opt/kudalite/node && install -d /opt/kudalite/node
  tar -xJf "/var/tmp/$file" -C /opt/kudalite/node --strip-components=1
  rm -f "/var/tmp/$file"
}

find_node() {
  if have node && node_ok "$(command -v node)"; then
    node_bin="$(command -v node)"
  elif [[ -x /opt/kudalite/node/bin/node ]] && node_ok /opt/kudalite/node/bin/node; then
    node_bin=/opt/kudalite/node/bin/node
  elif [[ $install_node == 1 ]]; then
    download_node
    node_bin=/opt/kudalite/node/bin/node
  else
    die "the dashboard needs Node.js >= 20.9 ($(node --version 2>/dev/null || echo 'none') found). Re-run with --install-node, or install it from your distribution / nodejs.org"
  fi
  log "using Node.js $("$node_bin" --version) at $node_bin"
}

install_dashboard() {
  have python3 || die "python3 not found"
  python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' ||
    die "the dashboard backend needs Python >= 3.10 ($(python3 --version) found)"
  find_node
  local npm
  npm="$(dirname "$node_bin")/npm"

  log "installing the dashboard into $dashboard_dir"
  install -d "$dashboard_dir"
  rsync -a --delete --exclude node_modules --exclude .next --exclude .venv --exclude __pycache__ \
    "$src/dashboard/backend/" "$dashboard_dir/backend/"

  log "backend: Python virtualenv + dependencies"
  python3 -m venv "$dashboard_dir/backend/.venv"
  "$dashboard_dir/backend/.venv/bin/pip" install -q --upgrade pip
  "$dashboard_dir/backend/.venv/bin/pip" install -q -r "$dashboard_dir/backend/requirements.txt"
  "$dashboard_dir/backend/.venv/bin/python" -m compileall -q "$dashboard_dir/backend/app"

  log "front end: npm ci + production build"
  local fe=/var/tmp/kudalite-dashboard-frontend
  rm -rf "$fe" && install -d "$fe"
  rsync -a --exclude node_modules --exclude .next "$src/dashboard/frontend/" "$fe/"
  (cd "$fe" && PATH="$(dirname "$node_bin"):$PATH" NEXT_TELEMETRY_DISABLED=1 "$npm" ci --no-audit --no-fund &&
    PATH="$(dirname "$node_bin"):$PATH" NEXT_TELEMETRY_DISABLED=1 "$npm" run build)
  # Next.js "standalone" output: a self-contained server.js with only the needed node_modules.
  rm -rf "$dashboard_dir/web"
  cp -a "$fe/.next/standalone" "$dashboard_dir/web"
  cp -a "$fe/.next/static" "$dashboard_dir/web/.next/static"
  install -d -o kudalite -g kudalite "$dashboard_dir/web/.next/cache"
  rm -rf "$fe"

  install_config "$src/deploy/config/dashboard.env" /etc/kudalite/dashboard.env
  local gen
  gen="$(mktemp -d)"
  for u in kudalite-dashboard-api kudalite-dashboard-web; do
    sed -e "s#@DASHBOARD_DIR@#$dashboard_dir#g" -e "s#@NODE@#$node_bin#g" \
      "$src/deploy/systemd/$u.service.in" >"$gen/$u.service"
    install_unit "$gen/$u.service"
  done
  rm -rf "$gen"
  activate kudalite-dashboard-api kudalite-dashboard-web
  local port
  port="$(sed -n 's/^PORT=//p' /etc/kudalite/dashboard.env | tail -1)"
  log "dashboard: http://$(hostname):${port:-3000}/"
}

# ---- Main ---------------------------------------------------------------------------------------

log "KUDA-Lite install: role=$role source=$src prefix=$prefix"
if ((with_deps)); then install_packages; fi

if [[ $tune_network == 1 ]]; then
  install -m 0644 "$src/deploy/sysctl/90-kudalite.conf" /etc/sysctl.d/90-kudalite.conf
  sysctl -q -p /etc/sysctl.d/90-kudalite.conf || warn "could not apply sysctl settings now; they apply at next boot"
fi

case "$role" in
  controller)
    build_and_install
    ensure_user
    install_config "$src/deploy/config/controller.conf" /etc/kudalite/controller.conf
    install_unit "$prefix/share/kudalite/systemd/kudalite-controller.service"
    activate kudalite-controller
    ;;
  worker)
    build_and_install
    ensure_user
    install_config "$src/deploy/config/worker.conf" /etc/kudalite/worker.conf
    install_unit "$prefix/share/kudalite/systemd/kudalite-worker.service"
    activate kudalite-worker
    ;;
  dashboard)
    ensure_user
    install_dashboard
    ;;
  host)
    build_and_install
    ;;
esac

log "done. Check the cluster with: $prefix/bin/cl-info ${controller:-127.0.0.1}"
