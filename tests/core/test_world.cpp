// The physics core on its own: hand-built worlds, no game harness (links svx_core only).
#include <cmath>
#include <vector>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/material/material.hpp"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

constexpr f64 kH = 0.125;

void box(VoxelGrid& g, const IVec3& lo, const IVec3& hi, Vox v) {
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y) g.fill_column(x, y, lo[2], hi[2], v);
}

const Vox kRock = make_vox(MaterialId::Rock, true);
const Vox kConcrete = make_vox(MaterialId::Concrete, false);

// A 4 m x 4 m slab, 25 cm thick, 3 m up on four 25 cm legs, on an anchored rock plate.
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

V3 leg_centre(i32 lx, i32 ly, f64 z) { return {kH * (lx + 0.5), kH * (ly + 0.5), z}; }

struct Run {
  std::vector<WorldEvent> events;
  int added = 0, removed = 0;
};

void run(World& w, int ticks, Run* r) {
  for (int t = 0; t < ticks; ++t) {
    w.tick();
    for (WorldEvent& e : w.take_events()) {
      r->added += e.kind == WorldEvent::Kind::PieceAdded;
      r->removed += e.kind == WorldEvent::Kind::PieceRemoved;
      r->events.push_back(e);
    }
  }
}

}  // namespace

TEST_CASE("world: a designed table stands; cut its legs and the slab falls as pieces") {
  World w;
  w.load(table_world());
  REQUIRE(w.bake());
  Run r;
  run(w, 30, &r);
  CHECK(r.added == 0);
  CHECK(w.pieces().empty());
  w.take_changed_chunks();
  for (i32 lx : {0, 30})
    for (i32 ly : {0, 30}) w.carve(leg_centre(lx, ly, 1.5), 0.3);
  run(w, 1, &r);
  CHECK_FALSE(w.take_changed_chunks().empty());  // the carves
  run(w, 240, &r);
  CHECK(r.added >= 1);
  bool from_world = false;
  for (const WorldEvent& e : r.events)
    if (e.kind == WorldEvent::Kind::PieceAdded && e.parent == 0) from_world = true;
  CHECK(from_world);
  CHECK_FALSE(vox_solid(w.grid().get(16, 16, 25)));  // the slab left the grid
  // every piece announced is either alive or was reported removed
  CHECK(static_cast<int>(w.pieces().size()) == r.added - r.removed);
  f64 lowest = 1e9;
  for (const PieceState& p : w.pieces()) lowest = std::min(lowest, p.pos.z);
  CHECK(lowest < 2.5);  // it fell
}

TEST_CASE("world: pieces take impulses and can be removed") {
  World w;
  w.load(table_world());
  REQUIRE(w.bake());
  for (i32 lx : {0, 30})
    for (i32 ly : {0, 30}) w.carve(leg_centre(lx, ly, 1.5), 0.3);
  Run r;
  run(w, 30, &r);
  REQUIRE_FALSE(w.pieces().empty());
  const PieceState p = w.pieces().front();
  const Body* b = w.piece(p.id);
  REQUIRE(b != nullptr);
  CHECK(b->shape.count == p.voxels);
  CHECK(w.apply_impulse(p.id, p.pos, V3{0, 0, 50.0 * p.mass}));  // +50 m/s up
  CHECK(w.piece(p.id)->v.z > 40.0);
  CHECK_FALSE(w.apply_impulse(-7, p.pos, V3{1, 0, 0}));
  w.take_events();
  CHECK(w.remove_piece(p.id));
  CHECK(w.piece(p.id) == nullptr);
  const std::vector<WorldEvent> ev = w.take_events();
  REQUIRE(ev.size() == 1);
  CHECK(ev[0].kind == WorldEvent::Kind::PieceRemoved);
  CHECK(ev[0].end == PieceEnd::Removed);
  CHECK(ev[0].id == p.id);
}

TEST_CASE("world: set_voxels — isolated anchored parts bond to nothing, untracked edits stay out of the delta") {
  World w;
  w.load(table_world());
  REQUIRE(w.bake());
  // a kinematic block (a door) next to a leg: anchored, isolated, untracked
  std::vector<VoxelEdit> door;
  for (i32 z = 0; z < 16; ++z) door.push_back({{2, 0, z}, make_vox(MaterialId::Steel, true)});
  CHECK(w.set_voxels(door, kEditUntracked | kEditIsolated) == 16);
  for (i32 z = 0; z < 16; ++z) {
    CHECK_FALSE(w.grid().bond({1, 0, z}, 0));  // the leg and the door do not bond
    CHECK_FALSE(w.grid().bond({2, 0, z}, 2));
  }
  CHECK_FALSE(w.modified());
  // a tracked edit is part of the delta
  CHECK(w.set_voxels({{{20, 20, 26}, kConcrete}}) == 1);
  CHECK(w.modified());
  CHECK(w.set_voxels({{{20, 20, 26}, kConcrete}}) == 0);  // (no change)
}

TEST_CASE("world: the material registry") {
  bool ok = false;
  CHECK(material_from_name("masonry", &ok) == MaterialId::Masonry);
  CHECK(ok);
  material_from_name("unobtainium", &ok);
  CHECK_FALSE(ok);
  Material glass;
  glass.name = "glass";
  glass.ft = glass.fb = 0.05e6;
  glass.indestructible = true;
  MaterialId id;
  REQUIRE(register_material(glass, &id));
  CHECK(static_cast<int>(id) >= kStandardMaterials);
  CHECK(material(id).name == "glass");
  CHECK(material_from_name("glass") == id);
  // carves leave indestructible materials
  World w;
  VoxelGrid g = table_world();
  box(g, {10, 10, 0}, {12, 12, 4}, make_vox(id, false));
  w.load(std::move(g));
  w.carve({kH * 10.5, kH * 10.5, kH * 2}, 0.2);  // (not reaching the rock it stands on)
  w.tick();
  CHECK(vox_solid(w.grid().get(10, 10, 1)));
  CHECK(vox_solid(w.grid().get(11, 11, 2)));
  reset_materials();
  CHECK_FALSE(material_registered(id));
}

namespace {

// A column 2 x 2 voxels, 2 m tall, standing on the rock plate at (8, 8).
VoxelGrid column_world() {
  VoxelGrid g = table_world();
  box(g, {8, 8, 0}, {10, 10, 16}, kConcrete);
  return g;
}

// A streamed world: an anchored ground (z < 0) and a free concrete pillar (2 x 2 voxels, 3 m) in
// the middle of every 8th chunk column, and a 12 m long wall straddling chunk columns 16 / 17.
class PillarSource final : public ChunkSource {
 public:
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    out.assign(kChunkVox, kAir);
    bool any = false;
    for (int x = 0; x < kChunk; ++x)
      for (int y = 0; y < kChunk; ++y)
        for (int z = 0; z < kChunk; ++z) {
          const IVec3 p{cc[0] * kChunk + x, cc[1] * kChunk + y, cc[2] * kChunk + z};
          Vox v = kAir;
          if (p[2] < 0 && p[2] >= -8) v = kRock;
          const bool pillar = (cc[0] % 8 == 0) && (cc[1] % 8 == 0) && (x == 16 || x == 17) && (y == 16 || y == 17) &&
                              p[2] >= 0 && p[2] < 24;
          const bool wall = p[0] >= 16 * kChunk - 48 && p[0] < 17 * kChunk + 48 && p[1] >= 40 * kChunk && p[1] < 40 * kChunk + 2 &&
                            p[2] >= 0 && p[2] < 16;
          if (pillar || wall) v = kConcrete;
          if (v != kAir) {
            out[size_t(chunk_index(p))] = v;
            any = true;
          }
        }
    return any;
  }
  IVec3 chunk_lo() const override { return {0, 0, -1}; }
  IVec3 chunk_hi() const override { return {64, 64, 2}; }
};

}  // namespace

TEST_CASE("world: removing the ground under a registered structure drops it") {
  World w;
  w.load(column_world());
  REQUIRE(w.bake());
  REQUIRE(w.probe_utilization({8, 8, 4}) >= 0.0);  // (its structure is registered now)
  Run r;
  run(w, 10, &r);
  CHECK(r.added == 0);
  // the rock under it goes (anchored voxels: the free voxels' fragments do not change)
  std::vector<VoxelEdit> dig;
  for (i32 x = 7; x <= 10; ++x)
    for (i32 y = 7; y <= 10; ++y)
      for (i32 z = -4; z < 0; ++z) dig.push_back({{x, y, z}, kAir});
  CHECK(w.set_voxels(dig) == 64);
  run(w, 60, &r);
  CHECK(r.added >= 1);
  CHECK_FALSE(vox_solid(w.grid().get(8, 8, 8)));
}

TEST_CASE("world: hostile inputs are refused without effect") {
  World w;
  w.load(table_world());
  REQUIRE(w.bake());
  const u64 h0 = w.state_hash();
  const f64 inf = INFINITY;
  w.carve({NAN, 0, 0}, 1.0);
  w.carve({1, 1, 1}, NAN);
  w.carve({1, 1, 1}, -1.0);
  w.carve({1e12, 0, 0}, 1.0);
  w.carve({inf, 0, 0}, 1.0);
  w.blast({1, 1, 1}, 1.0, NAN);
  w.blast({NAN, 1, 1}, 1.0, 1e6);
  w.carve({3000, 3000, 3000}, 1e9);  // (far away: clamped to max_event_radius, carves nothing there)
  w.set_focus({V3{NAN, 0, 0}, V3{1e30, 0, 0}});
  WorldParams p;
  p.fragility = NAN;
  p.impact = -inf;
  p.dif = 1e30;
  w.set_params(p);
  CHECK(w.params().fragility == 1.0);  // (non-finite: the default)
  CHECK(w.params().impact == 1.0);
  CHECK(w.params().dif <= 10.0);
  w.set_params(WorldParams{});
  CHECK_FALSE(w.raycast({NAN, 0, 0}, {1, 0, 0}, 10.0).hit);
  CHECK_FALSE(w.raycast({0, 0, 1}, {0, 0, 0}, 10.0).hit);
  CHECK_FALSE(w.raycast({0, 0, 1}, {1, 0, 0}, NAN).hit);
  const CollideResult c = w.collide({0, 0, 5}, {0.5, 0.5, 6}, {1e9, NAN, -1e9});
  CHECK(std::isfinite(c.move.x));
  CHECK(c.move.x == 0.0);  // (a non-finite move is refused whole)
  const CollideResult c2 = w.collide({10, 10, 5}, {10.5, 10.5, 6}, {0, 0, -1e9});
  CHECK(c2.move.z >= -16.0);
  CHECK(w.set_voxels({{{1 << 21, 0, 0}, kConcrete}, {{0, -(1 << 20), 0}, kConcrete}}) == 0);
  CHECK_FALSE(w.load_delta({}));
  CHECK_FALSE(w.load_delta({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}));
  std::vector<u8> d = w.save_delta();
  d.resize(d.size() / 2 + 3);
  CHECK_FALSE(w.load_delta(d));
  CHECK_FALSE(w.apply_impulse(1, {NAN, 0, 0}, {1, 0, 0}));
  CHECK_FALSE(w.remove_piece(12345));
  for (int t = 0; t < 10; ++t) w.tick();
  CHECK(w.state_hash() == h0);
  CHECK(w.stats().events == 1);  // (the far carve only)
}

TEST_CASE("world: a delta restores the voxels and their design classes (v2 records)") {
  World a;
  VoxelGrid g = table_world();
  box(g, {0, 12, 24}, {32, 20, 26}, kAir);  // (the slab gets a hole: its members work harder)
  a.load(std::move(g));
  REQUIRE(a.bake());
  REQUIRE(a.design_report().strengthened_voxels > 0);
  a.set_voxels({{{16, 4, 26}, kConcrete}, {{16, 28, 26}, kConcrete}});  // (tracked edits on the slab)
  a.tick();
  const std::vector<u8> delta = a.save_delta();
  REQUIRE_FALSE(delta.empty());
  World b;
  VoxelGrid g2 = table_world();
  box(g2, {0, 12, 24}, {32, 20, 26}, kAir);
  b.load(std::move(g2));  // (no bake: the classes come from the delta)
  REQUIRE(b.load_delta(delta));
  CHECK(b.state_hash() == a.state_hash());
  i64 classes = 0, differ = 0;
  for (u64 k : a.grid().modified_chunks()) {
    const IVec3 cc = unkey3(k);
    for (int i = 0; i < kChunkVox; ++i) {
      const IVec3 l{i / (kChunk * kChunk), (i / kChunk) % kChunk, i % kChunk};
      const IVec3 p{cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]};
      classes += a.grid().strength(p) > 0;
      differ += a.grid().strength(p) != b.grid().strength(p);
    }
  }
  CHECK(classes > 0);
  CHECK(differ == 0);
}

TEST_CASE("world: streaming around several focus points; edits survive eviction; a wall cut by the evict radius stands") {
  World w;
  VoxelGrid g;
  g.h = kH;
  w.load(std::move(g));
  StreamConfig sc;
  sc.load_radius = 20.0;
  sc.evict_radius = 28.0;
  sc.chunks_per_tick = 400;
  w.enable_streaming(std::make_shared<PillarSource>(), sc);
  const V3 a{kH * (8 * kChunk + 16), kH * (8 * kChunk + 16), 0.0}, b{kH * (40 * kChunk + 16), kH * (8 * kChunk + 16), 0.0};
  w.set_focus({a, b});
  for (int t = 0; t < 10; ++t) w.tick();
  CHECK(w.chunk_resident({8, 8, 0}));
  CHECK(w.chunk_resident({40, 8, 0}));
  CHECK_FALSE(w.chunk_resident({24, 8, 0}));
  CHECK(vox_solid(w.grid().get(8 * kChunk + 16, 8 * kChunk + 16, 10)));
  // cut the pillar at a (it falls), and knock a notch into the one at b
  w.carve({a.x + 0.06, a.y + 0.06, 1.0}, 0.3);
  w.set_voxels({{{40 * kChunk + 16, 8 * kChunk + 16, 23}, kAir}});
  for (int t = 0; t < 30; ++t) w.tick();
  CHECK_FALSE(vox_solid(w.grid().get(8 * kChunk + 16, 8 * kChunk + 16, 20)));
  const u64 before = w.state_hash();
  (void)before;
  // move away from b and back: its edit comes back with its chunk
  w.set_focus(a);
  for (int t = 0; t < 10; ++t) w.tick();
  CHECK_FALSE(w.chunk_resident({40, 8, 0}));
  CHECK(w.stats().archived_chunks >= 1);
  CHECK_FALSE(w.take_evicted_chunks().empty());
  w.set_focus({a, b});
  for (int t = 0; t < 10; ++t) w.tick();
  CHECK_FALSE(vox_solid(w.grid().get(40 * kChunk + 16, 8 * kChunk + 16, 23)));
  CHECK(vox_solid(w.grid().get(40 * kChunk + 16, 8 * kChunk + 16, 22)));
  // the long wall: register it, then let the evict radius cut through it - no collapse
  const V3 c{kH * (16 * kChunk), kH * (40 * kChunk), 0.0};
  w.set_focus(c);
  for (int t = 0; t < 10; ++t) w.tick();
  REQUIRE(vox_solid(w.grid().get(16 * kChunk, 40 * kChunk, 8)));
  REQUIRE(w.probe_utilization({16 * kChunk, 40 * kChunk, 8}) >= 0.0);
  const i64 broken0 = w.stats().bonds_broken;
  const V3 c2{c.x - 25.0, c.y, 0.0};  // (the wall's east end is beyond the evict radius now)
  w.set_focus(c2);
  Run r;
  run(w, 60, &r);
  CHECK(w.stats().bonds_broken == broken0);
  int near_wall = 0;  // (the pillar cut before may still be breaking where it landed)
  for (const WorldEvent& e : r.events) near_wall += e.kind == WorldEvent::Kind::PieceAdded && std::abs(e.pos.y - c.y) < 10.0;
  CHECK(near_wall == 0);
  CHECK(vox_solid(w.grid().get(16 * kChunk - 40, 40 * kChunk, 8)));
}

TEST_CASE("world: a session is bit-identical on any thread count, and after load() again") {
  auto session = [](int threads, World* reuse) {
    set_num_threads(threads);
    World local;
    World& w = reuse ? *reuse : local;
    w.load(table_world());
    w.bake();
    for (i32 lx : {0, 30})
      for (i32 ly : {0, 30}) w.carve(leg_centre(lx, ly, 1.5), 0.3);
    for (int t = 0; t < 180; ++t) {
      if (t == 60) w.blast({2.0, 2.0, 1.0}, 0.5, 1e5);
      w.tick();
    }
    return w.session_hash();
  };
  const int hw = num_threads();
  const u64 h1 = session(1, nullptr);
  const u64 h4 = session(4, nullptr);
  World twice;
  session(4, &twice);
  const u64 again = session(4, &twice);  // (ids and solver state start over at load())
  set_num_threads(hw);
  CHECK(h1 == h4);
  CHECK(again == h4);
}

TEST_CASE("world: nested parallel loops run inline (no deadlock)") {
  std::vector<i64> sum(64, 0);
  parallel_for(64, 1, [&](i64 a, i64 b) {
    for (i64 i = a; i < b; ++i)
      parallel_for(100, 7, [&](i64 c, i64 d) {
        for (i64 k = c; k < d; ++k) sum[size_t(i)] += k;
      });
  });
  for (i64 v : sum) CHECK(v == 4950);
}

TEST_CASE("world: free voxels written into the air become a piece (dropping and throwing things)") {
  World w;
  w.load(table_world());
  REQUIRE(w.bake());
  std::vector<VoxelEdit> crate;
  for (i32 x = 12; x < 20; ++x)
    for (i32 y = 12; y < 20; ++y)
      for (i32 z = 60; z < 68; ++z) crate.push_back({{x, y, z}, kConcrete});
  CHECK(w.set_voxels(crate) == 512);
  w.tick();
  i64 id = 0;
  for (const WorldEvent& e : w.take_events())
    if (e.kind == WorldEvent::Kind::PieceAdded) id = e.id;
  REQUIRE(id != 0);
  const PieceState p = w.pieces().front();
  CHECK(p.voxels == 512);
  CHECK_FALSE(vox_solid(w.grid().get(16, 16, 64)));  // (a piece, not the grid's any more)
  CHECK(w.apply_impulse(id, p.pos, V3{8.0 * p.mass, 0, 0}));  // thrown sideways at 8 m/s
  for (int t = 0; t < 30; ++t) w.tick();
  const Body* b = w.piece(id);
  REQUIRE(b != nullptr);
  CHECK(b->x.x > p.pos.x + 2.0);
  CHECK(b->x.z < p.pos.z);
}
