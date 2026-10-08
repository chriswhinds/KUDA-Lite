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

#include "common/thread_pool.h"

#include <algorithm>

namespace cl {
namespace {
thread_local bool tInsideBody = false;
}

ThreadPool::ThreadPool(unsigned threads) {
  if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
  for (unsigned i = 1; i < threads; ++i) threads_.emplace_back([this] { loop(); });
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  wake_.notify_all();
  for (auto& t : threads_) t.join();
}

void ThreadPool::parallelFor(size_t n, const Body& body) {
  if (n == 0) return;
  if (threads_.empty() || tInsideBody || n == 1) {
    if (!tInsideBody) busy_.fetch_add(1, std::memory_order_relaxed);
    struct Done {
      ThreadPool* pool;
      bool counted;
      ~Done() {
        if (counted) pool->busy_.fetch_sub(1, std::memory_order_relaxed);
      }
    } done{this, !tInsideBody};
    body(0, n);
    return;
  }
  std::lock_guard<std::mutex> call(callMu_);
  {
    std::lock_guard<std::mutex> lk(mu_);
    body_ = &body;
    n_ = n;
    // ~4 chunks per thread balances load without much scheduling overhead.
    chunk_ = std::max<size_t>(1, n / (size_t(size()) * 4));
    next_.store(0);
    active_ = static_cast<unsigned>(threads_.size());
    error_ = nullptr;
    ++generation_;
  }
  wake_.notify_all();
  runChunks();
  std::unique_lock<std::mutex> lk(mu_);
  done_.wait(lk, [&] { return active_ == 0; });
  body_ = nullptr;
  if (error_) std::rethrow_exception(error_);
}

void ThreadPool::runChunks() {
  const bool outer = tInsideBody;
  tInsideBody = true;
  busy_.fetch_add(1, std::memory_order_relaxed);
  try {
    for (;;) {
      const size_t b = next_.fetch_add(chunk_);
      if (b >= n_) break;
      (*body_)(b, std::min(n_, b + chunk_));
    }
  } catch (...) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!error_) error_ = std::current_exception();
    next_.store(n_);  // stop handing out work
  }
  busy_.fetch_sub(1, std::memory_order_relaxed);
  tInsideBody = outer;
}

void ThreadPool::loop() {
  uint64_t seen = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(mu_);
      wake_.wait(lk, [&] { return stop_ || generation_ != seen; });
      if (stop_) return;
      seen = generation_;
    }
    runChunks();
    std::lock_guard<std::mutex> lk(mu_);
    if (--active_ == 0) done_.notify_all();
  }
}

}  // namespace cl
