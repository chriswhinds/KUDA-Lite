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

// The worker's contribution to cluster global memory: one large anonymous mapping.
// Pages are committed lazily by the OS on first touch, so a large arena costs nothing until used.
#pragma once

#include <cstdint>

namespace cl {

class Arena {
 public:
  explicit Arena(uint64_t bytes);
  ~Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  uint8_t* data() const { return base_; }
  uint64_t size() const { return size_; }
  bool contains(uint64_t offset, uint64_t len) const { return offset <= size_ && len <= size_ - offset; }

 private:
  uint8_t* base_ = nullptr;
  uint64_t size_ = 0;
};

/// Physical RAM of this machine in bytes (0 if unknown).
uint64_t physicalMemoryBytes();

}  // namespace cl
