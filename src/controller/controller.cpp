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

#include "controller/controller.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <thread>

#include <unistd.h>

#include "common/log.h"
#include "kudalite/kudalite.h"

namespace cl {
namespace {

constexpr uint64_t kGvaAlign = 1ull << 20;    // allocations start on 1 MiB boundaries
constexpr uint64_t kCopyChunk = 16ull << 20;  // device-to-device staging buffer

uint64_t nowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

uint64_t roundUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

}  // namespace

Controller::Controller(ControllerOptions opts) : opts_(std::move(opts)), dsm_(map_, "controller") {}

int Controller::run() {
  Listener workerListener(opts_.bindHost, opts_.workerPort);
  Listener hostListener(opts_.bindHost, opts_.hostPort);
  LOG_INFO("listening for hosts on " << opts_.bindHost << ":" << hostListener.port() << ", workers on "
                                     << opts_.bindHost << ":" << workerListener.port() << " (page size "
                                     << opts_.pageSize << ")");
  std::thread workers([this, &workerListener] { acceptLoop(workerListener, false); });
  if (opts_.telemetryMs > 0) std::thread([this] { telemetryLoop(); }).detach();
  acceptLoop(hostListener, true);
  workers.join();
  return 1;
}

void Controller::acceptLoop(Listener& listener, bool hosts) {
  for (;;) {
    try {
      Socket s = listener.accept();
      if (hosts) {
        onHostConnect(std::move(s));
      } else {
        onWorkerConnect(std::move(s));
      }
    } catch (const std::exception& e) {
      LOG_ERROR("accept failed: " << e.what());
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

// =============================================================================================
// Workers
// =============================================================================================

void Controller::onWorkerConnect(Socket sock) {
  const std::string name = "worker@" + sock.peerName();
  auto idSlot = std::make_shared<std::atomic<uint32_t>>(kNoWorker);
  RpcPeer::start(
      std::move(sock),
      [this, idSlot](const std::shared_ptr<RpcPeer>& p, Message&& m) { onWorkerMessage(p, std::move(m), idSlot); },
      name,
      [this, idSlot] {
        if (idSlot->load() != kNoWorker) onWorkerClosed(idSlot->load());
      });
}

void Controller::onWorkerMessage(const std::shared_ptr<RpcPeer>& peer, Message&& m,
                                 const std::shared_ptr<std::atomic<uint32_t>>& idSlot) {
  switch (m.type) {
    case MsgType::Register:
      if (idSlot->load() != kNoWorker) {
        peer->reply(m, clErrorProtocol);  // already registered on this connection
        return;
      }
      registerWorker(peer, m, idSlot);
      return;
    case MsgType::Telemetry: {
      const uint32_t id = idSlot->load();
      if (id == kNoWorker) {
        peer->reply(m, clErrorProtocol);  // must register first
        return;
      }
      ByteReader r(m.payload);
      recordTelemetry(id, TelemetrySample::decode(r));
      peer->reply(m, clSuccess);
      return;
    }
    case MsgType::Ping:
      peer->reply(m, clSuccess);
      return;
    default:
      peer->reply(m, clErrorProtocol);
  }
}

void Controller::registerWorker(const std::shared_ptr<RpcPeer>& peer, const Message& m,
                                const std::shared_ptr<std::atomic<uint32_t>>& idSlot) {
  ByteReader r(m.payload);
  auto rec = std::make_shared<WorkerRec>();
  rec->hostname = r.getString();
  rec->host = r.getString();
  rec->dataPort = r.get<uint16_t>();
  rec->memBytes = r.get<uint64_t>();
  rec->cores = r.get<uint32_t>();
  const uint32_t nk = r.get<uint32_t>();
  if (nk > r.remaining()) throw ProtocolError("bad kernel count");
  for (uint32_t i = 0; i < nk; ++i) rec->kernels.push_back(r.getString());
  rec->peer = peer;
  rec->arena = FreeListAllocator(rec->memBytes);

  {
    // Holding memMu_ serialises with allocate()/release(): the new worker receives every
    // allocation that exists before it, and every later one via the normal broadcast.
    std::lock_guard<std::mutex> mem(memMu_);
    {
      std::lock_guard<std::mutex> lk(workersMu_);
      rec->id = nextWorkerId_++;
      workers_[rec->id] = rec;
    }
    idSlot->store(rec->id);
    ByteWriter w;
    w.put<uint32_t>(rec->id);
    peer->reply(m, clSuccess, w.take());
    for (const auto& a : map_.snapshot()) {
      ByteWriter aw;
      a->encode(aw);
      peer->callAsync(MsgType::AllocAdd, aw.take(), nullptr);
    }
  }
  LOG_INFO("worker " << rec->id << " joined: " << rec->hostname << " data=" << rec->host << ":" << rec->dataPort
                     << " mem=" << (rec->memBytes >> 20) << " MiB cores=" << rec->cores << " kernels="
                     << rec->kernels.size());
  broadcastClusterMap();
}

void Controller::onWorkerClosed(uint32_t id) {
  {
    std::lock_guard<std::mutex> lk(workersMu_);
    auto it = workers_.find(id);
    if (it == workers_.end()) return;
    it->second->alive = false;
  }
  LOG_ERROR("worker " << id << " disconnected: allocations with pages on it are lost; "
                      << "launches and copies touching them will fail");
  broadcastClusterMap();
}

std::vector<std::shared_ptr<Controller::WorkerRec>> Controller::aliveWorkers() {
  std::lock_guard<std::mutex> lk(workersMu_);
  std::vector<std::shared_ptr<WorkerRec>> out;
  for (const auto& [id, w] : workers_) {
    if (w->alive) out.push_back(w);
  }
  return out;
}

void Controller::broadcastClusterMap() {
  std::lock_guard<std::mutex> order(broadcastMu_);
  std::vector<PeerAddr> peers;
  std::vector<std::shared_ptr<RpcPeer>> targets;
  for (const auto& w : aliveWorkers()) {
    peers.push_back(PeerAddr{w->id, w->host, w->dataPort});
    targets.push_back(w->peer);
  }
  dsm_.setPeers(peers);
  ByteWriter w;
  encodeClusterMap(w, peers);
  const auto payload = w.take();
  for (const auto& t : targets) t->callAsync(MsgType::ClusterMap, payload, nullptr);
}

// =============================================================================================
// Memory
// =============================================================================================

clError_t Controller::allocate(uint64_t size, uint32_t dist, uint32_t pageSize, clDevPtr* out) {
  if (size == 0) return clErrorInvalidValue;
  std::lock_guard<std::mutex> mem(memMu_);
  const auto workers = aliveWorkers();
  if (workers.empty()) return clErrorNoWorkers;
  const size_t n = workers.size();

  Allocation a;
  a.size = size;
  if (dist == clDistBlocked) {
    a.pageSize = roundUp((size + n - 1) / n, 4096);
  } else if (dist == clDistStriped) {
    a.pageSize = pageSize != 0 ? pageSize : opts_.pageSize;
  } else {
    return clErrorInvalidValue;
  }
  if (a.pageSize < 64 || a.pageSize % 64 != 0) return clErrorInvalidValue;

  // Rotate the first owner so that many small allocations do not all start on worker 0.
  const size_t first = rotate_++ % n;
  for (size_t j = 0; j < n; ++j) a.owners.push_back(workers[(first + j) % n]->id);
  a.localBase.assign(n, 0);
  for (size_t j = 0; j < n; ++j) {
    const uint64_t bytes = a.localBytesOnOwner(j);
    if (bytes == 0) continue;
    WorkerRec& w = *workers[(first + j) % n];
    if (!w.arena.allocate(bytes, &a.localBase[j])) {
      LOG_WARN("allocation of " << size << " bytes failed: worker " << w.id << " has no " << bytes
                                << "-byte hole (largest " << w.arena.largestFree() << ")");
      for (size_t k = 0; k < j; ++k) {  // undo the slices placed so far
        if (a.localBytesOnOwner(k) != 0) workers[(first + k) % n]->arena.release(a.localBase[k]);
      }
      return clErrorMemoryAllocation;
    }
  }
  a.base = nextGva_;
  nextGva_ += roundUp(size, kGvaAlign) + kGvaAlign;  // unmapped gap catches overruns

  // Every worker must be able to translate the new range before the host can use it.
  ByteWriter w;
  a.encode(w);
  const auto payload = w.take();
  std::vector<std::future<Message>> acks;
  for (const auto& wk : workers) acks.push_back(wk->peer->call(MsgType::AllocAdd, payload));
  clError_t st = clSuccess;
  for (auto& f : acks) {
    const Message m = f.get();
    if (m.status != clSuccess) st = clErrorWorkerLost;
  }
  if (st != clSuccess) {
    releaseSlicesLocked(a);
    ByteWriter rw;
    rw.put<uint64_t>(a.base);
    const auto removal = rw.take();
    for (const auto& wk : workers) wk->peer->callAsync(MsgType::AllocRemove, removal, nullptr);
    return st;
  }
  *out = a.base;
  LOG_DEBUG("alloc 0x" << std::hex << a.base << std::dec << " size=" << size << " page=" << a.pageSize << " over "
                       << n << " worker(s)");
  map_.add(std::move(a));
  return clSuccess;
}

void Controller::releaseSlicesLocked(const Allocation& a) {
  std::lock_guard<std::mutex> lk(workersMu_);
  for (size_t j = 0; j < a.owners.size(); ++j) {
    if (a.localBytesOnOwner(j) == 0) continue;
    auto it = workers_.find(a.owners[j]);
    if (it != workers_.end()) it->second->arena.release(a.localBase[j]);
  }
}

clError_t Controller::release(clDevPtr base) {
  std::lock_guard<std::mutex> mem(memMu_);
  auto a = map_.remove(base);
  if (!a) return clErrorInvalidDevicePointer;
  releaseSlicesLocked(*a);
  ByteWriter w;
  w.put<uint64_t>(base);
  const auto payload = w.take();
  std::vector<std::future<Message>> acks;
  for (const auto& wk : aliveWorkers()) acks.push_back(wk->peer->call(MsgType::AllocRemove, payload));
  for (auto& f : acks) f.wait();
  return clSuccess;
}

clError_t Controller::copyDeviceToDevice(clDevPtr dst, clDevPtr src, uint64_t len) {
  std::vector<uint8_t> buf(static_cast<size_t>(std::min(len, kCopyChunk)));
  for (uint64_t off = 0; off < len; off += kCopyChunk) {
    const uint64_t n = std::min(kCopyChunk, len - off);
    clError_t st = dsm_.read(src + off, buf.data(), n);
    if (st == clSuccess) st = dsm_.write(dst + off, buf.data(), n);
    if (st != clSuccess) return st;
  }
  return clSuccess;
}

// =============================================================================================
// Execution
// =============================================================================================

clError_t Controller::runLaunch(const std::string& kernel, clDim3 grid, clDim3 block, const std::vector<uint8_t>& args,
                                const std::atomic<bool>& cancelled, LaunchResult* result) {
  const uint64_t total = grid.count();
  if (total == 0 || block.count() == 0 || kernel.empty()) return clErrorInvalidValue;
  const auto workers = aliveWorkers();
  if (workers.empty()) return clErrorNoWorkers;

  const uint64_t launchId = nextLaunch_++;
  ++launchesTotal_;
  ++activeLaunches_;
  struct ActiveGuard {
    std::atomic<uint32_t>& n;
    ~ActiveGuard() { --n; }
  } activeGuard{activeLaunches_};
  // Dynamic self-scheduling: each worker repeatedly takes the next chunk of consecutive blocks.
  // Several chunks per worker balance uneven speeds; consecutive blocks share input panels.
  const uint64_t chunk = std::max<uint64_t>(1, total / (workers.size() * std::max<uint32_t>(1, opts_.chunkFactor)));
  std::atomic<uint64_t> next{0};
  std::atomic<int> error{clSuccess};
  std::atomic<uint32_t> used{0};
  const auto t0 = std::chrono::steady_clock::now();

  std::vector<std::thread> drivers;
  drivers.reserve(workers.size());
  for (const auto& w : workers) {
    drivers.emplace_back([&, w] {
      bool any = false;
      for (;;) {
        if (error.load() != clSuccess) break;
        if (cancelled.load()) {  // the host went away: nobody is waiting for the result
          int expected = clSuccess;
          error.compare_exchange_strong(expected, clErrorNetwork);
          break;
        }
        const uint64_t begin = next.fetch_add(chunk);
        if (begin >= total) break;
        ExecBlocksRequest req;
        req.launchId = launchId;
        req.kernel = kernel;
        req.grid = grid;
        req.block = block;
        req.args = args;
        req.blockBegin = begin;
        req.blockEnd = std::min(total, begin + chunk);
        ByteWriter bw;
        req.encode(bw);
        const Message m = w->peer->callSync(MsgType::ExecBlocks, bw.take());
        if (m.status != clSuccess) {
          int expected = clSuccess;
          error.compare_exchange_strong(expected, m.status == clErrorNetwork ? clErrorWorkerLost : m.status);
          LOG_WARN("launch " << launchId << " '" << kernel << "' failed on worker " << w->id << ": "
                             << clGetErrorString(m.status));
          break;
        }
        any = true;
      }
      if (any) ++used;
    });
  }
  for (auto& t : drivers) t.join();

  result->blocks = total;
  result->workersUsed = used.load();
  result->wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  LOG_DEBUG("launch " << launchId << " '" << kernel << "' " << total << " blocks on " << result->workersUsed
                      << " worker(s) in " << result->wallMs << " ms");
  return static_cast<clError_t>(error.load());
}

// =============================================================================================
// Host sessions
// =============================================================================================

void Controller::onHostConnect(Socket sock) {
  auto session = std::make_shared<Session>();
  session->id = nextSession_++;
  {
    std::lock_guard<std::mutex> lk(sessionsMu_);
    sessions_[session->id] = session;
  }
  const uint64_t id = session->id;
  std::weak_ptr<Session> weak = session;
  RpcPeer::start(
      std::move(sock),
      [this, weak](const std::shared_ptr<RpcPeer>& p, Message&& m) {
        auto s = weak.lock();
        if (!s) {
          p->reply(m, clErrorNetwork);
          return;
        }
        onHostMessage(s, p, std::move(m));
      },
      "host#" + std::to_string(id), [this, id] { closeSession(id); });
}

void Controller::enqueue(const std::shared_ptr<Session>& s, uint32_t stream, std::function<void()> op) {
  std::lock_guard<std::mutex> lk(s->mu);
  if (s->closing) return;
  auto& q = s->streams[stream];
  if (!q) q = std::make_unique<StreamQueue>();
  q->push(std::move(op));
}

void Controller::onHostMessage(const std::shared_ptr<Session>& s, const std::shared_ptr<RpcPeer>& peer, Message&& m) {
  ByteReader r(m.payload);
  switch (m.type) {
    case MsgType::Hello: {
      const std::string client = r.getString();
      LOG_INFO("host session " << s->id << " opened by " << client << " (" << peer->name() << ")");
      ByteWriter w;
      w.put<uint64_t>(s->id);
      peer->reply(m, clSuccess, w.take());
      return;
    }
    case MsgType::GetDeviceProps: {
      uint64_t total = 0, used = 0;
      uint32_t cores = 0, count = 0;
      {
        std::lock_guard<std::mutex> mem(memMu_);
        for (const auto& w : aliveWorkers()) {
          total += w->arena.capacity();
          used += w->arena.used();
          cores += w->cores;
          ++count;
        }
      }
      ByteWriter w;
      w.putString("KUDA-Lite Raspberry Pi cluster");
      w.put<uint32_t>(count).put<uint32_t>(cores).put<uint64_t>(total).put<uint64_t>(total - used);
      w.put<uint32_t>(opts_.pageSize).put<uint32_t>(kProtocolVersion);
      peer->reply(m, clSuccess, w.take());
      return;
    }
    case MsgType::GetWorkers: {
      ByteWriter w;
      std::lock_guard<std::mutex> mem(memMu_);
      const auto workers = aliveWorkers();
      w.put<uint32_t>(static_cast<uint32_t>(workers.size()));
      for (const auto& wk : workers) {
        w.put<uint32_t>(wk->id);
        w.putString(wk->hostname);
        w.putString(wk->host);
        w.put<uint16_t>(wk->dataPort);
        w.put<uint32_t>(wk->cores);
        w.put<uint64_t>(wk->arena.capacity());
        w.put<uint64_t>(wk->arena.used());
      }
      peer->reply(m, clSuccess, w.take());
      return;
    }
    case MsgType::Malloc: {
      const uint64_t size = r.get<uint64_t>();
      const uint32_t dist = r.get<uint32_t>();
      const uint32_t pageSize = r.get<uint32_t>();
      clDevPtr ptr = 0;
      const clError_t st = allocate(size, dist, pageSize, &ptr);
      if (st == clSuccess) {
        std::lock_guard<std::mutex> lk(s->mu);
        s->allocations.insert(ptr);
      }
      ByteWriter w;
      w.put<uint64_t>(ptr);
      peer->reply(m, st, w.take());
      return;
    }
    case MsgType::Free: {
      const clDevPtr ptr = r.get<uint64_t>();
      bool owned = false;
      {
        std::lock_guard<std::mutex> lk(s->mu);
        owned = s->allocations.erase(ptr) != 0;
      }
      peer->reply(m, owned ? release(ptr) : clErrorInvalidDevicePointer);
      return;
    }
    case MsgType::StreamDestroy: {
      const uint32_t stream = r.get<uint32_t>();
      std::unique_ptr<StreamQueue> q;
      {
        std::lock_guard<std::mutex> lk(s->mu);
        auto it = s->streams.find(stream);
        if (it != s->streams.end()) {
          q = std::move(it->second);
          s->streams.erase(it);
        }
      }
      q.reset();  // joins the stream thread (the host synchronised it first)
      peer->reply(m, clSuccess);
      return;
    }
    case MsgType::EventRecord: {
      const uint32_t stream = r.get<uint32_t>();
      const uint32_t event = r.get<uint32_t>();
      uint64_t generation = 0;
      {
        std::lock_guard<std::mutex> lk(s->mu);
        generation = ++s->events[event].recorded;
      }
      const uint64_t reqId = m.requestId;
      enqueue(s, stream, [s, peer, reqId, event, generation] {
        const uint64_t ts = nowNs();
        {
          std::lock_guard<std::mutex> lk(s->mu);
          EventRec& e = s->events[event];
          e.completed = std::max(e.completed, generation);
          e.tsNs = ts;
        }
        s->cv.notify_all();
        ByteWriter w;
        w.put<uint64_t>(ts);
        peer->reply(reqId, MsgType::EventRecord, clSuccess, w.take());
      });
      return;
    }
    case MsgType::StreamWaitEvent: {
      const uint32_t stream = r.get<uint32_t>();
      const uint32_t event = r.get<uint32_t>();
      uint64_t target = 0;  // waits for the record issued most recently *before* this call
      {
        std::lock_guard<std::mutex> lk(s->mu);
        auto it = s->events.find(event);
        if (it != s->events.end()) target = it->second.recorded;
      }
      const uint64_t reqId = m.requestId;
      enqueue(s, stream, [s, peer, reqId, event, target] {
        std::unique_lock<std::mutex> lk(s->mu);
        s->cv.wait(lk, [&] { return s->closing || s->events[event].completed >= target; });
        const bool closing = s->closing;
        lk.unlock();
        peer->reply(reqId, MsgType::StreamWaitEvent, closing ? clErrorNetwork : clSuccess);
      });
      return;
    }
    case MsgType::GetTelemetry: {
      const uint32_t maxHistory = r.get<uint32_t>();
      peer->reply(m, clSuccess, encodeTelemetry(maxHistory));
      return;
    }
    case MsgType::MemcpyH2D:
    case MsgType::MemcpyD2H:
    case MsgType::MemcpyD2D:
    case MsgType::Memset:
    case MsgType::Launch: {
      const uint32_t stream = r.get<uint32_t>();
      auto msg = std::make_shared<Message>(std::move(m));
      enqueue(s, stream, [this, s, peer, msg] { executeStreamOp(s, peer, *msg); });
      return;
    }
    case MsgType::Ping:
      peer->reply(m, clSuccess);
      return;
    default:
      peer->reply(m, clErrorProtocol);
  }
}

void Controller::executeStreamOp(const std::shared_ptr<Session>& s, const std::shared_ptr<RpcPeer>& peer,
                                 const Message& m) {
  clError_t st = clSuccess;
  ByteWriter out;
  try {
    ByteReader r(m.payload);
    r.get<uint32_t>();  // stream id, already used for routing
    switch (m.type) {
      case MsgType::MemcpyH2D: {
        const clDevPtr dst = r.get<uint64_t>();
        const uint64_t len = r.get<uint64_t>();
        const uint8_t* data = r.view(len);
        st = dsm_.write(dst, data, len);
        break;
      }
      case MsgType::MemcpyD2H: {
        const clDevPtr src = r.get<uint64_t>();
        const uint64_t len = r.get<uint64_t>();
        if (len > kMaxPayloadBytes) {
          st = clErrorInvalidValue;
          break;
        }
        st = dsm_.read(src, out.append(len), len);
        if (st != clSuccess) out = ByteWriter();
        break;
      }
      case MsgType::MemcpyD2D: {
        const clDevPtr dst = r.get<uint64_t>();
        const clDevPtr src = r.get<uint64_t>();
        const uint64_t len = r.get<uint64_t>();
        st = copyDeviceToDevice(dst, src, len);
        break;
      }
      case MsgType::Memset: {
        const clDevPtr dst = r.get<uint64_t>();
        const uint8_t value = r.get<uint8_t>();
        const uint64_t len = r.get<uint64_t>();
        st = dsm_.fill(dst, value, len);
        break;
      }
      case MsgType::Launch: {
        const std::string kernel = r.getString();
        const clDim3 grid = getDim3(r);
        const clDim3 block = getDim3(r);
        const std::vector<uint8_t> args = r.getBlob();
        LaunchResult res;
        st = runLaunch(kernel, grid, block, args, s->cancelled, &res);
        out.put<uint64_t>(res.blocks).put<uint32_t>(res.workersUsed).put<double>(res.wallMs);
        break;
      }
      default:
        st = clErrorProtocol;
    }
  } catch (const ProtocolError& e) {
    LOG_WARN("malformed " << msgTypeName(m.type) << ": " << e.what());
    st = clErrorProtocol;
  }
  peer->reply(m.requestId, m.type, st, out.take());
}

// =============================================================================================
// Telemetry
// =============================================================================================

void Controller::recordTelemetry(uint32_t node, TelemetrySample sample) {
  std::lock_guard<std::mutex> lk(telemetryMu_);
  NodeTelemetry& t = telemetry_[node];
  t.samples.push_back(std::move(sample));
  while (t.samples.size() > std::max<uint32_t>(1, opts_.telemetryHistory)) t.samples.pop_front();
  t.lastSeenMs = steadyMs();
}

void Controller::telemetryLoop() {
  SystemSampler sampler;
  for (;;) {
    TelemetrySample t;
    sampler.sample(&t);
    t.netRxBytes = RpcPeer::totalBytesReceived();
    t.netTxBytes = RpcPeer::totalBytesSent();
    t.executing = activeLaunches_.load();  // on the controller: launches in progress
    recordTelemetry(kControllerNodeId, std::move(t));
    std::this_thread::sleep_for(std::chrono::milliseconds(opts_.telemetryMs));
  }
}

std::vector<uint8_t> Controller::encodeTelemetry(uint32_t maxHistory) {
  maxHistory = std::max<uint32_t>(1, maxHistory);
  struct NodeRow {
    uint32_t id;
    uint8_t role;  // clNodeRole
    uint8_t alive;
    std::string hostname;
    std::string address;
    uint64_t arenaUsed;
  };
  std::vector<NodeRow> rows;
  uint64_t arenaTotal = 0;
  {
    char name[256] = {0};
    ::gethostname(name, sizeof name - 1);
    rows.push_back(NodeRow{kControllerNodeId, 1, 1, name,
                           opts_.bindHost + ":" + std::to_string(opts_.hostPort), 0});
    std::lock_guard<std::mutex> mem(memMu_);
    std::lock_guard<std::mutex> lk(workersMu_);
    for (const auto& [id, w] : workers_) {
      rows.push_back(NodeRow{id, 0, static_cast<uint8_t>(w->alive ? 1 : 0), w->hostname,
                             w->host + ":" + std::to_string(w->dataPort), w->arena.used()});
      if (w->alive) arenaTotal += w->arena.capacity();
    }
  }
  const auto allocs = map_.snapshot();
  uint64_t allocated = 0;
  for (const auto& a : allocs) allocated += a->size;
  uint32_t sessions = 0;
  {
    std::lock_guard<std::mutex> lk(sessionsMu_);
    sessions = static_cast<uint32_t>(sessions_.size());
  }

  ByteWriter w(4096);
  w.put<uint64_t>(steadyMs() - startMs_);
  w.put<uint32_t>(sessions).put<uint32_t>(activeLaunches_.load()).put<uint64_t>(launchesTotal_.load());
  w.put<uint32_t>(static_cast<uint32_t>(allocs.size())).put<uint64_t>(allocated).put<uint64_t>(arenaTotal);
  w.put<uint32_t>(static_cast<uint32_t>(rows.size()));
  const uint64_t now = steadyMs();
  std::lock_guard<std::mutex> lk(telemetryMu_);
  for (const auto& row : rows) {
    w.put<uint32_t>(row.id).put<uint8_t>(row.role).put<uint8_t>(row.alive);
    w.putString(row.hostname).putString(row.address);
    w.put<uint64_t>(row.arenaUsed);
    auto it = telemetry_.find(row.id);
    if (it == telemetry_.end() || it->second.samples.empty()) {
      w.put<uint64_t>(UINT64_MAX).put<uint32_t>(0);  // never reported
      continue;
    }
    const auto& samples = it->second.samples;
    const size_t n = std::min<size_t>(maxHistory, samples.size());
    w.put<uint64_t>(now - it->second.lastSeenMs).put<uint32_t>(static_cast<uint32_t>(n));
    for (size_t i = samples.size() - n; i < samples.size(); ++i) samples[i].encode(w);
  }
  return w.take();
}

void Controller::closeSession(uint64_t id) {
  std::shared_ptr<Session> s;
  {
    std::lock_guard<std::mutex> lk(sessionsMu_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    s = std::move(it->second);
    sessions_.erase(it);
  }
  std::map<uint32_t, std::unique_ptr<StreamQueue>> streams;
  std::set<clDevPtr> allocations;
  {
    std::lock_guard<std::mutex> lk(s->mu);
    s->closing = true;
    s->cancelled = true;
    streams.swap(s->streams);
    allocations.swap(s->allocations);
  }
  s->cv.notify_all();  // release stream threads blocked in StreamWaitEvent
  streams.clear();     // joins each stream thread after its current operation
  for (clDevPtr p : allocations) release(p);
  LOG_INFO("host session " << id << " closed; released " << allocations.size() << " allocation(s)");
}

}  // namespace cl
