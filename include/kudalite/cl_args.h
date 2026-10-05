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

// KUDA-Lite — kernel argument packing.
//
// Kernel arguments travel as an opaque byte buffer (like the kernel-parameter buffer of GPU runtimes). The host packs
// values with clKernelArgs; the kernel unpacks them *in the same order* with clArgReader.
// Only trivially copyable types are allowed. Both ends are little-endian LP64 (x86-64,
// AArch64), so plain structs with naturally aligned fields have identical layouts.
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <vector>

class clKernelArgs {
 public:
  template <typename T>
  clKernelArgs& push(const T& value) {
    static_assert(std::is_trivially_copyable<T>::value, "kernel arguments must be trivially copyable");
    const size_t off = bytes_.size();
    bytes_.resize(off + sizeof(T));
    std::memcpy(bytes_.data() + off, &value, sizeof(T));
    return *this;
  }
  template <typename T>
  clKernelArgs& operator<<(const T& value) { return push(value); }

  const void* data() const { return bytes_.data(); }
  size_t size() const { return bytes_.size(); }

 private:
  std::vector<uint8_t> bytes_;
};

class clArgReader {
 public:
  clArgReader(const void* data, size_t size) : p_(static_cast<const uint8_t*>(data)), n_(size) {}

  template <typename T>
  T get() {
    static_assert(std::is_trivially_copyable<T>::value, "kernel arguments must be trivially copyable");
    if (sizeof(T) > n_ - pos_) throw std::out_of_range("kernel argument buffer too short");
    T value;
    std::memcpy(&value, p_ + pos_, sizeof(T));
    pos_ += sizeof(T);
    return value;
  }
  size_t remaining() const { return n_ - pos_; }

 private:
  const uint8_t* p_;
  size_t n_;
  size_t pos_ = 0;
};
