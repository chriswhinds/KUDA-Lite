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

#include "common/net.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace cl {
namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;  // Linux
#else
constexpr int kSendFlags = 0;  // macOS: SO_NOSIGPIPE is set per socket instead
#endif

std::string errnoText(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

void configureSocket(int fd) {
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

std::string numericHost(const sockaddr_storage& ss, socklen_t len, uint16_t* port) {
  char host[NI_MAXHOST] = {0};
  char serv[NI_MAXSERV] = {0};
  if (::getnameinfo(reinterpret_cast<const sockaddr*>(&ss), len, host, sizeof host, serv, sizeof serv,
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    return "?";
  }
  if (port) *port = static_cast<uint16_t>(std::atoi(serv));
  return host;
}

}  // namespace

Socket::~Socket() {
  if (fd_ >= 0) ::close(fd_);
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

Socket Socket::connectTo(const std::string& host, uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string service = std::to_string(port);
  const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
  if (rc != 0) throw NetError("resolve " + host + ": " + ::gai_strerror(rc));

  int fd = -1;
  std::string lastError = "no addresses";
  for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
    fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) {
      lastError = errnoText("socket");
      continue;
    }
    if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    lastError = errnoText("connect");
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(res);
  if (fd < 0) throw NetError(host + ":" + service + ": " + lastError);
  configureSocket(fd);
  return Socket(fd);
}

void Socket::sendAll(const void* data, size_t n) {
  const auto* p = static_cast<const uint8_t*>(data);
  while (n > 0) {
    const ssize_t k = ::send(fd_, p, n, kSendFlags);
    if (k < 0) {
      if (errno == EINTR) continue;
      throw NetError(errnoText("send"));
    }
    p += k;
    n -= static_cast<size_t>(k);
  }
}

void Socket::recvAll(void* data, size_t n) {
  auto* p = static_cast<uint8_t*>(data);
  while (n > 0) {
    const ssize_t k = ::recv(fd_, p, n, 0);
    if (k == 0) throw NetError("connection closed by peer");
    if (k < 0) {
      if (errno == EINTR) continue;
      throw NetError(errnoText("recv"));
    }
    p += k;
    n -= static_cast<size_t>(k);
  }
}

void Socket::shutdownBoth() {
  if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
}

std::string Socket::localHost() const {
  sockaddr_storage ss{};
  socklen_t len = sizeof ss;
  if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return "?";
  return numericHost(ss, len, nullptr);
}

std::string Socket::peerName() const {
  sockaddr_storage ss{};
  socklen_t len = sizeof ss;
  if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return "?";
  uint16_t port = 0;
  const std::string host = numericHost(ss, len, &port);
  return host + ":" + std::to_string(port);
}

Listener::Listener(const std::string& host, uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* res = nullptr;
  const std::string service = std::to_string(port);
  const int rc = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &res);
  if (rc != 0) throw NetError("resolve " + host + ": " + ::gai_strerror(rc));

  std::string lastError = "no addresses";
  for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
    const int fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) {
      lastError = errnoText("socket");
      continue;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (::bind(fd, p->ai_addr, p->ai_addrlen) == 0 && ::listen(fd, 128) == 0) {
      fd_ = fd;
      break;
    }
    lastError = errnoText("bind/listen");
    ::close(fd);
  }
  ::freeaddrinfo(res);
  if (fd_ < 0) throw NetError("listen on " + host + ":" + service + ": " + lastError);

  sockaddr_storage ss{};
  socklen_t len = sizeof ss;
  ::getsockname(fd_, reinterpret_cast<sockaddr*>(&ss), &len);
  numericHost(ss, len, &port_);
}

Listener::~Listener() {
  if (fd_ >= 0) ::close(fd_);
}

Socket Listener::accept() {
  for (;;) {
    const int fd = ::accept(fd_, nullptr, nullptr);
    if (fd >= 0) {
      configureSocket(fd);
      return Socket(fd);
    }
    if (errno == EINTR) continue;
    throw NetError(errnoText("accept"));
  }
}

bool parseHostPort(const std::string& text, uint16_t defaultPort, std::string* host, uint16_t* port) {
  *port = defaultPort;
  if (text.empty()) return false;
  std::string h = text;
  std::string p;
  if (text.front() == '[') {  // [v6]:port
    const size_t close = text.find(']');
    if (close == std::string::npos) return false;
    h = text.substr(1, close - 1);
    if (close + 1 < text.size()) {
      if (text[close + 1] != ':') return false;
      p = text.substr(close + 2);
    }
  } else {
    const size_t colon = text.rfind(':');
    if (colon != std::string::npos && text.find(':') == colon) {  // exactly one colon: host:port
      h = text.substr(0, colon);
      p = text.substr(colon + 1);
    }
  }
  if (!p.empty()) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(p.c_str(), &end, 10);
    if (*end != '\0' || v > 65535) return false;
    *port = static_cast<uint16_t>(v);
  }
  *host = h;
  return !h.empty();
}

void ignoreSigpipe() { ::signal(SIGPIPE, SIG_IGN); }

}  // namespace cl
