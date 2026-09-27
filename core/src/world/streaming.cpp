#include "svx/world/streaming.hpp"

#include <algorithm>
#include <cmath>

namespace svx {

namespace {

inline u64 mix64(u64 x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

struct Box {
  IVec3 lo, hi;  // [lo, hi) voxels
};

class CitySource final : public ChunkSource {
 public:
  CitySource(u64 seed, f64 extent_m, f64 h) : seed_(seed), h_(h) {
    extent_ = static_cast<i32>(extent_m / h);
    blocks_ = std::max(1, extent_ / kPitch);
    // one extra chunk of margin around the city
    const i32 n = (blocks_ * kPitch + kChunk - 1) / kChunk + 1;
    clo_ = {-1, -1, -1};
    chi_ = {n, n, (kGround + kMaxStoreys * (kStorey + kSlab)) / kChunk + 2};
  }

  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    const IVec3 b{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
    const IVec3 e{b[0] + kChunk, b[1] + kChunk, b[2] + kChunk};
    out.assign(kChunkVox, kAir);
    bool any = false;
    auto fill = [&](const Box& bx, Vox v) {
      const i32 x0 = std::max(bx.lo[0], b[0]), x1 = std::min(bx.hi[0], e[0]);
      const i32 y0 = std::max(bx.lo[1], b[1]), y1 = std::min(bx.hi[1], e[1]);
      const i32 z0 = std::max(bx.lo[2], b[2]), z1 = std::min(bx.hi[2], e[2]);
      if (x0 >= x1 || y0 >= y1 || z0 >= z1) return;
      any = any || v != kAir;
      for (i32 x = x0; x < x1; ++x)
        for (i32 y = y0; y < y1; ++y)
          for (i32 z = z0; z < z1; ++z) out[chunk_index({x, y, z})] = v;
    };
    // ground: anchored rock slab below z = 0 over the whole city (plus margin)
    const i32 gmin = -kChunk, gmax = blocks_ * kPitch + kChunk;
    fill({{gmin, gmin, -kGround}, {gmax, gmax, 0}}, make_vox(MaterialId::Rock, true));
    // buildings of the blocks overlapping this chunk
    const i32 bx0 = std::max(0, b[0] / kPitch - 1), bx1 = std::min(blocks_ - 1, (e[0] - 1) / kPitch);
    const i32 by0 = std::max(0, b[1] / kPitch - 1), by1 = std::min(blocks_ - 1, (e[1] - 1) / kPitch);
    for (i32 bx = bx0; bx <= bx1; ++bx)
      for (i32 by = by0; by <= by1; ++by) building(bx, by, fill);
    return any;
  }

  bool coarse(const IVec3& lo, const IVec3& n, i32 f, std::vector<Vox>& out) const override {
    out.assign(size_t(n[0]) * n[1] * n[2], kAir);
    auto fill = [&](const Box& bx, Vox v) {
      // solid: every cell the box overlaps (thin slabs / columns / walls survive);
      // air: only cells it covers entirely (window bands stay walls unless they span a cell)
      i32 c0[3], c1[3];
      for (int q = 0; q < 3; ++q) {
        const f64 a = f64(bx.lo[q] - lo[q]) / f, b = f64(bx.hi[q] - lo[q]) / f;
        c0[q] = static_cast<i32>(v == kAir ? std::ceil(a) : std::floor(a));
        c1[q] = static_cast<i32>(v == kAir ? std::floor(b) : std::ceil(b));
        c0[q] = std::max(c0[q], 0);
        c1[q] = std::min(c1[q], n[q]);
        if (c0[q] >= c1[q]) return;
      }
      for (i32 x = c0[0]; x < c1[0]; ++x)
        for (i32 y = c0[1]; y < c1[1]; ++y)
          for (i32 z = c0[2]; z < c1[2]; ++z) out[(size_t(x) * n[1] + y) * n[2] + z] = v;
    };
    const i32 gmin = -kChunk, gmax = blocks_ * kPitch + kChunk;
    fill({{gmin, gmin, -kGround}, {gmax, gmax, 0}}, make_vox(MaterialId::Rock, true));
    const IVec3 e{lo[0] + n[0] * f, lo[1] + n[1] * f, lo[2] + n[2] * f};
    const i32 bx0 = std::max(0, lo[0] / kPitch - 1), bx1 = std::min(blocks_ - 1, (e[0] - 1) / kPitch);
    const i32 by0 = std::max(0, lo[1] / kPitch - 1), by1 = std::min(blocks_ - 1, (e[1] - 1) / kPitch);
    for (i32 bx = bx0; bx <= bx1; ++bx)
      for (i32 by = by0; by <= by1; ++by) building(bx, by, fill);
    return true;
  }

  IVec3 chunk_lo() const override { return clo_; }
  IVec3 chunk_hi() const override { return chi_; }
  std::array<f64, 3> spawn_pos() const override {
    // in the street next to the central block
    const i32 mid = blocks_ / 2;
    return {h_ * (mid * kPitch - kStreet / 2), h_ * (mid * kPitch + kPitch / 2), -0.5 * h_ + 0.02};
  }
  std::array<f64, 3> spawn_dir() const override { return {0.0, 1.0, 0.1}; }

 private:
  static constexpr i32 kPitch = 192;   // 24 m block pitch
  static constexpr i32 kStreet = 48;   // 6 m street
  static constexpr i32 kGround = 4;
  static constexpr i32 kStorey = 24;   // 3 m
  static constexpr i32 kSlab = 2;
  static constexpr i32 kMaxStoreys = 14;

  template <typename Fill>
  void building(i32 bx, i32 by, Fill&& fill) const {
    u64 r = mix64(seed_ * 0x9E3779B97F4A7C15ull ^ (u64(u32(bx)) << 32) ^ u64(u32(by)));
    auto next = [&](i32 lo, i32 hi) {
      r = mix64(r + 0x632BE59BD9B4E019ull);
      return lo + static_cast<i32>(r % u64(hi - lo + 1));
    };
    if (next(0, 9) == 0) return;  // an empty lot now and then
    const Vox rc = make_vox(MaterialId::Rc, false);
    const Vox masonry = make_vox(MaterialId::Masonry, false);
    const i32 ox = bx * kPitch + kStreet / 2, oy = by * kPitch + kStreet / 2;
    const i32 foot = kPitch - kStreet;          // 144 voxels = 18 m
    const i32 bays = next(3, 5);
    const i32 bay = (foot - 3) / bays;
    const i32 X = bays * bay + 3;
    const i32 tall = next(0, 7) == 0;
    const i32 storeys = tall ? next(8, kMaxStoreys) : next(2, 6);
    const i32 col = 3;
    for (i32 s = 0; s < storeys; ++s) {
      const i32 z0 = s * (kStorey + kSlab);
      for (i32 ix = 0; ix <= bays; ++ix)
        for (i32 iy = 0; iy <= bays; ++iy)
          fill({{ox + ix * bay, oy + iy * bay, z0}, {ox + ix * bay + col, oy + iy * bay + col, z0 + kStorey}}, rc);
      fill({{ox, oy, z0 + kStorey}, {ox + X, oy + X, z0 + kStorey + kSlab}}, rc);
      // perimeter walls with a window band (masonry infill), two opposite sides
      for (int side = 0; side < 2; ++side) {
        const i32 yw = side == 0 ? oy : oy + X - 2;
        fill({{ox, yw, z0}, {ox + X, yw + 2, z0 + kStorey}}, masonry);
        for (i32 ix = 0; ix < bays; ++ix) {
          const i32 wx0 = ox + ix * bay + col + 3, wx1 = ox + (ix + 1) * bay - 3;
          if (wx1 > wx0) fill({{wx0, yw, z0 + 8}, {wx1, yw + 2, z0 + kStorey - 5}}, kAir);
        }
      }
    }
  }

  u64 seed_;
  f64 h_;
  i32 extent_ = 0, blocks_ = 1;
  IVec3 clo_{0, 0, 0}, chi_{1, 1, 1};
};

}  // namespace

std::unique_ptr<ChunkSource> make_city_source(u64 seed, f64 extent_m, f64 h) {
  return std::make_unique<CitySource>(seed, extent_m, h);
}

}  // namespace svx
