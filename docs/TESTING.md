<!--
Copyright 2026 Christopher Hinds, Stratum Labs
SPDX-License-Identifier: Apache-2.0 (see the LICENSE file at the project root)
-->

# KUDA-Lite Testing

## 1. Test inventory

| Program | Needs a cluster | What it covers |
|---|---|---|
| `tests/test_unit.cpp` | no | Serialisation (round trips, truncation), free-list allocation and coalescing, allocation page math, address translation (mid-page, cross-page, out of range, unmapped), extent coalescing, kernel-argument packing, `ExecBlocks`/cluster-map encoding, host:port and size parsing, thread pool (coverage, nesting, exception propagation) |
| `tests/test_memory.cpp` | yes | Striped (64 KiB and 4 KiB pages, odd sizes) and blocked round trips, unaligned cross-worker sub-ranges, memset, device-to-device copy, two streams ordered by an event, event timing, error reporting (out-of-range, unmapped, unknown kernel, double free, bad page size) |
| `tests/test_matmul.cpp` | yes | Test case 1: SGEMM/DGEMM against a reference with a formal error bound ([MATMUL.md §5](MATMUL.md#5-verification-method)) |
| `examples/saxpy.cpp` | yes | Smallest end-to-end program, using `cl_saxpy` |
| `tools/cl_info.cpp` | yes | Cluster query; smoke test for connectivity |
| `tools/cl_top.cpp` | yes | Live telemetry of every node; smoke test for the telemetry path |
| `dashboard/backend/tests/` (pytest) | no (uses the mock controller) | Frame header layout, sample and `GetTelemetry` round trips, forward compatibility (ignores appended fields), truncation errors; the FastAPI endpoints end to end over TCP against the mock (cluster view, history, 404, disconnected state) |

Every test program exits with status 0 on success, so it can be used from a script or CI.

## 2. Simulated cluster on one machine

The whole system runs on one Linux or macOS machine, with each worker on an ephemeral data port. This is the fastest way to develop and debug before involving the Pis.

(The build system comes in the next phase. The binary names below are the ones it will produce.)

```bash
cl-controller --bind 127.0.0.1 &
```

```bash
for i in 0 1 2; do cl-worker --controller 127.0.0.1 --bind 127.0.0.1 --data-port 0 --mem 256M --cache 64M --threads 2 & done
```

```bash
cl-info && test_memory && test_matmul
```

## 3. Verification performed for this delivery

A throwaway compile, not the project's build system, was run with `g++ 15.2 -std=c++17 -Wall -Wextra -Wpedantic` on Linux x86-64. Everything compiled with **no warnings**. `clang++ -fsyntax-only` with the same flags was also clean, as a stand-in check for macOS.

The simulated cluster ran 1 controller and 3 workers (256 MiB arena and 2 threads each) on one machine:

| Check | Result |
|---|---|
| `test_unit` | PASS (10/10) |
| `test_memory` | PASS |
| `saxpy` (4 M elements) | correct |
| `test_matmul` (64, 257×129×300, 512, 1024; β=0 and β≠0; dgemm) | PASS, worst error/bound 0.005 |
| `test_matmul --sizes 2048` (sampled check) | PASS |
| Whole suite under **ThreadSanitizer** (all 3 daemons + test programs) | **0 data-race reports** |
| Host killed (`kill -9`) in the middle of a 2048 launch | Launch cancelled, the session's 3 allocations reclaimed within ~0.1 s, cluster memory back to 100% free |
| Worker killed | Its in-flight chunk fails with `clErrorWorkerLost` and the session is cleaned up. New allocations and launches run on the 2 surviving workers (test_matmul PASS). |

Throughput observed on this machine (x86 over localhost, so **not representative of Pis**): 18–20 GFLOP/s at n = 1024 and 26–28 GFLOP/s at n = 2048 for sgemm across 6 threads.

### Observability (second delivery)

| Check | Result |
|---|---|
| `test_unit`: 12 tests, including telemetry round trip, forward compatibility, the sampler on this machine, and the busy-thread gauge | PASS |
| `cl-top` during a 2048 matmul | Busy threads 2/2 on every worker, `cl_sgemm` shown as the running kernel, CPU ~70%, SoC temperature rising 53 → 64 °C, block counts increasing; idle afterwards |
| Python client against the **real C++ controller** | Decodes every node, with ~5 min of history backfill (296 samples per node) |
| Backend pytest | 9 passed |
| Front end: `tsc --noEmit` and `next build` | Clean |
| Full stack (C++ cluster → FastAPI → Next.js, in the browser) | All nodes, tiles, sparklines, meters and six live charts render in light and dark themes and at 390 px wide with no horizontal scroll. Crosshair tooltip works by pointer and keyboard. |
| Controller killed | Backend reports disconnected, UI shows "Controller unreachable" plus an alert, and nodes are marked stale |
| Controller replaced (mock on the same port) | Backend reconnects on its own (backoff ≤ 10 s); 7 nodes shown, including one offline |
| Whole suite under **ThreadSanitizer** with telemetry every 100 ms and `cl-top` polling during load | **0 data-race reports** |

Because the simulated cluster runs on one machine, every "Pi" reports the same host memory and temperature. On real hardware each node reports its own.

### Build system and deployment (third delivery)

| Check | Result |
|---|---|
| `cmake` configure + build (Ninja, `-Werror`), all targets | Clean |
| `ctest`: `unit` + `integration` (local cluster: cl-info, cl-test-memory, cl-test-matmul, saxpy, cl-top telemetry from 3 workers) | 2/2 passed |
| `ctest --preset tsan` (integration under ThreadSanitizer, `halt_on_error=1`) | 2/2 passed |
| Presets `host` (library + tools only) and `pi5` (refuses non-AArch64 with a clear message) | As designed |
| `cmake --install` layout; `find_package(kudalite)` from a separate project, which then ran saxpy on a cluster | Works |
| Generated systemd units (all four) | `systemd-analyze verify`: no issues |
| Daemons started from the shipped config files; command-line overrides; malformed and unknown keys | Work; rejected with exit 78 / 64 and a message |
| `install.sh controller` + `install.sh worker` run in a user-namespace sandbox (copy-on-write `/etc`) | Build, unit tests, install, config with controller address, units written; re-run keeps an edited config and writes `.new`. User creation could not be exercised in the sandbox. |
| Dashboard deployed as `install.sh` does it (venv, `npm ci`, standalone build, `node server.js`, env from `dashboard.env`) against a live cluster | Page, static assets and live API all serve |
| `deploy-cluster.sh` dry run with stand-in ssh/rsync | Correct order (controller, workers, dashboard on 127.0.0.1), one copy per host, correct services per host in `status`, `logs` validation |

Not exercised: real `sudo`/`useradd`/`systemctl enable` on a node, the package-manager step, `--install-node` download, cross-compilation (no aarch64 toolchain here), and `deploy-cluster.sh` against real SSH hosts.

### Rename to KUDA-Lite and release re-test (fourth delivery)

After the rename, everything was rebuilt and re-run from clean:

| Check | Result |
|---|---|
| Stale-name audit (no "CUDA" outside documentation; every doc mention refers to NVIDIA) | Clean |
| Markdown cross-references (files and heading anchors) | 0 broken |
| `release` preset in the project (`build/release/`), `-Werror` | Clean; `ctest` 2/2 |
| `debug` (`-Werror`), `tsan` (integration under ThreadSanitizer), `host` presets | All pass |
| Dashboard: backend pytest; front end `tsc` + production standalone build | 9/9; clean |
| `deploy-cluster.sh render` for the example inventory (5 nodes) | All files rendered, no placeholders left, all 7 units pass `systemd-analyze verify` |
| Sandboxed `install.sh controller` + `worker` compared with `render` output | Config files byte-identical; units identical apart from the install prefix |
| `deploy-cluster.sh install` dry run (stand-in ssh/rsync) | Correct order and arguments |
| `scripts/package-release.sh` → unpack → `cmake --preset release` → `ctest` | Checksum OK; builds; 2/2 pass; no build output or private files in the tarball |

**Not yet verified:** Raspberry Pi 5 hardware, real 1 GbE networking, macOS host at runtime, long-duration soak.

## 4. Hardware acceptance plan (next phase)

| # | Test | Pass criterion |
|---|---|---|
| H1 | `cl-info` from the Linux host and from the macOS host | All workers listed, arena sizes as configured |
| H2 | `test_unit` on a Pi (AArch64) | PASS; confirms the host/worker struct layout and endianness assumptions |
| H3 | `test_memory` | PASS |
| H4 | `test_matmul --sizes 64,257x129x300,512,1024,2048,4096 --repeat 3` | PASS; record GFLOP/s per size |
| H5 | Scaling: H4 at n = 4096 with 1, 2, 4, … workers | Speed-up ≥ 0.7 × worker count at n = 4096 |
| H6 | Thermal: `test_matmul --sizes 4096 --repeat 20` | PASS; dynamic scheduling keeps throttled nodes from stalling the launch |
| H7 | Fault injection: unplug a worker's network during a launch | Host gets `clErrorWorkerLost`; controller and remaining workers stay up |
| H8 | Soak: loop `test_memory` and `test_matmul` for 8 h | No failures, no growth in controller/worker RSS |
