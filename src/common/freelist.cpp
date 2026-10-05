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

#include "common/freelist.h"

#include <algorithm>

namespace cl {

FreeListAllocator::FreeListAllocator(uint64_t capacity, uint64_t alignment)
    : capacity_(capacity / alignment * alignment), align_(alignment) {
  if (capacity_ > 0) free_.emplace(0, capacity_);
}

bool FreeListAllocator::allocate(uint64_t size, uint64_t* offset) {
  if (size == 0) size = 1;
  if (size > capacity_) return false;
  size = (size + align_ - 1) / align_ * align_;
  for (auto it = free_.begin(); it != free_.end(); ++it) {
    if (it->second < size) continue;
    const uint64_t off = it->first;
    const uint64_t rest = it->second - size;
    free_.erase(it);
    if (rest > 0) free_.emplace(off + size, rest);
    live_.emplace(off, size);
    used_ += size;
    *offset = off;
    return true;
  }
  return false;
}

bool FreeListAllocator::release(uint64_t offset) {
  auto live = live_.find(offset);
  if (live == live_.end()) return false;
  uint64_t off = offset;
  uint64_t len = live->second;
  live_.erase(live);
  used_ -= len;

  // Coalesce with the following hole ...
  auto next = free_.lower_bound(off);
  if (next != free_.end() && off + len == next->first) {
    len += next->second;
    next = free_.erase(next);
  }
  // ... and with the preceding one.
  if (next != free_.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == off) {
      off = prev->first;
      len += prev->second;
      free_.erase(prev);
    }
  }
  free_.emplace(off, len);
  return true;
}

uint64_t FreeListAllocator::largestFree() const {
  uint64_t best = 0;
  for (const auto& [off, len] : free_) best = std::max(best, len);
  return best;
}

}  // namespace cl
