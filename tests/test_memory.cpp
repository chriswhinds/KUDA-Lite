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

// Integration test: global memory, streams and events against a running cluster.
//   test_memory [--controller HOST[:PORT]]
// Exit code 0 = all checks passed.
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "kudalite/kudalite.h"

namespace {

int gFailures = 0;

#define CHECK(cond)                                                                   \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::fprintf(stderr, "  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++gFailures;                                                                    \
    }                                                                                 \
  } while (0)

#define CHECK_OK(expr)                                                                                  \
  do {                                                                                                  \
    const clError_t e_ = (expr);                                                                        \
    if (e_ != clSuccess) {                                                                              \
      std::fprintf(stderr, "  %s:%d: %s -> %s\n", __FILE__, __LINE__, #expr, clGetErrorString(e_));   \
      ++gFailures;                                                                                      \
    }                                                                                                   \
  } while (0)

std::vector<uint8_t> randomBytes(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<uint8_t> v(n);
  for (auto& b : v) b = static_cast<uint8_t>(rng());
  return v;
}

void section(const char* name) { std::printf("-- %s\n", name); }

void testRoundTrip(clDistribution dist, uint32_t pageSize, size_t bytes) {
  clDevPtr d = 0;
  CHECK_OK(clMallocEx(&d, bytes, dist, pageSize));
  const auto src = randomBytes(bytes, static_cast<uint32_t>(bytes));
  std::vector<uint8_t> back(bytes, 0);
  CHECK_OK(clMemcpyHtoD(d, src.data(), bytes));
  CHECK_OK(clMemcpyDtoH(back.data(), d, bytes));
  CHECK(back == src);

  // Unaligned sub-range that crosses several page (and therefore worker) boundaries.
  const size_t off = 12345, len = std::min<size_t>(bytes - off, 300000);
  std::vector<uint8_t> part(len);
  CHECK_OK(clMemcpyDtoH(part.data(), d + off, len));
  CHECK(std::memcmp(part.data(), src.data() + off, len) == 0);
  CHECK_OK(clFree(d));
}

}  // namespace

int main(int argc, char** argv) {
  const char* controller = nullptr;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--controller") == 0) controller = argv[i + 1];
  }
  if (clInit(controller) != clSuccess) {
    std::fprintf(stderr, "cannot connect to the controller\n");
    return 2;
  }

  section("device properties");
  clDeviceProp prop{};
  CHECK_OK(clGetDeviceProperties(&prop));
  std::printf("   %s: %u workers, %u cores, %.1f MiB global memory (%.1f MiB free)\n", prop.name, prop.workerCount,
              prop.totalCores, prop.totalGlobalMem / 1048576.0, prop.freeGlobalMem / 1048576.0);
  CHECK(prop.workerCount > 0);

  section("striped round trip (default 64 KiB pages)");
  testRoundTrip(clDistStriped, 0, 10u << 20);
  section("striped round trip (4 KiB pages, odd size)");
  testRoundTrip(clDistStriped, 4096, (3u << 20) + 77);
  section("blocked round trip");
  testRoundTrip(clDistBlocked, 0, (5u << 20) + 3);

  section("memset and device-to-device copy");
  {
    const size_t n = (2u << 20) + 5;
    clDevPtr a = 0, b = 0;
    CHECK_OK(clMalloc(&a, n));
    CHECK_OK(clMalloc(&b, n));
    CHECK_OK(clMemset(a, 0x5A, n));
    CHECK_OK(clMemset(a + 1000, 0x01, 70000));
    CHECK_OK(clMemcpyDtoD(b, a, n));
    std::vector<uint8_t> host(n);
    CHECK_OK(clMemcpyDtoH(host.data(), b, n));
    bool ok = true;
    for (size_t i = 0; i < n; ++i) {
      const uint8_t expect = (i >= 1000 && i < 71000) ? 0x01 : 0x5A;
      if (host[i] != expect) ok = false;
    }
    CHECK(ok);
    CHECK_OK(clFree(a));
    CHECK_OK(clFree(b));
  }

  section("streams and events");
  {
    const size_t n = 8u << 20;
    clStream_t s1 = nullptr, s2 = nullptr;
    clEvent_t start = nullptr, stop = nullptr, copied = nullptr;
    CHECK_OK(clStreamCreate(&s1));
    CHECK_OK(clStreamCreate(&s2));
    CHECK_OK(clEventCreate(&start));
    CHECK_OK(clEventCreate(&stop));
    CHECK_OK(clEventCreate(&copied));
    clDevPtr a = 0, b = 0;
    CHECK_OK(clMalloc(&a, n));
    CHECK_OK(clMalloc(&b, n));
    const auto src = randomBytes(n, 7);
    std::vector<uint8_t> back(n);

    CHECK_OK(clEventRecord(start, s1));
    CHECK_OK(clMemcpyHtoDAsync(a, src.data(), n, s1));
    CHECK_OK(clEventRecord(copied, s1));
    CHECK_OK(clStreamWaitEvent(s2, copied));  // s2 must not copy a -> b before s1 filled a
    CHECK_OK(clMemcpyDtoDAsync(b, a, n, s2));
    CHECK_OK(clMemcpyDtoHAsync(back.data(), b, n, s2));
    CHECK_OK(clEventRecord(stop, s2));
    CHECK_OK(clEventSynchronize(stop));
    CHECK_OK(clStreamSynchronize(s1));
    CHECK_OK(clStreamSynchronize(s2));
    CHECK(back == src);
    CHECK(clStreamQuery(s2) == clSuccess);
    float ms = -1;
    CHECK_OK(clEventElapsedTime(&ms, start, stop));
    CHECK(ms >= 0);
    std::printf("   8 MiB H2D + D2D + D2H across two streams: %.2f ms (controller clock)\n", ms);

    CHECK_OK(clFree(a));
    CHECK_OK(clFree(b));
    CHECK_OK(clEventDestroy(start));
    CHECK_OK(clEventDestroy(stop));
    CHECK_OK(clEventDestroy(copied));
    CHECK_OK(clStreamDestroy(s1));
    CHECK_OK(clStreamDestroy(s2));
  }

  section("error reporting");
  {
    clDevPtr d = 0;
    CHECK_OK(clMalloc(&d, 4096));
    uint8_t buf[64];
    // Past the end of the allocation: reported when the stream is synchronised.
    CHECK(clMemcpyDtoH(buf, d + 4090, sizeof buf) == clErrorInvalidDevicePointer);
    CHECK(clMemcpyDtoH(buf, 0x42, sizeof buf) == clErrorInvalidDevicePointer);
    CHECK(clStreamSynchronize(nullptr) == clSuccess);  // errors are cleared once reported
    CHECK(clLaunch("no_such_kernel", clDim3(1), clDim3(1), nullptr, d) == clSuccess);
    CHECK(clStreamSynchronize(nullptr) == clErrorKernelNotFound);
    CHECK(clFree(d) == clSuccess);
    CHECK(clFree(d) == clErrorInvalidDevicePointer);
    CHECK(clMallocEx(&d, 4096, clDistStriped, 100) == clErrorInvalidValue);  // not a multiple of 64
    clGetLastError();
  }

  CHECK_OK(clShutdown());
  std::printf("%s (%d failure%s)\n", gFailures == 0 ? "PASS" : "FAIL", gFailures, gFailures == 1 ? "" : "s");
  return gFailures == 0 ? 0 : 1;
}
