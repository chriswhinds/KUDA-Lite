// cl-controller entry point.
#include <cstdio>
#include <cstdlib>
#include <iostream>

#include "common/cli.h"
#include "common/log.h"
#include "common/net.h"
#include "controller/controller.h"

namespace {

void usage() {
  std::cout << "usage: cl-controller [--config FILE] [options]\n"
               "  --config FILE        read options from FILE (key = value; command line wins)\n"
               "  --bind ADDR          listen address (default 0.0.0.0)\n"
               "  --host-port PORT     port for host runtimes (default 7070)\n"
               "  --worker-port PORT   port for worker registration (default 7071)\n"
               "  --page-size SIZE     default striping page size, multiple of 64 (default 64K)\n"
               "  --chunk-factor N     block chunks per worker per launch (default 4)\n"
               "  --telemetry-ms MS    controller self-sampling period (default 1000; 0 = off)\n"
               "  --telemetry-history N  samples kept per node (default 600)\n"
               "  --log-level LEVEL    debug | info | warn | error (default info)\n";
}

}  // namespace

int main(int argc, char** argv) {
  using namespace cl;
  ignoreSigpipe();
  log::setComponent("controller");
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
  const auto bad = args.unknown({"config", "bind", "host-port", "worker-port", "page-size", "chunk-factor",
                                 "telemetry-ms", "telemetry-history", "log-level"});
  if (!bad.empty()) {
    std::cerr << "unknown option or config key '" << bad.front() << "'\n";
    usage();
    return 64;
  }

  ControllerOptions o;
  try {
    o.bindHost = args.get("bind", "0.0.0.0");
    o.hostPort = static_cast<uint16_t>(args.getU64("host-port", kDefaultHostPort));
    o.workerPort = static_cast<uint16_t>(args.getU64("worker-port", kDefaultWorkerPort));
    o.pageSize = static_cast<uint32_t>(args.getSize("page-size", 64 * 1024));
    o.chunkFactor = static_cast<uint32_t>(args.getU64("chunk-factor", 4));
    o.telemetryMs = static_cast<uint32_t>(args.getU64("telemetry-ms", 1000));
    o.telemetryHistory = static_cast<uint32_t>(args.getU64("telemetry-history", 600));
    if (o.pageSize < 64 || o.pageSize % 64 != 0) {
      std::cerr << "--page-size must be a multiple of 64\n";
      return 64;
    }
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
    auto* controller = new Controller(o);  // leaked on purpose, see cl-worker
    rc = controller->run();
  } catch (const std::exception& e) {
    LOG_ERROR("fatal: " << e.what());
  }
  std::fflush(stderr);
  std::_Exit(rc);
}
