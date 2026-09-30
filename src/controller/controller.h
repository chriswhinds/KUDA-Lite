// cl-controller: the cluster controller (runs on the dedicated controller Pi).
//
// Responsibilities
//  * Worker membership: accepts registrations, assigns ids, publishes the cluster map.
//  * Global memory management: owns the GVA space and a free-list per worker arena, places
//    allocations, and publishes allocation records to every worker before returning a pointer.
//  * Host sessions: one per connected host process. Each host stream is a StreamQueue whose
//    operations (copies, memsets, launches, event records/waits) run in order.
//  * Kernel scheduling: splits a launch's grid into chunks of blocks and hands them to idle
//    workers (dynamic self-scheduling) until the grid is exhausted.
//  * Data movement for the host: memcpy/memset requests are serviced through a DsmClient that
//    talks to the owning workers directly.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/address_map.h"
#include "common/dsm_client.h"
#include "common/freelist.h"
#include "common/net.h"
#include "common/protocol.h"
#include "common/rpc.h"
#include "common/telemetry.h"
#include "controller/stream_queue.h"

namespace cl {

struct ControllerOptions {
  std::string bindHost = "0.0.0.0";
  uint16_t hostPort = kDefaultHostPort;
  uint16_t workerPort = kDefaultWorkerPort;
  uint32_t pageSize = 64 * 1024;  // default striping page
  uint32_t chunkFactor = 4;       // target chunks per worker per launch (load balancing)
  uint32_t telemetryMs = 1000;    // self-sampling period; 0 = off
  uint32_t telemetryHistory = 600;  // samples kept per node (10 min at 1 s)
};

class Controller {
 public:
  explicit Controller(ControllerOptions opts);
  /// Serves forever; returns only if a listener fails.
  int run();

 private:
  static constexpr clDevPtr kGvaBase = 0x100000000000ull;  // non-zero so 0 is never valid
  static constexpr uint32_t kControllerNodeId = 0xFFFFFFFEu;  // telemetry id of the controller itself

  struct WorkerRec {
    uint32_t id = kNoWorker;
    std::string hostname;
    std::string host;  // data-plane address
    uint16_t dataPort = 0;
    uint64_t memBytes = 0;
    uint32_t cores = 0;
    std::vector<std::string> kernels;
    std::shared_ptr<RpcPeer> peer;  // control connection
    FreeListAllocator arena;        // guarded by memMu_
    bool alive = true;              // guarded by workersMu_
  };

  struct EventRec {
    uint64_t recorded = 0;   // generation of the latest EventRecord received
    uint64_t completed = 0;  // highest generation that has executed
    uint64_t tsNs = 0;
  };

  struct Session {
    uint64_t id = 0;
    std::mutex mu;
    std::condition_variable cv;  // signalled when an event completes or the session closes
    bool closing = false;
    std::atomic<bool> cancelled{false};  // set on disconnect; running launches stop early
    std::map<uint32_t, std::unique_ptr<StreamQueue>> streams;
    std::unordered_map<uint32_t, EventRec> events;
    std::set<clDevPtr> allocations;
  };

  struct NodeTelemetry {
    std::deque<TelemetrySample> samples;  // oldest first
    uint64_t lastSeenMs = 0;              // steady clock
  };

  struct LaunchResult {
    uint64_t blocks = 0;
    uint32_t workersUsed = 0;
    double wallMs = 0;
  };

  void acceptLoop(Listener& listener, bool hosts);

  // Workers
  void onWorkerConnect(Socket sock);
  void onWorkerMessage(const std::shared_ptr<RpcPeer>& peer, Message&& m,
                       const std::shared_ptr<std::atomic<uint32_t>>& idSlot);
  void registerWorker(const std::shared_ptr<RpcPeer>& peer, const Message& m,
                      const std::shared_ptr<std::atomic<uint32_t>>& idSlot);
  void onWorkerClosed(uint32_t id);
  std::vector<std::shared_ptr<WorkerRec>> aliveWorkers();
  void broadcastClusterMap();

  // Hosts
  void onHostConnect(Socket sock);
  void onHostMessage(const std::shared_ptr<Session>& s, const std::shared_ptr<RpcPeer>& peer, Message&& m);
  void enqueue(const std::shared_ptr<Session>& s, uint32_t stream, std::function<void()> op);
  void executeStreamOp(const std::shared_ptr<Session>& s, const std::shared_ptr<RpcPeer>& peer,
                       const Message& m);
  void closeSession(uint64_t id);

  // Memory
  clError_t allocate(uint64_t size, uint32_t dist, uint32_t pageSize, clDevPtr* out);
  clError_t release(clDevPtr base);
  void releaseSlicesLocked(const Allocation& a);
  clError_t copyDeviceToDevice(clDevPtr dst, clDevPtr src, uint64_t len);

  // Telemetry
  void recordTelemetry(uint32_t node, TelemetrySample sample);
  void telemetryLoop();
  std::vector<uint8_t> encodeTelemetry(uint32_t maxHistory);

  // Execution
  clError_t runLaunch(const std::string& kernel, clDim3 grid, clDim3 block, const std::vector<uint8_t>& args,
                      const std::atomic<bool>& cancelled, LaunchResult* result);

  ControllerOptions opts_;
  AddressMap map_;
  DsmClient dsm_;

  std::mutex workersMu_;  // lock order: memMu_ before workersMu_
  std::map<uint32_t, std::shared_ptr<WorkerRec>> workers_;
  uint32_t nextWorkerId_ = 0;
  std::mutex broadcastMu_;  // keeps cluster maps arriving in order

  std::mutex memMu_;
  clDevPtr nextGva_ = kGvaBase;
  uint32_t rotate_ = 0;

  std::mutex sessionsMu_;
  std::map<uint64_t, std::shared_ptr<Session>> sessions_;
  std::atomic<uint64_t> nextSession_{1};
  std::atomic<uint64_t> nextLaunch_{1};
  std::atomic<uint64_t> launchesTotal_{0};
  std::atomic<uint32_t> activeLaunches_{0};

  std::mutex telemetryMu_;
  std::map<uint32_t, NodeTelemetry> telemetry_;
  const uint64_t startMs_ = steadyMs();
};

}  // namespace cl
