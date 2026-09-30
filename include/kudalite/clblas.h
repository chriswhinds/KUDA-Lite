// KUDA-Lite BLAS — the GPU BLAS-library analogue. Row-major, no transposes.
//
// The host wrappers pick a tiling and launch the built-in worker kernels cl_sgemm / cl_dgemm /
// cl_saxpy. The parameter structs are shared verbatim between host and worker, so their layout
// is pinned with static_asserts.
#pragma once

#include <cstddef>

#include "kudalite/kudalite.h"

#define CL_KERNEL_SGEMM "cl_sgemm"
#define CL_KERNEL_DGEMM "cl_dgemm"
#define CL_KERNEL_SAXPY "cl_saxpy"

/// C = alpha * A(MxK) * B(KxN) + beta * C(MxN). Leading dimensions are in elements.
/// Launch shape: blockDim = (tileN, tileM), grid = (ceil(N/tileN), ceil(M/tileM)).
template <typename T>
struct clGemmParams {
  uint32_t M, N, K;
  uint32_t lda, ldb, ldc;
  T alpha, beta;
  clDevPtr A, B, C;
};
static_assert(sizeof(clGemmParams<float>) == 56, "clGemmParams<float> layout changed");
static_assert(offsetof(clGemmParams<float>, A) == 32, "clGemmParams<float> layout changed");
static_assert(sizeof(clGemmParams<double>) == 64, "clGemmParams<double> layout changed");
static_assert(offsetof(clGemmParams<double>, A) == 40, "clGemmParams<double> layout changed");

/// y = alpha * x + y over n floats. Launch shape: blockDim.x = elements per block.
struct clAxpyParams {
  uint64_t n;
  float alpha;
  uint32_t reserved;
  clDevPtr x, y;
};
static_assert(sizeof(clAxpyParams) == 32, "clAxpyParams layout changed");

clError_t clblasSgemm(clStream_t stream, int M, int N, int K, float alpha, clDevPtr A, int lda, clDevPtr B, int ldb,
                      float beta, clDevPtr C, int ldc);

clError_t clblasDgemm(clStream_t stream, int M, int N, int K, double alpha, clDevPtr A, int lda, clDevPtr B,
                      int ldb, double beta, clDevPtr C, int ldc);

clError_t clblasSaxpy(clStream_t stream, size_t n, float alpha, clDevPtr x, clDevPtr y);
