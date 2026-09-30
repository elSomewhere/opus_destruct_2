// svx_anim bodies: the rigid system (XPBD) on its own, and the humanoid body made of it - stepped
// on its own (the shallow path) and as a core articulation in a World (the deep path).
#include <cmath>

#include "doctest.h"
#include "svx/anim/body/humanoid.hpp"
#include "svx/anim/physics/core_binding.hpp"
#include "svx/anim/physics/world_collision.hpp"
#include "svx/world/world.hpp"

using namespace svx;
using namespace svx::anim;

namespace {

constexpr f64 DT = 1.0 / 60.0;

struct Pendulum {
  std::unique_ptr<RigidSystem> sys;
  RigidBody* top = nullptr;
  RigidBody* bob = nullptr;
  anim::Joint* j = nullptr;
};

// Two unit boxes joined at a point: a pendulum hanging from a fixed body.
Pendulum pendulum(JointKind kind, std::optional<std::pair<f64, f64>> hinge = std::nullopt) {
  Pendulum p;
  p.sys = std::make_unique<RigidSystem>(nullptr);
  const V3 I{0.1, 0.1, 0.1};
  p.top = p.sys->add(std::make_unique<RigidBody>(0.0, V3{}, V3{0, 0, 1}, Quat{}));
  p.bob = p.sys->add(std::make_unique<RigidBody>(2.0, I, V3{0, 0, 0.5}, Quat{}));
  JointOptions o;
  o.kind = kind;
  o.anchor_a = V3{};
  o.anchor_b = V3{0, 0, 0.5};
  o.hinge = hinge;
  p.j = p.sys->add_joint(std::make_unique<anim::Joint>(p.top, p.bob, o));
  return p;
}

// The core's ground: rock below z = 0 (its top at -h/2).
constexpr f64 kH = 0.125;
constexpr f64 kTop = -0.5 * kH;

VoxelGrid ground(i32 half = 32) {
  VoxelGrid g;
  g.h = kH;
  for (i32 x = -half; x < half; ++x)
    for (i32 y = -half; y < half; ++y) g.fill_column(x, y, -4, 0, make_vox(MaterialId::Rock, true));
  g.compact();
  g.lo = {-half, -half, -4};
  g.hi = {half, half, 64};
  return g;
}

struct Humanoid {
  SkeletonPtr sk;
  std::unique_ptr<Pose> pose;
  std::unique_ptr<WorldPose> world;
  std::unique_ptr<HumanoidBody> body;
};

Humanoid humanoid(const CollisionWorld* collision, f64 z) {
  Humanoid h;
  h.sk = humanoid_skeleton();
  h.pose = std::make_unique<Pose>(h.sk);
  h.world = std::make_unique<WorldPose>(h.sk);
  h.world->compute(*h.pose, V3{0, 0, z}, qz(0.0));
  h.body = std::make_unique<HumanoidBody>(h.sk, collision);
  return h;
}

// Holds a standing pose: muscles, the legs' support, the feet pinned.
void hold_standing(Humanoid& h) {
  HumanoidBody& body = *h.body;
  const WorldPose& world = *h.world;
  body.set_from_pose(world, nullptr, 0.0);
  body.track(world, *h.pose);
  body.apply_tone();
  const f64 m = body.total_mass;
  body.support->enabled = true;
  body.support->stiffness = m * 900;
  body.support->max_force = m * 25;
  body.support->damping = m * 55;
  body.support->target.z = world.p[H::pelvis].z - 0.02;
  body.steer->enabled = true;
  body.steer->stiffness = m * 700;
  body.steer->max_force = m * 10;
  body.steer->damping = m * 50;
  body.upright->enabled = true;
  body.upright->stiffness = 5000;
  body.upright->max_torque = 900;
  body.upright->damping = 450;
  for (i32 i = 0; i < 2; ++i) {
    Attachment& a = *body.feet[size_t(i)];
    a.enabled = true;
    a.target = world.p[size_t(i == 0 ? H::footL : H::footR)];
    body.feet_turn[size_t(i)]->enabled = true;
  }
}

void check_standing(const Humanoid& h) {
  WorldPose out(h.sk);
  h.body->write_pose(out);
  const WorldPose& world = *h.world;
  CHECK(vdist(out.p[H::head], world.p[H::head]) < 0.06);
  // the hands hang where the pose has them (gravity compensation)
  for (i32 hb : {H::handL, H::handR}) CHECK(vdist(out.p[size_t(hb)], world.p[size_t(hb)]) < 0.06);
  const V3 up = rotate(out.q[H::chest], V3{0, 0, 1});
  CHECK(up.z > 0.98);
}

}  // namespace

TEST_CASE("anim rigid: a joint holds its anchors together while the body swings, and energy does not grow") {
  Pendulum p = pendulum(JointKind::Ball);
  p.bob->v.x = 3.0;
  f64 worst = 0.0, max_z = -1e300;
  for (i32 i = 0; i < 600; ++i) {
    p.sys->step(DT);
    worst = std::max(worst, vdist(p.bob->point(V3{0, 0, 0.5}), V3{0, 0, 1}));
    if (i > 60) max_z = std::max(max_z, p.bob->x.z);
  }
  CHECK(worst < 0.002);
  // it started at the bottom with 3 m/s: it can never swing higher than v^2 / 2g above it
  CHECK(max_z < 0.5 + 9.0 / (2.0 * 9.81) + 0.02);
}

TEST_CASE("anim rigid: a hinge bends one way within its range; a drive holds a target against gravity") {
  Pendulum p = pendulum(JointKind::Hinge, std::make_pair(-0.3, 1.2));
  p.bob->v.y = -4.0;  // push it round the wrong way (about +x, backwards)
  f64 low = 0.0;
  for (i32 i = 0; i < 240; ++i) {
    p.sys->step(DT);
    const V3 d{p.bob->x.x, p.bob->x.y, p.bob->x.z - 1.0};
    // angle of the bob about +x from hanging straight down
    low = std::min(low, std::atan2(d.y, -d.z));
  }
  CHECK(low > -0.36);
  // a drive: hold the bob 60 degrees forward
  p.j->target = qaxis(V3{1, 0, 0}, 1.05);
  p.j->stiffness = 400;
  p.j->damping = 20;
  for (i32 i = 0; i < 240; ++i) p.sys->step(DT);
  const V3 d{p.bob->x.x, p.bob->x.y, p.bob->x.z - 1.0};
  CHECK(std::abs(std::atan2(d.y, -d.z) - 1.05) < 0.08);
}

TEST_CASE("anim body: segments weigh what bodies weigh, and a pose goes into the bodies and back unchanged") {
  FlatGround flat(0.0);
  Humanoid h = humanoid(&flat, 0.0);
  HumanoidBody& body = *h.body;
  CHECK(std::abs(body.total_mass - 75.0) < 0.5);
  const f64 legs = body.parts[B::thighL]->mass + body.parts[B::shinL]->mass + body.parts[B::footL]->mass;
  CHECK(legs > 10.0);
  CHECK(legs < 14.0);
  // the centre of mass of a standing body is just above the hips
  body.set_from_pose(*h.world, nullptr, 0.0);
  const V3 c = body.com();
  CHECK(c.z > 0.9);
  CHECK(c.z < 1.1);
  WorldPose out(h.sk);
  body.write_pose(out);
  for (i32 b = 0; b < h.sk->count; ++b) {
    if (b == H::weapon) continue;
    CHECK(vdist(out.p[size_t(b)], h.world->p[size_t(b)]) < 1e-6);
  }
}

TEST_CASE("anim body (shallow): without muscle it collapses onto the ground and sleeps; it never sinks in") {
  FlatGround flat(0.0);
  Humanoid h = humanoid(&flat, 0.0);
  HumanoidBody& body = *h.body;
  body.set_from_pose(*h.world, nullptr, 0.0);
  body.tone.fill(0.0f);
  body.apply_tone();
  f64 t = 0.0;
  for (; t < 8.0 && !body.system.try_sleep(0.5); t += DT) body.system.step(DT);
  CHECK(body.system.asleep);
  for (const RigidBody* p : body.parts)
    for (const Sphere& s : p->spheres) CHECK(p->point(s.c).z > s.r - 0.03);
  CHECK(body.parts[B::head]->x.z < 0.4);
}

TEST_CASE("anim body (shallow): muscles and assists hold a standing pose; the arms keep their shape under gravity") {
  FlatGround flat(0.0);
  Humanoid h = humanoid(&flat, 0.0);
  hold_standing(h);
  for (i32 i = 0; i < 180; ++i) {
    h.body->compensate_gravity();
    h.body->system.step(DT);
  }
  check_standing(h);
}

TEST_CASE("anim body (deep): a core articulation; without muscle it collapses onto the ground and sleeps; it never sinks in") {
  World w;
  w.load(ground());
  WorldCollision wc(w);
  Humanoid h = humanoid(&wc, kTop);
  HumanoidBody& body = *h.body;
  body.set_from_pose(*h.world, nullptr, 0.0);
  body.tone.fill(0.0f);
  body.apply_tone();
  CoreBinding bind;
  REQUIRE(bind.bind(w, body.system));
  CHECK(body.system.external);
  f64 t = 0.0;
  for (; t < 8.0 && !body.system.asleep; t += DT) {
    body.system.try_sleep(0.5);
    bind.push(w);
    w.tick();
    REQUIRE(bind.pull(w, DT));
  }
  CHECK(body.system.asleep);
  for (const RigidBody* p : body.parts)
    for (const Sphere& s : p->spheres) CHECK(p->point(s.c).z - kTop > s.r - 0.03);
  CHECK(body.parts[B::head]->x.z - kTop < 0.4);
  // unbound, the system is its own again, where the core left it
  const V3 head = body.parts[B::head]->x;
  bind.unbind(w);
  CHECK(!body.system.external);
  CHECK(w.articulations().empty());
  CHECK(vdist(body.parts[B::head]->x, head) == 0.0);
}

TEST_CASE("anim body (deep): muscles and assists hold a standing pose in the core; the arms keep their shape under gravity") {
  World w;
  w.load(ground());
  WorldCollision wc(w);
  Humanoid h = humanoid(&wc, kTop);
  hold_standing(h);
  CoreBinding bind;
  REQUIRE(bind.bind(w, h.body->system));
  for (i32 i = 0; i < 180; ++i) {
    h.body->compensate_gravity();
    bind.push(w);
    w.tick();
    REQUIRE(bind.pull(w, DT));
  }
  check_standing(h);
  // a shove the host gives the bodies goes to the links
  const V3 before = h.body->com_velocity();
  h.body->shove(V3{1.5, 0, 0});
  bind.push(w);
  w.tick();
  REQUIRE(bind.pull(w, DT));
  CHECK(h.body->com_velocity().x > before.x + 0.5);
}
