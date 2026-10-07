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

// cl-worker: one worker node (Raspberry Pi 5, Orange Pi 6 Plus, or any 64-bit Linux machine).
//
// Responsibilities
//  * Contributes an arena of RAM to cluster global memory and serves the data plane
//    (MemReadV / MemWriteV / MemFillV) for it, to the controller and to peer workers.
//  * Registers with the controller and follows its control messages (cluster map, allocation
//    table updates).
//  * Executes ExecBlocks requests one at a time on a dedicated executor thread — a worker
//    runs a single operation at a time — using every core for the blocks via a thread pool.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "common/address_map.h"
#include "common/dsm_client.h"
#include "common/net.h"
#include "common/protocol.h"
#include "common/rpc.h"
#include "common/thread_pool.h"
#include "worker/arena.h"
#include "worker/exec_env.h"
#include "worker/page_cache.h"

namespace cl {

struct WorkerOptions {
  std::string controllerHost = "127.0.0.1";
  uint16_t controllerPort = kDefaultWorkerPort;
  std::string bindHost = "0.0.0.0";
  uint16_t dataPort = kDefaultDataPort;  // 0 = ephemeral (handy for several workers on one host)
  std::string advertise;                 // address peers should dial; default = our side of the
                                         // controller connection
  uint64_t memBytes = 0;                 // arena size
  uint64_t cacheBytes = 0;               // remote page cache budget
  unsigned threads = 0;                  // 0 = all cores
  unsigned telemetryMs = 1000;           // telemetry push period; 0 = disabled
};

class Worker {
 public:
  explicit Worker(WorkerOptions opts);
  /// Runs until the controller connection is lost. Returns a process exit code.
  int run();

 private:
  struct ExecJob {
    std::shared_ptr<RpcPeer> peer;
    uint64_t requestId = 0;
    ExecBlocksRequest req;
  };

  void acceptLoop();
  void onControl(const std::shared_ptr<RpcPeer>& peer, Message&& m);
  void onData(const std::shared_ptr<RpcPeer>& peer, Message&& m);
  void execLoop();
  clError_t execute(const ExecBlocksRequest& req, uint64_t* blocksRun);
  void telemetryLoop();

  WorkerOptions opts_;
  std::unique_ptr<Arena> arena_;
  AddressMap map_;
  DsmClient dsm_;
  std::unique_ptr<PageCache> cache_;
  std::unique_ptr<ThreadPool> pool_;
  std::unique_ptr<Listener> listener_;
  std::atomic<uint32_t> id_{kNoWorker};

  std::mutex queueMu_;
  std::condition_variable queueCv_;
  std::deque<ExecJob> queue_;

  // Executor-thread state
  uint64_t lastLaunch_ = 0;
  ExecEnv env_;

  // Telemetry (written by the executor, read by the telemetry thread)
  std::mutex ctrlMu_;
  std::shared_ptr<RpcPeer> ctrl_;  // current controller connection
  std::atomic<uint64_t> execsCompleted_{0};
  std::atomic<uint64_t> blocksExecuted_{0};
  std::atomic<uint64_t> execBusyNs_{0};
  std::atomic<uint32_t> executing_{0};
  std::mutex kernelMu_;
  std::string currentKernel_;
};

}  // namespace cl
