// Regressions for active arm returns and contact-preserving gait transitions.
#include "scene.hpp"

using namespace scene;

namespace {
constexpr Path paths[] = {Path::Shallow, Path::Deep};
struct Rms {
  f64 sum = 0.0;
  int n = 0;
  void add(f64 x) { sum += x * x; ++n; }
  f64 value() const { return std::sqrt(sum / std::max(1, n)); }
};
f64 wrist_error(const Character& c, int side) {
  const int hand = side ? H::handR : H::handL, forearm = side ? H::forearmR : H::forearmL;
  return norm(qerror(conj(c.pose.q[forearm]) * c.pose.q[hand], conj(c.motion.world.q[forearm]) * c.motion.world.q[hand]));
}
void neutral(Character& c) {
  c.motion.input.idle = false;
  c.motion.style = kNeutralStyle;
}
}

TEST_CASE("motion quality: wrists settle under control after actions on both physics paths") {
  for (Path path : paths) for (const char* name : {"frontKick", "frontKick.m", "wipeBrow", "wipeBrow.m"}) {
    INFO(std::string(path_name(path)) << ": " << std::string(name));
    Scene s(path);
    Character& c = s.civilian(7);
    neutral(c);
    Host host = run(s, c, 2.0);
    REQUIRE(c.motion.play(name));
    const f64 duration = action_def(name)->duration;
    Rms error;
    host = run(s, c, duration + 1.0, 0.0, [&](Character& x, f64 t, Host&) {
      if (t > duration) for (int side = 0; side < 2; ++side) error.add(wrist_error(x, side));
      CHECK(x.behaviours.mode == BodyMode::Animated);
    }, host);
    CHECK(error.value() < 12.0 * kDeg);
    run(s, c, 1.0, 0.0, nullptr, host);
    for (int side = 0; side < 2; ++side) CHECK(wrist_error(c, side) < 5.0 * kDeg);
  }
}

TEST_CASE("motion quality: arms yield to shoves and turns without sustained wrist flopping") {
  for (Path path : paths) for (bool push : {false, true}) {
    INFO(std::string(path_name(path)) << ", push " << push);
    Scene s(path);
    Character& c = s.civilian(7);
    neutral(c);
    Host host = run(s, c, 2.0);
    if (push) c.push(V3{0.7, -1.0, 0.0}, 1.4);
    Rms error;
    bool reacted = false;
    run(s, c, 4.0, 0.0, [&](Character& x, f64 t, Host& h) {
      if (!push) h.yaw = kPi / 2 + kPi * 0.75 * std::min(1.0, t / 0.2);
      reacted = reacted || x.controlled();
      for (int side = 0; side < 2; ++side) error.add(wrist_error(x, side));
    }, host);
    CHECK(error.value() < 18.0 * kDeg);
    if (push) CHECK(reacted);
    CHECK(c.behaviours.mode == BodyMode::Animated);
  }
}

TEST_CASE("motion quality: muscle recruitment never holds a dead or stunned arm rigid") {
  for (Path path : paths) {
    INFO(std::string(path_name(path)));
    Scene s(path);
    Character& c = s.civilian(7);
    neutral(c);
    run(s, c, 1.0);
    REQUIRE(c.motion.play("wipeBrow"));
    run(s, c, 0.7);
    const f64 active = c.body.tone[B::handL];
    c.behaviours.push(V3{0.1, 0, 0}, 1.0);
    s.frame({&c});
    CHECK(c.body.tone[B::handL] < active * 0.5);
    c.behaviours.die(0.0);
    run(s, c, 0.8);
    REQUIRE(c.behaviours.mode == BodyMode::Dead);
    for (int part : {B::upperarmL, B::forearmL, B::handL, B::upperarmR, B::forearmR, B::handR}) {
      CHECK(c.body.tone[part] == 0.0f);
      CHECK(c.body.joints[part]->stiffness == 0.0);
    }
    CHECK_FALSE(c.body.hands[0]->enabled);
    CHECK_FALSE(c.body.hands[1]->enabled);
  }
}

TEST_CASE("motion quality: walking contacts stay locked through heel roll and touchdown") {
  FlatGround ground;
  for (f64 scale : {0.8, 1.0, 1.15}) for (f64 speed : {0.35, 0.7, 1.0, 1.4, 1.8, 2.2, 3.0, 4.0}) {
    INFO("scale " << scale << ", speed " << speed);
    MotionPlan p(humanoid_skeleton(HumanoidBuild{scale}), &ground, 7);
    p.style = kNeutralStyle;
    p.input.idle = false;
    p.place(V3{}, kPi / 2);
    V3 root;
    f64 v = 0.0, landing = 0.0, contact_error = 0.0;
    std::array<V3, 2> last{}, anchor{};
    std::array<bool, 2> planted{false, false};
    int contacts = 0;
    for (int tick = 0; tick < 360; ++tick) {
      v = std::min(speed, v + 4.0 * DT);
      root.y += v * DT;
      p.set_root(root, kPi / 2);
      p.update(DT);
      for (int side = 0; side < 2; ++side) {
        const auto& f = p.feet_planner.feet[side];
        const int bone = side ? H::footR : H::footL;
        if (tick >= 120) {
          if (f.planted) {
            const auto& d = p.feet_planner.dims;
            const V3 pivot{0, f.pitch < 0 ? d.ball_fwd : -d.heel_back, -d.ankle_h};
            const V3 contact = p.world.p[bone] + rotate(p.world.q[bone], pivot);
            const V3 lock = f.pos + rotate(qz(f.yaw - kPi / 2), V3{0, pivot.y, 0});
            contact_error = std::max(contact_error, norm(contact - lock));
            if (planted[side]) CHECK(norm(f.pos - anchor[side]) < 1e-9);
            ++contacts;
          }
          if (p.feet_planner.landed[side]) landing = std::max(landing, norm(p.world.p[bone] - last[side]) / DT);
        }
        last[side] = p.world.p[bone];
        anchor[side] = f.pos;
        planted[side] = f.planted;
      }
    }
    CHECK(contacts > 80);
    CHECK(contact_error < 0.002 * scale);
    CHECK(landing < (speed <= 1.8 ? 2.0 : 5.0) * std::sqrt(scale));
  }
}

TEST_CASE("motion quality: physical feet follow their rolling contacts on both paths") {
  for (Path path : paths) for (f64 speed : {0.7, 1.4, 1.8, 3.0}) {
    INFO(std::string(path_name(path)) << ", speed " << speed);
    Scene s(path);
    Character& c = s.civilian(7);
    neutral(c);
    Rms contact_error;
    run(s, c, 6.0, speed, [&](Character& x, f64 t, Host&) {
      if (t < 2.0) return;
      CHECK(x.behaviours.mode == BodyMode::Animated);
      const auto& d = x.motion.feet_planner.dims;
      for (int side = 0; side < 2; ++side) {
        const auto& f = x.motion.feet_planner.feet[side];
        if (!f.planted) continue;
        const int bone = side ? H::footR : H::footL;
        const V3 pivot{0, f.pitch < 0 ? d.ball_fwd : -d.heel_back, -d.ankle_h};
        const V3 lock = f.pos + rotate(qz(f.yaw - kPi / 2), V3{0, pivot.y, 0});
        contact_error.add(norm(x.pose.p[bone] + rotate(x.pose.q[bone], pivot) - lock));
      }
    });
    CHECK(contact_error.value() < (speed <= 1.4 ? 0.018 : 0.03));
  }
}
