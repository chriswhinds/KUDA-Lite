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

// Node telemetry: one sample of a node's health and activity, how it is encoded on the wire,
// and a sampler for the OS-level metrics (memory, threads, CPU, temperature).
//
// Workers push a sample to the controller every --telemetry-ms (Telemetry message); the
// controller samples itself on the same period, keeps a short history per node, and serves it
// to hosts (GetTelemetry). Layout on the wire: docs/OBSERVABILITY.md and docs/PROTOCOL.md.
#pragma once

#include <cstdint>
#include <string>

#include "common/bytes.h"

namespace cl {

/// Version of the sample encoding. New fields are only ever appended; readers ignore trailing
/// bytes they do not understand, so older dashboards keep working against newer nodes.
constexpr uint16_t kTelemetryVersion = 2;  // v2 appended `board`

struct TelemetrySample {
  // Identity of the moment
  uint64_t timestampMs = 0;  // node wall clock, Unix epoch milliseconds
  uint64_t uptimeMs = 0;     // since the daemon started

  // System memory (the whole Pi)
  uint64_t memTotalBytes = 0;
  uint64_t memAvailableBytes = 0;

  // This process
  uint64_t rssBytes = 0;
  uint32_t processThreads = 0;  // OS threads in the daemon (service + compute)

  // KUDA-Lite memory
  uint64_t arenaBytes = 0;          // arena contributed to global memory (0 on the controller)
  uint64_t cacheBytes = 0;          // remote-page cache currently held
  uint64_t cacheCapacityBytes = 0;
  uint64_t cacheHits = 0;           // cumulative
  uint64_t cacheMisses = 0;         // cumulative

  // Compute threads
  uint32_t computeThreads = 0;  // size of the kernel thread pool
  uint32_t busyThreads = 0;     // pool threads inside a kernel body at sample time

  // CPU
  uint32_t cpuCores = 0;
  float cpuPercent = 0;  // whole-system utilisation since the previous sample, 0..100
  float load1 = 0;       // 1-minute load average
  float cpuTempC = -1;   // SoC temperature, -1 if unknown
  uint32_t cpuFreqMHz = 0;

  // Executor (workers)
  uint32_t queueDepth = 0;  // ExecBlocks requests waiting
  uint32_t executing = 0;   // 1 while an operation is running
  uint64_t execsCompleted = 0;
  uint64_t blocksExecuted = 0;
  uint64_t execBusyNs = 0;  // cumulative time spent executing

  // Network (all KUDA-Lite connections of this process, framing included), cumulative
  uint64_t netRxBytes = 0;
  uint64_t netTxBytes = 0;

  std::string currentKernel;  // empty when idle

  // --- version 2 ---
  std::string board;  // hardware model, e.g. "Raspberry Pi 5 Model B Rev 1.0", "Orange Pi 6 Plus"

  /// Appends the sample as a length-prefixed blob.
  void encode(ByteWriter& w) const;
  /// Reads a blob written by encode() (any version >= 1).
  static TelemetrySample decode(ByteReader& r);
};

/// Samples the OS-level fields (time, memory, process threads, CPU, temperature, frequency).
/// KUDA-Lite-specific fields are filled in by the caller. Not thread-safe: keep one per thread.
///
/// Works across boards: the temperature is the hottest thermal zone (Pi 5 has one; the CIX P1
/// on the Orange Pi 6 Plus has several) and the frequency is the fastest cpufreq policy (on
/// big.LITTLE parts cpu0 is often a LITTLE core, which would under-report the clock).
class SystemSampler {
 public:
  /// `root` prefixes every /proc and /sys path (tests point it at a fake tree).
  explicit SystemSampler(std::string root = "");
  void sample(TelemetrySample* out);
  const std::string& board() const { return board_; }

 private:
  std::string root_;
  std::string board_;
  uint64_t startMs_;
  uint64_t prevBusy_ = 0;
  uint64_t prevTotal_ = 0;
};

/// Hardware model from the device tree (Raspberry Pi, most ARM boards) or DMI (UEFI/ACPI
/// boards, x86). Empty if neither is available.
std::string detectBoardModel(const std::string& root = "");

/// Steady-clock milliseconds (for ages and intervals).
uint64_t steadyMs();
/// Wall-clock Unix epoch milliseconds.
uint64_t wallMs();

}  // namespace cl
