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

// Unified memory view vs. explicit copies: the same host-side work done both ways.
//   cl-bench-unified [--controller HOST[:PORT]] [--mib N] [--chunk-kib N] [--runs N]
// Prints time and bytes moved for each task. Results are discussed in docs/UNIFIED_MEMORY.md §5.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "kudalite/clblas.h"
#include "kudalite/kudalite.h"

namespace {

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Stat {
  double seconds;
  uint64_t moved;
};

void row(const char* name, Stat e, Stat u) {
  std::printf("%-40s explicit %8.1f ms %8.1f MiB | unified %8.1f ms %8.1f MiB | %5.2fx\n", name, e.seconds * 1e3,
              static_cast<double>(e.moved) / 1048576.0, u.seconds * 1e3, static_cast<double>(u.moved) / 1048576.0,
              u.seconds / e.seconds);
}

uint64_t moved(clUnifiedView_t v) {
  clUnifiedInfo i{};
  clUnifiedGetInfo(v, &i);
  return i.bytesFetched + i.bytesWrittenBack;
}

}  // namespace

int main(int argc, char** argv) {
  const char* controller = nullptr;
  size_t mib = 192, chunkKib = 256;
  int runs = 2;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--controller") == 0) controller = argv[i + 1];
    if (std::strcmp(argv[i], "--mib") == 0) mib = std::strtoull(argv[i + 1], nullptr, 10);
    if (std::strcmp(argv[i], "--chunk-kib") == 0) chunkKib = std::strtoull(argv[i + 1], nullptr, 10);
    if (std::strcmp(argv[i], "--runs") == 0) runs = std::atoi(argv[i + 1]);
  }
  if (clInit(controller) != clSuccess) {
    std::fprintf(stderr, "cannot connect to the controller\n");
    return 2;
  }
  const size_t bytes = mib << 20;
  clUnifiedView_t v = nullptr;
  const clError_t st = clUnifiedOpen(&v, bytes, chunkKib << 10);
  if (st != clSuccess) {
    std::fprintf(stderr, "clUnifiedOpen: %s\n", clGetErrorString(st));
    return 1;
  }
  clUnifiedInfo info{};
  clUnifiedGetInfo(v, &info);
  auto* p = static_cast<float*>(info.base);
  const size_t n = bytes / sizeof(float);
  clDevPtr d = 0;
  if (clMallocEx(&d, bytes, clDistBlocked, 0) != clSuccess) {  // same layout as the window
    std::fprintf(stderr, "not enough cluster memory for two %zu MiB buffers\n", mib);
    return 1;
  }
  std::vector<float> h(n);
  std::printf("%zu MiB, chunk %zu KiB, %u workers, write tracking %s\n", mib, info.chunkBytes >> 10, info.workers,
              info.writeTracking ? "on" : "off");
  int mismatches = 0;
  for (int r = 0; r < runs; ++r) {
    std::printf("-- run %d\n", r + 1);
    // 1. Fill a whole array and push it to the workers.
    double t = now();
    for (size_t i = 0; i < n; ++i) h[i] = static_cast<float>(i);
    clMemcpyHtoD(d, h.data(), bytes);
    const Stat fillE{now() - t, bytes};
    clUnifiedInvalidate(v);
    uint64_t m0 = moved(v);
    t = now();
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<float>(i);
    clUnifiedSync(v);
    row("1. write whole array, push", fillE, {now() - t, moved(v) - m0});
    clUnifiedInvalidate(v);
    m0 = moved(v);
    t = now();
    clUnifiedDiscard(v, 0, bytes);
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<float>(i);
    clUnifiedSync(v);
    row("1b. same, clUnifiedDiscard first", fillE, {now() - t, moved(v) - m0});

    // 2. A kernel: identical work on worker memory either way.
    t = now();
    clblasSaxpy(nullptr, n / 2, 2.f, d, d + bytes / 2);
    clStreamSynchronize(nullptr);
    const Stat kE{now() - t, 0};
    t = now();
    clblasSaxpy(nullptr, n / 2, 2.f, info.devPtr, info.devPtr + bytes / 2);
    clStreamSynchronize(nullptr);
    row("2. saxpy kernel on the workers", kE, {now() - t, 0});

    // 3. Read the whole result array.
    double sumE = 0;
    t = now();
    clMemcpyDtoH(h.data(), d, bytes);
    for (size_t i = 0; i < n; ++i) sumE += h[i];
    const Stat readE{now() - t, bytes};
    for (int prefetch = 0; prefetch < 2; ++prefetch) {
      clUnifiedInvalidate(v);
      m0 = moved(v);
      double sumU = 0;
      t = now();
      if (prefetch) clUnifiedPrefetch(v, 0, bytes);
      for (size_t i = 0; i < n; ++i) sumU += p[i];
      row(prefetch ? "3b. read whole array, prefetch first" : "3. read whole array, on demand", readE,
          {now() - t, moved(v) - m0});
      if (sumU != sumE) ++mismatches;
    }

    // 4. Read 2000 values at random places.
    std::mt19937_64 rng(1);
    std::vector<size_t> idx(2000);
    for (auto& x : idx) x = rng() % n;
    float f = 0;
    t = now();
    for (size_t x : idx) clMemcpyDtoH(&f, d + x * sizeof(float), sizeof(float));
    const Stat randE{now() - t, idx.size() * sizeof(float)};
    clUnifiedInvalidate(v);
    m0 = moved(v);
    volatile float g = 0;
    t = now();
    for (size_t x : idx) g = g + p[x];
    row("4. read 2000 random values", randE, {now() - t, moved(v) - m0});

    // 5. Change 100 scattered values and push the change.
    clUnifiedPrefetch(v, 0, bytes);
    std::vector<size_t> upd;
    for (size_t i = 0; i < n; i += n / 100) upd.push_back(i);
    t = now();
    for (size_t x : upd) h[x] += 1;
    clMemcpyHtoD(d, h.data(), bytes);
    const Stat updE{now() - t, bytes};
    m0 = moved(v);
    t = now();
    for (size_t x : upd) p[x] += 1;
    clUnifiedSync(v);
    const Stat updU{now() - t, moved(v) - m0};
    row("5. change 100 scattered values, push", updE, updU);
    t = now();
    for (size_t x : upd) {
      h[x] += 1;
      clMemcpyHtoD(d + x * sizeof(float), &h[x], sizeof(float));
    }
    row("5b. ...vs. 100 tiny explicit copies", {now() - t, upd.size() * sizeof(float)}, updU);
  }
  clFree(d);
  clUnifiedClose(v);
  clShutdown();
  if (mismatches != 0) std::printf("WARNING: %d read mismatches\n", mismatches);
  return mismatches == 0 ? 0 : 1;
}
