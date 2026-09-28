// Joints (docs/MOTION.md §2): pendulums, hinges, sliders, ropes; their loads, their breaking,
// and how their ends follow the voxels they hold on to.
#include <algorithm>
#include <cmath>
#include <vector>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

constexpr f64 h = 0.125;
const Quat kId{0, 0, 0, 1};

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

// A free object of this session: a box of voxels [lo, hi) of a grid placed at origin.
GridId object(World& w, const V3& origin, const IVec3& lo, const IVec3& hi, MaterialId m) {
  VoxelGrid g;
  g.h = h;
  box(g, lo, hi, make_vox(m, false));
  g.compact();
  return w.add_grid(GridFrame{origin, kId}, std::move(g), false);
}

JointAnchor at_world(const V3& p) {
  JointAnchor a;
  a.kind = JointAnchor::Kind::World;
  a.point = p;
  return a;
}

JointAnchor at_grid(GridId g, const V3& p) {
  JointAnchor a;
  a.kind = JointAnchor::Kind::Grid;
  a.id = g;
  a.point = p;
  return a;
}

}  // namespace

TEST_CASE("joints: a pendulum swings about its pivot, and comes to rest hanging") {
  World w;
  w.load(ground());
  w.bake();
  // a 0.5 m wooden block, its near face's centre 1 m from the pivot, level with it
  const GridId g = object(w, V3{1.0, 0.0, 3.0}, {0, -2, -2}, {4, 2, 2}, MaterialId::Wood);
  REQUIRE(g != 0);
  JointDesc d;
  d.type = JointType::Ball;
  d.a = at_world(V3{1.0 - 0.5 * h, 0.0, 3.0 - 0.5 * h});
  d.b = at_grid(g, V3{1.0 - 0.5 * h, 0.0, 3.0 - 0.5 * h});
  const JointId j = w.add_joint(d);
  REQUIRE(j != 0);
  // (a ball joint on the block's face: it hangs from it; a rope from a pivot 1 m off)
  JointDesc r;
  r.type = JointType::Distance;
  r.a = at_world(V3{0.0, 0.0, 3.0 - 0.5 * h});
  r.b = at_grid(g, V3{1.0 - 0.5 * h, 0.0, 3.0 - 0.5 * h});
  r.rope = false;  // (a rod)
  REQUIRE(w.remove_joint(j));
  const JointId rod = w.add_joint(r);
  REQUIRE(rod != 0);
  w.tick();
  REQUIRE(w.pieces().size() == 1);
  JointState s;
  REQUIRE(w.joint(rod, &s));
  CHECK(s.piece_b == w.pieces().front().id);
  f64 min_x = 1e9, max_err = 0.0, low = 1e9;
  for (int t = 0; t < 240; ++t) {
    w.tick();
    REQUIRE(w.joint(rod, &s));
    max_err = std::max(max_err, std::abs(norm(s.b - s.a) - 1.0 + 0.5 * h));
    const PieceState p = w.pieces().front();
    min_x = std::min(min_x, p.pos.x);
    low = std::min(low, p.pos.z);
  }
  MESSAGE("pendulum: swung to x " << min_x << ", lowest centre z " << low << ", rod length error " << max_err << " m");
  CHECK(max_err < 0.01);
  CHECK(min_x < -0.8);  // (through the bottom and up the other side)
  CHECK(low < 3.0 - 1.0);
  // a minute on: it swings about hanging down, and the rod carries its weight on average
  for (int t = 0; t < 60 * 60; ++t) w.tick();
  f64 mx = 0.0, fz = 0.0;
  const i32 n = 120;
  for (int t = 0; t < n; ++t) {
    w.tick();
    REQUIRE(w.joint(rod, &s));
    mx += w.pieces().front().pos.x / n;
    fz += s.force.z / n;
  }
  const PieceState p = w.pieces().front();
  MESSAGE("pendulum after a minute: mean x " << mx << ", mean pull " << fz << " N (its weight " << p.mass * 9.81 << "), asleep " << p.asleep);
  CHECK(std::abs(mx) < 0.1);
  CHECK(fz == doctest::Approx(p.mass * 9.81).epsilon(0.1));
}

TEST_CASE("joints: a door on a hinge swings when pushed, stops at its limit, and does not sag") {
  World w;
  w.load(ground());
  w.bake();
  // a wooden door 1 m wide, 2 m high, 0.25 m thick, standing 1 cm off the ground, its hinge side at x = 0
  const GridId g = object(w, V3{0.0, 0.0, 0.1}, {0, 0, 0}, {8, 2, 16}, MaterialId::Wood);
  REQUIRE(g != 0);
  JointDesc d;
  d.type = JointType::Hinge;
  const V3 pin{-0.5 * h, 0.5 * h, 0.1 + 7.5 * h};  // (the door's edge, halfway up)
  d.a = at_world(pin);
  d.b = at_grid(g, pin);
  d.axis = V3{0, 0, 1};
  d.limited = true;
  d.lower = -1.2;
  d.upper = 1.2;
  const JointId j = w.add_joint(d);
  REQUIRE(j != 0);
  for (int t = 0; t < 30; ++t) w.tick();
  REQUIRE(w.pieces().size() == 1);
  const PieceState p0 = w.pieces().front();
  JointState s;
  REQUIRE(w.joint(j, &s));
  CHECK(std::abs(s.value) < 0.02);
  // pushed at its free edge
  REQUIRE(w.apply_impulse(p0.id, V3{1.0, 0.0, 1.0}, V3{0.0, 60.0, 0.0}));
  f64 top = 0.0, sag = 0.0;
  for (int t = 0; t < 240; ++t) {
    w.tick();
    REQUIRE(w.joint(j, &s));
    top = std::max(top, s.value);
    sag = std::max(sag, std::abs(w.pieces().front().pos.z - p0.pos.z));
  }
  MESSAGE("door: turned up to " << top << " rad (limit 1.2), its centre moved up or down at most " << sag << " m");
  CHECK(top > 0.8);
  CHECK(top < 1.2 + 0.05);
  CHECK(sag < 0.02);
  CHECK(norm(s.b - s.a) < 0.01);
}

TEST_CASE("joints: a slider carries a block along its axis at its motor's speed, to its limit") {
  World w;
  w.load(ground());
  w.bake();
  const GridId g = object(w, V3{0.0, 0.0, 2.0}, {-2, -2, -2}, {2, 2, 2}, MaterialId::Steel);
  JointDesc d;
  d.type = JointType::Slider;
  d.a = at_world(V3{0.0, 0.0, 2.0});
  d.b = at_grid(g, V3{0.0, 0.0, 2.0});
  d.axis = V3{1, 0, 0};
  d.limited = true;
  d.lower = -0.5;
  d.upper = 1.5;
  d.motor = true;
  d.motor_speed = 0.5;
  d.motor_max = 20000.0;
  const JointId j = w.add_joint(d);
  REQUIRE(j != 0);
  for (int t = 0; t < 60; ++t) w.tick();
  JointState s;
  REQUIRE(w.joint(j, &s));
  const PieceState p = w.pieces().front();
  MESSAGE("slider after 1 s: offset " << s.value << ", velocity " << p.vel.x << " " << p.vel.y << " " << p.vel.z << ", z " << p.pos.z);
  CHECK(s.value == doctest::Approx(0.5).epsilon(0.05));
  CHECK(p.vel.x == doctest::Approx(0.5).epsilon(0.05));
  CHECK(std::abs(p.pos.z - 2.0 + 0.5 * h) < 0.01);  // (held up)
  CHECK(norm(p.ang) < 0.01);
  for (int t = 0; t < 240; ++t) w.tick();
  REQUIRE(w.joint(j, &s));
  MESSAGE("slider after 5 s: offset " << s.value << " (limit 1.5)");
  CHECK(s.value == doctest::Approx(1.5).epsilon(0.02));
  // reversed
  REQUIRE(w.set_joint_motor(j, true, -1.0, 20000.0));
  for (int t = 0; t < 180; ++t) w.tick();
  REQUIRE(w.joint(j, &s));
  CHECK(s.value == doctest::Approx(-0.5).epsilon(0.05));
}

TEST_CASE("joints: a joint gives way beyond its strength, and when the voxel it holds on to goes") {
  World w;
  w.load(ground());
  w.bake();
  // a 1 t steel block on a rope that holds 5 kN
  const GridId g = object(w, V3{0.0, 0.0, 3.0}, {-2, -2, -4}, {2, 2, 0}, MaterialId::Steel);
  JointDesc d;
  d.type = JointType::Distance;
  d.a = at_world(V3{0.0, 0.0, 4.0});
  d.b = at_grid(g, V3{0.0, 0.0, 3.0 - 0.5 * h});
  d.break_force = 5000.0;
  const JointId j = w.add_joint(d);
  REQUIRE(j != 0);
  bool broke = false;
  f64 at = 0.0;
  for (int t = 0; t < 120 && !broke; ++t) {
    w.tick();
    for (const WorldEvent& e : w.take_events())
      if (e.kind == WorldEvent::Kind::JointBroken && e.id == static_cast<i64>(j)) {
        broke = true;
        at = e.strength;
      }
  }
  MESSAGE("rope of 5 kN under a 1 t block: gave way at " << at << " N");
  CHECK(broke);
  CHECK(at > 5000.0);
  CHECK(w.joints().empty());
  JointState s;
  CHECK_FALSE(w.joint(j, &s));
  // a hook in a beam of the world grid: carved away, the rope lets go
  World w2;
  VoxelGrid gg = ground();
  box(gg, {-2, -2, 0}, {2, 2, 40}, make_vox(MaterialId::Concrete, false));
  box(gg, {-2, -2, 40}, {24, 2, 44}, make_vox(MaterialId::Concrete, false));
  gg.compact();
  w2.load(std::move(gg));
  w2.bake();
  const GridId b2 = object(w2, V3{2.0, 0.0, 3.0}, {-2, -2, -2}, {2, 2, 2}, MaterialId::Wood);
  JointDesc r;
  r.type = JointType::Distance;
  r.a = at_grid(kWorldGrid, V3{2.0, 0.0, 40 * h - 0.5 * h});
  r.b = at_grid(b2, V3{2.0, 0.0, 3.0 + 1.5 * h});
  const JointId j2 = w2.add_joint(r);
  REQUIRE(j2 != 0);
  for (int t = 0; t < 60; ++t) w2.tick();
  REQUIRE(w2.joint(j2, &s));
  CHECK(s.piece_a == 0);
  CHECK(s.piece_b != 0);
  w2.carve(V3{2.0, 0.0, 40 * h}, 0.2);
  bool lost = false;
  for (int t = 0; t < 10; ++t) {
    w2.tick();
    for (const WorldEvent& e : w2.take_events())
      if (e.kind == WorldEvent::Kind::JointBroken && e.id == static_cast<i64>(j2)) lost = e.strength == 0.0;
  }
  CHECK(lost);
  CHECK(w2.joints().empty());
}

TEST_CASE("joints: an end follows its voxel into the piece it breaks off in, and into the part of a piece it stays in") {
  // a lamp hangs on a rope from a free beam of the world grid resting on two columns
  World w;
  VoxelGrid g = ground();
  box(g, {-20, -2, 0}, {-16, 2, 32}, make_vox(MaterialId::Concrete, false));
  box(g, {16, -2, 0}, {20, 2, 32}, make_vox(MaterialId::Concrete, false));
  box(g, {-20, -2, 32}, {20, 2, 36}, make_vox(MaterialId::Rc, false));
  g.compact();
  w.load(std::move(g));
  w.bake();
  // (a 0.5 m wooden lamp: 64 voxels, a piece - smaller ones break off as dust)
  const GridId lamp = object(w, V3{0.0, 0.0, 2.5}, {-2, -2, -2}, {2, 2, 2}, MaterialId::Wood);
  JointDesc d;
  d.type = JointType::Distance;
  d.a = at_grid(kWorldGrid, V3{0.0, 0.0, 32 * h - 0.5 * h});
  d.b = at_grid(lamp, V3{0.0, 0.0, 2.5 + 1.5 * h});
  const JointId j = w.add_joint(d);
  REQUIRE(j != 0);
  for (int t = 0; t < 60; ++t) w.tick();
  JointState s;
  REQUIRE(w.joint(j, &s));
  CHECK(s.piece_a == 0);  // (the beam is the world grid's)
  // the columns' tops cut: the beam falls, a piece, with the lamp on it
  w.carve(V3{-18 * h, 0.0, 30 * h}, 0.4);
  w.carve(V3{18 * h, 0.0, 30 * h}, 0.4);
  i64 on = 0;
  for (int t = 0; t < 20; ++t) {
    w.tick();
    if (w.joint(j, &s) && s.piece_a != 0) {
      on = s.piece_a;
      break;
    }
  }
  MESSAGE("the rope's upper end went with piece " << on);
  REQUIRE(on != 0);
  const Body* beam = w.piece(on);
  REQUIRE(beam);
  // the beam piece, cut in two away from the hook: the rope stays with the part the hook is in
  const V3 cut = beam->x + V3{1.2, 0.0, 0.0};
  w.carve(cut, 0.35);
  for (int t = 0; t < 3; ++t) w.tick();
  REQUIRE(w.joint(j, &s));
  MESSAGE("after the cut: the rope's upper end on piece " << s.piece_a << " (it was " << on << ")");
  CHECK(s.piece_a != 0);
  CHECK(norm(s.a - V3{0.0, 0.0, s.a.z}) < 0.6);  // (the hook's part, not the cut-off one)
}

TEST_CASE("joints: a weight on a rope loads the structure it hangs from") {
  auto phi_with = [](bool weight) {
    World w;
    VoxelGrid g = ground();
    // an anchored pier and a concrete cantilever, 2 m out, 0.5 m square
    box(g, {-8, -2, 0}, {0, 2, 32}, make_vox(MaterialId::Rock, true));
    box(g, {0, -2, 26}, {16, 2, 30}, make_vox(MaterialId::Concrete, false));
    g.compact();
    w.load(std::move(g));
    w.bake();
    if (weight) {
      // (a 0.25 t steel block: 4 x 4 x 2 voxels)
      const GridId b = object(w, V3{1.75, 0.0, 1.5}, {-2, -2, 0}, {2, 2, 2}, MaterialId::Steel);
      JointDesc d;
      d.type = JointType::Distance;
      d.a = at_grid(kWorldGrid, V3{1.75, 0.0, 26 * h - 0.5 * h});
      d.b = at_grid(b, V3{1.75, 0.0, 1.5 + 1.5 * h});
      REQUIRE(w.add_joint(d) != 0);
    }
    for (int t = 0; t < 120; ++t) w.tick();
    return std::make_pair(w.probe_utilization(IVec3{1, 0, 28}), static_cast<i32>(w.pieces().size()));
  };
  const auto [bare, n0] = phi_with(false);
  const auto [loaded, n1] = phi_with(true);
  MESSAGE("cantilever root utilization: bare " << bare << ", a 0.25 t block hanging from its end " << loaded);
  CHECK(n0 == 0);
  CHECK(n1 == 1);
  CHECK(loaded > 1.3 * bare);
}

TEST_CASE("joints: a wrecking ball on a crane's rope, swung by the arm, knocks a wall down") {
  World w;
  VoxelGrid g = ground(96);
  // a masonry wall, 8 m long, 3 m high, 0.25 m thick, across the ball's path
  box(g, {-32, 36, 0}, {32, 38, 24}, make_vox(MaterialId::Masonry, false));
  g.compact();
  w.load(std::move(g));
  w.bake();
  const i64 wall0 = w.grid().solid_count();
  // the crane: a mast of the world grid would do; here a kinematic arm turning about the vertical
  // at (0, 0, 6), its steel jib 5 m along +x
  const KinematicId arm = w.add_kinematic(Pose{V3{0.0, 0.0, 6.0}, kId});
  VoxelGrid jib;
  jib.h = h;
  box(jib, {-2, -2, -2}, {2, 2, 2}, make_vox(MaterialId::Steel, true));
  box(jib, {2, -1, -1}, {40, 1, 1}, make_vox(MaterialId::Steel, false));
  jib.compact();
  GridDesc jd;
  jd.body = arm;
  const GridId jg = w.add_grid(jd, std::move(jib));
  REQUIRE(jg != 0);
  // a 1 t steel ball (0.5 m cube) on a 4 m rope from the jib's tip
  const GridId ball = object(w, V3{4.9, 0.0, 1.75}, {-2, -2, -2}, {2, 2, 2}, MaterialId::Steel);
  JointDesc r;
  r.type = JointType::Distance;
  r.a = at_grid(jg, V3{4.9, 0.0, 6.0 - 0.5 * h});
  r.b = at_grid(ball, V3{4.9, 0.0, 1.75 + 1.5 * h});
  const JointId rope = w.add_joint(r);
  REQUIRE(rope != 0);
  for (int t = 0; t < 60; ++t) w.tick();
  JointState s;
  REQUIRE(w.joint(rope, &s));
  MESSAGE("crane: the rope carries " << s.force.z << " N at rest");
  // the arm swings round towards the wall (at +y): a quarter turn in 1.5 s, and on
  for (int t = 0; t < 240; ++t) {
    const f64 u = std::min(1.0, t / 90.0);
    w.set_kinematic_velocity(arm, V3{}, V3{0.0, 0.0, 1.2 * u});
    w.tick();
  }
  const i64 wall1 = w.grid().solid_count();
  MESSAGE("crane: the wall lost " << wall0 - wall1 << " voxels, " << w.pieces().size() << " pieces; the rope "
                                  << (w.joint(rope, &s) ? "holds" : "broke"));
  CHECK(wall1 < wall0);
  CHECK(w.pieces().size() > 3);
}

TEST_CASE("joints: a session with joints is bit-identical on any thread count") {
  auto run = [](int threads) {
    set_num_threads(threads);
    World w;
    VoxelGrid g = ground();
    box(g, {-16, 20, 0}, {16, 22, 24}, make_vox(MaterialId::Masonry, false));
    g.compact();
    w.load(std::move(g));
    w.bake();
    // a chain of three blocks from a pivot, dropped from the side into the wall
    GridId prev = 0;
    V3 top{0.0, 0.0, 4.0};
    for (int k = 0; k < 3; ++k) {
      // (0.5 m wooden links, 1 m apart, joined by rods from face to face)
      const V3 c{0.0, -1.0 * (k + 1), 4.0};
      const GridId b = object(w, c, {-2, -2, -2}, {2, 2, 2}, MaterialId::Wood);
      JointDesc d;
      d.type = JointType::Distance;
      d.a = prev ? at_grid(prev, top) : at_world(top);
      d.b = at_grid(b, c + V3{0.0, 1.5 * h, 0.0});
      d.rope = false;
      REQUIRE(w.add_joint(d) != 0);
      prev = b;
      top = c - V3{0.0, 2.5 * h, 0.0};
    }
    for (int t = 0; t < 300; ++t) w.tick();
    return std::make_pair(w.session_hash(), static_cast<i32>(w.pieces().size()));
  };
  const int hw = num_threads();
  const auto a = run(1), b = run(4);
  set_num_threads(hw);
  MESSAGE("joints session: " << a.second << " pieces, hash " << a.first);
  CHECK(a.second >= 3);
  CHECK(a.first == b.first);
}
