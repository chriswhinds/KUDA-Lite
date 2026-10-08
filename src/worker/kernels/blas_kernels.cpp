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

// Built-in BLAS kernels: cl_sgemm, cl_dgemm, cl_saxpy.
// Host-side wrappers and parameter structs: include/kudalite/clblas.h. Design: docs/MATMUL.md.
#include <algorithm>
#include <vector>

#include "kudalite/cl_kernel.h"
#include "kudalite/clblas.h"

namespace {

// k-panel depth: a KB x tileN slice of B (128 x 128 floats = 64 KiB) stays in the Cortex-A76's
// 512 KiB L2 while every row of the tile streams over it.
constexpr size_t kKBlock = 128;

/// One block computes one tileM x tileN tile of C:
///   1. gather the A row panel (rows x K) and B column panel (K x cols) — cached, because
///      neighbouring blocks on this worker reuse them;
///   2. multiply on all cores (rows are split across threads);
///   3. write the tile back to C.
template <typename T>
void gemmBlock(clBlockContext& ctx, clArgReader& args) {
  const auto p = args.get<clGemmParams<T>>();
  const uint64_t tileM = ctx.blockDim.y;
  const uint64_t tileN = ctx.blockDim.x;
  const uint64_t row0 = uint64_t(ctx.blockIdx.y) * tileM;
  const uint64_t col0 = uint64_t(ctx.blockIdx.x) * tileN;
  if (row0 >= p.M || col0 >= p.N) return;
  const size_t rows = static_cast<size_t>(std::min<uint64_t>(tileM, p.M - row0));
  const size_t cols = static_cast<size_t>(std::min<uint64_t>(tileN, p.N - col0));
  const size_t K = p.K;
  constexpr size_t E = sizeof(T);

  T* a = static_cast<T*>(ctx.scratch(std::max<size_t>(1, rows * K) * E));  // rows x K, pitch K
  T* b = static_cast<T*>(ctx.scratch(std::max<size_t>(1, K * cols) * E));  // K x cols, pitch cols
  T* c = static_cast<T*>(ctx.scratch(rows * cols * E));                    // rows x cols

  if (K > 0) {
    ctx.read2D(a, K * E, p.A + row0 * p.lda * E, size_t(p.lda) * E, K * E, rows);
    ctx.read2D(b, cols * E, p.B + col0 * E, size_t(p.ldb) * E, cols * E, K);
  }
  const bool useBeta = p.beta != T(0);
  if (useBeta) {
    ctx.read2D(c, cols * E, p.C + (row0 * p.ldc + col0) * E, size_t(p.ldc) * E, cols * E, rows, clCache::Uncached);
  }

  ctx.parallelFor(rows, [&](size_t r0, size_t r1) {
    const size_t nr = r1 - r0;
    std::vector<T> acc(nr * cols, T(0));
    for (size_t kk = 0; kk < K; kk += kKBlock) {
      const size_t ke = std::min(K, kk + kKBlock);
      for (size_t i = 0; i < nr; ++i) {
        const T* ai = a + (r0 + i) * K;
        T* __restrict ci = acc.data() + i * cols;
        for (size_t k = kk; k < ke; ++k) {
          const T aik = ai[k];
          const T* __restrict bk = b + k * cols;
          for (size_t j = 0; j < cols; ++j) ci[j] += aik * bk[j];  // auto-vectorised (NEON / AVX)
        }
      }
    }
    for (size_t i = 0; i < nr; ++i) {
      T* out = c + (r0 + i) * cols;
      const T* in = acc.data() + i * cols;
      for (size_t j = 0; j < cols; ++j) out[j] = p.alpha * in[j] + (useBeta ? p.beta * out[j] : T(0));
    }
  });

  ctx.write2D(p.C + (row0 * p.ldc + col0) * E, size_t(p.ldc) * E, c, cols * E, cols * E, rows);
}

}  // namespace

CL_KERNEL(cl_sgemm) { gemmBlock<float>(ctx, args); }

CL_KERNEL(cl_dgemm) { gemmBlock<double>(ctx, args); }

// y[i] = alpha * x[i] + y[i]; each block handles blockDim.x consecutive elements.
CL_KERNEL(cl_saxpy) {
  const auto p = args.get<clAxpyParams>();
  const uint64_t per = ctx.blockDim.x;
  const uint64_t begin = ctx.blockLinear * per;
  if (begin >= p.n) return;
  const size_t count = static_cast<size_t>(std::min<uint64_t>(per, p.n - begin));
  const size_t bytes = count * sizeof(float);
  auto* x = static_cast<float*>(ctx.scratch(bytes));
  auto* y = static_cast<float*>(ctx.scratch(bytes));
  ctx.read(p.x + begin * sizeof(float), x, bytes, clCache::Uncached);
  ctx.read(p.y + begin * sizeof(float), y, bytes, clCache::Uncached);
  ctx.parallelFor(count, [&](size_t b, size_t e) {
    for (size_t i = b; i < e; ++i) y[i] = p.alpha * x[i] + y[i];
  });
  ctx.write(p.y + begin * sizeof(float), y, bytes);
}
