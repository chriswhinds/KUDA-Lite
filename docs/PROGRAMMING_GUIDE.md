<!--
Copyright 2026 Christopher Hinds, Stratum Labs
SPDX-License-Identifier: Apache-2.0 (see the LICENSE file at the project root)
-->

# KUDA-Lite Programming Guide

## 1. Host API at a glance

Header: [`include/kudalite/kudalite.h`](../include/kudalite/kudalite.h). Every call returns `clError_t`.

| Area | Functions | NVIDIA CUDA analogue |
|---|---|---|
| Setup | `clInit(endpoint)`, `clShutdown()`, `clGetErrorString`, `clGetLastError` | implicit context, `cudaDeviceReset` |
| Device info | `clGetDeviceProperties(&prop)`, `clGetWorkerInfo(infos, cap, &n)` | `cudaGetDeviceProperties` |
| Observability | `clGetTelemetry(&cluster, nodes, cap, &n)` ([Observability](OBSERVABILITY.md)) | NVML |
| Memory | `clMalloc`, `clMallocEx(ptr, bytes, dist, pageSize)`, `clFree` | `cudaMalloc`, `cudaFree` |
| Copies | `clMemcpyHtoD/DtoH/DtoD[Async]`, `clMemset[Async]` | `cuMemcpyHtoD` … |
| Streams | `clStreamCreate/Destroy/Synchronize/Query`, `clStreamWaitEvent` | `cudaStream*` |
| Events | `clEventCreate/Destroy/Record/Synchronize/Query/ElapsedTime` | `cudaEvent*` |
| Launch | `clLaunchKernel(name, grid, block, args, bytes, stream)`, `clLaunch(name, grid, block, stream, args...)` | `cudaLaunchKernel`, `<<< >>>` |
| Sync | `clDeviceSynchronize()`, `clGetLastLaunchInfo(stream, &info)` | `cudaDeviceSynchronize` |
| BLAS | `clblasSgemm`, `clblasDgemm`, `clblasSaxpy` ([`clblas.h`](../include/kudalite/clblas.h)) | cuBLAS |

### Connecting

```cpp
clInit("pi-controller:7070");   // or nullptr: $KUDALITE_CONTROLLER, else 127.0.0.1:7070
```

If you skip `clInit`, the first API call connects with the default endpoint.

### Device pointers

`clDevPtr` is a 64-bit integer, not a C pointer. You can offset it by bytes (`buf + i * sizeof(float)`), but dereferencing it is impossible by construction. Global memory is only reachable through the API and through kernels.

### Errors and asynchrony

- Async functions return `clSuccess` once the work is *queued*. Execution errors (a bad pointer range, a missing kernel, a lost worker) are reported by the next `clStreamSynchronize`, `clEventSynchronize`, `clDeviceSynchronize`, or synchronous copy on that stream. Reporting clears the error.
- Buffers passed to `clMemcpyDtoHAsync` must stay valid until the stream is synchronised. `clMemcpyHtoDAsync` copies the source before it returns.
- `clGetLastError()` returns and clears the most recent error from any call.

### Streams

```cpp
clStream_t s1, s2;  clEvent_t ready;
clStreamCreate(&s1); clStreamCreate(&s2); clEventCreate(&ready);
clMemcpyHtoDAsync(a, hostA, bytes, s1);
clEventRecord(ready, s1);
clStreamWaitEvent(s2, ready);              // s2 waits for the copy on s1
clLaunch("my_kernel", grid, block, s2, a, n);
clStreamSynchronize(s2);
```

The default stream (`nullptr`) is an ordinary stream. It does not implicitly synchronise with other streams.

## 2. Writing kernels

Kernels are compiled into the worker binary. Put them in `src/worker/kernels/`: one `.cpp` file, any number of kernels. Every worker must run a binary that contains the kernel.

### 2.1 Anatomy

```cpp
#include "kudalite/cl_kernel.h"

struct ScaleParams { uint64_t n; float s; uint32_t pad; clDevPtr x; };   // shared with the host

CL_KERNEL(my_scale) {                                    // registered as "my_scale"
  const auto p = args.get<ScaleParams>();                // unpack in the order the host packed
  const uint64_t per = ctx.blockDim.x;                   // elements per block (kernel-defined)
  const uint64_t begin = ctx.blockLinear * per;
  if (begin >= p.n) return;
  const size_t count = std::min<uint64_t>(per, p.n - begin);

  float* buf = static_cast<float*>(ctx.scratch(count * sizeof(float)));   // "__shared__"
  ctx.read(p.x + begin * 4, buf, count * 4, clCache::Uncached);           // one batched fetch
  ctx.parallelFor(count, [&](size_t b, size_t e) {                         // the block's "threads"
    for (size_t i = b; i < e; ++i) buf[i] *= p.s;
  });                                                                       // implicit barrier
  ctx.write(p.x + begin * 4, buf, count * 4);                               // write-through
}
```

Host side:

```cpp
ScaleParams p{n, 3.0f, 0, x};
clLaunchKernel("my_scale", clDim3((n + 65535) / 65536), clDim3(65536), &p, sizeof p, stream);
// or: clLaunch("my_scale", grid, block, stream, p);
```

### 2.2 `clBlockContext` reference

| Member | Meaning |
|---|---|
| `gridDim`, `blockDim`, `blockIdx`, `blockLinear` | Launch geometry and this block's position |
| `read(src, dst, bytes, cache)` | Global → local. One round trip per owning worker. |
| `read2D(dst, dstPitch, src, srcPitch, width, rows, cache)` | Strided gather (sub-matrices) in a single batched round trip |
| `write(dst, src, bytes)`, `write2D(...)` | Local → global, write-through to the owners |
| `parallelFor(n, body(begin, end))` | Runs body on disjoint chunks of `[0, n)` over all cores and waits. Nested calls run inline. |
| `numThreads()` | Cores in use (the size of `parallelFor`'s pool) |
| `scratch(bytes)` | Per-block local memory. Each call returns a distinct region, recycled for the next block, and not zeroed. |
| `workerId()` | Which worker is running this block |

Memory errors throw `clKernelFault`, which the worker reports as the launch's error. You do not need to check return values.

### 2.3 Rules and performance guidance

1. **Batch memory access.** Every `read`/`write` call is at least one network round trip (roughly 100–200 µs on 1 GbE). Fetch whole tiles with `read2D` rather than element by element.
2. **Blocks are independent.** Blocks may run in any order, on any worker, concurrently. Do not read data that another block of the same launch writes ([Memory Model §5](MEMORY_MODEL.md#5-consistency-model)).
3. **Use the cache for reuse, bypass it for streams.** Neighbouring blocks on one worker share cached pages within a launch.
4. **Make blocks big enough.** A block should do tens of milliseconds of work so that fetch latency and scheduling cost (one RPC per chunk) are amortised.
5. **Parameter structs** must be trivially copyable, with naturally aligned fields and explicit padding. Pin their layout with `static_assert`, as `clblas.h` does.
6. Kernels must be reentrant with respect to `parallelFor` bodies. Keep per-thread state local to the body.

### 2.4 Adding a kernel

1. Create `src/worker/kernels/my_kernels.cpp` with `CL_KERNEL(...)` definitions.
2. Put parameter structs in a header shared with the host program.
3. Rebuild and redeploy `cl-worker` on every worker. At registration the controller logs how many kernels each worker reported.

## 3. Differences from NVIDIA CUDA

| CUDA | KUDA-Lite | Why |
|---|---|---|
| Per-thread kernels, `threadIdx`, `__syncthreads()` | Per-block kernels, `parallelFor` (implicit barrier) | A board has 4 (Pi 5) or 12 (Orange Pi 6 Plus) CPU cores, not thousands of lanes |
| `__shared__` | `ctx.scratch()` | |
| Direct pointer dereference of global memory | `ctx.read*/write*` | Global memory lives across a network |
| `void*` device pointers | `clDevPtr` (u64) | Prevents accidental host dereference |
| Legacy default stream syncs with all streams | Stream 0 is an ordinary stream | Simplicity |
| Kernels in fatbinaries, loaded at runtime | Kernels compiled into `cl-worker` | Security (no code over the network) and simplicity |
| ≤ 1024 threads per block | `blockDim` is kernel-defined, no limit | A block runs on one whole node |
| Atomics, cooperative groups, unified memory | Not in v1 | See [Roadmap](ROADMAP.md) |
| Error stays sticky for the context | Cleared once reported by a sync | Simpler recovery in long-running programs |
| Overlapping `cudaMemcpy` is undefined | `clMemcpyDtoD` with overlapping ranges is undefined | Same |
