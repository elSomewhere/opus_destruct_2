#include "svx/anim/motion/actions.hpp"

#include <algorithm>
#include <map>

namespace svx::anim {

// ---- definitions ---------------------------------------------------------------------------

void ActionDef::set(Channel c, std::vector<Key> keys) {
  const size_t i = size_t(c);
  for (Key& key : keys)
    if (key.v.size() > 3) key.v.resize(3);
  keys_[i] = std::move(keys);
  if (keys_[i].empty()) {
    tracks_[i] = Track();
    driven_ &= ~(1u << i);
    return;
  }
  tracks_[i] = Track(keys_[i]);
  driven_ |= 1u << i;
}

namespace {

using C = Channel;

Channel mirror_channel(Channel c) {
  switch (c) {
    case C::HandR: return C::HandL;
    case C::HandL: return C::HandR;
    case C::HandRw: return C::HandLw;
    case C::HandLw: return C::HandRw;
    case C::HandRrot: return C::HandLrot;
    case C::HandLrot: return C::HandRrot;
    case C::ElbowR: return C::ElbowL;
    case C::ElbowL: return C::ElbowR;
    case C::StrikeR: return C::StrikeL;
    case C::StrikeL: return C::StrikeR;
    case C::FootR: return C::FootL;
    case C::FootL: return C::FootR;
    case C::FootRw: return C::FootLw;
    case C::FootLw: return C::FootRw;
    case C::FootRpole: return C::FootLpole;
    case C::FootLpole: return C::FootRpole;
    case C::StrikeFootR: return C::StrikeFootL;
    case C::StrikeFootL: return C::StrikeFootR;
    case C::ClavR: return C::ClavL;
    case C::ClavL: return C::ClavR;
    default: return c;
  }
}

bool is_position(Channel c) {
  return c == C::HandR || c == C::HandL || c == C::ElbowR || c == C::ElbowL || c == C::FootR || c == C::FootL || c == C::Pelvis || c == C::WeaponPos || c == C::FootRpole || c == C::FootLpole;
}

bool is_euler(Channel c) {
  return c == C::HandRrot || c == C::HandLrot || c == C::Spine || c == C::Chest || c == C::Neck || c == C::Head || c == C::ClavR || c == C::ClavL ||
         c == C::PelvisRot || c == C::WeaponRot;
}

Limb mirror_limb(Limb l) {
  switch (l) {
    case Limb::HandR: return Limb::HandL;
    case Limb::HandL: return Limb::HandR;
    case Limb::FootR: return Limb::FootL;
    case Limb::FootL: return Limb::FootR;
    default: return l;
  }
}

}  // namespace

ActionDef mirror_action(const ActionDef& def) { return mirror_action(def, def.name + ".m"); }

ActionDef mirror_action(const ActionDef& def, std::string name) {
  ActionDef out;
  out.name = std::move(name);
  out.duration = def.duration;
  out.loop = def.loop;
  out.fade_in = def.fade_in;
  out.fade_out = def.fade_out;
  out.layer = def.layer;
  out.targeted = def.targeted;
  out.reach = def.reach;
  out.step_distance = def.step_distance;
  out.kick_pitch = def.kick_pitch;
  out.requires_tags = def.requires_tags;
  out.free_hands = def.free_hands;
  out.two_hands = def.two_hands;
  out.reverse_grip = def.reverse_grip;
  out.minimum_leg = def.minimum_leg;
  out.minimum_arm = def.minimum_arm;
  out.target_height = def.target_height;
  out.lead_side = -def.lead_side;
  out.support_turn = def.support_turn;
  out.left_handed = !def.left_handed;
  for (int i = 0; i < kChannelCount; ++i) {
    const Channel c = Channel(i);
    if (!def.drives(c)) continue;
    std::vector<Key> keys = def.keys(c);
    for (Key& key : keys) {
      // (a scalar key stays as it is)
      if (key.v.size() == 1) continue;
      if (is_position(c)) {
        key.v[0] = -key.v[0];
      } else if (is_euler(c)) {
        if (key.v.size() > 1) key.v[1] = -key.v[1];
        if (key.v.size() > 2) key.v[2] = -key.v[2];
      }
    }
    out.set(mirror_channel(c), std::move(keys));
  }
  out.events = def.events;
  for (ActionEvent& e : out.events) e.limb = mirror_limb(e.limb);
  return out;
}

// ---- players --------------------------------------------------------------------------------

bool ActionPlayer::done() const {
  if (stopping_) return time - stop_at_ >= def->fade_out;
  return !def->loop && time >= def->duration;
}

f64 ActionPlayer::weight() const {
  const f64 fi = def->fade_in;
  const f64 fo = def->fade_out;
  if (stopping_) {
    const f64 out = fo > 0.0 ? clamp(1.0 - (time - stop_at_) / fo, 0.0, 1.0) : 0.0;
    return stop_weight_ * out * out * (3.0 - 2.0 * out);
  }
  f64 w = preparation > 0.0 ? clamp((time + preparation) / preparation, 0.0, 1.0) : fi > 0.0 ? clamp(time / fi, 0.0, 1.0) : 1.0;
  if (!def->loop && fo > 0.0) w *= std::min(1.0, std::max(0.0, (def->duration - time) / fo));
  return w * w * (3.0 - 2.0 * w);
}

void ActionPlayer::stop() {
  if (!stopping_) { stop_weight_ = weight(); stop_at_ = time; stopping_ = true; }
}

void ActionPlayer::advance(f64 dt, std::vector<const ActionEvent*>* crossed) {
  const f64 before = time;
  time += dt * rate;
  if (stopping_) return; // a cancelled strike cannot land during its fade-out
  const std::vector<ActionEvent>& evs = def->events;
  if (evs.empty()) return;
  const f64 d = def->duration;
  auto local = [&](f64 t) { return def->loop ? std::fmod(t, d) : t; };
  for (i32 i = 0; i < i32(evs.size()); ++i) {
    const ActionEvent& e = evs[size_t(i)];
    // one-shots: each event once; loops: each cycle
    const f64 a = local(before), b = local(time);
    const bool hit = b >= a ? e.t > a && e.t <= b : e.t > a || e.t <= b;
    if (hit && (def->loop || i > fired_to_)) {
      if (crossed) crossed->push_back(&e);
      if (!def->loop) fired_to_ = i;
    }
  }
}

ChannelFrame& ActionPlayer::sample(ChannelFrame& out) const {
  const f64 t = def->loop ? std::fmod(time, def->duration) : std::min(time, def->duration);
  const u32 driven = def->driven();
  for (int i = 0; i < kChannelCount; ++i) {
    if (!((driven >> i) & 1u)) continue;
    std::array<f64, 3>& v = out.v[size_t(i)];
    v = {0.0, 0.0, 0.0};
    def->track(Channel(i)).sample(t, v.data());
    out.driven |= 1u << i;
  }
  return out;
}

// ---- the library ------------------------------------------------------------------------------

namespace {

// (keys as the tables write them: k(time, value or vector, ease))
constexpr Ease snap = Ease::Snap;
constexpr Ease out = Ease::Out;
constexpr Ease inout = Ease::InOut;

Key k(f64 t, f64 v, Ease e = Ease::Smooth) { return Key(t, v, e); }
Key k(f64 t, std::initializer_list<f64> v, Ease e = Ease::Smooth) { return Key(t, v, e); }
Key k(f64 t, const V3& v, Ease e = Ease::Smooth) { return Key(t, std::vector<f64>{v.x, v.y, v.z}, e); }

constexpr V3 kFistUpR{65, 80, 10};
constexpr V3 kFistUpL{65, -80, -10};
constexpr V3 kPoleR{0.45, -0.1, -1};
constexpr V3 kPoleL{-0.45, -0.1, -1};

// The fighting guard's hands from t0 to t1.
void guard_hands(ActionDef& a, f64 t0, f64 t1) {
  a.set(C::HandR, {k(t0, kGuardR), k(t1, kGuardR)});
  a.set(C::HandL, {k(t0, kGuardL), k(t1, kGuardL)});
  a.set(C::HandRrot, {k(t0, kFistUpR), k(t1, kFistUpR)});
  a.set(C::HandLrot, {k(t0, kFistUpL), k(t1, kFistUpL)});
  a.set(C::ElbowR, {k(t0, kPoleR), k(t1, kPoleR)});
  a.set(C::ElbowL, {k(t0, kPoleL), k(t1, kPoleL)});
}

// The actions (a channel set twice keeps the later keys). (A block's `a` is used before the next
// def() only: the list may grow.)
std::vector<ActionDef> build() {
  std::vector<ActionDef> list;
  list.reserve(44);
  auto def = [&list](const char* name, f64 duration, f64 fade_in, f64 fade_out) -> ActionDef& {
    list.emplace_back();
    ActionDef& a = list.back();
    a.name = name;
    a.duration = duration;
    a.fade_in = fade_in;
    a.fade_out = fade_out;
    return a;
  };
  // ---- fighting
  {
    ActionDef& a = def("guard", 1.2, 0.2, 0.25);
    a.lead_side = -1;
    a.layer = ActionLayer::Pose;
    a.loop = true;
    guard_hands(a, 0, 1.2);
    // light bounce on the balls of the feet, chin down, bladed
    a.set(C::Crouch, {k(0, 0.18), k(0.3, 0.24), k(0.6, 0.18), k(0.9, 0.24), k(1.2, 0.18)});
    a.set(C::PelvisRot, {k(0, {0, 0, -22}), k(1.2, {0, 0, -22})});
    a.set(C::Chest, {k(0, {-6, 0, -8}), k(1.2, {-6, 0, -8})});
    a.set(C::Head, {k(0, {-10, 0, 20}), k(1.2, {-10, 0, 20})});
  }
  {
    // a knife fighter's guard: the blade low and forward, the free hand up in front
    ActionDef& a = def("knifeGuard", 1.4, 0.2, 0.25);
    a.lead_side = -1;
    a.requires_tags = {"short_blade"};
    a.layer = ActionLayer::Pose;
    a.loop = true;
    a.set(C::HandR, {k(0, {0.17, 0.28, 0.02}), k(0.7, {0.19, 0.3, 0.05}), k(1.4, {0.17, 0.28, 0.02})});
    a.set(C::HandRrot, {k(0, {0, 90, 0}), k(1.4, {0, 90, 0})});
    a.set(C::ElbowR, {k(0, {0.7, -0.3, -0.7}), k(1.4, {0.7, -0.3, -0.7})});
    a.set(C::HandL, {k(0, {-0.12, 0.3, 0.24}), k(0.7, {-0.14, 0.32, 0.22}), k(1.4, {-0.12, 0.3, 0.24})});
    a.set(C::HandLrot, {k(0, kFistUpL), k(1.4, kFistUpL)});
    a.set(C::ElbowL, {k(0, kPoleL), k(1.4, kPoleL)});
    a.set(C::Crouch, {k(0, 0.24), k(0.35, 0.3), k(0.7, 0.24), k(1.05, 0.3), k(1.4, 0.24)});
    a.set(C::PelvisRot, {k(0, {0, 0, -18}), k(1.4, {0, 0, -18})});
    a.set(C::Chest, {k(0, {-8, 0, -6}), k(1.4, {-8, 0, -6})});
    a.set(C::Head, {k(0, {-8, 0, 16}), k(1.4, {-8, 0, 16})});
  }
  {
    // lead-hand jab: fast out, fast back
    ActionDef& a = def("jab", 0.42, 0.04, 0.12);
    a.targeted = true;
    a.lead_side = -1;
    a.reach = 0.7;
    guard_hands(a, 0, 0.42);
    a.set(C::StrikeL, {k(0, 0), k(0.11, 1, inout), k(0.16, 1), k(0.38, 0, out)});
    a.set(C::HandLrot, {k(0, kFistUpL), k(0.1, {0, 0, 0}, inout), k(0.17, {0, 0, 0}), k(0.38, kFistUpL, out)});
    a.set(C::ElbowL, {k(0, kPoleL), k(0.1, {-0.8, -0.2, -0.6}), k(0.38, kPoleL)});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.1, {-4, 0, -16}, inout), k(0.4, {0, 0, 0}, out)});
    a.set(C::Pelvis, {k(0, {0, 0, 0}), k(0.11, {0, 0.06, -0.01}, inout), k(0.4, {0, 0, 0}, out)});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.1, {-4, 0, 8}), k(0.4, {0, 0, 0})});
    a.events = {{0.11, "strike", Limb::HandL}};
  }
  {
    // rear-hand cross: the hip turns it over
    ActionDef& a = def("cross", 0.55, 0.05, 0.15);
    a.targeted = true;
    a.lead_side = -1;
    a.reach = 0.68;
    guard_hands(a, 0, 0.55);
    a.set(C::StrikeR, {k(0, 0), k(0.06, -0.1), k(0.17, 1, inout), k(0.22, 1), k(0.5, 0, out)});
    a.set(C::HandRrot, {k(0, kFistUpR), k(0.16, {0, 0, 0}, inout), k(0.23, {0, 0, 0}), k(0.5, kFistUpR, out)});
    a.set(C::ElbowR, {k(0, kPoleR), k(0.16, {0.8, -0.2, -0.5}), k(0.5, kPoleR)});
    a.set(C::PelvisRot, {k(0, {0, 0, 0}), k(0.06, {0, 0, -6}), k(0.17, {0, 0, 24}, inout), k(0.52, {0, 0, 0}, out)});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.06, {0, 0, -6}), k(0.17, {-8, 0, 28}, inout), k(0.52, {0, 0, 0}, out)});
    a.set(C::Pelvis, {k(0, {0, 0, 0}), k(0.17, {0.02, 0.1, -0.03}, inout), k(0.52, {0, 0, 0}, out)});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.17, {-6, 0, -18}), k(0.52, {0, 0, 0})});
    a.events = {{0.17, "strike", Limb::HandR}};
  }
  {
    // lead hook: elbow up, fist sweeps round from the side
    ActionDef& a = def("hook", 0.6, 0.05, 0.15);
    a.targeted = true;
    a.lead_side = -1;
    a.reach = 0.58;
    guard_hands(a, 0, 0.6);
    a.set(C::HandL, {k(0, kGuardL), k(0.1, {-0.4, 0.18, 0.3}), k(0.2, {-0.25, 0.42, 0.34}), k(0.55, kGuardL, out)});
    a.set(C::StrikeL, {k(0, 0), k(0.1, 0.1), k(0.21, 0.95, inout), k(0.27, 0.9), k(0.55, 0, out)});
    a.set(C::HandLrot, {k(0, kFistUpL), k(0.12, {0, -90, -40}), k(0.21, {0, -90, -70}), k(0.55, kFistUpL, out)});
    a.set(C::ElbowL, {k(0, kPoleL), k(0.12, {-1, -0.1, 0.4}), k(0.21, {-0.6, 0.4, 0.6}), k(0.55, kPoleL)});
    a.set(C::PelvisRot, {k(0, {0, 0, 0}), k(0.1, {0, 0, 10}), k(0.21, {0, 0, -26}, inout), k(0.58, {0, 0, 0}, out)});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.1, {0, -4, 12}), k(0.21, {-6, 6, -34}, inout), k(0.58, {0, 0, 0}, out)});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.21, {0, 0, 16}), k(0.58, {0, 0, 0})});
    a.events = {{0.21, "strike", Limb::HandL}};
  }
  {
    // rear uppercut: dip, then drive up through the target
    ActionDef& a = def("uppercut", 0.65, 0.05, 0.15);
    a.targeted = true;
    a.lead_side = -1;
    a.reach = 0.58;
    guard_hands(a, 0, 0.65);
    a.set(C::HandR, {k(0, kGuardR), k(0.12, {0.12, 0.2, 0.02}), k(0.24, {0.05, 0.36, 0.3}), k(0.6, kGuardR, out)});
    a.set(C::StrikeR, {k(0, 0), k(0.12, 0), k(0.24, 1, inout), k(0.3, 0.9), k(0.6, 0, out)});
    a.set(C::HandRrot, {k(0, kFistUpR), k(0.12, {40, 90, 0}), k(0.24, {80, 90, 0}), k(0.6, kFistUpR)});
    a.set(C::ElbowR, {k(0, kPoleR), k(0.12, {0.3, -0.6, -0.8}), k(0.24, {0.3, 0.2, -1}), k(0.6, kPoleR)});
    a.set(C::Crouch, {k(0, 0), k(0.12, 0.25, out), k(0.24, -0.05, inout), k(0.62, 0, out)});
    a.set(C::PelvisRot, {k(0, {0, 0, 0}), k(0.12, {0, 0, -8}), k(0.24, {0, 0, 22}, inout), k(0.62, {0, 0, 0})});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.12, {-14, 6, -10}), k(0.24, {6, 0, 24}, inout), k(0.62, {0, 0, 0}, out)});
    a.events = {{0.24, "strike", Limb::HandR}};
  }
  {
    // rear-leg front (push) kick: chamber, extend, retract, plant
    ActionDef& a = def("frontKick", 0.85, 0.08, 0.15);
    a.targeted = true;
    a.lead_side = -1;
    a.reach = 0.8;
    a.kick_pitch = 0.35;
    a.target_height = 1.05;
    a.support_turn = 0.25;
    guard_hands(a, 0, 0.85);
    a.set(C::FootRw, {k(0, 0), k(0.08, 1), k(0.72, 1), k(0.85, 0)});
    a.set(C::FootR, {k(0, {0.12, -0.05, 0.09}), k(0.22, {0.1, 0.28, 0.5}, out), k(0.5, {0.1, 0.3, 0.52}), k(0.8, {0.12, 0.02, 0.09}, inout)});
    a.set(C::StrikeFootR, {k(0, 0), k(0.22, 0), k(0.33, 1, inout), k(0.4, 1), k(0.52, 0, out)});
    a.set(C::Spine, {k(0, {0, 0, 0}), k(0.33, {18, 0, 0}, inout), k(0.8, {0, 0, 0})});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.33, {10, 0, 0}), k(0.8, {0, 0, 0})});
    a.set(C::Pelvis, {k(0, {0, 0, 0}), k(0.2, {-0.03, -0.02, 0.03}), k(0.33, {-0.04, -0.06, 0.04}), k(0.8, {0, 0, 0})});
    a.set(C::HandR, {k(0, kGuardR), k(0.33, {0.24, 0.05, 0.1}), k(0.7, kGuardR)});
    a.events = {{0.33, "strike", Limb::FootR}};
  }
  {
    // rear-leg roundhouse: the hip turns over, the shin sweeps round
    ActionDef& a = def("roundhouse", 0.95, 0.08, 0.15);
    a.targeted = true;
    a.lead_side = -1;
    a.reach = 0.78;
    a.kick_pitch = -0.65;
    a.target_height = 1.1;
    a.support_turn = 0.8;
    a.set(C::FootRpole, {k(0, {0.4, 1, 0.8}), k(0.22, {1, 0.15, 0.4}), k(0.45, {1, 0.1, 0.25}), k(0.9, {0.4, 1, 0.8})});
    guard_hands(a, 0, 0.95);
    a.set(C::FootRw, {k(0, 0), k(0.08, 1), k(0.8, 1), k(0.95, 0)});
    a.set(C::FootR, {k(0, {0.12, -0.05, 0.09}), k(0.22, {0.42, 0.05, 0.55}, out), k(0.38, {0.2, 0.5, 0.8}), k(0.6, {0.3, 0.1, 0.45}), k(0.9, {0.12, 0.02, 0.09}, inout)});
    a.set(C::StrikeFootR, {k(0, 0), k(0.25, 0.1), k(0.38, 0.95, inout), k(0.45, 0.8), k(0.6, 0, out)});
    a.set(C::PelvisRot, {k(0, {0, 0, 0}), k(0.25, {0, -10, 35}), k(0.38, {0, -15, 60}, inout), k(0.9, {0, 0, 0}, inout)});
    a.set(C::Spine, {k(0, {0, 0, 0}), k(0.38, {4, -8, -20}), k(0.9, {0, 0, 0})});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.38, {0, -3, -30}), k(0.9, {0, 0, 0})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.38, {0, 5, -10}), k(0.9, {0, 0, 0})});
    // Counterbalance around the outside of the shoulder. A straight path from
    // the chin to behind the body passed through the shoulder's IK singularity.
    a.set(C::HandR, {k(0, kGuardR), k(0.16, {0.42, 0.22, 0.12}), k(0.38, {0.46, -0.12, -0.12}), k(0.64, {0.42, 0.22, 0.12}), k(0.85, kGuardR)});
    a.events = {{0.38, "strike", Limb::FootR}};
  }
  {
    // knife thrust
    ActionDef& a = def("stab", 0.62, 0.06, 0.15);
    a.targeted = true;
    a.lead_side = -1;
    a.reach = 0.66;
    a.requires_tags = {"short_blade"};
    a.target_height = 1.2;
    a.set(C::HandR, {k(0, {0.2, 0.2, 0.0}), k(0.16, {0.25, 0.25, 0.1}, inout), k(0.6, {0.2, 0.2, 0.0})});
    a.set(C::HandRrot, {k(0, {0, 90, 0}), k(0.6, {0, 90, 0})});
    a.set(C::StrikeR, {k(0, 0), k(0.16, 0), k(0.28, 1, inout), k(0.34, 1), k(0.58, 0, out)});
    a.set(C::ElbowR, {k(0, {0.6, -0.4, -0.8}), k(0.6, {0.6, -0.4, -0.8})});
    a.set(C::HandL, {k(0, {-0.18, 0.28, 0.12}), k(0.28, {-0.25, 0.1, 0.05}), k(0.6, {-0.18, 0.28, 0.12})});
    a.set(C::HandLw, {k(0, 0.7), k(0.6, 0.7)});
    a.set(C::PelvisRot, {k(0, {0, 0, 0}), k(0.16, {0, 0, -10}), k(0.28, {0, 0, 18}, inout), k(0.6, {0, 0, 0})});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.16, {2, 0, -12}), k(0.28, {-12, 0, 22}, inout), k(0.6, {0, 0, 0}, out)});
    a.set(C::Pelvis, {k(0, {0, 0, 0}), k(0.28, {0, 0.12, -0.04}, inout), k(0.6, {0, 0, 0}, out)});
    a.set(C::Crouch, {k(0, 0.1), k(0.28, 0.2), k(0.6, 0.1)});
    a.events = {{0.28, "strike", Limb::Blade}};
  }
  {
    // knife slash: a backhand arc across the target
    ActionDef& a = def("slash", 0.62, 0.06, 0.15);
    a.targeted = true;
    a.lead_side = -1;
    a.reach = 0.62;
    a.requires_tags = {"short_blade"};
    a.target_height = 1.2;
    a.set(C::HandR, {k(0, {0.2, 0.2, 0.0}), k(0.14, {0.46, 0.3, 0.22}, inout), k(0.3, {-0.12, 0.46, 0.05}, inout), k(0.6, {0.2, 0.2, 0.0}, inout)});
    a.set(C::StrikeR, {k(0, 0), k(0.14, 0), k(0.22, 0.75, inout), k(0.3, 0.3), k(0.6, 0)});
    a.set(C::HandRrot, {k(0, {0, 90, 0}), k(0.14, {15, 100, 25}), k(0.3, {0, 100, -35}, inout), k(0.6, {0, 90, 0})});
    a.set(C::ElbowR, {k(0, {0.8, -0.4, -0.5}), k(0.16, {1, -0.2, 0.1}), k(0.36, {0.8, -0.2, -0.4}), k(0.6, {0.8, -0.4, -0.5})});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.14, {0, 6, -26}), k(0.3, {-6, -6, 30}, inout), k(0.6, {0, 0, 0})});
    a.set(C::PelvisRot, {k(0, {0, 0, 0}), k(0.14, {0, 0, -12}), k(0.3, {0, 0, 16}, inout), k(0.6, {0, 0, 0})});
    a.set(C::Pelvis, {k(0, {0, 0, 0}), k(0.22, {0, 0.08, -0.02}), k(0.6, {0, 0, 0})});
    a.events = {{0.22, "strike", Limb::Blade}};
  }
  {
    // underhand thrust into the gut: low from the hip, driving up, the free hand pulling
    ActionDef& a = def("gutStab", 0.72, 0.06, 0.15);
    a.targeted = true;
    a.lead_side = -1;
    a.reach = 0.62;
    a.requires_tags = {"short_blade"};
    a.target_height = 1.0;
    a.set(C::HandR, {k(0, {0.18, 0.12, -0.12}), k(0.2, {0.2, -0.02, -0.2}, out), k(0.7, {0.18, 0.12, -0.12})});
    a.set(C::HandRrot, {k(0, {-40, 90, 0}), k(0.7, {-40, 90, 0})});
    a.set(C::StrikeR, {k(0, 0), k(0.2, 0), k(0.32, 1, inout), k(0.4, 1), k(0.66, 0, out)});
    a.set(C::ElbowR, {k(0, {0.7, -0.5, -0.6}), k(0.7, {0.7, -0.5, -0.6})});
    a.set(C::HandL, {k(0, {-0.16, 0.3, 0.16}), k(0.32, {-0.12, 0.44, 0.1}), k(0.7, {-0.16, 0.3, 0.16})});
    a.set(C::HandLw, {k(0, 0.7), k(0.7, 0.7)});
    a.set(C::PelvisRot, {k(0, {0, 0, 0}), k(0.2, {0, 0, -8}), k(0.32, {0, 0, 14}, inout), k(0.7, {0, 0, 0})});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.2, {6, 0, -8}), k(0.32, {-16, 0, 16}, inout), k(0.7, {0, 0, 0}, out)});
    a.set(C::Pelvis, {k(0, {0, 0, 0}), k(0.32, {0, 0.14, -0.06}, inout), k(0.7, {0, 0, 0}, out)});
    a.set(C::Crouch, {k(0, 0.15), k(0.32, 0.32), k(0.7, 0.15)});
    a.events = {{0.36, "strike", Limb::Blade}};
  }
  {
    // forehand slash: from high on the left across and down
    ActionDef& a = def("forehandSlash", 0.66, 0.06, 0.15);
    a.targeted = true;
    a.lead_side = -1;
    a.reach = 0.62;
    a.requires_tags = {"short_blade"};
    a.target_height = 1.2;
    a.set(C::HandR, {k(0, {0.2, 0.2, 0.0}), k(0.16, {-0.08, 0.4, 0.28}, inout), k(0.25, {0.12, 0.48, 0.14}, inout), k(0.34, {0.4, 0.36, -0.08}), k(0.64, {0.2, 0.2, 0.0}, inout)});
    a.set(C::StrikeR, {k(0, 0), k(0.16, 0), k(0.25, 0.9, inout), k(0.32, 0.3), k(0.64, 0)});
    a.set(C::HandRrot, {k(0, {0, 90, 0}), k(0.16, {40, 60, 60}), k(0.25, {0, 90, 42}, inout), k(0.34, {-20, 120, -40}), k(0.64, {0, 90, 0})});
    a.set(C::ElbowR, {k(0, {0.8, -0.4, -0.5}), k(0.16, {1, -0.2, 0.1}), k(0.36, {0.8, -0.2, -0.4}), k(0.6, {0.8, -0.4, -0.5})});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.16, {0, -6, 24}), k(0.32, {-8, 6, -28}, inout), k(0.64, {0, 0, 0})});
    a.set(C::PelvisRot, {k(0, {0, 0, 0}), k(0.16, {0, 0, 12}), k(0.32, {0, 0, -16}, inout), k(0.64, {0, 0, 0})});
    a.set(C::Pelvis, {k(0, {0, 0, 0}), k(0.25, {0, 0.08, -0.02}), k(0.64, {0, 0, 0})});
    a.events = {{0.25, "strike", Limb::Blade}};
  }
  {
    // shove / rifle jab: both hands (or the weapon) driven forward
    ActionDef& a = def("riflePush", 0.55, 0.05, 0.15);
    a.requires_tags = {"long_firearm"};
    a.set(C::StrikeR, {k(0, 0), k(.1, 0), k(.2, 1), k(.3, .6), k(.5, 0)});
    a.targeted = true;
    a.lead_side = -1;
    a.reach = 0.7;
    a.set(C::WeaponPos, {k(0, {0, 0, 0}), k(0.1, {0, -0.24, -0.04}), k(0.2, {0, 0.32, -0.1}, inout), k(0.26, {0, 0.3, -0.1}), k(0.52, {0, 0, 0}, out)});
    a.set(C::WeaponRot, {k(0, {0, 0, 0}), k(0.2, {-8, 0, 0}), k(0.52, {0, 0, 0})});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.1, {4, 0, -8}), k(0.2, {-10, 0, 6}, inout), k(0.52, {0, 0, 0})});
    a.set(C::Pelvis, {k(0, {0, 0, 0}), k(0.2, {0, 0.12, -0.02}, inout), k(0.52, {0, 0, 0}, out)});
    a.events = {{0.2, "strike", Limb::Muzzle}};
  }
  {
    // forearms up in front of the face
    ActionDef& a = def("block", 0.9, 0.06, 0.2);
    a.set(C::HandR, {k(0, {0.08, 0.26, 0.42}), k(0.9, {0.08, 0.26, 0.42})});
    a.set(C::HandL, {k(0, {-0.08, 0.28, 0.44}), k(0.9, {-0.08, 0.28, 0.44})});
    a.set(C::HandRrot, {k(0, {80, 90, 20}), k(0.9, {80, 90, 20})});
    a.set(C::HandLrot, {k(0, {80, -90, -20}), k(0.9, {80, -90, -20})});
    a.set(C::ElbowR, {k(0, {0.3, 0.6, -0.7}), k(0.9, {0.3, 0.6, -0.7})});
    a.set(C::ElbowL, {k(0, {-0.3, 0.6, -0.7}), k(0.9, {-0.3, 0.6, -0.7})});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.1, {-10, 0, 0}), k(0.9, {-8, 0, 0})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.1, {-18, 0, 0}), k(0.9, {-14, 0, 0})});
    a.set(C::Crouch, {k(0, 0), k(0.1, 0.25), k(0.9, 0.2)});
  }
  // ---- weapons
  {
    ActionDef& a = def("reloadRifle", 2.3, 0.15, 0.25);
    a.set(C::WeaponRot, {k(0, {0, 0, 0}), k(0.3, {-12, 28, 8}), k(1.9, {-12, 28, 8}), k(2.2, {0, 0, 0})});
    a.set(C::WeaponPos, {k(0, {0, 0, 0}), k(0.3, {0.02, -0.06, -0.04}), k(1.9, {0.02, -0.06, -0.04}), k(2.2, {0, 0, 0})});
    a.set(C::HandL, {k(0.2, {0.02, 0.36, -0.05}), k(0.45, {0.04, 0.3, -0.12}), k(0.8, {-0.06, 0.16, -0.12}, inout), k(1.1, {-0.06, 0.16, -0.12}), k(1.4, {0.04, 0.3, -0.1}, inout), k(1.55, {0.04, 0.3, -0.07}, snap), k(1.8, {0.1, 0.28, 0.08}), k(1.95, {0.12, 0.2, 0.08}, snap)});
    a.set(C::HandLw, {k(0, 0), k(0.2, 1), k(2.05, 1), k(2.3, 0)});
    a.set(C::HandLrot, {k(0.2, {0, -90, 0}), k(0.8, {-40, -90, 0}), k(1.4, {0, -90, 0}), k(1.8, {0, -90, 40})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.3, {-20, 8, 6}), k(1.9, {-18, 8, 6}), k(2.2, {0, 0, 0})});
    a.set(C::Look, {k(0, 1), k(0.3, 0.2), k(1.9, 0.2), k(2.2, 1)});
    a.events = {{2.0, "reloaded"}};
  }
  {
    ActionDef& a = def("reloadPistol", 1.6, 0.1, 0.2);
    a.set(C::WeaponRot, {k(0, {0, 0, 0}), k(0.25, {30, 20, 0}), k(1.3, {30, 20, 0}), k(1.55, {0, 0, 0})});
    a.set(C::WeaponPos, {k(0, {0, 0, 0}), k(0.25, {0, -0.18, -0.1}), k(1.3, {0, -0.18, -0.1}), k(1.55, {0, 0, 0})});
    a.set(C::HandL, {k(0.15, {-0.02, 0.28, 0.05}), k(0.45, {-0.14, 0.12, -0.22}, inout), k(0.7, {-0.14, 0.12, -0.22}), k(1.0, {0.02, 0.26, 0.0}, inout), k(1.1, {0.02, 0.26, 0.03}, snap)});
    a.set(C::HandLw, {k(0, 0), k(0.15, 1), k(1.3, 1), k(1.55, 0)});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.25, {-22, 0, 4}), k(1.3, {-22, 0, 4}), k(1.55, {0, 0, 0})});
    a.set(C::Look, {k(0, 1), k(0.25, 0.2), k(1.3, 0.2), k(1.55, 1)});
    a.events = {{1.3, "reloaded"}};
  }
  // ---- a soldier's pauses
  {
    // the weapon lowered a moment: a deep breath, the shoulders rising and settling, a glance down
    ActionDef& a = def("catchBreath", 2.6, 0.4, 0.5);
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.7, {5, 0, 0}), k(1.2, {-3, 0, 0}, inout), k(1.8, {4, 0, 0}), k(2.3, {-2, 0, 0}), k(2.6, {0, 0, 0})});
    a.set(C::Spine, {k(0, {0, 0, 0}), k(1.2, {-4, 0, 0}), k(2.6, {0, 0, 0})});
    a.set(C::ClavR, {k(0, {0, 0, 0}), k(0.7, {0, -7, 0}), k(1.2, {0, 2, 0}), k(1.8, {0, -5, 0}), k(2.6, {0, 0, 0})});
    a.set(C::ClavL, {k(0, {0, 0, 0}), k(0.7, {0, 7, 0}), k(1.2, {0, -2, 0}), k(1.8, {0, 5, 0}), k(2.6, {0, 0, 0})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.7, {10, 0, 0}), k(1.3, {-16, 0, 6}), k(2.0, {-10, 0, -4}), k(2.6, {0, 0, 0})});
    a.set(C::Look, {k(0, 1), k(0.5, 0.3), k(2.1, 0.3), k(2.6, 1)});
  }
  {
    // a look round: the eyes and the trunk sweep one way, then the other
    ActionDef& a = def("lookAround", 2.8, 0.3, 0.4);
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.6, {4, 0, 50}), k(1.1, {4, 0, 50}), k(1.8, {2, 0, -45}), k(2.3, {2, 0, -45}), k(2.8, {0, 0, 0})});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.6, {0, 0, 14}), k(1.1, {0, 0, 14}), k(1.8, {0, 0, -12}), k(2.3, {0, 0, -12}), k(2.8, {0, 0, 0})});
    a.set(C::Look, {k(0, 1), k(0.4, 0), k(2.4, 0), k(2.8, 1)});
  }
  {
    // a hand to the helmet (or the head), setting it straight
    ActionDef& a = def("adjustHelmet", 1.8, 0.3, 0.35);
    a.set(C::HandL, {k(0, {-0.12, 0.06, 0.52}), k(0.7, {-0.1, 0.02, 0.55}), k(1.0, {-0.13, 0.06, 0.53}), k(1.8, {-0.12, 0.06, 0.52})});
    a.set(C::HandLw, {k(0, 1), k(1.8, 1)});
    a.set(C::HandLrot, {k(0, {130, -90, -60}), k(1.8, {130, -90, -60})});
    a.set(C::ElbowL, {k(0, {-1, 0.3, 0.3}), k(1.8, {-1, 0.3, 0.3})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.5, {-8, 4, 4}), k(1.3, {-8, 4, 4}), k(1.8, {0, 0, 0})});
  }
  {
    // the back of the hand across the brow
    ActionDef& a = def("wipeBrow", 1.5, 0.25, 0.3);
    a.set(C::HandL, {k(0, {-0.12, 0.18, 0.42}), k(0.55, {-0.1, 0.19, 0.45}), k(0.9, {0.08, 0.19, 0.45}, inout), k(1.5, {0.08, 0.19, 0.45})});
    a.set(C::HandLw, {k(0, 1), k(1.5, 1)});
    a.set(C::HandLrot, {k(0, {90, -90, 0}), k(1.5, {90, -90, 0})});
    a.set(C::ElbowL, {k(0, {-1, 0.4, -0.3}), k(1.5, {-1, 0.4, -0.3})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.5, {-10, 0, 0}), k(1.1, {-10, 0, 0}), k(1.5, {0, 0, 0})});
  }
  {
    // shoulders rolled and the neck stretched
    ActionDef& a = def("rollShoulders", 1.8, 0.25, 0.3);
    a.set(C::ClavR, {k(0, {0, 0, 0}), k(0.4, {0, -10, 6}), k(0.8, {0, 4, -6}), k(1.2, {0, -6, 0}), k(1.8, {0, 0, 0})});
    a.set(C::ClavL, {k(0, {0, 0, 0}), k(0.4, {0, 10, 6}), k(0.8, {0, -4, -6}), k(1.2, {0, 6, 0}), k(1.8, {0, 0, 0})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.5, {0, 20, 0}), k(1.0, {0, -20, 0}), k(1.5, {6, 0, 0}), k(1.8, {0, 0, 0})});
    a.set(C::Look, {k(0, 1), k(0.3, 0.2), k(1.5, 0.2), k(1.8, 1)});
  }
  {
    // a glance at the weapon: turned a little and looked over
    ActionDef& a = def("checkWeapon", 2.0, 0.3, 0.35);
    a.set(C::WeaponRot, {k(0, {0, 0, 0}), k(0.5, {10, 25, -20}), k(1.5, {10, 25, -20}), k(2.0, {0, 0, 0})});
    a.set(C::WeaponPos, {k(0, {0, 0, 0}), k(0.5, {-0.04, 0.02, 0.06}), k(1.5, {-0.04, 0.02, 0.06}), k(2.0, {0, 0, 0})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.5, {-25, 0, 10}), k(1.5, {-25, 0, 10}), k(2.0, {0, 0, 0})});
    a.set(C::Look, {k(0, 1), k(0.4, 0), k(1.6, 0), k(2.0, 1)});
  }
  // ---- balance and reflexes
  {
    // flinching from a round close by: the head ducks, the shoulders come up, the knees dip
    ActionDef& a = def("flinch", 0.7, 0.04, 0.3);
    a.set(C::Crouch, {k(0, 0), k(0.1, 0.22, snap), k(0.35, 0.18), k(0.7, 0, out)});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.1, {-22, 0, 8}, snap), k(0.4, {-18, 0, 6}), k(0.7, {0, 0, 0})});
    a.set(C::Neck, {k(0, {0, 0, 0}), k(0.1, {-8, 0, 0}, snap), k(0.7, {0, 0, 0})});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.1, {-8, 0, 0}, snap), k(0.7, {0, 0, 0})});
    a.set(C::ClavR, {k(0, {0, 0, 0}), k(0.1, {0, -12, 0}, snap), k(0.7, {0, 0, 0})});
    a.set(C::ClavL, {k(0, {0, 0, 0}), k(0.1, {0, 12, 0}, snap), k(0.7, {0, 0, 0})});
    a.set(C::HandL, {k(0, {-0.1, 0.24, 0.35}), k(0.1, {-0.06, 0.2, 0.4}, snap), k(0.7, {-0.06, 0.2, 0.4})});
    a.set(C::HandLw, {k(0, 0), k(0.1, 0.7, snap), k(0.45, 0.6), k(0.7, 0)});
    a.set(C::Look, {k(0, 1), k(0.08, 0), k(0.5, 0), k(0.7, 1)});
  }
  {
    // thrown off balance (a blast close by, a shove): the arms go out, the knees give, the trunk
    // catches up; the push itself moves the body and the feet stumble after it
    ActionDef& a = def("stumble", 1.3, 0.05, 0.4);
    a.set(C::Crouch, {k(0, 0), k(0.15, 0.3, snap), k(0.6, 0.2), k(1.3, 0, out)});
    a.set(C::HandL, {k(0, {-0.3, 0.15, 0.0}), k(0.15, {-0.5, 0.15, 0.05}, snap), k(0.7, {-0.42, 0.2, 0.0}), k(1.3, {-0.3, 0.15, -0.1})});
    a.set(C::HandLw, {k(0, 0), k(0.12, 1, snap), k(0.9, 0.8), k(1.3, 0)});
    a.set(C::HandR, {k(0, {0.3, 0.15, 0.0}), k(0.15, {0.5, 0.15, 0.05}, snap), k(0.7, {0.42, 0.2, 0.0}), k(1.3, {0.3, 0.15, -0.1})});
    a.set(C::HandRw, {k(0, 0), k(0.12, 1, snap), k(0.9, 0.8), k(1.3, 0)});
    a.set(C::HandLrot, {k(0, {0, -90, -80}), k(1.3, {0, -90, -80})});
    a.set(C::HandRrot, {k(0, {0, 90, 80}), k(1.3, {0, 90, 80})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.15, {-15, 0, 0}, snap), k(0.6, {5, 0, 0}), k(1.3, {0, 0, 0})});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.2, {-10, 0, 0}), k(0.7, {4, 0, 0}), k(1.3, {0, 0, 0})});
    a.set(C::Look, {k(0, 1), k(0.1, 0.2), k(1.0, 0.4), k(1.3, 1)});
  }
  {
    // a foot caught: the body pitches forward, the arms go out in front, quick steps catch it
    ActionDef& a = def("trip", 1.2, 0.05, 0.35);
    a.set(C::Spine, {k(0, {0, 0, 0}), k(0.25, {-22, 0, 0}), k(0.55, {-16, 0, 0}), k(1.2, {0, 0, 0}, out)});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.25, {-14, 0, 0}), k(0.55, {-8, 0, 0}), k(1.2, {0, 0, 0}, out)});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.25, {22, 0, 0}), k(0.8, {8, 0, 0}), k(1.2, {0, 0, 0})});
    a.set(C::Crouch, {k(0, 0), k(0.3, 0.3), k(0.7, 0.15), k(1.2, 0, out)});
    a.set(C::HandL, {k(0, {-0.2, 0.35, 0.0}), k(0.25, {-0.22, 0.5, -0.05}), k(0.7, {-0.25, 0.4, -0.05}), k(1.2, {-0.2, 0.2, -0.2})});
    a.set(C::HandLw, {k(0, 0), k(0.15, 1), k(0.8, 0.8), k(1.2, 0)});
    a.set(C::HandR, {k(0, {0.2, 0.35, 0.0}), k(0.25, {0.22, 0.5, -0.05}), k(0.7, {0.25, 0.4, -0.05}), k(1.2, {0.2, 0.2, -0.2})});
    a.set(C::HandRw, {k(0, 0), k(0.15, 1), k(0.8, 0.8), k(1.2, 0)});
    a.set(C::Look, {k(0, 1), k(0.1, 0.3), k(0.9, 0.5), k(1.2, 1)});
  }
  // ---- idle poses (held)
  {
    ActionDef& a = def("armsCrossed", 4, 0.5, 0.5);
    a.layer = ActionLayer::Pose;
    a.loop = true;
    a.set(C::HandR, {k(0, {-0.1, 0.17, 0.05}), k(2, {-0.1, 0.17, 0.06}), k(4, {-0.1, 0.17, 0.05})});
    a.set(C::HandL, {k(0, {0.11, 0.15, 0.1}), k(2, {0.11, 0.15, 0.11}), k(4, {0.11, 0.15, 0.1})});
    a.set(C::HandRrot, {k(0, {0, 90, 80}), k(4, {0, 90, 80})});
    a.set(C::HandLrot, {k(0, {0, -90, -80}), k(4, {0, -90, -80})});
    a.set(C::ElbowR, {k(0, {1, 0.2, -0.3}), k(4, {1, 0.2, -0.3})});
    a.set(C::ElbowL, {k(0, {-1, 0.2, -0.3}), k(4, {-1, 0.2, -0.3})});
    a.set(C::Chest, {k(0, {4, 0, 0}), k(4, {4, 0, 0})});
  }
  {
    ActionDef& a = def("pockets", 4, 0.5, 0.5);
    a.layer = ActionLayer::Pose;
    a.loop = true;
    a.set(C::HandR, {k(0, {0.14, 0.07, -0.31}), k(4, {0.14, 0.07, -0.31})});
    a.set(C::HandL, {k(0, {-0.14, 0.07, -0.31}), k(4, {-0.14, 0.07, -0.31})});
    a.set(C::HandRrot, {k(0, {-80, 90, 0}), k(4, {-80, 90, 0})});
    a.set(C::HandLrot, {k(0, {-80, -90, 0}), k(4, {-80, -90, 0})});
    a.set(C::ElbowR, {k(0, {0.8, -0.5, -0.3}), k(4, {0.8, -0.5, -0.3})});
    a.set(C::ElbowL, {k(0, {-0.8, -0.5, -0.3}), k(4, {-0.8, -0.5, -0.3})});
    a.set(C::Chest, {k(0, {-3, 0, 0}), k(4, {-3, 0, 0})});
  }
  {
    ActionDef& a = def("handsOnHips", 4, 0.5, 0.5);
    a.layer = ActionLayer::Pose;
    a.loop = true;
    a.set(C::HandR, {k(0, {0.2, 0.02, -0.24}), k(4, {0.2, 0.02, -0.24})});
    a.set(C::HandL, {k(0, {-0.2, 0.02, -0.24}), k(4, {-0.2, 0.02, -0.24})});
    a.set(C::HandRrot, {k(0, {-90, 90, 30}), k(4, {-90, 90, 30})});
    a.set(C::HandLrot, {k(0, {-90, -90, -30}), k(4, {-90, -90, -30})});
    a.set(C::ElbowR, {k(0, {1, -0.3, 0}), k(4, {1, -0.3, 0})});
    a.set(C::ElbowL, {k(0, {-1, -0.3, 0}), k(4, {-1, -0.3, 0})});
    a.set(C::Chest, {k(0, {6, 0, 0}), k(4, {6, 0, 0})});
  }
  {
    ActionDef& a = def("handsBehind", 4, 0.5, 0.5);
    a.layer = ActionLayer::Pose;
    a.loop = true;
    a.set(C::HandR, {k(0, {0.03, -0.17, -0.24}), k(4, {0.03, -0.17, -0.24})});
    a.set(C::HandL, {k(0, {-0.03, -0.16, -0.22}), k(4, {-0.03, -0.16, -0.22})});
    a.set(C::HandRrot, {k(0, {-90, 90, 180}), k(4, {-90, 90, 180})});
    a.set(C::HandLrot, {k(0, {-90, -90, 180}), k(4, {-90, -90, 180})});
    a.set(C::ElbowR, {k(0, {0.6, -1, 0}), k(4, {0.6, -1, 0})});
    a.set(C::ElbowL, {k(0, {-0.6, -1, 0}), k(4, {-0.6, -1, 0})});
    a.set(C::Chest, {k(0, {8, 0, 0}), k(4, {8, 0, 0})});
  }
  {
    ActionDef& a = def("handsFolded", 4, 0.5, 0.5);
    a.layer = ActionLayer::Pose;
    a.loop = true;
    a.set(C::HandR, {k(0, {0.03, 0.14, -0.22}), k(4, {0.03, 0.14, -0.22})});
    a.set(C::HandL, {k(0, {-0.03, 0.15, -0.21}), k(4, {-0.03, 0.15, -0.21})});
    a.set(C::HandRrot, {k(0, {-30, 90, 60}), k(4, {-30, 90, 60})});
    a.set(C::HandLrot, {k(0, {-30, -90, -60}), k(4, {-30, -90, -60})});
    a.set(C::ElbowR, {k(0, {0.7, -0.3, -0.7}), k(4, {0.7, -0.3, -0.7})});
    a.set(C::ElbowL, {k(0, {-0.7, -0.3, -0.7}), k(4, {-0.7, -0.3, -0.7})});
  }
  {
    // looking at a phone held in both hands
    ActionDef& a = def("phone", 3, 0.5, 0.5);
    a.layer = ActionLayer::Pose;
    a.loop = true;
    a.set(C::HandR, {k(0, {0.04, 0.3, 0.12}), k(1.5, {0.04, 0.3, 0.13}), k(3, {0.04, 0.3, 0.12})});
    a.set(C::HandL, {k(0, {-0.02, 0.29, 0.1}), k(3, {-0.02, 0.29, 0.1})});
    a.set(C::HandRrot, {k(0, {30, 150, 0}), k(3, {30, 150, 0})});
    a.set(C::HandLrot, {k(0, {30, -150, 0}), k(3, {30, -150, 0})});
    a.set(C::ElbowR, {k(0, {0.6, -0.4, -0.8}), k(3, {0.6, -0.4, -0.8})});
    a.set(C::ElbowL, {k(0, {-0.6, -0.4, -0.8}), k(3, {-0.6, -0.4, -0.8})});
    a.set(C::Neck, {k(0, {-18, 0, 0}), k(3, {-18, 0, 0})});
    a.set(C::Head, {k(0, {-24, 0, 0}), k(3, {-24, 0, 0})});
    a.set(C::Look, {k(0, 0.1), k(3, 0.1)});
  }
  // ---- fidgets (one-shots)
  {
    ActionDef& a = def("checkWatch", 2.2, 0.3, 0.4);
    a.set(C::HandL, {k(0, {-0.1, 0.24, 0.12}), k(2.2, {-0.1, 0.24, 0.12})});
    a.set(C::HandLrot, {k(0, {10, -30, -60}), k(2.2, {10, -30, -60})});
    a.set(C::ElbowL, {k(0, {-0.8, -0.2, -0.6}), k(2.2, {-0.8, -0.2, -0.6})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.4, {-30, 0, 18}), k(1.8, {-30, 0, 18}), k(2.2, {0, 0, 0})});
    a.set(C::Look, {k(0, 1), k(0.3, 0), k(1.9, 0), k(2.2, 1)});
  }
  {
    ActionDef& a = def("scratchHead", 2.4, 0.35, 0.4);
    a.set(C::HandR, {k(0, {0.1, 0.02, 0.52}), k(0.8, {0.08, -0.02, 0.54}), k(1.0, {0.1, 0.02, 0.52}), k(1.2, {0.08, -0.02, 0.54}), k(1.4, {0.1, 0.02, 0.52}), k(2.4, {0.1, 0.02, 0.52})});
    a.set(C::HandRrot, {k(0, {120, 90, 60}), k(2.4, {120, 90, 60})});
    a.set(C::ElbowR, {k(0, {1, 0.3, 0.2}), k(2.4, {1, 0.3, 0.2})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.4, {-6, -10, -6}), k(2.0, {-6, -10, -6}), k(2.4, {0, 0, 0})});
  }
  {
    ActionDef& a = def("stretch", 3, 0.6, 0.6);
    a.set(C::HandR, {k(0, {0.12, 0.05, 0.75}), k(3, {0.12, 0.05, 0.75})});
    a.set(C::HandL, {k(0, {-0.12, 0.05, 0.75}), k(3, {-0.12, 0.05, 0.75})});
    a.set(C::HandRrot, {k(0, {170, 90, 0}), k(3, {170, 90, 0})});
    a.set(C::HandLrot, {k(0, {170, -90, 0}), k(3, {170, -90, 0})});
    a.set(C::ElbowR, {k(0, {1, -0.2, 0.2}), k(3, {1, -0.2, 0.2})});
    a.set(C::ElbowL, {k(0, {-1, -0.2, 0.2}), k(3, {-1, -0.2, 0.2})});
    a.set(C::Spine, {k(0, {0, 0, 0}), k(1.2, {12, 0, 0}), k(1.8, {12, 0, 0}), k(3, {0, 0, 0})});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(1.2, {10, 0, 0}), k(1.8, {10, 0, 0}), k(3, {0, 0, 0})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(1.2, {18, 0, 0}), k(1.8, {18, 0, 0}), k(3, {0, 0, 0})});
    a.set(C::Look, {k(0, 1), k(0.6, 0), k(2.4, 0), k(3, 1)});
  }
  {
    ActionDef& a = def("rubNeck", 2.2, 0.35, 0.4);
    a.set(C::HandR, {k(0, {0.06, -0.08, 0.38}), k(0.9, {0.02, -0.09, 0.4}), k(1.3, {0.07, -0.08, 0.37}), k(2.2, {0.06, -0.08, 0.38})});
    a.set(C::HandRrot, {k(0, {120, 90, 120}), k(2.2, {120, 90, 120})});
    a.set(C::ElbowR, {k(0, {1, 0.4, 0}), k(2.2, {1, 0.4, 0})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.5, {-12, -8, 10}), k(1.2, {-8, 8, -8}), k(2.2, {0, 0, 0})});
  }
  {
    ActionDef& a = def("wave", 1.8, 0.25, 0.35);
    a.set(C::HandR, {k(0, {0.3, 0.12, 0.5}), k(0.5, {0.26, 0.14, 0.52}), k(0.75, {0.36, 0.12, 0.5}), k(1.0, {0.26, 0.14, 0.52}), k(1.25, {0.36, 0.12, 0.5}), k(1.8, {0.3, 0.12, 0.5})});
    a.set(C::HandRrot, {k(0, {160, 0, 0}), k(1.8, {160, 0, 0})});
    a.set(C::ElbowR, {k(0, {1, 0, -0.5}), k(1.8, {1, 0, -0.5})});
  }
  // ---- conversation
  {
    // the speaker's hands moving with the words (looped; the animator adds beats)
    ActionDef& a = def("talk", 2.4, 0.4, 0.5);
    a.layer = ActionLayer::Pose;
    a.loop = true;
    a.set(C::HandR, {k(0, {0.12, 0.2, -0.1}), k(0.6, {0.16, 0.26, -0.04}), k(1.2, {0.1, 0.22, -0.12}), k(1.8, {0.18, 0.28, -0.06}), k(2.4, {0.12, 0.2, -0.1})});
    a.set(C::HandL, {k(0, {-0.12, 0.21, -0.12}), k(0.8, {-0.15, 0.24, -0.08}), k(1.6, {-0.1, 0.2, -0.13}), k(2.4, {-0.12, 0.21, -0.12})});
    a.set(C::HandRrot, {k(0, {10, 130, 0}), k(1.2, {20, 150, 10}), k(2.4, {10, 130, 0})});
    a.set(C::HandLrot, {k(0, {10, -130, 0}), k(1.2, {20, -150, -10}), k(2.4, {10, -130, 0})});
    a.set(C::ElbowR, {k(0, {0.8, -0.4, -0.6}), k(2.4, {0.8, -0.4, -0.6})});
    a.set(C::ElbowL, {k(0, {-0.8, -0.4, -0.6}), k(2.4, {-0.8, -0.4, -0.6})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.6, {-4, 3, 0}), k(1.2, {2, -2, 0}), k(1.8, {-3, 0, 0}), k(2.4, {0, 0, 0})});
  }
  {
    ActionDef& a = def("gestureOpen", 1.5, 0.2, 0.35);
    a.set(C::HandR, {k(0, {0.14, 0.24, -0.08}), k(0.4, {0.3, 0.3, 0.0}, out), k(1.0, {0.32, 0.28, 0.0}), k(1.5, {0.14, 0.24, -0.08})});
    a.set(C::HandL, {k(0, {-0.14, 0.24, -0.08}), k(0.4, {-0.3, 0.3, 0.0}, out), k(1.0, {-0.32, 0.28, 0.0}), k(1.5, {-0.14, 0.24, -0.08})});
    a.set(C::HandRrot, {k(0, {10, 150, 0}), k(0.4, {10, 180, -20}), k(1.5, {10, 150, 0})});
    a.set(C::HandLrot, {k(0, {10, -150, 0}), k(0.4, {10, -180, 20}), k(1.5, {10, -150, 0})});
    a.set(C::ElbowR, {k(0, {0.8, -0.4, -0.6}), k(1.5, {0.8, -0.4, -0.6})});
    a.set(C::ElbowL, {k(0, {-0.8, -0.4, -0.6}), k(1.5, {-0.8, -0.4, -0.6})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.4, {4, 0, 0}), k(1.5, {0, 0, 0})});
  }
  {
    ActionDef& a = def("gesturePoint", 1.4, 0.15, 0.35);
    a.set(C::HandR, {k(0, {0.14, 0.24, -0.08}), k(0.3, {0.16, 0.55, 0.18}, out), k(0.5, {0.16, 0.52, 0.16}), k(0.7, {0.16, 0.55, 0.18}), k(1.1, {0.16, 0.55, 0.18}), k(1.4, {0.14, 0.24, -0.08})});
    a.set(C::HandRrot, {k(0, {10, 130, 0}), k(0.3, {0, 90, 0}), k(1.4, {10, 130, 0})});
    a.set(C::ElbowR, {k(0, {0.8, -0.4, -0.6}), k(1.4, {0.8, -0.4, -0.6})});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.3, {-4, 0, 8}), k(1.4, {0, 0, 0})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.3, {-6, 0, 0}), k(0.5, {-2, 0, 0}), k(0.7, {-6, 0, 0}), k(1.4, {0, 0, 0})});
  }
  {
    ActionDef& a = def("shrug", 1.3, 0.15, 0.3);
    a.set(C::HandR, {k(0, {0.16, 0.2, -0.14}), k(0.35, {0.26, 0.24, -0.06}, out), k(0.9, {0.26, 0.24, -0.06}), k(1.3, {0.16, 0.2, -0.14})});
    a.set(C::HandL, {k(0, {-0.16, 0.2, -0.14}), k(0.35, {-0.26, 0.24, -0.06}, out), k(0.9, {-0.26, 0.24, -0.06}), k(1.3, {-0.16, 0.2, -0.14})});
    a.set(C::HandRrot, {k(0, {0, 180, -20}), k(1.3, {0, 180, -20})});
    a.set(C::HandLrot, {k(0, {0, -180, 20}), k(1.3, {0, -180, 20})});
    a.set(C::ElbowR, {k(0, {1, -0.3, -0.4}), k(1.3, {1, -0.3, -0.4})});
    a.set(C::ElbowL, {k(0, {-1, -0.3, -0.4}), k(1.3, {-1, -0.3, -0.4})});
    a.set(C::ClavR, {k(0, {0, 0, 0}), k(0.35, {0, -18, 0}, out), k(0.9, {0, -18, 0}), k(1.3, {0, 0, 0})});
    a.set(C::ClavL, {k(0, {0, 0, 0}), k(0.35, {0, 18, 0}, out), k(0.9, {0, 18, 0}), k(1.3, {0, 0, 0})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.35, {4, -12, 0}), k(0.9, {4, -12, 0}), k(1.3, {0, 0, 0})});
  }
  {
    ActionDef& a = def("handOnChest", 1.5, 0.2, 0.35);
    a.set(C::HandR, {k(0, {0.14, 0.24, -0.08}), k(0.35, {0.0, 0.17, 0.1}, out), k(1.1, {0.0, 0.17, 0.1}), k(1.5, {0.14, 0.24, -0.08})});
    a.set(C::HandRrot, {k(0, {10, 130, 0}), k(0.35, {60, 90, 90}), k(1.1, {60, 90, 90}), k(1.5, {10, 130, 0})});
    a.set(C::ElbowR, {k(0, {0.8, -0.4, -0.6}), k(1.5, {0.8, -0.4, -0.6})});
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.35, {-4, 0, 0}), k(1.5, {0, 0, 0})});
  }
  {
    ActionDef& a = def("nod", 0.9, 0.05, 0.15);
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.2, {-14, 0, 0}), k(0.4, {2, 0, 0}), k(0.6, {-10, 0, 0}), k(0.9, {0, 0, 0})});
  }
  {
    ActionDef& a = def("shakeHead", 1.0, 0.05, 0.15);
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.18, {0, 0, 14}), k(0.4, {0, 0, -14}), k(0.62, {0, 0, 12}), k(0.82, {0, 0, -6}), k(1.0, {0, 0, 0})});
  }
  {
    ActionDef& a = def("laugh", 1.6, 0.1, 0.3);
    a.set(C::Head, {k(0, {0, 0, 0}), k(0.2, {16, 0, 0}), k(0.35, {10, 0, 0}), k(0.5, {16, 0, 0}), k(0.65, {10, 0, 0}), k(0.8, {15, 0, 0}), k(1.6, {0, 0, 0})});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(0.2, {4, 0, 0}), k(0.35, {-2, 0, 0}), k(0.5, {4, 0, 0}), k(0.65, {-2, 0, 0}), k(0.8, {4, 0, 0}), k(1.6, {0, 0, 0})});
    a.set(C::ClavR, {k(0, {0, 0, 0}), k(0.35, {0, -8, 0}), k(0.65, {0, -8, 0}), k(1.6, {0, 0, 0})});
    a.set(C::ClavL, {k(0, {0, 0, 0}), k(0.35, {0, 8, 0}), k(0.65, {0, 8, 0}), k(1.6, {0, 0, 0})});
  }
  // Shared street strikes: clear windup, committed contact, visible follow-through.
  struct Strike {
    const char* name;
    const char* tag;
    const char* feature;
    V3 wind, follow;
    f64 duration;
    bool two, reverse;
  };
  constexpr Strike strikes[] = {{"reverseStab", "short_blade", "tip", {.28, .12, .46}, {.1, .46, -.12}, .85, false, true},
                                {"reverseSlash", "short_blade", "edge", {.4, .18, .3}, {-.12, .43, -.12}, .88, false, true},
                                {"bladeChop", "long_blade", "edge", {.24, .13, .48}, {.12, .5, -.24}, 1.2, false, false},
                                {"bladeDiagonal", "long_blade", "edge", {.42, .18, .35}, {-.16, .5, -.2}, 1.15, false, false},
                                {"bladeBackhand", "long_blade", "edge", {-.15, .33, .26}, {.42, .4, -.08}, 1.15, false, false},
                                {"bladeThrust", "long_blade", "tip", {.3, .12, .05}, {.14, .55, .04}, 1.05, false, false},
                                {"clubOverhead", "club", "surface", {.26, .12, .48}, {.14, .5, -.22}, 1.2, false, false},
                                {"clubForehand", "club", "surface", {.43, .14, .22}, {-.15, .45, -.14}, 1.15, false, false},
                                {"clubBackhand", "club", "surface", {-.12, .32, .2}, {.44, .43, -.1}, 1.15, false, false},
                                {"clubJab", "club", "pommel", {.23, .1, .06}, {.18, .52, .05}, .85, false, false},
                                {"batSwing", "club", "surface", {.24, .18, .2}, {-.08, .4, -.06}, 1.8, true, false},
                                {"batOverhead", "club", "surface", {.12, .22, .38}, {.1, .4, -.16}, 1.85, true, false},
                                {"batButtJab", "club", "pommel", {.26, .12, .05}, {.14, .5, .08}, .95, true, false}};
  for (const auto& s : strikes) {
    ActionDef& a = def(s.name, s.duration, .14, .25);
    a.requires_tags = {s.tag};
    a.two_hands = s.two;
    a.reverse_grip = s.reverse;
    a.targeted = true;
    a.reach = s.reverse ? .3 : std::string_view(s.feature) == "pommel" ? (s.two ? .4 : .5) : .9;
    a.step_distance = s.reverse ? .52 : s.two && std::string_view(s.feature) == "pommel" ? .5 : .36;
    a.target_height = 1.25;
    a.lead_side = -1;
    const f64 t = s.duration;
    const V3 guard{.2, .28, .08};
    a.set(C::HandR, {k(0, {guard.x, guard.y, guard.z}), k(t * .25, {s.wind.x, s.wind.y, s.wind.z}), k(t * .52, {s.follow.x, s.follow.y, s.follow.z}, inout),
                     k(t, {guard.x, guard.y, guard.z}, inout)});
    if (s.reverse)
      a.set(C::HandRrot, {k(0, {35, 60, 0}), k(t * .25, {50, 70, -15}), k(t * .52, {130, 30, 0}), k(t, {35, 60, 0})});
    else if (s.two)
      a.set(C::HandRrot, {k(0, {0, 20, 0}), k(t * .25, {20, 25, -5}), k(t * .52, {-20, 25, -25}), k(t, {0, 20, 0})});
    else
      a.set(C::HandRrot, {k(0, {0, 60, 0}), k(t * .25, {20, 80, -25}), k(t * .52, {-20, 80, 35}), k(t, {0, 60, 0})});
    a.set(C::ElbowR, {k(0, {.8, -.3, -.4}), k(t, {.8, -.3, -.4})});
    a.set(C::HandL, {k(0, {-.12, .28, .27}), k(t, {-.12, .28, .27})});
    a.set(C::Chest, {k(0, {0, 0, 0}), k(t * .25, {4, -5, -28}), k(t * .52, {-12, 5, 32}), k(t, {0, 0, 0})});
    a.set(C::PelvisRot, {k(0, {0, 0, 0}), k(t * .25, {0, 0, -15}), k(t * .52, {-4, 0, 20}), k(t, {0, 0, 0})});
    a.set(C::Crouch, {k(0, .18), k(t * .52, .3), k(t, .18)});
    a.set(C::StrikeR, {k(0, 0), k(t * .27, 0), k(t * .42, s.reverse ? 1.15 : .9, inout), k(t * .56, s.reverse ? .95 : .5), k(t * .75, 0)});
    a.events = {{t * .44, "strike", Limb::Blade, s.feature}};
  }
  for (const auto& spec : std::vector<std::pair<std::string, std::string>>{
           {"bladeGuard", "long_blade"}, {"clubGuard", "club"}, {"batStance", "club"}, {"propBlock", "blocking"}}) {
    ActionDef& a = def(spec.first.c_str(), spec.first == "propBlock" ? .8 : 1.4, .2, .25);
    a.requires_tags = {spec.second};
    a.two_hands = spec.first == "batStance";
    a.lead_side = -1;
    a.layer = spec.first == "propBlock" ? ActionLayer::Act : ActionLayer::Pose;
    a.loop = a.layer == ActionLayer::Pose;
    guard_hands(a, 0, a.duration);
    a.set(C::HandR, {k(0, {.18, .32, .25}), k(a.duration, {.18, .32, .25})});
    a.set(C::HandRrot, {k(0, {20, 70, -20}), k(a.duration, {20, 70, -20})});
  }
  for (const auto* name : {"overcommitRecover", "fumble", "releaseForFall"}) {
    ActionDef& a = def(name, 1.1, .16, .28);
    a.lead_side = -1;
    a.set(C::HandR, {k(0, {.28, .35, -.18}), k(.5, {.3, .2, -.1}), k(1.1, {.18, .22, .05})});
    a.set(C::Chest, {k(0, {-16, 0, 20}), k(.5, {-8, 0, 8}), k(1.1, {0, 0, 0})});
    if (a.name == "releaseForFall") a.events = {{.1, "release", Limb::None}};
  }
  for (auto& a : list) {
    if (a.name == "pockets" || a.name == "armsCrossed" || a.name == "checkWatch" || a.name == "handsFolded" || a.name == "handsBehind") a.free_hands = true;
    for (auto& e : a.events)
      if (e.limb == Limb::Blade && e.feature.empty()) e.feature = a.name == "stab" || a.name == "gutStab" ? "tip" : "edge";
    for (auto& e : a.events)
      if (e.limb == Limb::Muzzle) e.feature = "muzzle";
  }
  return list;
}

struct Library {
  std::vector<ActionDef> list;
  std::map<std::string, i32, std::less<>> index;
};

const Library& library() {
  static const Library lib = [] {
    Library l;
    std::vector<ActionDef> base = build();
    l.list.reserve(base.size() * 2);
    for (ActionDef& a : base) {
      ActionDef m = mirror_action(a);
      l.list.push_back(std::move(a));
      l.list.push_back(std::move(m));
    }
    for (i32 i = 0; i < i32(l.list.size()); ++i) l.index.insert_or_assign(l.list[size_t(i)].name, i);
    return l;
  }();
  return lib;
}

}  // namespace

const std::vector<ActionDef>& actions() { return library().list; }

const ActionDef* action_def(std::string_view name) {
  const Library& l = library();
  const auto it = l.index.find(name);
  return it == l.index.end() ? nullptr : &l.list[size_t(it->second)];
}

}  // namespace svx::anim
