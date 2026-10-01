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
const Vox kWood = make_vox(MaterialId::Wood, false);

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

std::vector<VoxelEdit> block(const IVec3& lo, const IVec3& hi, Vox v) {
  std::vector<VoxelEdit> e;
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y)
      for (i32 z = lo[2]; z < hi[2]; ++z) e.push_back({{x, y, z}, v});
  return e;
}

bool all_asleep(const World& w) {
  for (const PieceState& p : w.pieces())
    if (!p.asleep) return false;
  return !w.pieces().empty();
}

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

TEST_CASE("streaming: a crate asleep on a beam goes out of range with it and comes back on it (nothing is woken to fall)") {
  StreamConfig sc;
  sc.load_radius = 12.0;
  sc.evict_radius = 16.0;
  sc.chunks_per_tick = 400;
  sc.archive_mb = 0.0;  // (unbounded: nothing is forgotten)
  World w = streamed(std::make_shared<Strip>(), sc, V3{16.0, 16.0, 0.0});
  for (int t = 0; t < 5; ++t) w.tick();
  // a timber beam set down on the ground, then a crate on the beam (the beam's id is the lower
  // one: its group is archived first)
  REQUIRE(w.set_voxels(block({112, 124, 1}, {144, 132, 5}, kWood)) > 0);
  for (int t = 0; t < 400 && !all_asleep(w); ++t) w.tick();
  REQUIRE(w.pieces().size() == 1);
  REQUIRE(all_asleep(w));
  REQUIRE(w.set_voxels(block({124, 124, 5}, {132, 132, 13}, kWood)) > 0);
  for (int t = 0; t < 400 && !(all_asleep(w) && w.pieces().size() == 2); ++t) w.tick();
  REQUIRE(w.pieces().size() == 2);
  REQUIRE(all_asleep(w));
  const std::vector<PieceState> before = w.pieces();
  int fell = 0;
  auto go = [&](f64 x, int ticks) {
    w.set_focus(V3{x, 16.0, 0.0});
    for (int t = 0; t < ticks; ++t) {
      w.tick();
      for (const WorldEvent& e : w.take_events()) fell += e.kind == WorldEvent::Kind::PieceRemoved && e.end == PieceEnd::OutOfWorld;
    }
  };
  go(160.0, 40);  // (out of range, both)
  CHECK(w.pieces().empty());
  CHECK(w.stats().archived_pieces == 2);
  go(16.0, 3);  // (back as they went: where they were, asleep - the crate was never woken to fall)
  const std::vector<PieceState> after = w.pieces();
  REQUIRE(after.size() == 2);
  for (size_t k = 0; k < after.size(); ++k) {
    CHECK(after[k].id == before[k].id);
    CHECK(norm(after[k].pos - before[k].pos) < 1e-9);
    CHECK(after[k].asleep);
  }
  CHECK(fell == 0);
}

TEST_CASE("streaming: a session's pieces come back over their ground (generated first), however slowly the world streams") {
  StreamConfig sc;
  sc.load_radius = 60.0;
  sc.evict_radius = 80.0;
  sc.chunks_per_tick = 400;
  World w = streamed(std::make_shared<CitySquare>(), sc, V3{0.0, 0.0, 0.0});
  for (int t = 0; t < 10; ++t) w.tick();
  // a crate falling 50 m from the focus (beyond what a first focus makes resident at once)
  REQUIRE(w.set_voxels(block({400, -4, 6}, {408, 4, 14}, kWood)) > 0);
  for (int t = 0; t < 3; ++t) w.tick();
  REQUIRE(w.pieces().size() == 1);
  REQUIRE_FALSE(w.pieces().front().asleep);
  const std::vector<u8> delta = w.save_delta();
  // a session of the same world, streaming at the default pace: its ground there comes in some
  // fifty ticks - the crate would have fallen through it by then (it lands in 15)
  StreamConfig slow = sc;
  slow.chunks_per_tick = StreamConfig{}.chunks_per_tick;
  World w2 = streamed(std::make_shared<CitySquare>(), slow, V3{0.0, 0.0, 0.0});
  REQUIRE(w2.load_delta(delta));
  for (int t = 0; t < 180; ++t) w2.tick();
  REQUIRE(w2.pieces().size() == 1);
  const f64 z = w2.pieces().front().pos.z;  // (resting on the ground: its top at 0.44 m, the crate 1 m)
  CHECK(z > 0.85);
  CHECK(z < 1.05);
}

TEST_CASE("streaming: a source's grid placed anew is there again when its chunk went and came back") {
  StreamConfig sc;
  sc.load_radius = 20.0;
  sc.evict_radius = 28.0;
  sc.chunks_per_tick = 400;
  sc.archive_mb = 0.0;
  World w = streamed(std::make_shared<BuildingSource>(), sc, V3{2.0, 2.0, 0.0});
  for (int t = 0; t < 5; ++t) w.tick();
  REQUIRE(w.grid(1) != nullptr);
  const GridFrame moved{V3{3.0, 1.5, 0.0}, Quat{0, 0, 0, 1}};
  REQUIRE(w.set_grid_frame(1, moved));
  for (int t = 0; t < 5; ++t) w.tick();
  auto go = [&](f64 x, int ticks) {
    w.set_focus(V3{x, 2.0, 0.0});
    for (int t = 0; t < ticks; ++t) w.tick();
  };
  go(-140.0, 30);  // (its home chunk out of range: it goes with it)
  REQUIRE(w.grid(1) == nullptr);
  go(2.0, 30);
  GridFrame f;
  REQUIRE(w.grid_frame(1, &f));
  CHECK(norm(f.origin - moved.origin) < 1e-9);
  CHECK(std::abs(f.rot.w) == doctest::Approx(1.0));
}

TEST_CASE("streaming: a source's grid removed in play never comes back - after its chunk went, nor in a session loaded away from it") {
  StreamConfig sc;
  sc.load_radius = 20.0;
  sc.evict_radius = 28.0;
  sc.chunks_per_tick = 400;
  sc.archive_mb = 0.0;
  World w = streamed(std::make_shared<BuildingSource>(), sc, V3{2.0, 2.0, 0.0});
  for (int t = 0; t < 5; ++t) w.tick();
  REQUIRE(w.remove_grid(1));
  auto go = [](World& at, f64 x, int ticks) {
    at.set_focus(V3{x, 2.0, 0.0});
    for (int t = 0; t < ticks; ++t) at.tick();
  };
  go(w, -140.0, 30);
  go(w, 2.0, 30);
  CHECK(w.grid(1) == nullptr);
  // saved, and loaded where the grid's chunk is not resident: the removal holds when it comes
  const std::vector<u8> delta = w.save_delta();
  World w2 = streamed(std::make_shared<BuildingSource>(), sc, V3{-140.0, 2.0, 0.0});
  for (int t = 0; t < 5; ++t) w2.tick();
  REQUIRE(w2.load_delta(delta));
  go(w2, 2.0, 30);
  CHECK(w2.grid(1) == nullptr);
}
