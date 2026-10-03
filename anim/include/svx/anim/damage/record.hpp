// Explicit little-endian records: no struct padding, pointer values or native enums on disk.
#pragma once
#include <algorithm>
#include <bit>
#include <span>
#include <string>
#include <vector>
#include "svx/anim/math.hpp"
#include "svx/anim/voxel/model.hpp"
namespace svx::anim::record {
struct Writer {
  std::vector<u8> bytes;
  void integer(u64 value, int size = 4) {
    for (int i = 0; i < size; ++i) bytes.push_back(u8(value >> (8 * i)));
  }
  void number(f64 value) { integer(std::bit_cast<u64>(value), 8); }
  void vector(const V3& v) {
    number(v.x);
    number(v.y);
    number(v.z);
  }
  void quat(const Quat& q) {
    number(q.x);
    number(q.y);
    number(q.z);
    number(q.w);
  }
  void block(std::span<const u8> value) {
    integer(value.size());
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
  void string(const std::string& value) { block({reinterpret_cast<const u8*>(value.data()), value.size()}); }
};
struct Reader {
  std::span<const u8> bytes;
  size_t at = 0;
  bool ok = true;
  u64 integer(int size = 4) {
    if (!ok || size_t(size) > bytes.size() - at) {
      ok = false;
      return 0;
    }
    u64 out = 0;
    for (int i = 0; i < size; ++i) out |= u64(bytes[at++]) << (8 * i);
    return out;
  }
  f64 number() {
    const f64 x = std::bit_cast<f64>(integer(8));
    if (!std::isfinite(x)) ok = false;
    return x;
  }
  V3 vector() {
    const f64 x = number(), y = number(), z = number();
    return {x, y, z};
  }
  Quat quat() {
    const f64 x = number(), y = number(), z = number(), w = number();
    return {x, y, z, w};
  }
  std::span<const u8> block() {
    const auto n = integer();
    if (!ok || n > bytes.size() - at) {
      ok = false;
      return {};
    }
    auto out = bytes.subspan(at, size_t(n));
    at += size_t(n);
    return out;
  }
  std::string string() {
    const auto b = block();
    if (b.size() > 1024) {
      ok = false;
      return {};
    }
    return std::string(b.begin(), b.end());
  }
  bool done() const { return ok && at == bytes.size(); }
};
// A part's cells and what is kept per cell (VoxelPart: slot, shade, stain, tissue). `channels`
// false: cells and shades only (the records before stains and tissues had their own channels).
inline void write_cells(Writer& w, const VoxelPart& p) {
  w.block(p.cells);
  w.block(p.shade);
  w.block(p.stain);
  w.block(p.tissue);
}
inline bool read_cells(Reader& r, VoxelPart& p, size_t cells, bool channels) {
  const auto data = r.block(), shade = r.block();
  const auto stain = channels ? r.block() : std::span<const u8>{}, tissue = channels ? r.block() : std::span<const u8>{};
  if (!r.ok || data.size() != cells) return false;
  for (const auto& b : {shade, stain, tissue})
    if (!b.empty() && b.size() != cells) return false;
  for (u8 v : data)
    if (v > kSlotCount) return false;
  for (u8 v : tissue)
    if (v > kTissueCount) return false;
  p.cells.assign(data.begin(), data.end());
  p.shade.assign(shade.begin(), shade.end());
  p.stain.assign(stain.begin(), stain.end());
  p.tissue.assign(tissue.begin(), tissue.end());
  p.count = i32(std::count_if(p.cells.begin(), p.cells.end(), [](u8 c) { return c != 0; }));
  return true;
}
}  // namespace svx::anim::record
