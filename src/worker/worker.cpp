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

#include "worker/worker.h"

#include <unistd.h>

#include <chrono>
#include <cstring>
#include <future>
#include <thread>
#include <utility>

#include "common/log.h"
#include "common/telemetry.h"
#include "kudalite/kudalite.h"

namespace cl {
namespace {

std::string hostName() {
  char buf[256] = {0};
  if (::gethostname(buf, sizeof buf - 1) != 0) return "unknown";
  return buf;
}

/// Parses the (offset, len) list shared by the data-plane requests and bounds-checks it.
bool readExtents(ByteReader& r, const Arena& arena, std::vector<std::pair<uint64_t, uint64_t>>* out,
                 uint64_t* total) {
  const uint32_t count = r.get<uint32_t>();
  if (uint64_t(count) * 16 > r.remaining()) throw ProtocolError("extent list truncated");
  out->resize(count);
  *total = 0;
  bool ok = true;
  for (auto& [off, len] : *out) {
    off = r.get<uint64_t>();
    len = r.get<uint64_t>();
    if (!arena.contains(off, len)) ok = false;
    *total += len;
  }
  return ok;
}

}  // namespace

Worker::Worker(WorkerOptions opts) : opts_(std::move(opts)), dsm_(map_, "worker") {}

int Worker::run() {
  arena_ = std::make_unique<Arena>(opts_.memBytes);
  dsm_.setLocalArena(arena_->data(), arena_->size());
  pool_ = std::make_unique<ThreadPool>(opts_.threads);
  cache_ = std::make_unique<PageCache>(dsm_, opts_.cacheBytes);
  env_.dsm = &dsm_;
  env_.cache = cache_.get();
  env_.pool = pool_.get();

  listener_ = std::make_unique<Listener>(opts_.bindHost, opts_.dataPort);
  const uint16_t dataPort = listener_->port();
  LOG_INFO("arena " << (arena_->size() >> 20) << " MiB, page cache " << (opts_.cacheBytes >> 20) << " MiB, "
                    << pool_->size() << " threads, data plane on port " << dataPort);
  std::thread([this] { acceptLoop(); }).detach();
  std::thread([this] { execLoop(); }).detach();
  if (opts_.telemetryMs > 0) std::thread([this] { telemetryLoop(); }).detach();

  for (;;) {
    Socket sock;
    try {
      sock = Socket::connectTo(opts_.controllerHost, opts_.controllerPort);
    } catch (const std::exception& e) {
      LOG_WARN("controller " << opts_.controllerHost << ":" << opts_.controllerPort << " not reachable (" << e.what()
                             << "); retrying in 2 s");
      std::this_thread::sleep_for(std::chrono::seconds(2));
      continue;
    }
    const std::string advertise = opts_.advertise.empty() ? sock.localHost() : opts_.advertise;

    std::promise<void> closed;
    auto closedFuture = closed.get_future();
    auto ctrl = RpcPeer::start(
        std::move(sock), [this](const std::shared_ptr<RpcPeer>& p, Message&& m) { onControl(p, std::move(m)); },
        "controller", [&closed] { closed.set_value(); });

    ByteWriter w;
    w.putString(hostName());
    w.putString(advertise);
    w.put<uint16_t>(dataPort);
    w.put<uint64_t>(arena_->size());
    w.put<uint32_t>(pool_->size());
    const auto names = kernelNames();
    w.put<uint32_t>(static_cast<uint32_t>(names.size()));
    for (const auto& n : names) w.putString(n);

    // The id must be known before the controller's follow-up messages are processed, so it is
    // set in the response callback, which runs on the reader thread in message order.
    std::promise<clError_t> registered;
    auto registeredFuture = registered.get_future();
    ctrl->callAsync(MsgType::Register, w.take(), [this, &registered, advertise, dataPort](Message&& m) {
      clError_t st = m.status;
      if (st == clSuccess) {
        try {
          ByteReader r(m.payload);
          const uint32_t id = r.get<uint32_t>();
          id_ = id;
          env_.workerId = id;
          dsm_.setSelfId(id);
          LOG_INFO("registered as worker " << id << " (data plane " << advertise << ":" << dataPort << ")");
        } catch (const std::exception&) {
          st = clErrorProtocol;
        }
      }
      registered.set_value(st);
    });
    const clError_t st = registeredFuture.get();
    if (st == clSuccess) {
      std::lock_guard<std::mutex> lk(ctrlMu_);
      ctrl_ = ctrl;  // telemetry starts flowing
    }
    if (st != clSuccess) {
      LOG_ERROR("registration failed: " << clGetErrorString(st));
      ctrl->close();
      closedFuture.wait();
      return 1;
    }
    closedFuture.wait();
    // Global memory held by this worker is gone with the session; rejoining would present an
    // empty arena as if it held data, so we exit and let the supervisor restart us cleanly.
    LOG_ERROR("lost connection to controller; exiting");
    return 2;
  }
}

void Worker::acceptLoop() {
  for (;;) {
    try {
      Socket s = listener_->accept();
      const std::string name = "data<-" + s.peerName();
      RpcPeer::start(
          std::move(s), [this](const std::shared_ptr<RpcPeer>& p, Message&& m) { onData(p, std::move(m)); }, name);
    } catch (const std::exception& e) {
      LOG_ERROR("data plane accept failed: " << e.what());
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void Worker::onControl(const std::shared_ptr<RpcPeer>& peer, Message&& m) {
  ByteReader r(m.payload);
  switch (m.type) {
    case MsgType::ClusterMap: {
      const auto peers = decodeClusterMap(r);
      dsm_.setPeers(peers);
      LOG_INFO("cluster map: " << peers.size() << " worker(s)");
      peer->reply(m, clSuccess);
      return;
    }
    case MsgType::AllocAdd: {
      Allocation a = Allocation::decode(r);
      LOG_DEBUG("alloc add base=0x" << std::hex << a.base << std::dec << " size=" << a.size);
      map_.add(std::move(a));
      peer->reply(m, clSuccess);
      return;
    }
    case MsgType::AllocRemove: {
      const clDevPtr base = r.get<uint64_t>();
      map_.remove(base);
      peer->reply(m, clSuccess);
      return;
    }
    case MsgType::ExecBlocks: {
      ExecJob job{peer, m.requestId, ExecBlocksRequest::decode(r)};
      {
        std::lock_guard<std::mutex> lk(queueMu_);
        queue_.push_back(std::move(job));
      }
      queueCv_.notify_one();
      return;  // answered by the executor thread
    }
    case MsgType::Ping:
      peer->reply(m, clSuccess);
      return;
    default:
      peer->reply(m, clErrorProtocol);
  }
}

void Worker::onData(const std::shared_ptr<RpcPeer>& peer, Message&& m) {
  ByteReader r(m.payload);
  std::vector<std::pair<uint64_t, uint64_t>> extents;
  uint64_t total = 0;
  switch (m.type) {
    case MsgType::MemReadV: {
      if (!readExtents(r, *arena_, &extents, &total)) {
        peer->reply(m, clErrorInvalidDevicePointer);
        return;
      }
      if (total > kMaxPayloadBytes) {
        peer->reply(m, clErrorInvalidValue);
        return;
      }
      std::vector<uint8_t> out(total);
      uint8_t* p = out.data();
      for (const auto& [off, len] : extents) {
        std::memcpy(p, arena_->data() + off, len);
        p += len;
      }
      peer->reply(m, clSuccess, out);
      return;
    }
    case MsgType::MemWriteV: {
      if (!readExtents(r, *arena_, &extents, &total)) {
        peer->reply(m, clErrorInvalidDevicePointer);
        return;
      }
      const uint8_t* src = r.view(total);
      for (const auto& [off, len] : extents) {
        std::memcpy(arena_->data() + off, src, len);
        src += len;
      }
      peer->reply(m, clSuccess);
      return;
    }
    case MsgType::MemFillV: {
      const uint8_t value = r.get<uint8_t>();
      if (!readExtents(r, *arena_, &extents, &total)) {
        peer->reply(m, clErrorInvalidDevicePointer);
        return;
      }
      for (const auto& [off, len] : extents) std::memset(arena_->data() + off, value, len);
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

void Worker::execLoop() {
  for (;;) {
    ExecJob job;
    {
      std::unique_lock<std::mutex> lk(queueMu_);
      queueCv_.wait(lk, [&] { return !queue_.empty(); });
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    {
      std::lock_guard<std::mutex> lk(kernelMu_);
      currentKernel_ = job.req.kernel;
    }
    executing_ = 1;
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t blocks = 0;
    const clError_t st = execute(job.req, &blocks);
    const uint64_t ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    executing_ = 0;
    {
      std::lock_guard<std::mutex> lk(kernelMu_);
      currentKernel_.clear();
    }
    ++execsCompleted_;
    blocksExecuted_ += blocks;
    execBusyNs_ += ns;
    LOG_DEBUG("launch " << job.req.launchId << " " << job.req.kernel << " blocks [" << job.req.blockBegin << ","
                        << job.req.blockEnd << ") in " << ns / 1000 << " us: " << clGetErrorString(st));
    ByteWriter w;
    w.put<uint64_t>(ns).put<uint64_t>(blocks);
    job.peer->reply(job.requestId, MsgType::ExecBlocks, st, w.take());
  }
}

void Worker::telemetryLoop() {
  SystemSampler sampler;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(opts_.telemetryMs));
    std::shared_ptr<RpcPeer> ctrl;
    {
      std::lock_guard<std::mutex> lk(ctrlMu_);
      ctrl = ctrl_;
    }
    TelemetrySample t;
    sampler.sample(&t);  // sample even while unregistered, so CPU deltas stay meaningful
    if (!ctrl || !ctrl->alive()) continue;

    t.arenaBytes = arena_->size();
    const PageCache::Stats cs = cache_->stats();
    t.cacheBytes = cs.bytes;
    t.cacheCapacityBytes = cs.capacity;
    t.cacheHits = cs.hits;
    t.cacheMisses = cs.misses;
    t.computeThreads = pool_->size();
    t.busyThreads = pool_->busy();
    {
      std::lock_guard<std::mutex> lk(queueMu_);
      t.queueDepth = static_cast<uint32_t>(queue_.size());
    }
    t.executing = executing_.load();
    t.execsCompleted = execsCompleted_.load();
    t.blocksExecuted = blocksExecuted_.load();
    t.execBusyNs = execBusyNs_.load();
    t.netRxBytes = RpcPeer::totalBytesReceived();
    t.netTxBytes = RpcPeer::totalBytesSent();
    {
      std::lock_guard<std::mutex> lk(kernelMu_);
      t.currentKernel = currentKernel_;
    }
    ByteWriter w(256);
    t.encode(w);
    ctrl->callAsync(MsgType::Telemetry, w.take(), nullptr);  // fire and forget
  }
}

clError_t Worker::execute(const ExecBlocksRequest& req, uint64_t* blocksRun) {
  const clKernelFn fn = findKernel(req.kernel);
  if (fn == nullptr) {
    LOG_WARN("kernel '" << req.kernel << "' is not registered on this worker");
    return clErrorKernelNotFound;
  }
  const uint64_t total = req.grid.count();
  if (total == 0 || req.block.count() == 0 || req.blockBegin > req.blockEnd || req.blockEnd > total) {
    return clErrorInvalidValue;
  }
  if (req.launchId != lastLaunch_) {  // new launch: cached remote pages may be stale
    cache_->clear();
    lastLaunch_ = req.launchId;
  }

  clBlockContext ctx(&env_);
  ctx.gridDim = req.grid;
  ctx.blockDim = req.block;
  const uint64_t gx = req.grid.x;
  const uint64_t gxy = gx * req.grid.y;
  try {
    for (uint64_t b = req.blockBegin; b < req.blockEnd; ++b) {
      ctx.blockLinear = b;
      ctx.blockIdx = clDim3(static_cast<uint32_t>(b % gx), static_cast<uint32_t>((b / gx) % req.grid.y),
                            static_cast<uint32_t>(b / gxy));
      env_.scratchNext = 0;
      clArgReader args(req.args.data(), req.args.size());
      fn(ctx, args);
      ++*blocksRun;
    }
  } catch (const clKernelFault& f) {
    LOG_WARN("kernel '" << req.kernel << "' memory fault: " << clGetErrorString(f.code()));
    return f.code();
  } catch (const std::exception& e) {
    LOG_WARN("kernel '" << req.kernel << "' failed: " << e.what());
    return clErrorLaunchFailure;
  }
  return clSuccess;
}

}  // namespace cl
