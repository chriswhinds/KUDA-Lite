// KUDA-Lite — worker-side ("device") kernel API.
//
// Kernels are compiled into the worker executable and looked up by name at launch time.
// A KUDA-Lite kernel is written per *block*, not per thread: the function is called once for
// every block of the grid assigned to a worker, and spreads the work over the worker's CPU
// cores with ctx.parallelFor(). Global memory is reached only through ctx.read*/ctx.write*,
// which transparently fetch from / store to whichever workers own the addressed pages.
//
//   CL_KERNEL(my_scale) {                       // registered under the name "my_scale"
//     auto p = args.get<MyParams>();
//     float* buf = static_cast<float*>(ctx.scratch(p.n * sizeof(float)));
//     ctx.read(p.x + ctx.blockLinear * p.n * 4, buf, p.n * 4);
//     ctx.parallelFor(p.n, [&](size_t b, size_t e) { for (size_t i = b; i < e; ++i) buf[i] *= p.s; });
//     ctx.write(p.x + ctx.blockLinear * p.n * 4, buf, p.n * 4);
//   }
//
// Memory errors inside ctx.* throw clKernelFault; the worker reports the launch as failed.
#pragma once

#include <cstddef>
#include <exception>
#include <functional>

#include "kudalite/cl_args.h"
#include "kudalite/cl_types.h"

namespace cl {
struct ExecEnv;  // worker internals
}

/// Whether a global-memory read may be served from / populate the worker's page cache.
/// The cache lives for the duration of one launch; use Uncached for data read exactly once.
enum class clCache { Cached, Uncached };

class clKernelFault : public std::exception {
 public:
  explicit clKernelFault(clError_t code) : code_(code) {}
  clError_t code() const { return code_; }
  const char* what() const noexcept override { return "KUDA-Lite kernel memory fault"; }

 private:
  clError_t code_;
};

class clBlockContext {
 public:
  explicit clBlockContext(cl::ExecEnv* env) : env_(env) {}

  clDim3 gridDim;         // grid size of the launch
  clDim3 blockDim;        // block size of the launch (meaning defined by the kernel)
  clDim3 blockIdx;        // this block's coordinates
  uint64_t blockLinear = 0;  // blockIdx.x + gridDim.x * (blockIdx.y + gridDim.y * blockIdx.z)

  /// Copies `bytes` of global memory at `src` into local memory `dst`.
  void read(clDevPtr src, void* dst, size_t bytes, clCache cache = clCache::Cached);

  /// Gathers `rows` rows of `widthBytes` each; source rows are `srcPitch` bytes apart,
  /// destination rows `dstPitch` bytes apart. All rows are fetched in one batched round trip
  /// per owning worker.
  void read2D(void* dst, size_t dstPitch, clDevPtr src, size_t srcPitch, size_t widthBytes, size_t rows,
              clCache cache = clCache::Cached);

  /// Copies local memory into global memory (write-through to the owning workers).
  void write(clDevPtr dst, const void* src, size_t bytes);

  /// Scatters `rows` rows of `widthBytes` to global memory rows `dstPitch` bytes apart.
  void write2D(clDevPtr dst, size_t dstPitch, const void* src, size_t srcPitch, size_t widthBytes, size_t rows);

  /// Runs body(begin, end) over disjoint sub-ranges of [0, n) on all worker cores and waits.
  /// This is the KUDA-Lite equivalent of a block's threads. Nested calls run serially.
  void parallelFor(size_t n, const std::function<void(size_t begin, size_t end)>& body);

  /// Number of CPU threads parallelFor uses on this worker.
  unsigned numThreads() const;

  /// Per-block scratch memory (the analogue of __shared__). Each call within one block returns
  /// a distinct region of at least `bytes`; regions are recycled when the next block starts.
  /// Contents are NOT zeroed.
  void* scratch(size_t bytes);

  /// Id of the worker executing this block.
  uint32_t workerId() const;

 private:
  cl::ExecEnv* env_;
};

using clKernelFn = void (*)(clBlockContext& ctx, clArgReader& args);

/// Registers a kernel under `name` (normally via CL_KERNEL). Returns true.
bool clRegisterKernel(const char* name, clKernelFn fn);

/// Defines and registers a block kernel. Inside the body `ctx` and `args` are in scope.
#define CL_KERNEL(name)                                                                \
  static void name(clBlockContext& ctx, clArgReader& args);                            \
  [[maybe_unused]] static const bool cl_kernel_registered_##name = clRegisterKernel(#name, name); \
  static void name([[maybe_unused]] clBlockContext& ctx, [[maybe_unused]] clArgReader& args)
