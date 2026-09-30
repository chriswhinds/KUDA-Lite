// The worker's contribution to cluster global memory: one large anonymous mapping.
// Pages are committed lazily by the OS on first touch, so a large arena costs nothing until used.
#pragma once

#include <cstdint>

namespace cl {

class Arena {
 public:
  explicit Arena(uint64_t bytes);
  ~Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  uint8_t* data() const { return base_; }
  uint64_t size() const { return size_; }
  bool contains(uint64_t offset, uint64_t len) const { return offset <= size_ && len <= size_ - offset; }

 private:
  uint8_t* base_ = nullptr;
  uint64_t size_ = 0;
};

/// Physical RAM of this machine in bytes (0 if unknown).
uint64_t physicalMemoryBytes();

}  // namespace cl
