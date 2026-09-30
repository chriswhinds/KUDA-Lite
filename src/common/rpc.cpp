#include "common/rpc.h"

#include <thread>

#include "common/log.h"

namespace cl {
namespace {

std::atomic<uint64_t> gBytesSent{0};
std::atomic<uint64_t> gBytesReceived{0};

Message failure(MsgType type) {
  Message m;
  m.type = type;
  m.flags = kFlagResponse;
  m.status = clErrorNetwork;
  return m;
}

}  // namespace

RpcPeer::RpcPeer(Socket sock, Handler handler, std::string name, std::function<void()> onClose)
    : sock_(std::move(sock)), handler_(std::move(handler)), name_(std::move(name)), onClose_(std::move(onClose)) {}

uint64_t RpcPeer::totalBytesSent() { return gBytesSent.load(std::memory_order_relaxed); }
uint64_t RpcPeer::totalBytesReceived() { return gBytesReceived.load(std::memory_order_relaxed); }

std::shared_ptr<RpcPeer> RpcPeer::start(Socket sock, Handler handler, std::string name,
                                        std::function<void()> onClose) {
  std::shared_ptr<RpcPeer> peer(new RpcPeer(std::move(sock), std::move(handler), std::move(name), std::move(onClose)));
  // The reader thread co-owns the peer, so it stays alive until the connection ends.
  std::thread([peer] { peer->readerLoop(); }).detach();
  return peer;
}

std::shared_ptr<RpcPeer> RpcPeer::connect(const std::string& host, uint16_t port, Handler handler, std::string name,
                                          std::function<void()> onClose) {
  return start(Socket::connectTo(host, port), std::move(handler), std::move(name), std::move(onClose));
}

void RpcPeer::callAsync(MsgType type, std::vector<uint8_t> payload, Callback cb) {
  uint64_t id = 0;
  bool open = false;
  {
    std::lock_guard<std::mutex> lk(pendMu_);
    open = alive_.load();
    if (open) {
      id = nextId_++;
      pending_.emplace(id, std::move(cb));
    }
  }
  if (!open) {
    if (cb) cb(failure(type));
    return;
  }
  if (!sendFrame(type, 0, clSuccess, id, payload)) {
    // The reader may already have failed it; only complete it if it is still ours.
    Callback mine;
    {
      std::lock_guard<std::mutex> lk(pendMu_);
      auto it = pending_.find(id);
      if (it != pending_.end()) {
        mine = std::move(it->second);
        pending_.erase(it);
      }
    }
    if (mine) mine(failure(type));
  }
}

std::future<Message> RpcPeer::call(MsgType type, std::vector<uint8_t> payload) {
  auto promise = std::make_shared<std::promise<Message>>();
  auto future = promise->get_future();
  callAsync(type, std::move(payload), [promise](Message&& m) { promise->set_value(std::move(m)); });
  return future;
}

void RpcPeer::reply(uint64_t requestId, MsgType type, clError_t status, const std::vector<uint8_t>& payload) {
  sendFrame(type, kFlagResponse, status, requestId, payload);
}

void RpcPeer::close() { sock_.shutdownBoth(); }

bool RpcPeer::sendFrame(MsgType type, uint32_t flags, clError_t status, uint64_t id,
                        const std::vector<uint8_t>& payload) {
  FrameHeader h{kMagic, kProtocolVersion, static_cast<uint16_t>(type), flags, static_cast<int32_t>(status), id,
                payload.size()};
  std::lock_guard<std::mutex> lk(sendMu_);
  try {
    sock_.sendAll(&h, sizeof h);
    if (!payload.empty()) sock_.sendAll(payload.data(), payload.size());
    gBytesSent.fetch_add(sizeof h + payload.size(), std::memory_order_relaxed);
    return true;
  } catch (const std::exception& e) {
    LOG_DEBUG(name_ << ": send " << msgTypeName(type) << " failed: " << e.what());
    sock_.shutdownBoth();  // make the reader notice and fail everything pending
    return false;
  }
}

void RpcPeer::readerLoop() {
  auto self = shared_from_this();
  try {
    for (;;) {
      FrameHeader h{};
      sock_.recvAll(&h, sizeof h);
      if (h.magic != kMagic) throw ProtocolError("bad frame magic");
      if (h.version != kProtocolVersion) throw ProtocolError("unsupported protocol version " + std::to_string(h.version));
      if (h.payloadLen > kMaxPayloadBytes) throw ProtocolError("frame too large");
      Message m;
      m.type = static_cast<MsgType>(h.type);
      m.flags = h.flags;
      m.status = static_cast<clError_t>(h.status);
      m.requestId = h.requestId;
      m.payload.resize(h.payloadLen);
      if (h.payloadLen != 0) sock_.recvAll(m.payload.data(), m.payload.size());
      gBytesReceived.fetch_add(sizeof h + h.payloadLen, std::memory_order_relaxed);
      if (m.isResponse()) {
        complete(std::move(m));
      } else {
        dispatch(self, std::move(m));
      }
    }
  } catch (const std::exception& e) {
    LOG_DEBUG(name_ << ": connection ended: " << e.what());
  }
  {
    std::lock_guard<std::mutex> lk(pendMu_);
    alive_ = false;
  }
  sock_.shutdownBoth();
  failPending();
  if (onClose_) {
    try {
      onClose_();
    } catch (const std::exception& e) {
      LOG_ERROR(name_ << ": onClose threw: " << e.what());
    }
  }
}

void RpcPeer::dispatch(const std::shared_ptr<RpcPeer>& self, Message&& m) {
  const uint64_t id = m.requestId;
  const MsgType type = m.type;
  if (!handler_) {
    reply(id, type, clErrorProtocol);
    return;
  }
  try {
    handler_(self, std::move(m));
  } catch (const ProtocolError& e) {
    LOG_WARN(name_ << ": malformed " << msgTypeName(type) << ": " << e.what());
    reply(id, type, clErrorProtocol);
  } catch (const std::exception& e) {
    LOG_ERROR(name_ << ": handler for " << msgTypeName(type) << " threw: " << e.what());
    reply(id, type, clErrorUnknown);
  }
}

void RpcPeer::complete(Message&& m) {
  Callback cb;
  {
    std::lock_guard<std::mutex> lk(pendMu_);
    auto it = pending_.find(m.requestId);
    if (it == pending_.end()) {
      LOG_WARN(name_ << ": response for unknown request " << m.requestId);
      return;
    }
    cb = std::move(it->second);
    pending_.erase(it);
  }
  if (!cb) return;
  try {
    cb(std::move(m));
  } catch (const std::exception& e) {
    LOG_ERROR(name_ << ": response callback threw: " << e.what());
  }
}

void RpcPeer::failPending() {
  std::unordered_map<uint64_t, Callback> orphans;
  {
    std::lock_guard<std::mutex> lk(pendMu_);
    orphans.swap(pending_);
  }
  for (auto& [id, cb] : orphans) {
    if (!cb) continue;
    try {
      cb(failure(MsgType::Ping));
    } catch (const std::exception& e) {
      LOG_ERROR(name_ << ": failure callback threw: " << e.what());
    }
  }
}

}  // namespace cl
