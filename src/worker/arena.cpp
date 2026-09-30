#include "worker/arena.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace cl {

Arena::Arena(uint64_t bytes) : size_(bytes) {
  if (bytes == 0) throw std::invalid_argument("arena size must be > 0");
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
  flags |= MAP_NORESERVE;
#endif
  void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (p == MAP_FAILED) {
    throw std::runtime_error("cannot map " + std::to_string(bytes) + " byte arena: " + std::strerror(errno));
  }
  base_ = static_cast<uint8_t*>(p);
}

Arena::~Arena() {
  if (base_ != nullptr) ::munmap(base_, size_);
}

uint64_t physicalMemoryBytes() {
  const long pages = ::sysconf(_SC_PHYS_PAGES);
  const long pageSize = ::sysconf(_SC_PAGESIZE);
  if (pages <= 0 || pageSize <= 0) return 0;
  return static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize);
}

}  // namespace cl
