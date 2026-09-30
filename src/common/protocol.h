// KUDA-Lite wire protocol: frame header, message types and shared payload structures.
// The authoritative description of every payload is docs/PROTOCOL.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/bytes.h"
#include "kudalite/cl_types.h"

namespace cl {

constexpr uint32_t kMagic = 0x31544C43;  // bytes "CLT1" on the wire
constexpr uint16_t kProtocolVersion = 1;
constexpr uint64_t kMaxPayloadBytes = 1ull << 30;  // sanity limit per frame

constexpr uint16_t kDefaultHostPort = 7070;    // host runtime  -> controller
constexpr uint16_t kDefaultWorkerPort = 7071;  // worker        -> controller (registration/control)
constexpr uint16_t kDefaultDataPort = 7100;    // anyone        -> worker (global-memory data plane)

constexpr uint32_t kNoWorker = 0xFFFFFFFFu;

enum class MsgType : uint16_t {
  Ping = 1,

  // Host runtime -> controller
  Hello = 100,
  GetDeviceProps = 101,
  GetWorkers = 102,
  Malloc = 103,
  Free = 104,
  MemcpyH2D = 110,  // stream op
  MemcpyD2H = 111,  // stream op
  MemcpyD2D = 112,  // stream op
  Memset = 113,     // stream op
  Launch = 114,     // stream op
  EventRecord = 115,
  StreamWaitEvent = 116,
  StreamDestroy = 117,
  GetTelemetry = 118,

  // Worker <-> controller (control plane)
  Register = 200,     // worker -> controller
  ClusterMap = 201,   // controller -> worker
  AllocAdd = 202,     // controller -> worker
  AllocRemove = 203,  // controller -> worker
  ExecBlocks = 204,   // controller -> worker
  Telemetry = 205,    // worker -> controller, periodic

  // Data plane (controller or worker -> owning worker); offsets are arena offsets
  MemReadV = 300,
  MemWriteV = 301,
  MemFillV = 302,
};

const char* msgTypeName(MsgType type);

#pragma pack(push, 1)
struct FrameHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t type;        // MsgType
  uint32_t flags;       // kFlagResponse
  int32_t status;       // clError_t; meaningful on responses
  uint64_t requestId;   // echoed by the response
  uint64_t payloadLen;  // bytes following the header
};
#pragma pack(pop)
static_assert(sizeof(FrameHeader) == 32, "FrameHeader must be 32 bytes");

constexpr uint32_t kFlagResponse = 1u;

struct Message {
  MsgType type = MsgType::Ping;
  uint32_t flags = 0;
  clError_t status = clSuccess;
  uint64_t requestId = 0;
  std::vector<uint8_t> payload;

  bool isResponse() const { return (flags & kFlagResponse) != 0; }
};

// ---- Shared payload structures ------------------------------------------------------------

inline void putDim3(ByteWriter& w, const clDim3& d) {
  w.put<uint32_t>(d.x).put<uint32_t>(d.y).put<uint32_t>(d.z);
}
inline clDim3 getDim3(ByteReader& r) {
  const uint32_t x = r.get<uint32_t>(), y = r.get<uint32_t>(), z = r.get<uint32_t>();
  return clDim3(x, y, z);
}

/// Data-plane address of one worker.
struct PeerAddr {
  uint32_t id = kNoWorker;
  std::string host;
  uint16_t port = 0;
};
void encodeClusterMap(ByteWriter& w, const std::vector<PeerAddr>& peers);
std::vector<PeerAddr> decodeClusterMap(ByteReader& r);

/// Controller -> worker: run blocks [blockBegin, blockEnd) of a launch.
struct ExecBlocksRequest {
  uint64_t launchId = 0;
  std::string kernel;
  clDim3 grid;
  clDim3 block;
  std::vector<uint8_t> args;
  uint64_t blockBegin = 0;
  uint64_t blockEnd = 0;

  void encode(ByteWriter& w) const;
  static ExecBlocksRequest decode(ByteReader& r);
};

}  // namespace cl
