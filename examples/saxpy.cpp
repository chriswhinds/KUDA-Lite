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

// Minimal KUDA-Lite program: y = 2x + y on the Pi cluster.
// Compare with the classic GPU saxpy — the shape of the code is the same.
#include <cstdio>
#include <vector>

#include "kudalite/clblas.h"
#include "kudalite/kudalite.h"

#define CL_CHECK(expr)                                                        \
  do {                                                                        \
    const clError_t err_ = (expr);                                            \
    if (err_ != clSuccess) {                                                  \
      std::fprintf(stderr, "%s failed: %s\n", #expr, clGetErrorString(err_)); \
      return 1;                                                               \
    }                                                                         \
  } while (0)

int main() {
  const size_t n = 1u << 22;  // 4M floats = 16 MiB per vector
  std::vector<float> x(n, 1.0f), y(n, 2.0f);

  CL_CHECK(clInit(nullptr));  // $KUDALITE_CONTROLLER or 127.0.0.1:7070

  clDevPtr dx = 0, dy = 0;
  CL_CHECK(clMalloc(&dx, n * sizeof(float)));  // striped over every worker's RAM
  CL_CHECK(clMalloc(&dy, n * sizeof(float)));
  CL_CHECK(clMemcpyHtoD(dx, x.data(), n * sizeof(float)));
  CL_CHECK(clMemcpyHtoD(dy, y.data(), n * sizeof(float)));

  CL_CHECK(clblasSaxpy(nullptr, n, 2.0f, dx, dy));  // launches the "cl_saxpy" kernel
  CL_CHECK(clMemcpyDtoH(y.data(), dy, n * sizeof(float)));  // waits for the kernel

  size_t bad = 0;
  for (float v : y) bad += (v != 4.0f);
  std::printf("saxpy on %zu elements: %s\n", n, bad == 0 ? "correct" : "WRONG");

  CL_CHECK(clFree(dx));
  CL_CHECK(clFree(dy));
  CL_CHECK(clShutdown());
  return bad == 0 ? 0 : 1;
}
