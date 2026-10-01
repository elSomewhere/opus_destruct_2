// Streaming's bookkeeping (audit regressions): the ids of grids made in play, what one call's
// edits make resident, what the change archive forgets when a record cannot fit.
#include <memory>
#include <vector>

#include "doctest.h"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

constexpr f64 kH = 0.125;
const Vox kRock = make_vox(MaterialId::Rock, true);
const Vox kConcrete = make_vox(MaterialId::Concrete, false);

// Rock ground, and a turned building of the source's - grid 1, as the city numbers its first
// block's - at home in chunk (0, 0, 0).
class BuildingSource final : public ChunkSource {
 public:
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    if (cc[2] != -1) return false;
    out.assign(kChunkVox, kRock);
    return true;
  }
  IVec3 chunk_lo() const override { return {-40, -4, -1}; }
  IVec3 chunk_hi() const override { return {40, 4, 2}; }
  std::vector<SourceGrid> grids(const IVec3& cc) const override {
    if (cc != IVec3{0, 0, 0}) return {};
    SourceGrid g;
    g.id = 1;
    g.origin = V3{1.0, 1.0, 0.0};
    g.rot = Quat{0, 0, 0.3826834323650898, 0.9238795325112867};
    return {g};
  }
  bool generate_grid(u32 id, VoxelGrid& out) const override {
    if (id != 1) return false;
    for (i32 x = 0; x < 8; ++x)
      for (i32 y = 0; y < 8; ++y) out.fill_column(x, y, 0, 16, kRock);
    out.compact();
    return true;
  }
};

// Ground 4 voxels deep over a 1 km square.
class CitySquare final : public ChunkSource {
 public:
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    if (cc[2] != 0) return false;
    out.assign(kChunkVox, kAir);
    for (int i = 0; i < kChunkVox; ++i)
      if (i % kChunk < 4) out[size_t(i)] = kRock;
    return true;
  }
  IVec3 chunk_lo() const override { return {-125, -125, -1}; }
  IVec3 chunk_hi() const override { return {125, 125, 3}; }
};

// A strip of rock ground, 80 chunks along x.
class Strip final : public ChunkSource {
 public:
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    if (cc[2] != -1) return false;
    out.assign(kChunkVox, kRock);
    return true;
  }
  IVec3 chunk_lo() const override { return {0, 0, -1}; }
  IVec3 chunk_hi() const override { return {80, 8, 1}; }
};

World streamed(std::shared_ptr<const ChunkSource> src, const StreamConfig& sc, const V3& focus) {
  World w;
  VoxelGrid g;
  g.h = kH;
  w.load(std::move(g));
  w.enable_streaming(std::move(src), sc);
  w.set_focus(focus);
  return w;
}

}  // namespace

TEST_CASE("streaming: a grid made in play never takes the id of a source's grid") {
  StreamConfig sc;
  sc.load_radius = 20.0;
  sc.evict_radius = 28.0;
  sc.chunks_per_tick = 400;
  World w = streamed(std::make_shared<BuildingSource>(), sc, V3{2.0, 2.0, 0.0});
  for (int t = 0; t < 5; ++t) w.tick();
  REQUIRE(w.grid(1) != nullptr);
  const i64 building = w.grid(1)->solid_count();
  // a crate dropped in: a grid of the session
  VoxelGrid crate;
  crate.h = kH;
  for (i32 x = 0; x < 4; ++x)
    for (i32 y = 0; y < 4; ++y) crate.fill_column(x, y, 0, 4, kConcrete);
  crate.compact();
  GridDesc d;
  d.frame = GridFrame{V3{-6.0, -6.0, 3.0}, Quat{0, 0, 0, 1}};
  d.base = false;
  const GridId cid = w.add_grid(d, std::move(crate));
  CHECK(cid >= 0x40000000u);
  for (int t = 0; t < 120; ++t) w.tick();  // (the crate falls: its grid empties and goes)
  REQUIRE(w.grid(1) != nullptr);
  CHECK(w.grid(1)->solid_count() == building);  // (the building, still in the world as grid 1)
  bool listed = false;
  for (GridId id : w.grids()) listed = listed || id == 1;
  CHECK(listed);
}

TEST_CASE("streaming: one call's edits far apart make their own chunks resident, not the world between them") {
  StreamConfig sc;
  sc.load_radius = 40.0;
  sc.evict_radius = 60.0;
  World w = streamed(std::make_shared<CitySquare>(), sc, V3{0, 0, 0});
  for (int t = 0; t < 3; ++t) w.tick();
  const i64 before = w.stats().resident_chunks;
  CHECK(w.set_voxels({{{10, 10, 4}, kConcrete}, {{3200, 3200, 4}, kConcrete}}) == 2);  // (400 m apart)
  CHECK(w.stats().resident_chunks <= before + 16);
  CHECK(w.grid().get(3200, 3200, 4) == kConcrete);
}

TEST_CASE("streaming: a change too large for the whole archive forgets nothing else") {
  StreamConfig sc;
  sc.load_radius = 12.0;
  sc.evict_radius = 16.0;
  sc.chunks_per_tick = 400;
  sc.archive_mb = 0.05;  // (51 pages of 1 KB)
  World w = streamed(std::make_shared<Strip>(), sc, V3{16.0, 16.0, 0.0});
  auto go = [&](f64 x, int ticks) {
    w.set_focus(V3{x, 16.0, 0.0});
    for (int t = 0; t < ticks; ++t) w.tick();
  };
  // small changes in four regions, each left behind
  for (int r = 1; r <= 4; ++r) {
    const f64 x = 32.0 * r + 16.0;
    go(x, 15);
    w.set_voxels({{{static_cast<i32>(x / kH), 128, -1}, kAir}});
    go(x, 5);
  }
  go(32.0 * 5 + 16.0, 20);
  REQUIRE(w.stats().archived_chunks == 4);
  REQUIRE(w.stats().forgotten_regions == 0);
  // a fifth region's chunk changed in a pattern that does not compress (a record of some 98 KB)
  std::vector<VoxelEdit> e;
  const i32 x0 = (5 * 256 + 64) & ~31, y0 = 128 & ~31;
  for (i32 x = 0; x < 32; ++x)
    for (i32 y = 0; y < 32; ++y)
      for (i32 z = 0; z < 32; ++z)
        if ((x + y + z) & 1) e.push_back({{x0 + x, y0 + y, -32 + z}, kAir});
  REQUIRE(w.set_voxels(e, kEditIsolated) > 0);
  go(32.0 * 5 + 16.0, 5);
  go(32.0 * 7 + 16.0, 20);
  CHECK(w.stats().forgotten_regions == 0);  // (the four small ones are kept: forgetting them would not make room)
  CHECK(w.stats().archived_chunks == 4);
  CHECK(w.stats().forgotten_chunks >= 1);   // (the large one alone is lost)
}
