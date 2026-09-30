// Bidirectional, multiplexed request/response over one TCP connection.
//
// Either side may issue requests at any time; responses are matched by request id, so many
// requests can be in flight at once on the same connection. One reader thread per peer
// receives frames:
//   * responses complete the pending callback (run on the reader thread);
//   * requests are passed to the Handler (run on the reader thread).
// Rules that follow from this:
//   * Handlers and callbacks must be quick. Long work (kernel execution, stream ops) must be
//     handed to another thread, which answers later with reply().
//   * Never block on callSync() of a peer from inside that same peer's handler or callback.
#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/net.h"
#include "common/protocol.h"

namespace cl {

class RpcPeer : public std::enable_shared_from_this<RpcPeer> {
 public:
  using Handler = std::function<void(const std::shared_ptr<RpcPeer>& peer, Message&& request)>;
  /// Always invoked exactly once. On connection loss the message has status clErrorNetwork.
  using Callback = std::function<void(Message&& response)>;

  /// Takes ownership of a connected socket and starts the reader thread. `onClose` runs once,
  /// on the reader thread, after the connection ends and all pending calls have failed.
  static std::shared_ptr<RpcPeer> start(Socket sock, Handler handler, std::string name,
                                        std::function<void()> onClose = nullptr);
  static std::shared_ptr<RpcPeer> connect(const std::string& host, uint16_t port, Handler handler, std::string name,
                                          std::function<void()> onClose = nullptr);

  void callAsync(MsgType type, std::vector<uint8_t> payload, Callback cb);
  std::future<Message> call(MsgType type, std::vector<uint8_t> payload);
  Message callSync(MsgType type, std::vector<uint8_t> payload) { return call(type, std::move(payload)).get(); }

  void reply(uint64_t requestId, MsgType type, clError_t status, const std::vector<uint8_t>& payload = {});
  void reply(const Message& request, clError_t status, const std::vector<uint8_t>& payload = {}) {
    reply(request.requestId, request.type, status, payload);
  }

  void close();
  bool alive() const { return alive_.load(); }
  const std::string& name() const { return name_; }

  /// Bytes sent / received by every RpcPeer in this process (headers included), for telemetry.
  static uint64_t totalBytesSent();
  static uint64_t totalBytesReceived();

 private:
  RpcPeer(Socket sock, Handler handler, std::string name, std::function<void()> onClose);
  void readerLoop();
  void dispatch(const std::shared_ptr<RpcPeer>& self, Message&& m);
  void complete(Message&& m);
  void failPending();
  bool sendFrame(MsgType type, uint32_t flags, clError_t status, uint64_t id, const std::vector<uint8_t>& payload);

  Socket sock_;
  Handler handler_;
  std::string name_;
  std::function<void()> onClose_;

  std::mutex sendMu_;
  std::mutex pendMu_;
  std::unordered_map<uint64_t, Callback> pending_;
  uint64_t nextId_ = 1;
  std::atomic<bool> alive_{true};
};

}  // namespace cl
