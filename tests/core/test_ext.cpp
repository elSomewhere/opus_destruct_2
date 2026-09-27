// The world's extension points: layers, damage, loads, forces, systems (links svx_core only).
#include <memory>
#include <vector>

#include "doctest.h"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

constexpr f64 kH = 0.125;
const Vox kRock = make_vox(MaterialId::Rock, true);
const Vox kConc = make_vox(MaterialId::Concrete, false);

void box(VoxelGrid& g, const IVec3& lo, const IVec3& hi, Vox v) {
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y) g.fill_column(x, y, lo[2], hi[2], v);
}

// A rock plate with a concrete cantilever (3 x 3 voxels, 2 m) out of a rock wall.
VoxelGrid cantilever() {
  VoxelGrid g;
  g.h = kH;
  box(g, {-8, -8, -4}, {40, 16, 0}, kRock);
  box(g, {-8, -8, 0}, {0, 16, 40}, kRock);
  box(g, {0, 0, 24}, {16, 3, 27}, kConc);
  g.compact();
  return g;
}

// A 3 m high, 3 m long concrete wall (0.25 m thick) on the plate.
VoxelGrid wall() {
  VoxelGrid g;
  g.h = kH;
  box(g, {-8, -8, -4}, {40, 16, 0}, kRock);
  box(g, {0, 4, 0}, {24, 6, 24}, kConc);
  g.compact();
  return g;
}

struct Counting final : WorldSystem {
  int attached = 0, loads = 0, steps = 0, generated = 0, evicted = 0, changed = 0;
  const char* name() const override { return "counting"; }
  void attach(World&) override { ++attached; }
  void on_load(World&) override { ++loads; }
  void on_generated(World&, const std::vector<u64>& c) override { generated += static_cast<int>(c.size()); }
  void on_evicted(World&, const std::vector<u64>& c) override { evicted += static_cast<int>(c.size()); }
  void on_voxels_changed(World&, const std::vector<u64>& c) override { changed += static_cast<int>(c.size()); }
  void step(World&, f64) override { ++steps; }
  i64 memory_bytes() const override { return 1234; }
};

class Flat final : public ChunkSource {
 public:
  bool generate(const IVec3& c, std::vector<Vox>& out) const override {
    out.assign(kChunkVox, kAir);
    if (c[2] != -1) return false;
    std::fill(out.begin(), out.end(), kRock);
    return true;
  }
  IVec3 chunk_lo() const override { return {-16, -16, -1}; }
  IVec3 chunk_hi() const override { return {16, 16, 2}; }
  bool generate_layer(const IVec3& c, const std::string& layer, std::vector<u8>& out) const override {
    if (layer != "puddle" || c[2] != 0) return false;
    out.assign(kChunkVox, 0);
    out[0] = 200;  // (one voxel of each ground-level chunk)
    return true;
  }
};

}  // namespace

TEST_CASE("ext: layers are kept by name, saved when persistent, carried by pieces") {
  World w;
  const int heat = w.add_layer({"heat", false});
  const int soot = w.add_layer({"soot", true});
  CHECK(w.layer_index("damage") == World::kDamageLayer);
  CHECK(heat > 0);
  CHECK(w.add_layer({"soot", true}) == soot);
  w.load(cantilever());
  CHECK(w.set_layer(soot, {{{4, 1, 25}, 77}, {{5, 1, 25}, 78}}) == 2);
  CHECK(w.set_layer(heat, {{{4, 1, 25}, 99}}) == 1);
  CHECK(w.layer(soot, {4, 1, 25}) == 77);
  CHECK_FALSE(w.take_layer_changes(soot).empty());
  CHECK(w.take_layer_changes(soot).empty());
  // a delta keeps the persistent layer, not the transient one
  World b;
  b.add_layer({"heat", false});
  b.add_layer({"soot", true});
  b.load(cantilever());
  REQUIRE(b.load_delta(w.save_delta()));
  CHECK(b.layer(soot, {5, 1, 25}) == 78);
  CHECK(b.layer(heat, {4, 1, 25}) == 0);
  // the cantilever comes off its wall: the piece has its voxels' layers
  std::vector<VoxelEdit> cut;
  for (i32 y = 0; y < 3; ++y)
    for (i32 z = 24; z < 27; ++z) cut.push_back({{0, y, z}, kAir});
  w.set_voxels(cut);
  for (int t = 0; t < 5; ++t) w.tick();
  REQUIRE(w.pieces().size() == 1);
  const i64 id = w.pieces()[0].id;
  CHECK(w.piece_layer(id, soot, {4, 1, 25}) == 77);
  CHECK(w.piece_layer(id, heat, {4, 1, 25}) == 99);
  CHECK(w.layer(soot, {4, 1, 25}) == 0);  // (they left the grid with the voxels)
  CHECK(w.set_piece_layer(id, heat, {{{4, 1, 25}, 5}}) == 1);
  CHECK(w.piece_layer(id, heat, {4, 1, 25}) == 5);
}

TEST_CASE("ext: damage takes the strength of the sections it is in") {
  World w;
  w.load(cantilever());
  w.bake();
  REQUIRE(w.probe_utilization({8, 1, 25}) >= 0.0);
  for (int t = 0; t < 30; ++t) w.tick();
  CHECK(w.pieces().empty());
  // its root is burnt through, nearly
  std::vector<LayerEdit> dmg;
  for (i32 y = 0; y < 3; ++y)
    for (i32 z = 24; z < 27; ++z) dmg.push_back({{0, y, z}, 250});
  CHECK(w.set_layer(World::kDamageLayer, dmg) == 9);
  for (int t = 0; t < 60; ++t) w.tick();
  CHECK_FALSE(w.pieces().empty());  // it broke off
}

TEST_CASE("ext: loads on the static world break what they overload; the design pass designs for them") {
  auto push = [](World& w) {
    std::vector<VoxelLoad> l;
    for (i32 x = 0; x < 24; ++x)
      for (i32 z = 12; z < 24; ++z) l.push_back({{x, 5, z}, {0.0, 2.5e4, 0.0}});  // (7.2 MN over the upper half)
    w.set_loads(7, l);
  };
  {
    World w;
    w.load(wall());
    w.bake();
    push(w);
    for (int t = 0; t < 120; ++t) w.tick();
    CHECK(w.stats().bonds_broken > 0);
  }
  {
    World w;
    w.load(wall());
    push(w);
    w.bake();  // (designed for the load)
    for (int t = 0; t < 120; ++t) w.tick();
    CHECK(w.stats().bonds_broken == 0);
    w.set_loads(7, {});
    for (int t = 0; t < 30; ++t) w.tick();
    CHECK(w.stats().bonds_broken == 0);
  }
}

TEST_CASE("ext: forces on pieces; removing a piece's voxels splits it") {
  World w;
  w.load(cantilever());
  std::vector<VoxelEdit> cut;
  for (i32 y = 0; y < 3; ++y)
    for (i32 z = 24; z < 27; ++z) cut.push_back({{0, y, z}, kAir});
  w.set_voxels(cut);
  w.tick();
  REQUIRE(w.pieces().size() == 1);
  PieceState p = w.pieces()[0];
  const f64 z0 = p.pos.z;
  for (int t = 0; t < 30; ++t) {
    w.apply_force(p.id, w.pieces()[0].pos, {0, 0, 2.0 * p.mass * 9.81});  // (twice its weight, up)
    w.tick();
  }
  CHECK(w.pieces()[0].pos.z > z0);
  // burn through its middle: two pieces
  std::vector<IVec3> mid;
  for (i32 y = 0; y < 3; ++y)
    for (i32 z = 24; z < 27; ++z) mid.push_back({8, y, z});
  w.take_events();
  CHECK(w.remove_piece_voxels(p.id, mid, true));
  CHECK(w.piece(p.id) == nullptr);
  w.tick();  // (new pieces are announced at the end of a tick)
  CHECK(w.pieces().size() == 2);
  int dust = 0, added = 0;
  for (const WorldEvent& e : w.take_events()) {
    dust += e.kind == WorldEvent::Kind::Dust;
    added += e.kind == WorldEvent::Kind::PieceAdded && e.parent == p.id;
  }
  CHECK(dust >= 1);
  CHECK(added == 2);
}

TEST_CASE("ext: systems are told of loads, streaming and voxel changes, and stepped every tick") {
  auto sys = std::make_shared<Counting>();
  World w;
  w.add_system(sys);
  CHECK(sys->attached == 1);
  const int puddle = w.add_layer({"puddle", true});
  VoxelGrid g;
  g.h = kH;
  w.load(std::move(g));
  CHECK(sys->loads == 1);
  StreamConfig sc;
  sc.load_radius = 16.0;
  sc.evict_radius = 20.0;
  sc.chunks_per_tick = 400;
  w.enable_streaming(std::make_shared<Flat>(), sc);
  w.set_focus(V3{0, 0, 0});
  for (int t = 0; t < 5; ++t) w.tick();
  CHECK(sys->steps == 5);
  CHECK(sys->generated > 10);
  CHECK(w.layer(puddle, {0, 0, 0}) == 200);  // (the source's layer)
  w.carve({0.5, 0.5, -0.2}, 0.4);
  w.tick();
  CHECK(sys->changed > 0);
  w.set_focus(V3{60, 0, 0});
  for (int t = 0; t < 5; ++t) w.tick();
  CHECK(sys->evicted > 0);
  CHECK(w.memory().systems == 1234);
}

// ---- hardening (audit regressions)

TEST_CASE("ext: layer values belong to their voxel, to the air, or to the place") {
  World w;
  const int soot = w.add_layer({"soot", true, LayerBind::Solid});
  const int wet = w.add_layer({"wet", true, LayerBind::Air});
  const int mark = w.add_layer({"mark", true, LayerBind::Place});
  CHECK(w.add_layer({"soot", true, LayerBind::Place}) == -1);  // (another layer under that name)
  w.load(wall());
  w.bake();
  const IVec3 p{10, 4, 10}, air{10, 3, 10};
  w.set_layer(soot, {{p, 50}});
  w.set_layer(World::kDamageLayer, {{p, 40}});
  w.set_layer(mark, {{p, 60}, {air, 61}});
  w.set_layer(wet, {{air, 200}});
  REQUIRE(w.layer(soot, p) == 50);
  // the wall's voxel is carved away: what belonged to it goes, the place keeps its mark
  w.carve({kH * p[0], kH * p[1], kH * p[2]}, 0.05);
  w.tick();
  REQUIRE_FALSE(vox_solid(w.grid().get(p)));
  CHECK(w.layer(soot, p) == 0);
  CHECK(w.layer(World::kDamageLayer, p) == 0);
  CHECK(w.layer(mark, p) == 60);
  // a solid placed in the wet air: the water there goes
  w.set_voxels({{air, kConc}});
  CHECK(w.layer(wet, air) == 0);
  CHECK(w.layer(mark, air) == 61);
}

TEST_CASE("ext: full damage is no strength at all (it breaks), never a 0 / 0 that holds") {
  World w;
  w.load(cantilever());
  w.bake();
  for (int t = 0; t < 30; ++t) w.tick();
  REQUIRE(w.pieces().empty());
  std::vector<LayerEdit> dmg;
  for (i32 y = 0; y < 3; ++y)
    for (i32 z = 24; z < 27; ++z) dmg.push_back({{0, y, z}, 255});
  w.set_layer(World::kDamageLayer, dmg);
  for (int t = 0; t < 120 && w.pieces().empty(); ++t) w.tick();
  CHECK_FALSE(w.pieces().empty());
}

TEST_CASE("ext: damage extracts a structure nobody holds, even where its fragments are cached") {
  WorldConfig c;
  c.memory.structure_mb = 0.0;  // (idle structures are dropped at once: their fragment caches stay)
  World w;
  w.configure(c);
  w.load(cantilever());
  // extracted by a load, then dropped as idle
  w.set_loads(1, {{{8, 1, 26}, V3{0, 0, -1.0}}});
  for (int t = 0; t < 30; ++t) w.tick();
  w.set_loads(1, {});
  for (int t = 0; t < 30; ++t) w.tick();
  REQUIRE(w.pieces().empty());
  std::vector<LayerEdit> dmg;
  for (i32 y = 0; y < 3; ++y)
    for (i32 z = 24; z < 27; ++z) dmg.push_back({{0, y, z}, 250});
  w.set_layer(World::kDamageLayer, dmg);
  for (int t = 0; t < 120 && w.pieces().empty(); ++t) w.tick();
  CHECK_FALSE(w.pieces().empty());
}

namespace {

struct Reentrant final : WorldSystem {
  int steps = 0;
  const char* name() const override { return "reentrant"; }
  void step(World& w, f64) override {
    ++steps;
    w.tick();                // (refused: inside a tick)
    w.load(VoxelGrid{});     // (refused)
    w.add_system(std::make_shared<Counting>());  // (added: stepped from the next tick)
  }
};

}  // namespace

TEST_CASE("ext: a system cannot tick or load the world from inside its tick") {
  World w;
  auto r = std::make_shared<Reentrant>();
  w.add_system(r);
  w.load(wall());
  for (int t = 0; t < 3; ++t) w.tick();
  CHECK(r->steps == 3);
  CHECK(w.ticks() == 3);
  CHECK(w.grid().get(10, 4, 10) == kConc);  // (the world was not replaced)
  CHECK(w.systems().size() == 4);
}

TEST_CASE("ext: forces set while paused do not pile up") {
  WorldParams p;
  World w;
  w.load(cantilever());
  std::vector<VoxelEdit> cut;
  for (i32 y = 0; y < 3; ++y)
    for (i32 z = 24; z < 27; ++z) cut.push_back({{0, y, z}, kAir});
  w.set_voxels(cut);
  for (int t = 0; t < 3; ++t) w.tick();
  REQUIRE(w.pieces().size() == 1);
  const i64 id = w.pieces()[0].id;
  p.paused = true;
  w.set_params(p);
  for (int t = 0; t < 60; ++t) {
    const PieceState ps = w.pieces()[0];
    w.apply_force(id, ps.pos, V3{1e6, 0, 0});
    w.tick();
  }
  p.paused = false;
  w.set_params(p);
  const f64 vx0 = w.pieces()[0].vel.x;
  w.tick();
  const PieceState ps = w.pieces()[0];
  // (at most one tick of the force: F dt / m, not sixty)
  CHECK(ps.vel.x - vx0 < 1.5 * 1e6 * w.config().dt / ps.mass);
}

TEST_CASE("ext: invalid voxel values are refused or cleaned; layer writes skip chunks not generated") {
  World w;
  w.load(wall());
  CHECK(w.set_voxels({{{0, 0, 30}, Vox{0x80}}}) == 0);  // ("anchored air")
  VoxelGrid g = wall();
  g.set(1, 1, 30, Vox{0x80});
  World b;
  b.load(std::move(g));
  CHECK(b.grid().get(1, 1, 30) == kAir);
  // streamed: a layer write where nothing is generated yet is dropped (no chunk made for it)
  World s;
  const int soot = s.add_layer({"soot", true});
  VoxelGrid e;
  e.h = kH;
  s.load(std::move(e));
  StreamConfig sc;
  sc.load_radius = 8.0;
  sc.evict_radius = 12.0;
  sc.chunks_per_tick = 400;
  s.enable_streaming(std::make_shared<Flat>(), sc);
  s.set_focus(V3{0, 0, 0});
  for (int t = 0; t < 5; ++t) s.tick();
  const IVec3 far{400, 0, -3};  // (chunk 12: in the source's range, 50 m away)
  REQUIRE_FALSE(s.chunk_resident(chunk_of(far)));
  CHECK(s.set_layer(soot, {{far, 9}}) == 0);
  CHECK(s.grid().chunk(chunk_of(far)) == nullptr);
  CHECK(s.set_layer(soot, {{{2, 2, -3}, 9}}) == 1);
}
