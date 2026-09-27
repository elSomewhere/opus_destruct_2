// Water (svx_env): a voxel fluid that falls, spreads, rests, presses on structures, floats
// pieces and puts out fires.
#include <memory>
#include <vector>

#include "doctest.h"
#include "svx/env/env.hpp"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

constexpr f64 kH = 0.125;
const Vox kRock = make_vox(MaterialId::Rock, true);
const Vox kConc = make_vox(MaterialId::Concrete, false);
const Vox kWood = make_vox(MaterialId::Wood, false);
const Vox kStone = make_vox(MaterialId::Stone, false);

void box(VoxelGrid& g, const IVec3& lo, const IVec3& hi, Vox v) {
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y) g.fill_column(x, y, lo[2], hi[2], v);
}

// A rock plate and a concrete basin on it (16 x 16 voxels inside, 2 m high).
VoxelGrid basin() {
  VoxelGrid g;
  g.h = kH;
  box(g, {-40, -40, -4}, {80, 80, 0}, kRock);
  box(g, {0, 0, 0}, {20, 20, 16}, kConc);
  box(g, {2, 2, 0}, {18, 18, 16}, kAir);
  g.compact();
  return g;
}

V3 at(const IVec3& p) { return {kH * p[0], kH * p[1], kH * p[2]}; }

struct Setup {
  World w;
  Environment env;
  explicit Setup(VoxelGrid g, const EnvConfig& c = {}) {
    env.attach(w, c);
    w.load(std::move(g));
  }
  void run(f64 s) {
    const int n = static_cast<int>(s / w.config().dt + 0.5);
    for (int t = 0; t < n; ++t) w.tick();
  }
  WaterSystem& water() { return *env.water(); }
  // total water (units) inside / outside the box [lo, hi) in x and y
  std::pair<i64, i64> water_in(const IVec3& lo, const IVec3& hi) const {
    i64 in = 0, out = 0;
    const int L = env.water()->water_layer();
    for (const auto& [k, c] : w.grid().chunks()) {
      if (c.layer[size_t(L)].empty()) continue;
      const IVec3 cc = unkey3(k);
      for (i32 i = 0; i < kChunkVox; ++i) {
        const u8 a = c.layer[size_t(L)][size_t(i)];
        if (!a) continue;
        const i32 x = cc[0] * kChunk + i / (kChunk * kChunk), y = cc[1] * kChunk + (i / kChunk) % kChunk;
        (x >= lo[0] && x < hi[0] && y >= lo[1] && y < hi[1] ? in : out) += a;
      }
    }
    return {in, out};
  }
};

}  // namespace

TEST_CASE("water: poured water falls, fills a basin level and rests; a breach drains it") {
  Setup s(basin());
  for (int k = 0; k < 6; ++k) s.water().pour(s.w, at({10, 10, 24 + 7 * k}), 0.45);
  const auto [poured, none] = s.water_in({2, 2, 0}, {18, 18, 0});
  CHECK(none == 0);
  s.run(12.0);
  const auto [in, out] = s.water_in({2, 2, 0}, {18, 18, 0});
  CHECK(out == 0);
  CHECK(in > poured * 99 / 100);  // (only thin films dry up)
  CHECK(in <= poured);
  CHECK(s.water().stats().active == 0);  // (at rest)
  // level: the same number of full layers everywhere
  const int L = s.water().water_layer();
  for (i32 x = 2; x < 18; x += 3)
    for (i32 y = 2; y < 18; y += 3) {
      CHECK(s.w.layer(L, {x, y, 3}) == 255);
      CHECK(s.w.layer(L, {x, y, 5}) == 0);
    }
  CHECK(s.water().stats().loads > 0);  // (it presses on the basin)
  // a hole in the wall: the water runs out
  std::vector<VoxelEdit> hole;
  for (i32 x = 8; x < 12; ++x)
    for (i32 y = 0; y < 2; ++y)
      for (i32 z = 0; z < 6; ++z) hole.push_back({{x, y, z}, kAir});
  s.w.set_voxels(hole);
  s.run(10.0);
  const auto [in2, out2] = s.water_in({2, 2, 0}, {18, 18, 0});
  CHECK(out2 > in / 4);
  CHECK(in2 < in * 3 / 4);
}

TEST_CASE("water: the pressure of a tank breaks a weak wall; the same wall stands dry") {
  // A rock tank (3 m deep) closed on one side by a stone pane (1 voxel, 2 m wide). (A glass
  // pane that thick holds: about a third of its strength.)
  auto tank = [] {
    VoxelGrid g;
    g.h = kH;
    box(g, {-40, -40, -4}, {80, 80, 0}, kRock);
    box(g, {0, 0, 0}, {20, 20, 26}, kRock);
    box(g, {2, 2, 0}, {18, 18, 26}, kAir);
    box(g, {2, 0, 0}, {18, 2, 26}, kAir);  // (the open side ...)
    box(g, {2, 1, 0}, {18, 2, 24}, kStone);  // (... closed by the pane)
    g.compact();
    return g;
  };
  Setup dry(tank());
  dry.w.bake();
  dry.run(3.0);
  CHECK(dry.w.pieces().empty());
  Setup wet(tank());
  wet.w.bake();
  std::vector<LayerEdit> fill;
  for (i32 x = 2; x < 18; ++x)
    for (i32 y = 2; y < 18; ++y)
      for (i32 z = 0; z < 22; ++z) fill.push_back({{x, y, z}, 255});
  wet.w.set_layer(wet.water().water_layer(), fill);
  bool broke = false;
  for (int k = 0; k < 40 && !broke; ++k) {
    wet.run(0.25);
    broke = !wet.w.pieces().empty() || wet.w.stats().bonds_broken > 0;
  }
  CHECK(wet.water().stats().loads > 0);
  CHECK(broke);
  wet.run(8.0);
  CHECK(wet.water_in({2, 2, 0}, {18, 18, 0}).second > 0);  // (and out it comes)
}

TEST_CASE("water: wood floats, stone sinks") {
  VoxelGrid g;
  g.h = kH;
  box(g, {-40, -40, -4}, {80, 80, 0}, kRock);
  box(g, {0, 0, 0}, {40, 40, 20}, kRock);
  box(g, {2, 2, 0}, {38, 38, 20}, kAir);  // a pool 4.5 m square, 2.5 m deep
  g.compact();
  Setup s(std::move(g));
  std::vector<LayerEdit> fill;
  for (i32 x = 2; x < 38; ++x)
    for (i32 y = 2; y < 38; ++y)
      for (i32 z = 0; z < 16; ++z) fill.push_back({{x, y, z}, 255});
  s.w.set_layer(s.water().water_layer(), fill);
  // a wooden beam and a stone block above the water: they fall in
  std::vector<VoxelEdit> drop;
  for (i32 x = 8; x < 24; ++x)
    for (i32 y = 8; y < 11; ++y)
      for (i32 z = 24; z < 27; ++z) drop.push_back({{x, y, z}, kWood});
  for (i32 x = 24; x < 30; ++x)
    for (i32 y = 26; y < 32; ++y)
      for (i32 z = 24; z < 30; ++z) drop.push_back({{x, y, z}, kStone});
  s.w.set_voxels(drop);
  s.run(12.0);
  REQUIRE(s.w.pieces().size() == 2);
  f64 wood_z = 0.0, stone_z = 0.0;
  for (const PieceState& p : s.w.pieces()) (p.mass / p.voxels < 0.125 * 0.125 * 0.125 * 1000.0 ? wood_z : stone_z) = p.pos.z;
  const f64 surface = kH * 15.5;
  CHECK(wood_z > surface - 0.25);  // (floating: about half under)
  CHECK(wood_z < surface + 0.25);
  CHECK(stone_z < kH * 4.0);  // (on the bottom)
  CHECK(s.water().stats().floating == 2);
}

TEST_CASE("water: puts out a fire") {
  VoxelGrid g;
  g.h = kH;
  box(g, {-40, -40, -4}, {80, 80, 0}, kRock);
  box(g, {0, 4, 0}, {24, 5, 24}, kWood);
  g.compact();
  Setup s(std::move(g));
  s.env.fire()->ignite(s.w, at({12, 4, 4}), 0.3);
  s.run(8.0);
  REQUIRE(s.env.fire()->stats().burning > 10);
  // a hose over the burning planks: water runs down the wall and quenches it
  for (int k = 0; k < 90; ++k) {
    for (i32 x = 0; x < 24; x += 3) s.water().pour(s.w, at({x, 3, 22}), 0.2);
    s.w.tick();
  }
  s.run(3.0);
  CHECK(s.env.fire()->stats().burning == 0);
}

namespace {

// Rock ground with a lake (a pit of water at rest) in chunk (0, 0, 0).
class Lake final : public ChunkSource {
 public:
  bool generate(const IVec3& c, std::vector<Vox>& out) const override {
    out.assign(kChunkVox, kAir);
    if (c[2] == -1) {
      std::fill(out.begin(), out.end(), kRock);
      return true;
    }
    if (c[2] != 0) return false;
    for (i32 x = 0; x < kChunk; ++x)
      for (i32 y = 0; y < kChunk; ++y)
        for (i32 z = 0; z < 8; ++z)
          if (c[0] != 0 || c[1] != 0 || x < 4 || x >= 28 || y < 4 || y >= 28) out[size_t(chunk_index({x, y, z}))] = kRock;
    return true;
  }
  bool generate_layer(const IVec3& c, const std::string& layer, std::vector<u8>& out) const override {
    if (layer != "water" || c[0] != 0 || c[1] != 0 || c[2] != 0) return false;
    out.assign(kChunkVox, 0);
    for (i32 x = 4; x < 28; ++x)
      for (i32 y = 4; y < 28; ++y)
        for (i32 z = 0; z < 7; ++z) out[size_t(chunk_index({x, y, z}))] = 255;
    return true;
  }
  IVec3 chunk_lo() const override { return {-16, -16, -1}; }
  IVec3 chunk_hi() const override { return {16, 16, 2}; }
};

}  // namespace

TEST_CASE("water: streamed: a generated lake rests; poured water is kept with its chunk; saved in deltas") {
  World w;
  Environment env;
  env.attach(w);
  VoxelGrid g;
  g.h = kH;
  w.load(std::move(g));
  StreamConfig sc;
  sc.load_radius = 12.0;
  sc.evict_radius = 16.0;
  sc.chunks_per_tick = 400;
  w.enable_streaming(std::make_shared<Lake>(), sc);
  w.set_focus(V3{0, 0, 0});
  for (int t = 0; t < 30; ++t) w.tick();
  WaterSystem& ws = *env.water();
  REQUIRE(w.layer(ws.water_layer(), {10, 10, 3}) == 255);
  CHECK(ws.stats().active == 0);
  // a bucket poured beside the lake, on the ground: a puddle
  ws.pour(w, at({40, 40, 10}), 0.4);
  for (int t = 0; t < 1800 && (t < 10 || ws.stats().active > 0); ++t) w.tick();
  REQUIRE(ws.stats().active == 0);
  i64 puddle = 0;
  for (i32 x = 30; x < 50; ++x)
    for (i32 y = 30; y < 50; ++y) puddle += w.layer(ws.water_layer(), {x, y, 8});
  REQUIRE(puddle > 0);
  const std::vector<u8> delta = w.save_delta();
  // away and back: the puddle comes back from the archive, at rest
  w.set_focus(V3{120, 0, 0});
  for (int t = 0; t < 20; ++t) w.tick();
  CHECK_FALSE(w.chunk_resident({1, 1, 0}));
  CHECK(ws.stats().active == 0);
  w.set_focus(V3{0, 0, 0});
  for (int t = 0; t < 20; ++t) w.tick();
  i64 again = 0;
  for (i32 x = 30; x < 50; ++x)
    for (i32 y = 30; y < 50; ++y) again += w.layer(ws.water_layer(), {x, y, 8});
  CHECK(again == puddle);
  CHECK(w.layer(ws.water_layer(), {10, 10, 3}) == 255);
  // and a delta carries it
  World b;
  Environment eb;
  eb.attach(b);
  VoxelGrid g2;
  g2.h = kH;
  b.load(std::move(g2));
  b.enable_streaming(std::make_shared<Lake>(), sc);
  b.set_focus(V3{0, 0, 0});
  REQUIRE(b.load_delta(delta));
  for (int t = 0; t < 20; ++t) b.tick();
  i64 loaded = 0;
  for (i32 x = 30; x < 50; ++x)
    for (i32 y = 30; y < 50; ++y) loaded += b.layer(eb.water()->water_layer(), {x, y, 8});
  CHECK(loaded == puddle);
}

TEST_CASE("water: deterministic, and within its step budget") {
  EnvConfig c;
  c.water_config.max_active = 200;
  Setup a(basin(), c), b(basin(), c);
  for (Setup* s : {&a, &b})
    for (int k = 0; k < 4; ++k) s->water().pour(s->w, at({10, 10, 24 + 7 * k}), 0.45);
  for (int k = 0; k < 10; ++k) {
    a.run(1.0);
    b.run(1.0);
  }
  CHECK(a.water().state_hash() == b.water().state_hash());
  CHECK(a.w.state_hash() == b.w.state_hash());
  // (slower, but it gets there)
  a.run(30.0);
  CHECK(a.water().stats().active == 0);
  CHECK(a.water_in({2, 2, 0}, {18, 18, 0}).second == 0);
}

// ---- hardening (audit regressions)

TEST_CASE("water: a tank across a chunk boundary: its bottom walls feel the water above the boundary") {
  // a concrete tank from z = 16 to z = 50, full to z = 46 (the chunk boundary at z = 32)
  VoxelGrid g;
  g.h = kH;
  box(g, {-40, -40, -4}, {80, 80, 16}, kRock);
  box(g, {0, 0, 16}, {20, 20, 50}, kConc);
  box(g, {2, 2, 16}, {18, 18, 50}, kAir);
  const int L = g.add_layer({"water", true, LayerBind::Air});
  for (i32 x = 2; x < 18; ++x)
    for (i32 y = 2; y < 18; ++y)
      for (i32 z = 16; z < 46; ++z) g.set_layer(L, {x, y, z}, 255);
  g.compact();
  Setup s(std::move(g));
  s.w.bake();
  s.run(2.0);
  const IVec3 wall{1, 10, 17};  // (a bottom wall voxel: in the lower chunk)
  const f64 full = s.w.probe_utilization(wall);
  REQUIRE(full > 0.0);
  // most of the water above the boundary drained: the lower chunk's water did not change, its
  // load did
  for (i32 x = 3; x < 18; x += 3)
    for (i32 y = 3; y < 18; y += 3) s.water().drain(s.w, at({x, y, 40}), 0.75);
  s.run(3.0);
  i64 upper = 0;
  for (i32 x = 2; x < 18; ++x)
    for (i32 y = 2; y < 18; ++y)
      for (i32 z = 32; z < 46; ++z) upper += s.w.layer(s.water().water_layer(), {x, y, z}) > 0;
  REQUIRE(upper < 16 * 16 * 14 / 3);
  for (i32 x = 2; x < 18; x += 5)
    for (i32 y = 2; y < 18; y += 5) REQUIRE(s.w.layer(s.water().water_layer(), {x, y, 31}) == 255);
  const f64 half = s.w.probe_utilization(wall);
  CHECK(half < 0.8 * full);
}

TEST_CASE("water: switched off, its loads leave the world; bad settings are brought into range") {
  Setup s(basin());
  for (int k = 0; k < 6; ++k) s.water().pour(s.w, at({10, 10, 24 + 7 * k}), 0.45);
  s.run(10.0);
  REQUIRE(s.water().stats().loads > 0);
  WaterConfig c = s.water().config();
  c.loads = false;
  s.water().configure(c);
  s.run(0.2);
  CHECK(s.water().stats().loads == 0);
  WaterConfig bad;
  bad.step_s = -1.0;
  bad.fall = 0;
  bad.max_active = -5;
  bad.density = std::nan("");
  bad.drag = 1e9;
  WaterSystem ws(bad);
  CHECK(ws.config().fall >= 1);
  CHECK(ws.config().max_active >= 1);
  CHECK(ws.config().density == 1000.0);
  CHECK(ws.config().drag <= 20.0);
  s.water().pour(s.w, {1e12, 0, 0}, 1.0);  // (out of range: nothing)
  s.water().drain(s.w, {0, std::nan(""), 0}, 1.0);
}

TEST_CASE("water: the same session from a load, whatever came before it") {
  auto session = [](Setup& s) {
    s.w.load(basin());
    for (int k = 0; k < 3; ++k) s.water().pour(s.w, at({10, 10, 24 + 7 * k}), 0.45);
    for (int t = 0; t < 240; ++t) s.w.tick();
    return s.w.session_hash();
  };
  Setup a(basin()), b(basin());
  b.water().pour(b.w, at({10, 10, 30}), 0.45);
  b.run(3.0);
  CHECK(session(a) == session(b));
}

namespace {

// Rock ground and a rock wall, a 4 m concrete cantilever out of it (it stands only designed).
class Ledge final : public ChunkSource {
 public:
  bool generate(const IVec3& c, std::vector<Vox>& out) const override {
    out.assign(kChunkVox, kAir);
    bool any = false;
    for (i32 x = 0; x < kChunk; ++x)
      for (i32 y = 0; y < kChunk; ++y)
        for (i32 z = 0; z < kChunk; ++z) {
          const IVec3 p{c[0] * kChunk + x, c[1] * kChunk + y, c[2] * kChunk + z};
          Vox v = kAir;
          if (p[2] < 0 || (p[0] < 0 && p[0] >= -8 && p[2] < 40 && p[1] >= -8 && p[1] < 16)) v = kRock;
          else if (p[0] >= 0 && p[0] < 31 && p[1] >= 0 && p[1] < 3 && p[2] >= 24 && p[2] < 27) v = kConc;
          if (v != kAir) {
            out[size_t(chunk_index({x, y, z}))] = v;
            any = true;
          }
        }
    return any;
  }
  IVec3 chunk_lo() const override { return {-8, -8, -1}; }
  IVec3 chunk_hi() const override { return {8, 8, 3}; }
};

}  // namespace

TEST_CASE("water: water on a streamed structure (also archived and back) does not stop its design on first touch") {
  auto run = [](bool evict) {
    World w;
    Environment env;
    env.attach(w);
    VoxelGrid g;
    g.h = kH;
    w.load(std::move(g));
    StreamConfig sc;
    sc.load_radius = 14.0;
    sc.evict_radius = 18.0;
    sc.chunks_per_tick = 400;
    w.enable_streaming(std::make_shared<Ledge>(), sc);
    w.set_focus(V3{2, 0, 3});
    for (int t = 0; t < 10; ++t) w.tick();
    REQUIRE(w.grid().get(20, 1, 25) == kConc);
    // a puddle on the beam (a persistent layer: the chunk's changes)
    env.water()->pour(w, at({20, 1, 28}), 0.2);
    for (int t = 0; t < 120; ++t) w.tick();
    if (evict) {
      w.set_focus(V3{90, 0, 3});
      for (int t = 0; t < 10; ++t) w.tick();
      REQUIRE_FALSE(w.chunk_resident({0, 0, 0}));
      w.set_focus(V3{2, 0, 3});
      for (int t = 0; t < 10; ++t) w.tick();
      REQUIRE(w.chunk_resident({0, 0, 0}));
    }
    // touched: designed (strengthened), it stands
    w.set_loads(7, {{{30, 1, 26}, V3{0, 0, -1}}});
    for (int t = 0; t < 180; ++t) w.tick();
    CHECK(w.pieces().empty());
    CHECK(w.stats().strengthened_voxels > 0);
  };
  run(false);
  run(true);
}
