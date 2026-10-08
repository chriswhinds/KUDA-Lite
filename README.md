<!--
Copyright 2026 Christopher Hinds, Stratum Labs llc
SPDX-License-Identifier: Apache-2.0 (see the LICENSE file at the project root)
-->

# KUDA-Lite

KUDA-Lite is a runtime modelled on NVIDIA CUDA that makes a cluster of ARM single-board computers look like **one compute device** to a program running on a Linux or macOS machine.

A host program allocates "device" memory, copies data in, launches kernels on streams, and copies results out, using an API modelled on the NVIDIA CUDA runtime. Behind that API:

- the **host runtime** (`libkudalite`, the KUDA-Lite "kernel" on the Linux/macOS machine) sends each stream of work to the cluster;
- one board, the **controller**, manages the cluster: it schedules work, manages memory and tracks membership;
- the other boards, the **workers**, run the kernels. Each worker runs one operation at a time, using all of its cores;
- the RAM of all workers is pooled into a **single global address space** that every worker can read and write.

```
 Linux / macOS host                 Controller board                 Worker boards
┌──────────────────────┐  7070   ┌──────────────────────┐  7071   ┌────────────────────┐
│ your program         │────────▶│ cl-controller        │◀────────│ cl-worker 0        │
│   clMalloc/clMemcpy  │  TCP    │  • sessions/streams  │ control │  • arena (RAM)     │
│   clblasSgemm(...)   │         │  • memory manager    │         │  • executor        │
│ libkudalite          │         │  • block scheduler   │         │  • kernels         │
└──────────────────────┘         │  • data proxy        │         └─────────┬──────────┘
                                 └──────────┬───────────┘                   │ 7100 data plane
                                            │ 7100 data plane    ┌──────────┴─────────┐
                                            └───────────────────▶│ cl-worker 1 … N    │
                                                                 └────────────────────┘
                       global memory = worker 0 RAM ∪ worker 1 RAM ∪ … (striped pages)
```

## Supported platforms

| Platform | Per board | Network | Build preset | Power (5-node cluster) |
|---|---|---|---|---|
| **Raspberry Pi 5** | 4 × Cortex-A76, 4–16 GB | 1 GbE | `pi5` | ≈ 22 W idle, ≈ 67 W peak ([POWER.md](docs/POWER.md)) |
| **Orange Pi 6 Plus** | 12 cores (CIX P1: 8 × A720 + 4 × A520), 16–64 GB, NPU | 2 × 5 GbE | `opi6plus` | ≈ 100 W idle, ≈ 250 W peak ([POWER_OPI6PLUS.md](docs/POWER_OPI6PLUS.md)) |

One codebase serves both. Each node's board is detected at install time and gets its own CPU tuning and network profile, and clusters may mix boards. Any other 64-bit Linux machine works as a generic node. KUDA-Lite runs everything on the CPU cores; using the Orange Pi's NPU is on the roadmap. See [Platforms](docs/PLATFORMS.md).

## A first look

```cpp
#include "kudalite/kudalite.h"
#include "kudalite/clblas.h"

clInit("pi-controller:7070");

clDevPtr A, B, C;                                   // addresses in cluster global memory
clMalloc(&A, M * K * sizeof(float));                // pages striped over every worker's RAM
clMalloc(&B, K * N * sizeof(float));
clMalloc(&C, M * N * sizeof(float));
clMemcpyHtoD(A, hostA, M * K * sizeof(float));
clMemcpyHtoD(B, hostB, K * N * sizeof(float));

clblasSgemm(nullptr, M, N, K, 1.0f, A, K, B, N, 0.0f, C, N);   // runs on all workers
clMemcpyDtoH(hostC, C, M * N * sizeof(float));                  // waits for the kernel

clFree(A); clFree(B); clFree(C);
clShutdown();
```

To write your own kernel, define a *block* function with `CL_KERNEL(name) { ... }` and compile it into the worker (see the [Programming Guide](docs/PROGRAMMING_GUIDE.md)).

## How the requirements are met

| # | Requirement | How it is met |
|---|---|---|
| 1 | A kernel on Linux/macOS that manages the communication of work streams to a worker cluster, like CUDA | `libkudalite` ([`include/kudalite/kudalite.h`](include/kudalite/kudalite.h), [`src/host/`](src/host)). It provides streams, events, async copies and kernel launches. The controller runs each stream in order and runs separate streams concurrently. Portable POSIX C++17 that runs on Linux and macOS. |
| 2 | A worker cluster (originally Raspberry Pi 5; Orange Pi 6 Plus added in 0.3) managed by a single controller board. Each worker runs a single operation. All workers' RAM is one shared workspace. | `cl-controller` ([`src/controller/`](src/controller)) and `cl-worker` ([`src/worker/`](src/worker)). Each worker runs one operation at a time on a dedicated executor thread. Global memory is a distributed shared memory: allocations are striped page by page across every worker's arena, and any worker can reach any address ([Memory Model](docs/MEMORY_MODEL.md)). |
| 3 | First test case: matrix multiplication | Built-in `cl_sgemm` / `cl_dgemm` kernels, the `clblasSgemm` / `clblasDgemm` wrappers, and [`tests/test_matmul.cpp`](tests/test_matmul.cpp), which checks results against a CPU reference with a formal error bound ([Matmul design](docs/MATMUL.md)). |
| 4 | C++ implementation | C++17 throughout, with no third-party dependencies. |
| 5 | Any Linux distribution on all nodes | The code uses only POSIX sockets, `mmap` and `std::thread`. No distribution-specific packages or kernel features are needed. |

## Repository layout

```
include/kudalite/          public headers
  kudalite.h               host runtime API (the CUDA runtime analogue)
  cl_types.h               shared types: clDevPtr, clDim3, clError_t, properties
  cl_args.h                kernel argument packing
  cl_kernel.h              worker-side kernel API (clBlockContext, CL_KERNEL)
  clblas.h                 BLAS wrappers + parameter structs (the cuBLAS analogue)
src/common/                code shared by all three programs
  bytes.h protocol.*       wire format and message types
  net.* rpc.*              TCP sockets and multiplexed bidirectional RPC
  address_map.*            global address → (worker, offset) translation
  telemetry.*              node telemetry sample + OS sampler
  dsm_client.*             batched reads/writes of global memory
  freelist.* thread_pool.* log.* cli.h errors.cpp
src/host/                  libkudalite: runtime.cpp, clblas.cpp
src/controller/            cl-controller: controller.*, stream_queue.h, main.cpp
src/worker/                cl-worker: worker.*, arena.*, page_cache.*, kernel_context.cpp, main.cpp
src/worker/kernels/        built-in kernels: cl_sgemm, cl_dgemm, cl_saxpy
tests/                     test_unit (no cluster), test_memory, test_matmul
CMakeLists.txt, CMakePresets.json, cmake/   build system (presets: release, pi5, pi5-cross, host, debug, tsan)
deploy/                    install.sh (one node), deploy-cluster.sh (whole cluster over SSH),
                           config/ templates, systemd/ units, sysctl/ per-board network profiles,
                           cluster.inventory.example (Pi 5), cluster.inventory.opi6plus.example
scripts/                   run-local-cluster.sh (simulated cluster), integration-test.sh (CTest),
                           package-release.sh (dist/kudalite-<version>.tar.gz)
tools/cl_info.cpp          cl-info: cluster "deviceQuery"
tools/cl_top.cpp           cl-top: live per-node telemetry in the terminal
dashboard/                 web dashboard: FastAPI backend + Next.js front end
examples/saxpy.cpp         smallest complete program
docs/                      design documentation (below)
```

## Documentation

| Document | Contents |
|---|---|
| [Building](docs/BUILD.md) | CMake presets and options, tests, install layout, cross-compiling, using the library |
| [Power: Raspberry Pi 5](docs/POWER.md) | Estimated power for a 5-Pi cluster + switch: idle, compute, peak, running cost, PSU/PoE/UPS advice |
| [Power: Orange Pi 6 Plus](docs/POWER_OPI6PLUS.md) | The same for a 5-board Orange Pi 6 Plus cluster with a multi-gig switch, a mixed variant, and compute per watt |
| [NPU inference](docs/NPU.md) | Why Orange Pi 6 Plus NPU inference was deferred, what is known about the NPU and its SDK, proposed design, decisions needed, and the plan for when boards arrive |
| [Platforms](docs/PLATFORMS.md) | Raspberry Pi 5 vs Orange Pi 6 Plus: specs, what is platform-specific, mixed clusters, the NPU, adding a board |
| [Deployment](docs/DEPLOYMENT.md) | Preparing the boards, deploying the whole cluster, configuration, operations, troubleshooting |
| [Architecture](docs/ARCHITECTURE.md) | Components, threading, execution and stream model, sequence diagrams, failure model, design decisions |
| [Memory Model](docs/MEMORY_MODEL.md) | Global address space, page striping, address translation, allocation, consistency and caching |
| [Protocol](docs/PROTOCOL.md) | Wire format and every message payload |
| [Programming Guide](docs/PROGRAMMING_GUIDE.md) | Host API reference, writing kernels, differences from NVIDIA CUDA |
| [Matrix Multiplication](docs/MATMUL.md) | Test case 1: decomposition, data movement, performance model |
| [Observability](docs/OBSERVABILITY.md) | Telemetry from every node: what is measured, how it flows, host API, `cl-top`, the web dashboard |
| [Dashboard](dashboard/README.md) | Running and configuring the web dashboard |
| [Testing](docs/TESTING.md) | Test plan, how to run a simulated cluster, verification results |
| [Roadmap](docs/ROADMAP.md) | Known limitations and planned work |

## Observability

Every worker (and the controller) reports its board model, memory, compute and OS thread counts, CPU, SoC temperature, the running kernel and network traffic once a second. You can read it with `clGetTelemetry()`, watch it with `cl-top`, or open the **web dashboard** (`dashboard/`: FastAPI + Next.js), which shows every Pi in the cluster live. See [Observability](docs/OBSERVABILITY.md).

## Quick start

Build and test on your machine; the tests run a simulated cluster on localhost:

```bash
cmake --preset release && cmake --build --preset release && ctest --preset release
```

Deploy to the boards (see [Deployment](docs/DEPLOYMENT.md) for preparing them first; for an Orange Pi 6 Plus cluster, copy `deploy/cluster.inventory.opi6plus.example` instead and add `-x "--tune-network --install-node"` to `install`):

```bash
cp deploy/cluster.inventory.example deploy/cluster.inventory
```

```bash
deploy/deploy-cluster.sh render
```

```bash
deploy/deploy-cluster.sh install && deploy/deploy-cluster.sh verify
```

`render` writes each node's config files and services to `deploy/rendered/` for review before anything is installed.

## Status

Code, documentation, the build system (CMake), configuration (`/etc/kudalite/*.conf`), systemd services, and install/deploy scripts are complete. They have been verified on a simulated cluster on one Linux machine: unit tests, integration tests, ThreadSanitizer, the full dashboard stack, and `install.sh` run inside a sandbox. **They have not yet run on Raspberry Pi 5 or Orange Pi 6 Plus hardware, or on macOS**; [Testing](docs/TESTING.md) lists exactly what was checked and the hardware acceptance plan.

> **Security:** the protocol is unauthenticated and unencrypted. Run the cluster on a private network segment. Workers only execute kernels compiled into their own binary; no code is ever sent over the network.

## License

Copyright 2026 Christopher Hinds, Stratum Labs llc.

KUDA-Lite is licensed under the [Apache License, Version 2.0](LICENSE). See [NOTICE](NOTICE) for attribution and trademark notices. Every source file carries the standard Apache 2.0 header with `SPDX-License-Identifier: Apache-2.0`.

---

*CUDA is a trademark of NVIDIA Corporation. KUDA-Lite is an independent project and is not affiliated with, endorsed by, or sponsored by NVIDIA. The documentation mentions CUDA only to compare KUDA-Lite with NVIDIA's platform.*
