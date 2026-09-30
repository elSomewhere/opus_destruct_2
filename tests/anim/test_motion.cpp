// svx_anim motion (the port of the original's test/motion.test.ts): keyframe tracks, actions and
// their players, stances, lying and getting up, weapon holds, strikes, idles and conversation, the
// gait's feet, turning, personal styles. (The original's tests of whole characters - dropped
// weapons, brawls, melee - belong with Character.)
#include <algorithm>
#include <cmath>
#include <memory>
#include <set>

#include "doctest.h"
#include "svx/anim/motion/plan.hpp"
#include "svx/anim/rig.hpp"

using namespace svx;
using namespace svx::anim;

namespace {

const f64 DT = 1.0 / 60.0;

const FlatGround& flat_ground() {
  static const FlatGround g(0.0);
  return g;
}

// A motion plan at the origin facing +y (yaw pi/2: model space = world space).
std::unique_ptr<MotionPlan> standing(f64 yaw = kPi / 2, f64 seed = 5) {
  auto a = std::make_unique<MotionPlan>(humanoid_skeleton(), &flat_ground(), seed);
  a->place(V3{0, 0, 0}, yaw);
  return a;
}

// Steps the plan in place for `seconds`, calling `each` after every frame.
template <class F>
void run(MotionPlan& a, f64 seconds, F&& each) {
  const int n = int(std::floor(seconds * 60 + 0.5));
  for (int i = 0; i < n; ++i) {
    a.set_root(a.root_pos, a.root_yaw);
    a.update(DT);
    each(a);
  }
}
void run(MotionPlan& a, f64 seconds) {
  run(a, seconds, [](MotionPlan&) {});
}

bool finite_pose(const MotionPlan& a) {
  for (const V3& p : a.world.p)
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
  for (const Quat& q : a.world.q)
    if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w)) return false;
  return true;
}

f64 sample1(const Track& tr, f64 t) {
  f64 v[3] = {0, 0, 0};
  tr.sample(t, v);
  return v[0];
}

// The original's props (their attach points: the plan needs no model).
PropPtr make_prop(PropKind kind) {
  auto p = std::make_shared<Prop>();
  p->kind = kind;
  switch (kind) {
    case PropKind::Rifle:
      p->support = V3{0, 0.3 * 0.8, 0.03};
      p->stock = V3{0, -0.43 * 0.8, 0.05};
      p->muzzle = V3{0, 0.68 * 0.8, 0.068};
      p->magazine = V3{0, 0.12 * 0.8, -0.06};
      break;
    case PropKind::Lmg:
      p->support = V3{0, 0.25, 0.03};
      p->stock = V3{0, -0.39, 0.05};
      p->muzzle = V3{0, 0.65, 0.07};
      p->magazine = V3{0.03, 0.1, -0.05};
      break;
    case PropKind::Pistol:
      p->support = V3{-0.018, -0.01, -0.035};
      p->stock = V3{0, -0.03, 0.02};
      p->muzzle = V3{0, 0.18, 0.045};
      p->magazine = V3{0, -0.01, -0.08};
      p->one_handed = true;
      break;
    case PropKind::Knife:
      p->stock = V3{0, -0.07, 0};
      p->muzzle = V3{0, 0.22, 0.004};
      p->one_handed = true;
      break;
    default:
      break;
  }
  return p;
}

}  // namespace

// ---- 1. curves ---------------------------------------------------------------------------------

TEST_CASE("anim motion: curve smooth keys pass through their values and clamp outside the range") {
  const Track tr({Key(0, 1.0), Key(1, 3.0), Key(2, -2.0), Key(3, 0.0)});
  const f64 at[4][2] = {{0, 1}, {1, 3}, {2, -2}, {3, 0}};
  for (const auto& tv : at) CHECK(std::abs(sample1(tr, tv[0]) - tv[1]) < 1e-9);
  CHECK(sample1(tr, -5) == 1.0);
  CHECK(sample1(tr, 10) == 0.0);
  CHECK(tr.start() == 0.0);
  CHECK(tr.end() == 3.0);
  // between keys the value stays between neighbours (roughly: cubic may overshoot a little)
  const f64 mid = sample1(tr, 0.5);
  CHECK(mid > 1.0);
  CHECK(mid < 3.2);
}

TEST_CASE("anim motion: curve hold steps, snap reaches most of the change early, linear is linear") {
  const Track hold({Key(0, 0.0), Key(1, 10.0, Ease::Hold)});
  CHECK(sample1(hold, 0.99) == 0.0);
  CHECK(sample1(hold, 1) == 10.0);
  const Track snap({Key(0, 0.0), Key(1, 1.0, Ease::Snap)});
  CHECK(sample1(snap, 0.33) > 0.75);
  const Track lin({Key(0, 0.0), Key(2, 4.0, Ease::Linear)});
  CHECK(std::abs(sample1(lin, 0.5) - 1.0) < 1e-9);
  const Track inn({Key(0, 0.0), Key(1, 1.0, Ease::In)});
  const Track out({Key(0, 0.0), Key(1, 1.0, Ease::Out)});
  CHECK(sample1(inn, 0.5) < 0.5);
  CHECK(sample1(out, 0.5) > 0.5);
}

TEST_CASE("anim motion: curve vector tracks sample every component") {
  const Track tr({Key(0, {0, 1, 2}), Key(1, {4, 5, 6})});
  CHECK(tr.dim == 3);
  f64 a[3], b[3], m[3];
  tr.sample(0, a);
  tr.sample(1, b);
  tr.sample(0.5, m);
  for (int i = 0; i < 3; ++i) {
    CHECK(a[i] == f64(i));
    CHECK(b[i] == f64(i + 4));
    CHECK(std::abs(m[i] - (i + 2)) < 1e-9);
  }
}

// ---- 2. actions --------------------------------------------------------------------------------

TEST_CASE("anim motion: mirroring swaps sides and mirrors positions and rotations") {
  ActionDef def;
  def.name = "x";
  def.duration = 1;
  def.set(Channel::HandR, {Key(0, {0.2, 0.3, 0.4})});
  def.set(Channel::HandRrot, {Key(0, {10, 20, 30})});
  def.set(Channel::StrikeR, {Key(0, 1.0)});
  def.set(Channel::Chest, {Key(0, {5, 6, 7})});
  def.events = {ActionEvent{0.5, "strike", Limb::HandR}};
  const ActionDef m = mirror_action(def);
  CHECK(m.name == "x.m");
  CHECK(m.keys(Channel::HandL)[0].v == std::vector<f64>{-0.2, 0.3, 0.4});
  CHECK(m.keys(Channel::HandLrot)[0].v == std::vector<f64>{10, -20, -30});
  CHECK(m.keys(Channel::StrikeL)[0].v == std::vector<f64>{1});
  CHECK(m.keys(Channel::Chest)[0].v == std::vector<f64>{5, -6, -7});
  CHECK(!m.drives(Channel::HandR));
  CHECK(m.events[0].limb == Limb::HandL);
  // the library carries mirrored versions of everything
  CHECK(action_def("jab") != nullptr);
  CHECK(action_def("jab.m") != nullptr);
  REQUIRE(action_def("cross.m") != nullptr);
  CHECK(action_def("cross.m")->drives(Channel::StrikeL));
  // (an unknown action: none, and a plan does not play it)
  CHECK(action_def("moonwalk") == nullptr);
  CHECK(!standing()->play("moonwalk"));
}

TEST_CASE("anim motion: players fade in and out; one-shot events fire once, loop events every cycle") {
  ActionPlayer one(action_def("jab"));
  int strikes = 0;
  std::vector<f64> weights;
  std::vector<const ActionEvent*> crossed;
  for (f64 t = 0; t < 1; t += 1.0 / 60) {
    crossed.clear();
    one.advance(1.0 / 60, &crossed);
    for (const ActionEvent* e : crossed) strikes += e->name == "strike" ? 1 : 0;
    weights.push_back(one.weight());
  }
  CHECK(strikes == 1);
  CHECK(one.done());
  CHECK(weights.front() < 1.0);  // (fades in)
  CHECK(*std::max_element(weights.begin(), weights.end()) > 0.99);  // (reaches full weight)
  CHECK(weights.back() < 0.01);  // (fades out)

  ActionDef loop_def;
  loop_def.name = "l";
  loop_def.duration = 0.5;
  loop_def.loop = true;
  loop_def.set(Channel::Head, {Key(0, {0, 0, 0})});
  loop_def.events = {ActionEvent{0.25, "beat", Limb::None}};
  ActionPlayer loop(&loop_def);
  size_t beats = 0;
  for (f64 t = 0; t < 2.01; t += 1.0 / 60) {
    crossed.clear();
    loop.advance(1.0 / 60, &crossed);
    beats += crossed.size();
  }
  CHECK(beats == 4);
  CHECK(!loop.done());
  loop.stop();
  for (int i = 0; i < 30; ++i) loop.advance(1.0 / 60);
  CHECK(loop.done());  // (a stopped loop finishes after its fade)
}

TEST_CASE("anim motion: every action samples finite values on all channels over its duration") {
  CHECK(actions().size() == 88);
  for (const ActionDef& def : actions()) {
    ActionPlayer p(&def);
    ChannelFrame frame;
    bool finite = true;
    for (f64 t = 0; t <= def.duration + 1e-9; t += def.duration / 24) {
      p.time = t;
      p.sample(frame);
      for (int c = 0; c < kChannelCount; ++c)
        if (frame.has(Channel(c)))
          for (const f64 x : frame.v[size_t(c)]) finite = finite && std::isfinite(x);
    }
    CHECK_MESSAGE(finite, def.name);
    CHECK_MESSAGE(frame.driven != 0u, def.name, " has channels");
  }
}

// ---- 3. stances --------------------------------------------------------------------------------

TEST_CASE("anim motion: kneel, prone, sit and ground are reached with sensible heights, then back to standing") {
  for (const Stance s : {Stance::Kneel, Stance::Prone, Stance::Sit, Stance::Ground}) {
    CAPTURE(int(s));
    auto a = standing();
    a->input.stance = s;
    if (s == Stance::Sit) a->input.seat = SeatInfo{V3{0, -0.38, 0.46}, true, std::nullopt, std::nullopt};
    run(*a, 3);
    CHECK(a->stance == s);
    CHECK(finite_pose(*a));
    const V3 pelvis = a->world.p[H::pelvis];
    switch (s) {
      case Stance::Kneel:
        CHECK(pelvis.z > 0.5);
        CHECK(pelvis.z < 0.65);
        CHECK(a->world.p[H::footL].z < 0.2);  // (the left, front foot on the ground)
        break;
      case Stance::Prone:
        CHECK(pelvis.z < 0.3);
        CHECK(a->world.p[H::head].z < 0.6);
        break;
      case Stance::Sit:
        CHECK(pelvis.z > 0.5);
        CHECK(pelvis.z < 0.65);
        CHECK(pelvis.y < -0.2);  // (the pelvis sits on the seat behind the root)
        CHECK(a->world.p[H::footL].z < 0.2);
        CHECK(a->world.p[H::footR].z < 0.2);
        break;
      default:
        CHECK(pelvis.z < 0.3);
    }
    a->input.stance = Stance::Stand;
    run(*a, 3);
    CHECK(a->stance == Stance::Stand);
    CHECK(a->world.p[H::pelvis].z > 0.85);
    CHECK(finite_pose(*a));
  }
}

TEST_CASE("anim motion: every way of sitting on the ground keeps the pelvis low") {
  for (const GroundVariant v : {GroundVariant::Cross, GroundVariant::KneesUp, GroundVariant::LegsOut}) {
    CAPTURE(int(v));
    auto a = standing();
    a->input.stance = Stance::Ground;
    a->input.ground_variant = v;
    run(*a, 3);
    CHECK(a->stance == Stance::Ground);
    CHECK(a->world.p[H::pelvis].z < 0.3);
    CHECK(finite_pose(*a));
  }
}

// ---- 4. lying and getting up (the plan's side of a fall) --------------------------------------

TEST_CASE("anim motion: a fall played by the plan goes down, and getting up comes back through the stances") {
  auto a = standing();
  run(*a, 0.5);
  a->fall(true);
  CHECK(a->down());
  run(*a, 1.5);
  CHECK(a->stance == Stance::Down);
  CHECK(a->world.p[H::pelvis].z < 0.3);
  a->get_up();
  std::set<Stance> seen;
  run(*a, 6, [&](MotionPlan& b) { seen.insert(b.stance); });
  CHECK(seen.count(Stance::Ground) == 1);
  CHECK(seen.count(Stance::Kneel) == 1);
  CHECK(a->stance == Stance::Stand);
  CHECK(!a->down());  // (up again)
  CHECK(a->world.p[H::pelvis].z > 0.85);
  CHECK(finite_pose(*a));
  // face down: pushing up through prone
  auto b = standing();
  b->fall(false);
  run(*b, 1.5);
  b->get_up();
  std::set<Stance> seen_b;
  run(*b, 6, [&](MotionPlan& x) { seen_b.insert(x.stance); });
  CHECK(seen_b.count(Stance::Prone) == 1);
  CHECK(seen_b.count(Stance::Kneel) == 1);
  CHECK(b->stance == Stance::Stand);
}

// ---- 6. weapons --------------------------------------------------------------------------------

namespace {

const V3 kFar{1.5, 10, 1.4};

// The cosine between the held prop's barrel (+y) and the direction from its muzzle to t.
f64 barrel_cos(const MotionPlan& a, const V3& t) {
  const V3 ax = rotate(a.weapon_rot, V3{0, 1, 0});
  const V3 m = a.prop_point(a.weapon->muzzle);
  const V3 d = t - m;
  return dot(ax, d) / hypot3(d.x, d.y, d.z);
}

std::unique_ptr<MotionPlan> armed(PropKind kind, Carry carry) {
  auto a = standing();
  a->weapon = make_prop(kind);
  a->input.carry = carry;
  a->input.aim_at = kFar;
  run(*a, 2);
  return a;
}

}  // namespace

TEST_CASE("anim motion: an aimed pistol points at the target with both hands on the grip") {
  auto a = armed(PropKind::Pistol, Carry::Aim);
  CHECK(barrel_cos(*a, kFar) > 0.99);
  const V3 grip = a->prop_point(a->weapon->grip);
  CHECK(vdist(a->world.p[H::handR], grip) < 0.15);  // (the right wrist at the grip)
  CHECK(vdist(a->world.p[H::handL], grip) < 0.15);  // (the left wrist wraps the gun hand)
}

TEST_CASE("anim motion: a pistol fired one-handed is held by the right hand only") {
  auto a = armed(PropKind::Pistol, Carry::Hip);
  CHECK(barrel_cos(*a, kFar) > 0.99);
  const V3 grip = a->prop_point(a->weapon->grip);
  CHECK(vdist(a->world.p[H::handR], grip) < 0.15);  // (the right hand holds it)
  CHECK(vdist(a->world.p[H::handL], grip) > 0.3);   // (the left hand is elsewhere)
}

TEST_CASE("anim motion: a machine gun fired from the hip points roughly at the target") {
  auto a = armed(PropKind::Lmg, Carry::Hip);
  CHECK(barrel_cos(*a, kFar) > 0.95);
  CHECK(vdist(a->world.p[H::handR], a->prop_point(a->weapon->grip)) < 0.15);
  const Skeleton& sk = *a->skeleton;
  const V3 h = sk.rest_head[H::handL], t = sk.rest_tail[H::handL];
  const V3 palm = a->world.point_of(H::handL, h + (t - h) * 0.42);
  CHECK(vdist(palm, a->prop_point(a->weapon->support)) < 0.05);  // (the support palm on the handguard)
}

TEST_CASE("anim motion: a knife stays in the right hand while walking") {
  auto a = standing();
  a->weapon = make_prop(PropKind::Knife);
  run(*a, 0.5);
  const Skeleton& sk = *a->skeleton;
  const V3 h = sk.rest_head[H::handR], t = sk.rest_tail[H::handR];
  const V3 palm_rest = h + (t - h) * 0.42;
  f64 y = 0, worst = 0;
  for (int i = 0; i < 120; ++i) {
    y += 1.3 * DT;
    a->set_root(V3{0, y, 0}, kPi / 2);
    a->update(DT);
    worst = std::max(worst, vdist(a->prop_point(a->weapon->grip), a->world.point_of(H::handR, palm_rest)));
  }
  CHECK(worst < 0.1);
}

// ---- 7. actions on the body --------------------------------------------------------------------

namespace {

struct Strike {
  std::optional<AnimEvent> ev;
  f64 slide = 0.0;
  bool finite = true;
};

// Plays a strike from the guard: its strike event and the support foot's slide.
Strike strike(std::string_view name, const V3& target) {
  auto a = standing();
  a->input.guard = true;
  run(*a, 1);
  a->take_events();
  const FootState left0 = a->foot_state()[0];
  Strike s;
  const bool played = a->play(name, target);
  CHECK(played);
  run(*a, 1.2, [&](MotionPlan& b) {
    const FootState l = b.foot_state()[0];
    if (l.planted && left0.planted) s.slide = std::max(s.slide, vdist(l.pos, left0.pos));
    for (const AnimEvent& e : b.take_events())
      if (e.name == "strike") s.ev = e;
    s.finite = s.finite && finite_pose(b);
  });
  return s;
}

}  // namespace

TEST_CASE("anim motion: a jab lands on a target within arm reach") {
  const V3 t{0, 0.75, 1.55};
  const Strike s = strike("jab", t);
  REQUIRE(s.ev.has_value());
  CHECK(s.finite);
  CHECK(s.ev->limb == Limb::HandL);
  CHECK(vdist(s.ev->pos, t) < 0.12);
  REQUIRE(s.ev->target.has_value());
  CHECK(s.ev->target->x == t.x);
  CHECK(s.ev->target->y == t.y);
  CHECK(s.ev->target->z == t.z);
}

// (fighters stand ~0.9-1 m apart: strikes step in to targets out of reach)
TEST_CASE("anim motion: a jab steps in to reach a head 0.95 m away") {
  const V3 t{0, 0.95, 1.6};
  const Strike s = strike("jab", t);
  REQUIRE(s.ev.has_value());
  CHECK(vdist(s.ev->pos, t) < 0.12);
}

TEST_CASE("anim motion: a front kick reaches the belly and the support foot stays planted") {
  const V3 t{0, 0.85, 1.0};
  const Strike s = strike("frontKick", t);
  REQUIRE(s.ev.has_value());
  CHECK(s.ev->limb == Limb::FootR);
  CHECK(vdist(s.ev->pos, t) < 0.2);
  CHECK(s.slide < 1e-6);
}

TEST_CASE("anim motion: a front kick drives the hips in to reach a belly 1.1 m away") {
  const V3 t{0, 1.1, 1.0};
  const Strike s = strike("frontKick", t);
  REQUIRE(s.ev.has_value());
  CHECK(vdist(s.ev->pos, t) < 0.2);
}

TEST_CASE("anim motion: a rifle reload ends with a reloaded event") {
  auto a = armed(PropKind::Rifle, Carry::Aim);
  a->take_events();
  CHECK(a->play("reloadRifle"));
  CHECK(a->busy());
  std::vector<AnimEvent> evs;  // (taken into a buffer of the host's)
  run(*a, 3, [&](MotionPlan& b) { b.take_events(evs); });
  CHECK(std::any_of(evs.begin(), evs.end(), [](const AnimEvent& e) { return e.name == "reloaded"; }));
  CHECK(a->events.empty());
  CHECK(!a->busy());
  CHECK(barrel_cos(*a, kFar) > 0.99);  // (back on target after the reload)
}

TEST_CASE("anim motion: a speaker gestures; an idle character takes postures and fidgets") {
  auto talker = standing();
  talker->input.talk = Talk::Speak;
  std::set<std::string> said;
  run(*talker, 6, [&](MotionPlan& b) {
    if (!b.action_name().empty()) said.insert(std::string(b.action_name()));
  });
  CHECK(!said.empty());  // (gestures while speaking)

  auto idle = standing();
  const V3 hand0 = idle->world.p[H::handR];
  std::set<std::string> done;
  f64 moved = 0;
  run(*idle, 25, [&](MotionPlan& b) {
    if (!b.action_name().empty()) done.insert(std::string(b.action_name()));
    moved = std::max(moved, vdist(b.world.p[H::handR], hand0));
  });
  CHECK((!done.empty() || moved > 0.15));
  CHECK(finite_pose(*idle));
}

TEST_CASE("anim motion: the feet alternate (half a cycle apart) walking and running, from a standstill and turning") {
  struct Case {
    f64 speed, turn;
    int style_seed;
  };
  for (const Case c : {Case{1.4, 0, 0}, Case{2.2, 0, 0}, Case{3.5, 0, 3}, Case{5, 0, 0}, Case{6, 0, 4}, Case{4.5, 1.2, 0}}) {
    CAPTURE(c.speed);
    CAPTURE(c.turn);
    auto a = standing(kPi / 2);
    if (c.style_seed) a->style = random_style(c.style_seed, StyleKind::Soldier);
    run(*a, 0.5);
    f64 x = 0, y = 0, yaw = kPi / 2, t = 0;
    bool prev[2] = {true, true};
    std::vector<f64> lands[2];
    for (int i = 0; i < 60 * 7; ++i) {
      t += DT;
      const f64 v = std::min(c.speed, t * 4);
      yaw += c.turn * DT;
      x += anim::cos(yaw) * v * DT;
      y += anim::sin(yaw) * v * DT;
      a->set_root(V3{x, y, 0}, yaw);
      a->update(DT);
      const auto fs = a->foot_state();
      for (int k = 0; k < 2; ++k) {
        if (fs[size_t(k)].planted && !prev[k] && t > 3) lands[k].push_back(t);
        prev[k] = fs[size_t(k)].planted;
      }
    }
    const std::vector<f64>& L = lands[0];
    const std::vector<f64>& R = lands[1];
    REQUIRE(L.size() >= 2);
    const f64 cycle = (L.back() - L.front()) / f64(L.size() - 1);
    // one landing per foot per gait cycle, the other foot half a cycle later
    CHECK(std::abs(cycle * a->gait.freq - 1) < 0.1);
    for (const f64 l : L) {
      const auto r = std::find_if(R.begin(), R.end(), [&](f64 v) { return v > l; });
      if (r == R.end()) continue;
      const f64 off = std::fmod((*r - l) / cycle, 1.0);
      CHECK(off > 0.4);
      CHECK(off < 0.6);
    }
  }
}

TEST_CASE("anim motion: walking keeps the trunk upright (a peek lean, a body turning to its target)") {
  for (const int lean : {0, 1}) {
    CAPTURE(lean);
    auto a = standing(0);
    a->style = random_style(2, StyleKind::Soldier);
    a->weapon = make_prop(PropKind::Rifle);
    a->input.carry = Carry::Aim;
    a->input.lean = lean;
    run(*a, 0.5);
    f64 x = 0, yaw = 0, worst = 0;
    for (int i = 0; i < 60 * 5; ++i) {
      x += 1.5 * DT;
      // the body swings round to a target off to the side while walking straight on
      yaw = std::min(1.2, yaw + 0.6 * DT);
      a->input.aim_at = V3{x + anim::cos(yaw) * 20, anim::sin(yaw) * 20, 1.4};
      a->set_root(V3{x, 0, 0}, yaw);
      a->update(DT);
      if (i < 90) continue;
      const V3 u = rotate(a->world.q[H::chest], V3{0, 0, 1});
      worst = std::max(worst, std::abs(anim::asin(u.x * anim::sin(yaw) - u.y * anim::cos(yaw))));
    }
    CHECK(worst < 0.07);  // (the chest rolls under 4 degrees)
  }
}

TEST_CASE("anim motion: turning on the spot: the legs point with their feet, the trunk leads within its twist, the feet follow") {
  auto yaw_of = [](const Quat& q) {
    const V3 f = rotate(q, V3{0, 1, 0});
    return anim::atan2(f.y, f.x);
  };
  auto wrap = [](f64 x) { return anim::atan2(anim::sin(x), anim::cos(x)); };
  auto a = standing(0);
  a->style = random_style(3, StyleKind::Soldier);
  a->weapon = make_prop(PropKind::Rifle);
  a->input.carry = Carry::Aim;
  a->input.aim_at = V3{20, 0, 1.4};
  run(*a, 1.5);
  a->input.aim_at = V3{0, 20, 1.4};
  f64 yaw = 0, leg_worst = 0, trunk_worst = 0;
  for (int i = 0; i < 60 * 3; ++i) {
    yaw = std::min(kPi / 2, yaw + 2.5 * DT);
    a->set_root(V3{0, 0, 0}, yaw);
    a->update(DT);
    const auto fs = a->foot_state();
    const i32 thighs[2] = {H::thighL, H::thighR};
    for (int k = 0; k < 2; ++k) {
      const FootState& f = fs[size_t(k)];
      if (f.planted) leg_worst = std::max(leg_worst, std::abs(wrap(yaw_of(a->world.q[size_t(thighs[k])]) - f.yaw)));
    }
    trunk_worst = std::max(trunk_worst, std::abs(wrap(yaw_of(a->world.q[H::chest]) - yaw_of(a->world.q[H::pelvis]))));
  }
  CHECK(leg_worst < 0.45);   // (a planted leg points with its foot)
  CHECK(trunk_worst < 0.9);  // (the chest turns within the spine's twist off the hips)
  // the feet came round to the new facing (bladed: a little to the right of it)
  for (const FootState& f : a->foot_state()) CHECK(std::abs(wrap(f.yaw - (kPi / 2 - 0.42))) < 0.55);
}

// ---- 8. styles ---------------------------------------------------------------------------------

TEST_CASE("anim motion: characters walk differently; soldiers are heavy") {
  struct Walk {
    f64 freq, bob, cycles;
  };
  auto walk = [](f64 seed) {
    auto a = standing();
    a->style = random_style(seed, StyleKind::Civilian);
    f64 y = 0, cycles = 0, last = a->phase();
    for (int i = 0; i < 300; ++i) {
      y += 1.4 * DT;
      a->set_root(V3{0, y, 0}, kPi / 2);
      a->update(DT);
      const f64 d = a->phase() - last;
      cycles += d < 0 ? d + 1 : d;
      last = a->phase();
    }
    return Walk{a->gait.freq, a->gait.bob, cycles};
  };
  const Walk a = walk(1), b = walk(2);
  CHECK((std::abs(a.freq - b.freq) > 0.01 || std::abs(a.bob - b.bob) > 0.002));
  CHECK(std::abs(a.cycles - b.cycles) > 0.05);  // (a different cadence over 5 s)
  for (int s = 1; s <= 10; ++s) CHECK(random_style(s, StyleKind::Soldier).heavy > kNeutralStyle.heavy);
  auto mean = [](StyleKind k) {
    f64 m = 0;
    for (int s = 1; s <= 20; ++s) m += random_style(s, k).heavy / 20;
    return m;
  };
  CHECK(mean(StyleKind::Soldier) > mean(StyleKind::Civilian));
}

// ---- the port's own: determinism --------------------------------------------------------------

TEST_CASE("anim motion: a plan is deterministic (the same seed and inputs, the same pose, bit for bit)") {
  auto go = [](f64 seed) {
    auto a = standing(kPi / 2, seed);
    a->weapon = make_prop(PropKind::Rifle);
    std::vector<V3> trace;
    f64 y = 0;
    for (int i = 0; i < 600; ++i) {
      if (i < 200) y += 1.2 * DT;
      a->input.carry = i > 300 ? Carry::Aim : Carry::Ready;
      a->input.aim_at = V3{2, 8, 1.4};
      a->set_root(V3{0, y, 0}, kPi / 2);
      a->update(DT);
      if (i == 400) a->play("reloadRifle");
      trace.push_back(a->world.p[H::handL]);
      trace.push_back(a->world.p[H::footR]);
    }
    return trace;
  };
  const std::vector<V3> a = go(3), b = go(3), c = go(4);
  bool same = true, differs = false;
  for (size_t i = 0; i < a.size(); ++i) {
    same = same && a[i].x == b[i].x && a[i].y == b[i].y && a[i].z == b[i].z;
    differs = differs || a[i].x != c[i].x || a[i].z != c[i].z;
  }
  CHECK(same);
  CHECK(differs);  // (another seed: another character)
}
