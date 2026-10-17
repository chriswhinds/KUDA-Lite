// Copyright 2026 Christopher Hinds, Stratum Technologies LLC
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

// The whole cluster's RAM as one array: a unified memory view (needs -DKUDALITE_UNIFIED_MEMORY=ON).
#include <cstdio>

#include "kudalite/clblas.h"
#include "kudalite/kudalite.h"

int main() {
  if (clInit(nullptr) != clSuccess) return 1;  // $KUDALITE_CONTROLLER or 127.0.0.1:7070

  clUnifiedView_t view;
  const clError_t st = clUnifiedOpen(&view, 0, 0);  // 0 = as much cluster memory as possible
  if (st != clSuccess) {
    std::fprintf(stderr, "clUnifiedOpen: %s\n", clGetErrorString(st));
    return 1;
  }
  clUnifiedInfo info;
  clUnifiedGetInfo(view, &info);
  std::printf("%zu MiB across %u workers, mapped at %p\n", info.bytes >> 20, info.workers, info.base);

  // Plain pointer access: the first touch of each chunk maps it in (fetched, or zeroed after a discard).
  float* data = static_cast<float*>(info.base);
  const size_t n = 1u << 22;  // use the first 16 MiB here
  clUnifiedDiscard(view, 0, 2 * n * sizeof(float));  // all new data: skip fetching the old contents
  for (size_t i = 0; i < 2 * n; ++i) data[i] = 1.0f;

  // Release: push the writes to the workers, then let a kernel work on the same memory.
  clUnifiedSync(view);
  clblasSaxpy(nullptr, n, 3.0f, info.devPtr, info.devPtr + n * sizeof(float));
  clStreamSynchronize(nullptr);

  // Acquire: drop local copies so the kernel's results are fetched on the next read.
  clUnifiedInvalidate(view);
  std::printf("y[0] = %.1f (expected 4.0)\n", data[n]);

  clUnifiedClose(view);
  clShutdown();
  return 0;
}
