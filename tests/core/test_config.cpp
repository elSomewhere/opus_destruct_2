// A host's configuration and inputs, held where they mean something (audit regressions): NaN
// knobs, counts that would stall a tick, budgets of any value, a source's extent, the thread
// count, the tunables from a host's static constructors.
#include <climits>
#include <cmath>
#include <memory>
#include <vector>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/world/tunables.hpp"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

// (a host's global, constructed before main - in whatever order: the tunables answer already)
const i32 g_early_gravity = tunable_index("rigid.gravity");

constexpr f64 kH = 0.125;
const Vox kRock = make_vox(MaterialId::Rock, true);
const Vox kConcrete = make_vox(MaterialId::Concrete, false);

void box(VoxelGrid& g, const IVec3& lo, const IVec3& hi, Vox v) {
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y) g.fill_column(x, y, lo[2], hi[2], v);
}

// A 4 m x 4 m slab 3 m up on four legs, on an anchored rock plate (test_world's table).
VoxelGrid table_world() {
  VoxelGrid g;
  g.h = kH;
  box(g, {-8, -8, -4}, {40, 40, 0}, kRock);
  for (i32 lx : {0, 30})
    for (i32 ly : {0, 30}) box(g, {lx, ly, 0}, {lx + 2, ly + 2, 24}, kConcrete);
  box(g, {0, 0, 24}, {32, 32, 26}, kConcrete);
  g.compact();
  g.lo = {-8, -8, -4};
  g.hi = {40, 40, 26};
  return g;
}

void cut_legs(World& w) {
  for (i32 lx : {0, 30})
    for (i32 ly : {0, 30}) w.carve({kH * (lx + 0.5), kH * (ly + 0.5), 1.5}, 0.3);
}

// Rock ground at chunk height -1, over whatever extent it is given.
class Ground final : public ChunkSource {
 public:
  Ground(IVec3 lo, IVec3 hi) : lo_(lo), hi_(hi) {}
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    if (cc[2] != -1) return false;
    out.assign(kChunkVox, kRock);
    return true;
  }
  IVec3 chunk_lo() const override { return lo_; }
  IVec3 chunk_hi() const override { return hi_; }

 private:
  IVec3 lo_, hi_;
};

}  // namespace

TEST_CASE("config: the tunables answer from a host's static constructors") {
  CHECK(g_early_gravity >= 0);
  CHECK(g_early_gravity == tunable_index("rigid.gravity"));
}

TEST_CASE("config: a NaN knob takes its default; counts and fractions are held to their ranges") {
  WorldConfig c;
  c.stress_rtol = NAN;
  c.rigid.gravity = NAN;
  c.max_event_radius = NAN;
  c.memory.piece_mb = NAN;
  c.frag.jitter_lo = NAN;
  c.frag.scale = 1e9;
  c.dt = -1.0;
  c.rigid.substeps = INT_MAX;
  c.rigid.iterations = INT_MAX;
  c.rigid.position_iterations = -3;
  c.rigid.link_substeps = INT_MAX;
  c.rigid.manifold = INT_MAX;
  c.rigid.manifold_per_m = 1e300;
  c.body_stress_maxit = INT_MAX;
  c.body_check_ticks = INT_MAX;
  c.impact_round_fraction = 5.0;
  c.frag.min_voxels = INT_MAX;
  World w;
  w.configure(c);
  const WorldConfig& g = w.config();
  const WorldConfig d;
  CHECK(g.stress_rtol == d.stress_rtol);
  CHECK(g.rigid.gravity == d.rigid.gravity);
  CHECK(g.max_event_radius == d.max_event_radius);
  CHECK(g.memory.piece_mb == d.memory.piece_mb);
  CHECK(g.frag.jitter_lo == d.frag.jitter_lo);
  CHECK(g.frag.scale == 64.0);
  CHECK(g.dt == d.dt);
  CHECK(g.rigid.substeps == 64);
  CHECK(g.rigid.iterations == 256);
  CHECK(g.rigid.position_iterations == 0);
  CHECK(g.rigid.link_substeps == 64);
  CHECK(g.rigid.manifold == 4096);
  CHECK(g.rigid.manifold_per_m == 1e3);
  CHECK(g.body_stress_maxit == 10000);
  CHECK(g.body_check_ticks == (1 << 20));
  CHECK(g.impact_round_fraction == 1.0);
  CHECK(g.frag.min_voxels == kChunkVox);
  CHECK(w.rigid().par.substeps == 64);  // (the solver's own copy)
}

TEST_CASE("config: a world's voxel size is one it can work with") {
  for (const auto& [h, want] : std::vector<std::pair<f64, f64>>{{NAN, 0.125}, {0.0, 0.125}, {-1.0, 0.125}, {INFINITY, 0.125}, {1e9, 100.0}, {1e-9, 1e-3}, {0.0625, 0.0625}}) {
    World w;
    VoxelGrid g;
    g.h = h;
    w.load(std::move(g));
    CHECK(w.grid().h == want);
  }
}

TEST_CASE("config: a carve is clamped to max_event_radius, a NaN one too") {
  // a rock strip 40 m long: a carve of 1 km at its end leaves what is beyond the clamp
  WorldConfig c;
  c.max_event_radius = NAN;
  World w;
  w.configure(c);
  VoxelGrid g;
  g.h = kH;
  box(g, {0, 0, 0}, {320, 8, 4}, kRock);
  g.compact();
  w.load(std::move(g));
  w.carve({0.0, 0.5, 0.25}, 1000.0);
  w.tick();
  const f64 r = WorldConfig{}.max_event_radius;  // (its crater's edge is jagged by 9%)
  CHECK(w.grid().get({static_cast<i32>(0.85 * r / kH), 4, 2}) == kAir);
  CHECK(w.grid().get({static_cast<i32>(1.15 * r / kH), 4, 2}) != kAir);
}

TEST_CASE("config: counts that would stall a tick are bounded - a collapse at their bounds runs") {
  WorldConfig c;
  c.rigid.substeps = INT_MAX;
  c.rigid.iterations = INT_MAX;
  c.rigid.position_iterations = INT_MAX;
  c.rigid.busy_iterations = INT_MAX;
  c.body_stress_maxit = INT_MAX;
  World w;
  w.configure(c);
  w.load(table_world());
  w.bake();
  cut_legs(w);
  for (int t = 0; t < 20; ++t) w.tick();
  CHECK_FALSE(w.pieces().empty());
}

TEST_CASE("config: memory budgets of any value - an infinite one is no bound, on every platform") {
  auto session = [](f64 mb, WorldStats* st) {
    WorldConfig c;
    c.memory.piece_mb = mb;
    c.memory.structure_mb = mb;
    c.memory.fragment_cache_mb = mb;
    c.memory.cache_mb = mb;
    World w;
    w.configure(c);
    w.load(table_world());
    w.bake();
    cut_legs(w);
    for (int t = 0; t < 120; ++t) w.tick();
    *st = w.stats();
    return w.session_hash();
  };
  WorldStats huge, inf, none;
  const u64 h_huge = session(1e300, &huge);
  const u64 h_inf = session(INFINITY, &inf);
  session(-5.0, &none);  // (none: everything not held goes)
  for (const WorldStats* s : {&huge, &inf}) {
    CHECK(s->culled_pieces == 0);
    CHECK(s->released_solvers == 0);
    CHECK(s->dropped_structures == 0);
    CHECK(s->dropped_fragment_caches == 0);
  }
  CHECK(h_inf == h_huge);
  CHECK(none.dropped_fragment_caches > 0);
}

TEST_CASE("config: a source's extent is held within the key range, never inverted, a column's content at most kMaxColumnChunks tall") {
  StreamConfig sc;
  sc.load_radius = 1e300;  // (held to 1024 chunks)
  sc.evict_radius = INFINITY;
  sc.chunks_per_tick = INT_MAX;
  sc.forget_after_s = 1e300;
  {
    World w;
    VoxelGrid g;
    g.h = kH;
    w.load(std::move(g));
    w.enable_streaming(std::make_shared<Ground>(IVec3{INT_MIN, INT_MIN, -1}, IVec3{INT_MAX, INT_MAX, INT_MAX}), sc);
    const IVec3 lo = w.grid().lo, hi = w.grid().hi;
    CHECK(in_voxel_range(lo));
    CHECK(in_voxel_range({hi[0] - 1, hi[1] - 1, hi[2] - 1}));
    CHECK(lo[2] == -kChunk);
    CHECK(hi[2] == (kVoxelLimit / kChunk - 1) * kChunk);  // (the extent: the key range)
    w.set_focus(V3{0.0, 0.0, 1.0});
    CHECK(w.grid().get({0, 0, -2}) == kRock);  // (the ground around the first focus, at once)
    // (a column's content - the whole extent, as the source says nothing - is held to kMaxColumnChunks)
    i32 z_lo = 0, z_hi = 0;
    Vox below = kRock;
    REQUIRE(w.column_range(0, 0, &z_lo, &z_hi, &below));
    CHECK(z_lo == -1);
    CHECK(z_hi == -1 + kMaxColumnChunks);
    CHECK(below == kAir);
    // (beyond it, as the budget allows - a budget of INT_MAX chunks a tick is 4096: streaming goes on)
    for (int t = 0; t < 2; ++t) w.tick();
    CHECK(w.grid().chunk({7, 0, -1}) != nullptr);
  }
  {
    // an archive beyond any memory is held to its bound
    StreamConfig big;
    big.archive_mb = 1e300;
    World w;
    VoxelGrid g;
    g.h = kH;
    w.load(std::move(g));
    w.enable_streaming(std::make_shared<Ground>(IVec3{-4, -4, -1}, IVec3{4, 4, 1}), big);
    CHECK(w.stats().archive_capacity_mb <= (sizeof(size_t) >= 8 ? 4096.0 : 1024.0));
  }
  {
    // inverted: an empty world (nothing generated, nothing to fall through)
    World w;
    VoxelGrid g;
    g.h = kH;
    w.load(std::move(g));
    w.enable_streaming(std::make_shared<Ground>(IVec3{8, 8, 2}, IVec3{-8, -8, -2}), StreamConfig{});
    w.set_focus(V3{0.0, 0.0, 1.0});
    for (int t = 0; t < 3; ++t) w.tick();
    CHECK(w.grid().chunks().empty());
  }
}

TEST_CASE("config: a thread count beyond any machine's is held to 256") {
  const int before = num_threads();
  set_num_threads(1 << 20);
  CHECK(num_threads() == 256);
  set_num_threads(-7);
  CHECK(num_threads() == 1);
  set_num_threads(before);
  CHECK(num_threads() == before);
}
