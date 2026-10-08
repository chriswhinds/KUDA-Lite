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

// First-fit free-list allocator over an abstract [0, capacity) range.
// The controller keeps one per worker to place allocation slices inside that worker's arena.
// Not thread-safe; the owner serialises access.
#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>

namespace cl {

class FreeListAllocator {
 public:
  explicit FreeListAllocator(uint64_t capacity = 0, uint64_t alignment = 64);

  /// Reserves `size` bytes (rounded up to the alignment). Returns false if no hole fits.
  bool allocate(uint64_t size, uint64_t* offset);

  /// Releases a block previously returned by allocate(). Returns false for unknown offsets.
  bool release(uint64_t offset);

  uint64_t capacity() const { return capacity_; }
  uint64_t used() const { return used_; }
  uint64_t largestFree() const;

 private:
  uint64_t capacity_;
  uint64_t align_;
  uint64_t used_ = 0;
  std::map<uint64_t, uint64_t> free_;            // offset -> length, non-adjacent holes
  std::unordered_map<uint64_t, uint64_t> live_;  // offset -> length
};

}  // namespace cl
