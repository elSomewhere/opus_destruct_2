// structvox — the change archive of a streamed world (World internals, docs/CORE.md §4):
// compressed records of the chunks that were changed and are not resident, grouped by region.
//
// A bounded archive is one arena allocated once (pages of kPage bytes; a record takes a chain
// of pages): it never grows and never fragments. (It is not initialized: the system commits its
// pages as records first reach them.) What does not fit is the World's to decide
// (it forgets whole regions, least recently seen first, and they come back from the generator
// as they were). An unbounded archive (capacity 0) grows its arena as needed.
#pragma once

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

#include "svx/base/mem.hpp"
#include "svx/base/types.hpp"

namespace svx::world_detail {

class ChangeArchive {
 public:
  static constexpr size_t kPage = 1024;

  // Empties the archive; capacity in bytes (0: unbounded).
  void reset(size_t capacity) {
    bounded_ = capacity > 0;
    const size_t pages = bounded_ ? std::max<size_t>(1, capacity / kPage) : 0;
    data_.reset(pages ? new u8[pages * kPage] : nullptr);  // (default-initialized: untouched)
    std::vector<i32>(pages, -1).swap(next_);
    free_.clear();
    free_.reserve(pages);
    touched_ = 0;
    for (size_t p = pages; p-- > 0;) free_.push_back(static_cast<i32>(p));
    records_.clear();
    regions_.clear();
    columns_.clear();
  }
  bool bounded() const { return bounded_; }
  size_t capacity() const { return next_.size() * kPage; }
  size_t free_bytes() const { return free_.size() * kPage; }
  size_t used_bytes() const { return (next_.size() - free_.size()) * kPage; }
  static size_t pages_for(size_t bytes) { return std::max<size_t>(1, (bytes + kPage - 1) / kPage); }
  bool fits(size_t bytes) const { return !bounded_ || pages_for(bytes) <= free_.size(); }

  // Stores rec as chunk key's changes (replacing what it had), in region `region` last seen at
  // tick `seen`. False if a bounded archive has no room (nothing stored).
  bool put(u64 key, u64 region, const std::vector<u8>& rec, i64 seen) {
    erase(key);
    const size_t need = pages_for(rec.size());
    if (need > free_.size()) {
      if (bounded_) return false;
      grow(need);
    }
    i32 first = -1, prev = -1;
    for (size_t k = 0; k < need; ++k) {
      const i32 p = free_.back();
      free_.pop_back();
      touched_ = std::max(touched_, static_cast<size_t>(p) + 1);
      next_[size_t(p)] = -1;
      if (prev >= 0) next_[size_t(prev)] = p;
      else first = p;
      prev = p;
      const size_t off = k * kPage, n = std::min(kPage, rec.size() - std::min(rec.size(), off));
      if (n) std::copy(rec.begin() + static_cast<long>(off), rec.begin() + static_cast<long>(off + n), data_.get() + size_t(p) * kPage);
    }
    records_[key] = {first, static_cast<u32>(rec.size()), region};
    if (!(key >> 63)) {  // (a chunk's: indexed by its column)
      std::vector<i32>& zs = columns_[column_key(key)];
      const i32 z = z_of(key);
      zs.insert(std::lower_bound(zs.begin(), zs.end(), z), z);
    }
    Region& r = regions_[region];
    r.chunks.push_back(key);
    r.pages += static_cast<u32>(need);
    r.seen = std::max(r.seen, seen);
    return true;
  }

  bool has(u64 key) const { return records_.count(key) > 0; }

  // The record of chunk key (empty if none).
  std::vector<u8> get(u64 key) const {
    std::vector<u8> out;
    const auto it = records_.find(key);
    if (it == records_.end()) return out;
    out.resize(it->second.size);
    size_t off = 0;
    for (i32 p = it->second.first; p >= 0 && off < out.size(); p = next_[size_t(p)]) {
      const size_t n = std::min(kPage, out.size() - off);
      std::copy(data_.get() + size_t(p) * kPage, data_.get() + size_t(p) * kPage + n, out.begin() + static_cast<long>(off));
      off += n;
    }
    return out;
  }

  void erase(u64 key) {
    const auto it = records_.find(key);
    if (it == records_.end()) return;
    u32 pages = 0;
    for (i32 p = it->second.first; p >= 0;) {
      const i32 n = next_[size_t(p)];
      next_[size_t(p)] = -1;
      free_.push_back(p);
      ++pages;
      p = n;
    }
    const auto rt = regions_.find(it->second.region);
    if (rt != regions_.end()) {
      auto& c = rt->second.chunks;
      c.erase(std::remove(c.begin(), c.end(), key), c.end());
      rt->second.pages -= std::min(rt->second.pages, pages);
      if (c.empty()) regions_.erase(rt);
    }
    records_.erase(it);
    if (!(key >> 63)) {
      const auto ct = columns_.find(column_key(key));
      if (ct != columns_.end()) {
        auto& zs = ct->second;
        const auto zt = std::lower_bound(zs.begin(), zs.end(), z_of(key));
        if (zt != zs.end() && *zt == z_of(key)) zs.erase(zt);
        if (zs.empty()) columns_.erase(ct);
      }
    }
  }

  // The z of the chunks of a column (key3(cx, cy, 0)) that have records, ascending (nullptr: none).
  const std::vector<i32>* column(u64 col) const {
    const auto ct = columns_.find(col);
    return ct == columns_.end() ? nullptr : &ct->second;
  }

  // Forgets every record of a region; returns the chunks it held.
  std::vector<u64> forget(u64 region) {
    std::vector<u64> keys;
    const auto rt = regions_.find(region);
    if (rt == regions_.end()) return keys;
    keys = rt->second.chunks;
    for (u64 k : keys) erase(k);
    return keys;
  }

  struct Region {
    std::vector<u64> chunks;
    u32 pages = 0;
    i64 seen = 0;  // the last tick one of its chunks was resident
  };
  const std::unordered_map<u64, Region>& regions() const { return regions_; }
  void seen(u64 region, i64 tick) {
    const auto rt = regions_.find(region);
    if (rt != regions_.end()) rt->second.seen = std::max(rt->second.seen, tick);
  }
  std::vector<u64> keys() const {
    std::vector<u64> k;
    k.reserve(records_.size());
    for (const auto& [key, r] : records_) k.push_back(key);
    std::sort(k.begin(), k.end());
    return k;
  }
  size_t size() const { return records_.size(); }
  i64 memory_bytes() const {
    // (the arena's pages reached so far: the rest is reserved, not committed)
    i64 b = static_cast<i64>(touched_ * kPage) + vec_bytes(next_) + vec_bytes(free_) + hash_bytes(records_) + hash_bytes(regions_) + hash_bytes(columns_);
    for (const auto& [k, r] : regions_) b += vec_bytes(r.chunks);
    for (const auto& [k, z] : columns_) b += vec_bytes(z);
    return b;
  }

 private:
  struct Rec {
    i32 first = -1;
    u32 size = 0;
    u64 region = 0;
  };
  void grow(size_t need) {
    const size_t old = next_.size();
    const size_t pages = std::max(old + need, old + old / 2 + 64);
    std::unique_ptr<u8[]> bigger(new u8[pages * kPage]);
    if (old) std::copy(data_.get(), data_.get() + old * kPage, bigger.get());
    data_ = std::move(bigger);
    next_.resize(pages, -1);
    for (size_t p = pages; p-- > old;) free_.push_back(static_cast<i32>(p));
  }
  // (a chunk key's column, and its z: key3's packing)
  static u64 column_key(u64 key) { return (key & ~0x1FFFFFull) | (u64(1) << 20); }
  static i32 z_of(u64 key) { return static_cast<i32>(static_cast<i64>(key & 0x1FFFFF) - (i64(1) << 20)); }
  bool bounded_ = false;
  std::unique_ptr<u8[]> data_;
  size_t touched_ = 0;  // pages below this were used at some point (committed)
  std::vector<i32> next_;  // page -> next page of its record (-1: last)
  std::vector<i32> free_;  // free pages (a stack)
  std::unordered_map<u64, Rec> records_;
  std::unordered_map<u64, Region> regions_;
  std::unordered_map<u64, std::vector<i32>> columns_;  // column -> z of its chunks' records
};

}  // namespace svx::world_detail
