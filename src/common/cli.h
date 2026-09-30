// Tiny command-line parser for the daemons: --key value, --key=value, and boolean --flags.
#pragma once

#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace cl {

/// Parses sizes such as "65536", "64K", "512M", "6G" (optional trailing "B" / "iB").
inline uint64_t parseSize(const std::string& text) {
  size_t idx = 0;
  const unsigned long long value = std::stoull(text, &idx, 10);
  uint64_t mult = 1;
  if (idx < text.size()) {
    switch (text[idx]) {
      case 'k': case 'K': mult = 1ull << 10; break;
      case 'm': case 'M': mult = 1ull << 20; break;
      case 'g': case 'G': mult = 1ull << 30; break;
      case 't': case 'T': mult = 1ull << 40; break;
      default: throw std::invalid_argument("bad size: " + text);
    }
    const std::string rest = text.substr(idx + 1);
    if (!rest.empty() && rest != "B" && rest != "iB") throw std::invalid_argument("bad size: " + text);
  }
  return value * mult;
}

class ArgParser {
 public:
  ArgParser(int argc, char** argv, const std::set<std::string>& booleanFlags = {}) {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg.rfind("--", 0) != 0) {
        positional_.push_back(arg);
        continue;
      }
      std::string key = arg.substr(2);
      std::string value = "true";
      const size_t eq = key.find('=');
      if (eq != std::string::npos) {
        value = key.substr(eq + 1);
        key = key.substr(0, eq);
      } else if (!booleanFlags.count(key) && i + 1 < argc) {
        value = argv[++i];
      }
      opts_[key] = value;
    }
  }

  /// Merges a config file of "key = value" lines ('#' starts a comment; values may be quoted).
  /// Keys are the long option names without "--". Options given on the command line win.
  /// Returns false and sets *error if the file cannot be read or a line is malformed.
  bool loadConfigFile(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in) {
      *error = "cannot read config file " + path;
      return false;
    }
    std::string line;
    for (int lineNo = 1; std::getline(in, line); ++lineNo) {
      const size_t hash = line.find('#');
      if (hash != std::string::npos) line.erase(hash);
      const std::string text = trim(line);
      if (text.empty()) continue;
      const size_t eq = text.find('=');
      if (eq == std::string::npos) {
        *error = path + ":" + std::to_string(lineNo) + ": expected 'key = value'";
        return false;
      }
      const std::string key = trim(text.substr(0, eq));
      std::string value = trim(text.substr(eq + 1));
      if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
        value = value.substr(1, value.size() - 2);
      }
      if (key.empty()) {
        *error = path + ":" + std::to_string(lineNo) + ": empty key";
        return false;
      }
      opts_.emplace(key, value);  // does not overwrite command-line values
    }
    return true;
  }

  bool has(const std::string& key) const { return opts_.count(key) != 0; }
  std::string get(const std::string& key, const std::string& def) const {
    auto it = opts_.find(key);
    return it == opts_.end() ? def : it->second;
  }
  uint64_t getU64(const std::string& key, uint64_t def) const {
    auto it = opts_.find(key);
    return it == opts_.end() ? def : std::stoull(it->second);
  }
  uint64_t getSize(const std::string& key, uint64_t def) const {
    auto it = opts_.find(key);
    return it == opts_.end() ? def : parseSize(it->second);
  }
  /// Keys that are not in `known` (for "unknown option" errors).
  std::vector<std::string> unknown(const std::set<std::string>& known) const {
    std::vector<std::string> out;
    for (const auto& [k, v] : opts_) {
      if (!known.count(k)) out.push_back(k);
    }
    return out;
  }
  const std::vector<std::string>& positional() const { return positional_; }

 private:
  static std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return "";
    const size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
  }

  std::map<std::string, std::string> opts_;
  std::vector<std::string> positional_;
};

}  // namespace cl
