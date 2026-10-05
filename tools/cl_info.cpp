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

// cl-info: prints the cluster's properties and workers (the KUDA-Lite "deviceQuery").
//   cl-info [HOST[:PORT]]
#include <cstdio>
#include <vector>

#include "kudalite/kudalite.h"

int main(int argc, char** argv) {
  const clError_t init = clInit(argc > 1 ? argv[1] : nullptr);
  if (init != clSuccess) {
    std::fprintf(stderr, "cl-info: %s\n", clGetErrorString(init));
    return 1;
  }
  clDeviceProp prop{};
  if (clGetDeviceProperties(&prop) != clSuccess) {
    std::fprintf(stderr, "cl-info: cannot query the controller\n");
    return 1;
  }
  std::printf("Device:            %s\n", prop.name);
  std::printf("Protocol version:  %u\n", prop.protocolVersion);
  std::printf("Workers:           %u\n", prop.workerCount);
  std::printf("CPU cores:         %u\n", prop.totalCores);
  std::printf("Global memory:     %.1f MiB (%.1f MiB free)\n", prop.totalGlobalMem / 1048576.0,
              prop.freeGlobalMem / 1048576.0);
  std::printf("Default page size: %u bytes\n\n", prop.pageSize);

  uint32_t count = 0;
  clGetWorkerInfo(nullptr, 0, &count);
  std::vector<clWorkerInfo> workers(count);
  if (count > 0 && clGetWorkerInfo(workers.data(), count, &count) == clSuccess) {
    std::printf("  ID  %-20s %-22s %6s %12s %12s\n", "HOST", "DATA PLANE", "CORES", "ARENA MiB", "USED MiB");
    for (uint32_t i = 0; i < count && i < workers.size(); ++i) {
      const clWorkerInfo& w = workers[i];
      char addr[96];
      std::snprintf(addr, sizeof addr, "%s:%u", w.address, w.dataPort);
      std::printf("%4u  %-20s %-22s %6u %12.1f %12.1f\n", w.id, w.hostname, addr, w.cores, w.memBytes / 1048576.0,
                  w.memUsed / 1048576.0);
    }
  }
  clShutdown();
  return 0;
}
