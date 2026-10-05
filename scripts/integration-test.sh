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
# End-to-end test on a simulated local cluster (run by `ctest`, label "integration").
#
#   scripts/integration-test.sh BIN_DIR
#
# Starts a controller and 3 workers on localhost (ports 27070/27071 so a developer's own local
# cluster is not disturbed), runs every cluster test program, and always stops the cluster.
set -euo pipefail

bin="$(cd "${1:?usage: $0 BIN_DIR}" && pwd)"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export KUDALITE_BIN="$bin"
export KUDALITE_HOST_PORT="${KUDALITE_HOST_PORT:-27070}"
export KUDALITE_WORKER_PORT="${KUDALITE_WORKER_PORT:-27071}"
export KUDALITE_RUN_DIR="${KUDALITE_RUN_DIR:-${TMPDIR:-/tmp}/kudalite-it-$$}"
export KUDALITE_CONTROLLER="127.0.0.1:$KUDALITE_HOST_PORT"

cleanup() {
  "$here/run-local-cluster.sh" stop >/dev/null || true
  rm -rf "$KUDALITE_RUN_DIR"
}
trap cleanup EXIT

"$here/run-local-cluster.sh" start 3

step() { echo; echo "=== $* ==="; }

step "cl-info";        "$bin/cl-info" "$KUDALITE_CONTROLLER"
step "cl-test-memory"; "$bin/cl-test-memory" --controller "$KUDALITE_CONTROLLER"
step "cl-test-matmul"; "$bin/cl-test-matmul" --controller "$KUDALITE_CONTROLLER" --sizes 64,257x129x300,512
if [[ -x "$bin/saxpy" ]]; then
  step "saxpy";        "$bin/saxpy"
fi
step "cl-top (telemetry)"
sleep 1.5  # let every node push at least one sample
out="$("$bin/cl-top" "$KUDALITE_CONTROLLER" --once)"
echo "$out"
workers_ok="$(grep -cE '^[0-9]+ +worker .* ok ' <<<"$out" || true)"
[[ "$workers_ok" == 3 ]] || { echo "expected 3 workers reporting telemetry, got $workers_ok" >&2; exit 1; }

echo; echo "integration test PASSED"
