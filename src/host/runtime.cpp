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

// libkudalite: implementation of the host API in include/kudalite/kudalite.h.
//
// The runtime holds one RPC connection to the controller. Every stream operation is sent as a
// request tagged with its stream id; the controller executes a stream's requests in order and
// answers each one when it has completed. The runtime counts unanswered requests per stream,
// so synchronising a stream means waiting for that count to reach zero. Large copies are
// split into 8 MiB requests so they pipeline through the controller.
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "common/bytes.h"
#include "common/net.h"
#include "common/protocol.h"
#include "common/rpc.h"
#include "common/telemetry.h"
#include "kudalite/kudalite.h"

using cl::ByteReader;
using cl::ByteWriter;
using cl::Message;
using cl::MsgType;
using cl::RpcPeer;

namespace {

constexpr uint64_t kCopyChunk = 8ull << 20;

struct StreamState {
  explicit StreamState(uint32_t streamId) : id(streamId) {}
  const uint32_t id;
  std::mutex mu;
  std::condition_variable cv;
  uint64_t inflight = 0;
  clError_t sticky = clSuccess;  // first asynchronous error since the last synchronisation
  clLaunchInfo lastLaunch{};
};

struct EventState {
  std::mutex mu;
  std::condition_variable cv;
  bool recorded = false;
  bool pending = false;
  uint64_t tsNs = 0;
  clError_t status = clSuccess;
};

}  // namespace

struct clStream_st {
  std::shared_ptr<StreamState> state;
};

struct clEvent_st {
  uint32_t id = 0;
  std::shared_ptr<EventState> state;
};

namespace {

class Runtime {
 public:
  static clError_t create(const std::string& endpoint, std::shared_ptr<Runtime>* out) {
    std::string host;
    uint16_t port = 0;
    if (!cl::parseHostPort(endpoint, cl::kDefaultHostPort, &host, &port)) return clErrorInvalidValue;
    auto rt = std::shared_ptr<Runtime>(new Runtime());
    try {
      rt->peer_ = RpcPeer::connect(
          host, port, [](const std::shared_ptr<RpcPeer>& p, Message&& m) { p->reply(m, clErrorProtocol); },
          "controller");
    } catch (const std::exception& e) {
      std::fprintf(stderr, "kudalite: cannot connect to controller %s: %s\n", endpoint.c_str(), e.what());
      return clErrorInitializationError;
    }
    char hostname[128] = {0};
    ::gethostname(hostname, sizeof hostname - 1);
    ByteWriter w;
    w.putString(std::string(hostname) + " pid " + std::to_string(::getpid()));
    Message reply = rt->peer_->callSync(MsgType::Hello, w.take());
    if (reply.status != clSuccess) return clErrorInitializationError;
    try {
      ByteReader r(reply.payload);
      rt->session_ = r.get<uint64_t>();
    } catch (const std::exception&) {
      return clErrorProtocol;
    }
    rt->defaultStream_ = std::make_shared<StreamState>(0);
    rt->streams_[0] = rt->defaultStream_;
    *out = std::move(rt);
    return clSuccess;
  }

  ~Runtime() {
    if (peer_) peer_->close();
  }

  std::shared_ptr<StreamState> stream(clStream_t s) const { return s ? s->state : defaultStream_; }

  /// Synchronous request outside any stream (malloc, free, queries).
  clError_t call(MsgType type, std::vector<uint8_t> payload, Message* reply) {
    Message m = peer_->callSync(type, std::move(payload));
    const clError_t st = m.status;
    if (reply) *reply = std::move(m);
    return st;
  }

  /// Asynchronous stream operation. `onDone` runs on the network thread when the controller
  /// answers (or the connection drops); it may downgrade m.status, e.g. on a short payload.
  void submit(const std::shared_ptr<StreamState>& s, MsgType type, std::vector<uint8_t> payload,
              std::function<void(Message&)> onDone = nullptr) {
    {
      std::lock_guard<std::mutex> lk(s->mu);
      ++s->inflight;
    }
    peer_->callAsync(type, std::move(payload), [s, onDone = std::move(onDone)](Message&& m) {
      if (onDone) onDone(m);
      std::lock_guard<std::mutex> lk(s->mu);
      if (m.status != clSuccess && s->sticky == clSuccess) s->sticky = m.status;
      if (--s->inflight == 0) s->cv.notify_all();
    });
  }

  clError_t sync(const std::shared_ptr<StreamState>& s) {
    std::unique_lock<std::mutex> lk(s->mu);
    s->cv.wait(lk, [&] { return s->inflight == 0; });
    const clError_t st = s->sticky;
    s->sticky = clSuccess;
    return st;
  }

  clError_t syncAll() {
    std::vector<std::shared_ptr<StreamState>> all;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (const auto& [id, s] : streams_) all.push_back(s);
    }
    clError_t first = clSuccess;
    for (const auto& s : all) {
      const clError_t st = sync(s);
      if (first == clSuccess) first = st;
    }
    return first;
  }

  std::shared_ptr<StreamState> newStream() {
    std::lock_guard<std::mutex> lk(mu_);
    auto s = std::make_shared<StreamState>(nextStream_++);
    streams_[s->id] = s;
    return s;
  }

  void dropStream(uint32_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    streams_.erase(id);
  }

  uint32_t newEventId() { return nextEvent_++; }

 private:
  Runtime() = default;

  std::shared_ptr<RpcPeer> peer_;
  uint64_t session_ = 0;
  std::mutex mu_;
  std::map<uint32_t, std::shared_ptr<StreamState>> streams_;
  std::shared_ptr<StreamState> defaultStream_;
  uint32_t nextStream_ = 1;
  std::atomic<uint32_t> nextEvent_{1};
};

std::mutex gMu;
std::shared_ptr<Runtime> gRuntime;
std::atomic<int> gLastError{clSuccess};

clError_t record(clError_t e) {
  if (e != clSuccess) gLastError.store(e);
  return e;
}

std::string resolveEndpoint(const char* controller) {
  if (controller != nullptr && *controller != '\0') return controller;
  const char* env = std::getenv("KUDALITE_CONTROLLER");
  if (env != nullptr && *env != '\0') return env;
  return "127.0.0.1:7070";
}

/// Returns the runtime, connecting with the default endpoint on first use.
std::shared_ptr<Runtime> runtime(clError_t* err) {
  std::lock_guard<std::mutex> lk(gMu);
  if (!gRuntime) {
    std::shared_ptr<Runtime> rt;
    *err = Runtime::create(resolveEndpoint(nullptr), &rt);
    if (*err != clSuccess) return nullptr;
    gRuntime = std::move(rt);
  }
  *err = clSuccess;
  return gRuntime;
}

#define CL_RUNTIME(rt)                    \
  clError_t rt##_err = clSuccess;         \
  auto rt = runtime(&rt##_err);           \
  if (!rt) return record(rt##_err)

void copyName(char* dst, size_t cap, const std::string& src) { std::snprintf(dst, cap, "%s", src.c_str()); }

}  // namespace

// ---------------------------------------------------------------------------------------------
// Initialisation and errors
// ---------------------------------------------------------------------------------------------

clError_t clInit(const char* controller) {
  std::lock_guard<std::mutex> lk(gMu);
  if (gRuntime) return clSuccess;
  std::shared_ptr<Runtime> rt;
  const clError_t st = Runtime::create(resolveEndpoint(controller), &rt);
  if (st == clSuccess) gRuntime = std::move(rt);
  return record(st);
}

clError_t clShutdown() {
  std::shared_ptr<Runtime> rt;
  {
    std::lock_guard<std::mutex> lk(gMu);
    rt = std::move(gRuntime);
  }
  if (!rt) return clSuccess;
  return record(rt->syncAll());  // the controller frees the session's memory on disconnect
}

clError_t clGetLastError() { return static_cast<clError_t>(gLastError.exchange(clSuccess)); }

// ---------------------------------------------------------------------------------------------
// Cluster information
// ---------------------------------------------------------------------------------------------

clError_t clGetDeviceProperties(clDeviceProp* prop) {
  if (prop == nullptr) return record(clErrorInvalidValue);
  CL_RUNTIME(rt);
  Message reply;
  const clError_t st = rt->call(MsgType::GetDeviceProps, {}, &reply);
  if (st != clSuccess) return record(st);
  try {
    ByteReader r(reply.payload);
    *prop = clDeviceProp{};
    copyName(prop->name, sizeof prop->name, r.getString());
    prop->workerCount = r.get<uint32_t>();
    prop->totalCores = r.get<uint32_t>();
    prop->totalGlobalMem = r.get<uint64_t>();
    prop->freeGlobalMem = r.get<uint64_t>();
    prop->pageSize = r.get<uint32_t>();
    prop->protocolVersion = r.get<uint32_t>();
  } catch (const std::exception&) {
    return record(clErrorProtocol);
  }
  return clSuccess;
}

clError_t clGetWorkerInfo(clWorkerInfo* infos, uint32_t capacity, uint32_t* count) {
  if (count == nullptr || (infos == nullptr && capacity > 0)) return record(clErrorInvalidValue);
  CL_RUNTIME(rt);
  Message reply;
  const clError_t st = rt->call(MsgType::GetWorkers, {}, &reply);
  if (st != clSuccess) return record(st);
  try {
    ByteReader r(reply.payload);
    const uint32_t n = r.get<uint32_t>();
    *count = n;
    for (uint32_t i = 0; i < n; ++i) {
      clWorkerInfo w{};
      w.id = r.get<uint32_t>();
      copyName(w.hostname, sizeof w.hostname, r.getString());
      copyName(w.address, sizeof w.address, r.getString());
      w.dataPort = r.get<uint16_t>();
      w.cores = r.get<uint32_t>();
      w.memBytes = r.get<uint64_t>();
      w.memUsed = r.get<uint64_t>();
      if (i < capacity) infos[i] = w;
    }
  } catch (const std::exception&) {
    return record(clErrorProtocol);
  }
  return clSuccess;
}

clError_t clGetTelemetry(clClusterTelemetry* cluster, clNodeTelemetry* nodes, uint32_t capacity, uint32_t* count) {
  if (count == nullptr || (nodes == nullptr && capacity > 0)) return record(clErrorInvalidValue);
  CL_RUNTIME(rt);
  ByteWriter req;
  req.put<uint32_t>(1);  // latest sample only
  Message reply;
  const clError_t st = rt->call(MsgType::GetTelemetry, req.take(), &reply);
  if (st != clSuccess) return record(st);
  try {
    ByteReader r(reply.payload);
    clClusterTelemetry c{};
    c.controllerUptimeMs = r.get<uint64_t>();
    c.sessions = r.get<uint32_t>();
    c.activeLaunches = r.get<uint32_t>();
    c.launchesTotal = r.get<uint64_t>();
    c.allocations = r.get<uint32_t>();
    c.allocatedBytes = r.get<uint64_t>();
    c.arenaTotalBytes = r.get<uint64_t>();
    if (cluster) *cluster = c;
    const uint32_t n = r.get<uint32_t>();
    *count = n;
    for (uint32_t i = 0; i < n; ++i) {
      clNodeTelemetry t{};
      t.id = r.get<uint32_t>();
      t.role = static_cast<clNodeRole>(r.get<uint8_t>());
      t.alive = r.get<uint8_t>();
      copyName(t.hostname, sizeof t.hostname, r.getString());
      copyName(t.address, sizeof t.address, r.getString());
      t.arenaUsedBytes = r.get<uint64_t>();
      t.lastSeenAgeMs = r.get<uint64_t>();
      const uint32_t samples = r.get<uint32_t>();
      cl::TelemetrySample latest;
      for (uint32_t k = 0; k < samples; ++k) latest = cl::TelemetrySample::decode(r);
      if (samples > 0) {
        t.hasSample = 1;
        t.timestampMs = latest.timestampMs;
        t.uptimeMs = latest.uptimeMs;
        t.memTotalBytes = latest.memTotalBytes;
        t.memAvailableBytes = latest.memAvailableBytes;
        t.rssBytes = latest.rssBytes;
        t.processThreads = latest.processThreads;
        t.arenaBytes = latest.arenaBytes;
        t.cacheBytes = latest.cacheBytes;
        t.cacheCapacityBytes = latest.cacheCapacityBytes;
        t.cacheHits = latest.cacheHits;
        t.cacheMisses = latest.cacheMisses;
        t.computeThreads = latest.computeThreads;
        t.busyThreads = latest.busyThreads;
        t.cpuCores = latest.cpuCores;
        t.cpuPercent = latest.cpuPercent;
        t.load1 = latest.load1;
        t.cpuTempC = latest.cpuTempC;
        t.cpuFreqMHz = latest.cpuFreqMHz;
        t.queueDepth = latest.queueDepth;
        t.executing = latest.executing;
        t.execsCompleted = latest.execsCompleted;
        t.blocksExecuted = latest.blocksExecuted;
        t.execBusyNs = latest.execBusyNs;
        t.netRxBytes = latest.netRxBytes;
        t.netTxBytes = latest.netTxBytes;
        copyName(t.currentKernel, sizeof t.currentKernel, latest.currentKernel);
      }
      if (i < capacity) nodes[i] = t;
    }
  } catch (const std::exception&) {
    return record(clErrorProtocol);
  }
  return clSuccess;
}

// ---------------------------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------------------------

clError_t clMallocEx(clDevPtr* ptr, size_t bytes, clDistribution dist, uint32_t pageSize) {
  if (ptr == nullptr || bytes == 0) return record(clErrorInvalidValue);
  CL_RUNTIME(rt);
  ByteWriter w;
  w.put<uint64_t>(bytes).put<uint32_t>(static_cast<uint32_t>(dist)).put<uint32_t>(pageSize);
  Message reply;
  const clError_t st = rt->call(MsgType::Malloc, w.take(), &reply);
  if (st != clSuccess) return record(st);
  try {
    ByteReader r(reply.payload);
    *ptr = r.get<uint64_t>();
  } catch (const std::exception&) {
    return record(clErrorProtocol);
  }
  return clSuccess;
}

clError_t clMalloc(clDevPtr* ptr, size_t bytes) { return clMallocEx(ptr, bytes, clDistStriped, 0); }

clError_t clFree(clDevPtr ptr) {
  if (ptr == 0) return clSuccess;
  CL_RUNTIME(rt);
  const clError_t pending = rt->syncAll();
  ByteWriter w;
  w.put<uint64_t>(ptr);
  const clError_t st = rt->call(MsgType::Free, w.take(), nullptr);
  return record(st != clSuccess ? st : pending);
}

clError_t clMemcpyHtoDAsync(clDevPtr dst, const void* src, size_t bytes, clStream_t stream) {
  if (bytes == 0) return clSuccess;
  if (src == nullptr) return record(clErrorInvalidValue);
  CL_RUNTIME(rt);
  auto s = rt->stream(stream);
  const auto* p = static_cast<const uint8_t*>(src);
  for (uint64_t off = 0; off < bytes; off += kCopyChunk) {
    const uint64_t n = std::min<uint64_t>(kCopyChunk, bytes - off);
    ByteWriter w(24 + n);
    w.put<uint32_t>(s->id).put<uint64_t>(dst + off).put<uint64_t>(n);
    w.putBytes(p + off, n);  // copied now, so the caller's buffer is free after this returns
    rt->submit(s, MsgType::MemcpyH2D, w.take());
  }
  return clSuccess;
}

clError_t clMemcpyDtoHAsync(void* dst, clDevPtr src, size_t bytes, clStream_t stream) {
  if (bytes == 0) return clSuccess;
  if (dst == nullptr) return record(clErrorInvalidValue);
  CL_RUNTIME(rt);
  auto s = rt->stream(stream);
  auto* p = static_cast<uint8_t*>(dst);
  for (uint64_t off = 0; off < bytes; off += kCopyChunk) {
    const uint64_t n = std::min<uint64_t>(kCopyChunk, bytes - off);
    ByteWriter w;
    w.put<uint32_t>(s->id).put<uint64_t>(src + off).put<uint64_t>(n);
    uint8_t* target = p + off;
    rt->submit(s, MsgType::MemcpyD2H, w.take(), [target, n](Message& m) {
      if (m.status != clSuccess) return;
      if (m.payload.size() != n) {
        m.status = clErrorProtocol;
        return;
      }
      std::memcpy(target, m.payload.data(), n);
    });
  }
  return clSuccess;
}

clError_t clMemcpyDtoDAsync(clDevPtr dst, clDevPtr src, size_t bytes, clStream_t stream) {
  if (bytes == 0) return clSuccess;
  CL_RUNTIME(rt);
  auto s = rt->stream(stream);
  ByteWriter w;
  w.put<uint32_t>(s->id).put<uint64_t>(dst).put<uint64_t>(src).put<uint64_t>(bytes);
  rt->submit(s, MsgType::MemcpyD2D, w.take());
  return clSuccess;
}

clError_t clMemsetAsync(clDevPtr dst, int value, size_t bytes, clStream_t stream) {
  if (bytes == 0) return clSuccess;
  CL_RUNTIME(rt);
  auto s = rt->stream(stream);
  ByteWriter w;
  w.put<uint32_t>(s->id).put<uint64_t>(dst).put<uint8_t>(static_cast<uint8_t>(value)).put<uint64_t>(bytes);
  rt->submit(s, MsgType::Memset, w.take());
  return clSuccess;
}

clError_t clMemcpyHtoD(clDevPtr dst, const void* src, size_t bytes) {
  clError_t st = clMemcpyHtoDAsync(dst, src, bytes, nullptr);
  if (st != clSuccess) return st;
  return clStreamSynchronize(nullptr);
}

clError_t clMemcpyDtoH(void* dst, clDevPtr src, size_t bytes) {
  clError_t st = clMemcpyDtoHAsync(dst, src, bytes, nullptr);
  if (st != clSuccess) return st;
  return clStreamSynchronize(nullptr);
}

clError_t clMemcpyDtoD(clDevPtr dst, clDevPtr src, size_t bytes) {
  clError_t st = clMemcpyDtoDAsync(dst, src, bytes, nullptr);
  if (st != clSuccess) return st;
  return clStreamSynchronize(nullptr);
}

clError_t clMemset(clDevPtr dst, int value, size_t bytes) {
  clError_t st = clMemsetAsync(dst, value, bytes, nullptr);
  if (st != clSuccess) return st;
  return clStreamSynchronize(nullptr);
}

// ---------------------------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------------------------

clError_t clStreamCreate(clStream_t* stream) {
  if (stream == nullptr) return record(clErrorInvalidValue);
  CL_RUNTIME(rt);
  *stream = new clStream_st{rt->newStream()};
  return clSuccess;
}

clError_t clStreamDestroy(clStream_t stream) {
  if (stream == nullptr) return record(clErrorInvalidResourceHandle);
  CL_RUNTIME(rt);
  const clError_t pending = rt->sync(stream->state);
  ByteWriter w;
  w.put<uint32_t>(stream->state->id);
  rt->call(MsgType::StreamDestroy, w.take(), nullptr);
  rt->dropStream(stream->state->id);
  delete stream;
  return record(pending);
}

clError_t clStreamSynchronize(clStream_t stream) {
  CL_RUNTIME(rt);
  return record(rt->sync(rt->stream(stream)));
}

clError_t clStreamQuery(clStream_t stream) {
  CL_RUNTIME(rt);
  auto s = rt->stream(stream);
  std::lock_guard<std::mutex> lk(s->mu);
  return s->inflight == 0 ? clSuccess : clErrorNotReady;
}

clError_t clStreamWaitEvent(clStream_t stream, clEvent_t event) {
  if (event == nullptr) return record(clErrorInvalidResourceHandle);
  CL_RUNTIME(rt);
  auto s = rt->stream(stream);
  ByteWriter w;
  w.put<uint32_t>(s->id).put<uint32_t>(event->id);
  rt->submit(s, MsgType::StreamWaitEvent, w.take());
  return clSuccess;
}

// ---------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------

clError_t clEventCreate(clEvent_t* event) {
  if (event == nullptr) return record(clErrorInvalidValue);
  CL_RUNTIME(rt);
  *event = new clEvent_st{rt->newEventId(), std::make_shared<EventState>()};
  return clSuccess;
}

clError_t clEventDestroy(clEvent_t event) {
  if (event == nullptr) return record(clErrorInvalidResourceHandle);
  delete event;  // an in-flight record keeps the shared state alive
  return clSuccess;
}

clError_t clEventRecord(clEvent_t event, clStream_t stream) {
  if (event == nullptr) return record(clErrorInvalidResourceHandle);
  CL_RUNTIME(rt);
  auto s = rt->stream(stream);
  auto ev = event->state;
  {
    std::lock_guard<std::mutex> lk(ev->mu);
    ev->recorded = true;
    ev->pending = true;
  }
  ByteWriter w;
  w.put<uint32_t>(s->id).put<uint32_t>(event->id);
  rt->submit(s, MsgType::EventRecord, w.take(), [ev](Message& m) {
    std::lock_guard<std::mutex> lk(ev->mu);
    ev->status = m.status;
    if (m.status == clSuccess) {
      try {
        ByteReader r(m.payload);
        ev->tsNs = r.get<uint64_t>();
      } catch (const std::exception&) {
        ev->status = m.status = clErrorProtocol;
      }
    }
    ev->pending = false;
    ev->cv.notify_all();
  });
  return clSuccess;
}

clError_t clEventSynchronize(clEvent_t event) {
  if (event == nullptr) return record(clErrorInvalidResourceHandle);
  auto ev = event->state;
  std::unique_lock<std::mutex> lk(ev->mu);
  ev->cv.wait(lk, [&] { return !ev->pending; });
  return record(ev->status);
}

clError_t clEventQuery(clEvent_t event) {
  if (event == nullptr) return record(clErrorInvalidResourceHandle);
  auto ev = event->state;
  std::lock_guard<std::mutex> lk(ev->mu);
  return ev->pending ? clErrorNotReady : ev->status;
}

clError_t clEventElapsedTime(float* ms, clEvent_t start, clEvent_t end) {
  if (ms == nullptr) return record(clErrorInvalidValue);
  if (start == nullptr || end == nullptr) return record(clErrorInvalidResourceHandle);
  uint64_t ts[2] = {0, 0};
  EventState* const events[2] = {start->state.get(), end->state.get()};
  for (int i = 0; i < 2; ++i) {
    std::lock_guard<std::mutex> lk(events[i]->mu);
    if (!events[i]->recorded) return record(clErrorInvalidResourceHandle);
    if (events[i]->pending) return clErrorNotReady;
    if (events[i]->status != clSuccess) return record(events[i]->status);
    ts[i] = events[i]->tsNs;
  }
  *ms = static_cast<float>((static_cast<double>(ts[1]) - static_cast<double>(ts[0])) / 1e6);
  return clSuccess;
}

// ---------------------------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------------------------

clError_t clLaunchKernel(const char* kernel, clDim3 grid, clDim3 block, const void* args, size_t argBytes,
                         clStream_t stream) {
  if (kernel == nullptr || *kernel == '\0' || grid.count() == 0 || block.count() == 0 ||
      (args == nullptr && argBytes != 0) || argBytes > 0xFFFFFFFFu) {
    return record(clErrorInvalidValue);
  }
  CL_RUNTIME(rt);
  auto s = rt->stream(stream);
  ByteWriter w;
  w.put<uint32_t>(s->id);
  w.putString(kernel);
  cl::putDim3(w, grid);
  cl::putDim3(w, block);
  w.putBlob(args, argBytes);
  rt->submit(s, MsgType::Launch, w.take(), [s](Message& m) {
    if (m.status != clSuccess) return;
    try {
      ByteReader r(m.payload);
      clLaunchInfo info{};
      info.blocks = r.get<uint64_t>();
      info.workersUsed = r.get<uint32_t>();
      info.wallMs = r.get<double>();
      std::lock_guard<std::mutex> lk(s->mu);
      s->lastLaunch = info;
    } catch (const std::exception&) {
      m.status = clErrorProtocol;
    }
  });
  return clSuccess;
}

clError_t clDeviceSynchronize() {
  CL_RUNTIME(rt);
  return record(rt->syncAll());
}

clError_t clGetLastLaunchInfo(clStream_t stream, clLaunchInfo* info) {
  if (info == nullptr) return record(clErrorInvalidValue);
  CL_RUNTIME(rt);
  auto s = rt->stream(stream);
  std::lock_guard<std::mutex> lk(s->mu);
  *info = s->lastLaunch;
  return clSuccess;
}
