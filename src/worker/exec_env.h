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

// Worker-internal state behind clBlockContext, plus the kernel registry lookup.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/protocol.h"
#include "kudalite/cl_kernel.h"

namespace cl {

class DsmClient;
class PageCache;
class ThreadPool;

struct ExecEnv {
  DsmClient* dsm = nullptr;
  PageCache* cache = nullptr;
  ThreadPool* pool = nullptr;
  uint32_t workerId = kNoWorker;
  std::vector<std::vector<uint8_t>> scratch;  // recycled per block
  size_t scratchNext = 0;
};

clKernelFn findKernel(const std::string& name);
std::vector<std::string> kernelNames();

}  // namespace cl
