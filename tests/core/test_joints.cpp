// Joints (docs/MOTION.md §2): pendulums, hinges, sliders, ropes; their loads, their breaking,
// and how their ends follow the voxels they hold on to.
#include <algorithm>
#include <cmath>
#include <vector>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/world/tunables.hpp"
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

TEST_CASE("joints: a slider carries a block along its axis at its drive's speed, to its limit") {
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
  d.drive.kind = JointDrive::Kind::Speed;
  d.drive.speed = 0.5;
  d.drive.max = 20000.0;
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
  d.drive.speed = -1.0;
  REQUIRE(w.set_joint_drive(j, d.drive));
  for (int t = 0; t < 180; ++t) w.tick();
  REQUIRE(w.joint(j, &s));
  CHECK(s.value == doctest::Approx(-0.5).epsilon(0.05));
}

TEST_CASE("joints: a servo lifts a block to its target and holds it; a program moves it; too weak, it stalls") {
  // a 0.5 t steel block on a vertical slider (a lift's car), from the world at 2 m
  auto lift = [](World& w, const JointDrive& drive, JointId* id) {
    w.load(ground());
    w.bake();
    const GridId g = object(w, V3{0.0, 0.0, 2.0}, {-2, -2, -2}, {2, 2, 2}, MaterialId::Steel);
    JointDesc d;
    d.type = JointType::Slider;
    d.a = at_world(V3{0.0, 0.0, 2.0});
    d.b = at_grid(g, V3{0.0, 0.0, 2.0});
    d.axis = V3{0, 0, 1};
    d.limited = true;
    d.lower = -1.5;
    d.upper = 3.0;
    d.drive = drive;
    *id = w.add_joint(d);
    return *id != 0;
  };
  JointDrive to;
  to.kind = JointDrive::Kind::Target;
  to.speed = 1.0;
  to.max = 20000.0;
  to.target = 1.2;
  {
    World w;
    JointId j = 0;
    REQUIRE(lift(w, to, &j));
    for (int t = 0; t < 180; ++t) w.tick();
    JointState s;
    REQUIRE(w.joint(j, &s));
    const PieceState p = w.pieces().front();
    MESSAGE("servo after 3 s: at " << s.value << " (target 1.2), velocity " << p.vel.z);
    CHECK(s.value == doctest::Approx(1.2).epsilon(0.01));
    CHECK(std::abs(p.vel.z) < 0.02);
    // it goes no faster than its speed on the way
    to.target = -1.0;
    REQUIRE(w.set_joint_drive(j, to));
    f64 fastest = 0.0;
    for (int t = 0; t < 60; ++t) {
      w.tick();
      fastest = std::max(fastest, std::abs(w.pieces().front().vel.z));
    }
    MESSAGE("servo down: fastest " << fastest << " m/s (speed 1)");
    CHECK(fastest < 1.05);
    CHECK(fastest > 0.9);
  }
  {
    // a program: from 0 to 2 m and back every 6 s; it follows it
    JointDrive osc = to;
    osc.kind = JointDrive::Kind::Oscillate;
    osc.target = 0.0;
    osc.target2 = 2.0;
    osc.period = 6.0;
    osc.speed = 3.0;
    World w;
    JointId j = 0;
    REQUIRE(lift(w, osc, &j));
    f64 worst = 0.0, top = 0.0;
    for (int t = 0; t < 720; ++t) {
      w.tick();
      JointState s;
      REQUIRE(w.joint(j, &s));
      f64 x = 0.0, rate = 0.0;
      osc.goal(w.time(), &x, &rate);
      if (t > 30) worst = std::max(worst, std::abs(s.value - x));
      top = std::max(top, s.value);
    }
    MESSAGE("program: worst lag " << worst << " m, top " << top);
    CHECK(worst < 0.05);
    CHECK(top == doctest::Approx(2.0).epsilon(0.02));
  }
  {
    // a drive weaker than the block's weight (4.9 kN) cannot lift it: it stalls, sinking to its limit
    JointDrive weak = to;
    weak.max = 2000.0;
    World w;
    JointId j = 0;
    REQUIRE(lift(w, weak, &j));
    for (int t = 0; t < 180; ++t) w.tick();
    JointState s;
    REQUIRE(w.joint(j, &s));
    MESSAGE("weak drive: at " << s.value << " (limit -1.5)");
    CHECK(s.value < -1.4);
  }
}

TEST_CASE("joints: a character stands on a lift's car (a piece on a driven slider) and rides it up and down") {
  World w;
  w.load(ground());
  w.bake();
  // a 1.5 m square steel car, 0.25 m thick, at 1 m on a vertical slider: up 3 m and back every 8 s
  const GridId g = object(w, V3{0.0, 0.0, 1.0}, {-6, -6, -1}, {6, 6, 1}, MaterialId::Steel);
  JointDesc d;
  d.type = JointType::Slider;
  d.a = at_world(V3{0.0, 0.0, 1.0});
  d.b = at_grid(g, V3{0.0, 0.0, 1.0});
  d.axis = V3{0, 0, 1};
  d.drive.kind = JointDrive::Kind::Oscillate;
  d.drive.target = 0.0;
  d.drive.target2 = 3.0;
  d.drive.period = 8.0;
  d.drive.speed = 4.0;
  d.drive.max = 1e5;
  REQUIRE(w.add_joint(d) != 0);
  w.tick();
  // a character (0.6 m x 0.6 m x 1.8 m) dropped onto it; a controller: lifted out of what it is
  // in, carried by what it stands on, falling otherwise
  const f64 dt = w.config().dt;
  V3 feet{0.1, -0.2, 1.4};
  f64 vz = 0.0;
  V3 ride;
  bool on = false;
  i64 stood_on = 0;
  f64 top = 0.0, lowest_gap = 1e9;
  for (int t = 0; t < 960; ++t) {
    const V3 lo{feet.x - 0.3, feet.y - 0.3, feet.z}, hi{feet.x + 0.3, feet.y + 0.3, feet.z + 1.8};
    const f64 up = w.depenetrate(lo, hi, 0.5);
    REQUIRE(up >= 0.0);
    feet.z += up;
    vz = on ? 0.0 : vz - 9.81 * dt;
    const V3 move = (on ? ride : V3{}) * dt + V3{0, 0, vz * dt - (on ? 1e-3 : 0.0)};
    const CollideResult r = w.collide(V3{feet.x - 0.3, feet.y - 0.3, feet.z}, V3{feet.x + 0.3, feet.y + 0.3, feet.z + 1.8}, move);
    feet += r.move;
    on = r.on_ground;
    ride = r.ground_velocity;
    if (on && r.ground_piece != 0) stood_on = r.ground_piece;
    w.tick();
    const PieceState p = w.pieces().front();
    top = std::max(top, feet.z);
    if (t > 60) lowest_gap = std::min(lowest_gap, feet.z - (p.pos.z + 0.125));
  }
  const PieceState p = w.pieces().front();
  MESSAGE("rider: stood on piece " << stood_on << " (the car " << p.id << "), rose to " << top << " m, feet above the car's top at least "
                                   << lowest_gap << " m, now " << feet.z << " (car top " << p.pos.z + 0.125 << ")");
  CHECK(stood_on == p.id);
  CHECK(top > 4.0);
  CHECK(lowest_gap > -0.07);
  CHECK(std::abs(feet.z - (p.pos.z + 0.125)) < 0.1);
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

TEST_CASE("joints: a wrecking ball on a crane's rope, swung by its jib, knocks a wall down") {
  World w;
  VoxelGrid g = ground(96);
  // a masonry wall, 8 m long, 3 m high, 0.25 m thick, across the ball's path
  box(g, {-32, 36, 0}, {32, 38, 24}, make_vox(MaterialId::Masonry, false));
  // the crane's mast: a steel column (0.5 m square, 6 m) at the origin
  box(g, {-2, -2, 0}, {2, 2, 48}, make_vox(MaterialId::Steel, false));
  g.compact();
  w.load(std::move(g));
  w.bake();
  const i64 wall0 = w.grid().solid_count();
  // its jib: a steel beam (5 m along +x) a voxel above the mast's top, on a hinge on the mast's
  // axis, its drive holding it still
  VoxelGrid jib;
  jib.h = h;
  box(jib, {-2, -1, 0}, {40, 1, 2}, make_vox(MaterialId::Steel, false));
  jib.compact();
  const GridId jg = w.add_grid(GridFrame{V3{0.0, 0.0, 49 * h}, kId}, std::move(jib), false);
  REQUIRE(jg != 0);
  JointDesc hd;
  hd.type = JointType::Hinge;
  hd.a = at_grid(kWorldGrid, V3{-0.5 * h, -0.5 * h, 48 * h});
  hd.b = at_grid(jg, V3{-0.5 * h, -0.5 * h, 48 * h});
  hd.axis = V3{0, 0, 1};
  hd.drive.kind = JointDrive::Kind::Target;
  hd.drive.target = 0.0;
  hd.drive.speed = 1.2;
  hd.drive.max = 2e5;
  const JointId arm = w.add_joint(hd);
  REQUIRE(arm != 0);
  // a 1 t steel ball (0.5 m cube) on a 4 m rope from the jib's tip
  const GridId ball = object(w, V3{4.9, 0.0, 49 * h - 4.0 - 2.5 * h}, {-2, -2, -2}, {2, 2, 2}, MaterialId::Steel);
  JointDesc r;
  r.type = JointType::Distance;
  r.a = at_grid(jg, V3{4.9, 0.0, 48.5 * h});
  r.b = at_grid(ball, V3{4.9, 0.0, 49 * h - 4.0 - 0.5 * h});
  r.stiffness = 1e6;
  r.damping = 2e4;
  const JointId rope = w.add_joint(r);
  REQUIRE(rope != 0);
  for (int t = 0; t < 60; ++t) w.tick();
  JointState s;
  REQUIRE(w.joint(rope, &s));
  MESSAGE("crane: the rope carries " << s.force.z << " N at rest");
  // the jib swings round towards the wall (at +y) at 1.2 rad/s
  hd.drive.target = 2.5;
  REQUIRE(w.set_joint_drive(arm, hd.drive));
  for (int t = 0; t < 240; ++t) w.tick();
  const i64 wall1 = w.grid().solid_count();
  MESSAGE("crane: the wall lost " << wall0 - wall1 << " voxels, " << w.pieces().size() << " pieces; the rope "
                                  << (w.joint(rope, &s) ? "holds" : "broke") << ", the jib's hinge " << (w.joint(arm, &s) ? "holds" : "broke"));
  CHECK(wall0 - wall1 > 200);
  CHECK(w.pieces().size() >= 3);  // (the jib, the ball, and what it knocked out)
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

namespace {

// A steel cantilever: a 5 m arm of a 25 cm steel section (2 x 2 voxels) off a stout column, a
// weight of `tonnes` hanging from its tip on a rope. Returns the world (baked, the rope on).
struct Cantilever {
  World w;
  JointId rope = 0;
  V3 tip;
};

void cantilever(Cantilever& c, f64 tonnes, bool hinges) {
  World& w = c.w;
  set_tunable(w, "plastic_hinges", hinges ? 1.0 : 0.0);
  VoxelGrid g = ground();
  const Vox steel = make_vox(MaterialId::SteelSection, false);
  box(g, {-6, -5, -4}, {6, 7, 0}, make_vox(MaterialId::Rc, false));  // a concrete footing ...
  box(g, {-2, -1, -4}, {2, 3, 64}, steel);   // ... the column set in it: 50 cm, 8 m tall
  box(g, {2, 0, 62}, {42, 2, 64}, steel);    // the arm
  g.compact();
  w.load(std::move(g));
  w.bake();
  // the weight: a steel block (7.85 t/m^3) under the tip, well off the ground
  const i32 side = static_cast<i32>(std::lround(std::cbrt(tonnes / 7.85) / h));
  c.tip = V3{41 * h, 0.5 * h, 62 * h - 0.5 * h};
  const f64 zb = 5.0;
  const GridId wt = object(w, V3{c.tip.x, c.tip.y, zb}, {-side / 2, -side / 2, 0}, {side - side / 2, side - side / 2, side}, MaterialId::Steel);
  JointDesc d;
  d.type = JointType::Distance;
  d.a = at_grid(kWorldGrid, c.tip);
  d.b = at_grid(wt, V3{c.tip.x, c.tip.y, zb + (side - 0.5) * h});
  d.length = norm(d.a.point - d.b.point);
  c.rope = w.add_joint(d);
}

}  // namespace

TEST_CASE("plastic hinges: a steel arm bent past its strength folds down at its root, and tears off only once turned far") {
  Cantilever c;
  cantilever(c, 10.0, true);
  REQUIRE(c.rope != 0);
  JointId hinge = 0;
  i64 arm = 0;
  Quat q0;
  f64 turned = 0.0, turned_at_tear = -1.0;
  int formed = -1, tore = -1;
  for (int t = 0; t < 600; ++t) {
    c.w.tick();
    for (const WorldEvent& e : c.w.take_events())
      if (e.kind == WorldEvent::Kind::JointBroken && hinge != 0 && e.id == static_cast<i64>(hinge) && tore < 0) {
        tore = t;
        turned_at_tear = turned;
      }
    if (hinge == 0)
      for (JointId j : c.w.joints()) {
        JointState s;
        if (j == c.rope || !c.w.joint(j, &s) || s.type != JointType::Hinge) continue;
        hinge = j;
        formed = t;
        arm = s.piece_a != 0 ? s.piece_a : s.piece_b;
        if (const Body* b = c.w.piece(arm)) q0 = b->q;
      }
    if (arm != 0)
      if (const Body* b = c.w.piece(arm)) {
        const Quat dq = b->q * conj(q0);
        turned = std::max(turned, 2.0 * std::acos(std::min(1.0, std::abs(dq.w))));
      }
  }
  MESSAGE("bonds broken " << c.w.stats().bonds_broken << ", pieces " << c.w.stats().bodies << ", max util " << c.w.stats().max_utilization
                          << ", structures " << c.w.stats().structures << ", pcg " << c.w.stats().pcg_iters << ", solves " << c.w.stats().solves);
  MESSAGE("10 t on a 5 m arm (25 cm steel section): hinge formed at tick " << formed << ", the arm turned " << turned << " rad, tore at tick "
                                                                         << tore << " having turned " << turned_at_tear << " rad; "
                                                                         << c.w.stats().plastic_hinges << " hinges");
  REQUIRE(hinge != 0);
  CHECK(c.w.stats().plastic_hinges >= 1);
  CHECK(arm != 0);
  CHECK(turned > 0.1);  // (it folded down)
  // hanging off the ground, the weight keeps pulling it round: it tears at its rotation capacity
  CHECK(tore >= 0);
  CHECK(turned_at_tear > 0.25);  // (not before)
  CHECK(tore - formed > 10);     // (it hung on its hinge a while, folding)
  // the same, without plastic hinges: it snaps off at once
  Cantilever d;
  cantilever(d, 5.0, false);
  int loose = -1;
  for (int t = 0; t < 300 && loose < 0; ++t) {
    d.w.tick();
    for (const WorldEvent& e : d.w.take_events())
      if (e.kind == WorldEvent::Kind::PieceAdded && e.voxels > 100) loose = t;
  }
  CHECK(loose >= 0);
  CHECK(d.w.stats().plastic_hinges == 0);
  for (JointId j : d.w.joints()) {
    JointState s;
    if (d.w.joint(j, &s)) CHECK(s.type != JointType::Hinge);
  }
}
