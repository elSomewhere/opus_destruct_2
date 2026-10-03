// Articulations (docs/MOTION.md §6): links (bodies of no voxels, colliding as spheres) held by
// joints with cones, twist and hinge ranges and muscles, pulled by targets; standing on the
// structures and loading them, knocked by pieces and knocking them, stepped finely on their own.
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/base/rotation.hpp"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

constexpr f64 h = 0.125;
constexpr f64 kPi = 3.141592653589793;
const f64 kTop = -0.5 * h;  // (the ground's surface)

void box(VoxelGrid& g, const IVec3& lo, const IVec3& hi, Vox v) {
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y) g.fill_column(x, y, lo[2], hi[2], v);
}

VoxelGrid ground(i32 half = 48) {
  VoxelGrid g;
  g.h = h;
  box(g, {-half, -half, -4}, {half, half, 0}, make_vox(MaterialId::Rock, true));
  g.compact();
  g.lo = {-half, -half, -4};
  g.hi = {half, half, 64};
  return g;
}

// A rod along z of length len (m), radius r, mass m: two spheres along it.
LinkDesc rod(const V3& pos, f64 len, f64 r, f64 m, const Quat& rot = Quat{}) {
  LinkDesc L;
  L.mass = m;
  const f64 perp = m * (3.0 * r * r + len * len) / 12.0, along = 0.5 * m * r * r;
  L.inertia = V3{perp, perp, along};
  L.pos = pos;
  L.rot = rot;
  L.spheres = {{V3{0, 0, -0.3 * len}, r}, {V3{0, 0, 0.3 * len}, r}};
  L.long_axis = V3{0, 0, 1};
  return L;
}

LinkDesc ball(const V3& pos, f64 r, f64 m) {
  LinkDesc L;
  L.mass = m;
  const f64 I = 0.4 * m * r * r;
  L.inertia = V3{I, I, I};
  L.pos = pos;
  L.spheres = {{V3{}, r}};
  return L;
}

// A chain of n rods hanging down from `top`, each len long, joined by balls (their frames' z along
// the chain, down), the first held to the world at `top` by a ball joint.
struct Chain {
  ArticulationId id = 0;
  JointId hold = 0;
  f64 len = 0.3;
  i32 n = 0;
};

Chain chain(World& w, const V3& top, i32 n, f64 len, f64 m) {
  ArticulationDesc d;
  const Quat down = rotation_of(V3{kPi, 0, 0});  // (z down)
  for (i32 i = 0; i < n; ++i) d.links.push_back(rod(top - V3{0, 0, len * (i + 0.5)}, len, 0.04, m));
  for (i32 i = 1; i < n; ++i) {
    ArticulationJointDesc J;
    J.parent = static_cast<u16>(i - 1);
    J.child = static_cast<u16>(i);
    J.type = JointType::Ball;
    J.anchor_parent = V3{0, 0, -0.5 * len};
    J.anchor_child = V3{0, 0, 0.5 * len};
    J.frame_parent = J.frame_child = down;
    d.joints.push_back(J);
  }
  Chain c;
  c.id = w.add_articulation(d);
  c.len = len;
  c.n = n;
  if (c.id == 0) return c;
  JointDesc hd;
  hd.type = JointType::Ball;
  hd.a.kind = JointAnchor::Kind::World;
  hd.a.point = top;
  hd.b.kind = JointAnchor::Kind::Link;
  hd.b.id = static_cast<u64>(w.link_body(c.id, 0));
  hd.b.point = top;
  c.hold = w.add_joint(hd);
  return c;
}

// The largest gap between neighbouring rods' joint points (m).
f64 chain_gap(const World& w, const Chain& c) {
  ArticulationState s;
  if (!w.articulation_state(c.id, &s)) return 1e9;
  f64 gap = 0.0;
  for (i32 i = 1; i < c.n; ++i) {
    const LinkState& a = s.links[size_t(i - 1)];
    const LinkState& b = s.links[size_t(i)];
    const V3 pa = a.pos + rotate(a.rot, V3{0, 0, -0.5 * c.len});
    const V3 pb = b.pos + rotate(b.rot, V3{0, 0, 0.5 * c.len});
    gap = std::max(gap, norm(pa - pb));
  }
  return gap;
}

f64 energy(const World& w, ArticulationId id, const std::vector<LinkDesc>& links) {
  ArticulationState s;
  w.articulation_state(id, &s);
  f64 e = 0.0;
  for (size_t i = 0; i < s.links.size(); ++i) {
    const LinkState& L = s.links[i];
    const V3 I = links[i].inertia;
    const V3 wl = rotate_inv(L.rot, L.ang);
    e += 0.5 * L.mass * norm2(L.vel) + 0.5 * (I.x * wl.x * wl.x + I.y * wl.y * wl.y + I.z * wl.z * wl.z) + L.mass * 9.81 * L.pos.z;
  }
  return e;
}

}  // namespace

TEST_CASE("articulations: a chain of links hangs from a point, swings, and holds together") {
  for (int substeps : {4, 1}) {
    World w;
    WorldConfig cfg;
    cfg.rigid.link_substeps = substeps;
    w.configure(cfg);
    w.load(ground());
    w.bake();
    const Chain c = chain(w, V3{0, 0, 4.0}, 5, 0.3, 2.0);
    REQUIRE(c.id != 0);
    REQUIRE(c.hold != 0);
    // pushed sideways at its end
    REQUIRE(w.add_link_velocity(c.id, 4, V3{3.0, 0, 0}, V3{}));
    f64 gap = 0.0, lowest = 1e9, reach = 0.0;
    for (int t = 0; t < 240; ++t) {
      w.tick();
      gap = std::max(gap, chain_gap(w, c));
      ArticulationState s;
      REQUIRE(w.articulation_state(c.id, &s));
      lowest = std::min(lowest, s.links.back().pos.z);
      reach = std::max(reach, std::abs(s.links.back().pos.x));
    }
    MESSAGE("chain (link substeps " << substeps << "): largest joint gap " << gap << " m, its end swung out " << reach << " m, lowest " << lowest);
    CHECK(gap < 0.01);
    CHECK(reach > 0.2);
    CHECK(lowest > 4.0 - 1.5 - 0.1);  // (it hangs: it does not stretch)
    // its swing dies away (air, tissue), slowly: a pendulum
    f64 late = 0.0;
    for (int t = 0; t < 1200; ++t) {
      w.tick();
      ArticulationState s;
      REQUIRE(w.articulation_state(c.id, &s));
      if (t >= 1080) late = std::max(late, std::abs(s.links.back().pos.x));
    }
    MESSAGE("... 20 s on, it swings out " << late << " m");
    CHECK(late < 0.8 * reach);
    ArticulationState s;
    REQUIRE(w.articulation_state(c.id, &s));
    CHECK(!s.asleep);  // (its host has not let it sleep)
  }
}

TEST_CASE("articulations: a free swinging chain does not gain energy") {
  World w;
  w.load(ground());
  w.bake();
  // a chain let go level: it swings down; its energy never grows past where it started
  ArticulationDesc d;
  const f64 len = 0.3;
  const Quat side = rotation_of(V3{0, kPi / 2, 0});  // (z along +x)
  for (i32 i = 0; i < 4; ++i) d.links.push_back(rod(V3{len * (i + 0.5), 0, 3.0}, len, 0.04, 1.5, side));
  for (i32 i = 1; i < 4; ++i) {
    ArticulationJointDesc J;
    J.parent = static_cast<u16>(i - 1);
    J.child = static_cast<u16>(i);
    J.anchor_parent = V3{0, 0, 0.5 * len};
    J.anchor_child = V3{0, 0, -0.5 * len};
    d.joints.push_back(J);
  }
  const ArticulationId id = w.add_articulation(d);
  REQUIRE(id != 0);
  JointDesc hd;
  hd.a.kind = JointAnchor::Kind::World;
  hd.a.point = V3{0, 0, 3.0};
  hd.b.kind = JointAnchor::Kind::Link;
  hd.b.id = static_cast<u64>(w.link_body(id, 0));
  hd.b.point = V3{0, 0, 3.0};
  REQUIRE(w.add_joint(hd) != 0);
  const f64 e0 = energy(w, id, d.links);
  f64 emax = e0;
  for (int t = 0; t < 600; ++t) {
    w.tick();
    emax = std::max(emax, energy(w, id, d.links));
  }
  MESSAGE("swinging chain: energy at the start " << e0 << " J, largest " << emax << " J, after 10 s " << energy(w, id, d.links));
  CHECK(emax < e0 + 0.02 * std::abs(e0) + 1.0);
}

TEST_CASE("articulations: a link lands on the ground, rests there, feels it, and sleeps when let") {
  World w;
  w.load(ground());
  w.bake();
  ArticulationDesc d;
  d.links.push_back(ball(V3{0, 0, 1.0}, 0.1, 5.0));
  const ArticulationId id = w.add_articulation(d);
  REQUIRE(id != 0);
  for (int t = 0; t < 120; ++t) w.tick();
  ArticulationState s;
  REQUIRE(w.articulation_state(id, &s));
  MESSAGE("resting ball: z " << s.links[0].pos.z << " (surface " << kTop << " + 0.1), contact " << s.links[0].contact << ", normal z "
                            << s.links[0].contact_normal.z);
  CHECK(s.links[0].pos.z == doctest::Approx(kTop + 0.1).epsilon(0.02));
  CHECK(s.links[0].contact);
  CHECK(s.links[0].contact_normal.z > 0.99);
  CHECK(!s.asleep);
  w.articulation_control(id)->can_sleep = true;
  for (int t = 0; t < 60; ++t) w.tick();
  REQUIRE(w.articulation_state(id, &s));
  CHECK(s.asleep);
  // a push wakes it
  REQUIRE(w.add_link_velocity(id, 0, V3{1.0, 0, 0}, V3{}));
  REQUIRE(w.articulation_state(id, &s));
  CHECK(!s.asleep);
}

TEST_CASE("articulations: one comes down on another asleep - it lands on it; a hard enough blow wakes it") {
  // (the sleeper first along x: its box before the other's in the fine collision's sweep - the
  // pair's link stepped finely is the one that moves, whichever comes first)
  for (const bool hard : {false, true}) {
    CAPTURE(hard);
    World w;
    w.load(ground());
    w.bake();
    // (a heavy sleeper: set down on it, the other's weight is less than half its own - it stays
    // asleep, a support)
    ArticulationDesc d;
    d.links.push_back(ball(V3{0, 0, kTop + 0.1}, 0.1, 50.0));
    const ArticulationId a = w.add_articulation(d);
    REQUIRE(a != 0);
    w.articulation_control(a)->can_sleep = true;
    for (int t = 0; t < 120; ++t) w.tick();
    REQUIRE(w.articulation_asleep(a));
    ArticulationState sa, sb;
    REQUIRE(w.articulation_state(a, &sa));
    const V3 rest = sa.links[0].pos;
    ArticulationDesc e;
    e.links.push_back(ball(V3{0.05, 0, kTop + 0.3 + (hard ? 1.0 : 0.002)}, 0.1, 5.0));
    const ArticulationId b = w.add_articulation(e);
    REQUIRE(b != 0);
    bool woke = false;
    f64 closest = 1e9;
    for (int t = 0; t < 90; ++t) {
      w.tick();
      woke = woke || !w.articulation_asleep(a);
      REQUIRE(w.articulation_state(a, &sa));
      REQUIRE(w.articulation_state(b, &sb));
      closest = std::min(closest, norm(sa.links[0].pos - sb.links[0].pos));
    }
    const std::string how = hard ? "dropped from 1 m" : "set down", then = woke ? "woke" : "slept on";
    MESSAGE(how << ": the sleeper " << then << ", moved " << norm(sa.links[0].pos - rest) << " m; the centres at least " << closest << " m apart");
    CHECK(std::isfinite(sb.links[0].pos.x));
    CHECK(std::isfinite(sb.links[0].pos.z));
    // (never through it: two 0.1 m balls)
    CHECK(closest > 0.18);
    CHECK(sb.links[0].pos.z > kTop + 0.08);
    // (set down on it, a sleeper is a support: it stays put; a body dropped on it wakes it)
    CHECK(woke == hard);
    if (!hard) CHECK(norm(sa.links[0].pos - rest) < 1e-9);
  }
}

TEST_CASE("articulations: a ball's cone and twist and a hinge's range hold") {
  World w;
  w.load(ground());
  w.bake();
  // a kinematic block at 3 m (mass 0: it stays), a rod hanging from it on a ball with a 0.5 rad cone
  // and a +-0.4 rad twist, a second rod below on a hinge that bends one way (0 .. 1.5 rad)
  ArticulationDesc d;
  LinkDesc top = ball(V3{0, 0, 3.0}, 0.1, 0.0);
  d.links.push_back(top);
  const Quat down = rotation_of(V3{kPi, 0, 0});
  d.links.push_back(rod(V3{0, 0, 3.0 - 0.3}, 0.4, 0.04, 2.0));
  d.links.push_back(rod(V3{0, 0, 3.0 - 0.7}, 0.4, 0.04, 1.5));
  ArticulationJointDesc J;
  J.parent = 0;
  J.child = 1;
  J.type = JointType::Ball;
  J.anchor_parent = V3{0, 0, -0.1};
  J.anchor_child = V3{0, 0, 0.2};
  J.frame_parent = J.frame_child = down;
  J.swing_limited = true;
  J.swing[0] = J.swing[1] = J.swing[2] = J.swing[3] = 0.5;
  J.twist_limited = true;
  J.twist_lower = -0.4;
  J.twist_upper = 0.4;
  d.joints.push_back(J);
  ArticulationJointDesc K;
  K.parent = 1;
  K.child = 2;
  K.type = JointType::Hinge;
  K.anchor_parent = V3{0, 0, -0.2};
  K.anchor_child = V3{0, 0, 0.2};
  K.frame_parent = K.frame_child = down;  // (it bends about x)
  K.hinge_limited = true;
  K.hinge_lower = 0.0;
  K.hinge_upper = 1.5;
  d.joints.push_back(K);
  const ArticulationId id = w.add_articulation(d);
  REQUIRE(id != 0);
  f64 swing = 0.0, twist = 0.0, bend_lo = 0.0, bend_hi = 0.0;
  for (int t = 0; t < 360; ++t) {
    ArticulationControl* C = w.articulation_control(id);
    // pushed hard sideways and twisted, then the other way
    const f64 s = t < 180 ? 1.0 : -1.0;
    C->force[1] = V3{400.0 * s, 150.0 * s, 0};
    C->torque[1] = V3{0, 0, 40.0 * s};
    C->force[2] = V3{0, 120.0 * s, 0};
    w.tick();
    ArticulationState st;
    REQUIRE(w.articulation_state(id, &st));
    const V3 z0{0, 0, -1};
    const V3 z1 = rotate(st.links[1].rot, V3{0, 0, -1});
    swing = std::max(swing, std::acos(std::clamp(dot(z0, z1), -1.0, 1.0)));
    // twist: the rod's x about its axis, from the swing's carrying of the block's x
    const Quat rel = conj(down) * st.links[1].rot * down;
    twist = std::max(twist, std::abs(2.0 * std::atan2(rel.z, rel.w)));
    // the hinge: the lower rod's axis turned about the upper's x
    const V3 za = rotate(st.links[1].rot, V3{0, 0, -1}), zb = rotate(st.links[2].rot, V3{0, 0, -1});
    const V3 xa = rotate(st.links[1].rot, V3{1, 0, 0});
    const f64 bend = std::atan2(dot(cross(za, zb), xa), dot(za, zb));
    bend_lo = std::min(bend_lo, bend);
    bend_hi = std::max(bend_hi, bend);
  }
  MESSAGE("cone 0.5 rad: largest swing " << swing << "; twist 0.4: largest " << twist << "; hinge 0..1.5: " << bend_lo << " .. " << bend_hi);
  CHECK(swing < 0.5 + 0.06);
  CHECK(swing > 0.4);
  CHECK(twist < 0.4 + 0.06);
  CHECK(bend_lo > -0.06);
  CHECK(bend_hi < 1.5 + 0.06);
  CHECK(bend_hi > 0.2);
}

TEST_CASE("articulations: a muscle drives its joint to the target and holds it against gravity; a weak one sags") {
  for (f64 strength : {1.0, 0.25}) {
    World w;
    w.load(ground());
    w.bake();
    ArticulationDesc d;
    d.links.push_back(ball(V3{0, 0, 2.0}, 0.1, 0.0));  // (kinematic: a shoulder held in place)
    const Quat down = rotation_of(V3{kPi, 0, 0});
    const f64 len = 0.6, m = 3.0;
    d.links.push_back(rod(V3{0, 0, 2.0 - 0.1 - 0.5 * len}, len, 0.05, m, Quat{}));
    ArticulationJointDesc J;
    J.parent = 0;
    J.child = 1;
    J.anchor_parent = V3{0, 0, -0.1};
    J.anchor_child = V3{0, 0, 0.5 * len};
    J.frame_parent = J.frame_child = down;
    d.joints.push_back(J);
    const ArticulationId id = w.add_articulation(d);
    REQUIRE(id != 0);
    // the arm raised forward, level: its target turned 90 degrees about x
    const Quat target = rotation_of(V3{kPi / 2, 0, 0});
    const f64 I = m * len * len / 3.0;  // (about the joint)
    const f64 hold = m * 9.81 * 0.5 * len;  // (the torque to hold it level)
    ArticulationControl* C = w.articulation_control(id);
    C->muscles[0].target = target;
    C->muscles[0].stiffness = I * 15.0 * 15.0 * 4.0;
    C->muscles[0].damping = 2.0 * I * 15.0;
    C->muscles[0].inertia = I;
    C->muscles[0].max_torque = strength * 2.0 * hold;
    for (int t = 0; t < 180; ++t) w.tick();
    ArticulationState s;
    REQUIRE(w.articulation_state(id, &s));
    const V3 along = rotate(s.links[1].rot, V3{0, 0, -1});  // (from the shoulder towards the hand)
    const f64 elevation = std::asin(std::clamp(along.z, -1.0, 1.0));
    const f64 err = std::acos(std::clamp(dot(rotate(s.links[1].rot, V3{0, 0, 1}), rotate(target, V3{0, 0, 1})), -1.0, 1.0));
    MESSAGE("muscle at " << strength << " x the strength it needs: arm elevation " << elevation << " rad (level: 0), off its target by " << err);
    if (strength >= 1.0) CHECK(err < 0.06);
    else CHECK(elevation < -0.3);  // (too weak: it sags)
  }
}

TEST_CASE("articulations: a target pulls a link's point to a world point, as strong as it is") {
  World w;
  w.load(ground());
  w.bake();
  ArticulationDesc d;
  d.links.push_back(ball(V3{0, 0, 1.0}, 0.1, 10.0));
  ArticulationTargetDesc T;
  T.link = 0;
  T.kind = Target::Kind::Point;
  d.targets.push_back(T);
  const ArticulationId id = w.add_articulation(d);
  REQUIRE(id != 0);
  ArticulationControl* C = w.articulation_control(id);
  TargetDrive& D = C->targets[0];
  D.on = true;
  D.pos = V3{0.5, 0.2, 1.5};
  D.stiffness = 10.0 * 900.0;
  D.damping = 10.0 * 60.0;
  D.max = 10.0 * 9.81 * 3.0;
  for (int t = 0; t < 180; ++t) w.tick();
  ArticulationState s;
  REQUIRE(w.articulation_state(id, &s));
  const f64 sag = 10.0 * 9.81 / D.stiffness;  // (a spring holding a weight)
  MESSAGE("target: link at " << s.links[0].pos.x << ", " << s.links[0].pos.y << ", " << s.links[0].pos.z << "; it pulls " << s.target_applied[0].z
                             << " N up (its weight " << 10.0 * 9.81 << ")");
  CHECK(norm(s.links[0].pos - (D.pos - V3{0, 0, sag})) < 0.02);
  CHECK(s.target_applied[0].z == doctest::Approx(10.0 * 9.81).epsilon(0.05));
  // too weak to hold the weight: it sinks - its spring gives half of it, its damper slows the
  // fall (each within the most) - down to the ground
  w.articulation_control(id)->targets[0].max = 10.0 * 9.81 * 0.5;
  for (int t = 0; t < 180; ++t) w.tick();
  REQUIRE(w.articulation_state(id, &s));
  const f64 z3 = s.links[0].pos.z;
  CHECK(z3 < D.pos.z - sag - 0.15);
  CHECK(s.target_applied[0].z == doctest::Approx(10.0 * 9.81 * 0.5).epsilon(0.05));
  for (int t = 0; t < 1200; ++t) w.tick();
  REQUIRE(w.articulation_state(id, &s));
  CHECK(s.links[0].pos.z < kTop + 0.1 + 0.02);
}

TEST_CASE("articulations: a heavy body standing on a weak beam loads it until it breaks") {
  for (bool heavy : {false, true}) {
    World w;
    VoxelGrid g = ground();
    // a wall, and a timber beam 3 m out from it, 25 cm square, 2 m above the ground
    box(g, {-2, -4, 0}, {0, 4, 24}, make_vox(MaterialId::Concrete, true));
    box(g, {0, -1, 16}, {24, 1, 18}, make_vox(MaterialId::Wood, false));
    g.compact();
    w.load(std::move(g));
    w.bake();
    for (int t = 0; t < 30; ++t) w.tick();
    const i32 pieces0 = static_cast<i32>(w.pieces().size());
    if (heavy) {
      // four heavy links stacked, dropped onto the beam's end
      ArticulationDesc d;
      for (i32 i = 0; i < 4; ++i) d.links.push_back(ball(V3{2.6, 0, 2.6 + 0.3 * i}, 0.14, 150.0));
      for (i32 i = 1; i < 4; ++i) {
        ArticulationJointDesc J;
        J.parent = static_cast<u16>(i - 1);
        J.child = static_cast<u16>(i);
        J.anchor_parent = V3{0, 0, 0.15};
        J.anchor_child = V3{0, 0, -0.15};
        d.joints.push_back(J);
      }
      REQUIRE(w.add_articulation(d) != 0);
    }
    for (int t = 0; t < 240; ++t) w.tick();
    const i32 pieces1 = static_cast<i32>(w.pieces().size());
    MESSAGE("beam " << (heavy ? "with 600 kg on its end" : "alone") << ": pieces " << pieces0 << " -> " << pieces1 << ", bonds broken " << w.stats().bonds_broken);
    if (heavy) CHECK(pieces1 > pieces0);
    else CHECK(pieces1 == pieces0);
  }
}

TEST_CASE("articulations: a piece knocks a body, and a body pushes a piece") {
  World w;
  w.load(ground());
  w.bake();
  // a 150 kg timber block sliding at 6 m/s into a chain hanging at 1 m
  const Chain c = chain(w, V3{0, 0, 2.0}, 3, 0.3, 5.0);
  REQUIRE(c.id != 0);
  VoxelGrid g;
  g.h = h;
  box(g, {0, -2, 0}, {4, 2, 4}, make_vox(MaterialId::Wood, false));
  g.compact();
  const GridId gid = w.add_grid(GridFrame{V3{-3.0, 0.0, 1.15}, Quat{}}, std::move(g), false);
  REQUIRE(gid != 0);
  const i64 block = w.loosen_grid(gid);
  REQUIRE(block != 0);
  w.tick();
  REQUIRE(w.apply_impulse(block, w.piece(block)->x, V3{w.piece(block)->mass * 6.0, 0, w.piece(block)->mass * 2.5}));
  f64 swing = 0.0;
  for (int t = 0; t < 90; ++t) {
    w.tick();
    ArticulationState s;
    REQUIRE(w.articulation_state(c.id, &s));
    swing = std::max(swing, s.links.back().pos.x);
  }
  MESSAGE("a block into a hanging chain: its end knocked out to x " << swing);
  CHECK(swing > 0.3);
  // a heavy ball rolled into a light crate on the ground pushes it along
  World v;
  v.load(ground());
  v.bake();
  VoxelGrid cg;
  cg.h = h;
  box(cg, {0, -2, 0}, {4, 2, 3}, make_vox(MaterialId::Wood, false));
  cg.compact();
  const GridId crate = v.add_grid(GridFrame{V3{1.0, 0.0, 0.1}, Quat{}}, std::move(cg), false);
  REQUIRE(crate != 0);
  const i64 cp = v.loosen_grid(crate);
  REQUIRE(cp != 0);
  for (int t = 0; t < 30; ++t) v.tick();  // (it settles on the ground)
  ArticulationDesc d;
  d.links.push_back(ball(V3{0.0, 0.0, kTop + 0.2}, 0.2, 60.0));
  d.links[0].vel = V3{4.0, 0, 0};
  const ArticulationId bid = v.add_articulation(d);
  REQUIRE(bid != 0);
  const f64 x0 = v.piece(cp)->x.x;
  for (int t = 0; t < 60; ++t) v.tick();
  MESSAGE("a 60 kg ball at 4 m/s into a crate: the crate moved " << v.piece(cp)->x.x - x0 << " m");
  CHECK(v.piece(cp)->x.x - x0 > 0.05);  // (inelastic, then sliding on the ground: about 0.1 - 0.2 m)
}

namespace {

// (a piece at rest on the ground at (x, y) and a slack rope from `link_body` to it: a joint between
// a link and a piece puts the articulation with the pieces; the rope itself never pulls)
void tie_to_piece(World& w, i64 link_body, const V3& at, f64 x, f64 y) {
  VoxelGrid g;
  g.h = h;
  box(g, {0, 0, 0}, {3, 3, 3}, make_vox(MaterialId::Concrete, false));
  g.compact();
  const GridId gid = w.add_grid(GridFrame{V3{x, y, 0.0}, Quat{}}, std::move(g), false);
  REQUIRE(gid != 0);
  const i64 p = w.loosen_grid(gid);
  REQUIRE(p != 0);
  w.tick();  // (announced: a joint may hold it)
  JointDesc r;
  r.type = JointType::Distance;
  r.rope = true;
  r.length = 20.0;
  r.a.kind = JointAnchor::Kind::Link;
  r.a.id = static_cast<u64>(link_body);
  r.a.point = at;
  r.b.kind = JointAnchor::Kind::Piece;
  r.b.id = static_cast<u64>(p);
  r.b.point = w.piece(p)->x;
  REQUIRE(w.add_joint(r) != 0);
}

}  // namespace

TEST_CASE("articulations: one solved with the pieces has their substep - or, where quality asks, the tick at the fine steps' rate") {
  // An arm raised level by its muscle from a held shoulder: on its own (its fine steps), and tied to
  // a piece (solved with the pieces: their substep - its muscle damped more than in the steps its
  // characters were tuned in), and so with rigid.mixed_substeps 8 (the tick at 1/480 s: as on its own).
  struct Run {
    f64 overshoot = -1.0, err = 0.0;
    int ticks = 0, at_mixed = 0, at_two = 0;
    u64 hash = 0;
  };
  auto run = [](bool tied, int mixed, int threads) {
    set_num_threads(threads);
    World w;
    WorldConfig cfg;
    cfg.rigid.mixed_substeps = mixed;
    w.configure(cfg);
    w.load(ground());
    w.bake();
    ArticulationDesc d;
    d.links.push_back(ball(V3{0, 0, 2.0}, 0.1, 0.0));  // (kinematic: a shoulder held in place)
    const Quat down = rotation_of(V3{kPi, 0, 0});
    const f64 len = 0.6, m = 3.0;
    d.links.push_back(rod(V3{0, 0, 2.0 - 0.1 - 0.5 * len}, len, 0.05, m, Quat{}));
    ArticulationJointDesc J;
    J.parent = 0;
    J.child = 1;
    J.anchor_parent = V3{0, 0, -0.1};
    J.anchor_child = V3{0, 0, 0.5 * len};
    J.frame_parent = J.frame_child = down;
    d.joints.push_back(J);
    const ArticulationId id = w.add_articulation(d);
    REQUIRE(id != 0);
    if (tied) tie_to_piece(w, w.link_body(id, 0), V3{0, 0, 2.0}, 2.0, 2.0);
    const Quat target = rotation_of(V3{kPi / 2, 0, 0});
    const f64 I = m * len * len / 3.0;
    ArticulationControl* C = w.articulation_control(id);
    C->muscles[0].target = target;
    C->muscles[0].stiffness = I * 15.0 * 15.0 * 4.0;
    C->muscles[0].damping = 2.0 * I * 15.0;
    C->muscles[0].inertia = I;
    C->muscles[0].max_torque = 2.0 * m * 9.81 * 0.5 * len;
    Run r;
    for (int t = 0; t < 180; ++t) {
      w.tick();
      ++r.ticks;
      if (w.stats().substeps == mixed) ++r.at_mixed;
      if (w.stats().substeps == 2) ++r.at_two;
      ArticulationState s;
      REQUIRE(w.articulation_state(id, &s));
      const V3 along = rotate(s.links[1].rot, V3{0, 0, -1});
      r.overshoot = std::max(r.overshoot, std::asin(std::clamp(along.z, -1.0, 1.0)));
      r.err = std::acos(std::clamp(dot(rotate(s.links[1].rot, V3{0, 0, 1}), rotate(target, V3{0, 0, 1})), -1.0, 1.0));
    }
    r.hash = w.session_hash();
    return r;
  };
  const Run alone = run(false, 0, 0), coarse = run(true, 0, 0), fine = run(true, 8, 0), alone8 = run(false, 8, 0);
  MESSAGE("an arm raised by its muscle overshoots level by " << alone.overshoot << " rad on its own, " << coarse.overshoot << " solved with the pieces, "
                                                             << fine.overshoot << " so in 8 substeps a tick");
  CHECK(coarse.at_two == coarse.ticks);  // (the pieces' substeps)
  CHECK(fine.at_mixed == fine.ticks);    // (8: tied, it is always with the pieces)
  CHECK(alone8.at_two == alone8.ticks);  // (on its own: the knob changes nothing)
  CHECK(alone8.hash == alone.hash);
  CHECK(std::abs(coarse.overshoot - alone.overshoot) > 0.05);   // (the pieces' substep: damped more)
  CHECK(std::abs(fine.overshoot - alone.overshoot) < 0.005);    // (the fine steps' rate: as on its own)
  CHECK(fine.err < 0.06);
  CHECK(coarse.err < 0.06);
  CHECK(run(true, 8, 1).hash == run(true, 8, 4).hash);  // (bit-identical on any thread count)
  set_num_threads(0);
}

TEST_CASE("articulations: a chain of light links holds a heavy one - its joints carry it from each fine step to the next") {
  // Four 1 kg rods and a 30 kg weight at their end, hanging from the world: at rigid.link_warm 1
  // (its joints' point rows start each fine step from all of the last one's impulses) it hangs
  // with its joints closed; at 0.9 - a tenth lost every fine step, 16 a tick, and two velocity
  // passes to find it again - they open far.
  auto hang = [](f64 warm) {
    World w;
    WorldConfig cfg;
    cfg.rigid.link_warm = warm;
    w.configure(cfg);
    w.load(ground());
    w.bake();
    const f64 len = 0.3, top = 3.0;
    ArticulationDesc d;
    const Quat down = rotation_of(V3{kPi, 0, 0});
    for (i32 i = 0; i < 4; ++i) d.links.push_back(rod(V3{0, 0, top - len * (i + 0.5)}, len, 0.04, i < 3 ? 1.0 : 30.0));
    for (i32 i = 1; i < 4; ++i) {
      ArticulationJointDesc J;
      J.parent = static_cast<u16>(i - 1);
      J.child = static_cast<u16>(i);
      J.anchor_parent = V3{0, 0, -0.5 * len};
      J.anchor_child = V3{0, 0, 0.5 * len};
      J.frame_parent = J.frame_child = down;
      d.joints.push_back(J);
    }
    Chain c;
    c.id = w.add_articulation(d);
    c.len = len;
    c.n = 4;
    REQUIRE(c.id != 0);
    JointDesc hd;
    hd.a.kind = JointAnchor::Kind::World;
    hd.a.point = V3{0, 0, top};
    hd.b.kind = JointAnchor::Kind::Link;
    hd.b.id = static_cast<u64>(w.link_body(c.id, 0));
    hd.b.point = V3{0, 0, top};
    REQUIRE(w.add_joint(hd) != 0);
    for (int t = 0; t < 120; ++t) w.tick();
    f64 gap = 0.0;
    for (int t = 0; t < 60; ++t) {
      w.tick();
      gap = std::max(gap, chain_gap(w, c));
    }
    // knocked: the weight thrown sideways at 4 m/s
    REQUIRE(w.add_link_velocity(c.id, 3, V3{4.0, 0, 0}, V3{}));
    f64 swing = 0.0;
    for (int t = 0; t < 120; ++t) {
      w.tick();
      swing = std::max(swing, chain_gap(w, c));
    }
    return std::pair<f64, f64>{gap, swing};
  };
  const auto [gap1, swing1] = hang(1.0);
  const auto [gap09, swing09] = hang(0.9);
  MESSAGE("30 kg on four 1 kg rods: joints open " << gap1 << " m at rest, " << swing1 << " m swinging (link_warm 1); " << gap09 << " / " << swing09
                                                  << " m at 0.9");
  CHECK(gap1 < 0.003);
  CHECK(swing1 < 0.02);
  CHECK(gap09 > 0.05);
}

TEST_CASE("articulations: its own joints are in its state, not the host's joints; a host's joint to a link is the host's") {
  World w;
  w.load(ground());
  w.bake();
  const Chain c = chain(w, V3{0, 0, 3.0}, 4, 0.3, 2.0);
  REQUIRE(c.id != 0);
  REQUIRE(c.hold != 0);
  // (the three between its links are its own: a front end that draws the host's joints does not
  // draw a body's)
  CHECK(w.joints() == std::vector<JointId>{c.hold});
  for (int t = 0; t < 30; ++t) w.tick();
  CHECK(w.joints() == std::vector<JointId>{c.hold});
  JointState js;
  CHECK(w.joint(c.hold, &js));
  REQUIRE(w.remove_articulation(c.id));
  for (int t = 0; t < 2; ++t) w.tick();
  CHECK(w.joints().empty());  // (the host's joint lost its end with it)
}

TEST_CASE("articulations: a link is not a piece - remove_piece refuses it, joined_pieces leaves it out") {
  World w;
  w.load(ground());
  w.bake();
  const Chain c = chain(w, V3{0, 0, 3.0}, 3, 0.3, 2.0);
  REQUIRE(c.id != 0);
  const i64 l1 = w.link_body(c.id, 1);
  REQUIRE(l1 != 0);
  CHECK_FALSE(w.remove_piece(l1));  // (it goes with its articulation: remove_articulation)
  CHECK(w.joined_pieces(w.link_body(c.id, 0)).empty());
  for (int t = 0; t < 30; ++t) w.tick();
  ArticulationState s;
  REQUIRE(w.articulation_state(c.id, &s));
  CHECK(s.links.size() == 3);
  CHECK(chain_gap(w, c) < 0.02);  // (whole: its joints hold)
  CHECK(w.load_delta(w.save_delta()));  // (and its record is whole)
}

TEST_CASE("articulations: removed, it is gone; a lost link touches nothing; no piece events") {
  World w;
  w.load(ground());
  w.bake();
  w.take_events();
  ArticulationDesc d;
  d.links.push_back(ball(V3{0, 0, 0.5}, 0.1, 5.0));
  d.links.push_back(ball(V3{1, 0, 0.5}, 0.1, 5.0));
  const ArticulationId id = w.add_articulation(d);
  REQUIRE(id != 0);
  REQUIRE(w.lose_link(id, 1, 0.05));
  for (int t = 0; t < 60; ++t) w.tick();
  ArticulationState s;
  REQUIRE(w.articulation_state(id, &s));
  CHECK(s.links[0].pos.z > 0.0);   // (on the ground)
  CHECK(s.links[1].pos.z < -0.5);  // (through it)
  CHECK(s.links[1].gone);
  for (const WorldEvent& e : w.take_events()) CHECK(e.kind != WorldEvent::Kind::PieceAdded);
  CHECK(w.pieces().empty());
  REQUIRE(w.remove_articulation(id));
  CHECK(w.articulations().empty());
  bool removed = false;
  for (const WorldEvent& e : w.take_events()) removed = removed || (e.kind == WorldEvent::Kind::ArticulationRemoved && e.id == id);
  CHECK(removed);
}

TEST_CASE("articulations: a session with articulations and pieces is bit-identical on 1 and 4 threads") {
  auto run = [](int threads) {
    set_num_threads(threads);
    World w;
    w.load(ground());
    w.bake();
    const Chain c = chain(w, V3{0, 0, 2.5}, 4, 0.3, 3.0);
    REQUIRE(c.id != 0);
    REQUIRE(w.add_link_velocity(c.id, 3, V3{2.0, 1.0, 0}, V3{}));
    for (i32 i = 0; i < 3; ++i) {
      ArticulationDesc d;
      d.links.push_back(ball(V3{0.3 * i - 0.3, 0.8, 1.0 + 0.4 * i}, 0.12, 8.0));
      REQUIRE(w.add_articulation(d) != 0);
    }
    VoxelGrid g;
    g.h = h;
    box(g, {0, 0, 0}, {3, 3, 3}, make_vox(MaterialId::Concrete, false));
    g.compact();
    const GridId gid = w.add_grid(GridFrame{V3{0.0, 0.7, 2.5}, Quat{}}, std::move(g), false);
    REQUIRE(gid != 0);
    for (int t = 0; t < 180; ++t) w.tick();
    return w.session_hash();
  };
  const u64 a = run(1), b = run(4);
  set_num_threads(0);
  CHECK(a == b);
}

namespace {

// Two rods joined by a hinge, lying on the ground: a body at rest, with its host's data.
ArticulationDesc two_rods(const V3& at) {
  ArticulationDesc d;
  const Quat flat = rotation_of(V3{kPi / 2.0, 0, 0});  // (along y)
  d.links.push_back(rod(at, 0.6, 0.06, 6.0, flat));
  d.links.push_back(rod(at + V3{0, 0.62, 0}, 0.6, 0.06, 6.0, flat));
  ArticulationJointDesc J;
  J.parent = 0;
  J.child = 1;
  J.type = JointType::Hinge;
  J.anchor_parent = V3{0, 0, 0.31};
  J.anchor_child = V3{0, 0, -0.31};
  J.hinge_limited = true;
  J.hinge_lower = -1.0;
  J.hinge_upper = 1.0;
  d.joints.push_back(J);
  d.group = 7;
  d.tag = 42;
  d.data = {1, 2, 3, 4, 5};
  return d;
}

// A flat streamed world of rock, 64 x 64 chunks.
class RockSource final : public ChunkSource {
 public:
  bool generate(const IVec3& c, std::vector<Vox>& out) const override {
    out.assign(kChunkVox, kAir);
    if (c[2] != -1) return false;
    std::fill(out.begin(), out.end(), make_vox(MaterialId::Rock, true));
    return true;
  }
  IVec3 chunk_lo() const override { return {-32, -32, -1}; }
  IVec3 chunk_hi() const override { return {32, 32, 2}; }
};

}  // namespace

TEST_CASE("articulations: a saved session brings them back as they were - their ids, host data, drives, sleep") {
  World w;
  w.load(ground());
  const ArticulationId id = w.add_articulation(two_rods(V3{0.5, 0.0, 0.3}));
  REQUIRE(id != 0);
  w.articulation_control(id)->can_sleep = true;
  w.articulation_control(id)->muscles[0].damping = 3.0;
  for (int t = 0; t < 300; ++t) w.tick();
  ArticulationState sa;
  REQUIRE(w.articulation_state(id, &sa));
  CHECK(sa.asleep);
  const std::vector<u8> delta = w.save_delta();
  World b;
  b.load(ground());
  REQUIRE(b.load_delta(delta));
  REQUIRE(b.articulations().size() == 1);
  CHECK(b.articulations()[0] == id);
  bool added = false;
  for (const WorldEvent& e : b.take_events()) added = added || (e.kind == WorldEvent::Kind::ArticulationAdded && e.id == id);
  CHECK(added);
  ArticulationState sb;
  REQUIRE(b.articulation_state(id, &sb));
  CHECK(sb.asleep);
  CHECK(sb.group == 7);
  CHECK(sb.tag == 42);
  REQUIRE(b.articulation_data(id));
  CHECK(*b.articulation_data(id) == std::vector<u8>{1, 2, 3, 4, 5});
  CHECK(b.articulation_control(id)->muscles[0].damping == 3.0);
  CHECK(b.articulation_control(id)->can_sleep);
  for (size_t i = 0; i < 2; ++i) {
    CHECK(norm(sb.links[i].pos - sa.links[i].pos) == 0.0);
    CHECK(sb.links[i].mass == sa.links[i].mass);
  }
  // (and it goes on from there: woken, it is a body again)
  REQUIRE(b.add_link_velocity(id, 1, V3{0, 0, 2.0}, V3{}));
  for (int t = 0; t < 10; ++t) b.tick();
  REQUIRE(b.articulation_state(id, &sb));
  CHECK(!sb.asleep);
  CHECK(sb.links[1].pos.z > sa.links[1].pos.z + 0.05);
}

TEST_CASE("articulations: one at rest goes out of range with its region, and comes back as it was") {
  World w;
  VoxelGrid g;
  g.h = h;
  w.load(std::move(g));
  StreamConfig sc;
  sc.load_radius = 24.0;
  sc.evict_radius = 32.0;
  sc.chunks_per_tick = 400;
  w.enable_streaming(std::make_shared<RockSource>(), sc);
  w.set_focus(V3{0, 0, 0});
  for (int t = 0; t < 5; ++t) w.tick();
  const ArticulationId id = w.add_articulation(two_rods(V3{2.0, 2.0, 0.3}));
  REQUIRE(id != 0);
  w.articulation_control(id)->can_sleep = true;
  for (int t = 0; t < 300; ++t) w.tick();
  ArticulationState s0;
  REQUIRE(w.articulation_state(id, &s0));
  REQUIRE(s0.asleep);
  w.take_events();
  w.set_focus(V3{120, 0, 0});
  for (int t = 0; t < 30; ++t) w.tick();
  CHECK(w.articulations().empty());
  CHECK(w.stats().archived_articulations == 1);
  bool unloaded = false;
  for (const WorldEvent& e : w.take_events()) unloaded = unloaded || (e.kind == WorldEvent::Kind::ArticulationRemoved && e.id == id && e.end == PieceEnd::Unloaded);
  CHECK(unloaded);
  w.set_focus(V3{0, 0, 0});
  for (int t = 0; t < 30; ++t) w.tick();
  REQUIRE(w.articulations().size() == 1);
  CHECK(w.articulations()[0] == id);
  CHECK(w.stats().archived_articulations == 0);
  ArticulationState s1;
  REQUIRE(w.articulation_state(id, &s1));
  CHECK(s1.tag == 42);
  for (size_t i = 0; i < 2; ++i) CHECK(norm(s1.links[i].pos - s0.links[i].pos) < 1e-9);
  // (a saved session keeps one archived out of range, too)
  w.set_focus(V3{120, 0, 0});
  for (int t = 0; t < 30; ++t) w.tick();
  REQUIRE(w.articulations().empty());
  const std::vector<u8> delta = w.save_delta();
  World b;
  VoxelGrid g2;
  g2.h = h;
  b.load(std::move(g2));
  b.enable_streaming(std::make_shared<RockSource>(), sc);
  b.set_focus(V3{120, 0, 0});
  for (int t = 0; t < 5; ++t) b.tick();
  REQUIRE(b.load_delta(delta));
  CHECK(b.stats().archived_articulations == 1);
  b.set_focus(V3{0, 0, 0});
  for (int t = 0; t < 30; ++t) b.tick();
  REQUIRE(b.articulations().size() == 1);
  ArticulationState s2;
  REQUIRE(b.articulation_state(id, &s2));
  for (size_t i = 0; i < 2; ++i) CHECK(norm(s2.links[i].pos - s0.links[i].pos) < 1e-9);
}

TEST_CASE("articulations: moving a point attachment changes the contact and survives a saved session") {
  World w;
  w.load(ground());
  ArticulationDesc d;
  d.links.push_back(ball(V3{0, 0, 1.5}, 0.1, 1.0));
  ArticulationTargetDesc point;
  point.kind = Target::Kind::Point;
  d.targets.push_back(point);
  ArticulationTargetDesc rotation;
  rotation.kind = Target::Kind::Rotation;
  d.targets.push_back(rotation);
  const auto id = w.add_articulation(d);
  REQUIRE(id != 0);
  auto* control = w.articulation_control(id);
  REQUIRE(control);
  for (auto& target : control->targets) {
    target.on = true;
    target.stiffness = 10000;
    target.damping = 200;
  }
  control->targets[0].pos = V3{0, 0, 1.5};
  for (int n = 0; n < 60; ++n) w.tick();
  const V3 offset{0.2, 0, 0};
  control->target_local = {offset};
  for (int n = 0; n < 120; ++n) w.tick();
  ArticulationState state;
  REQUIRE(w.articulation_state(id, &state));
  const auto& link = state.links[0];
  CHECK(norm(link.pos + rotate(link.rot, offset) - control->targets[0].pos) < 0.01);
  CHECK(link.pos.x < -0.18);
  // Invalid per-tick input leaves the most recent valid attachment in place.
  control->target_local[0].x = std::numeric_limits<f64>::quiet_NaN();
  for (int n = 0; n < 10; ++n) w.tick();
  World restored;
  restored.load(ground());
  REQUIRE(restored.load_delta(w.save_delta()));
  auto* loaded = restored.articulation_control(id);
  REQUIRE(loaded);
  REQUIRE(loaded->target_local.size() == 2);
  CHECK(norm(loaded->target_local[0] - offset) < 1e-12);
  for (int n = 0; n < 60; ++n) restored.tick();
  REQUIRE(restored.articulation_state(id, &state));
  CHECK(norm(state.links[0].pos + rotate(state.links[0].rot, offset) - loaded->targets[0].pos) < 0.01);
}

TEST_CASE("articulations: a relative point target shares momentum and survives saving") {
  World w;
  w.load(ground());
  ArticulationDesc d;
  d.links = {ball({-.2, 0, 4}, .05, 2), ball({.2, 0, 4}, .05, 1)};
  d.targets.push_back({1, Target::Kind::Point, {}});
  const auto id = w.add_articulation(d);
  REQUIRE(id != 0);
  auto* control = w.articulation_control(id);
  control->keep_linear = control->keep_angular = 1;
  control->targets[0].on = true;
  control->targets[0].stiffness = 10000;
  control->targets[0].damping = 150;
  control->targets[0].max = 800;
  control->target_reference = {0};
  control->target_reference_local = {{.4, 0, 0}};
  w.add_link_velocity(id, 1, {2, 0, 0}, {});
  for (int i = 0; i < 20; ++i) w.tick();
  ArticulationState state;
  REQUIRE(w.articulation_state(id, &state));
  CHECK(state.links[0].vel.x > .5);
  CHECK(std::abs(2 * state.links[0].vel.x + state.links[1].vel.x - 2) < 1e-6);
  CHECK(norm(state.links[1].pos - state.links[0].pos - rotate(state.links[0].rot, V3{.4, 0, 0})) < .02);
  World restored;
  restored.load(ground());
  REQUIRE(restored.load_delta(w.save_delta()));
  const auto* loaded = restored.articulation_control(id);
  REQUIRE(loaded);
  REQUIRE(loaded->target_reference.size() == 1);
  CHECK(loaded->target_reference[0] == 0);
  CHECK(norm(loaded->target_reference_local[0] - V3{.4, 0, 0}) < 1e-12);
}

TEST_CASE("articulations: a link's mass changes only with valid values, acts at once and is saved") {
  World w;
  w.load(ground());
  ArticulationDesc d;
  d.links = {ball({-1, 0, 4}, .05, 1), ball({1, 0, 4}, .05, 1)};
  d.links[1].mass = 0;  // kinematic
  const auto id = w.add_articulation(d);
  REQUIRE(id != 0);
  const V3 inertia{.02, .03, .04};
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  CHECK_FALSE(w.set_link_mass(id + 1, 0, 4, inertia));
  CHECK_FALSE(w.set_link_mass(id, 2, 4, inertia));
  CHECK_FALSE(w.set_link_mass(id, 1, 4, inertia));  // a kinematic link has no mass to change
  CHECK_FALSE(w.set_link_mass(id, 0, 0, inertia));
  CHECK_FALSE(w.set_link_mass(id, 0, -1, inertia));
  CHECK_FALSE(w.set_link_mass(id, 0, nan, inertia));
  CHECK_FALSE(w.set_link_mass(id, 0, 4, V3{.02, 0, .04}));
  CHECK_FALSE(w.set_link_mass(id, 0, 4, V3{.02, nan, .04}));
  ArticulationState s;
  REQUIRE(w.articulation_state(id, &s));
  CHECK(s.links[0].mass == 1.0);
  REQUIRE(w.set_link_mass(id, 0, 4, inertia));
  REQUIRE(w.articulation_state(id, &s));
  CHECK(s.links[0].mass == 4.0);
  World restored;
  restored.load(ground());
  REQUIRE(restored.load_delta(w.save_delta()));
  // the same off-centre impulse gives the same velocities in both: mass and inertia were kept
  for (World* world : {&w, &restored}) {
    REQUIRE(world->articulation_state(id, &s));
    const V3 at = s.links[0].pos + V3{0, .05, 0};
    REQUIRE(world->apply_link_impulse(id, 0, at, V3{8, 0, 0}));
    ArticulationState after;
    REQUIRE(world->articulation_state(id, &after));
    CHECK(after.links[0].mass == 4.0);
    CHECK(std::abs(after.links[0].vel.x - s.links[0].vel.x - 2) < 1e-9);       // J / m
    CHECK(std::abs(after.links[0].ang.z - s.links[0].ang.z + .4 / .04) < 1e-9);  // (r x J) / I
  }
}

#include "fixtures/articulation_v1_session.inc"

TEST_CASE("articulations: a session saved with articulation records v1 still loads and steps as it did") {
  World w;
  w.load(ground());
  const std::vector<u8> bytes(std::begin(kArticulationV1Session), std::end(kArticulationV1Session));
  REQUIRE(w.load_delta(bytes));
  REQUIRE(w.articulations().size() == 1);
  const ArticulationId id = w.articulations()[0];
  const auto* control = w.articulation_control(id);
  REQUIRE(control);
  REQUIRE(control->targets.size() == 1);
  CHECK(control->targets[0].on);
  REQUIRE(control->target_local.size() == 1);
  CHECK(norm(control->target_local[0] - V3{.01, 0, 0}) < 1e-12);
  // v1 knew no second link for a point target: the target stays a world target
  REQUIRE(control->target_reference.size() == 1);
  CHECK(control->target_reference[0] == -1);
  for (int i = 0; i < 30; ++i) w.tick();
  ArticulationState s;
  REQUIRE(w.articulation_state(id, &s));
  for (int i = 0; i < 2; ++i)
    CHECK(norm(s.links[i].pos - V3{kArticulationV1After[i][0], kArticulationV1After[i][1], kArticulationV1After[i][2]}) < 1e-12);
}
