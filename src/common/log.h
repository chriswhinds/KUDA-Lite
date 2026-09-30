// Minimal thread-safe leveled logging to stderr.
#pragma once

#include <sstream>
#include <string>

namespace cl::log {

enum class Level : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void setLevel(Level level);
Level level();
bool parseLevel(const std::string& text, Level* out);
void setComponent(const std::string& name);  // prefix shown on every line, e.g. "worker"
void write(Level level, const std::string& message);

}  // namespace cl::log

#define CL_LOG(lvl, expr)                                                         \
  do {                                                                            \
    if (static_cast<int>(lvl) >= static_cast<int>(::cl::log::level())) {         \
      std::ostringstream cl_log_os_;                                              \
      cl_log_os_ << expr;                                                         \
      ::cl::log::write(lvl, cl_log_os_.str());                                    \
    }                                                                             \
  } while (0)

#define LOG_DEBUG(expr) CL_LOG(::cl::log::Level::Debug, expr)
#define LOG_INFO(expr) CL_LOG(::cl::log::Level::Info, expr)
#define LOG_WARN(expr) CL_LOG(::cl::log::Level::Warn, expr)
#define LOG_ERROR(expr) CL_LOG(::cl::log::Level::Error, expr)
