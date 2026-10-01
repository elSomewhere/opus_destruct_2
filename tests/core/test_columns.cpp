// Column content ranges (ChunkSource::column_range, docs/CORE.md §3 Streaming): a streamed world
// generates only the chunks of a column that hold content, and the floor under it; the solid fill
// below and the air above stay implicit until something changes them.
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "doctest.h"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

constexpr f64 kH = 0.125;
const Vox kRock = make_vox(MaterialId::Rock, true);
const Vox kConcrete = make_vox(MaterialId::Concrete, false);
const Vox kWood = make_vox(MaterialId::Wood, false);

// Ground at a height per column (chunks): a 5 km mountain east of x = 20 chunks, low ground
// elsewhere, over an extent 600 m deep and 7 km tall. Each column's content is the chunk under
// its surface, the surface chunk (rock up to voxel 16) and the one above it.
class Relief final : public ChunkSource {
 public:
  static i32 top(i32 cx, i32 cy) { return cx >= 20 ? 1300 : 2 + ((cx + cy) & 1); }
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    {
      std::lock_guard<std::mutex> lk(m_);
      ++calls;
      const i32 t = top(cc[0], cc[1]);
      if (cc[2] < t - 1 || cc[2] > t + 1) ++outside;
    }
    const i32 t = top(cc[0], cc[1]);
    if (cc[2] > t) return false;
    out.assign(kChunkVox, kAir);
    for (int i = 0; i < kChunkVox; ++i)
      if (cc[2] < t || (i & 31) < 16) out[size_t(i)] = kRock;
    return true;
  }
  IVec3 chunk_lo() const override { return {-64, -64, -150}; }
  IVec3 chunk_hi() const override { return {64, 64, 1750}; }
  void column_range(i32 cx, i32 cy, i32* z_lo, i32* z_hi, Vox* below) const override {
    const i32 t = top(cx, cy);
    *z_lo = t - 1;
    *z_hi = t + 2;
    *below = kRock;
  }
  mutable std::mutex m_;
  mutable i64 calls = 0, outside = 0;
};

World streamed(std::shared_ptr<const ChunkSource> src, const V3& focus, f64 load_radius = 20.0) {
  World w;
  VoxelGrid g;
  g.h = kH;
  w.load(std::move(g));
  StreamConfig sc;
  sc.load_radius = load_radius;
  sc.evict_radius = load_radius + 16.0;
  sc.chunks_per_tick = 4096;
  sc.archive_mb = 0.0;
  w.enable_streaming(std::move(src), sc);
  w.set_focus(focus);
  return w;
}

V3 ground(i32 cx, i32 cy) {
  // (the surface of a column's middle: voxel 16 of its top chunk)
  return V3{(cx * kChunk + 16) * kH, (cy * kChunk + 16) * kH, (Relief::top(cx, cy) * kChunk + 16) * kH};
}

}  // namespace

TEST_CASE("columns: only the content of each column is generated, under a 5 km mountain and over a 600 m deep extent") {
  auto src = std::make_shared<Relief>();
  World w = streamed(src, ground(2, 2));
  for (int t = 0; t < 4; ++t) w.tick();
  const WorldStats s = w.stats();
  // a column: its floor, the chunk under the surface, the surface chunk and the one above
  const i64 cols = s.resident_chunks / 4;
  CHECK(s.resident_chunks == cols * 4);
  CHECK(cols > 30);
  CHECK(cols < 400);
  CHECK(src->outside == 0);
  CHECK(src->calls == cols * 3);  // (the floor is the column's fill: never asked of the source)
  i32 z_lo = 0, z_hi = 0;
  Vox below = kAir;
  REQUIRE(w.column_range(2, 2, &z_lo, &z_hi, &below));
  CHECK(z_lo == Relief::top(2, 2) - 1);
  CHECK(z_hi == Relief::top(2, 2) + 2);
  CHECK(below == kRock);
  // the floor is stored (rock), the fill under it implicit: not resident, solid
  CHECK(w.chunk_resident({2, 2, z_lo - 1}));
  CHECK(w.grid().get(2 * kChunk, 2 * kChunk, (z_lo - 1) * kChunk) == kRock);
  CHECK_FALSE(w.chunk_resident({2, 2, z_lo - 5}));
  CHECK(w.grid().chunk({2, 2, z_lo - 5}) == nullptr);
  // the air above: known
  CHECK(w.chunk_resident({2, 2, z_hi + 40}));
}

TEST_CASE("columns: a mountain 5 km high streams whole (no cut at the extent's old 1024 chunks)") {
  auto src = std::make_shared<Relief>();
  World w = streamed(src, ground(24, 2));
  for (int t = 0; t < 4; ++t) w.tick();
  CHECK(src->outside == 0);
  // its surface is there, and a ray from above hits it
  const V3 g = ground(24, 2);
  CHECK(w.grid().get(24 * kChunk + 16, 2 * kChunk + 16, Relief::top(24, 2) * kChunk + 10) == kRock);
  const RayHit h = w.raycast(V3{g.x, g.y, g.z + 20.0}, V3{0, 0, -1}, 40.0);
  CHECK(h.hit);
  CHECK(std::fabs(h.pos.z - (g.z - 0.5 * kH)) < 0.2);
}

TEST_CASE("columns: a crater into the implicit fill is carved, its floor stored; it comes back after its column went") {
  auto src = std::make_shared<Relief>();
  World w = streamed(src, ground(2, 2));
  for (int t = 0; t < 2; ++t) w.tick();
  i32 z_lo = 0, z_hi = 0;
  Vox below = kAir;
  REQUIRE(w.column_range(2, 2, &z_lo, &z_hi, &below));
  // a carve 3 m deep under the floor chunk: into the implicit fill
  const V3 deep{(2 * kChunk + 16) * kH, (2 * kChunk + 16) * kH, ((z_lo - 1) * kChunk - 24) * kH};
  CHECK(w.grid().chunk({2, 2, z_lo - 2}) == nullptr);
  w.carve(deep, 2.0);
  w.tick();
  const IVec3 c{2 * kChunk + 16, 2 * kChunk + 16, (z_lo - 1) * kChunk - 24};
  CHECK(w.grid().get(c) == kAir);                              // carved
  CHECK(w.grid().get(c[0], c[1], c[2] - 20) == kRock);         // its floor is stored rock
  CHECK(w.grid().chunk({2, 2, z_lo - 2}) != nullptr);          // the fill was made
  // the column goes out of range and comes back: the crater with it
  w.set_focus(ground(-40, -40));
  for (int t = 0; t < 40; ++t) w.tick();
  CHECK(w.grid().chunk({2, 2, z_lo - 2}) == nullptr);
  w.set_focus(ground(2, 2));
  for (int t = 0; t < 40; ++t) w.tick();
  CHECK(w.grid().get(c) == kAir);
  CHECK(w.grid().get(c[0], c[1], c[2] - 20) == kRock);
}

TEST_CASE("columns: the air above a column's content does not hold a structure (the fill below does)") {
  // a wooden beam hanging in the top content chunk, touching nothing: knocked, it falls
  class Hanging final : public ChunkSource {
   public:
    bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
      if (cc[2] == 0) {
        out.assign(kChunkVox, kAir);
        for (int i = 0; i < kChunkVox; ++i)
          if ((i & 31) < 8) out[size_t(i)] = kRock;
        return true;
      }
      if (cc[2] == 1 && cc[0] == 0 && cc[1] == 0) {
        // a beam along x at the very top of the chunk (its top faces against the implicit air)
        out.assign(kChunkVox, kAir);
        for (i32 x = 4; x < 28; ++x)
          for (i32 y = 14; y < 18; ++y)
            for (i32 z = 28; z < 32; ++z) out[size_t((x * kChunk + y) * kChunk + z)] = kWood;
        return true;
      }
      return false;
    }
    IVec3 chunk_lo() const override { return {-8, -8, -4}; }
    IVec3 chunk_hi() const override { return {8, 8, 200}; }
    void column_range(i32, i32, i32* z_lo, i32* z_hi, Vox* below) const override {
      *z_lo = 0;
      *z_hi = 2;
      *below = kRock;
    }
  };
  World w = streamed(std::make_shared<Hanging>(), V3{2.0, 2.0, 2.0});
  for (int t = 0; t < 4; ++t) w.tick();
  REQUIRE(w.grid().get(10, 15, 60) == kWood);
  CHECK(w.chunk_resident({0, 0, 2}));  // (the air above: known)
  w.shoot(V3{10 * kH, 15 * kH, 60 * kH}, 0.1, 50.0);
  int pieces = 0;
  for (int t = 0; t < 30; ++t) {
    w.tick();
    for (const WorldEvent& e : w.take_events())
      if (e.kind == WorldEvent::Kind::PieceAdded) ++pieces;
  }
  CHECK(pieces >= 1);
  CHECK(w.grid().get(10, 15, 60) == kAir);
}

TEST_CASE("columns: voxels written into the implicit air above the content are a piece that lands") {
  World w = streamed(std::make_shared<Relief>(), ground(2, 2));
  for (int t = 0; t < 2; ++t) w.tick();
  const V3 g = ground(2, 2);
  const IVec3 base{2 * kChunk + 8, 2 * kChunk + 8, static_cast<i32>(g.z / kH) + 56};  // 7 m up: chunks above z_hi
  std::vector<VoxelEdit> crate;
  for (i32 x = 0; x < 6; ++x)
    for (i32 y = 0; y < 6; ++y)
      for (i32 z = 0; z < 6; ++z) crate.push_back({{base[0] + x, base[1] + y, base[2] + z}, kConcrete});
  CHECK(w.set_voxels(crate) == 216);
  i32 z_lo = 0, z_hi = 0;
  Vox below = kAir;
  REQUIRE(w.column_range(2, 2, &z_lo, &z_hi, &below));
  CHECK(base[2] >= z_hi * kChunk);
  int pieces = 0;
  for (int t = 0; t < 300; ++t) {
    w.tick();
    for (const WorldEvent& e : w.take_events())
      if (e.kind == WorldEvent::Kind::PieceAdded) ++pieces;
  }
  CHECK(pieces >= 1);
  CHECK_FALSE(w.pieces().empty());
  for (const PieceState& p : w.pieces()) CHECK(p.pos.z < g.z + 1.5);  // (landed on the ground)
}

TEST_CASE("columns: a source that does not say its ranges streams every chunk of its extent, as before") {
  class Flat final : public ChunkSource {
   public:
    bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
      if (cc[2] != 0) return false;
      out.assign(kChunkVox, kRock);
      return true;
    }
    IVec3 chunk_lo() const override { return {-8, -8, -2}; }
    IVec3 chunk_hi() const override { return {8, 8, 3}; }
  };
  World w = streamed(std::make_shared<Flat>(), V3{2.0, 2.0, 4.0}, 16.0);
  for (int t = 0; t < 4; ++t) w.tick();
  const WorldStats s = w.stats();
  CHECK(s.resident_chunks % 5 == 0);  // (whole columns: 5 chunks each)
  i32 z_lo = 0, z_hi = 0;
  Vox below = kRock;
  REQUIRE(w.column_range(0, 0, &z_lo, &z_hi, &below));
  CHECK(z_lo == -2);
  CHECK(z_hi == 3);
  CHECK(below == kAir);
}
