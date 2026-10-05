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

// Test case 1: matrix multiplication on the cluster, verified against a CPU reference.
//
//   test_matmul [--controller HOST[:PORT]] [--sizes LIST] [--repeat N] [--seed S]
//
// LIST is comma-separated; each entry is "N" (square) or "MxNxK". Default:
//   64,257x129x300,512,1024
// Every size runs sgemm with (alpha=1, beta=0) and (alpha=0.5, beta=2), and the first size also
// runs dgemm. Results are compared with a double-precision reference using the standard GEMM
// forward-error bound |C - C_ref| <= c * K * eps * (|alpha| |A||B| + |beta| |C|).
// For large problems (M*N*K > 2^31) only 64 sampled rows are checked.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "kudalite/clblas.h"
#include "kudalite/kudalite.h"

namespace {

struct Shape {
  int M, N, K;
};

bool parseSizes(const std::string& text, std::vector<Shape>* out) {
  std::stringstream ss(text);
  std::string item;
  while (std::getline(ss, item, ',')) {
    int m = 0, n = 0, k = 0;
    if (std::sscanf(item.c_str(), "%dx%dx%d", &m, &n, &k) == 3) {
      out->push_back({m, n, k});
    } else if (std::sscanf(item.c_str(), "%d", &m) == 1) {
      out->push_back({m, m, m});
    } else {
      return false;
    }
    if (out->back().M <= 0 || out->back().N <= 0 || out->back().K <= 0) return false;
  }
  return !out->empty();
}

#define REQUIRE_OK(expr)                                                                          \
  do {                                                                                            \
    const clError_t e_ = (expr);                                                                  \
    if (e_ != clSuccess) {                                                                        \
      std::fprintf(stderr, "%s:%d: %s -> %s\n", __FILE__, __LINE__, #expr, clGetErrorString(e_)); \
      return false;                                                                               \
    }                                                                                             \
  } while (0)

double seconds(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

template <typename T>
clError_t gemm(clStream_t s, const Shape& sh, T alpha, clDevPtr A, clDevPtr B, T beta, clDevPtr C);
template <>
clError_t gemm<float>(clStream_t s, const Shape& sh, float alpha, clDevPtr A, clDevPtr B, float beta, clDevPtr C) {
  return clblasSgemm(s, sh.M, sh.N, sh.K, alpha, A, sh.K, B, sh.N, beta, C, sh.N);
}
template <>
clError_t gemm<double>(clStream_t s, const Shape& sh, double alpha, clDevPtr A, clDevPtr B, double beta, clDevPtr C) {
  return clblasDgemm(s, sh.M, sh.N, sh.K, alpha, A, sh.K, B, sh.N, beta, C, sh.N);
}

template <typename T>
bool runCase(const Shape& sh, T alpha, T beta, int repeat, uint32_t seed) {
  const char* type = sizeof(T) == 4 ? "sgemm" : "dgemm";
  const size_t M = sh.M, N = sh.N, K = sh.K;
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> dist(-1.0, 1.0);
  std::vector<T> A(M * K), B(K * N), C0(M * N), C(M * N);
  for (auto& v : A) v = static_cast<T>(dist(rng));
  for (auto& v : B) v = static_cast<T>(dist(rng));
  for (auto& v : C0) v = static_cast<T>(dist(rng));

  clDevPtr dA = 0, dB = 0, dC = 0;
  REQUIRE_OK(clMalloc(&dA, A.size() * sizeof(T)));
  REQUIRE_OK(clMalloc(&dB, B.size() * sizeof(T)));
  REQUIRE_OK(clMalloc(&dC, C.size() * sizeof(T)));
  clEvent_t start = nullptr, stop = nullptr;
  REQUIRE_OK(clEventCreate(&start));
  REQUIRE_OK(clEventCreate(&stop));

  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE_OK(clMemcpyHtoD(dA, A.data(), A.size() * sizeof(T)));
  REQUIRE_OK(clMemcpyHtoD(dB, B.data(), B.size() * sizeof(T)));
  const auto t1 = std::chrono::steady_clock::now();

  float bestMs = std::numeric_limits<float>::max();
  clLaunchInfo info{};
  for (int rep = 0; rep < repeat; ++rep) {
    REQUIRE_OK(clMemcpyHtoD(dC, C0.data(), C0.size() * sizeof(T)));  // beta != 0 reads C
    REQUIRE_OK(clEventRecord(start, nullptr));
    REQUIRE_OK(gemm<T>(nullptr, sh, alpha, dA, dB, beta, dC));
    REQUIRE_OK(clEventRecord(stop, nullptr));
    REQUIRE_OK(clEventSynchronize(stop));
    REQUIRE_OK(clStreamSynchronize(nullptr));
    float ms = 0;
    REQUIRE_OK(clEventElapsedTime(&ms, start, stop));
    bestMs = std::min(bestMs, ms);
    REQUIRE_OK(clGetLastLaunchInfo(nullptr, &info));
  }

  const auto t2 = std::chrono::steady_clock::now();
  REQUIRE_OK(clMemcpyDtoH(C.data(), dC, C.size() * sizeof(T)));
  const auto t3 = std::chrono::steady_clock::now();

  // Reference check (all rows, or a sample for large problems).
  std::vector<size_t> rows;
  const bool sampled = double(M) * N * K > double(1ull << 31);
  if (sampled) {
    std::uniform_int_distribution<size_t> pick(0, M - 1);
    for (int i = 0; i < 64; ++i) rows.push_back(pick(rng));
  } else {
    for (size_t i = 0; i < M; ++i) rows.push_back(i);
  }
  const double eps = std::numeric_limits<T>::epsilon();
  double worst = 0;  // max |err| / bound, must stay <= 1
  std::vector<double> ref(N), mag(N);
  for (size_t i : rows) {
    std::fill(ref.begin(), ref.end(), 0.0);
    std::fill(mag.begin(), mag.end(), 0.0);
    for (size_t k = 0; k < K; ++k) {
      const double a = A[i * K + k];
      for (size_t j = 0; j < N; ++j) {
        ref[j] += a * B[k * N + j];
        mag[j] += std::fabs(a * B[k * N + j]);
      }
    }
    for (size_t j = 0; j < N; ++j) {
      const double expect = double(alpha) * ref[j] + double(beta) * C0[i * N + j];
      const double bound =
          4.0 * double(K + 2) * eps * (std::fabs(double(alpha)) * mag[j] + std::fabs(double(beta) * C0[i * N + j])) +
          std::numeric_limits<double>::min();
      worst = std::max(worst, std::fabs(double(C[i * N + j]) - expect) / bound);
    }
  }
  const bool pass = worst <= 1.0;
  const double gflops = 2.0 * M * N * K / (bestMs * 1e6);
  std::printf("%s %5zux%5zux%5zu a=%-3g b=%-3g  kernel %9.2f ms %7.2f GFLOP/s | H2D %7.1f ms D2H %7.1f ms | "
              "%llu blocks on %u worker(s) | err/bound %.3f%s  %s\n",
              type, M, N, K, double(alpha), double(beta), bestMs, gflops, seconds(t0, t1) * 1e3,
              seconds(t2, t3) * 1e3, static_cast<unsigned long long>(info.blocks), info.workersUsed, worst,
              sampled ? " (sampled)" : "", pass ? "PASS" : "FAIL");

  clEventDestroy(start);
  clEventDestroy(stop);
  REQUIRE_OK(clFree(dA));
  REQUIRE_OK(clFree(dB));
  REQUIRE_OK(clFree(dC));
  return pass;
}

}  // namespace

int main(int argc, char** argv) {
  const char* controller = nullptr;
  std::string sizes = "64,257x129x300,512,1024";
  int repeat = 1;
  uint32_t seed = 12345;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    if (k == "--controller") controller = argv[i + 1];
    else if (k == "--sizes") sizes = argv[i + 1];
    else if (k == "--repeat") repeat = std::max(1, std::atoi(argv[i + 1]));
    else if (k == "--seed") seed = static_cast<uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
    else {
      std::fprintf(stderr, "usage: test_matmul [--controller HOST[:PORT]] [--sizes LIST] [--repeat N] [--seed S]\n");
      return 64;
    }
  }
  std::vector<Shape> shapes;
  if (!parseSizes(sizes, &shapes)) {
    std::fprintf(stderr, "bad --sizes '%s'\n", sizes.c_str());
    return 64;
  }
  if (clInit(controller) != clSuccess) {
    std::fprintf(stderr, "cannot connect to the controller\n");
    return 2;
  }
  clDeviceProp prop{};
  if (clGetDeviceProperties(&prop) != clSuccess || prop.workerCount == 0) {
    std::fprintf(stderr, "cluster has no workers\n");
    return 2;
  }
  std::printf("%s: %u workers, %u cores, %.0f MiB global memory\n", prop.name, prop.workerCount, prop.totalCores,
              prop.totalGlobalMem / 1048576.0);

  int failures = 0;
  for (size_t i = 0; i < shapes.size(); ++i) {
    failures += !runCase<float>(shapes[i], 1.0f, 0.0f, repeat, seed + uint32_t(i));
    failures += !runCase<float>(shapes[i], 0.5f, 2.0f, 1, seed + 100 + uint32_t(i));
    if (i == 0) failures += !runCase<double>(shapes[i], 1.0, 1.0, 1, seed + 200);
  }
  clShutdown();
  std::printf("%s (%d failing case%s)\n", failures == 0 ? "PASS" : "FAIL", failures, failures == 1 ? "" : "s");
  return failures == 0 ? 0 : 1;
}
