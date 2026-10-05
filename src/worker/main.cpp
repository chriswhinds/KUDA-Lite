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

// cl-worker entry point.
#include <cstdio>
#include <cstdlib>
#include <iostream>

#include "common/cli.h"
#include "common/log.h"
#include "common/net.h"
#include "worker/worker.h"

namespace {

void usage() {
  std::cout << "usage: cl-worker [--config FILE] [options]\n"
               "  --config FILE             read options from FILE (key = value; command line wins)\n"
               "  --controller HOST[:PORT]  controller worker port (default 127.0.0.1:7071)\n"
               "  --bind ADDR               data plane bind address (default 0.0.0.0)\n"
               "  --data-port PORT          data plane port (default 7100; 0 = ephemeral)\n"
               "  --advertise HOST          address other nodes use to reach this worker\n"
               "  --mem SIZE                arena contributed to global memory (default 60% of RAM)\n"
               "  --cache SIZE              remote page cache budget (default 15% of RAM)\n"
               "  --threads N               compute threads (default: all cores)\n"
               "  --telemetry-ms MS         telemetry push period (default 1000; 0 = off)\n"
               "  --log-level LEVEL         debug | info | warn | error (default info)\n";
}

}  // namespace

int main(int argc, char** argv) {
  using namespace cl;
  ignoreSigpipe();
  log::setComponent("worker");
  ArgParser args(argc, argv, {"help"});
  if (args.has("help")) {
    usage();
    return 0;
  }
  if (args.has("config")) {
    std::string error;
    if (!args.loadConfigFile(args.get("config", ""), &error)) {
      std::cerr << error << "\n";
      return 78;  // EX_CONFIG
    }
  }
  const auto bad = args.unknown(
      {"config", "controller", "bind", "data-port", "advertise", "mem", "cache", "threads", "telemetry-ms", "log-level"});
  if (!bad.empty()) {
    std::cerr << "unknown option or config key '" << bad.front() << "'\n";
    usage();
    return 64;
  }

  WorkerOptions o;
  try {
    const uint64_t ram = physicalMemoryBytes();
    if (!parseHostPort(args.get("controller", "127.0.0.1"), kDefaultWorkerPort, &o.controllerHost,
                       &o.controllerPort)) {
      std::cerr << "bad --controller\n";
      return 64;
    }
    o.bindHost = args.get("bind", "0.0.0.0");
    o.dataPort = static_cast<uint16_t>(args.getU64("data-port", kDefaultDataPort));
    o.advertise = args.get("advertise", "");
    o.memBytes = args.getSize("mem", ram ? ram / 10 * 6 : (1ull << 30));
    o.cacheBytes = args.getSize("cache", ram ? ram / 100 * 15 : (256ull << 20));
    o.threads = static_cast<unsigned>(args.getU64("threads", 0));
    o.telemetryMs = static_cast<unsigned>(args.getU64("telemetry-ms", 1000));
    log::Level lvl;
    if (!log::parseLevel(args.get("log-level", "info"), &lvl)) {
      std::cerr << "bad --log-level\n";
      return 64;
    }
    log::setLevel(lvl);
  } catch (const std::exception& e) {
    std::cerr << "bad option value: " << e.what() << "\n";
    return 64;
  }

  int rc = 1;
  try {
    // Deliberately leaked: detached service threads keep using it until the process exits.
    auto* worker = new Worker(o);
    rc = worker->run();
  } catch (const std::exception& e) {
    LOG_ERROR("fatal: " << e.what());
  }
  std::fflush(stderr);
  std::_Exit(rc);  // skip static destructors while detached threads may still be running
}
