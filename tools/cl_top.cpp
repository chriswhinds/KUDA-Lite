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

// cl-top: live telemetry for every Pi in the cluster, in the terminal.
//   cl-top [HOST[:PORT]] [--once] [--interval SECONDS]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "kudalite/kudalite.h"

namespace {

std::string gib(uint64_t bytes) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f", bytes / 1073741824.0);
  return buf;
}

std::string duration(uint64_t ms) {
  const uint64_t s = ms / 1000;
  char buf[32];
  if (s >= 86400) std::snprintf(buf, sizeof buf, "%llud%02lluh", (unsigned long long)(s / 86400),
                                (unsigned long long)(s % 86400 / 3600));
  else std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu", (unsigned long long)(s / 3600),
                     (unsigned long long)(s % 3600 / 60), (unsigned long long)(s % 60));
  return buf;
}

bool render(bool clear) {
  uint32_t count = 0;
  clClusterTelemetry cluster{};
  if (clGetTelemetry(&cluster, nullptr, 0, &count) != clSuccess) return false;
  std::vector<clNodeTelemetry> nodes(count);
  if (clGetTelemetry(&cluster, nodes.data(), count, &count) != clSuccess) return false;
  if (count < nodes.size()) nodes.resize(count);

  if (clear) std::printf("\033[H\033[2J");
  std::printf("KUDA-Lite  up %s  sessions %u  launches %u active / %llu total  allocations %u (%s GiB of %s GiB)\n\n",
              duration(cluster.controllerUptimeMs).c_str(), cluster.sessions, cluster.activeLaunches,
              (unsigned long long)cluster.launchesTotal, cluster.allocations, gib(cluster.allocatedBytes).c_str(),
              gib(cluster.arenaTotalBytes).c_str());
  std::printf("%-5s %-11s %-18s %-8s %6s %6s %13s %7s %8s %11s %9s  %s\n", "ID", "ROLE", "HOST", "STATE", "CPU%",
              "TEMP", "MEM GiB", "RSS", "ARENA", "THREADS", "BLOCKS", "KERNEL");
  for (const auto& n : nodes) {
    char id[16], mem[32], rss[16], arena[32], threads[32], temp[16], cpu[16];
    if (n.id == CL_CONTROLLER_NODE_ID) std::snprintf(id, sizeof id, "ctl");
    else std::snprintf(id, sizeof id, "%u", n.id);
    const char* state = !n.alive ? "OFFLINE" : !n.hasSample ? "WAITING" : n.lastSeenAgeMs > 5000 ? "STALE" : "ok";
    std::snprintf(mem, sizeof mem, "%s/%s", gib(n.memTotalBytes - n.memAvailableBytes).c_str(),
                  gib(n.memTotalBytes).c_str());
    std::snprintf(rss, sizeof rss, "%lluM", (unsigned long long)(n.rssBytes >> 20));
    if (n.role == clNodeWorker) std::snprintf(arena, sizeof arena, "%.0f%%",
                                              n.arenaBytes ? 100.0 * n.arenaUsedBytes / n.arenaBytes : 0.0);
    else std::snprintf(arena, sizeof arena, "-");
    if (n.role == clNodeWorker) std::snprintf(threads, sizeof threads, "%u/%u (%u)", n.busyThreads, n.computeThreads,
                                              n.processThreads);
    else std::snprintf(threads, sizeof threads, "- (%u)", n.processThreads);
    if (n.cpuTempC >= 0) std::snprintf(temp, sizeof temp, "%.0fC", n.cpuTempC);
    else std::snprintf(temp, sizeof temp, "-");
    std::snprintf(cpu, sizeof cpu, "%.0f", n.cpuPercent);
    std::printf("%-5s %-11s %-18.18s %-8s %6s %6s %13s %7s %8s %11s %9llu  %s\n", id,
                n.role == clNodeController ? "controller" : "worker", n.hostname, state, cpu, temp, mem, rss, arena,
                threads, (unsigned long long)n.blocksExecuted, n.currentKernel[0] ? n.currentKernel : "-");
  }
  std::fflush(stdout);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const char* endpoint = nullptr;
  bool once = false;
  double interval = 1.0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--once") == 0) once = true;
    else if (std::strcmp(argv[i], "--interval") == 0 && i + 1 < argc) interval = std::atof(argv[++i]);
    else if (argv[i][0] != '-') endpoint = argv[i];
    else {
      std::fprintf(stderr, "usage: cl-top [HOST[:PORT]] [--once] [--interval SECONDS]\n");
      return 64;
    }
  }
  const clError_t init = clInit(endpoint);
  if (init != clSuccess) {
    std::fprintf(stderr, "cl-top: %s\n", clGetErrorString(init));
    return 1;
  }
  do {
    if (!render(!once)) {
      std::fprintf(stderr, "cl-top: lost the controller\n");
      return 1;
    }
    if (!once) std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(interval * 1000)));
  } while (!once);
  clShutdown();
  return 0;
}
