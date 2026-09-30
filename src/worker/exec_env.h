// Worker-internal state behind clBlockContext, plus the kernel registry lookup.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/protocol.h"
#include "kudalite/cl_kernel.h"

namespace cl {

class DsmClient;
class PageCache;
class ThreadPool;

struct ExecEnv {
  DsmClient* dsm = nullptr;
  PageCache* cache = nullptr;
  ThreadPool* pool = nullptr;
  uint32_t workerId = kNoWorker;
  std::vector<std::vector<uint8_t>> scratch;  // recycled per block
  size_t scratchNext = 0;
};

clKernelFn findKernel(const std::string& name);
std::vector<std::string> kernelNames();

}  // namespace cl
