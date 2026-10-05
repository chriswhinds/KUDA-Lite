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

// clBlockContext implementation and the kernel registry.
#include <map>
#include <mutex>

#include "common/dsm_client.h"
#include "common/thread_pool.h"
#include "worker/exec_env.h"
#include "worker/page_cache.h"

namespace {

std::mutex& registryMutex() {
  static std::mutex mu;
  return mu;
}
std::map<std::string, clKernelFn>& registry() {
  static std::map<std::string, clKernelFn> kernels;
  return kernels;
}

void check(clError_t st) {
  if (st != clSuccess) throw clKernelFault(st);
}

}  // namespace

bool clRegisterKernel(const char* name, clKernelFn fn) {
  std::lock_guard<std::mutex> lk(registryMutex());
  registry()[name] = fn;
  return true;
}

namespace cl {

clKernelFn findKernel(const std::string& name) {
  std::lock_guard<std::mutex> lk(registryMutex());
  auto it = registry().find(name);
  return it == registry().end() ? nullptr : it->second;
}

std::vector<std::string> kernelNames() {
  std::lock_guard<std::mutex> lk(registryMutex());
  std::vector<std::string> names;
  for (const auto& [name, fn] : registry()) names.push_back(name);
  return names;
}

}  // namespace cl

void clBlockContext::read(clDevPtr src, void* dst, size_t bytes, clCache cache) {
  if (bytes == 0) return;
  const std::vector<cl::ReadSeg> segs{cl::ReadSeg{src, bytes, dst}};
  check(cache == clCache::Cached && env_->cache ? env_->cache->readv(segs) : env_->dsm->readv(segs));
}

void clBlockContext::read2D(void* dst, size_t dstPitch, clDevPtr src, size_t srcPitch, size_t widthBytes,
                            size_t rows, clCache cache) {
  if (rows == 0 || widthBytes == 0) return;
  std::vector<cl::ReadSeg> segs;
  if (widthBytes == srcPitch && srcPitch == dstPitch) {
    segs.push_back(cl::ReadSeg{src, widthBytes * rows, dst});
  } else {
    segs.reserve(rows);
    for (size_t r = 0; r < rows; ++r) {
      segs.push_back(cl::ReadSeg{src + r * srcPitch, widthBytes, static_cast<uint8_t*>(dst) + r * dstPitch});
    }
  }
  check(cache == clCache::Cached && env_->cache ? env_->cache->readv(segs) : env_->dsm->readv(segs));
}

void clBlockContext::write(clDevPtr dst, const void* src, size_t bytes) {
  if (bytes == 0) return;
  check(env_->dsm->write(dst, src, bytes));
  if (env_->cache) env_->cache->invalidate(dst, bytes);
}

void clBlockContext::write2D(clDevPtr dst, size_t dstPitch, const void* src, size_t srcPitch, size_t widthBytes,
                             size_t rows) {
  if (rows == 0 || widthBytes == 0) return;
  std::vector<cl::WriteSeg> segs;
  if (widthBytes == srcPitch && srcPitch == dstPitch) {
    segs.push_back(cl::WriteSeg{dst, widthBytes * rows, src});
  } else {
    segs.reserve(rows);
    for (size_t r = 0; r < rows; ++r) {
      segs.push_back(cl::WriteSeg{dst + r * dstPitch, widthBytes, static_cast<const uint8_t*>(src) + r * srcPitch});
    }
  }
  check(env_->dsm->writev(segs));
  if (env_->cache) env_->cache->invalidate(dst, (rows - 1) * dstPitch + widthBytes);
}

void clBlockContext::parallelFor(size_t n, const std::function<void(size_t, size_t)>& body) {
  env_->pool->parallelFor(n, body);
}

unsigned clBlockContext::numThreads() const { return env_->pool->size(); }

void* clBlockContext::scratch(size_t bytes) {
  auto& bufs = env_->scratch;
  if (env_->scratchNext == bufs.size()) bufs.emplace_back();
  auto& buf = bufs[env_->scratchNext++];
  if (buf.size() < bytes) buf.resize(bytes);
  return buf.data();
}

uint32_t clBlockContext::workerId() const { return env_->workerId; }
