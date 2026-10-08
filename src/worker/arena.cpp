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

#include "worker/arena.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace cl {

Arena::Arena(uint64_t bytes) : size_(bytes) {
  if (bytes == 0) throw std::invalid_argument("arena size must be > 0");
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
  flags |= MAP_NORESERVE;
#endif
  void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (p == MAP_FAILED) {
    throw std::runtime_error("cannot map " + std::to_string(bytes) + " byte arena: " + std::strerror(errno));
  }
  base_ = static_cast<uint8_t*>(p);
}

Arena::~Arena() {
  if (base_ != nullptr) ::munmap(base_, size_);
}

uint64_t physicalMemoryBytes() {
  const long pages = ::sysconf(_SC_PHYS_PAGES);
  const long pageSize = ::sysconf(_SC_PAGESIZE);
  if (pages <= 0 || pageSize <= 0) return 0;
  return static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize);
}

}  // namespace cl
