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

#include "worker/page_cache.h"

#include <cstring>

namespace cl {

PageCache::PageCache(DsmClient& dsm, uint64_t capacityBytes) : dsm_(dsm), capacity_(capacityBytes) {}

clError_t PageCache::readv(const std::vector<ReadSeg>& segs) {
  if (capacity_ == 0) return dsm_.readv(segs);

  struct Pending {
    Extent extent;
    uint8_t* dst;
  };
  struct Use {
    Entry entry;
    uint64_t inPage;
    uint64_t len;
    uint8_t* dst;
  };

  // 1. Translate; local pieces go straight to the raw read list.
  const uint32_t self = dsm_.selfId();
  std::vector<RawRead> raws;
  std::vector<Pending> remote;
  std::vector<Extent> ext;
  for (const auto& s : segs) {
    ext.clear();
    const clError_t st = dsm_.map().resolve(s.addr, s.len, &ext);
    if (st != clSuccess) return st;
    for (const auto& e : ext) {
      uint8_t* dst = static_cast<uint8_t*>(s.dst) + e.requestOffset;
      if (e.worker == self) {
        raws.push_back(RawRead{e.worker, e.localOffset, e.len, dst});
      } else {
        remote.push_back(Pending{e, dst});
      }
    }
  }

  // 2. Look up remote pages; claim the missing ones by inserting a not-yet-ready entry.
  std::vector<Use> uses;
  std::vector<std::shared_ptr<std::promise<clError_t>>> promises;
  std::vector<clDevPtr> claimed;
  uses.reserve(remote.size());
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& p : remote) {
      const Extent& e = p.extent;
      auto it = entries_.find(e.pageGva);
      if (it == entries_.end()) {
        auto promise = std::make_shared<std::promise<clError_t>>();
        Entry entry{std::make_shared<std::vector<uint8_t>>(e.pageLen), promise->get_future().share()};
        it = entries_.emplace(e.pageGva, entry).first;
        fifo_.push_back(e.pageGva);
        bytes_ += e.pageLen;
        raws.push_back(RawRead{e.worker, e.pageLocalOffset, e.pageLen, entry.data->data()});
        promises.push_back(std::move(promise));
        claimed.push_back(e.pageGva);
        ++misses_;
      } else {
        ++hits_;
      }
      uses.push_back(Use{it->second, e.localOffset - e.pageLocalOffset, e.len, p.dst});
    }
    evictLocked();  // uses[] keeps evicted-but-needed pages alive
  }

  // 3. One batched fetch for every missing page plus the local pieces.
  const clError_t st = dsm_.readRaw(raws);
  for (auto& promise : promises) promise->set_value(st);
  if (st != clSuccess) {
    std::lock_guard<std::mutex> lk(mu_);
    for (clDevPtr key : claimed) {
      auto it = entries_.find(key);
      if (it != entries_.end()) {
        bytes_ -= it->second.data->size();
        entries_.erase(it);
      }
    }
    return st;
  }

  // 4. Copy out (waiting on pages another thread is still fetching).
  for (const auto& u : uses) {
    const clError_t ready = u.entry.ready.get();
    if (ready != clSuccess) return ready;
    std::memcpy(u.dst, u.entry.data->data() + u.inPage, u.len);
  }
  return clSuccess;
}

void PageCache::invalidate(clDevPtr addr, uint64_t len) {
  std::vector<Extent> ext;
  if (dsm_.map().resolve(addr, len, &ext) != clSuccess) return;
  std::lock_guard<std::mutex> lk(mu_);
  for (const auto& e : ext) {
    auto it = entries_.find(e.pageGva);
    if (it != entries_.end()) {
      bytes_ -= it->second.data->size();
      entries_.erase(it);
    }
  }
}

PageCache::Stats PageCache::stats() {
  std::lock_guard<std::mutex> lk(mu_);
  return Stats{bytes_, capacity_, hits_, misses_};
}

void PageCache::clear() {
  std::lock_guard<std::mutex> lk(mu_);
  entries_.clear();
  fifo_.clear();
  bytes_ = 0;
}

void PageCache::evictLocked() {
  while (bytes_ > capacity_ && !fifo_.empty()) {
    const clDevPtr key = fifo_.front();
    fifo_.pop_front();
    auto it = entries_.find(key);
    if (it != entries_.end()) {
      bytes_ -= it->second.data->size();
      entries_.erase(it);
    }
  }
}

}  // namespace cl
