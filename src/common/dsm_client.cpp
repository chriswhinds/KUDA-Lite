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

#include "common/dsm_client.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <map>

#include "common/log.h"

namespace cl {
namespace {

constexpr uint64_t kMaxBatchBytes = 16ull << 20;

/// Counts outstanding asynchronous requests and keeps the first error.
class Completion {
 public:
  void add() {
    std::lock_guard<std::mutex> lk(mu_);
    ++pending_;
  }
  void done(clError_t status) {
    std::lock_guard<std::mutex> lk(mu_);
    if (status != clSuccess && err_ == clSuccess) err_ = status;
    if (--pending_ == 0) cv_.notify_all();
  }
  clError_t wait() {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&] { return pending_ == 0; });
    return err_;
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  size_t pending_ = 0;
  clError_t err_ = clSuccess;
};

void rejectRequests(const std::shared_ptr<RpcPeer>& peer, Message&& m) { peer->reply(m, clErrorProtocol); }

}  // namespace

DsmClient::DsmClient(const AddressMap& map, std::string name) : map_(map), name_(std::move(name)) {}

void DsmClient::setLocalArena(uint8_t* base, uint64_t size) {
  arena_ = base;
  arenaSize_ = size;
}

void DsmClient::setPeers(const std::vector<PeerAddr>& peers) {
  std::vector<std::shared_ptr<RpcPeer>> stale;
  {
    std::lock_guard<std::mutex> lk(peersMu_);
    addrs_.clear();
    for (const auto& p : peers) addrs_[p.id] = p;
    for (auto it = conns_.begin(); it != conns_.end();) {
      auto a = addrs_.find(it->first);
      if (a == addrs_.end()) {
        stale.push_back(std::move(it->second));
        it = conns_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& peer : stale) peer->close();
}

std::shared_ptr<RpcPeer> DsmClient::peerFor(uint32_t worker, clError_t* err) {
  std::lock_guard<std::mutex> lk(peersMu_);
  auto it = conns_.find(worker);
  if (it != conns_.end() && it->second->alive()) return it->second;
  auto a = addrs_.find(worker);
  if (a == addrs_.end()) {
    *err = clErrorWorkerLost;
    return nullptr;
  }
  try {
    auto peer = RpcPeer::connect(a->second.host, a->second.port, rejectRequests,
                                 name_ + "->worker" + std::to_string(worker));
    conns_[worker] = peer;
    return peer;
  } catch (const std::exception& e) {
    LOG_ERROR(name_ << ": cannot reach worker " << worker << " at " << a->second.host << ":" << a->second.port
                    << ": " << e.what());
    *err = clErrorNetwork;
    return nullptr;
  }
}

clError_t DsmClient::readv(const std::vector<ReadSeg>& segs) {
  std::vector<RawRead> raws;
  std::vector<Extent> ext;
  for (const auto& s : segs) {
    ext.clear();
    const clError_t st = map_.resolve(s.addr, s.len, &ext);
    if (st != clSuccess) return st;
    coalesceExtents(&ext);
    for (const auto& e : ext) {
      raws.push_back(RawRead{e.worker, e.localOffset, e.len, static_cast<uint8_t*>(s.dst) + e.requestOffset});
    }
  }
  return readRaw(raws);
}

clError_t DsmClient::writev(const std::vector<WriteSeg>& segs) {
  std::vector<RawWrite> raws;
  std::vector<Extent> ext;
  for (const auto& s : segs) {
    ext.clear();
    const clError_t st = map_.resolve(s.addr, s.len, &ext);
    if (st != clSuccess) return st;
    coalesceExtents(&ext);
    for (const auto& e : ext) {
      raws.push_back(RawWrite{e.worker, e.localOffset, e.len, static_cast<const uint8_t*>(s.src) + e.requestOffset});
    }
  }
  return writeRaw(raws);
}

clError_t DsmClient::readRaw(const std::vector<RawRead>& reads) {
  // Split oversized pieces so that no single message exceeds kMaxBatchBytes of data.
  std::vector<RawRead> pieces;
  pieces.reserve(reads.size());
  for (const auto& r : reads) {
    for (uint64_t o = 0; o < r.len; o += kMaxBatchBytes) {
      pieces.push_back(RawRead{r.worker, r.offset + o, std::min(kMaxBatchBytes, r.len - o),
                               static_cast<uint8_t*>(r.dst) + o});
    }
  }
  std::map<uint32_t, std::vector<const RawRead*>> byWorker;
  for (const auto& p : pieces) byWorker[p.worker].push_back(&p);

  auto done = std::make_shared<Completion>();
  clError_t localErr = clSuccess;
  for (auto& [worker, list] : byWorker) {
    if (worker == self_.load()) {
      for (const RawRead* r : list) {
        if (!localRange(r->offset, r->len)) {
          localErr = clErrorInvalidDevicePointer;
          continue;
        }
        std::memcpy(r->dst, arena_ + r->offset, r->len);
        localBytes_ += r->len;
      }
      continue;
    }
    clError_t err = clSuccess;
    auto peer = peerFor(worker, &err);
    if (!peer) {
      localErr = err;
      continue;
    }
    size_t i = 0;
    while (i < list.size()) {
      auto batch = std::make_shared<std::vector<const RawRead*>>();
      uint64_t bytes = 0;
      while (i < list.size() && (batch->empty() || bytes + list[i]->len <= kMaxBatchBytes)) {
        bytes += list[i]->len;
        batch->push_back(list[i]);
        ++i;
      }
      ByteWriter w(4 + 16 * batch->size());
      w.put<uint32_t>(static_cast<uint32_t>(batch->size()));
      for (const RawRead* r : *batch) w.put<uint64_t>(r->offset).put<uint64_t>(r->len);
      done->add();
      remoteReadBytes_ += bytes;
      peer->callAsync(MsgType::MemReadV, w.take(), [done, batch, bytes](Message&& m) {
        clError_t st = m.status;
        if (st == clSuccess) {
          if (m.payload.size() != bytes) {
            st = clErrorProtocol;
          } else {
            const uint8_t* p = m.payload.data();
            for (const RawRead* r : *batch) {
              std::memcpy(r->dst, p, r->len);
              p += r->len;
            }
          }
        }
        done->done(st);
      });
    }
  }
  const clError_t remote = done->wait();
  return localErr != clSuccess ? localErr : remote;
}

clError_t DsmClient::writeRaw(const std::vector<RawWrite>& writes) {
  std::vector<RawWrite> pieces;
  pieces.reserve(writes.size());
  for (const auto& w : writes) {
    for (uint64_t o = 0; o < w.len; o += kMaxBatchBytes) {
      pieces.push_back(RawWrite{w.worker, w.offset + o, std::min(kMaxBatchBytes, w.len - o),
                                static_cast<const uint8_t*>(w.src) + o});
    }
  }
  std::map<uint32_t, std::vector<const RawWrite*>> byWorker;
  for (const auto& p : pieces) byWorker[p.worker].push_back(&p);

  auto done = std::make_shared<Completion>();
  clError_t localErr = clSuccess;
  for (auto& [worker, list] : byWorker) {
    if (worker == self_.load()) {
      for (const RawWrite* w : list) {
        if (!localRange(w->offset, w->len)) {
          localErr = clErrorInvalidDevicePointer;
          continue;
        }
        std::memcpy(arena_ + w->offset, w->src, w->len);
        localBytes_ += w->len;
      }
      continue;
    }
    clError_t err = clSuccess;
    auto peer = peerFor(worker, &err);
    if (!peer) {
      localErr = err;
      continue;
    }
    size_t i = 0;
    while (i < list.size()) {
      const size_t first = i;
      uint64_t bytes = 0;
      while (i < list.size() && (i == first || bytes + list[i]->len <= kMaxBatchBytes)) {
        bytes += list[i]->len;
        ++i;
      }
      const size_t count = i - first;
      ByteWriter w(4 + 16 * count + bytes);
      w.put<uint32_t>(static_cast<uint32_t>(count));
      for (size_t k = first; k < i; ++k) w.put<uint64_t>(list[k]->offset).put<uint64_t>(list[k]->len);
      for (size_t k = first; k < i; ++k) w.putBytes(list[k]->src, list[k]->len);
      done->add();
      remoteWriteBytes_ += bytes;
      peer->callAsync(MsgType::MemWriteV, w.take(), [done](Message&& m) { done->done(m.status); });
    }
  }
  const clError_t remote = done->wait();
  return localErr != clSuccess ? localErr : remote;
}

clError_t DsmClient::fill(clDevPtr dst, uint8_t value, uint64_t len) {
  std::vector<Extent> ext;
  const clError_t st = map_.resolve(dst, len, &ext);
  if (st != clSuccess) return st;
  coalesceExtents(&ext);
  std::map<uint32_t, std::vector<const Extent*>> byWorker;
  for (const auto& e : ext) byWorker[e.worker].push_back(&e);

  auto done = std::make_shared<Completion>();
  clError_t localErr = clSuccess;
  for (auto& [worker, list] : byWorker) {
    if (worker == self_.load()) {
      for (const Extent* e : list) {
        if (!localRange(e->localOffset, e->len)) {
          localErr = clErrorInvalidDevicePointer;
          continue;
        }
        std::memset(arena_ + e->localOffset, value, e->len);
      }
      continue;
    }
    clError_t err = clSuccess;
    auto peer = peerFor(worker, &err);
    if (!peer) {
      localErr = err;
      continue;
    }
    ByteWriter w(5 + 16 * list.size());
    w.put<uint8_t>(value);
    w.put<uint32_t>(static_cast<uint32_t>(list.size()));
    for (const Extent* e : list) w.put<uint64_t>(e->localOffset).put<uint64_t>(e->len);
    done->add();
    peer->callAsync(MsgType::MemFillV, w.take(), [done](Message&& m) { done->done(m.status); });
  }
  const clError_t remote = done->wait();
  return localErr != clSuccess ? localErr : remote;
}

DsmClient::Stats DsmClient::stats() const {
  Stats s;
  s.localBytes = localBytes_.load();
  s.remoteReadBytes = remoteReadBytes_.load();
  s.remoteWriteBytes = remoteWriteBytes_.load();
  return s;
}

}  // namespace cl
