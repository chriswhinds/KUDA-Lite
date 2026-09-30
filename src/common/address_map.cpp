#include "common/address_map.h"

#include <algorithm>
#include <mutex>

namespace cl {

void Allocation::encode(ByteWriter& w) const {
  w.put<uint64_t>(base).put<uint64_t>(size).put<uint64_t>(pageSize);
  w.put<uint32_t>(static_cast<uint32_t>(owners.size()));
  for (uint32_t id : owners) w.put<uint32_t>(id);
  for (uint64_t off : localBase) w.put<uint64_t>(off);
}

Allocation Allocation::decode(ByteReader& r) {
  Allocation a;
  a.base = r.get<uint64_t>();
  a.size = r.get<uint64_t>();
  a.pageSize = r.get<uint64_t>();
  const uint32_t n = r.get<uint32_t>();
  if (n == 0 || a.pageSize == 0 || n > r.remaining()) throw ProtocolError("bad allocation record");
  a.owners.resize(n);
  a.localBase.resize(n);
  for (auto& id : a.owners) id = r.get<uint32_t>();
  for (auto& off : a.localBase) off = r.get<uint64_t>();
  return a;
}

void AddressMap::add(Allocation a) {
  auto rec = std::make_shared<const Allocation>(std::move(a));
  std::unique_lock<std::shared_mutex> lk(mu_);
  allocs_[rec->base] = std::move(rec);
}

std::shared_ptr<const Allocation> AddressMap::remove(clDevPtr base) {
  std::unique_lock<std::shared_mutex> lk(mu_);
  auto it = allocs_.find(base);
  if (it == allocs_.end()) return nullptr;
  auto rec = std::move(it->second);
  allocs_.erase(it);
  return rec;
}

std::shared_ptr<const Allocation> AddressMap::find(clDevPtr addr) const {
  std::shared_lock<std::shared_mutex> lk(mu_);
  auto it = allocs_.upper_bound(addr);
  if (it == allocs_.begin()) return nullptr;
  --it;
  return it->second->contains(addr) ? it->second : nullptr;
}

std::vector<std::shared_ptr<const Allocation>> AddressMap::snapshot() const {
  std::shared_lock<std::shared_mutex> lk(mu_);
  std::vector<std::shared_ptr<const Allocation>> out;
  out.reserve(allocs_.size());
  for (const auto& [base, rec] : allocs_) out.push_back(rec);
  return out;
}

clError_t AddressMap::resolve(clDevPtr addr, uint64_t len, std::vector<Extent>* out) const {
  if (len == 0) return clSuccess;
  auto a = find(addr);
  if (!a) return clErrorInvalidDevicePointer;
  const uint64_t start = addr - a->base;
  if (len > a->size - start) return clErrorInvalidDevicePointer;

  const uint64_t ps = a->pageSize;
  const uint64_t n = a->owners.size();
  uint64_t off = start;
  uint64_t done = 0;
  while (done < len) {
    const uint64_t page = off / ps;
    const uint64_t inPage = off % ps;
    const uint64_t pageLen = std::min<uint64_t>(ps, a->size - page * ps);
    const uint64_t take = std::min<uint64_t>(len - done, pageLen - inPage);
    const size_t j = static_cast<size_t>(page % n);
    const uint64_t pageLocal = a->localBase[j] + (page / n) * ps;
    out->push_back(Extent{a->owners[j], pageLocal + inPage, take, done, a->base + page * ps, pageLocal, pageLen});
    off += take;
    done += take;
  }
  return clSuccess;
}

void coalesceExtents(std::vector<Extent>* extents) {
  auto& v = *extents;
  if (v.size() < 2) return;
  size_t w = 0;
  for (size_t i = 1; i < v.size(); ++i) {
    Extent& prev = v[w];
    const Extent& cur = v[i];
    if (cur.worker == prev.worker && prev.localOffset + prev.len == cur.localOffset &&
        prev.requestOffset + prev.len == cur.requestOffset) {
      prev.len += cur.len;
    } else {
      v[++w] = cur;
    }
  }
  v.resize(w + 1);
}

}  // namespace cl
