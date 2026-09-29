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
  CHECK(b->count == p.voxels);
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
  // a mover's block (a door) next to a leg: anchored, isolated, untracked
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
  CHECK(material_from_name("wood") == MaterialId::Wood);
  CHECK(material(MaterialId::Rebar).ductile);
  CHECK(material(MaterialId::Steel).ductile);
  CHECK_FALSE(material(MaterialId::Concrete).ductile);
  Material adamant;
  adamant.name = "adamant";
  adamant.ft = adamant.fb = 0.05e6;
  adamant.indestructible = true;
  MaterialId id;
  REQUIRE(register_material(adamant, &id));
  CHECK(static_cast<int>(id) >= kStandardMaterials);
  CHECK(material(id).name == "adamant");
  CHECK(material_from_name("adamant") == id);
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

TEST_CASE("world: extractions of a structure too large for one never chase each other") {
  // A 40 m wall, extractions limited to 6 m around their seed: touched at two places 5 m apart,
  // the two structures meet between them (each is the other's frontier) and stay registered.
  WorldConfig cfg;
  cfg.structure_max_radius = 6.0;
  World w;
  w.configure(cfg);
  VoxelGrid g;
  g.h = kH;
  box(g, {-8, -8, -4}, {336, 16, 0}, kRock);
  box(g, {0, 0, 0}, {320, 2, 24}, kConcrete);
  g.compact();
  w.load(std::move(g));
  REQUIRE(w.bake());
  const i64 e0 = w.stats().extractions;
  w.carve({15.0, 0.1, 2.5}, 0.3);
  w.carve({20.0, 0.1, 2.5}, 0.3);
  for (int t = 0; t < 120; ++t) w.tick();
  const i64 extractions = w.stats().extractions - e0;
  MESSAGE("extractions after two carves and 2 s: " << extractions);
  CHECK(extractions <= 8);
  CHECK(w.stats().structures >= 2);
}

// ---- memory: a streamed world's changes, budgets

namespace {

// A trip along the pillar row at chunk row 8: pillar k (every 8 chunks = 32 m) is cut at 1 m.
struct PillarTrip {
  World w;
  Run r;
  explicit PillarTrip(f64 archive_mb, f64 forget_after_s = 0.0) {
    VoxelGrid g;
    g.h = kH;
    w.load(std::move(g));
    StreamConfig sc;
    sc.load_radius = 20.0;
    sc.evict_radius = 28.0;
    sc.chunks_per_tick = 400;
    sc.archive_mb = archive_mb;
    sc.forget_after_s = forget_after_s;
    w.enable_streaming(std::make_shared<PillarSource>(), sc);
  }
  static V3 at(int k) { return {kH * (8 * k * kChunk + 16), kH * (8 * kChunk + 16), 0.0}; }
  static IVec3 voxel(int k, i32 z) { return {8 * k * kChunk + 16, 8 * kChunk + 16, z}; }
  void go(int k, int ticks = 10) {
    w.set_focus(at(k));
    run(w, ticks, &r);
  }
  void cut(int k) {
    go(k);
    w.carve(at(k) + V3{0.06, 0.06, 1.0}, 0.3);
    run(w, 90, &r);  // (the pillar falls and comes to rest)
  }
  int count(WorldEvent::Kind kind) const {
    int n = 0;
    for (const WorldEvent& e : r.events) n += e.kind == kind;
    return n;
  }
};

}  // namespace

TEST_CASE("memory: a bounded change archive forgets the regions seen least recently; they come back as generated") {
  PillarTrip t(6.0 / 1024.0);  // (six pages of 1 KB)
  for (int k = 1; k <= 10; ++k) t.cut(k);
  t.go(11);
  const WorldStats s = t.w.stats();
  MESSAGE("archive " << s.archived_chunks << " chunks, " << s.archive_used_mb * 1024 << " of " << s.archive_capacity_mb * 1024
                     << " KB; forgotten " << s.forgotten_regions << " regions, " << s.forgotten_chunks << " chunks");
  CHECK(s.archive_used_mb <= s.archive_capacity_mb);
  CHECK(s.forgotten_regions >= 3);
  CHECK(t.count(WorldEvent::Kind::Forgotten) == s.forgotten_regions);
  // the rubble of the pillars left behind was unloaded with its chunks (it pins nothing)
  int unloaded = 0;
  for (const WorldEvent& e : t.r.events) unloaded += e.kind == WorldEvent::Kind::PieceRemoved && e.end == PieceEnd::Unloaded;
  CHECK(unloaded >= 5);
  const i64 mem = t.w.memory().archive;
  // back to the first pillar: forgotten, so it stands again as generated
  t.go(1, 20);
  CHECK(vox_solid(t.w.grid().get(PillarTrip::voxel(1, 10))));
  CHECK(vox_solid(t.w.grid().get(PillarTrip::voxel(1, 8))));
  // ... and the one cut last is still cut
  t.go(10, 20);
  CHECK_FALSE(vox_solid(t.w.grid().get(PillarTrip::voxel(10, 8))));
  CHECK(t.w.memory().archive == mem);  // (the arena never grows)
}

TEST_CASE("memory: rubble goes out of range with its chunks, in the archive, and comes back with them") {
  PillarTrip t(0.0);  // (unbounded: nothing is forgotten)
  t.cut(1);
  std::vector<PieceState> before = t.w.pieces();
  REQUIRE(before.size() >= 1);
  t.go(4, 30);  // (96 m on: pillar 1 is out of range)
  CHECK(t.w.pieces().empty());
  CHECK(t.w.stats().archived_pieces == static_cast<i64>(before.size()));
  const int unloaded = t.count(WorldEvent::Kind::PieceRemoved);
  t.go(1, 30);
  const std::vector<PieceState> after = t.w.pieces();
  MESSAGE("pillar 1's rubble: " << before.size() << " pieces archived out of range, " << after.size() << " back (" << unloaded << " unloaded)");
  REQUIRE(after.size() == before.size());
  f64 worst = 0.0;
  for (size_t k = 0; k < before.size(); ++k) {
    CHECK(after[k].id == before[k].id);
    worst = std::max(worst, norm(after[k].pos - before[k].pos));
  }
  CHECK(worst < 0.01);
  CHECK(t.w.stats().archived_pieces == 0);
  // saved while it is out of range, and loaded in a session of the same world: it comes back there too
  t.go(4, 30);
  REQUIRE(t.w.stats().archived_pieces == static_cast<i64>(before.size()));
  const std::vector<u8> delta = t.w.save_delta();
  PillarTrip t2(0.0);
  t2.go(4, 5);
  REQUIRE(t2.w.load_delta(delta));
  CHECK(t2.w.stats().archived_pieces == static_cast<i64>(before.size()));
  t2.go(1, 30);
  const std::vector<PieceState> again = t2.w.pieces();
  REQUIRE(again.size() == before.size());
  for (size_t k = 0; k < before.size(); ++k) CHECK(norm(again[k].pos - before[k].pos) < 0.01);
}

namespace {

// A lift a source makes: a concrete tower (1 m square, 3 m) in chunk (0, 0, 0), a timber car (1
// m square) beside it - a grid of the source's - on a slider the source makes, its drive going
// up 2 m and down every 4 s; ground along x for 256 m.
class LiftSource final : public ChunkSource {
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
          if (p[0] >= 4 && p[0] < 12 && p[1] >= 4 && p[1] < 12 && p[2] >= 0 && p[2] < 24) v = kConcrete;
          if (v != kAir) {
            out[size_t(chunk_index(p))] = v;
            any = true;
          }
        }
    return any;
  }
  IVec3 chunk_lo() const override { return {0, 0, -1}; }
  IVec3 chunk_hi() const override { return {64, 2, 2}; }
  std::vector<SourceGrid> grids(const IVec3& cc) const override {
    if (cc != IVec3{0, 0, 0}) return {};
    SourceGrid g;
    g.id = 1;
    g.origin = V3{kH * 13, kH * 4, kH * 1};
    return {g};
  }
  bool generate_grid(u32 id, VoxelGrid& out) const override {
    if (id != 1) return false;
    box(out, {0, 0, 0}, {8, 8, 2}, make_vox(MaterialId::Wood, false));
    return true;
  }
  std::vector<SourceJoint> joints(const IVec3& cc) const override {
    if (cc != IVec3{0, 0, 0}) return {};
    SourceJoint j;
    j.id = 1;
    const V3 p{kH * 12, kH * 8, kH * 1};  // (the gap between the tower's face and the car)
    j.desc.type = JointType::Slider;
    j.desc.a.kind = JointAnchor::Kind::Grid;
    j.desc.a.id = kWorldGrid;
    j.desc.a.point = p;
    j.desc.b.kind = JointAnchor::Kind::Grid;
    j.desc.b.id = 1;
    j.desc.b.point = p;
    j.desc.axis = V3{0, 0, 1};
    j.desc.drive.kind = JointDrive::Kind::Oscillate;
    j.desc.drive.target2 = 2.0;
    j.desc.drive.period = 4.0;
    j.desc.drive.speed = 2.0;
    j.desc.drive.max = 20000.0;
    return {j};
  }
};

}  // namespace

TEST_CASE("memory: a machine out of range is archived with its joint, and comes back running its program") {
  World w;
  VoxelGrid g;
  g.h = kH;
  w.load(std::move(g));
  StreamConfig sc;
  sc.load_radius = 20.0;
  sc.evict_radius = 28.0;
  sc.chunks_per_tick = 400;
  w.enable_streaming(std::make_shared<LiftSource>(), sc);
  Run r;
  w.set_focus(V3{2.0, 2.0, 0.0});
  run(w, 150, &r);
  REQUIRE(w.pieces().size() == 1);
  REQUIRE(w.joints().size() == 1);
  const JointId j = w.joints().front();
  JointState s;
  REQUIRE(w.joint(j, &s));
  const f64 goal = [&] {
    JointDrive d;
    d.kind = JointDrive::Kind::Oscillate;
    d.target2 = 2.0;
    d.period = 4.0;
    f64 x = 0.0, rate = 0.0;
    d.goal(w.time(), &x, &rate);
    return x;
  }();
  MESSAGE("the source's lift: its car at " << s.value << " m on its slider (its program: " << goal << ")");
  CHECK(std::abs(s.value - goal) < 0.05);
  // away: the lift is archived with its joint (it keeps nothing resident)
  w.set_focus(V3{200.0, 2.0, 0.0});
  run(w, 60, &r);
  CHECK(w.pieces().empty());
  CHECK(w.joints().empty());
  CHECK(w.stats().archived_pieces == 1);
  // back: it comes back as it was, with its joint (not a second one from the source), and runs on
  w.set_focus(V3{2.0, 2.0, 0.0});
  run(w, 120, &r);
  REQUIRE(w.pieces().size() == 1);
  REQUIRE(w.joints().size() == 1);
  CHECK(w.joints().front() == j);
  REQUIRE(w.joint(j, &s));
  JointDrive d;
  d.kind = JointDrive::Kind::Oscillate;
  d.target2 = 2.0;
  d.period = 4.0;
  f64 x = 0.0, rate = 0.0;
  d.goal(w.time(), &x, &rate);
  MESSAGE("back after " << w.time() << " s: the car at " << s.value << " m (its program: " << x << ")");
  CHECK(std::abs(s.value - x) < 0.05);
}

TEST_CASE("memory: an unbounded change archive keeps every change (a bounded level streamed from a file)") {
  PillarTrip t(0.0);
  for (int k = 1; k <= 10; ++k) t.cut(k);
  t.go(11);
  CHECK(t.w.stats().forgotten_regions == 0);
  CHECK(t.w.stats().archived_chunks >= 10);
  t.go(1, 20);
  CHECK_FALSE(vox_solid(t.w.grid().get(PillarTrip::voxel(1, 8))));
}

TEST_CASE("memory: regions out of range for forget_after_s heal, with room left") {
  PillarTrip t(64.0, 2.0);
  t.cut(1);
  t.go(4, 30);
  CHECK(t.w.stats().forgotten_regions == 0);
  t.go(4, 150);  // (3 s out of range)
  CHECK(t.w.stats().forgotten_regions >= 1);
  t.go(1, 20);
  CHECK(vox_solid(t.w.grid().get(PillarTrip::voxel(1, 8))));
}

TEST_CASE("memory: budgets bound what a bounded level keeps, however long it runs; results do not depend on them") {
  auto session = [](bool tight, MemoryReport* peak, WorldStats* st) {
    WorldConfig cfg;
    if (tight) {
      cfg.memory.fragment_cache_mb = 0.02;
      cfg.memory.structure_mb = 64.0;
      cfg.memory.cache_mb = 0.05;
      cfg.memory.max_events = 64;
    }
    World w;
    w.configure(cfg);
    w.load(table_world());
    w.bake();
    for (i32 lx : {0, 30})
      for (i32 ly : {0, 30}) w.carve(leg_centre(lx, ly, 1.5), 0.3);
    for (int t = 0; t < 600; ++t) {
      if (t % 60 == 30) w.blast({2.0 + 0.004 * t, 2.0, 0.6}, 0.5, 1e5);
      w.tick();  // (events never taken)
      const MemoryReport m = w.memory();
      peak->queues = std::max(peak->queues, m.queues);
      peak->caches = std::max(peak->caches, m.caches);
    }
    peak->fragments = w.memory().fragments;  // (at the end: the design pass builds them all first)
    *st = w.stats();
    return w.session_hash();
  };
  MemoryReport loose{}, tight{};
  WorldStats sl, stt;
  const u64 h_loose = session(false, &loose, &sl);
  const u64 h_tight = session(true, &tight, &stt);
  MESSAGE("fragment caches: " << loose.fragments / 1024 << " KB loose, " << tight.fragments / 1024 << " KB tight ("
                                   << stt.dropped_fragment_caches << " dropped); events dropped " << stt.dropped_events);
  CHECK(stt.dropped_fragment_caches > 0);
  CHECK(tight.fragments < loose.fragments);
  CHECK(stt.dropped_events > 0);
  CHECK(tight.queues < 64 * 1024);
  CHECK(h_tight == h_loose);  // (fragment caches and warm starts are rebuilt identically; events are output only)
}

TEST_CASE("memory: the pieces' budget culls the smallest pieces, sleeping ones first") {
  WorldConfig cfg;
  cfg.memory.piece_mb = 0.1;
  World w;
  w.configure(cfg);
  w.load(table_world());
  w.bake();
  for (i32 lx : {0, 30})
    for (i32 ly : {0, 30}) w.carve(leg_centre(lx, ly, 1.5), 0.3);
  Run r;
  i64 peak = 0;
  for (int t = 0; t < 300; ++t) {
    w.tick();
    peak = std::max(peak, w.memory().pieces);
  }
  CHECK(w.stats().culled_pieces > 0);
  MESSAGE("pieces: peak " << peak / 1024 << " KB, " << w.stats().culled_pieces << " culled");
  CHECK(peak <= static_cast<i64>(0.1 * 1048576.0) + 64 * 1024);  // (checked every tick; contacts count too)
}

TEST_CASE("world: a later extraction takes a registered structure over whole (no frontier next to what happens)") {
  auto wall = [](const WorldConfig& cfg) {
    auto w = std::make_unique<World>();
    w->configure(cfg);
    VoxelGrid g;
    g.h = kH;
    box(g, {-8, -8, -4}, {336, 16, 0}, kRock);
    box(g, {0, 0, 0}, {320, 2, 24}, kConcrete);
    g.compact();
    w->load(std::move(g));
    w->bake();
    return w;
  };
  WorldConfig cfg;
  cfg.structure_max_radius = 6.0;
  {
    auto w = wall(cfg);
    w->carve({15.0, 0.1, 2.5}, 0.15);
    for (int t = 0; t < 10; ++t) w->tick();
    w->carve({20.0, 0.1, 2.5}, 0.15);
    for (int t = 0; t < 60; ++t) w->tick();
    CHECK(w->stats().structures == 1);
    CHECK(w->stats().bonds_broken == 0);  // (small carves in a designed wall)
  }
  {
    cfg.structure_max_nodes = 150;  // (too small to take a structure over whole: partial takeovers)
    auto w = wall(cfg);
    const i64 e0 = w->stats().extractions;
    for (int k = 0; k < 6; ++k) {
      w->carve({10.0 + 2.0 * k, 0.1, 2.5}, 0.15);
      for (int t = 0; t < 10; ++t) w->tick();
    }
    const i64 settled = w->stats().extractions;
    for (int t = 0; t < 120; ++t) w->tick();
    MESSAGE("extractions: " << settled - e0 << " for six carves, then " << w->stats().extractions - settled << " in 2 s");
    CHECK(w->stats().extractions - settled <= 2);
  }
}

// ---- materials

namespace {

// A concrete cantilever 3 x 3 voxels, `len` voxels long, out of an anchored rock wall; if
// reinforced, bars along its top (tension) face, anchored half a metre into the wall.
VoxelGrid cantilever_world(i32 len, bool reinforced) {
  VoxelGrid g;
  g.h = kH;
  box(g, {-8, -8, -4}, {len + 16, 16, 0}, kRock);
  box(g, {-8, -8, 0}, {0, 16, 40}, kRock);
  const Vox conc = make_vox(MaterialId::Concrete, false);
  box(g, {0, 0, 24}, {len, 3, 27}, conc);
  if (reinforced) box(g, {-4, 0, 26}, {len, 3, 27}, make_vox(MaterialId::Rebar, false));
  g.compact();
  return g;
}

}  // namespace

TEST_CASE("materials: bars in the tension face carry a cantilever that plain concrete does not") {
  auto stands = [](bool reinforced) {
    World w;
    w.load(cantilever_world(20, reinforced));  // (2.5 m; no design pass: as built)
    REQUIRE(w.probe_utilization({10, 1, 25}) >= 0.0);
    for (int t = 0; t < 240; ++t) w.tick();
    return vox_solid(w.grid().get(18, 1, 25));
  };
  CHECK_FALSE(stands(false));
  CHECK(stands(true));
}

TEST_CASE("materials: a blast strips concrete off its bars, and ductile bars never turn to dust") {
  VoxelGrid g = table_world();
  // a 3 m x 3 m reinforced wall 3 voxels thick under the table, bars every 4 voxels
  box(g, {4, 34, 0}, {28, 37, 24}, kConcrete);
  const Vox bar = make_vox(MaterialId::Rebar, false);
  for (i32 z = 1; z < 24; z += 4) box(g, {4, 35, z}, {28, 36, z + 1}, bar);
  for (i32 x = 5; x < 28; x += 4) box(g, {x, 35, 0}, {x + 1, 36, 24}, bar);
  World w;
  w.load(std::move(g));
  w.bake();
  auto count = [&](Vox v) {
    i64 n = 0;
    for (i32 x = 4; x < 28; ++x)
      for (i32 z = 0; z < 24; ++z)
        for (i32 y = 34; y < 37; ++y) n += w.grid().get(x, y, z) == v;
    for (const PieceState& p : w.pieces())
      for (const BodyShape& S : w.piece(p.id)->shapes)
        for (Vox s : S.vox) n += s == v;
    return n;
  };
  const i64 bars0 = count(bar), conc0 = count(kConcrete);
  REQUIRE(bars0 > 100);
  w.blast({2.0, 4.2, 1.5}, 0.6, 1e6);
  for (int t = 0; t < 180; ++t) w.tick();
  const f64 bars = static_cast<f64>(count(bar)) / static_cast<f64>(bars0);
  const f64 conc = static_cast<f64>(count(kConcrete)) / static_cast<f64>(conc0);
  MESSAGE("left after the blast: bars " << 100 * bars << "%, concrete " << 100 * conc << "%");
  CHECK(bars > conc);
}

TEST_CASE("world: a blast in the air loads what is around it by its energy") {
  // (nothing within its shatter radius: the kinetic energy it has goes to what it loads)
  auto run = [](f64 energy) {
    VoxelGrid g;
    g.h = kH;
    box(g, {-8, -8, -4}, {56, 40, 0}, kRock);
    box(g, {0, 20, 0}, {48, 23, 24}, make_vox(MaterialId::Masonry, false));  // 6 m x 3 m, 37.5 cm thick
    g.compact();
    World w;
    w.load(std::move(g));
    w.bake();
    w.blast({3.0, kH * 21.5 - 2.2, 1.5}, 1.0, energy);  // 2.2 m in front of it
    for (int t = 0; t < 60; ++t) w.tick();
    return w.stats().bonds_broken;
  };
  const i64 weak = run(10.0), strong = run(1e7);
  MESSAGE("bonds broken: 10 J " << weak << ", 10 MJ " << strong);
  CHECK(weak == 0);
  CHECK(strong > 0);
}

TEST_CASE("world: what a blast's load breaks off flies off with the momentum it gave it") {
  // (a masonry wall 2.2 m behind a blast in the air: nothing is shattered, the wall is broken by
  // the blast's load - and its parts move off away from the blast, as it pushed them)
  VoxelGrid g;
  g.h = kH;
  box(g, {-8, -8, -4}, {56, 40, 0}, kRock);
  box(g, {0, 20, 0}, {48, 23, 24}, make_vox(MaterialId::Masonry, false));
  g.compact();
  World w;
  w.load(std::move(g));
  w.bake();
  w.blast({3.0, kH * 21.5 - 2.2, 1.5}, 1.0, 1e7);
  f64 best = 0.0, m_best = 0.0;
  for (int t = 0; t < 30; ++t) {
    w.tick();
    // (the pieces' momentum away from the blast, per their mass, as they come loose)
    f64 p = 0.0, m = 0.0;
    for (const PieceState& ps : w.pieces()) {
      const Body* b = w.piece(ps.id);
      p += b->mass * ps.vel.y;
      m += b->mass;
    }
    if (m > 0.0 && p / m > best) {
      best = p / m;
      m_best = m;
    }
  }
  MESSAGE("blast 2.2 m in front of a wall: its parts moved off at up to " << best << " m/s on average (" << m_best << " kg)");
  CHECK(m_best > 100.0);
  CHECK(best > 1.0);
}

TEST_CASE("world: every cascade gets its own break rounds, however long a structure lives") {
  // (a wall carrying a slab, holed by one small blast after another: the holes add up until it
  // gives way - long after the first cascades used up max_rounds between them)
  const i32 len = 192;
  VoxelGrid g;
  g.h = kH;
  box(g, {-8, -16, -4}, {len + 8, 24, 0}, kRock);
  box(g, {0, 0, 0}, {len, 3, 40}, make_vox(MaterialId::Masonry, false));
  box(g, {0, 3, 36}, {len, 16, 40}, kConcrete);
  g.compact();
  World w;
  WorldConfig c = w.config();
  c.max_rounds = 6;
  w.configure(c);
  w.load(std::move(g));
  w.bake();
  for (int t = 0; t < 5; ++t) w.tick();
  i64 early = 0, late = 0;
  for (int k = 0; k <= 8; ++k) {
    const i64 b0 = w.stats().bonds_broken;
    w.blast({2.0 * k + 3.0, -0.07, 1.5}, 0.4, 3e4);
    for (int t = 0; t < 90; ++t) w.tick();
    (k < 6 ? early : late) += w.stats().bonds_broken - b0;
  }
  MESSAGE("bonds broken by the first six blasts " << early << ", by the last three " << late);
  CHECK(late > 100);
}

TEST_CASE("world: an impact load case passes; the structure is judged in its steady state after it") {
  // (a portal frame: a blast next to a leg loads it and breaks nothing; damage to the beam later
  // is judged under the frame's weight, not under the blast that has passed)
  auto run = [](bool blast) {
    VoxelGrid g;
    g.h = kH;
    box(g, {-16, -16, -4}, {64, 32, 0}, kRock);
    box(g, {0, 0, 0}, {8, 8, 48}, make_vox(MaterialId::Masonry, false));
    box(g, {32, 0, 0}, {40, 8, 48}, make_vox(MaterialId::Masonry, false));
    box(g, {0, 0, 48}, {40, 8, 52}, kConcrete);
    g.compact();
    World w;
    WorldConfig c = w.config();
    c.blast_max_speed = 4.0;  // (a load, nothing shattered or broken)
    w.configure(c);
    w.load(std::move(g));
    w.bake();
    for (int t = 0; t < 5; ++t) w.tick();
    if (blast) w.blast({2.5, 0.5, 5.4}, 0.3, 1e5);
    else w.probe_utilization({2, 2, 2});
    for (int t = 0; t < 30; ++t) w.tick();
    const i64 b0 = w.stats().bonds_broken;
    std::vector<LayerEdit> ed;
    for (i32 y = 0; y < 8; ++y)
      for (i32 z = 48; z < 52; ++z) ed.push_back({{8, y, z}, 150});
    w.set_layer(World::kDamageLayer, ed);
    for (int t = 0; t < 60; ++t) w.tick();
    CHECK(w.stats().bonds_broken == b0);
    return w.stats().max_utilization;
  };
  const f64 steady = run(false), after = run(true);
  MESSAGE("utilization after the damage: " << steady << ", with a blast before it " << after);
  CHECK(after == doctest::Approx(steady).epsilon(0.01));
}

TEST_CASE("world: a piece landing on a thin stub of its own goes on when the stub breaks off") {
  // (a heavy block with a post under it, cut off the slab it stands on: it falls onto the slab
  // post first; the post's last voxels break off as dust, and the block falls on - not stopped
  // in mid-air by a contact that went with them)
  VoxelGrid g;
  g.h = kH;
  box(g, {-100, -100, -4}, {150, 100, 0}, kRock);
  box(g, {0, 0, 0}, {4, 16, 24}, kConcrete);
  box(g, {44, 0, 0}, {48, 16, 24}, kConcrete);
  box(g, {0, 0, 24}, {48, 16, 25}, make_vox(MaterialId::Masonry, false));
  box(g, {20, 4, 33}, {28, 12, 41}, kConcrete);
  box(g, {24, 8, 25}, {25, 9, 33}, kConcrete);
  g.compact();
  World w;
  w.load(std::move(g));
  w.bake();
  w.carve({3.0, 1.0, kH * 26}, 0.1);  // (the post just over the slab: the block drops 1 m)
  f64 before = 0.0, after = 0.0;
  i64 block = -1;
  for (int t = 0; t < 40 && after == 0.0; ++t) {
    w.tick();
    for (const WorldEvent& e : w.take_events())
      if (e.kind == WorldEvent::Kind::PieceAdded && e.voxels >= 400) {
        if (block >= 0 && e.parent == block) after = e.vel.z;
        block = e.id;
      }
    if (block >= 0 && after == 0.0)
      if (const Body* b = w.piece(block)) before = b->v.z;
  }
  MESSAGE("block: " << before << " m/s before its stub broke, " << after << " m/s after");
  REQUIRE(before < -1.0);
  CHECK(after < 0.7 * before);
}

TEST_CASE("world: a frame left hanging by one face of its support is still judged (a stagnating solve goes multigrid)") {
  // A steel portal on rock whose left column's foot is cut away: the frame is a cantilever from
  // the right column's foot, which cannot carry it. Its bonds there break round by round; the
  // last round leaves the frame on one voxel face, a near-mechanism on which a block-Jacobi
  // preconditioned solve stagnates (it would never converge, so never be judged: the frame hung
  // there for good). The solve goes on with the structure's multigrid: the face breaks, it falls.
  const Vox steel = make_vox(MaterialId::Steel, false);
  VoxelGrid g;
  g.h = kH;
  box(g, {140, 20, -4}, {232, 64, 0}, kRock);
  for (i32 x : {160, 208}) box(g, {x, 40, 0}, {x + 3, 43, 44}, steel);
  box(g, {160, 40, 44}, {211, 43, 47}, steel);
  g.compact();
  World w;
  w.load(std::move(g));
  w.bake();
  for (int t = 0; t < 30; ++t) w.tick();
  CHECK(w.pieces().empty());
  std::vector<VoxelEdit> cut;
  for (i32 x = 160; x < 163; ++x)
    for (i32 y = 40; y < 43; ++y)
      for (i32 z = 0; z < 2; ++z) cut.push_back({{x, y, z}, kAir});
  w.set_voxels(cut);
  for (int t = 0; t < 120; ++t) w.tick();
  i32 left = 0;
  for (i32 x = 140; x < 232; ++x)
    for (i32 z = 0; z < 50; ++z) left += vox_solid(w.grid().get(x, 41, z)) ? 1 : 0;
  MESSAGE("portal after its foot is cut: " << left << " voxels standing, " << w.pieces().size() << " pieces, "
                                           << w.stats().bonds_broken << " bonds broken");
  CHECK(left == 0);
  CHECK(!w.pieces().empty());
  CHECK(w.stats().solving == 0);
  CHECK(w.stats().solves_abandoned == 0);
}

TEST_CASE("world: every world has its own materials") {
  // the same cantilever (a 2 m concrete arm from an anchored rock wall) in two worlds, the
  // second's concrete a tenth as strong: its root is ten times as utilized; the process's
  // concrete, and a third world made after, are as they were
  auto root = [](World& w) {
    VoxelGrid g;
    g.h = kH;
    box(g, {-8, -8, -4}, {24, 8, 0}, kRock);
    box(g, {-8, -2, 0}, {0, 2, 32}, kRock);
    box(g, {0, -2, 26}, {16, 2, 30}, kConcrete);
    g.compact();
    w.load(std::move(g));
    // (no design: the arm as it is)
    return w.probe_utilization(IVec3{1, 0, 28});
  };
  World a, b;
  Material weak = b.materials()[MaterialId::Concrete];
  weak.ft *= 0.1;
  weak.fb *= 0.1;
  weak.fc *= 0.1;
  weak.cohesion *= 0.1;
  b.set_material(MaterialId::Concrete, weak);
  const f64 ua = root(a), ub = root(b);
  World c;
  const f64 uc = root(c);
  MESSAGE("cantilever root utilization: " << ua << " in a world of standard concrete, " << ub << " in one of weak concrete, " << uc
                                          << " in a world made after");
  CHECK(ub == doctest::Approx(10.0 * ua).epsilon(0.02));
  CHECK(uc == ua);
  CHECK(material(MaterialId::Concrete).ft == a.materials()[MaterialId::Concrete].ft);
}
