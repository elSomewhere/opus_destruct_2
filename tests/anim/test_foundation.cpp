// svx_anim foundations: seeded randomness as the TypeScript original has it, skeletons and poses,
// the humanoid rig, springs, keyframe tracks, two-bone IK.
#include <cmath>

#include "doctest.h"
#include "svx/anim/curve.hpp"
#include "svx/anim/ik.hpp"
#include "svx/anim/rig.hpp"
#include "svx/anim/spring.hpp"

using namespace svx;
using namespace svx::anim;

TEST_CASE("anim: seeded randomness is the original's, bit for bit") {
  // (the values the TypeScript mulberry32 and hash3 give)
  Rng r(static_cast<f64>(1 * 977 + 5));
  CHECK(r.next() == 0.23148476006463170);
  CHECK(r.next() == 0.11145334923639894);
  CHECK(r.next() == 0.87845718092285097);
  CHECK(hash3(-3, 7, 11, 5) == 0.066282087704166770);
  Rng n(-7.5);  // (a negative, fractional seed: truncated, modulo 2^32)
  CHECK(n.next() == 0.43306733411736786);
}

TEST_CASE("anim: the humanoid rig and its poses") {
  const SkeletonPtr sk = humanoid_skeleton();
  REQUIRE(sk->count == H::count);
  CHECK(sk->parents[H::handR] == H::forearmR);
  CHECK(sk->parents[H::upperarmR] == H::clavicleR);
  CHECK(sk->rest_head[H::thighR].x == doctest::Approx(0.1));
  CHECK(sk->rest_head[H::pelvis].z == doctest::Approx(0.97));
  CHECK(sk->is_below(H::toeL, H::thighL));
  // a turned pose: the world transforms follow the parents
  Pose pose(sk);
  pose.r[H::pelvis] = qz(kPi / 2);
  WorldPose w(sk);
  w.compute(pose, V3{1, 2, 0}, Quat{});
  // (turned a quarter to the left about z: the left hip, at -x, is now at -y)
  CHECK(w.p[H::thighL].x == doctest::Approx(1.0).epsilon(1e-9));
  CHECK(w.p[H::thighL].y == doctest::Approx(2.0 - 0.1).epsilon(1e-9));
  std::vector<f32> skin(size_t(16 * sk->count));
  w.write_skin(skin.data());
  // the skin matrix maps the rest head to the posed head
  const V3 rh = sk->rest_head[H::thighL];
  const f32* m = &skin[16 * H::thighL];
  CHECK(m[0] * rh.x + m[4] * rh.y + m[8] * rh.z + m[12] == doctest::Approx(w.p[H::thighL].x).epsilon(1e-5));
}

TEST_CASE("anim: springs, tracks") {
  Spring s(12.0, 1.0);
  for (int i = 0; i < 120; ++i) s.update(1.0, 1.0 / 60.0);
  CHECK(s.x == doctest::Approx(1.0).epsilon(1e-3));
  Spring u(10.0, 0.3);
  f64 peak = 0.0;
  for (int i = 0; i < 60; ++i) peak = std::max(peak, u.update(1.0, 1.0 / 60.0));
  CHECK(peak > 1.2);  // (underdamped: it overshoots)
  Track t({Key(0.0, 0.0), Key(1.0, 1.0), Key(2.0, 0.0, Ease::Linear)});
  f64 v = 0.0;
  t.sample(0.5, &v);
  CHECK(v > 0.4);
  CHECK(v < 0.8);
  t.sample(1.5, &v);
  CHECK(v == doctest::Approx(0.5));
  t.sample(5.0, &v);
  CHECK(v == doctest::Approx(0.0));
}

TEST_CASE("anim: two-bone IK reaches its target and bends towards the pole") {
  const SkeletonPtr sk = humanoid_skeleton();
  Pose pose(sk);
  ModelFK fk(sk);
  fk.update(pose);
  const V3 knee_fwd{0, 1, 0};
  const V3 target = fk.p[H::footL] + V3{0.0, 0.1, 0.25};
  const TwoBoneResult r = solve_two_bone(pose, fk, H::thighL, H::shinL, H::footL, target, knee_fwd, 0.0, &knee_fwd);
  CHECK(norm(fk.p[H::footL] - target) < 1e-6);
  CHECK(r.mid.y > fk.p[H::thighL].y);  // (the knee forward)
}
