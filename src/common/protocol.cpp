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

#include "common/protocol.h"

namespace cl {

const char* msgTypeName(MsgType type) {
  switch (type) {
    case MsgType::Ping: return "Ping";
    case MsgType::Hello: return "Hello";
    case MsgType::GetDeviceProps: return "GetDeviceProps";
    case MsgType::GetWorkers: return "GetWorkers";
    case MsgType::Malloc: return "Malloc";
    case MsgType::Free: return "Free";
    case MsgType::MemcpyH2D: return "MemcpyH2D";
    case MsgType::MemcpyD2H: return "MemcpyD2H";
    case MsgType::MemcpyD2D: return "MemcpyD2D";
    case MsgType::Memset: return "Memset";
    case MsgType::Launch: return "Launch";
    case MsgType::EventRecord: return "EventRecord";
    case MsgType::StreamWaitEvent: return "StreamWaitEvent";
    case MsgType::StreamDestroy: return "StreamDestroy";
    case MsgType::GetTelemetry: return "GetTelemetry";
    case MsgType::Register: return "Register";
    case MsgType::ClusterMap: return "ClusterMap";
    case MsgType::AllocAdd: return "AllocAdd";
    case MsgType::AllocRemove: return "AllocRemove";
    case MsgType::ExecBlocks: return "ExecBlocks";
    case MsgType::Telemetry: return "Telemetry";
    case MsgType::MemReadV: return "MemReadV";
    case MsgType::MemWriteV: return "MemWriteV";
    case MsgType::MemFillV: return "MemFillV";
  }
  return "Unknown";
}

void encodeClusterMap(ByteWriter& w, const std::vector<PeerAddr>& peers) {
  w.put<uint32_t>(static_cast<uint32_t>(peers.size()));
  for (const auto& p : peers) {
    w.put<uint32_t>(p.id);
    w.putString(p.host);
    w.put<uint16_t>(p.port);
  }
}

std::vector<PeerAddr> decodeClusterMap(ByteReader& r) {
  const uint32_t n = r.get<uint32_t>();
  if (n > r.remaining()) throw ProtocolError("bad cluster map count");
  std::vector<PeerAddr> peers(n);
  for (auto& p : peers) {
    p.id = r.get<uint32_t>();
    p.host = r.getString();
    p.port = r.get<uint16_t>();
  }
  return peers;
}

void ExecBlocksRequest::encode(ByteWriter& w) const {
  w.put<uint64_t>(launchId);
  w.putString(kernel);
  putDim3(w, grid);
  putDim3(w, block);
  w.putBlob(args.data(), args.size());
  w.put<uint64_t>(blockBegin);
  w.put<uint64_t>(blockEnd);
}

ExecBlocksRequest ExecBlocksRequest::decode(ByteReader& r) {
  ExecBlocksRequest req;
  req.launchId = r.get<uint64_t>();
  req.kernel = r.getString();
  req.grid = getDim3(r);
  req.block = getDim3(r);
  req.args = r.getBlob();
  req.blockBegin = r.get<uint64_t>();
  req.blockEnd = r.get<uint64_t>();
  return req;
}

}  // namespace cl
