// structvox — the byte streams of persistence (world_io.cpp, world_session.cpp): little-endian
// integers, IEEE doubles bit for bit, and a reader that fails safe (a read past the end gives 0
// and marks it not ok).
#pragma once

#include <cstring>
#include <vector>

#include "svx/base/types.hpp"
#include "svx/base/vec.hpp"

namespace svx::world_detail {

inline void put32(std::vector<u8>& b, u32 v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
}
inline void put64(std::vector<u8>& b, u64 v) {
  for (int i = 0; i < 8; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
}
inline void putf(std::vector<u8>& b, f64 x) {
  u64 u;
  std::memcpy(&u, &x, 8);
  put64(b, u);
}
inline void put3(std::vector<u8>& b, const V3& v) {
  putf(b, v.x);
  putf(b, v.y);
  putf(b, v.z);
}
inline void putq(std::vector<u8>& b, const Quat& q) {
  putf(b, q.x);
  putf(b, q.y);
  putf(b, q.z);
  putf(b, q.w);
}

struct Rd {
  const std::vector<u8>& b;
  size_t p = 0;
  bool ok = true;
  bool need(size_t n) {
    if (n > b.size() - p) ok = false;
    return ok;
  }
  u8 u8_() { return need(1) ? b[p++] : 0; }
  u32 u32_() {
    if (!need(4)) return 0;
    u32 v = 0;
    for (int i = 0; i < 4; ++i) v |= u32(b[p++]) << (8 * i);
    return v;
  }
  u64 u64_() {
    if (!need(8)) return 0;
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= u64(b[p++]) << (8 * i);
    return v;
  }
  i32 i32_() { return static_cast<i32>(u32_()); }
  i64 i64_() { return static_cast<i64>(u64_()); }
  f64 f64_() {
    const u64 u = u64_();
    f64 x;
    std::memcpy(&x, &u, 8);
    return x;
  }
  V3 v3() {
    const f64 x = f64_(), y = f64_(), z = f64_();
    return V3{x, y, z};
  }
  Quat q4() {
    const f64 x = f64_(), y = f64_(), z = f64_(), w = f64_();
    return Quat{x, y, z, w};
  }
};

}  // namespace svx::world_detail
