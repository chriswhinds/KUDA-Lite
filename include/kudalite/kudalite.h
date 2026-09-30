// KUDA-Lite host runtime API (libkudalite).
//
// This library is the KUDA-Lite "kernel" on the host side: a Linux or macOS program links
// against it to allocate cluster memory, move data and launch kernels on the Raspberry Pi
// worker cluster. It plays the role a GPU vendor's runtime library plays. See docs/PROGRAMMING_GUIDE.md.
//
// Conventions (mirroring common GPU runtime APIs):
//  * Every call returns clError_t. Failures of asynchronous work are reported by the next
//    synchronising call on the same stream (clStreamSynchronize, clEventSynchronize,
//    clDeviceSynchronize, or a synchronous copy on that stream).
//  * A null clStream_t means the default stream (stream 0). Unlike the legacy default stream
//    of GPU runtimes, stream 0 does NOT implicitly synchronise with other streams.
//  * Async copies read/write host memory after the call returns: the host buffer must stay
//    valid until the stream is synchronised. Synchronous variants return after completion.
//  * The first API call connects implicitly (clInit(nullptr)) if clInit was not called.
#pragma once

#include "kudalite/cl_args.h"
#include "kudalite/cl_types.h"

#define KUDALITE_VERSION_MAJOR 0
#define KUDALITE_VERSION_MINOR 1

// ---------------------------------------------------------------------------------------------
// Initialisation and errors
// ---------------------------------------------------------------------------------------------

/// Connects to the cluster controller. `controller` is "host[:port]" (default port 7070).
/// nullptr uses $KUDALITE_CONTROLLER, falling back to "127.0.0.1:7070".
clError_t clInit(const char* controller);

/// Synchronises all streams, closes the session and releases every allocation it owns.
clError_t clShutdown();

const char* clGetErrorString(clError_t err);

/// Returns the last error produced by any API call and resets it to clSuccess.
clError_t clGetLastError();

// ---------------------------------------------------------------------------------------------
// Cluster ("device") information
// ---------------------------------------------------------------------------------------------

clError_t clGetDeviceProperties(clDeviceProp* prop);

/// Fills up to `capacity` entries; `*count` receives the number of live workers.
clError_t clGetWorkerInfo(clWorkerInfo* infos, uint32_t capacity, uint32_t* count);

// ---------------------------------------------------------------------------------------------
// Observability
// ---------------------------------------------------------------------------------------------

/// Latest telemetry for every node (controller first, then workers in id order, including
/// workers that have disconnected, with alive = 0). `cluster` may be null. Fills up to
/// `capacity` entries of `nodes`; `*count` receives the total number of nodes.
clError_t clGetTelemetry(clClusterTelemetry* cluster, clNodeTelemetry* nodes, uint32_t capacity, uint32_t* count);

// ---------------------------------------------------------------------------------------------
// Global memory (the pooled RAM of all workers)
// ---------------------------------------------------------------------------------------------

/// Allocates `bytes` of global memory striped across every worker (default page size).
clError_t clMalloc(clDevPtr* ptr, size_t bytes);

/// Allocation with an explicit distribution. pageSize 0 = cluster default; ignored for
/// clDistBlocked. pageSize must be a multiple of 64.
clError_t clMallocEx(clDevPtr* ptr, size_t bytes, clDistribution dist, uint32_t pageSize);

/// Frees an allocation. Implicitly synchronises every stream first (as GPU runtimes' free calls do).
clError_t clFree(clDevPtr ptr);

clError_t clMemcpyHtoD(clDevPtr dst, const void* src, size_t bytes);
clError_t clMemcpyDtoH(void* dst, clDevPtr src, size_t bytes);
clError_t clMemcpyDtoD(clDevPtr dst, clDevPtr src, size_t bytes);  // ranges must not overlap
clError_t clMemset(clDevPtr dst, int value, size_t bytes);

clError_t clMemcpyHtoDAsync(clDevPtr dst, const void* src, size_t bytes, clStream_t stream);
clError_t clMemcpyDtoHAsync(void* dst, clDevPtr src, size_t bytes, clStream_t stream);
clError_t clMemcpyDtoDAsync(clDevPtr dst, clDevPtr src, size_t bytes, clStream_t stream);
clError_t clMemsetAsync(clDevPtr dst, int value, size_t bytes, clStream_t stream);

// ---------------------------------------------------------------------------------------------
// Streams — ordered work queues. Work in different streams may run concurrently.
// ---------------------------------------------------------------------------------------------

clError_t clStreamCreate(clStream_t* stream);
clError_t clStreamDestroy(clStream_t stream);        // synchronises first
clError_t clStreamSynchronize(clStream_t stream);    // returns (and clears) the stream's first error
clError_t clStreamQuery(clStream_t stream);          // clSuccess if idle, else clErrorNotReady
clError_t clStreamWaitEvent(clStream_t stream, clEvent_t event);  // later work waits for event

// ---------------------------------------------------------------------------------------------
// Events — markers in a stream, timestamped by the controller.
// ---------------------------------------------------------------------------------------------

clError_t clEventCreate(clEvent_t* event);
clError_t clEventDestroy(clEvent_t event);
clError_t clEventRecord(clEvent_t event, clStream_t stream);
clError_t clEventSynchronize(clEvent_t event);
clError_t clEventQuery(clEvent_t event);
clError_t clEventElapsedTime(float* ms, clEvent_t start, clEvent_t end);

// ---------------------------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------------------------

/// Launches the kernel registered on the workers under `kernel` over `grid` blocks.
/// `block` is passed to every block as blockDim; its meaning is defined by the kernel
/// (there is no 1024-thread limit — a block is executed by one worker using all its cores).
/// `args` (argBytes long) is copied before the call returns.
clError_t clLaunchKernel(const char* kernel, clDim3 grid, clDim3 block, const void* args, size_t argBytes,
                         clStream_t stream);

/// Waits for all work in all streams of this process. Returns the first error found.
clError_t clDeviceSynchronize();

/// Scheduling statistics of the last completed launch on `stream`.
clError_t clGetLastLaunchInfo(clStream_t stream, clLaunchInfo* info);

/// Convenience: packs `args...` in order and launches.
///   clLaunch("my_kernel", grid, block, stream, n, ptrA, ptrB);
template <typename... Args>
clError_t clLaunch(const char* kernel, clDim3 grid, clDim3 block, clStream_t stream, const Args&... args) {
  clKernelArgs pack;
  (pack.push(args), ...);
  return clLaunchKernel(kernel, grid, block, pack.data(), pack.size(), stream);
}
