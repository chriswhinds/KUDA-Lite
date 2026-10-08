// Copyright 2026 Christopher Hinds, Stratum Labs llc
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

// Fork-join thread pool used by workers to run a block's "threads" on all CPU cores.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace cl {

class ThreadPool {
 public:
  using Body = std::function<void(size_t begin, size_t end)>;

  /// `threads` is the total parallelism including the calling thread (0 = all cores).
  explicit ThreadPool(unsigned threads);
  ~ThreadPool();
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  unsigned size() const { return static_cast<unsigned>(threads_.size()) + 1; }

  /// Threads currently running a parallelFor body (telemetry; momentary value).
  unsigned busy() const { return busy_.load(std::memory_order_relaxed); }

  /// Calls body over disjoint chunks covering [0, n) using every thread, then returns.
  /// The first exception thrown by body is rethrown here. Calls from inside a body run inline.
  void parallelFor(size_t n, const Body& body);

 private:
  void loop();
  void runChunks();

  std::vector<std::thread> threads_;
  std::mutex callMu_;  // one parallelFor at a time
  std::mutex mu_;
  std::condition_variable wake_;
  std::condition_variable done_;
  const Body* body_ = nullptr;
  size_t n_ = 0;
  size_t chunk_ = 1;
  std::atomic<size_t> next_{0};
  std::atomic<unsigned> busy_{0};
  unsigned active_ = 0;
  uint64_t generation_ = 0;
  bool stop_ = false;
  std::exception_ptr error_;
};

}  // namespace cl
