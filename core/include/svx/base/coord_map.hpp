// structvox — voxel coordinate -> i32 map (open addressing, linear probing; grows at half
// load). Lookups and inserts only: it is never iterated, so its layout (a function of the
// capacity) cannot reach any result.
#pragma once

#include <array>
#include <vector>

#include "svx/base/types.hpp"

namespace svx {

class CoordMap {
 public:
  void reserve(size_t n) {
    if (n * 2 > keys_.size()) rehash(capacity_for(n));
  }
  // Inserts (p, v) unless p is present; returns whether it inserted.
  bool insert(const std::array<i32, 3>& p, i32 v) {
    if ((size_ + 1) * 2 > keys_.size()) rehash(keys_.empty() ? size_t(64) : keys_.size() * 2);
    const u64 k = pack(p);
    u64 s = mix(k) & mask_;
    while (keys_[s] != kEmpty) {
      if (keys_[s] == k) return false;
      s = (s + 1) & mask_;
    }
    keys_[s] = k;
    vals_[s] = v;
    ++size_;
    return true;
  }
  i32 find(const std::array<i32, 3>& p) const {
    if (keys_.empty()) return -1;
    const u64 k = pack(p);
    u64 s = mix(k) & mask_;
    while (keys_[s] != kEmpty) {
      if (keys_[s] == k) return vals_[s];
      s = (s + 1) & mask_;
    }
    return -1;
  }
  bool contains(const std::array<i32, 3>& p) const { return find(p) >= 0; }
  size_t size() const { return size_; }

 private:
  static constexpr u64 kEmpty = ~u64(0);
  static u64 pack(const std::array<i32, 3>& p) {  // 21 bits per axis (±2^20 voxels)
    const u64 o = u64(1) << 20;
    return ((u64(p[0]) + o) & 0x1FFFFF) | (((u64(p[1]) + o) & 0x1FFFFF) << 21) | (((u64(p[2]) + o) & 0x1FFFFF) << 42);
  }
  static u64 mix(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
  }
  static size_t capacity_for(size_t n) {
    size_t cap = 64;
    while (cap < n * 2) cap <<= 1;
    return cap;
  }
  void rehash(size_t cap) {
    std::vector<u64> keys(cap, kEmpty);
    std::vector<i32> vals(cap, -1);
    const u64 mask = cap - 1;
    for (size_t i = 0; i < keys_.size(); ++i) {
      if (keys_[i] == kEmpty) continue;
      u64 s = mix(keys_[i]) & mask;
      while (keys[s] != kEmpty) s = (s + 1) & mask;
      keys[s] = keys_[i];
      vals[s] = vals_[i];
    }
    keys_.swap(keys);
    vals_.swap(vals);
    mask_ = mask;
  }
  std::vector<u64> keys_;
  std::vector<i32> vals_;
  u64 mask_ = 0;
  size_t size_ = 0;
};

}  // namespace svx
