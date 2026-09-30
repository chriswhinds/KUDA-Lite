#include "common/telemetry.h"

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

namespace cl {
namespace {

#ifdef __linux__
/// First line of a small sysfs/procfs file, or "" if it cannot be read.
std::string readLine(const char* path) {
  std::ifstream f(path);
  std::string line;
  std::getline(f, line);
  return line;
}

/// Value of "Key:   123 kB" in /proc/meminfo or /proc/self/status, or 0.
uint64_t procField(const char* path, const char* key) {
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
#endif

}  // namespace

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
  // Fields added by later versions follow here and are ignored by this reader.
  return s;
}

SystemSampler::SystemSampler() : startMs_(steadyMs()) {}

void SystemSampler::sample(TelemetrySample* s) {
  s->timestampMs = wallMs();
  s->uptimeMs = steadyMs() - startMs_;
  s->cpuCores = std::max(1u, std::thread::hardware_concurrency());
  double load[1] = {0};
  if (::getloadavg(load, 1) == 1) s->load1 = static_cast<float>(load[0]);

#ifdef __linux__
  s->memTotalBytes = procField("/proc/meminfo", "MemTotal") * 1024;
  s->memAvailableBytes = procField("/proc/meminfo", "MemAvailable") * 1024;
  s->processThreads = static_cast<uint32_t>(procField("/proc/self/status", "Threads"));
  s->rssBytes = procField("/proc/self/status", "VmRSS") * 1024;

  // Whole-system CPU utilisation from the aggregate "cpu" line of /proc/stat.
  std::istringstream cpu(readLine("/proc/stat"));
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

  // Raspberry Pi 5: thermal_zone0 is the SoC ("cpu-thermal"), millidegrees C.
  const std::string temp = readLine("/sys/class/thermal/thermal_zone0/temp");
  if (!temp.empty()) s->cpuTempC = static_cast<float>(std::strtol(temp.c_str(), nullptr, 10) / 1000.0);
  const std::string freq = readLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");  // kHz
  if (!freq.empty()) s->cpuFreqMHz = static_cast<uint32_t>(std::strtoull(freq.c_str(), nullptr, 10) / 1000);
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
