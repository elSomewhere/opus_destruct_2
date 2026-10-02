// svx_anim motion plan (the port of the original's test/plan.test.ts): planted feet that do not
// slide, legs that reach them walking, running and crouching, a shouldered rifle on its target.
#include <cmath>
#include <memory>
#include <optional>

#include "doctest.h"
#include "svx/anim/motion/plan.hpp"
#include "svx/anim/rig.hpp"

using namespace svx;
using namespace svx::anim;

namespace {

struct Walk {
  std::unique_ptr<MotionPlan> an;
  f64 max_slide = 0.0;
  f64 max_ik_err = 0.0;
  f64 max_swing_err = 0.0;
  int planted = 0;
  bool finite = true;
};

// (the original's rifle: its attach points)
PropPtr rifle() {
  auto p = std::make_shared<Prop>();
  p->tags = {"firearm", "long_firearm", "two_handed"};
  p->support = V3{0, 0.3 * 0.8, 0.03};
  p->stock = V3{0, -0.43 * 0.8, 0.05};
  p->muzzle = V3{0, 0.68 * 0.8, 0.068};
  p->magazine = V3{0, 0.12 * 0.8, -0.06};
  p->model = make_rifle()->model;
  p->sockets = {{"primary", p->grip}, {"secondary", p->support}};
  return p;
}

Walk walk(f64 speed, f64 seconds, f64 crouch = 0.0, bool with_rifle = false, bool aim = false) {
  static const FlatGround ground(0.0);
  Walk r;
  r.an = std::make_unique<MotionPlan>(humanoid_skeleton(), &ground, 7);
  MotionPlan& an = *r.an;
  if (with_rifle) an.weapon = rifle();
  an.input.crouch = crouch;
  if (aim) {
    an.input.carry = Carry::Aim;
    an.input.aim_at = V3{20, 5, 1.4};
  }
  an.place(V3{0, 0, 0}, 0);
  const f64 dt = 1.0 / 60.0;
  std::optional<V3> last[2];
  f64 x = 0;
  for (int i = 0; i < seconds * 60; ++i) {
    const f64 t = (i + 1) * dt;
    x += speed * std::min(1.0, t / 0.4) * dt;  // (hosts accelerate over a few tenths of a second)
    an.set_root(V3{x, 0, 0}, 0);
    an.update(dt);
    const auto feet = an.foot_state();
    for (int k = 0; k < 2; ++k) {
      const FootState& f = feet[size_t(k)];
      if (f.planted) {
        ++r.planted;
        if (last[k]) r.max_slide = std::max(r.max_slide, vdist(*last[k], f.pos));
        last[k] = f.pos;
      } else {
        last[k].reset();
      }
      const V3 ankle = an.world.p[k == 0 ? H::footL : H::footR];
      // a planted foot must be reached exactly; a swinging one may pass a little short
      if (f.planted) r.max_ik_err = std::max(r.max_ik_err, vdist(ankle, f.ankle));
      else r.max_swing_err = std::max(r.max_swing_err, vdist(ankle, f.ankle));
    }
    for (const V3& p : an.world.p) r.finite = r.finite && std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
  }
  return r;
}

}  // namespace

TEST_CASE("anim plan: walking plants feet without sliding and the legs reach them") {
  const Walk r = walk(1.4, 4);
  CHECK(r.finite);
  CHECK(r.max_slide < 1e-9);
  CHECK(r.max_ik_err < 0.03);
  CHECK(r.max_swing_err < 0.1);
  CHECK(r.planted > 200);
  // the character got where the root went and the pelvis is at a plausible height
  const V3 pelvis = r.an->world.p[H::pelvis];
  CHECK(std::abs(pelvis.x - (1.4 * 4 - 0.28)) < 0.3);
  CHECK(pelvis.z > 0.85);
  CHECK(pelvis.z < 1.0);
}

TEST_CASE("anim plan: running and crouch-walking stay within reach") {
  const Walk run = walk(4.5, 3);
  CHECK(run.finite);
  CHECK(run.max_slide < 1e-9);
  CHECK(run.max_ik_err < 0.03);
  CHECK(run.max_swing_err < 0.15);
  const Walk cr = walk(1.0, 3, 1.0);
  CHECK(cr.finite);
  CHECK(cr.max_ik_err < 0.02);
  CHECK(cr.an->world.p[H::pelvis].z < 0.72);
}

TEST_CASE("anim plan: a shouldered rifle points at the target and both hands hold it") {
  const Walk r = walk(0, 2, 0.0, true, true);
  const MotionPlan& an = *r.an;
  const Prop& rf = *an.weapon;
  const V3 muzzle = an.prop_point(rf.muzzle);
  const V3 stock = an.prop_point(rf.stock);
  const V3 dir = muzzle - stock;
  const V3 to_t = V3{20, 5, 1.4} - stock;
  const f64 cos = dot(dir, to_t) / (hypot3(dir.x, dir.y, dir.z) * hypot3(to_t.x, to_t.y, to_t.z));
  CHECK(cos > 0.995);
  const V3 grip = an.prop_point(rf.grip);
  const V3 hand_r = an.world.point_of(H::handR, an.skeleton->rest_head[H::handR]);
  CHECK(vdist(hand_r, grip) < 0.12);
}
