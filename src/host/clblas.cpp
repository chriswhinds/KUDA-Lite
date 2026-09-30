// Host-side BLAS wrappers: validate arguments, choose a tiling, launch the built-in kernels.
#include "kudalite/clblas.h"

#include <cstdlib>

namespace {

/// Tile edge for GEMM. 128x128 tiles amortise the panel fetch well; smaller matrices use 64 so
/// there are enough blocks to keep every worker busy. $KUDALITE_GEMM_TILE overrides it.
uint32_t gemmTile(int M, int N) {
  if (const char* env = std::getenv("KUDALITE_GEMM_TILE")) {
    const long t = std::strtol(env, nullptr, 10);
    if (t >= 8 && t <= 4096) return static_cast<uint32_t>(t);
  }
  return (M >= 512 && N >= 512) ? 128u : 64u;
}

template <typename T>
clError_t gemm(const char* kernel, clStream_t stream, int M, int N, int K, T alpha, clDevPtr A, int lda, clDevPtr B,
               int ldb, T beta, clDevPtr C, int ldc) {
  if (M < 0 || N < 0 || K < 0) return clErrorInvalidValue;
  if (M == 0 || N == 0) return clSuccess;
  if (lda < (K > 0 ? K : 1) || ldb < N || ldc < N) return clErrorInvalidValue;
  const uint32_t tile = gemmTile(M, N);
  clGemmParams<T> p{};
  p.M = static_cast<uint32_t>(M);
  p.N = static_cast<uint32_t>(N);
  p.K = static_cast<uint32_t>(K);
  p.lda = static_cast<uint32_t>(lda);
  p.ldb = static_cast<uint32_t>(ldb);
  p.ldc = static_cast<uint32_t>(ldc);
  p.alpha = alpha;
  p.beta = beta;
  p.A = A;
  p.B = B;
  p.C = C;
  const clDim3 grid((p.N + tile - 1) / tile, (p.M + tile - 1) / tile);
  const clDim3 block(tile, tile);
  return clLaunchKernel(kernel, grid, block, &p, sizeof p, stream);
}

}  // namespace

clError_t clblasSgemm(clStream_t stream, int M, int N, int K, float alpha, clDevPtr A, int lda, clDevPtr B, int ldb,
                      float beta, clDevPtr C, int ldc) {
  return gemm<float>(CL_KERNEL_SGEMM, stream, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
}

clError_t clblasDgemm(clStream_t stream, int M, int N, int K, double alpha, clDevPtr A, int lda, clDevPtr B, int ldb,
                      double beta, clDevPtr C, int ldc) {
  return gemm<double>(CL_KERNEL_DGEMM, stream, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
}

clError_t clblasSaxpy(clStream_t stream, size_t n, float alpha, clDevPtr x, clDevPtr y) {
  if (n == 0) return clSuccess;
  constexpr uint64_t kPerBlock = 1u << 18;  // 1 MiB of floats per block
  const uint64_t blocks = (n + kPerBlock - 1) / kPerBlock;
  if (blocks > 0xFFFFFFFFull) return clErrorInvalidValue;
  const clAxpyParams p{n, alpha, 0, x, y};
  return clLaunchKernel(CL_KERNEL_SAXPY, clDim3(static_cast<uint32_t>(blocks)), clDim3(kPerBlock), &p, sizeof p,
                        stream);
}
