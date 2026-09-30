#include "common/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace cl::log {
namespace {

std::atomic<int> gLevel{static_cast<int>(Level::Info)};
std::mutex gMu;
std::string gComponent = "kudalite";
const char* const kNames[] = {"DEBUG", "INFO", "WARN", "ERROR"};

}  // namespace

void setLevel(Level level) { gLevel.store(static_cast<int>(level)); }

Level level() { return static_cast<Level>(gLevel.load()); }

bool parseLevel(const std::string& text, Level* out) {
  if (text == "debug") *out = Level::Debug;
  else if (text == "info") *out = Level::Info;
  else if (text == "warn") *out = Level::Warn;
  else if (text == "error") *out = Level::Error;
  else return false;
  return true;
}

void setComponent(const std::string& name) {
  std::lock_guard<std::mutex> lk(gMu);
  gComponent = name;
}

void write(Level level, const std::string& message) {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t t = system_clock::to_time_t(now);
  const int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
  std::tm tm{};
  localtime_r(&t, &tm);
  char ts[16];
  std::strftime(ts, sizeof ts, "%H:%M:%S", &tm);

  std::lock_guard<std::mutex> lk(gMu);
  std::fprintf(stderr, "%s.%03d %-5s [%s] %s\n", ts, ms, kNames[static_cast<int>(level)], gComponent.c_str(),
               message.c_str());
}

}  // namespace cl::log
