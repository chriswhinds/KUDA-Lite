// Copyright 2026 Christopher Hinds, Stratum Labs
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// KUDA-Lite — core public types.
//
// Shared by the host runtime (libkudalite), the cluster controller and the workers.
// Everything in this header is plain data so it can be used on both sides of the wire.
#pragma once

#include <cstddef>
#include <cstdint>

/// Address inside the cluster-wide global memory space ("device pointer").
/// The host may offset it with byte arithmetic but can never dereference it.
typedef uint64_t clDevPtr;

typedef enum clError {
  clSuccess = 0,
  clErrorInvalidValue = 1,           // bad argument
  clErrorMemoryAllocation = 2,       // cluster memory exhausted
  clErrorInitializationError = 3,    // could not reach / handshake with the controller
  clErrorInvalidDevicePointer = 4,   // address not inside a live allocation (or range overruns it)
  clErrorInvalidResourceHandle = 5,  // bad stream / event handle, event never recorded
  clErrorKernelNotFound = 6,         // no kernel with that name is registered on the workers
  clErrorLaunchFailure = 7,          // kernel threw / aborted while running
  clErrorNotReady = 8,               // asynchronous work still in flight (query functions)
  clErrorNetwork = 9,                // connection dropped or I/O failure
  clErrorWorkerLost = 10,            // a worker left the cluster while work depended on it
  clErrorProtocol = 11,              // malformed or unexpected message
  clErrorNoWorkers = 12,             // cluster has no live workers
  clErrorUnknown = 999
} clError_t;

/// Grid / block dimensions, same meaning as dim3 in GPU programming models.
struct clDim3 {
  uint32_t x, y, z;
  constexpr clDim3(uint32_t x_ = 1, uint32_t y_ = 1, uint32_t z_ = 1) : x(x_), y(y_), z(z_) {}
  constexpr uint64_t count() const { return uint64_t(x) * uint64_t(y) * uint64_t(z); }
};

/// How an allocation's pages are spread across the workers' RAM.
typedef enum clDistribution {
  clDistStriped = 0,  // fixed-size pages dealt round-robin over all workers (default)
  clDistBlocked = 1   // one contiguous block per worker (page size = ceil(size / workers))
} clDistribution;

/// Cluster-wide properties; the cluster is presented to the host as a single "device".
struct clDeviceProp {
  char name[64];
  uint32_t workerCount;     // live workers
  uint32_t totalCores;      // sum of worker CPU cores
  uint64_t totalGlobalMem;  // sum of worker arenas, bytes
  uint64_t freeGlobalMem;   // unallocated bytes across all arenas
  uint32_t pageSize;        // default striping page size, bytes
  uint32_t protocolVersion;
};

struct clWorkerInfo {
  uint32_t id;
  char hostname[64];
  char address[64];  // address the data plane listens on
  uint16_t dataPort;
  uint32_t cores;
  uint64_t memBytes;  // arena capacity
  uint64_t memUsed;   // bytes allocated in the arena
};

/// Scheduling statistics for the most recent kernel launch on a stream.
struct clLaunchInfo {
  uint64_t blocks;       // blocks executed
  uint32_t workersUsed;  // workers that executed at least one block
  double wallMs;         // controller-side wall time of the launch
};

/// Telemetry: which kind of Pi a node is.
typedef enum clNodeRole { clNodeWorker = 0, clNodeController = 1 } clNodeRole;

/// Id used for the controller in clNodeTelemetry::id.
#define CL_CONTROLLER_NODE_ID 0xFFFFFFFEu

/// Latest telemetry of one node (worker or controller). See docs/OBSERVABILITY.md.
struct clNodeTelemetry {
  uint32_t id;           // worker id, or CL_CONTROLLER_NODE_ID
  clNodeRole role;
  uint32_t alive;        // 0 once the node has disconnected
  char hostname[64];
  char address[64];      // data plane (workers) or host API endpoint (controller)
  uint64_t lastSeenAgeMs;  // age of the newest sample at the controller; UINT64_MAX if none yet
  uint32_t hasSample;      // 0 until the node's first sample arrives; the fields below are then 0

  uint64_t timestampMs;  // node wall clock of the sample (Unix epoch ms)
  uint64_t uptimeMs;     // daemon uptime
  uint64_t memTotalBytes, memAvailableBytes;  // whole system
  uint64_t rssBytes;                          // daemon resident set
  uint32_t processThreads;                    // OS threads in the daemon
  uint64_t arenaBytes;                        // arena contributed to global memory
  uint64_t arenaUsedBytes;                    // of which allocated (controller's accounting)
  uint64_t cacheBytes, cacheCapacityBytes, cacheHits, cacheMisses;
  uint32_t computeThreads;  // kernel thread-pool size
  uint32_t busyThreads;     // pool threads running a kernel body right now
  uint32_t cpuCores;
  float cpuPercent;  // whole system, 0..100
  float load1;
  float cpuTempC;    // -1 if unknown
  uint32_t cpuFreqMHz;
  uint32_t queueDepth;  // operations waiting on the worker
  uint32_t executing;   // worker: 1 while running an operation; controller: launches in progress
  uint64_t execsCompleted, blocksExecuted, execBusyNs;  // cumulative
  uint64_t netRxBytes, netTxBytes;                      // cumulative KUDA-Lite traffic
  char currentKernel[64];                               // "" when idle
  char board[64];  // hardware model, e.g. "Orange Pi 6 Plus" ("" from nodes older than 0.3)
};

/// Cluster-wide counters kept by the controller.
struct clClusterTelemetry {
  uint64_t controllerUptimeMs;
  uint32_t sessions;        // connected host processes
  uint32_t activeLaunches;
  uint64_t launchesTotal;
  uint32_t allocations;
  uint64_t allocatedBytes;  // sum of live allocation sizes
  uint64_t arenaTotalBytes; // sum of live worker arenas
};

typedef struct clStream_st* clStream_t;
typedef struct clEvent_st* clEvent_t;
