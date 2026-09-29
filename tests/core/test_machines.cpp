// Machines (docs/MOTION.md): pieces on driven joints held by structures - a lift's car on a
// slider, a turntable on a hinge. What stands on them rides; the structure they hang on carries
// them, and shot away, lets them fall.
#include <algorithm>
#include <cmath>
#include <optional>
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
  g.lo = {-half, -half, -4};
  g.hi = {half, half, 64};
  return g;
}

// A free part of the level on the world's lattice: voxels [lo, hi) of a grid at world voxel o.
GridId part(World& w, const IVec3& o, const IVec3& lo, const IVec3& hi, MaterialId m) {
  VoxelGrid g;
  g.h = h;
  box(g, lo, hi, make_vox(m, false));
  g.compact();
  return w.add_grid(GridFrame{V3{h * o[0], h * o[1], h * o[2]}, kId}, std::move(g));
}

// A wooden crate (0.5 m, solid) dropped with its corner at p.
GridId drop_crate(World& w, const V3& p) {
  VoxelGrid c;
  c.h = h;
  box(c, {0, 0, 0}, {4, 4, 4}, make_vox(MaterialId::Wood, false));
  c.compact();
  return w.add_grid(GridFrame{p, kId}, std::move(c), false);
}

JointAnchor at(GridId g, const V3& p) {
  JointAnchor a;
  a.kind = JointAnchor::Kind::Grid;
  a.id = g;
  a.point = p;
  return a;
}

// A lift: a concrete pillar (1 m square, 5 m) at x [-1.5, -0.5] m, and beside it a timber car
// (2 m square, 0.25 m) on a slider held by the pillar's face (a voxel's gap between them), its
// bottom `z0` voxels up. The car's piece is the one piece; the slider's id is returned.
JointId lift(World& w, const JointDrive& drive, i32 z0 = 2) {
  VoxelGrid g = ground();
  box(g, {-12, -4, 0}, {-4, 4, 40}, make_vox(MaterialId::Concrete, false));
  g.compact();
  w.load(std::move(g));
  w.bake();
  const GridId car = part(w, {-3, -8, z0}, {0, 0, 0}, {16, 16, 2}, MaterialId::Wood);
  const V3 p{-4.0 * h, 0.0, h * z0};  // (the gap: the pillar's voxel next to it, and the car's)
  JointDesc d;
  d.type = JointType::Slider;
  d.a = at(kWorldGrid, p);
  d.b = at(car, p);
  d.axis = V3{0, 0, 1};
  d.drive = drive;
  return w.add_joint(d);
}

JointDrive servo(f64 target, f64 speed = 0.5) {
  JointDrive d;
  d.kind = JointDrive::Kind::Target;
  d.target = target;
  d.speed = speed;
  d.max = 40000.0;
  return d;
}

// A piece's state now (none: nullopt).
std::optional<PieceState> piece_of(const World& w, i64 id) {
  for (const PieceState& p : w.pieces())
    if (p.id == id) return p;
  return std::nullopt;
}

}  // namespace

TEST_CASE("machines: a lift's car carries a crate up, holds it there and brings it down") {
  World w;
  const JointId j = lift(w, servo(0.0));
  REQUIRE(j != 0);
  w.tick();
  REQUIRE(w.pieces().size() == 1);
  const i64 car = w.pieces().front().id;
  // a crate dropped on the car (its top at 3.5 h), settling
  drop_crate(w, V3{0.5, -0.25, 3.5 * h + 0.3});
  for (int t = 0; t < 120; ++t) w.tick();
  REQUIRE(w.pieces().size() == 2);
  const i64 crate = w.pieces().back().id;
  const PieceState p0 = *piece_of(w, crate);
  CHECK(p0.pos.z == doctest::Approx(3.5 * h + 0.25).epsilon(0.02));
  // up 2 m at 0.5 m/s: the servo takes it there and holds it
  REQUIRE(w.set_joint_drive(j, servo(2.0)));
  for (int t = 0; t < 120; ++t) w.tick();
  const PieceState mid = *piece_of(w, crate);
  CHECK(mid.pos.z == doctest::Approx(p0.pos.z + 1.0).epsilon(0.03));
  CHECK(mid.vel.z == doctest::Approx(0.5).epsilon(0.1));
  CHECK_FALSE(mid.asleep);
  for (int t = 0; t < 240; ++t) w.tick();
  JointState s;
  REQUIRE(w.joint(j, &s));
  const PieceState up = *piece_of(w, crate);
  const PieceState cp = *piece_of(w, car);
  MESSAGE("lift: the car at " << s.value << " m (target 2), the crate from z " << p0.pos.z << " to " << up.pos.z << ", x " << p0.pos.x << " -> "
                              << up.pos.x << "; the slider carries " << s.force.z << " N");
  CHECK(s.value == doctest::Approx(2.0).epsilon(0.01));
  CHECK(up.pos.z == doctest::Approx(p0.pos.z + 2.0).epsilon(0.01));
  CHECK(std::abs(up.pos.x - p0.pos.x) < 0.05);
  CHECK(std::abs(up.pos.y - p0.pos.y) < 0.05);
  CHECK(std::abs(cp.vel.z) < 0.02);
  // (the car and the crate, 0.55 t and 0.07 t: the slider holds their weight)
  CHECK(s.force.z == doctest::Approx((16 * 16 * 2 + 64) * h * h * h * 550.0 * 9.81).epsilon(0.02));
  // down 1 m
  REQUIRE(w.set_joint_drive(j, servo(1.0)));
  for (int t = 0; t < 240; ++t) w.tick();
  const PieceState down = *piece_of(w, crate);
  CHECK(down.pos.z == doctest::Approx(p0.pos.z + 1.0).epsilon(0.01));
}

TEST_CASE("machines: a turntable turns what stands on it") {
  World w;
  VoxelGrid g = ground();
  box(g, {-2, -2, 0}, {2, 2, 2}, make_vox(MaterialId::Concrete, false));  // (a pedestal under its centre)
  g.compact();
  w.load(std::move(g));
  w.bake();
  // a timber disc (2 m radius, 0.25 m) a voxel above the pedestal, on a hinge on the pedestal's top
  VoxelGrid d;
  d.h = h;
  for (i32 x = -16; x < 16; ++x)
    for (i32 y = -16; y < 16; ++y)
      if ((x + 0.5) * (x + 0.5) + (y + 0.5) * (y + 0.5) < 16.0 * 16.0) d.fill_column(x, y, 0, 2, make_vox(MaterialId::Wood, false));
  d.compact();
  const GridId disc = w.add_grid(GridFrame{V3{0.5 * h, 0.5 * h, 3.0 * h}, kId}, std::move(d), false);
  JointDesc jd;
  jd.type = JointType::Hinge;
  jd.a = at(kWorldGrid, V3{0.0, 0.0, 1.5 * h});
  jd.b = at(disc, V3{0.0, 0.0, 1.5 * h});
  jd.axis = V3{0, 0, 1};
  jd.drive.kind = JointDrive::Kind::Speed;
  jd.drive.speed = 0.0;  // (a brake, until the crate is on it)
  jd.drive.max = 20000.0;
  const JointId j = w.add_joint(jd);
  REQUIRE(j != 0);
  drop_crate(w, V3{0.75, -0.25, 4.5 * h + 0.3});  // (its centre 1 m out along x)
  for (int t = 0; t < 120; ++t) w.tick();
  REQUIRE(w.pieces().size() == 2);
  const i64 crate = w.pieces().back().id;
  const PieceState p0 = *piece_of(w, crate);
  const f64 a0 = std::atan2(p0.pos.y, p0.pos.x), r0 = std::hypot(p0.pos.x, p0.pos.y);
  const f64 om = 0.5;
  jd.drive.speed = om;
  REQUIRE(w.set_joint_drive(j, jd.drive));
  for (int t = 0; t < 180; ++t) w.tick();  // 3 s: about 1.5 rad (it spins up in a moment)
  JointState s;
  REQUIRE(w.joint(j, &s));
  const PieceState p1 = *piece_of(w, crate);
  const f64 a1 = std::atan2(p1.pos.y, p1.pos.x), r1 = std::hypot(p1.pos.x, p1.pos.y);
  MESSAGE("turntable: turned " << s.value << " rad; the crate from angle " << a0 << " r " << r0 << " to angle " << a1 << " r " << r1
                               << ", speed " << std::hypot(p1.vel.x, p1.vel.y) << " (the table's " << om * r1 << ")");
  CHECK(s.value == doctest::Approx(1.5).epsilon(0.05));
  CHECK(a1 - a0 == doctest::Approx(s.value).epsilon(0.05));
  CHECK(std::abs(r1 - r0) < 0.1);
  CHECK(std::hypot(p1.vel.x, p1.vel.y) == doctest::Approx(om * r1).epsilon(0.1));
  CHECK_FALSE(p1.asleep);
  // the crate turned with it, about its own centre
  CHECK(std::abs(p1.ang.z - om) < 0.1);
}

TEST_CASE("machines: shot away, the structure a machine hangs on lets it fall") {
  SUBCASE("the voxel it holds on to is gone: the joint lets go") {
    World w;
    const JointId j = lift(w, servo(0.0), 16);  // (the car 2 m up)
    REQUIRE(j != 0);
    for (int t = 0; t < 30; ++t) w.tick();
    REQUIRE(w.pieces().size() == 1);
    const f64 z0 = w.pieces().front().pos.z;
    CHECK(z0 > 2.0);
    w.carve(V3{-4.5 * h, 0.0, 16 * h}, 0.4);  // (a hole in the pillar's face there)
    bool broke = false;
    for (int t = 0; t < 90; ++t) {
      w.tick();
      for (const WorldEvent& e : w.take_events()) broke = broke || (e.kind == WorldEvent::Kind::JointBroken && e.id == j);
    }
    JointState s;
    const PieceState* car = nullptr;
    const std::vector<PieceState> ps = w.pieces();
    for (const PieceState& p : ps)
      if (!car || p.pos.z < car->pos.z) car = &p;
    MESSAGE("anchor shot away: the joint " << std::string(broke ? "let go" : "held") << "; the car from z " << z0 << " to " << (car ? car->pos.z : 0.0));
    CHECK(broke);
    CHECK_FALSE(w.joint(j, &s));
    REQUIRE(car != nullptr);
    CHECK(car->pos.z < 0.3);
  }
  SUBCASE("the pillar is cut below it: the car comes down with what it hangs on") {
    World w;
    const JointId j = lift(w, servo(0.0), 24);  // (3 m up)
    REQUIRE(j != 0);
    for (int t = 0; t < 30; ++t) w.tick();
    REQUIRE(w.pieces().size() == 1);
    const i64 car = w.pieces().front().id;
    // a slice through the pillar 1 m up: what is above it falls, the car's anchor with it
    std::vector<VoxelEdit> cut;
    for (i32 x = -12; x < -4; ++x)
      for (i32 y = -4; y < 4; ++y)
        for (i32 z = 8; z < 10; ++z) cut.push_back(VoxelEdit{{x, y, z}, kAir});
    REQUIRE(w.set_voxels(cut) == 128);
    JointState s;
    i64 held_by = 0;
    for (int t = 0; t < 30; ++t) {
      w.tick();
      if (w.joint(j, &s) && s.piece_a != 0) held_by = s.piece_a;
    }
    for (int t = 0; t < 150; ++t) w.tick();
    const std::optional<PieceState> cp = piece_of(w, car);
    MESSAGE("pillar cut: the slider's upper end went with piece " << held_by << "; the car now at z " << (cp ? cp->pos.z : -1.0));
    CHECK(held_by != 0);
    REQUIRE(cp);
    CHECK(cp->pos.z < 2.0);
  }
}

TEST_CASE("machines: over the pieces' budget, a joint's pieces and the host's kept ones are not culled") {
  World w;
  const JointId j = lift(w, servo(0.0));
  REQUIRE(j != 0);
  w.tick();
  const i64 car = w.pieces().front().id;
  for (int k = 0; k < 4; ++k) drop_crate(w, V3{0.8 * (k & 1), -0.9 + 1.0 * (k >> 1), 3.5 * h + 0.3});  // (apart: not bonded)
  for (int t = 0; t < 3; ++t) w.tick();
  std::vector<i64> crates;
  for (const PieceState& p : w.pieces())
    if (p.id != car) crates.push_back(p.id);
  REQUIRE(crates.size() == 4);
  REQUIRE(w.set_piece_keep(crates[2], true));
  WorldConfig c = w.config();
  c.max_bodies = 2;
  w.configure(c);
  for (int t = 0; t < 240; ++t) w.tick();
  std::vector<i64> left;
  for (const PieceState& p : w.pieces()) left.push_back(p.id);
  MESSAGE("over budget (2 pieces): " << left.size() << " pieces left");
  CHECK(left.size() == 2);
  CHECK(std::find(left.begin(), left.end(), car) != left.end());
  CHECK(std::find(left.begin(), left.end(), crates[2]) != left.end());
  CHECK(w.stats().culled_pieces == 3);
}

TEST_CASE("machines: a saved session comes back with its pieces, its machines on their joints and the world's clock") {
  JointDrive osc;
  osc.kind = JointDrive::Kind::Oscillate;
  osc.target = 0.0;
  osc.target2 = 2.0;
  osc.period = 4.0;
  osc.speed = 2.0;
  osc.max = 40000.0;
  World a;
  REQUIRE(lift(a, osc) != 0);
  for (int k = 0; k < 2; ++k) drop_crate(a, V3{0.8 * k, -0.25, 3.5 * h + 0.3});
  for (int t = 0; t < 90; ++t) a.tick();  // (1.5 s: the car on its way up, the crates riding)
  REQUIRE(a.pieces().size() == 3);
  a.take_events();
  const std::vector<u8> delta = a.save_delta();
  // the level made again, with its joint (a host makes a level's joints when it loads it): the
  // saved session's pieces and joints take their places
  World b;
  REQUIRE(lift(b, osc) != 0);
  REQUIRE(b.load_delta(delta));
  i32 added = 0;
  for (const WorldEvent& e : b.take_events()) added += e.kind == WorldEvent::Kind::PieceAdded ? 1 : 0;
  MESSAGE("saved session: " << delta.size() << " bytes; restored " << b.pieces().size() << " pieces, " << b.joints().size() << " joints, clock "
                            << b.time() << " s (was " << a.time() << ")");
  CHECK(added == 3);
  CHECK(b.pieces().size() == 3);
  CHECK(b.joints().size() == 1);
  CHECK(b.time() == a.time());
  CHECK(b.session_hash() == a.session_hash());
  CHECK(b.state_hash() == a.state_hash());
  // and it goes on as it would have (warm starts are not saved: close, not the same bits)
  for (int t = 0; t < 60; ++t) {
    a.tick();
    b.tick();
  }
  JointState sa, sb;
  REQUIRE(a.joint(1, &sa));
  REQUIRE(b.joint(1, &sb));
  f64 worst = 0.0;
  for (const PieceState& p : a.pieces()) {
    const std::optional<PieceState> q = piece_of(b, p.id);
    REQUIRE(q);
    worst = std::max(worst, norm(p.pos - q->pos));
  }
  MESSAGE("a second on: the car at " << sa.value << " / " << sb.value << " m; the pieces at most " << worst << " m apart");
  CHECK(std::abs(sa.value - sb.value) < 0.01);
  CHECK(worst < 0.03);
}

TEST_CASE("machines: a session with machines is bit-identical on any thread count") {
  auto run = [](int threads) {
    set_num_threads(threads);
    World w;
    JointDrive osc;
    osc.kind = JointDrive::Kind::Oscillate;
    osc.target = 0.0;
    osc.target2 = 2.0;
    osc.period = 4.0;
    osc.speed = 2.0;
    osc.max = 40000.0;
    lift(w, osc);
    for (int k = 0; k < 3; ++k) drop_crate(w, V3{0.1 + 0.6 * k, -0.25, 3.5 * h + 0.3 + 0.6 * k});
    for (int t = 0; t < 300; ++t) w.tick();
    return w.session_hash();
  };
  const u64 a = run(1), b = run(4);
  set_num_threads(0);
  MESSAGE("machines session: hash " << a);
  CHECK(a == b);
}
