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

// Unified memory view: one continuous address range in this process backed by cluster global
// memory that spans every worker's RAM (docs/UNIFIED_MEMORY.md).
//
// Mechanism (Linux userfaultfd, no kernel driver):
//  * the window is an anonymous mapping registered with userfaultfd;
//  * the first touch of a chunk raises a "missing" fault; a handler thread fetches that chunk
//    from the owning worker(s) through the normal host runtime and maps it in (UFFDIO_COPY);
//  * where the kernel supports write-protect faults, chunks are mapped read-only-tracked, so
//    the first write to a clean chunk raises a "write-protect" fault that marks it dirty;
//  * clUnifiedSync re-protects dirty chunks, then copies them back to the workers.
// The transport is the ordinary host runtime (clMemcpy*), so any data path KUDA-Lite gains later
// is used automatically.

#include "kudalite/kudalite.h"

#include "host/unified_internal.h"

#if KUDALITE_UNIFIED_MEMORY

#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// kDiscarded: absent, but zero-filled instead of fetched on the next touch (clUnifiedDiscard).
enum ChunkState : uint8_t { kAbsent = 0, kClean = 1, kDirty = 2, kDiscarded = 3 };
constexpr size_t kDefaultChunk = 256 * 1024;

}  // namespace

struct clUnifiedView_st {
  uint8_t* base = nullptr;
  size_t bytes = 0;
  size_t chunk = 0;
  size_t chunks = 0;
  clDevPtr dev = 0;
  uint32_t workers = 0;
  int uffd = -1;
  int wake[2] = {-1, -1};
  bool writeTracking = false;
  clStream_t fetchStream = nullptr;  // used under popMu only
  clStream_t syncStream = nullptr;   // used under syncMu only
  std::thread handler;

  std::mutex mu;  // guards state
  std::vector<uint8_t> state;
  std::mutex popMu;  // serialises fetch + map, shared by the handler and clUnifiedPrefetch
  std::mutex syncMu;
  std::vector<uint8_t> staging;  // under popMu

  std::atomic<uint64_t> faults{0};
  std::atomic<uint64_t> writeFaults{0};
  std::atomic<uint64_t> bytesFetched{0};
  std::atomic<uint64_t> bytesWritten{0};
  std::atomic<uint64_t> bytesDiscarded{0};
  std::atomic<int> error{clSuccess};  // first fetch failure, reported by the next sync
};

namespace {

// Open views, so the runtime can find out whether a copy destination lies inside one.
std::mutex gViewsMu;
std::vector<clUnifiedView_st*> gViews;

void recordError(clUnifiedView_st* v, clError_t e) {
  int expected = clSuccess;
  if (v->error.compare_exchange_strong(expected, e)) {
    std::fprintf(stderr, "kudalite: unified memory fetch failed (%s); the chunk was zero-filled\n",
                 clGetErrorString(e));
  }
}

void wakeRange(clUnifiedView_st* v, size_t idx) {
  uffdio_range r{reinterpret_cast<uint64_t>(v->base + idx * v->chunk), v->chunk};
  ioctl(v->uffd, UFFDIO_WAKE, &r);
}

void writeProtect(clUnifiedView_st* v, size_t idx, bool protect) {
  uffdio_writeprotect wp{};
  wp.range.start = reinterpret_cast<uint64_t>(v->base + idx * v->chunk);
  wp.range.len = v->chunk;
  wp.mode = protect ? UFFDIO_WRITEPROTECT_MODE_WP : 0;  // un-protecting also wakes the writer
  while (ioctl(v->uffd, UFFDIO_WRITEPROTECT, &wp) != 0 && errno == EAGAIN) {
  }
}

/// Maps the fetched chunks `idxs` (consecutive in `src`); `dirty[k]` maps chunk k writable and
/// dirty. Caller holds popMu.
void mapChunks(clUnifiedView_st* v, const std::vector<size_t>& idxs, const uint8_t* src,
               const std::vector<bool>& dirtyFlags) {
  for (size_t k = 0; k < idxs.size(); ++k) {
    const size_t idx = idxs[k];
    const bool dirty = dirtyFlags[k];
    uffdio_copy c{};
    c.dst = reinterpret_cast<uint64_t>(v->base + idx * v->chunk);
    c.src = reinterpret_cast<uint64_t>(src + k * v->chunk);
    c.len = v->chunk;
    // Read faults map the chunk write-protected so its first write is noticed; write faults are
    // mapped writable and marked dirty straight away (saves a second fault).
    c.mode = (v->writeTracking && !dirty) ? UFFDIO_COPY_MODE_WP : 0;
    for (;;) {
      if (ioctl(v->uffd, UFFDIO_COPY, &c) == 0) break;
      if (errno == EAGAIN) continue;
      if (errno == EEXIST) wakeRange(v, idx);  // raced with another mapping: just wake
      break;
    }
    std::lock_guard<std::mutex> lk(v->mu);
    v->state[idx] = dirty ? kDirty : kClean;
  }
}

/// Fetches and maps the absent chunks among `idxs` (in one batch of transfers). Discarded chunks
/// are zero-filled instead of fetched, and always mapped dirty so the next sync writes them back.
void populate(clUnifiedView_st* v, std::vector<size_t> idxs, bool dirty) {
  std::lock_guard<std::mutex> pop(v->popMu);
  std::vector<bool> discarded;
  {
    std::lock_guard<std::mutex> lk(v->mu);
    idxs.erase(std::remove_if(idxs.begin(), idxs.end(),
                              [&](size_t i) { return v->state[i] != kAbsent && v->state[i] != kDiscarded; }),
               idxs.end());
    for (size_t i : idxs) discarded.push_back(v->state[i] == kDiscarded);
  }
  if (idxs.empty()) return;
  v->staging.resize(idxs.size() * v->chunk);
  size_t fetched = 0;
  for (size_t k = 0; k < idxs.size(); ++k) {
    uint8_t* dst = v->staging.data() + k * v->chunk;
    if (discarded[k]) {
      std::memset(dst, 0, v->chunk);
      continue;
    }
    clMemcpyDtoHAsync(dst, v->dev + idxs[k] * v->chunk, v->chunk, v->fetchStream);
    ++fetched;
  }
  if (fetched > 0) {
    const clError_t st = clStreamSynchronize(v->fetchStream);
    if (st != clSuccess) {
      recordError(v, st);
      std::memset(v->staging.data(), 0, v->staging.size());
    } else {
      v->bytesFetched += fetched * v->chunk;
    }
  }
  v->bytesDiscarded += (idxs.size() - fetched) * v->chunk;
  std::vector<bool> dirtyFlags(idxs.size());
  for (size_t k = 0; k < idxs.size(); ++k) dirtyFlags[k] = dirty || discarded[k];
  mapChunks(v, idxs, v->staging.data(), dirtyFlags);
}

void handlerLoop(clUnifiedView_st* v) {
  pollfd fds[2] = {{v->uffd, POLLIN, 0}, {v->wake[0], POLLIN, 0}};
  for (;;) {
    if (poll(fds, 2, -1) < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (fds[1].revents != 0) return;  // clUnifiedClose
    if ((fds[0].revents & POLLIN) == 0) continue;
    uffd_msg msg{};
    const ssize_t n = read(v->uffd, &msg, sizeof msg);
    if (n != static_cast<ssize_t>(sizeof msg)) continue;  // EAGAIN: another wake-up raced us
    if (msg.event != UFFD_EVENT_PAGEFAULT) continue;
    const uint64_t addr = msg.arg.pagefault.address;
    const size_t idx = static_cast<size_t>((addr - reinterpret_cast<uint64_t>(v->base)) / v->chunk);
    if (idx >= v->chunks) continue;
    if (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP) {
      ++v->writeFaults;
      {
        std::lock_guard<std::mutex> lk(v->mu);
        // A discard may have dropped the chunk since the fault was queued; the writer then
        // retries and takes a missing fault instead.
        if (v->state[idx] == kClean) v->state[idx] = kDirty;
      }
      writeProtect(v, idx, false);
      continue;
    }
    ++v->faults;
    const bool write = (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE) != 0;
    populate(v, {idx}, write);
    wakeRange(v, idx);  // no-op if UFFDIO_COPY already woke the faulting thread
  }
}

/// userfaultfd with the user-mode-only flag works without privileges on kernels >= 5.11 even
/// when vm.unprivileged_userfaultfd = 0 (the usual default).
int openUffd() {
  int fd = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
  if (fd < 0 && errno == EINVAL) fd = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK));
  return fd;
}

void destroy(clUnifiedView_st* v) {
  {
    std::lock_guard<std::mutex> lk(gViewsMu);
    gViews.erase(std::remove(gViews.begin(), gViews.end(), v), gViews.end());
  }
  if (v->handler.joinable()) {
    const char b = 1;
    if (write(v->wake[1], &b, 1) < 0) {
    }
    v->handler.join();
  }
  if (v->base != nullptr) {
    if (v->uffd >= 0) {
      uffdio_range r{reinterpret_cast<uint64_t>(v->base), v->bytes};
      ioctl(v->uffd, UFFDIO_UNREGISTER, &r);
    }
    munmap(v->base, v->bytes);
  }
  if (v->uffd >= 0) close(v->uffd);
  for (int fd : v->wake) {
    if (fd >= 0) close(fd);
  }
  if (v->fetchStream) clStreamDestroy(v->fetchStream);
  if (v->syncStream) clStreamDestroy(v->syncStream);
  if (v->dev != 0) clFree(v->dev);
  delete v;
}

}  // namespace

clError_t clUnifiedOpen(clUnifiedView_t* out, size_t bytes, size_t chunkBytes) {
  if (out == nullptr) return clErrorInvalidValue;
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const size_t chunk = chunkBytes != 0 ? chunkBytes : std::max(kDefaultChunk, page);
  if (chunk < page || chunk % page != 0) return clErrorInvalidValue;

  // Size: explicit, or as much as every worker can give (a blocked allocation needs the same
  // slice on each worker, so the smallest free arena decides).
  uint32_t n = 0;
  clError_t st = clGetWorkerInfo(nullptr, 0, &n);
  if (st != clSuccess) return st;
  if (n == 0) return clErrorNoWorkers;
  std::vector<clWorkerInfo> workers(n);
  if ((st = clGetWorkerInfo(workers.data(), n, &n)) != clSuccess) return st;
  const bool autoSize = bytes == 0;
  if (autoSize) {
    uint64_t minFree = UINT64_MAX;
    for (uint32_t i = 0; i < n && i < workers.size(); ++i) {
      minFree = std::min<uint64_t>(minFree, workers[i].memBytes - workers[i].memUsed);
    }
    bytes = static_cast<size_t>(minFree / 100 * 95 / chunk * chunk) * n;
  } else {
    bytes = (bytes + chunk - 1) / chunk * chunk;
  }
  if (bytes == 0) return clErrorMemoryAllocation;

  auto* v = new clUnifiedView_st();
  v->chunk = chunk;
  v->workers = n;
  for (int attempt = 0;; ++attempt) {
    st = clMallocEx(&v->dev, bytes, clDistBlocked, 0);
    if (st == clSuccess) break;
    if (!autoSize || st != clErrorMemoryAllocation || attempt == 4) {
      delete v;
      return st;
    }
    bytes = (bytes / 10 * 9) / (chunk * n) * (chunk * n);  // fragmentation: try 10 % smaller
    if (bytes == 0) {
      delete v;
      return clErrorMemoryAllocation;
    }
  }
  v->bytes = bytes;
  v->chunks = bytes / chunk;
  v->state.assign(v->chunks, kAbsent);

  auto fail = [&](clError_t e, const char* what) {
    std::fprintf(stderr, "kudalite: unified memory: %s: %s\n", what, std::strerror(errno));
    destroy(v);
    return e;
  };

  v->uffd = openUffd();
  if (v->uffd < 0) {
    return fail(clErrorNotSupported,
                "userfaultfd unavailable (needs Linux >= 5.11, or vm.unprivileged_userfaultfd=1)");
  }
  uffdio_api api{};
  api.api = UFFD_API;
  api.features = UFFD_FEATURE_PAGEFAULT_FLAG_WP;
  if (ioctl(v->uffd, UFFDIO_API, &api) != 0) {  // no write-protect support: write back all chunks
    close(v->uffd);
    v->uffd = openUffd();
    api = uffdio_api{};
    api.api = UFFD_API;
    if (v->uffd < 0 || ioctl(v->uffd, UFFDIO_API, &api) != 0) return fail(clErrorNotSupported, "UFFDIO_API");
  } else {
    v->writeTracking = true;
  }

  void* mem = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (mem == MAP_FAILED) {
    v->base = nullptr;
    return fail(clErrorMemoryAllocation, "mmap");
  }
  v->base = static_cast<uint8_t*>(mem);
  madvise(v->base, bytes, MADV_NOHUGEPAGE);  // the chunk, not a huge page, is the unit

  uffdio_register reg{};
  reg.range = {reinterpret_cast<uint64_t>(v->base), bytes};
  reg.mode = UFFDIO_REGISTER_MODE_MISSING | (v->writeTracking ? UFFDIO_REGISTER_MODE_WP : 0);
  if (ioctl(v->uffd, UFFDIO_REGISTER, &reg) != 0) {
    if (!v->writeTracking) return fail(clErrorNotSupported, "UFFDIO_REGISTER");
    v->writeTracking = false;  // e.g. a kernel without write-protect for anonymous memory
    reg.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (ioctl(v->uffd, UFFDIO_REGISTER, &reg) != 0) return fail(clErrorNotSupported, "UFFDIO_REGISTER");
  }

  if (pipe2(v->wake, O_CLOEXEC) != 0) return fail(clErrorUnknown, "pipe2");
  if (clStreamCreate(&v->fetchStream) != clSuccess || clStreamCreate(&v->syncStream) != clSuccess) {
    return fail(clErrorNetwork, "stream");
  }
  v->handler = std::thread(handlerLoop, v);
  {
    std::lock_guard<std::mutex> lk(gViewsMu);
    gViews.push_back(v);
  }
  *out = v;
  return clSuccess;
}

clError_t clUnifiedGetInfo(clUnifiedView_t v, clUnifiedInfo* info) {
  if (v == nullptr) return clErrorInvalidResourceHandle;
  if (info == nullptr) return clErrorInvalidValue;
  *info = clUnifiedInfo{};
  info->base = v->base;
  info->devPtr = v->dev;
  info->bytes = v->bytes;
  info->chunkBytes = v->chunk;
  info->workers = v->workers;
  info->writeTracking = v->writeTracking ? 1 : 0;
  {
    std::lock_guard<std::mutex> lk(v->mu);
    for (uint8_t s : v->state) {
      if (s == kClean || s == kDirty) info->residentBytes += v->chunk;
      if (s == kDirty) info->dirtyBytes += v->chunk;
    }
  }
  info->faults = v->faults.load();
  info->writeFaults = v->writeFaults.load();
  info->bytesFetched = v->bytesFetched.load();
  info->bytesWrittenBack = v->bytesWritten.load();
  info->bytesDiscarded = v->bytesDiscarded.load();
  return clSuccess;
}

clError_t clUnifiedSync(clUnifiedView_t v) {
  if (v == nullptr) return clErrorInvalidResourceHandle;
  std::lock_guard<std::mutex> sync(v->syncMu);
  std::vector<size_t> idxs;
  {
    std::lock_guard<std::mutex> lk(v->mu);
    for (size_t i = 0; i < v->chunks; ++i) {
      if (v->writeTracking ? v->state[i] == kDirty : (v->state[i] == kClean || v->state[i] == kDirty)) {
        idxs.push_back(i);
        v->state[i] = kClean;
      }
    }
  }
  // Protect first, copy second: a write that lands during the copy faults again and is
  // picked up by the next sync instead of being lost.
  if (v->writeTracking) {
    for (size_t i : idxs) writeProtect(v, i, true);
  }
  for (size_t i : idxs) {
    clMemcpyHtoDAsync(v->dev + i * v->chunk, v->base + i * v->chunk, v->chunk, v->syncStream);
  }
  clError_t st = clStreamSynchronize(v->syncStream);
  if (st == clSuccess) v->bytesWritten += idxs.size() * v->chunk;
  const clError_t fetchErr = static_cast<clError_t>(v->error.exchange(clSuccess));
  return st != clSuccess ? st : fetchErr;
}

clError_t clUnifiedInvalidate(clUnifiedView_t v) {
  if (v == nullptr) return clErrorInvalidResourceHandle;
  const clError_t st = clUnifiedSync(v);
  std::lock_guard<std::mutex> pop(v->popMu);
  std::lock_guard<std::mutex> lk(v->mu);
  for (size_t i = 0; i < v->chunks; ++i) {
    if (v->state[i] == kClean || v->state[i] == kDirty) {
      madvise(v->base + i * v->chunk, v->chunk, MADV_DONTNEED);  // next access faults again
    }
    v->state[i] = kAbsent;  // also cancels a pending discard: the next touch fetches
  }
  return st;
}

clError_t clUnifiedPrefetch(clUnifiedView_t v, size_t offset, size_t bytes) {
  if (v == nullptr) return clErrorInvalidResourceHandle;
  if (offset > v->bytes || bytes > v->bytes - offset) return clErrorInvalidValue;
  if (bytes == 0) return clSuccess;
  std::vector<size_t> idxs;
  for (size_t i = offset / v->chunk; i <= (offset + bytes - 1) / v->chunk; ++i) idxs.push_back(i);
  constexpr size_t kBatch = 64;  // bounds the staging buffer
  for (size_t b = 0; b < idxs.size(); b += kBatch) {
    populate(v, std::vector<size_t>(idxs.begin() + b, idxs.begin() + std::min(idxs.size(), b + kBatch)), false);
  }
  return static_cast<clError_t>(v->error.load());
}

clError_t clUnifiedDiscard(clUnifiedView_t v, size_t offset, size_t bytes) {
  if (v == nullptr) return clErrorInvalidResourceHandle;
  if (offset > v->bytes || bytes > v->bytes - offset) return clErrorInvalidValue;
  const size_t first = (offset + v->chunk - 1) / v->chunk;  // whole chunks only: round inward
  const size_t last = (offset + bytes) / v->chunk;         // exclusive
  std::lock_guard<std::mutex> sync(v->syncMu);  // not while a sync is copying these chunks out
  std::lock_guard<std::mutex> pop(v->popMu);
  std::lock_guard<std::mutex> lk(v->mu);
  for (size_t i = first; i < last; ++i) {
    if (v->state[i] == kClean || v->state[i] == kDirty) {
      madvise(v->base + i * v->chunk, v->chunk, MADV_DONTNEED);  // next access faults again
    }
    v->state[i] = kDiscarded;
  }
  return clSuccess;
}

clError_t clUnifiedClose(clUnifiedView_t v) {
  if (v == nullptr) return clErrorInvalidResourceHandle;
  const clError_t st = clUnifiedSync(v);
  destroy(v);
  return st;
}

void kudalite::detail::unifiedPrepareHostWrite(void* dst, size_t bytes) {
  if (bytes == 0) return;
  const auto* p = static_cast<const uint8_t*>(dst);
  std::vector<std::pair<clUnifiedView_st*, std::vector<size_t>>> work;
  {
    std::lock_guard<std::mutex> lk(gViewsMu);
    for (clUnifiedView_st* v : gViews) {
      if (p + bytes <= v->base || p >= v->base + v->bytes) continue;
      const size_t lo = static_cast<size_t>(std::max(p, static_cast<const uint8_t*>(v->base)) - v->base);
      const size_t hi = static_cast<size_t>(std::min(p + bytes, static_cast<const uint8_t*>(v->base) + v->bytes) - v->base);
      std::vector<size_t> idxs;
      for (size_t i = lo / v->chunk; i <= (hi - 1) / v->chunk; ++i) idxs.push_back(i);
      work.emplace_back(v, std::move(idxs));
    }
  }
  // Afterwards every covered chunk is present, writable and dirty, so the network thread never
  // faults while writing the data in. It must not fault even on write protection: the single
  // fault-handler thread may itself be waiting on that network thread for a fetch.
  for (auto& [v, idxs] : work) {
    populate(v, idxs, true);  // absent chunks: fetched, mapped writable, dirty
    if (!v->writeTracking) continue;
    std::vector<size_t> unprotect;
    {
      std::lock_guard<std::mutex> lk(v->mu);
      for (size_t i : idxs) {
        if (v->state[i] == kClean) {
          v->state[i] = kDirty;
          unprotect.push_back(i);
        }
      }
    }
    for (size_t i : unprotect) writeProtect(v, i, false);
  }
}

#else  // built without KUDALITE_UNIFIED_MEMORY

void kudalite::detail::unifiedPrepareHostWrite(void*, size_t) {}

clError_t clUnifiedOpen(clUnifiedView_t*, size_t, size_t) { return clErrorNotSupported; }
clError_t clUnifiedGetInfo(clUnifiedView_t, clUnifiedInfo*) { return clErrorNotSupported; }
clError_t clUnifiedSync(clUnifiedView_t) { return clErrorNotSupported; }
clError_t clUnifiedInvalidate(clUnifiedView_t) { return clErrorNotSupported; }
clError_t clUnifiedPrefetch(clUnifiedView_t, size_t, size_t) { return clErrorNotSupported; }
clError_t clUnifiedDiscard(clUnifiedView_t, size_t, size_t) { return clErrorNotSupported; }
clError_t clUnifiedClose(clUnifiedView_t) { return clErrorNotSupported; }

#endif
