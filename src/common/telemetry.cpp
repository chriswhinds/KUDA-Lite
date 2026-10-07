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

#include "common/telemetry.h"

#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iterator>
#include <thread>
#include <vector>

namespace cl {
namespace {

/// First line of a small sysfs/procfs file, or "" if it cannot be read.
std::string readLine(const char* path) {
  std::ifstream f(path);
  std::string line;
  std::getline(f, line);
  return line;
}

/// Value of "Key:   123 kB" in /proc/meminfo or /proc/self/status, or 0.
[[maybe_unused]] uint64_t procField(const char* path, const char* key) {
  std::ifstream f(path);
  std::string line;
  const size_t keyLen = std::strlen(key);
  while (std::getline(f, line)) {
    if (line.compare(0, keyLen, key) == 0 && line.size() > keyLen && line[keyLen] == ':') {
      return std::strtoull(line.c_str() + keyLen + 1, nullptr, 10);
    }
  }
  return 0;
}

/// Entries of a directory whose names start with `prefix` (sorted), or none.
[[maybe_unused]] std::vector<std::string> listDir(const std::string& dir, const char* prefix) {
  std::vector<std::string> out;
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return out;
  const size_t n = std::strlen(prefix);
  while (dirent* e = ::readdir(d)) {
    if (std::strncmp(e->d_name, prefix, n) == 0) out.emplace_back(e->d_name);
  }
  ::closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

std::string trimModel(std::string s) {
  s.erase(std::find(s.begin(), s.end(), '\0'), s.end());  // device-tree strings are NUL-terminated
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s;
}

}  // namespace

std::string detectBoardModel(const std::string& root) {
  std::ifstream dt(root + "/proc/device-tree/model", std::ios::binary);
  std::string model((std::istreambuf_iterator<char>(dt)), std::istreambuf_iterator<char>());
  model = trimModel(model);
  if (!model.empty()) return model;
  std::string vendor = trimModel(readLine((root + "/sys/class/dmi/id/sys_vendor").c_str()));
  std::string product = trimModel(readLine((root + "/sys/class/dmi/id/product_name").c_str()));
  if (product.empty()) product = trimModel(readLine((root + "/sys/class/dmi/id/board_name").c_str()));
  if (product.empty()) return vendor;
  return vendor.empty() || product.rfind(vendor, 0) == 0 ? product : vendor + " " + product;
}

uint64_t steadyMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

uint64_t wallMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
}

void TelemetrySample::encode(ByteWriter& out) const {
  ByteWriter w(256);
  w.put<uint16_t>(kTelemetryVersion);
  w.put<uint64_t>(timestampMs).put<uint64_t>(uptimeMs);
  w.put<uint64_t>(memTotalBytes).put<uint64_t>(memAvailableBytes);
  w.put<uint64_t>(rssBytes).put<uint32_t>(processThreads);
  w.put<uint64_t>(arenaBytes).put<uint64_t>(cacheBytes).put<uint64_t>(cacheCapacityBytes);
  w.put<uint64_t>(cacheHits).put<uint64_t>(cacheMisses);
  w.put<uint32_t>(computeThreads).put<uint32_t>(busyThreads);
  w.put<uint32_t>(cpuCores).put<float>(cpuPercent).put<float>(load1).put<float>(cpuTempC).put<uint32_t>(cpuFreqMHz);
  w.put<uint32_t>(queueDepth).put<uint32_t>(executing);
  w.put<uint64_t>(execsCompleted).put<uint64_t>(blocksExecuted).put<uint64_t>(execBusyNs);
  w.put<uint64_t>(netRxBytes).put<uint64_t>(netTxBytes);
  w.putString(currentKernel);
  w.putString(board);  // v2
  const auto body = w.take();
  out.putBlob(body.data(), body.size());
}

TelemetrySample TelemetrySample::decode(ByteReader& outer) {
  const auto body = outer.getBlob();
  ByteReader r(body);
  const uint16_t version = r.get<uint16_t>();
  if (version < 1) throw ProtocolError("bad telemetry version");
  TelemetrySample s;
  s.timestampMs = r.get<uint64_t>();
  s.uptimeMs = r.get<uint64_t>();
  s.memTotalBytes = r.get<uint64_t>();
  s.memAvailableBytes = r.get<uint64_t>();
  s.rssBytes = r.get<uint64_t>();
  s.processThreads = r.get<uint32_t>();
  s.arenaBytes = r.get<uint64_t>();
  s.cacheBytes = r.get<uint64_t>();
  s.cacheCapacityBytes = r.get<uint64_t>();
  s.cacheHits = r.get<uint64_t>();
  s.cacheMisses = r.get<uint64_t>();
  s.computeThreads = r.get<uint32_t>();
  s.busyThreads = r.get<uint32_t>();
  s.cpuCores = r.get<uint32_t>();
  s.cpuPercent = r.get<float>();
  s.load1 = r.get<float>();
  s.cpuTempC = r.get<float>();
  s.cpuFreqMHz = r.get<uint32_t>();
  s.queueDepth = r.get<uint32_t>();
  s.executing = r.get<uint32_t>();
  s.execsCompleted = r.get<uint64_t>();
  s.blocksExecuted = r.get<uint64_t>();
  s.execBusyNs = r.get<uint64_t>();
  s.netRxBytes = r.get<uint64_t>();
  s.netTxBytes = r.get<uint64_t>();
  s.currentKernel = r.getString();
  if (version >= 2) s.board = r.getString();
  // Fields added by later versions follow here and are ignored by this reader.
  return s;
}

SystemSampler::SystemSampler(std::string root)
    : root_(std::move(root)), board_(detectBoardModel(root_)), startMs_(steadyMs()) {}

void SystemSampler::sample(TelemetrySample* s) {
  s->timestampMs = wallMs();
  s->uptimeMs = steadyMs() - startMs_;
  s->board = board_;
  s->cpuCores = std::max(1u, std::thread::hardware_concurrency());
  double load[1] = {0};
  if (::getloadavg(load, 1) == 1) s->load1 = static_cast<float>(load[0]);

#ifdef __linux__
  const std::string meminfo = root_ + "/proc/meminfo";
  const std::string status = root_ + "/proc/self/status";
  s->memTotalBytes = procField(meminfo.c_str(), "MemTotal") * 1024;
  s->memAvailableBytes = procField(meminfo.c_str(), "MemAvailable") * 1024;
  s->processThreads = static_cast<uint32_t>(procField(status.c_str(), "Threads"));
  s->rssBytes = procField(status.c_str(), "VmRSS") * 1024;

  // Whole-system CPU utilisation from the aggregate "cpu" line of /proc/stat.
  std::istringstream cpu(readLine((root_ + "/proc/stat").c_str()));
  std::string label;
  uint64_t v[10] = {0};
  cpu >> label;
  for (auto& x : v) cpu >> x;
  const uint64_t idle = v[3] + v[4];  // idle + iowait
  uint64_t total = 0;
  for (int i = 0; i < 8; ++i) total += v[i];  // guest time is already counted in user
  const uint64_t busy = total - idle;
  if (prevTotal_ != 0 && total > prevTotal_) {
    s->cpuPercent = static_cast<float>(100.0 * double(busy - prevBusy_) / double(total - prevTotal_));
  }
  prevBusy_ = busy;
  prevTotal_ = total;

  // Hottest thermal zone, millidegrees C. Pi 5: one zone (cpu-thermal). CIX P1: several (CPU
  // clusters, GPU, NPU...). Readings outside -40..150 C are sensor faults and are ignored.
  const std::string thermal = root_ + "/sys/class/thermal";
  for (const auto& zone : listDir(thermal, "thermal_zone")) {
    const std::string t = readLine((thermal + "/" + zone + "/temp").c_str());
    if (t.empty()) continue;
    char* end = nullptr;
    const double c = std::strtod(t.c_str(), &end) / 1000.0;
    if (end != t.c_str() && c > -40.0 && c < 150.0 && c > s->cpuTempC) s->cpuTempC = static_cast<float>(c);
  }

  // Fastest cpufreq policy, kHz (one policy per cluster; the big cores show throttling first).
  const std::string cpufreq = root_ + "/sys/devices/system/cpu/cpufreq";
  for (const auto& policy : listDir(cpufreq, "policy")) {
    const std::string f = readLine((cpufreq + "/" + policy + "/scaling_cur_freq").c_str());
    if (!f.empty()) s->cpuFreqMHz = std::max(s->cpuFreqMHz, static_cast<uint32_t>(std::strtoull(f.c_str(), nullptr, 10) / 1000));
  }
  if (s->cpuFreqMHz == 0) {  // kernels without the policy directory
    const std::string f = readLine((root_ + "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq").c_str());
    if (!f.empty()) s->cpuFreqMHz = static_cast<uint32_t>(std::strtoull(f.c_str(), nullptr, 10) / 1000);
  }
#else
  // Portable fallbacks (macOS development hosts): fewer fields, never wrong ones.
  const long pages = ::sysconf(_SC_PHYS_PAGES);
  const long pageSize = ::sysconf(_SC_PAGESIZE);
  if (pages > 0 && pageSize > 0) s->memTotalBytes = uint64_t(pages) * uint64_t(pageSize);
  rusage ru{};
  if (::getrusage(RUSAGE_SELF, &ru) == 0) s->rssBytes = static_cast<uint64_t>(ru.ru_maxrss);  // bytes on macOS
#endif
}

}  // namespace cl
