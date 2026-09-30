// Blocking TCP sockets (POSIX; works on Linux and macOS).
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace cl {

class NetError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Owning, move-only TCP socket. The descriptor is closed only by the destructor, so
/// shutdownBoth() may be called from any thread to unblock a reader safely.
class Socket {
 public:
  Socket() = default;
  explicit Socket(int fd) : fd_(fd) {}
  ~Socket();
  Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  static Socket connectTo(const std::string& host, uint16_t port);

  void sendAll(const void* data, size_t n);
  void recvAll(void* data, size_t n);  // throws NetError on EOF
  void shutdownBoth();

  bool valid() const { return fd_ >= 0; }
  std::string localHost() const;  // numeric address of our end
  std::string peerName() const;   // "host:port" of the remote end

 private:
  int fd_ = -1;
};

class Listener {
 public:
  Listener(const std::string& host, uint16_t port);  // port 0 = ephemeral
  ~Listener();
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  Socket accept();
  uint16_t port() const { return port_; }

 private:
  int fd_ = -1;
  uint16_t port_ = 0;
};

/// Parses "host", "host:port" or "[v6addr]:port".
bool parseHostPort(const std::string& text, uint16_t defaultPort, std::string* host, uint16_t* port);

/// Writes to a closed socket must return an error instead of killing the process.
void ignoreSigpipe();

}  // namespace cl
