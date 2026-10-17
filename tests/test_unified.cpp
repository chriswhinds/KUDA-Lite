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

// Integration test for the unified memory view (built with -DKUDALITE_UNIFIED_MEMORY=ON).
//   cl-test-unified [--controller HOST[:PORT]]
// Exit code 0 = all checks passed.
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "kudalite/clblas.h"
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

#define CHECK_OK(expr)                                                                                \
  do {                                                                                                \
    const clError_t e_ = (expr);                                                                      \
    if (e_ != clSuccess) {                                                                            \
      std::fprintf(stderr, "  %s:%d: %s -> %s\n", __FILE__, __LINE__, #expr, clGetErrorString(e_)); \
      ++gFailures;                                                                                    \
    }                                                                                                 \
  } while (0)

void section(const char* name) { std::printf("-- %s\n", name); }

uint8_t pattern(size_t i, uint32_t seed) { return static_cast<uint8_t>((i * 2654435761u) >> 13 ^ seed); }

std::vector<uint64_t> workerUsage() {
  uint32_t n = 0;
  clGetWorkerInfo(nullptr, 0, &n);
  std::vector<clWorkerInfo> w(n);
  clGetWorkerInfo(w.data(), n, &n);
  std::vector<uint64_t> used;
  for (uint32_t i = 0; i < n && i < w.size(); ++i) used.push_back(w[i].memUsed);
  return used;
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

  section("open a 48 MiB window spanning every worker");
  const std::vector<uint64_t> before = workerUsage();
  clUnifiedView_t view = nullptr;
  const clError_t open = clUnifiedOpen(&view, 48u << 20, 0);
  if (open == clErrorNotSupported) {
    std::printf("unified memory not supported by this build/system; skipping\n");
    return 0;
  }
  CHECK_OK(open);
  if (open != clSuccess) return 1;
  clUnifiedInfo info{};
  CHECK_OK(clUnifiedGetInfo(view, &info));
  auto* base = static_cast<uint8_t*>(info.base);
  const size_t bytes = info.bytes, chunk = info.chunkBytes;
  std::printf("   window %p, %zu MiB, chunk %zu KiB, %u workers, write tracking %s\n", info.base, bytes >> 20,
              chunk >> 10, info.workers, info.writeTracking ? "on" : "off");
  CHECK(bytes >= (48u << 20) && bytes % chunk == 0 && info.residentBytes == 0);
  const std::vector<uint64_t> after = workerUsage();
  CHECK(after.size() == before.size());
  for (size_t i = 0; i < after.size() && i < before.size(); ++i) CHECK(after[i] > before[i]);  // a slice on every worker

  section("write through the pointer, sync, read back through the device");
  for (size_t i = 0; i < bytes; ++i) base[i] = pattern(i, 1);
  CHECK_OK(clUnifiedGetInfo(view, &info));
  CHECK(info.residentBytes == bytes && info.faults == bytes / chunk);
  CHECK_OK(clUnifiedSync(view));
  std::vector<uint8_t> host(bytes);
  CHECK_OK(clMemcpyDtoH(host.data(), info.devPtr, bytes));
  bool ok = true;
  for (size_t i = 0; i < bytes && ok; ++i) ok = host[i] == pattern(i, 1);
  CHECK(ok);

  section("only modified chunks are written back");
  CHECK_OK(clUnifiedGetInfo(view, &info));
  const uint64_t written = info.bytesWrittenBack;
  base[3 * chunk + 17] ^= 0xFF;
  base[9 * chunk] ^= 0xFF;
  CHECK_OK(clUnifiedSync(view));
  CHECK_OK(clUnifiedGetInfo(view, &info));
  if (info.writeTracking) {
    CHECK(info.bytesWrittenBack - written == 2 * chunk);
  } else {
    CHECK(info.bytesWrittenBack - written == bytes);  // fallback: every resident chunk
  }
  base[3 * chunk + 17] ^= 0xFF;
  base[9 * chunk] ^= 0xFF;
  CHECK_OK(clUnifiedSync(view));

  section("device writes become visible after invalidate");
  std::vector<uint8_t> fresh(bytes);
  for (size_t i = 0; i < bytes; ++i) fresh[i] = pattern(i, 7);
  CHECK_OK(clMemcpyHtoD(info.devPtr, fresh.data(), bytes));
  CHECK_OK(clUnifiedInvalidate(view));
  CHECK_OK(clUnifiedGetInfo(view, &info));
  CHECK(info.residentBytes == 0);
  CHECK(std::memcmp(base, fresh.data(), bytes) == 0);

  section("kernels and pointers share the memory (saxpy)");
  const size_t n = (bytes / 2) / sizeof(float);
  auto* x = reinterpret_cast<float*>(base);
  auto* y = reinterpret_cast<float*>(base + n * sizeof(float));
  for (size_t i = 0; i < n; ++i) {
    x[i] = static_cast<float>(i % 1000);
    y[i] = 1.0f;
  }
  CHECK_OK(clUnifiedSync(view));
  CHECK_OK(clblasSaxpy(nullptr, n, 2.0f, info.devPtr, info.devPtr + n * sizeof(float)));
  CHECK_OK(clStreamSynchronize(nullptr));
  CHECK_OK(clUnifiedInvalidate(view));
  ok = true;
  for (size_t i = 0; i < n && ok; ++i) ok = y[i] == 2.0f * static_cast<float>(i % 1000) + 1.0f;
  CHECK(ok);

  section("concurrent threads touching different chunks");
  CHECK_OK(clUnifiedInvalidate(view));
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (size_t c = t; c < bytes / chunk; c += 4) {
        for (size_t i = c * chunk; i < (c + 1) * chunk; i += 64) base[i] = static_cast<uint8_t>(t + 1);
      }
    });
  }
  for (auto& th : threads) th.join();
  CHECK_OK(clUnifiedSync(view));
  CHECK_OK(clMemcpyDtoH(host.data(), info.devPtr, bytes));
  ok = true;
  for (size_t c = 0; c < bytes / chunk && ok; ++c) ok = host[c * chunk] == static_cast<uint8_t>(c % 4 + 1);
  CHECK(ok);

  section("system calls on prefetched memory");
  CHECK_OK(clUnifiedInvalidate(view));
  CHECK_OK(clUnifiedPrefetch(view, 5 * chunk, chunk));
  int fds[2];
  CHECK(pipe(fds) == 0);
  CHECK(write(fds[1], base + 5 * chunk, 4096) == 4096);  // kernel-mode read of the window
  std::vector<uint8_t> piped(4096);
  CHECK(read(fds[0], piped.data(), 4096) == 4096);
  CHECK(std::memcmp(piped.data(), base + 5 * chunk, 4096) == 0);
  close(fds[0]);
  close(fds[1]);

  section("device-to-host copies into the window (no deadlock)");
  clDevPtr other = 0;
  CHECK_OK(clMalloc(&other, 3 * chunk));
  std::vector<uint8_t> src(3 * chunk);
  for (size_t i = 0; i < src.size(); ++i) src[i] = pattern(i, 42);
  CHECK_OK(clMemcpyHtoD(other, src.data(), src.size()));
  CHECK_OK(clUnifiedInvalidate(view));
  (void)base[11 * chunk];  // chunk 11 present and clean (write-protected), 12-13 absent
  CHECK_OK(clMemcpyDtoH(base + 11 * chunk + 100, other, src.size() - 100));
  CHECK(std::memcmp(base + 11 * chunk + 100, src.data(), src.size() - 100) == 0);
  CHECK_OK(clUnifiedSync(view));
  std::vector<uint8_t> check(src.size() - 100);
  CHECK_OK(clMemcpyDtoH(check.data(), info.devPtr + 11 * chunk + 100, check.size()));
  CHECK(std::memcmp(check.data(), src.data(), check.size()) == 0);
  CHECK_OK(clFree(other));

  section("close releases the cluster memory");
  CHECK_OK(clUnifiedClose(view));
  const std::vector<uint64_t> closed = workerUsage();
  CHECK(closed == before);

  section("bytes = 0: the largest window the cluster can provide");
  clUnifiedView_t big = nullptr;
  CHECK_OK(clUnifiedOpen(&big, 0, 0));
  if (big != nullptr) {
    clUnifiedInfo bi{};
    CHECK_OK(clUnifiedGetInfo(big, &bi));
    uint64_t total = 0;
    uint32_t nw = 0;
    clGetWorkerInfo(nullptr, 0, &nw);
    std::vector<clWorkerInfo> w(nw);
    clGetWorkerInfo(w.data(), nw, &nw);
    for (const auto& wi : w) total += wi.memBytes;
    std::printf("   %zu MiB of %llu MiB cluster memory as one window\n", bi.bytes >> 20,
                static_cast<unsigned long long>(total >> 20));
    CHECK(bi.bytes >= total / 100 * 80);
    auto* b = static_cast<uint8_t*>(bi.base);
    b[0] = 1;
    b[bi.bytes - 1] = 2;  // first and last byte: first and last worker
    CHECK_OK(clUnifiedSync(big));
    uint8_t first = 0, last = 0;
    CHECK_OK(clMemcpyDtoH(&first, bi.devPtr, 1));
    CHECK_OK(clMemcpyDtoH(&last, bi.devPtr + bi.bytes - 1, 1));
    CHECK(first == 1 && last == 2);
    CHECK_OK(clUnifiedClose(big));
  }

  CHECK(clUnifiedSync(nullptr) == clErrorInvalidResourceHandle);
  clShutdown();
  std::printf("%s (%d failure%s)\n", gFailures == 0 ? "PASS" : "FAIL", gFailures, gFailures == 1 ? "" : "s");
  return gFailures == 0 ? 0 : 1;
}
