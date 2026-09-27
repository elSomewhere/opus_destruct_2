// The physics core on its own: hand-built worlds, no game harness (links svx_core only).
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
