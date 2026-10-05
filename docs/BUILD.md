<!--
Copyright 2026 Christopher Hinds, Stratum Labs
SPDX-License-Identifier: Apache-2.0 (see the LICENSE file at the project root)
-->

# Building KUDA-Lite

KUDA-Lite builds with CMake (≥ 3.21) and any C++17 compiler: GCC ≥ 9 or Clang ≥ 10 on Linux, and Apple Clang on macOS. It has no third-party C++ dependencies. The dashboard is built separately (see [§7](#7-dashboard)).

To install on the Pis you normally don't run CMake yourself: `deploy/install.sh` does it for you ([DEPLOYMENT.md](DEPLOYMENT.md)). This page is for development, for host machines, and for anyone who wants control over the build.

## 1. Quick start

```bash
cmake --preset release
```

```bash
cmake --build --preset release
```

```bash
ctest --preset release
```

`ctest` runs the unit tests and an **integration test** that starts a controller and 3 workers on localhost, then runs the memory, matmul, saxpy and telemetry checks against them (about 5 s).

## 2. Presets

| Preset | Use it on | What it builds |
|---|---|---|
| `release` | any machine | everything, optimised, for this CPU family |
| `pi5` | a Raspberry Pi 5 | everything, tuned with `-mcpu=cortex-a76` |
| `pi5-cross` | x86-64 Linux with `g++-aarch64-linux-gnu` | Pi 5 binaries (daemons + tools, no tests) |
| `host` | macOS, or a Linux machine that only runs programs | `libkudalite` + `cl-info`, `cl-top`, `cl-test-*`, `saxpy` |
| `debug` | development | debug build, warnings are errors |
| `tsan` | development | ThreadSanitizer build; `ctest --preset tsan` runs the integration test under it |

Build trees go to `build/<preset>/`.

## 3. Options

Pass options as `-D<NAME>=<value>` when configuring.

| Option | Default | Meaning |
|---|---|---|
| `KUDALITE_BUILD_CONTROLLER` | ON | `cl-controller` |
| `KUDALITE_BUILD_WORKER` | ON | `cl-worker`, including the built-in kernels |
| `KUDALITE_BUILD_TOOLS` | ON | `cl-info`, `cl-top`, `cl-test-memory`, `cl-test-matmul` |
| `KUDALITE_BUILD_TESTS` | ON | `test_unit`, plus the CTest `unit` and `integration` tests |
| `KUDALITE_BUILD_EXAMPLES` | ON | `saxpy` |
| `KUDALITE_SHARED` | OFF | build `libkudalite` as a shared library |
| `KUDALITE_CPU` | `generic` | `generic`, `native` (`-mcpu=native`), or `pi5` (`-mcpu=cortex-a76`; AArch64 only) |
| `KUDALITE_SANITIZER` | *(empty)* | `thread` or `address` |
| `KUDALITE_WERROR` | OFF | treat warnings as errors |
| `CMAKE_BUILD_TYPE` | `Release` | `Release`, `RelWithDebInfo`, `Debug` |
| `CMAKE_INSTALL_PREFIX` | `/usr/local` | where `cmake --install` puts things |

## 4. Programs and tests

| Target | Installed as | Purpose |
|---|---|---|
| `kudalite` | `lib/libkudalite.a` + `include/kudalite/` | host runtime library |
| `cl-controller`, `cl-worker` | `bin/` | the daemons |
| `cl-info`, `cl-top` | `bin/` | cluster query, live telemetry |
| `cl-test-memory`, `cl-test-matmul` | `bin/` | cluster acceptance tests (`tests/test_memory.cpp`, `tests/test_matmul.cpp`) |
| `saxpy` | source only, in `share/kudalite/examples/` | example program |
| `test_unit` | not installed | unit tests |

| CTest label | Run with | Needs |
|---|---|---|
| `unit` | `ctest --preset unit` | nothing |
| `integration` | `ctest --preset release` | bash; free localhost ports 27070/27071 |

To play with a local cluster by hand, run `scripts/run-local-cluster.sh start 3`, then `export KUDALITE_CONTROLLER=127.0.0.1:17070`, run any program, and finish with `scripts/run-local-cluster.sh stop`.

## 5. Installing

```bash
sudo cmake --install build/release
```

Layout (prefix `/usr/local`):

```
bin/cl-controller  bin/cl-worker  bin/cl-info  bin/cl-top  bin/cl-test-memory  bin/cl-test-matmul
lib/libkudalite.a
lib/cmake/kudalite/                     find_package(kudalite) support
include/kudalite/*.h
share/kudalite/config/                  controller.conf, worker.conf, dashboard.env templates
share/kudalite/systemd/                 kudalite-controller.service, kudalite-worker.service (paths filled in)
share/kudalite/examples/saxpy.cpp
share/doc/kudalite/                     this documentation
```

`cmake --install` never writes to `/etc` and never starts anything. `deploy/install.sh` does those steps.

### Using KUDA-Lite from your own CMake project

```cmake
find_package(kudalite 0.2 REQUIRED)
target_link_libraries(my_app PRIVATE kudalite::kudalite)
```

If KUDA-Lite is installed somewhere other than a default prefix, configure with `-DCMAKE_PREFIX_PATH=<prefix>`. Without CMake, compile with `-I<prefix>/include` and link `<prefix>/lib/libkudalite.a -pthread`.

### Release tarball

```bash
scripts/package-release.sh
```

This writes `dist/kudalite-<version>.tar.gz` and a `.sha256` file: the source tree without build output, `node_modules`, rendered configs or your inventory. It's for installing on nodes by hand ([DEPLOYMENT.md §6](DEPLOYMENT.md#6-manual-installation-one-node-at-a-time)). The tarball builds and tests on its own.

## 6. Cross-compiling for the Pis

Building natively on each Pi (what `install.sh` does) is simplest, should take a few minutes on a Pi 5 (not yet timed on hardware), and always matches the Pi's libraries. To build once on a fast x86-64 Linux machine instead:

```bash
sudo apt install g++-aarch64-linux-gnu
```

```bash
cmake --preset pi5-cross && cmake --build --preset pi5-cross
```

The binaries in `build/pi5-cross/` need a glibc and libstdc++ on the Pi at least as new as the cross toolchain's. Build on a distribution no newer than the Pis', add `-DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"`, or point `PI_SYSROOT` at a sysroot copied from a Pi. Copy `cl-controller` / `cl-worker` to `/usr/local/bin` on the nodes and install the config and units as in [DEPLOYMENT.md §6](DEPLOYMENT.md#6-manual-installation-one-node-at-a-time).

## 7. Dashboard

The dashboard is not part of the CMake build. See [dashboard/README.md](../dashboard/README.md) for development. `deploy/install.sh dashboard` builds and installs it for production:

- **Backend:** Python ≥ 3.10. `pip install -r dashboard/backend/requirements.txt`; tests use `requirements-dev.txt` and `pytest`.
- **Front end:** Node.js ≥ 20.9. `npm ci && npm run build` produces a self-contained server in `.next/standalone/` (`output: "standalone"`), plus `.next/static/`.

## 8. macOS

```bash
brew install cmake
```

```bash
cmake --preset host && cmake --build --preset host && sudo cmake --install build/host
```

The daemons also build and run on macOS (useful for development: `cmake --preset release`), but some telemetry fields are Linux-only ([OBSERVABILITY.md §2](OBSERVABILITY.md#2-what-is-measured)).
