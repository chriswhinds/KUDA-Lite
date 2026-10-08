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

// Distributed shared memory (DSM) client: reads and writes global memory ranges by talking to
// the workers that own the pages. Used by the controller (to service host memcpy/memset) and
// by every worker (so kernels can reach any address in the cluster).
//
// Requests are batched: all pieces destined for one worker are sent in a single MemReadV /
// MemWriteV message (split at 16 MiB), and the requests to different workers are in flight
// concurrently. Pieces owned by the local worker are served with a plain memcpy.
// Thread-safe.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/address_map.h"
#include "common/protocol.h"
#include "common/rpc.h"

namespace cl {

struct ReadSeg {
  clDevPtr addr;
  uint64_t len;
  void* dst;
};
struct WriteSeg {
  clDevPtr addr;
  uint64_t len;
  const void* src;
};
/// A read/write already translated to (worker, arena offset).
struct RawRead {
  uint32_t worker;
  uint64_t offset;
  uint64_t len;
  void* dst;
};
struct RawWrite {
  uint32_t worker;
  uint64_t offset;
  uint64_t len;
  const void* src;
};

class DsmClient {
 public:
  DsmClient(const AddressMap& map, std::string name);

  /// Workers only: the local arena, used to short-circuit pieces this node owns.
  void setLocalArena(uint8_t* base, uint64_t size);
  void setSelfId(uint32_t id) { self_ = id; }
  uint32_t selfId() const { return self_.load(); }

  /// Replaces the data-plane address book; connections to departed workers are closed.
  void setPeers(const std::vector<PeerAddr>& peers);

  const AddressMap& map() const { return map_; }

  clError_t read(clDevPtr src, void* dst, uint64_t len) { return readv({ReadSeg{src, len, dst}}); }
  clError_t write(clDevPtr dst, const void* src, uint64_t len) { return writev({WriteSeg{dst, len, src}}); }
  clError_t readv(const std::vector<ReadSeg>& segs);
  clError_t writev(const std::vector<WriteSeg>& segs);
  clError_t fill(clDevPtr dst, uint8_t value, uint64_t len);

  clError_t readRaw(const std::vector<RawRead>& reads);
  clError_t writeRaw(const std::vector<RawWrite>& writes);

  struct Stats {
    uint64_t localBytes = 0;
    uint64_t remoteReadBytes = 0;
    uint64_t remoteWriteBytes = 0;
  };
  Stats stats() const;

 private:
  std::shared_ptr<RpcPeer> peerFor(uint32_t worker, clError_t* err);
  bool localRange(uint64_t off, uint64_t len) const {
    return arena_ != nullptr && off <= arenaSize_ && len <= arenaSize_ - off;
  }

  const AddressMap& map_;
  std::string name_;
  std::atomic<uint32_t> self_{kNoWorker};
  uint8_t* arena_ = nullptr;
  uint64_t arenaSize_ = 0;

  std::mutex peersMu_;
  std::unordered_map<uint32_t, PeerAddr> addrs_;
  std::unordered_map<uint32_t, std::shared_ptr<RpcPeer>> conns_;

  std::atomic<uint64_t> localBytes_{0};
  std::atomic<uint64_t> remoteReadBytes_{0};
  std::atomic<uint64_t> remoteWriteBytes_{0};
};

}  // namespace cl
