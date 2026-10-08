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

// Global virtual address (GVA) space -> (worker, arena offset) translation.
//
// Every allocation is a contiguous GVA range cut into fixed-size pages. Page p lives on
// owners[p % n] at arena offset localBase[p % n] + (p / n) * pageSize, where n = owners.size().
// With one worker per page stripe this is a pure function of the Allocation record, so the
// controller only has to publish the record (AllocAdd) for every node to translate addresses.
// See docs/MEMORY_MODEL.md for worked examples.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <shared_mutex>
#include <vector>

#include "common/bytes.h"
#include "kudalite/cl_types.h"

namespace cl {

struct Allocation {
  clDevPtr base = 0;
  uint64_t size = 0;
  uint64_t pageSize = 0;
  std::vector<uint32_t> owners;     // worker ids, in stripe order
  std::vector<uint64_t> localBase;  // arena offset of this allocation's slice on owners[j]

  uint64_t pageCount() const { return (size + pageSize - 1) / pageSize; }
  uint64_t pagesOnOwner(size_t j) const {
    const uint64_t n = owners.size(), p = pageCount();
    return p / n + (j < p % n ? 1 : 0);
  }
  uint64_t localBytesOnOwner(size_t j) const { return pagesOnOwner(j) * pageSize; }
  bool contains(clDevPtr addr) const { return addr >= base && addr - base < size; }

  void encode(ByteWriter& w) const;
  static Allocation decode(ByteReader& r);
};

/// A piece of a global range that lives contiguously on one worker.
struct Extent {
  uint32_t worker;
  uint64_t localOffset;      // arena offset of the first byte
  uint64_t len;
  uint64_t requestOffset;    // offset of the first byte within the requested range
  clDevPtr pageGva;          // GVA of the page containing it (page-granular extents only)
  uint64_t pageLocalOffset;  // arena offset of that page
  uint64_t pageLen;          // valid bytes in that page (the last page may be short)
};

class AddressMap {
 public:
  void add(Allocation a);
  std::shared_ptr<const Allocation> remove(clDevPtr base);
  std::shared_ptr<const Allocation> find(clDevPtr addr) const;
  std::vector<std::shared_ptr<const Allocation>> snapshot() const;

  /// Splits [addr, addr+len) into page-granular extents, appended to *out. The whole range
  /// must lie inside one live allocation, otherwise clErrorInvalidDevicePointer.
  clError_t resolve(clDevPtr addr, uint64_t len, std::vector<Extent>* out) const;

 private:
  mutable std::shared_mutex mu_;
  std::map<clDevPtr, std::shared_ptr<const Allocation>> allocs_;
};

/// Merges neighbouring extents that are contiguous both in the request and on the same worker.
/// The page* fields of merged extents are no longer meaningful.
void coalesceExtents(std::vector<Extent>* extents);

}  // namespace cl
