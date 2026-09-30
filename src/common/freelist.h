// First-fit free-list allocator over an abstract [0, capacity) range.
// The controller keeps one per worker to place allocation slices inside that worker's arena.
// Not thread-safe; the owner serialises access.
#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>

namespace cl {

class FreeListAllocator {
 public:
  explicit FreeListAllocator(uint64_t capacity = 0, uint64_t alignment = 64);

  /// Reserves `size` bytes (rounded up to the alignment). Returns false if no hole fits.
  bool allocate(uint64_t size, uint64_t* offset);

  /// Releases a block previously returned by allocate(). Returns false for unknown offsets.
  bool release(uint64_t offset);

  uint64_t capacity() const { return capacity_; }
  uint64_t used() const { return used_; }
  uint64_t largestFree() const;

 private:
  uint64_t capacity_;
  uint64_t align_;
  uint64_t used_ = 0;
  std::map<uint64_t, uint64_t> free_;            // offset -> length, non-adjacent holes
  std::unordered_map<uint64_t, uint64_t> live_;  // offset -> length
};

}  // namespace cl
