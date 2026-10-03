#include "svx/anim/profile.hpp"

#include <cmath>

namespace svx::anim {

CharacterProfile legacy_profile() {
  CharacterProfile p;
  p.arms_at_ease = 0.62;
  p.arm_recruit = 1.0;
  p.arm_activation = false;
  p.dying_legs_hold = 0.75;
  p.spin_by_mode = false;
  p.ground_target_rate = 30.0;
  p.foot_pin_force = 60.0;
  p.foot_pin_effort = 400.0;
  p.foot_pin_idle_share = 0.4;
  p.foot_turn_damping = 6.0;
  p.hit_response = HitResponse::Shove;
  p.arm_muscles = {{{12, 1.0}, {13, 1.0}, {12, 1.3}}};
  p.solver_clamps = false;
  p.damage = DamageModel::Zones;
  p.gibs_carry_momentum = false;
  p.blunt_passes_props = false;
  p.blast_from_source = false;
  return p;
}

namespace {

enum class Kind : u8 { Real, Flag, Int, Long, Damage, Response };
struct Field {
  const char* name;
  Kind kind;
  void* (*at)(CharacterProfile&);
  f64 lo = -1e300, hi = 1e300;
};

#define SVX_P(name, kind, member, lo, hi) Field{name, kind, [](CharacterProfile& p) -> void* { return &p.member; }, lo, hi}
const Field kFields[] = {
    SVX_P("arms_at_ease", Kind::Real, arms_at_ease, 0, 4),
    SVX_P("neck_at_ease", Kind::Real, neck_at_ease, 0, 4),
    SVX_P("arm_recruit", Kind::Real, arm_recruit, 0, 4),
    SVX_P("arm_activation", Kind::Flag, arm_activation, 0, 1),
    SVX_P("tone_reacting_arms", Kind::Real, tone_reacting.arms, 0, 4),
    SVX_P("tone_falling_legs", Kind::Real, tone_falling.legs, 0, 4),
    SVX_P("tone_falling_trunk", Kind::Real, tone_falling.trunk, 0, 4),
    SVX_P("tone_falling_arms", Kind::Real, tone_falling.arms, 0, 4),
    SVX_P("tone_falling_neck", Kind::Real, tone_falling.neck, 0, 4),
    SVX_P("tone_lying", Kind::Real, tone_lying, 0, 4),
    SVX_P("tone_lying_writhing", Kind::Real, tone_lying_writhing, 0, 4),
    SVX_P("tone_lying_unconscious", Kind::Real, tone_lying_unconscious, 0, 4),
    SVX_P("tone_rising_from", Kind::Real, tone_rising_from, 0, 4),
    SVX_P("tone_dying", Kind::Real, tone_dying, 0, 4),
    SVX_P("tone_dying_arms", Kind::Real, tone_dying_arms, 0, 4),
    SVX_P("dying_legs_hold", Kind::Real, dying_legs_hold, 0, 4),
    SVX_P("spin_by_mode", Kind::Flag, spin_by_mode, 0, 1),
    SVX_P("spin_lying", Kind::Real, spin_lying, 0.1, 200),
    SVX_P("spin_ground", Kind::Real, spin_ground, 0.1, 200),
    SVX_P("spin_reacting", Kind::Real, spin_reacting, 0.1, 200),
    SVX_P("spin_busy", Kind::Real, spin_busy, 0.1, 200),
    SVX_P("spin_calm", Kind::Real, spin_calm, 0.1, 200),
    SVX_P("ground_target_rate", Kind::Real, ground_target_rate, 0.1, 1000),
    SVX_P("foot_pin_force", Kind::Real, foot_pin_force, 0, 1e5),
    SVX_P("foot_pin_effort", Kind::Real, foot_pin_effort, 0, 1e5),
    SVX_P("foot_pin_idle_share", Kind::Real, foot_pin_idle_share, 0, 1),
    SVX_P("foot_turn_damping", Kind::Real, foot_turn_damping, 0, 1e4),
    SVX_P("hit_response", Kind::Response, hit_response, 0, 1),
    SVX_P("idle_pose_after", Kind::Real, idle.pose_after, 0, 600),
    SVX_P("idle_pose_every", Kind::Real, idle.pose_every, 0, 600),
    SVX_P("idle_pose_spread", Kind::Real, idle.pose_spread, 0, 600),
    SVX_P("idle_pose_none", Kind::Real, idle.pose_none, 0, 1),
    SVX_P("idle_fidget_after", Kind::Real, idle.fidget_after, 0, 600),
    SVX_P("idle_fidget_every", Kind::Real, idle.fidget_every, 0, 600),
    SVX_P("idle_fidget_spread", Kind::Real, idle.fidget_spread, 0, 600),
    SVX_P("idle_fidget_chance", Kind::Real, idle.fidget_chance, 0, 1),
    SVX_P("idle_armed_after", Kind::Real, idle.armed_after, 0, 600),
    SVX_P("idle_armed_every", Kind::Real, idle.armed_every, 0, 600),
    SVX_P("idle_armed_spread", Kind::Real, idle.armed_spread, 0, 600),
    SVX_P("idle_armed_chance", Kind::Real, idle.armed_chance, 0, 1),
    SVX_P("idle_speak_every", Kind::Real, idle.speak_every, 0, 600),
    SVX_P("idle_speak_spread", Kind::Real, idle.speak_spread, 0, 600),
    SVX_P("idle_speak_nod", Kind::Real, idle.speak_nod, 0, 1),
    SVX_P("idle_listen_every", Kind::Real, idle.listen_every, 0, 600),
    SVX_P("idle_listen_spread", Kind::Real, idle.listen_spread, 0, 600),
    SVX_P("shoulder_frequency", Kind::Real, arm_muscles[0][0], 0.1, 200),
    SVX_P("shoulder_damping", Kind::Real, arm_muscles[0][1], 0, 10),
    SVX_P("elbow_frequency", Kind::Real, arm_muscles[1][0], 0.1, 200),
    SVX_P("elbow_damping", Kind::Real, arm_muscles[1][1], 0, 10),
    SVX_P("wrist_frequency", Kind::Real, arm_muscles[2][0], 0.1, 200),
    SVX_P("wrist_damping", Kind::Real, arm_muscles[2][1], 0, 10),
    SVX_P("solver_clamps", Kind::Flag, solver_clamps, 0, 1),
    SVX_P("damage", Kind::Damage, damage, 0, 1),
    SVX_P("gibs_carry_momentum", Kind::Flag, gibs_carry_momentum, 0, 1),
    SVX_P("blunt_passes_props", Kind::Flag, blunt_passes_props, 0, 1),
    SVX_P("blast_from_source", Kind::Flag, blast_from_source, 0, 1),
    SVX_P("max_injuries", Kind::Int, max_injuries, 1, 4096),
    SVX_P("max_wounds", Kind::Int, max_wounds, 1, 4096),
    SVX_P("wound_merge_distance", Kind::Real, wound_merge_distance, 0, 1),
    SVX_P("stain_work", Kind::Long, stain_work, 1, 1e12),
    SVX_P("fracture_limb", Kind::Real, fracture_limb, 0, 1e7),
    SVX_P("fracture_slender", Kind::Real, fracture_slender, 0, 1e7),
    SVX_P("fracture_trunk", Kind::Real, fracture_trunk, 0, 1e7),
    SVX_P("shatter_limb", Kind::Real, shatter_limb, 0, 1e7),
    SVX_P("shatter_trunk", Kind::Real, shatter_trunk, 0, 1e7),
    SVX_P("crush_remove_energy", Kind::Real, crush_remove_energy, 0, 1e9),
    SVX_P("knockout_head_energy", Kind::Real, knockout_head_energy, 0, 1e9),
    SVX_P("crush_contact_dv", Kind::Real, crush_contact_dv, 0, 1000),
};
#undef SVX_P

const Field* field(std::string_view name) {
  for (const Field& f : kFields)
    if (name == f.name) return &f;
  return nullptr;
}

}  // namespace

bool profile_set(CharacterProfile& p, std::string_view name, f64 v) {
  const Field* f = field(name);
  if (!f || !std::isfinite(v) || v < f->lo || v > f->hi) return false;
  void* at = f->at(p);
  switch (f->kind) {
    case Kind::Real: *static_cast<f64*>(at) = v; break;
    case Kind::Flag:
      if (v != 0 && v != 1) return false;
      *static_cast<bool*>(at) = v != 0;
      break;
    case Kind::Int:
      if (v != std::floor(v)) return false;
      *static_cast<i32*>(at) = static_cast<i32>(v);
      break;
    case Kind::Long:
      if (v != std::floor(v)) return false;
      *static_cast<i64*>(at) = static_cast<i64>(v);
      break;
    case Kind::Damage:
      if (v != std::floor(v)) return false;
      *static_cast<DamageModel*>(at) = static_cast<DamageModel>(v);
      break;
    case Kind::Response:
      if (v != std::floor(v)) return false;
      *static_cast<HitResponse*>(at) = static_cast<HitResponse>(v);
      break;
  }
  return true;
}

bool profile_get(const CharacterProfile& p, std::string_view name, f64* v) {
  const Field* f = field(name);
  if (!f || !v) return false;
  void* at = f->at(const_cast<CharacterProfile&>(p));
  switch (f->kind) {
    case Kind::Real: *v = *static_cast<const f64*>(at); break;
    case Kind::Flag: *v = *static_cast<const bool*>(at) ? 1 : 0; break;
    case Kind::Int: *v = *static_cast<const i32*>(at); break;
    case Kind::Long: *v = static_cast<f64>(*static_cast<const i64*>(at)); break;
    case Kind::Damage: *v = static_cast<f64>(*static_cast<const DamageModel*>(at)); break;
    case Kind::Response: *v = static_cast<f64>(*static_cast<const HitResponse*>(at)); break;
  }
  return true;
}

}  // namespace svx::anim
