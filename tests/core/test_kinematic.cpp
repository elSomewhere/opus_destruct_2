// Kinematic bodies (docs/GRIDS.md §6): rigid frames the host drives, the grids that move with
// them, what rides on them and what breaks off them.
#include <algorithm>
#include <cmath>
#include <vector>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

constexpr f64 kPi = 3.14159265358979323846;
constexpr f64 h = 0.125;

Quat turn(f64 deg, const V3& axis) {
  const V3 a = normalized(axis);
  const f64 t = 0.5 * deg * kPi / 180.0;
  const f64 s = std::sin(t);
  return Quat{a.x * s, a.y * s, a.z * s, std::cos(t)};
}

void box(VoxelGrid& g, const IVec3& lo, const IVec3& hi, Vox v) {
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y) g.fill_column(x, y, lo[2], hi[2], v);
}

VoxelGrid ground(i32 half = 64) {
  VoxelGrid g;
  g.h = h;
  box(g, {-half, -half, -4}, {half, half, 0}, make_vox(MaterialId::Rock, true));
  g.compact();
  g.lo = {-half, -half, -4};
  g.hi = {half, half, 64};
  return g;
}

// A grid of body k (its frame in the body's), of the voxels fill writes.
template <class Fill>
GridId body_grid(World& w, KinematicId k, const GridFrame& frame, Fill&& fill, bool base = true) {
  VoxelGrid g;
  g.h = h;
  fill(g);
  g.compact();
  GridDesc d;
  d.frame = frame;
  d.body = k;
  d.base = base;
  return w.add_grid(d, std::move(g));
}

// A wooden crate (0.5 m, solid) dropped with its corner at p.
GridId drop_crate(World& w, const V3& p) {
  VoxelGrid c;
  c.h = h;
  box(c, {0, 0, 0}, {4, 4, 4}, make_vox(MaterialId::Wood, false));
  c.compact();
  return w.add_grid(GridFrame{p, Quat{0, 0, 0, 1}}, std::move(c), false);
}

const Quat kId{0, 0, 0, 1};

}  // namespace

TEST_CASE("kinematic: a lift carries a crate up, holds it and brings it down") {
  World w;
  w.load(ground());
  w.bake();
  const KinematicId lift = w.add_kinematic(Pose{V3{0.0, 0.0, 0.25}, kId});
  REQUIRE(lift != 0);
  // (its deck is held by the drive: anchored)
  const GridId deck = body_grid(w, lift, GridFrame{}, [](VoxelGrid& g) { box(g, {-8, -8, 0}, {8, 8, 2}, make_vox(MaterialId::Steel, true)); });
  REQUIRE(deck != 0);
  CHECK(w.grid_body(deck) == lift);
  drop_crate(w, V3{-0.25, -0.25, 0.9});
  for (int t = 0; t < 120; ++t) w.tick();
  REQUIRE(w.pieces().size() == 1);
  const PieceState p0 = w.pieces().front();
  // (on the deck's top: 0.25 + 1.5 h, the crate's centre 0.25 m above it)
  CHECK(p0.pos.z == doctest::Approx(0.25 + 1.5 * h + 0.25).epsilon(0.02));
  // 2 m up at 0.5 m/s
  REQUIRE(w.set_kinematic_velocity(lift, V3{0.0, 0.0, 0.5}, V3{}));
  for (int t = 0; t < 120; ++t) w.tick();
  const PieceState mid = w.pieces().front();
  CHECK(mid.pos.z == doctest::Approx(p0.pos.z + 1.0).epsilon(0.02));
  CHECK(mid.vel.z == doctest::Approx(0.5).epsilon(0.1));
  CHECK_FALSE(mid.asleep);
  for (int t = 0; t < 120; ++t) w.tick();
  REQUIRE(w.set_kinematic_velocity(lift, V3{}, V3{}));
  for (int t = 0; t < 60; ++t) w.tick();
  KinematicState ks;
  REQUIRE(w.kinematic(lift, &ks));
  CHECK(ks.pose.pos.z == doctest::Approx(2.25).epsilon(1e-9));
  CHECK(ks.vel.z == 0.0);
  const PieceState up = w.pieces().front();
  MESSAGE("lift: crate from z " << p0.pos.z << " to " << up.pos.z << " (the deck 2 m up), x " << p0.pos.x << " -> " << up.pos.x);
  CHECK(up.pos.z == doctest::Approx(p0.pos.z + 2.0).epsilon(0.01));
  CHECK(std::abs(up.pos.x - p0.pos.x) < 0.05);
  CHECK(std::abs(up.pos.y - p0.pos.y) < 0.05);
  // the grid's frame in the world went with it
  GridFrame f;
  REQUIRE(w.grid_frame(deck, &f));
  CHECK(f.origin.z == doctest::Approx(2.25).epsilon(1e-9));
  // down again, driven to poses tick by tick (1 m in 2 s)
  for (int t = 1; t <= 120; ++t) {
    REQUIRE(w.drive_kinematic(lift, Pose{V3{0.0, 0.0, 2.25 - t / 120.0}, kId}));
    w.tick();
  }
  for (int t = 0; t < 60; ++t) w.tick();
  const PieceState down = w.pieces().front();
  CHECK(down.pos.z == doctest::Approx(p0.pos.z + 1.0).epsilon(0.01));
}

TEST_CASE("kinematic: a turntable turns what stands on it") {
  World w;
  w.load(ground());
  w.bake();
  const KinematicId tt = w.add_kinematic(Pose{V3{0.0, 0.0, 0.25}, kId});
  body_grid(w, tt, GridFrame{}, [](VoxelGrid& g) {
    for (i32 x = -16; x < 16; ++x)
      for (i32 y = -16; y < 16; ++y)
        if ((x + 0.5) * (x + 0.5) + (y + 0.5) * (y + 0.5) < 16.0 * 16.0) g.fill_column(x, y, 0, 2, make_vox(MaterialId::Steel, true));
  });
  drop_crate(w, V3{0.75, -0.25, 0.9});  // (its centre 1 m out along x)
  for (int t = 0; t < 90; ++t) w.tick();
  REQUIRE(w.pieces().size() == 1);
  const PieceState p0 = w.pieces().front();
  const f64 a0 = std::atan2(p0.pos.y, p0.pos.x), r0 = std::hypot(p0.pos.x, p0.pos.y);
  const f64 om = 0.5;
  REQUIRE(w.set_kinematic_velocity(tt, V3{}, V3{0.0, 0.0, om}));
  for (int t = 0; t < 180; ++t) w.tick();  // 3 s: 1.5 rad
  KinematicState ks;
  REQUIRE(w.kinematic(tt, &ks));
  const f64 turned = 2.0 * std::atan2(ks.pose.rot.z, ks.pose.rot.w);
  CHECK(turned == doctest::Approx(1.5).epsilon(1e-6));
  CHECK(ks.ang.z == om);
  const PieceState p1 = w.pieces().front();
  const f64 a1 = std::atan2(p1.pos.y, p1.pos.x), r1 = std::hypot(p1.pos.x, p1.pos.y);
  MESSAGE("turntable: crate from angle " << a0 << " r " << r0 << " to angle " << a1 << " r " << r1 << ", speed "
                                         << std::hypot(p1.vel.x, p1.vel.y) << " (the table's " << om * r1 << ")");
  CHECK(a1 - a0 == doctest::Approx(1.5).epsilon(0.05));
  CHECK(std::abs(r1 - r0) < 0.1);
  CHECK(std::hypot(p1.vel.x, p1.vel.y) == doctest::Approx(om * r1).epsilon(0.1));
  CHECK_FALSE(p1.asleep);
  // the crate turned with it, about its own centre
  CHECK(std::abs(p1.ang.z - om) < 0.1);
}

TEST_CASE("kinematic: a drawbridge's deck bends under its weight across it, less as it rises") {
  // A 2 m deck (0.25 m, 2 m wide) of reinforced concrete held at its hinge end by the drive (an
  // anchored block): solved in the body's frame, gravity turns in it as the bridge rises.
  World w;
  w.load(ground());
  const KinematicId br = w.add_kinematic(Pose{V3{0.0, 0.0, 2.0}, kId});
  const GridId g = body_grid(w, br, GridFrame{}, [](VoxelGrid& v) {
    box(v, {-2, -8, -2}, {2, 8, 2}, make_vox(MaterialId::Steel, true));
    box(v, {2, -8, -1}, {18, 8, 1}, make_vox(MaterialId::Rc, false));
  });
  REQUIRE(g != 0);
  const IVec3 root{3, 0, 0};
  for (int t = 0; t < 10; ++t) w.tick();
  const f64 flat = w.probe_utilization(g, root);
  // up to 80 degrees in 2 s, easing in and out (its +x end rising: a turn about -y)
  for (int t = 1; t <= 120; ++t) {
    const f64 u = t / 120.0, s = u * u * (3.0 - 2.0 * u);
    REQUIRE(w.drive_kinematic(br, Pose{V3{0.0, 0.0, 2.0}, turn(80.0 * s, V3{0, -1, 0})}));
    w.tick();
  }
  for (int t = 0; t < 30; ++t) w.tick();
  const f64 raised = w.probe_utilization(g, root);
  MESSAGE("drawbridge: root utilization flat " << flat << ", raised 80 degrees " << raised);
  CHECK(flat > 0.05);
  CHECK(raised < 0.5 * flat);
  CHECK(w.pieces().empty());
  // the deck's far end is up in the world
  const V3 tip = w.grid_to_world(g, V3{h * 17, 0.0, 0.0});
  CHECK(tip.z == doctest::Approx(2.0 + h * 17 * std::sin(80.0 * kPi / 180.0)).epsilon(1e-6));
}

TEST_CASE("kinematic: an arm spun up breaks off, and its pieces fly off with the body's motion") {
  // A 1.5 m concrete arm (0.5 m square) on a hub the drive holds, spun up steadily about the
  // vertical: its inertia (centrifugal, and the Euler load of the spin-up) loads it until it breaks.
  World w;
  w.load(ground());
  const KinematicId rotor = w.add_kinematic(Pose{V3{0.0, 0.0, 3.0}, kId});
  const GridId g = body_grid(w, rotor, GridFrame{}, [](VoxelGrid& v) {
    box(v, {-2, -2, -2}, {2, 2, 2}, make_vox(MaterialId::Steel, true));
    box(v, {2, -2, -2}, {14, 2, 2}, make_vox(MaterialId::Concrete, false));
  });
  REQUIRE(g != 0);
  for (int t = 0; t < 30; ++t) w.tick();
  const f64 still = w.probe_utilization(g, IVec3{3, 0, 0});
  CHECK(w.pieces().empty());
  f64 om = 0.0, broke_at = 0.0;
  i32 checked = 0;
  std::vector<i64> seen;
  i64 left = w.grid(g)->solid_count();
  for (int t = 0; t < 900 && checked < 4; ++t) {
    om += 0.02;  // (1.2 rad/s^2)
    REQUIRE(w.set_kinematic_velocity(rotor, V3{}, V3{0.0, 0.0, om}));
    w.tick();
    KinematicState ks;
    REQUIRE(w.kinematic(rotor, &ks));
    const i64 now = w.grid(g)->solid_count();
    for (const PieceState& p : w.pieces()) {
      if (std::find(seen.begin(), seen.end(), p.id) != seen.end()) continue;
      seen.push_back(p.id);
      if (now == left) continue;  // (not off the arm this tick: a piece's own split)
      if (broke_at == 0.0) broke_at = om;
      // (it left the body during this tick: the body's velocity where it is, less what the
      // body's turn and gravity changed since)
      const V3 vb = ks.vel + cross(ks.ang, p.pos - ks.pose.pos);
      const f64 dv = norm(p.vel - vb);
      MESSAGE("piece " << p.id << " off at " << om << " rad/s, r " << std::hypot(p.pos.x, p.pos.y) << ": speed " << norm(p.vel)
                       << " (the body's there " << norm(vb) << ", apart by " << dv << "), spin " << p.ang.z);
      CHECK(dv < 0.2 * norm(vb) + 0.3);
      CHECK(std::abs(p.ang.z - om) < 0.2 * om + 0.2);
      ++checked;
    }
    left = now;
  }
  MESSAGE("spun arm: utilization at rest " << still << ", broke at " << broke_at << " rad/s");
  CHECK(still < 0.9);
  CHECK(broke_at > 1.0);  // (not under its own weight)
  CHECK(checked > 0);
}

TEST_CASE("kinematic: a moving wall pushes a crate along the ground") {
  World w;
  w.load(ground());
  w.bake();
  const KinematicId pusher = w.add_kinematic(Pose{V3{-1.0, 0.0, 0.0}, kId});
  // (a wall 1 m high, clear of the ground)
  body_grid(w, pusher, GridFrame{}, [](VoxelGrid& v) { box(v, {-1, -8, 1}, {1, 8, 9}, make_vox(MaterialId::Steel, true)); });
  drop_crate(w, V3{0.5, -0.25, 0.1});
  for (int t = 0; t < 60; ++t) w.tick();
  REQUIRE(w.pieces().size() == 1);
  const f64 x0 = w.pieces().front().pos.x;
  REQUIRE(w.set_kinematic_velocity(pusher, V3{2.0, 0.0, 0.0}, V3{}));
  for (int t = 0; t < 90; ++t) w.tick();  // 3 m
  REQUIRE(w.set_kinematic_velocity(pusher, V3{}, V3{}));
  for (int t = 0; t < 60; ++t) w.tick();
  KinematicState ks;
  REQUIRE(w.kinematic(pusher, &ks));
  const PieceState p = w.pieces().front();
  const f64 face = ks.pose.pos.x + 0.5 * h;  // (the wall's front face)
  MESSAGE("pushed crate: from x " << x0 << " to " << p.pos.x << ", the wall's face at " << face);
  CHECK(ks.pose.pos.x == doctest::Approx(2.0).epsilon(1e-9));
  CHECK(p.pos.x > face + 0.2);  // (ahead of the wall: never passed through)
  CHECK(p.pos.x > x0 + 1.5);   // (pushed 1.6 m, then sliding on)
  CHECK(p.pos.z < 0.5);         // (on the ground)
}

TEST_CASE("kinematic: what rests on a body's structure loads it") {
  // A crate on the deck of the drawbridge (flat): the deck's root is more utilized with it on.
  auto root_phi = [](bool crate) {
    World w;
    w.load(ground());
    const KinematicId br = w.add_kinematic(Pose{V3{0.0, 0.0, 2.0}, kId});
    const GridId g = body_grid(w, br, GridFrame{}, [](VoxelGrid& v) {
      box(v, {-2, -8, -2}, {2, 8, 2}, make_vox(MaterialId::Steel, true));
      box(v, {2, -8, -1}, {18, 8, 1}, make_vox(MaterialId::Rc, false));
    });
    w.bake();
    if (crate) {
      // (a steel block of 1 t: 0.5 m cube)
      VoxelGrid c;
      c.h = h;
      box(c, {0, 0, 0}, {4, 4, 4}, make_vox(MaterialId::Steel, false));
      c.compact();
      w.add_grid(GridFrame{V3{1.6, -0.25, 2.3}, kId}, std::move(c), false);
    }
    for (int t = 0; t < 120; ++t) w.tick();
    return std::make_pair(w.probe_utilization(g, IVec3{3, 0, 0}), static_cast<i32>(w.pieces().size()));
  };
  const auto [bare, n0] = root_phi(false);
  const auto [loaded, n1] = root_phi(true);
  MESSAGE("drawbridge root utilization: bare " << bare << ", with a 1 t block near its end " << loaded);
  CHECK(n0 == 0);
  CHECK(n1 == 1);
  CHECK(loaded > 1.5 * bare);
}

TEST_CASE("kinematic: released, a body's grids fall as one piece with its velocity") {
  World w;
  w.load(ground());
  const KinematicId k = w.add_kinematic(Pose{V3{0.0, 0.0, 3.0}, kId});
  const GridId g = body_grid(w, k, GridFrame{}, [](VoxelGrid& v) { box(v, {-4, -4, -1}, {4, 4, 1}, make_vox(MaterialId::Steel, true)); });
  REQUIRE(g != 0);
  REQUIRE(w.set_kinematic_velocity(k, V3{1.0, 0.0, 2.0}, V3{0.0, 0.0, 1.0}));
  for (int t = 0; t < 30; ++t) w.tick();
  KinematicState ks;
  REQUIRE(w.kinematic(k, &ks));
  REQUIRE(w.remove_kinematic(k, true));
  CHECK(w.kinematics().empty());
  CHECK(w.grids().empty());
  REQUIRE(w.pieces().size() == 1);
  const PieceState p = w.pieces().front();
  MESSAGE("released: piece at " << p.pos.z << " vel " << p.vel.x << " " << p.vel.y << " " << p.vel.z << " ang " << p.ang.z);
  CHECK(p.voxels == 8 * 8 * 2);
  // (its centre of mass moves as the body's point there: v + w x (c - origin))
  const V3 vc = ks.vel + cross(ks.ang, p.pos - ks.pose.pos);
  CHECK(norm(p.vel - vc) < 1e-9);
  CHECK(p.vel.z == doctest::Approx(2.0).epsilon(1e-9));
  CHECK(p.ang.z == doctest::Approx(1.0).epsilon(1e-9));
  CHECK(p.pos.z == doctest::Approx(ks.pose.pos.z - 0.5 * h).epsilon(1e-9));
  for (int t = 0; t < 240; ++t) w.tick();
  CHECK(w.pieces().front().pos.z < 1.0);  // (it fell)
}

TEST_CASE("kinematic: bodies, their grids and their motion are saved and restored") {
  auto level = [](World& w, KinematicId* door) {
    w.load(ground());
    // a level's body (the level adds it again, before load_delta) with a grid of the level
    *door = w.add_kinematic(Pose{V3{2.0, 0.0, 0.0}, kId});
    body_grid(w, *door, GridFrame{}, [](VoxelGrid& v) { box(v, {0, -1, 0}, {8, 1, 16}, make_vox(MaterialId::Wood, true)); });
    w.bake();
  };
  World a;
  KinematicId door = 0;
  level(a, &door);
  // the door swings; a body of this session carries a crate round
  REQUIRE(a.set_kinematic_velocity(door, V3{}, V3{0.0, 0.0, 0.8}));
  const KinematicId tt = a.add_kinematic(Pose{V3{-3.0, 0.0, 0.25}, kId}, false);
  REQUIRE(tt != 0);
  const GridId ttg = body_grid(
      a, tt, GridFrame{V3{}, turn(10.0, V3{0, 0, 1})}, [](VoxelGrid& v) { box(v, {-12, -12, 0}, {12, 12, 2}, make_vox(MaterialId::Steel, true)); },
      false);
  REQUIRE(ttg != 0);
  REQUIRE(a.set_kinematic_velocity(tt, V3{}, V3{0.0, 0.0, -0.6}));
  drop_crate(a, V3{-2.25, -0.25, 1.0});
  for (int t = 0; t < 90; ++t) a.tick();
  const std::vector<u8> delta = a.save_delta();
  World b;
  KinematicId door_b = 0;
  level(b, &door_b);
  CHECK(door_b == door);
  REQUIRE(b.load_delta(delta));
  REQUIRE(b.kinematics().size() == 2);
  for (KinematicId id : a.kinematics()) {
    KinematicState ka, kb;
    REQUIRE(a.kinematic(id, &ka));
    REQUIRE(b.kinematic(id, &kb));
    CHECK(kb.pose.pos.x == ka.pose.pos.x);
    CHECK(kb.pose.rot.z == ka.pose.rot.z);
    CHECK(kb.ang.z == ka.ang.z);
    CHECK(kb.grids == ka.grids);
  }
  CHECK(b.grid_body(ttg) == tt);
  CHECK(b.state_hash() == a.state_hash());
  // (both go on alike: the pieces are not in the delta, the bodies' motion is)
  KinematicState ka, kb;
  for (int t = 0; t < 30; ++t) {
    a.tick();
    b.tick();
  }
  REQUIRE(a.kinematic(door, &ka));
  REQUIRE(b.kinematic(door, &kb));
  CHECK(kb.pose.rot.z == ka.pose.rot.z);
  GridFrame fa, fb;
  REQUIRE(a.grid_frame(ttg, &fa));
  REQUIRE(b.grid_frame(ttg, &fb));
  CHECK(fb.origin.x == fa.origin.x);
  CHECK(fb.rot.z == fa.rot.z);
}

TEST_CASE("kinematic: a session with moving bodies is bit-identical on any thread count") {
  auto run = [](int threads) {
    set_num_threads(threads);
    World w;
    w.load(ground());
    w.bake();
    const KinematicId lift = w.add_kinematic(Pose{V3{0.0, 0.0, 0.25}, kId});
    body_grid(w, lift, GridFrame{}, [](VoxelGrid& v) { box(v, {-8, -8, 0}, {8, 8, 2}, make_vox(MaterialId::Steel, true)); });
    const KinematicId rotor = w.add_kinematic(Pose{V3{4.0, 0.0, 3.0}, kId});
    body_grid(w, rotor, GridFrame{}, [](VoxelGrid& v) {
      box(v, {-2, -2, -2}, {2, 2, 2}, make_vox(MaterialId::Steel, true));
      box(v, {2, -2, -2}, {18, 2, 2}, make_vox(MaterialId::Concrete, false));
    });
    drop_crate(w, V3{-0.25, -0.25, 0.9});
    for (int t = 0; t < 60; ++t) w.tick();
    w.set_kinematic_velocity(lift, V3{0.0, 0.0, 0.7}, V3{});
    for (int t = 0; t < 240; ++t) {
      if (t % 15 == 0) w.set_kinematic_velocity(rotor, V3{}, V3{0.0, 0.0, 1.0 + static_cast<f64>(t / 15)});
      w.tick();
    }
    return std::make_pair(w.session_hash(), static_cast<i32>(w.pieces().size()));
  };
  const int hw = num_threads();
  const auto a = run(1), b = run(4);
  set_num_threads(hw);
  MESSAGE("kinematic session: " << a.second << " pieces, hash " << a.first);
  CHECK(a.second > 1);
  CHECK(a.first == b.first);
}
