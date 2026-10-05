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
# Runs a simulated KUDA-Lite cluster on this machine: one controller and N workers on localhost.
# For development and for the integration test; nothing is installed.
#
#   scripts/run-local-cluster.sh start [WORKERS]   (default 3)
#   scripts/run-local-cluster.sh status
#   scripts/run-local-cluster.sh stop
#
# Environment:
#   KUDALITE_BIN        directory with cl-controller, cl-worker, cl-info (default: build/release)
#   KUDALITE_HOST_PORT  controller host port   (default 17070)
#   KUDALITE_WORKER_PORT controller worker port (default 17071)
#   KUDALITE_WORKER_MEM / KUDALITE_WORKER_CACHE / KUDALITE_WORKER_THREADS  (256M / 64M / 2)
#   KUDALITE_RUN_DIR    where pid files and logs go (default ${TMPDIR:-/tmp}/kudalite-local-$USER)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin="${KUDALITE_BIN:-$here/../build/release}"
host_port="${KUDALITE_HOST_PORT:-17070}"
worker_port="${KUDALITE_WORKER_PORT:-17071}"
run_dir="${KUDALITE_RUN_DIR:-${TMPDIR:-/tmp}/kudalite-local-${USER:-user}}"

die() { echo "run-local-cluster: $*" >&2; exit 1; }

running() { [[ -f "$1" ]] && kill -0 "$(cat "$1")" 2>/dev/null; }

start() {
  local workers="${1:-3}"
  for b in cl-controller cl-worker cl-info; do
    [[ -x "$bin/$b" ]] || die "$bin/$b not found; build first (cmake --preset release && cmake --build --preset release) or set KUDALITE_BIN"
  done
  mkdir -p "$run_dir"
  running "$run_dir/controller.pid" && die "already running (scripts/run-local-cluster.sh stop first)"
  rm -f "$run_dir"/*.pid "$run_dir"/*.log

  "$bin/cl-controller" --bind 127.0.0.1 --host-port "$host_port" --worker-port "$worker_port" \
    >"$run_dir/controller.log" 2>&1 &
  echo $! >"$run_dir/controller.pid"

  for ((i = 0; i < workers; i++)); do
    "$bin/cl-worker" --controller "127.0.0.1:$worker_port" --bind 127.0.0.1 --data-port 0 \
      --mem "${KUDALITE_WORKER_MEM:-256M}" --cache "${KUDALITE_WORKER_CACHE:-64M}" \
      --threads "${KUDALITE_WORKER_THREADS:-2}" >"$run_dir/worker$i.log" 2>&1 &
    echo $! >"$run_dir/worker$i.pid"
  done

  # Wait until every worker has registered.
  for _ in $(seq 1 100); do
    if "$bin/cl-info" "127.0.0.1:$host_port" 2>/dev/null | grep -Eq "^Workers: +$workers\$"; then
      echo "local cluster up: controller 127.0.0.1:$host_port, $workers workers (logs in $run_dir)"
      echo "  export KUDALITE_CONTROLLER=127.0.0.1:$host_port"
      return 0
    fi
    sleep 0.1
  done
  stop >/dev/null
  die "workers did not register within 10 s; see $run_dir/*.log"
}

stop() {
  local any=0
  for pidfile in "$run_dir"/worker*.pid "$run_dir"/controller.pid; do
    [[ -f "$pidfile" ]] || continue
    if running "$pidfile"; then
      kill "$(cat "$pidfile")" 2>/dev/null || true
      any=1
    fi
    rm -f "$pidfile"
  done
  [[ $any == 1 ]] && echo "local cluster stopped" || echo "local cluster was not running"
}

status() {
  if running "$run_dir/controller.pid"; then
    "$bin/cl-info" "127.0.0.1:$host_port"
  else
    echo "local cluster is not running"
    return 1
  fi
}

case "${1:-}" in
  start) start "${2:-3}" ;;
  stop) stop ;;
  status) status ;;
  *) echo "usage: $0 start [WORKERS] | status | stop" >&2; exit 64 ;;
esac
