#include "common/freelist.h"

#include <algorithm>

namespace cl {

FreeListAllocator::FreeListAllocator(uint64_t capacity, uint64_t alignment)
    : capacity_(capacity / alignment * alignment), align_(alignment) {
  if (capacity_ > 0) free_.emplace(0, capacity_);
}

bool FreeListAllocator::allocate(uint64_t size, uint64_t* offset) {
  if (size == 0) size = 1;
  if (size > capacity_) return false;
  size = (size + align_ - 1) / align_ * align_;
  for (auto it = free_.begin(); it != free_.end(); ++it) {
    if (it->second < size) continue;
    const uint64_t off = it->first;
    const uint64_t rest = it->second - size;
    free_.erase(it);
    if (rest > 0) free_.emplace(off + size, rest);
    live_.emplace(off, size);
    used_ += size;
    *offset = off;
    return true;
  }
  return false;
}

bool FreeListAllocator::release(uint64_t offset) {
  auto live = live_.find(offset);
  if (live == live_.end()) return false;
  uint64_t off = offset;
  uint64_t len = live->second;
  live_.erase(live);
  used_ -= len;

  // Coalesce with the following hole ...
  auto next = free_.lower_bound(off);
  if (next != free_.end() && off + len == next->first) {
    len += next->second;
    next = free_.erase(next);
  }
  // ... and with the preceding one.
  if (next != free_.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == off) {
      off = prev->first;
      len += prev->second;
      free_.erase(prev);
    }
  }
  free_.emplace(off, len);
  return true;
}

uint64_t FreeListAllocator::largestFree() const {
  uint64_t best = 0;
  for (const auto& [off, len] : free_) best = std::max(best, len);
  return best;
}

}  // namespace cl
