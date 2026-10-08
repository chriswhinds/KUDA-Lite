// Copyright 2026 Christopher Hinds, Stratum Labs llc
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Unit tests for the pieces that need no cluster: serialisation, allocation, address
// translation, argument packing and the thread pool. Exit code 0 = all passed.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include "common/address_map.h"
#include "common/bytes.h"
#include "common/cli.h"
#include "common/freelist.h"
#include "common/net.h"
#include "common/protocol.h"
#include "common/telemetry.h"
#include "common/thread_pool.h"
#include "kudalite/cl_args.h"
#include "kudalite/clblas.h"

namespace {

int gFailures = 0;
std::vector<std::pair<const char*, std::function<void()>>>& tests() {
  static std::vector<std::pair<const char*, std::function<void()>>> t;
  return t;
}
struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { tests().emplace_back(name, std::move(fn)); }
};

#define CHECK(cond)                                                                   \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::fprintf(stderr, "  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++gFailures;                                                                    \
    }                                                                                 \
  } while (0)

#define TEST(name)                                   \
  void name();                                       \
  const Registrar reg_##name(#name, name);           \
  void name()

using namespace cl;

TEST(bytes_roundtrip) {
  ByteWriter w;
  w.put<uint8_t>(7).put<uint32_t>(0xDEADBEEF).put<uint64_t>(1ull << 40).put<double>(2.5);
  w.putString("hello");
  const uint8_t blob[3] = {1, 2, 3};
  w.putBlob(blob, 3);
  const auto buf = w.take();
  ByteReader r(buf);
  CHECK(r.get<uint8_t>() == 7);
  CHECK(r.get<uint32_t>() == 0xDEADBEEF);
  CHECK(r.get<uint64_t>() == (1ull << 40));
  CHECK(r.get<double>() == 2.5);
  CHECK(r.getString() == "hello");
  CHECK(r.getBlob() == std::vector<uint8_t>({1, 2, 3}));
  CHECK(r.remaining() == 0);
}

TEST(bytes_truncated_throws) {
  ByteWriter w;
  w.put<uint16_t>(1);
  const auto buf = w.take();
  ByteReader r(buf);
  bool threw = false;
  try {
    r.get<uint32_t>();
  } catch (const ProtocolError&) {
    threw = true;
  }
  CHECK(threw);
  ByteWriter s;
  s.put<uint32_t>(1000);  // claims 1000 bytes of string that are not there
  const auto sbuf = s.take();
  ByteReader rs(sbuf);
  threw = false;
  try {
    rs.getString();
  } catch (const ProtocolError&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(freelist_alloc_release_coalesce) {
  FreeListAllocator a(1024, 64);
  uint64_t x = 0, y = 0, z = 0;
  CHECK(a.allocate(100, &x) && x == 0);  // rounds to 128
  CHECK(a.allocate(64, &y) && y == 128);
  CHECK(a.allocate(64, &z) && z == 192);
  CHECK(a.used() == 256);
  uint64_t big = 0;
  CHECK(!a.allocate(1024, &big));
  CHECK(a.release(y));
  CHECK(!a.release(y));  // double free rejected
  CHECK(a.release(x));   // coalesces with y's hole -> [0,192)
  uint64_t w = 0;
  CHECK(a.allocate(192, &w) && w == 0);
  CHECK(a.release(w) && a.release(z));
  CHECK(a.used() == 0 && a.largestFree() == 1024);
}

Allocation sampleAllocation() {
  Allocation a;
  a.base = 0x100000;
  a.pageSize = 4096;
  a.size = 10 * 4096 + 100;  // 11 pages, the last one short
  a.owners = {7, 3, 5};
  a.localBase = {0, 1 << 20, 2 << 20};
  return a;
}

TEST(allocation_page_math) {
  const Allocation a = sampleAllocation();
  CHECK(a.pageCount() == 11);
  CHECK(a.pagesOnOwner(0) == 4 && a.pagesOnOwner(1) == 4 && a.pagesOnOwner(2) == 3);
  CHECK(a.localBytesOnOwner(2) == 3 * 4096);
  ByteWriter w;
  a.encode(w);
  const auto buf = w.take();
  ByteReader r(buf);
  const Allocation b = Allocation::decode(r);
  CHECK(b.base == a.base && b.size == a.size && b.pageSize == a.pageSize);
  CHECK(b.owners == a.owners && b.localBase == a.localBase);
}

TEST(address_map_resolve) {
  AddressMap map;
  map.add(sampleAllocation());
  std::vector<Extent> ext;
  // Whole allocation: 11 page extents covering exactly `size` bytes.
  CHECK(map.resolve(0x100000, 10 * 4096 + 100, &ext) == clSuccess);
  CHECK(ext.size() == 11);
  uint64_t sum = 0;
  for (const auto& e : ext) sum += e.len;
  CHECK(sum == 10 * 4096 + 100);
  // Page 4 -> owner index 1 (worker 3), second page on that owner.
  CHECK(ext[4].worker == 3 && ext[4].localOffset == (1u << 20) + 4096);
  CHECK(ext[10].len == 100 && ext[10].pageLen == 100);
  // A range starting mid-page and spanning two pages.
  ext.clear();
  CHECK(map.resolve(0x100000 + 4000, 200, &ext) == clSuccess);
  CHECK(ext.size() == 2);
  CHECK(ext[0].worker == 7 && ext[0].localOffset == 4000 && ext[0].len == 96 && ext[0].requestOffset == 0);
  CHECK(ext[1].worker == 3 && ext[1].localOffset == (1u << 20) && ext[1].len == 104 && ext[1].requestOffset == 96);
  CHECK(ext[1].pageGva == 0x100000 + 4096);
  // Out of range / unmapped.
  ext.clear();
  CHECK(map.resolve(0x100000 + 10 * 4096 + 50, 51, &ext) == clErrorInvalidDevicePointer);
  CHECK(map.resolve(0x0FFFFF, 1, &ext) == clErrorInvalidDevicePointer);
  CHECK(map.resolve(0x100000 + 10 * 4096 + 100, 1, &ext) == clErrorInvalidDevicePointer);
  CHECK(map.find(0x100000 + 17) != nullptr);
  CHECK(map.remove(0x100000) != nullptr);
  CHECK(map.find(0x100000) == nullptr);
}

TEST(coalesce_single_owner) {
  AddressMap map;
  Allocation a;
  a.base = 0x2000000;
  a.pageSize = 256;
  a.size = 4096;
  a.owners = {1};
  a.localBase = {512};
  map.add(a);
  std::vector<Extent> ext;
  CHECK(map.resolve(0x2000000 + 10, 3000, &ext) == clSuccess);
  CHECK(ext.size() > 1);
  coalesceExtents(&ext);
  CHECK(ext.size() == 1);
  CHECK(ext[0].worker == 1 && ext[0].localOffset == 522 && ext[0].len == 3000);
}

TEST(kernel_args_roundtrip) {
  clGemmParams<float> p{};
  p.M = 3;
  p.K = 5;
  p.alpha = 1.5f;
  p.C = 0xABCDEF;
  clKernelArgs pack;
  pack << p << uint32_t(42) << 2.0;
  clArgReader r(pack.data(), pack.size());
  const auto q = r.get<clGemmParams<float>>();
  CHECK(q.M == 3 && q.K == 5 && q.alpha == 1.5f && q.C == 0xABCDEF);
  CHECK(r.get<uint32_t>() == 42);
  CHECK(r.get<double>() == 2.0);
  bool threw = false;
  try {
    r.get<uint8_t>();
  } catch (const std::out_of_range&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(exec_request_and_cluster_map_roundtrip) {
  ExecBlocksRequest req;
  req.launchId = 99;
  req.kernel = "cl_sgemm";
  req.grid = clDim3(4, 5, 6);
  req.block = clDim3(128, 64);
  req.args = {9, 8, 7};
  req.blockBegin = 10;
  req.blockEnd = 20;
  ByteWriter w;
  req.encode(w);
  encodeClusterMap(w, {PeerAddr{0, "10.0.0.2", 7100}, PeerAddr{3, "pi-w3", 7101}});
  const auto buf = w.take();
  ByteReader r(buf);
  const auto got = ExecBlocksRequest::decode(r);
  CHECK(got.launchId == 99 && got.kernel == "cl_sgemm" && got.grid.z == 6 && got.block.y == 64);
  CHECK(got.args == req.args && got.blockBegin == 10 && got.blockEnd == 20);
  const auto peers = decodeClusterMap(r);
  CHECK(peers.size() == 2 && peers[1].id == 3 && peers[1].host == "pi-w3" && peers[1].port == 7101);
}

TEST(host_port_parsing) {
  std::string h;
  uint16_t p = 0;
  CHECK(parseHostPort("pi-ctrl", 7070, &h, &p) && h == "pi-ctrl" && p == 7070);
  CHECK(parseHostPort("10.0.0.1:9000", 7070, &h, &p) && h == "10.0.0.1" && p == 9000);
  CHECK(parseHostPort("[fe80::1]:81", 7070, &h, &p) && h == "fe80::1" && p == 81);
  CHECK(!parseHostPort("host:99999", 7070, &h, &p));
  CHECK(parseSize("64K") == 65536 && parseSize("2G") == (2ull << 30) && parseSize("512MiB") == (512ull << 20));
}

TEST(telemetry_sample_roundtrip) {
  TelemetrySample t;
  t.timestampMs = 1790000000123ull;
  t.memTotalBytes = 8ull << 30;
  t.memAvailableBytes = 5ull << 30;
  t.processThreads = 11;
  t.computeThreads = 4;
  t.busyThreads = 3;
  t.cpuPercent = 87.5f;
  t.cpuTempC = 61.25f;
  t.netRxBytes = 12345;
  t.currentKernel = "cl_sgemm";
  t.board = "Orange Pi 6 Plus";
  ByteWriter w;
  t.encode(w);
  w.put<uint32_t>(0xC0FFEE);  // whatever follows must still be readable
  const auto buf = w.take();
  ByteReader r(buf);
  const TelemetrySample u = TelemetrySample::decode(r);
  CHECK(u.timestampMs == t.timestampMs && u.memTotalBytes == t.memTotalBytes);
  CHECK(u.memAvailableBytes == t.memAvailableBytes && u.processThreads == 11);
  CHECK(u.computeThreads == 4 && u.busyThreads == 3 && u.cpuPercent == 87.5f && u.cpuTempC == 61.25f);
  CHECK(u.netRxBytes == 12345 && u.currentKernel == "cl_sgemm" && u.board == "Orange Pi 6 Plus");
  CHECK(r.get<uint32_t>() == 0xC0FFEE);

  // A newer node may append fields: an old reader skips them because the sample is a blob.
  ByteWriter body;
  ByteWriter inner;
  t.encode(inner);
  auto blob = inner.take();
  blob.push_back(0xAB);  // pretend v2 appended one byte
  const uint32_t len = static_cast<uint32_t>(blob.size() - 4);  // patch the blob length prefix
  std::memcpy(blob.data(), &len, 4);
  body.putBytes(blob.data(), blob.size());
  body.put<uint8_t>(7);
  const auto buf2 = body.take();
  ByteReader r2(buf2);
  CHECK(TelemetrySample::decode(r2).currentKernel == "cl_sgemm");
  CHECK(r2.get<uint8_t>() == 7);
}

TEST(telemetry_reads_version_1_samples) {
  // A 0.2.x node sends version 1: no board field. Build one by hand from a v2 encoding.
  TelemetrySample t;
  t.currentKernel = "k";
  t.board = "";
  ByteWriter w;
  t.encode(w);
  auto blob = w.take();
  blob.resize(blob.size() - 4);  // drop the empty board string (u32 length 0)
  const uint32_t len = static_cast<uint32_t>(blob.size() - 4);
  std::memcpy(blob.data(), &len, 4);
  const uint16_t v1 = 1;
  std::memcpy(blob.data() + 4, &v1, 2);
  ByteReader r(blob);
  const TelemetrySample u = TelemetrySample::decode(r);
  CHECK(u.currentKernel == "k" && u.board.empty() && r.remaining() == 0);
}

namespace {
void writeFile(const std::string& path, const std::string& text) {
  const std::string dir = path.substr(0, path.rfind('/'));
  CHECK(std::system(("mkdir -p '" + dir + "'").c_str()) == 0);
  std::ofstream(path, std::ios::binary) << text;
}

std::string fakeRoot(const char* name) {
  const std::string root = "/tmp/kudalite-fake-" + std::string(name) + "-" + std::to_string(::getpid());
  CHECK(std::system(("rm -rf '" + root + "'").c_str()) == 0);
  writeFile(root + "/proc/meminfo", "MemTotal:       65536000 kB\nMemAvailable:   60000000 kB\n");
  writeFile(root + "/proc/self/status", "Name:\tcl-worker\nThreads:\t17\nVmRSS:\t  102400 kB\n");
  writeFile(root + "/proc/stat", "cpu  100 0 100 800 0 0 0 0 0 0\n");
  return root;
}
}  // namespace

TEST(system_sampler_orange_pi_6_plus_layout) {
  // CIX P1: three cpufreq policies (A520 1.8 GHz, A720 2.4 GHz, A720 2.6 GHz) and several zones.
  const std::string root = fakeRoot("cix");
  writeFile(root + "/proc/device-tree/model", std::string("Orange Pi 6 Plus\0", 17));
  writeFile(root + "/sys/class/thermal/thermal_zone0/temp", "45000\n");
  writeFile(root + "/sys/class/thermal/thermal_zone1/temp", "61250\n");
  writeFile(root + "/sys/class/thermal/thermal_zone2/temp", "38000\n");
  writeFile(root + "/sys/class/thermal/thermal_zone3/temp", "999999\n");  // faulty sensor: ignored
  writeFile(root + "/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq", "1800000\n");
  writeFile(root + "/sys/devices/system/cpu/cpufreq/policy4/scaling_cur_freq", "2400000\n");
  writeFile(root + "/sys/devices/system/cpu/cpufreq/policy8/scaling_cur_freq", "2600000\n");
  SystemSampler sampler(root);
  TelemetrySample s;
  sampler.sample(&s);
#ifdef __linux__
  CHECK(s.board == "Orange Pi 6 Plus");
  CHECK(s.cpuTempC == 61.25f);    // hottest valid zone, not zone0
  CHECK(s.cpuFreqMHz == 2600);    // fastest cluster, not cpu0's LITTLE core
  CHECK(s.memTotalBytes == 65536000ull * 1024 && s.processThreads == 17);
#endif
  CHECK(std::system(("rm -rf '" + root + "'").c_str()) == 0);
}

TEST(system_sampler_raspberry_pi_5_layout) {
  const std::string root = fakeRoot("pi5");
  writeFile(root + "/proc/device-tree/model", std::string("Raspberry Pi 5 Model B Rev 1.0\0", 31));
  writeFile(root + "/sys/class/thermal/thermal_zone0/temp", "52300\n");
  writeFile(root + "/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq", "2400000\n");
  SystemSampler sampler(root);
  TelemetrySample s;
  sampler.sample(&s);
#ifdef __linux__
  CHECK(s.board == "Raspberry Pi 5 Model B Rev 1.0");
  CHECK(s.cpuTempC > 52.29f && s.cpuTempC < 52.31f);
  CHECK(s.cpuFreqMHz == 2400);
#endif
  CHECK(std::system(("rm -rf '" + root + "'").c_str()) == 0);
}

TEST(board_model_from_dmi) {
  const std::string root = fakeRoot("dmi");  // UEFI/ACPI boards have no device tree
  writeFile(root + "/sys/class/dmi/id/sys_vendor", "Orange Pi\n");
  writeFile(root + "/sys/class/dmi/id/product_name", "Orange Pi 6 Plus\n");
  CHECK(detectBoardModel(root) == "Orange Pi 6 Plus");  // vendor already in the product name
  writeFile(root + "/sys/class/dmi/id/product_name", "6 Plus\n");
  CHECK(detectBoardModel(root) == "Orange Pi 6 Plus");
  CHECK(std::system(("rm -rf '" + root + "'").c_str()) == 0);
}

TEST(system_sampler_reads_this_machine) {
  SystemSampler sampler;
  TelemetrySample a, b;
  sampler.sample(&a);
  sampler.sample(&b);
  CHECK(b.memTotalBytes > 0);
  CHECK(b.cpuCores >= 1);
  CHECK(b.timestampMs >= a.timestampMs);
  CHECK(b.cpuPercent >= 0.0f && b.cpuPercent <= 100.0f);
#ifdef __linux__
  CHECK(b.memAvailableBytes > 0 && b.memAvailableBytes <= b.memTotalBytes);
  CHECK(b.rssBytes > 0 && b.processThreads >= 1);
#endif
}

TEST(config_file_loading) {
  const std::string path = "/tmp/kudalite-test-" + std::to_string(::getpid()) + ".conf";
  {
    std::ofstream f(path);
    f << "# KUDA-Lite worker\n"
         "controller = pi5-ctl:7071   # trailing comment\n"
         "mem=6G\n"
         "\n"
         "advertise = \"10.0.0.12\"\n"
         "threads = 4\n";
  }
  const char* argv[] = {"cl-worker", "--threads", "2", "--config", path.c_str()};
  ArgParser args(5, const_cast<char**>(argv));
  std::string error;
  CHECK(args.loadConfigFile(path, &error));
  CHECK(args.get("controller", "") == "pi5-ctl:7071");
  CHECK(args.getSize("mem", 0) == (6ull << 30));
  CHECK(args.get("advertise", "") == "10.0.0.12");
  CHECK(args.getU64("threads", 0) == 2);  // the command line wins over the file
  {
    std::ofstream f(path);
    f << "controller pi5-ctl\n";  // missing '='
  }
  ArgParser bad(1, const_cast<char**>(argv));
  CHECK(!bad.loadConfigFile(path, &error) && error.find(":1:") != std::string::npos);
  CHECK(!bad.loadConfigFile(path + ".missing", &error));
  std::remove(path.c_str());
}

TEST(thread_pool_parallel_for) {
  ThreadPool pool(4);
  CHECK(pool.size() == 4);
  std::vector<int> hits(100000, 0);
  pool.parallelFor(hits.size(), [&](size_t b, size_t e) {
    for (size_t i = b; i < e; ++i) hits[i] += 1;
  });
  CHECK(std::accumulate(hits.begin(), hits.end(), 0) == 100000);
  CHECK(pool.busy() == 0);  // the busy-thread gauge returns to zero after the fork-join
  std::atomic<unsigned> maxBusy{0};
  pool.parallelFor(64, [&](size_t, size_t) {
    unsigned b = pool.busy();
    unsigned m = maxBusy.load();
    while (b > m && !maxBusy.compare_exchange_weak(m, b)) {
    }
  });
  CHECK(maxBusy.load() >= 1 && maxBusy.load() <= 4 && pool.busy() == 0);
  CHECK(std::all_of(hits.begin(), hits.end(), [](int h) { return h == 1; }));

  // Nested calls run inline instead of deadlocking.
  std::atomic<int> inner{0};
  pool.parallelFor(8, [&](size_t b, size_t e) {
    for (size_t i = b; i < e; ++i) pool.parallelFor(10, [&](size_t x, size_t y) { inner += int(y - x); });
  });
  CHECK(inner.load() == 80);

  // Exceptions propagate to the caller and the pool stays usable.
  bool threw = false;
  try {
    pool.parallelFor(1000, [](size_t b, size_t) {
      if (b >= 500) throw std::runtime_error("boom");
    });
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
  std::atomic<size_t> total{0};
  pool.parallelFor(1000, [&](size_t b, size_t e) { total += e - b; });
  CHECK(total.load() == 1000);
}

}  // namespace

int main() {
  for (const auto& [name, fn] : tests()) {
    const int before = gFailures;
    fn();
    std::printf("%-44s %s\n", name, gFailures == before ? "ok" : "FAILED");
  }
  std::printf("%s (%d failure%s)\n", gFailures == 0 ? "PASS" : "FAIL", gFailures, gFailures == 1 ? "" : "s");
  return gFailures == 0 ? 0 : 1;
}
