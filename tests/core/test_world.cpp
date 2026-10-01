// The physics core on its own: hand-built worlds, no game harness (links svx_core only).
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/material/material.hpp"
#include "svx/world/tunables.hpp"
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

// A tree as a generator might make one: a post, a block on it, and two clusters of 27 voxels
// that touch the block along an edge and at a corner only - no face: they hold on to nothing.
class TreeSource final : public ChunkSource {
 public:
  static bool edge_cluster(const IVec3& p) { return p[0] >= 22 && p[0] < 25 && p[1] >= 14 && p[1] < 17 && p[2] >= 28 && p[2] < 31; }
  static bool corner_cluster(const IVec3& p) { return p[0] >= 22 && p[0] < 25 && p[1] >= 22 && p[1] < 25 && p[2] >= 28 && p[2] < 31; }
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    out.assign(kChunkVox, kAir);
    bool any = false;
    for (int x = 0; x < kChunk; ++x)
      for (int y = 0; y < kChunk; ++y)
        for (int z = 0; z < kChunk; ++z) {
          const IVec3 p{cc[0] * kChunk + x, cc[1] * kChunk + y, cc[2] * kChunk + z};
          Vox v = kAir;
          if (p[2] < 0 && p[2] >= -8) v = kRock;
          const bool post = p[0] >= 16 && p[0] < 18 && p[1] >= 16 && p[1] < 18 && p[2] >= 0 && p[2] < 24;
          const bool block = p[0] >= 12 && p[0] < 22 && p[1] >= 12 && p[1] < 22 && p[2] >= 24 && p[2] < 28;
          if (post || block || edge_cluster(p) || corner_cluster(p)) v = kConcrete;
          if (v != kAir) {
            out[size_t(chunk_index(p))] = v;
            any = true;
          }
        }
    return any;
  }
  IVec3 chunk_lo() const override { return {-2, -2, -1}; }
  IVec3 chunk_hi() const override { return {3, 3, 2}; }
};

TEST_CASE("world: what held on to a piece that came loose only edge to edge, corner to corner, is checked for support: it falls") {
  auto session = [](bool recheck) {
    WorldConfig cfg;
    cfg.recheck_vacated = recheck;
    World w;
    w.configure(cfg);
    VoxelGrid g;
    g.h = kH;
    w.load(std::move(g));
    StreamConfig sc;
    sc.load_radius = 12.0;
    sc.evict_radius = 20.0;
    sc.chunks_per_tick = 400;
    w.enable_streaming(std::make_shared<TreeSource>(), sc);
    w.set_focus(V3{kH * 17, kH * 17, 0.0});
    Run r;
    run(w, 5, &r);
    REQUIRE(vox_solid(w.grid().get({23, 15, 29})));
    REQUIRE(vox_solid(w.grid().get({23, 23, 29})));
    w.carve(V3{kH * 17, kH * 17, kH * 8}, 0.3);  // (the post, under the block)
    run(w, 120, &r);
    i32 left = 0;  // (the clusters' voxels still in the grid)
    for (i32 x = 22; x < 25; ++x)
      for (i32 y = 12; y < 25; ++y)
        for (i32 z = 28; z < 31; ++z) left += vox_solid(w.grid().get({x, y, z})) ? 1 : 0;
    CHECK(!vox_solid(w.grid().get({17, 17, 25})));  // (the block fell either way)
    return left;
  };
  const i32 with = session(true), without = session(false);
  MESSAGE("voxels of the clusters left in the air: " << with << " (re-checked), " << without << " (not)");
  CHECK(with == 0);
  CHECK(without == 54);  // (the reference's way: nothing ever looked at them)
}

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

namespace {

// A delta of one piece, written by hand: its shape (dim, one run per array of `fill`) and its
// broken junction samples, as a malformed or hostile file might hold them.
std::vector<u8> one_piece_delta(i64 id, std::array<u32, 3> dim, Vox fill, const std::vector<u64>& jbrk) {
  auto p8 = [](std::vector<u8>& b, u8 v) { b.push_back(v); };
  auto p32 = [](std::vector<u8>& b, u32 v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
  };
  auto p64 = [](std::vector<u8>& b, u64 v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
  };
  auto pf = [&](std::vector<u8>& b, f64 x) {
    u64 u;
    std::memcpy(&u, &x, 8);
    p64(b, u);
  };
  std::vector<u8> rec;
  p8(rec, 2);  // (piece record version)
  p64(rec, static_cast<u64>(id));
  p64(rec, 0);
  p8(rec, 1);  // (asleep)
  for (f64 x : {0.0, 0.0, 10.0}) pf(rec, x);
  for (f64 x : {0.0, 0.0, 0.0, 1.0}) pf(rec, x);
  for (int k = 0; k < 6; ++k) pf(rec, 0.0);
  p32(rec, 0);
  p32(rec, 0);
  for (int k = 0; k < 3; ++k) pf(rec, 0.0);
  p32(rec, 1);  // (one shape)
  p8(rec, 1);   // (identity frame)
  p32(rec, 0);
  pf(rec, 0.125);
  p32(rec, 0);
  for (int a = 0; a < 3; ++a) p32(rec, 0);
  for (u32 d : dim) p32(rec, d);
  const u32 n = dim[0] * dim[1] * dim[2];
  p32(rec, 1), p32(rec, n), p8(rec, fill);                        // vox
  p32(rec, 1), p32(rec, n), p32(rec, vox_solid(fill) ? 1u : 0u);  // frag
  p32(rec, 1), p32(rec, n), p8(rec, 0);                           // brk
  p8(rec, 0);
  p32(rec, static_cast<u32>(jbrk.size()));
  for (u64 j : jbrk) p64(rec, j);
  p32(rec, 1);  // (one fragment)
  for (int k = 0; k < 3; ++k) pf(rec, 0.0);
  pf(rec, 1.0);
  for (int k = 0; k < 9; ++k) pf(rec, k % 4 == 0 ? 1.0 : 0.0);
  p8(rec, static_cast<u8>(MaterialId::Concrete));
  p32(rec, n);
  pf(rec, 1.0);
  p32(rec, 0);
  std::vector<u8> d;
  p32(d, 0x44585653), p32(d, 4), p32(d, 0);             // (world grid: no records)
  p32(d, 0x47585653), p32(d, 7), p32(d, 0), p32(d, 0);  // (grids: none)
  p32(d, 0x53534553);                                   // (the session)
  p64(d, 0), p64(d, static_cast<u64>(id + 1)), p32(d, 1), p32(d, 1);
  p32(d, 1);
  p32(d, static_cast<u32>(rec.size()));
  d.insert(d.end(), rec.begin(), rec.end());
  p32(d, 0), p32(d, 0), p32(d, 0);  // (joints, dead loads, wheels)
  p32(d, 0);                        // (archived groups)
  p32(d, 0), p32(d, 0);             // (articulations)
  return d;
}

}  // namespace

TEST_CASE("world: a delta's piece records are checked whole: boxes bounded by the record's size, junctions in the box, voxels in the piece") {
  World w;
  VoxelGrid g;
  g.h = kH;
  g.fill_column(0, 0, 0, 1, kRock);
  g.compact();
  w.load(std::move(g));
  REQUIRE(w.bake());
  // (as written: a piece of 2 x 2 x 2 voxels, one broken junction sample)
  const u64 j = (u64(3) << 16) | (u64(1) << 8) | 5;
  REQUIRE(w.load_delta(one_piece_delta(7, {2, 2, 2}, kConcrete, {j})));
  CHECK(w.pieces().size() == 1);
  // a junction sample of a cell outside the box, of a seventh face, of no sample
  CHECK_FALSE(w.load_delta(one_piece_delta(7, {2, 2, 2}, kConcrete, {u64(8) << 16})));
  CHECK_FALSE(w.load_delta(one_piece_delta(7, {2, 2, 2}, kConcrete, {u64(6) << 8})));
  CHECK_FALSE(w.load_delta(one_piece_delta(7, {2, 2, 2}, kConcrete, {u64(64)})));
  // a few hundred bytes for a box of 16 M cells of air (a piece of no voxels)
  CHECK_FALSE(w.load_delta(one_piece_delta(7, {4096, 4096, 1}, kAir, {})));
  CHECK_FALSE(w.load_delta(one_piece_delta(7, {2, 2, 2}, kAir, {})));
  // an id no session reaches
  CHECK_FALSE(w.load_delta(one_piece_delta(i64(1) << 62, {2, 2, 2}, kConcrete, {})));
  CHECK(w.pieces().size() == 1);  // (refused whole: the world is as it was)
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

TEST_CASE("memory: a level loaded after a streamed one keeps nothing of its archive - its saves load") {
  PillarTrip t(0.0);
  t.cut(1);
  REQUIRE_FALSE(t.w.pieces().empty());
  t.go(4, 30);  // (the rubble out of range, archived)
  REQUIRE(t.w.stats().archived_pieces > 0);
  // the next level - bounded - in the same world
  t.w.load(table_world());
  CHECK(t.w.stats().archived_pieces == 0);
  REQUIRE(t.w.bake());
  for (int k = 0; k < 10; ++k) t.w.tick();
  const std::vector<u8> d = t.w.save_delta();
  World w2;
  w2.load(table_world());
  REQUIRE(w2.bake());
  CHECK(w2.load_delta(d));
  // ... and the streamed one again: as new
  PillarTrip fresh(0.0);
  t.w.load(VoxelGrid{});
  StreamConfig sc;
  sc.load_radius = 20.0;
  sc.evict_radius = 28.0;
  sc.chunks_per_tick = 400;
  t.w.enable_streaming(std::make_shared<PillarSource>(), sc);
  fresh.go(1, 10);
  t.go(1, 10);
  CHECK(t.w.stats().archived_pieces == 0);
  CHECK(t.w.state_hash() == fresh.w.state_hash());
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

TEST_CASE("memory: the pieces' budget releases awake pieces' solvers, then culls the smallest pieces, sleeping ones first") {
  auto session = [](f64 mb, bool release, i64* peak, WorldStats* st) {
    WorldConfig cfg;
    cfg.memory.piece_mb = mb;
    cfg.release_solvers = release;
    World w;
    w.configure(cfg);
    w.load(table_world());
    w.bake();
    for (i32 lx : {0, 30})
      for (i32 ly : {0, 30}) w.carve(leg_centre(lx, ly, 1.5), 0.3);
    for (int t = 0; t < 300; ++t) {
      w.tick();
      // (the pieces' own data: the contacts - the solver's scratch of each substep - aside)
      const i64 own = w.memory().pieces - static_cast<i64>(w.rigid().contacts().capacity() * sizeof(Contact));
      *peak = std::max(*peak, own);
    }
    *st = w.stats();
  };
  // (checked every tick, by what the pieces use: what the allocator holds for them is a little more)
  auto bound = [](f64 mb) { return static_cast<i64>(1.2 * mb * 1048576.0) + 16 * 1024; };
  // a budget the pieces' solvers alone exceed: released, nothing is culled - kept, pieces are
  i64 peak = 0, peak_kept = 0, peak_tight = 0;
  WorldStats st, st_kept, st_tight;
  session(0.3, true, &peak, &st);
  session(0.3, false, &peak_kept, &st_kept);
  // one the pieces exceed without their solvers: released, and the smallest pieces culled
  session(0.1, true, &peak_tight, &st_tight);
  MESSAGE("0.3 MB: " << st.released_solvers << " solvers released, " << st.culled_pieces << " pieces culled, peak " << peak / 1024
                     << " KB (solvers kept: " << st_kept.culled_pieces << " culled, peak " << peak_kept / 1024 << " KB); 0.1 MB: "
                     << st_tight.released_solvers << " released, " << st_tight.culled_pieces << " culled, peak " << peak_tight / 1024 << " KB");
  CHECK(st.released_solvers > 0);
  CHECK(st.culled_pieces == 0);
  CHECK(st_kept.culled_pieces > 0);
  CHECK(st_tight.culled_pieces > 0);
  CHECK(peak <= bound(0.3));
  CHECK(peak_kept <= bound(0.3));
  CHECK(peak_tight <= bound(0.1));
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

TEST_CASE("world: a blast's shard keeps its own bonds - a shot breaks it as a piece, not all to dust (shards_hold_together)") {
  auto shot_shard = [](bool hold, i32* voxels, i64* left) {
    WorldConfig cfg;
    cfg.shards_hold_together = hold;
    World w;
    w.configure(cfg);
    VoxelGrid g;
    g.h = kH;
    box(g, {-48, -48, -4}, {48, 48, 0}, kRock);
    box(g, {0, 0, 0}, {48, 8, 32}, kConcrete);  // (a wall 6 m long, 1 m thick, 4 m high)
    g.compact();
    g.lo = {-48, -48, -4};
    g.hi = {48, 48, 64};
    w.load(std::move(g));
    REQUIRE(w.bake());
    w.blast(V3{kH * 24, kH * -1, kH * 16}, 0.6, 4e5);
    for (int t = 0; t < 2; ++t) w.tick();
    const Body* best = nullptr;
    for (const PieceState& p : w.pieces()) {
      const Body* b = w.piece(p.id);
      if (!best || b->count > best->count) best = b;
    }
    REQUIRE(best != nullptr);
    const i64 id = best->id;
    *voxels = best->count;
    // a bullet's hole at one of its voxels
    const BodyShape& S = best->shapes[0];
    IVec3 v{0, 0, 0};
    for (i32 i = 0; i < static_cast<i32>(S.vox.size()); ++i)
      if (vox_solid(S.vox[size_t(i)])) {
        v = S.voxel(i);
        break;
      }
    w.take_events();
    w.carve(best->lattice_to_world(0, V3{S.h * v[0], S.h * v[1], S.h * v[2]}), 0.07);
    w.tick();
    *left = 0;
    if (const Body* b = w.piece(id)) *left += b->count;
    for (const WorldEvent& e : w.take_events())
      if (e.kind == WorldEvent::Kind::PieceAdded && e.parent == id) *left += e.voxels;
  };
  i32 n = 0;
  i64 left = 0;
  shot_shard(true, &n, &left);
  MESSAGE("the blast's largest shard: " << n << " voxels, " << left << " of them a piece still after a shot");
  CHECK(n > 40);
  CHECK(left >= n / 2);
  shot_shard(false, &n, &left);  // (the reference's: a shard of loose voxels)
  CHECK(left == 0);
}

TEST_CASE("world: a piece cut down to fewer voxels than a piece has turns to dust") {
  World w;
  VoxelGrid g;
  g.h = kH;
  box(g, {-48, -48, -4}, {48, 48, 0}, kRock);
  g.compact();
  g.lo = {-48, -48, -4};
  g.hi = {48, 48, 64};
  w.load(std::move(g));
  REQUIRE(w.bake());
  // a 4 x 4 x 3 block (48 voxels) dropped in, cut down to a corner of 8
  std::vector<VoxelEdit> e;
  for (i32 x = 0; x < 4; ++x)
    for (i32 y = 0; y < 4; ++y)
      for (i32 z = 2; z < 5; ++z) e.push_back({{x, y, z}, kConcrete});
  w.set_voxels(e);
  for (int t = 0; t < 120; ++t) w.tick();
  REQUIRE(w.pieces().size() == 1);
  const i64 id = w.pieces().front().id;
  const Body* b = w.piece(id);
  std::vector<IVec3> cut;
  for (const BodyShape& S : b->shapes)
    for (i32 i = 0; i < static_cast<i32>(S.vox.size()); ++i) {
      if (!vox_solid(S.vox[size_t(i)])) continue;
      const IVec3 p = S.voxel(i);
      if (p[0] >= 2 || p[1] >= 2) cut.push_back(p);
    }
  REQUIRE(cut.size() >= 30);
  w.take_events();
  REQUIRE(w.remove_piece_voxels(id, 0, cut, true));
  w.tick();
  CHECK(w.pieces().empty());  // (what was left - fewer voxels than min_body_voxels - is dust)
  bool dust = false;
  for (const WorldEvent& ev : w.take_events()) dust = dust || ev.kind == WorldEvent::Kind::Dust;
  CHECK(dust);
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

TEST_CASE("world: every tunable has an id of its own, from its name (command logs replay across builds)") {
  std::unordered_set<u32> ids;
  for (i32 i = 0; i < tunable_count(); ++i) {
    const u32 id = tunable_id(i);
    CHECK(id != 0);
    CHECK(ids.insert(id).second);  // (unique)
    CHECK(tunable_by_id(id) == i);
  }
  // (the id is the name's: FNV-1a, 32 bits - whatever the order of the list)
  u32 h = 2166136261u;
  for (const char* c = "plastic_hinges"; *c; ++c) h = (h ^ static_cast<u8>(*c)) * 16777619u;
  CHECK(tunable_id(tunable_index("plastic_hinges")) == h);
  CHECK(tunable_by_id(0x12345678u) == -1);
  CHECK(tunable_id(-1) == 0);
}

TEST_CASE("world: a moved world goes on as it was, its systems handed the world it is now") {
  struct Seen : WorldSystem {
    std::vector<const World*> worlds;
    const char* name() const override { return "seen"; }
    void step(World& w, f64) override { worlds.push_back(&w); }
  };
  auto run = [](bool move) {
    auto seen = std::make_shared<Seen>();
    World a;
    a.add_system(seen);
    a.load(table_world());
    a.bake();
    for (int t = 0; t < 5; ++t) a.tick();
    a.carve(leg_centre(0, 0, 1.0), 0.4);
    World b = move ? std::move(a) : World();
    World& w = move ? b : a;
    for (int t = 0; t < 60; ++t) w.tick();
    const bool handed = !seen->worlds.empty() && seen->worlds.back() == &w;
    CHECK(handed);
    CHECK(w.grid().h == kH);            // (the inline reads, through the moved implementation)
    CHECK_FALSE(w.has_oriented_grids());
    return w.session_hash();
  };
  CHECK(run(true) == run(false));  // (the same world, bit for bit)
}
