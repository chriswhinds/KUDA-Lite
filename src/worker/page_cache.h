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

// Read cache for remote global-memory pages, scoped to one kernel launch.
//
// A cached read fetches whole pages from their owners and keeps them until the byte budget is
// exceeded (FIFO eviction) or the next launch starts. Concurrent readers of the same missing
// page share one fetch. Writes made through the kernel context invalidate overlapping pages.
// Pages owned by this worker are never cached — they are read straight from the arena.
//
// Coherence contract (the usual GPU global-memory rule between blocks): within a launch, a block
// must not rely on seeing another block's writes. Across launches everything is coherent,
// because the cache is dropped when a new launch id arrives.
#pragma once

#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/dsm_client.h"

namespace cl {

class PageCache {
 public:
  PageCache(DsmClient& dsm, uint64_t capacityBytes);

  clError_t readv(const std::vector<ReadSeg>& segs);
  void invalidate(clDevPtr addr, uint64_t len);
  void clear();

  struct Stats {
    uint64_t bytes = 0;
    uint64_t capacity = 0;
    uint64_t hits = 0;    // cumulative
    uint64_t misses = 0;  // cumulative
  };
  Stats stats();

 private:
  struct Entry {
    std::shared_ptr<std::vector<uint8_t>> data;
    std::shared_future<clError_t> ready;
  };
  void evictLocked();

  DsmClient& dsm_;
  const uint64_t capacity_;
  std::mutex mu_;
  std::unordered_map<clDevPtr, Entry> entries_;  // key: page GVA
  std::deque<clDevPtr> fifo_;
  uint64_t bytes_ = 0;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
};

}  // namespace cl
