// Damage (docs/DAMAGE.md). Crumpling: a car-like shell of sheet metal on a frame driven into walls
// and into another - its front folds, the collision's energy goes out over the distance it folds,
// what it hits feels the crush force (not a rigid spike), it keeps its id, and walls break when
// that force breaks them. Penetration: impacts hole what their energy density gets through.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/world/world.hpp"
#include "vehicle_materials.hpp"

using namespace svx;

namespace {

constexpr f64 h = 0.125;
constexpr f64 hc = 0.0625;  // (the car's voxels)

void box(VoxelGrid& g, const IVec3& lo, const IVec3& hi, Vox v) {
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y) g.fill_column(x, y, lo[2], hi[2], v);
}

// Road, and a wall across it at x = wall_x (world voxels) as in a building's frame: 5 m wide, 3 m
// high, 25 cm thick, between concrete columns under a concrete beam, on its footing.
VoxelGrid road_and_wall(i32 wall_x, MaterialId wall) {
  VoxelGrid g;
  g.h = h;
  box(g, {-160, -64, -4}, {160, 64, 0}, make_vox(testmat::Asphalt, true));
  box(g, {wall_x - 1, -23, -4}, {wall_x + 3, 23, 0}, make_vox(MaterialId::Rc, true));
  box(g, {wall_x, -20, 0}, {wall_x + 2, 20, 24}, make_vox(wall, false));
  for (i32 y : {-22, 20}) box(g, {wall_x, y, 0}, {wall_x + 2, y + 2, 26}, make_vox(MaterialId::Rc, false));
  box(g, {wall_x, -22, 24}, {wall_x + 2, 22, 26}, make_vox(MaterialId::Rc, false));
  g.lo = {-160, -64, -4};
  g.hi = {160, 64, 64};
  g.compact();
  return g;
}

struct Crate {
  GridId grid = 0;
  std::vector<WheelId> wheels;
  i32 front = 38;  // (its lattice's front: x of its foremost voxels + 1)
};

// A car-like body, 4.6 x 1.75 x 1.25 m in voxels of 6.25 cm: a shell of sheet metal with a
// floor pan, two frame rails along it, a plastic bumper, an engine block at its front; about 1 t,
// on four wheels. `at`: its bottom's centre; yaw 0: its front towards +x.
Crate crate(World& w, const V3& at, f64 yaw = 0.0) {
  VoxelGrid g;
  g.h = hc;
  const Vox sheet = make_vox(testmat::Sheet, false), frame = make_vox(testmat::CarFrame, false),
            engine = make_vox(testmat::Engine, false), plastic = make_vox(testmat::Plastic, false);
  box(g, {-36, -14, 0}, {36, 14, 20}, sheet);
  box(g, {-35, -13, 1}, {35, 13, 19}, kAir);           // (hollow)
  box(g, {-35, -13, 1}, {35, 13, 2}, sheet);           // floor pan
  for (i32 y : {-10, 8}) box(g, {-36, y, 2}, {36, y + 2, 4}, frame);  // rails
  box(g, {36, -13, 2}, {38, 13, 7}, plastic);          // bumper
  box(g, {19, -6, 2}, {29, 6, 10}, engine);            // engine, 0.4 m behind the front
  g.compact();
  const Quat q{0.0, 0.0, std::sin(0.5 * yaw), std::cos(0.5 * yaw)};
  const f64 cy = std::cos(yaw), sy = std::sin(yaw);
  Crate c;
  GridDesc d;
  d.frame = GridFrame{at + V3{0, 0, 0.5 * hc}, q};
  d.voxel_size = hc;
  d.base = false;
  c.grid = w.add_grid(d, std::move(g));
  REQUIRE(c.grid != 0);
  for (int k = 0; k < 4; ++k) {
    const f64 lx = k < 2 ? 1.45 : -1.45, ly = (k % 2 == 0) ? 0.8 : -0.8;
    WheelDesc wd;
    wd.material = testmat::Tyre;  // (what comes off is a tyre)
    wd.mount.kind = JointAnchor::Kind::Grid;
    wd.mount.id = c.grid;
    wd.mount.point = at + V3{lx * cy - ly * sy, lx * sy + ly * cy, 0.0};
    wd.axle = V3{-sy, cy, 0};
    wd.group = 1;
    const WheelId id = w.add_wheel(wd);
    REQUIRE(id != 0);
    c.wheels.push_back(id);
  }
  return c;
}

i64 piece_of(const World& w, const Crate& c) {
  WheelState s;
  for (WheelId id : c.wheels)
    if (w.wheel(id, &s) && s.piece != 0) return s.piece;
  return 0;
}

// Its front (lattice x, metres) now: the face of its foremost voxel across its front, on average
// (an offset crash folds part of it).
f64 front_of(const Body& b) {
  const BodyShape& S = b.shapes[0];
  f64 sum = 0.0;
  i32 n = 0;
  for (i32 y = S.lo[1]; y < S.lo[1] + S.dim[1]; ++y)
    for (i32 z = S.lo[2]; z < S.lo[2] + S.dim[2]; ++z)
      for (i32 x = S.lo[0] + S.dim[0] - 1; x >= 16; --x)
        if (vox_solid(S.get({x, y, z}))) {
          sum += (x + 0.5) * S.h;
          ++n;
          break;
        }
  return n > 0 ? sum / n : 0.0;
}

struct Crash {
  f64 v0 = 0.0, crush = 0.0, peak_decel = 0.0, duration = 0.0, v_after = 0.0, x_after = 0.0;
  i64 id = 0;
  bool same_id = true;
  i32 reshaped = 0;
  i64 wall_breaks = 0, punches = 0;
  f64 mass = 0.0;
};

// The car rolls at `speed` into a wall 1 m ahead of its front.
Crash crash(MaterialId wall, f64 speed) {
  testmat::ensure();
  World w;
  const i32 wall_x = 40;  // (5 m)
  w.load(road_and_wall(wall_x, wall));
  w.bake();
  const f64 front0 = wall_x * h - 0.5 * h - 1.0;  // (its front 1 m before the wall)
  Crate c = crate(w, V3{front0 - 38 * hc, 0, 0.62});
  for (int t = 0; t < 90; ++t) w.tick();
  Crash r;
  r.id = piece_of(w, c);
  REQUIRE(r.id != 0);
  const Body* b = w.piece(r.id);
  REQUIRE(b);
  r.mass = b->mass;
  const f64 f0 = front_of(*b);
  w.apply_impulse(r.id, b->x, V3{b->mass * speed, 0, 0});
  r.v0 = speed;
  (void)w.take_events();
  f64 prev = speed, t_hit = -1.0, t_end = -1.0;
  const i64 breaks0 = w.stats().bonds_broken;
  for (int t = 0; t < 150; ++t) {
    w.tick();
    for (const WorldEvent& e : w.take_events())
      if (e.kind == WorldEvent::Kind::PieceReshaped && e.id == r.id) ++r.reshaped;
    b = w.piece(r.id);
    if (!b) {
      r.same_id = false;
      break;
    }
    const f64 v = b->v.x;
    const f64 decel = (prev - v) / w.config().dt;
    if (decel > 30.0 && t_hit < 0.0) t_hit = t * w.config().dt;
    if (t_hit >= 0.0 && t_end < 0.0 && v < 0.3 * speed) t_end = t * w.config().dt;
    r.peak_decel = std::max(r.peak_decel, decel);
    prev = v;
  }
  if (b) {
    r.crush = f0 - front_of(*b);
    r.v_after = b->v.x;
    r.x_after = b->x.x;
  }
  r.duration = t_end >= 0.0 && t_hit >= 0.0 ? t_end - t_hit : -1.0;
  r.wall_breaks = w.stats().bonds_broken - breaks0;
  r.punches = w.stats().punches;
  return r;
}

}  // namespace

TEST_CASE("crumple: a car at 50 km/h into a concrete wall folds its front, over the time and distance a crash takes") {
  const Crash r = crash(MaterialId::Rc, 13.9);
  MESSAGE("50 km/h into RC: " << r.mass << " kg, its front folded " << r.crush << " m, peak deceleration " << r.peak_decel / 9.81 << " g, "
                              << r.duration * 1000 << " ms to a third of its speed, now " << r.v_after << " m/s; reshaped " << r.reshaped
                              << " times, id kept " << r.same_id << "; wall bonds broken " << r.wall_breaks);
  CHECK(r.same_id);
  CHECK(r.reshaped > 0);
  CHECK(r.crush > 0.12);  // (it folded: the rigid body stopped by a wall folds nothing)
  CHECK(r.crush < 1.2);
  CHECK(r.peak_decel < 120.0 * 9.81);  // (not the rigid spike of a car stopped in a substep: 14 m/s in 8 ms is 170 g)
  CHECK(r.duration > 0.02);            // (tens of ms, not one substep)
  CHECK(std::abs(r.v_after) < 3.0);    // (plastic: it does not bounce off)
}

TEST_CASE("crumple: faster, it folds further; at speed it goes through a brick wall where concrete stands") {
  const Crash slow = crash(MaterialId::Rc, 8.0), fast = crash(MaterialId::Rc, 25.0);
  MESSAGE("into RC: 29 km/h folds " << slow.crush << " m, 90 km/h folds " << fast.crush << " m");
  CHECK(fast.crush > slow.crush);
  // (its front folds back to its engine block, which a brick wall does not stop)
  const Crash brick = crash(MaterialId::Masonry, 25.0);
  MESSAGE("90 km/h into brick: " << brick.punches << " walls broken through, " << brick.wall_breaks << " bonds broken (RC: " << fast.punches << ", "
                                 << fast.wall_breaks << "), the car now at x " << brick.x_after << " (RC: " << fast.x_after << ")");
  CHECK(brick.punches > 0);
  CHECK(fast.punches == 0);
  CHECK(brick.x_after > fast.x_after + 0.5);  // (it goes on through the wall)
}

TEST_CASE("crumple: two cars head on both fold, and a crash is bit-identical on any thread count") {
  auto run = [](int threads, f64* crush_a, f64* crush_b, bool* ids) {
    set_num_threads(threads);
    testmat::ensure();
    World w;
    VoxelGrid g;
    g.h = h;
    box(g, {-160, -64, -4}, {160, 64, 0}, make_vox(testmat::Asphalt, true));
    g.lo = {-160, -64, -4};
    g.hi = {160, 64, 64};
    g.compact();
    w.load(std::move(g));
    w.bake();
    Crate a = crate(w, V3{-4.0, 0.2, 0.62}), b = crate(w, V3{4.0, -0.2, 0.62}, 3.14159265358979323846);
    for (int t = 0; t < 90; ++t) w.tick();
    const i64 ia = piece_of(w, a), ib = piece_of(w, b);
    REQUIRE(ia != 0);
    REQUIRE(ib != 0);
    const f64 fa = front_of(*w.piece(ia)), fb = front_of(*w.piece(ib));
    w.apply_impulse(ia, w.piece(ia)->x, V3{w.piece(ia)->mass * 12.0, 0, 0});
    w.apply_impulse(ib, w.piece(ib)->x, V3{-w.piece(ib)->mass * 12.0, 0, 0});
    for (int t = 0; t < 120; ++t) w.tick();
    *ids = w.piece(ia) && w.piece(ib);
    *crush_a = *ids ? fa - front_of(*w.piece(ia)) : -1.0;
    *crush_b = *ids ? fb - front_of(*w.piece(ib)) : -1.0;
    return w.session_hash();
  };
  f64 a1, b1, a4, b4;
  bool i1, i4;
  const u64 h1 = run(1, &a1, &b1, &i1), h4 = run(4, &a4, &b4, &i4);
  set_num_threads(0);
  MESSAGE("head on at 2 x 43 km/h: fronts folded " << a1 << " m and " << b1 << " m");
  CHECK(i1);
  CHECK(a1 > 0.08);
  CHECK(b1 > 0.08);
  CHECK(h1 == h4);
}

TEST_CASE("damage: a round holes sheet metal, a rocket's energy density a steel section, nothing solid steel; a cut takes all") {
  // three walls on anchored rock, 3 voxels thick: the game's sheet metal (2e4 J/m^3 to hole), a
  // steel section (1.5e5), solid steel (1e9)
  testmat::ensure();
  const MaterialId mats[3] = {testmat::Sheet, MaterialId::SteelSection, MaterialId::Steel};
  auto make = [&](bool penetration) {
    VoxelGrid g;
    g.h = h;
    box(g, {-8, -8, -4}, {72, 24, 0}, make_vox(MaterialId::Rock, true));
    for (int k = 0; k < 3; ++k) box(g, {24 * k, 8, 0}, {24 * k + 16, 11, 16}, make_vox(mats[k], false));
    g.lo = {-8, -8, -4};
    g.hi = {72, 24, 20};
    g.compact();
    auto w = std::make_unique<World>();
    WorldConfig c = w->config();
    c.impact_penetration = penetration;
    w->configure(c);
    w->load(std::move(g));
    w->bake();
    return w;
  };
  // (a wall's voxels, where they are: its grid's and the pieces')
  auto count = [](const World& w, int k) {
    i64 n = 0;
    for (i32 x = 24 * k; x < 24 * k + 16; ++x)
      for (i32 y = 8; y < 11; ++y)
        for (i32 z = 0; z < 16; ++z) n += vox_solid(w.grid().get(x, y, z));
    for (const PieceState& p : w.pieces())
      for (const BodyShape& S : w.piece(p.id)->shapes)
        for (Vox v : S.vox) n += vox_solid(v);
    return n;
  };
  auto face = [](int k) { return V3{h * (24 * k + 8), h * 8, h * 8}; };  // (the middle of its face)
  auto hit = [&](bool penetration, f64 radius, f64 energy) {
    auto w = make(penetration);
    std::array<i64, 3> before{}, lost{};
    for (int k = 0; k < 3; ++k) before[size_t(k)] = count(*w, k);
    for (int k = 0; k < 3; ++k) {
      if (energy < 0.0)
        w->carve(face(k), radius);
      else
        w->shoot(face(k), radius, energy);
    }
    for (int t = 0; t < 3; ++t) w->tick();
    for (int k = 0; k < 3; ++k) lost[size_t(k)] = before[size_t(k)] - count(*w, k);
    return lost;
  };
  // a pistol's round (0.15 m, 500 J): the sheet is holed, the sections are not
  const auto round = hit(true, 0.15, 500.0);
  MESSAGE("a round takes " << round[0] << " voxels of sheet, " << round[1] << " of steel section, " << round[2] << " of steel");
  CHECK(round[0] > 0);
  CHECK(round[1] == 0);
  CHECK(round[2] == 0);
  // a rocket's energy density (1 MJ over a 1 m sphere) on half the radius: the section is holed too
  const auto rocket = hit(true, 0.5, 1e6 / 8.0);
  MESSAGE("at a rocket's density: " << rocket[0] << " sheet, " << rocket[1] << " steel section, " << rocket[2] << " steel");
  CHECK(rocket[0] > round[0]);
  CHECK(rocket[1] > 0);
  CHECK(rocket[2] == 0);
  // a cut (no energy) takes whatever is not indestructible
  const auto cut = hit(true, 0.15, -1.0);
  CHECK(cut[0] > 0);
  CHECK(cut[1] > 0);
  CHECK(cut[2] > 0);
  // impact_penetration off (the structural reference): impacts never remove ductile material
  const auto reference = hit(false, 0.5, 1e6 / 8.0);
  CHECK(reference[0] == 0);
  CHECK(reference[1] == 0);
  CHECK(reference[2] == 0);
}
