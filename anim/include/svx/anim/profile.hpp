// svx_anim — a character's tuning as data (docs/ANIM.md §9): the numbers and switches its motor
// behaviours, its body and its damage would otherwise hard-code. A profile is chosen per character
// (CharacterOptions::profile) and can be read and written by name (profile_set / profile_get).
//
// `CharacterProfile{}` is the current behaviour. `legacy_profile()` is svx_anim as it was before
// anatomical damage and capability-driven motion (engine main 0eda3ca) wherever a mechanism changed,
// so that baseline stays reproducible: zone hit points instead of tissue mechanics, the shove and
// spin hit response, and the arm muscles, solver, spin caps and foot assists as they were tuned.
#pragma once

#include <array>
#include <string_view>

#include "svx/base/types.hpp"

namespace svx::anim {

enum class DamageModel : u8 {
  Anatomical,  // tissue mechanics, persistent wounds, physiology and capabilities (docs/WOUNDS.md)
  Zones,       // hit points by zone, spherical carves, collapse below 30% (legacy)
};

enum class HitResponse : u8 {
  Momentum,  // the blow's momentum at the hit point and a bounded trunk reflex (docs/ACTION_REFINEMENT.md)
  Shove,     // an authored shove, a spin about the lever arm and random knock-backs (legacy)
};

struct ModeTone {
  f64 base = 1, legs = 1, arms = 1, neck = 1, trunk = 1;
};

// What a character does by itself standing still (MotionInput::idle): when it settles into an idle
// posture and how often it changes it, its fidgets (armed or not) and its gestures in conversation.
// Times in seconds: a first wait, then `every + spread * random` (scaled by the style's fidget).
struct IdleAutonomy {
  f64 first_pose = 3.0, first_fidget = 6.0, first_gesture = 1.0;
  f64 pose_after = 2.0;                            // standing still this long before a posture shows
  f64 pose_restart = 4.0, pose_restart_spread = 4.0;  // (after an interruption)
  f64 pose_resume = 2.0, pose_resume_spread = 2.0;    // (after moving, talking or a guard)
  f64 pose_every = 7.0, pose_spread = 12.0;
  f64 pose_none = 0.3;                             // chance the next pick is no posture
  f64 fidget_after = 3.0, fidget_every = 5.0, fidget_spread = 10.0, fidget_chance = 0.7;
  f64 armed_after = 2.5, armed_every = 6.0, armed_spread = 9.0, armed_chance = 0.75;
  f64 speak_every = 1.2, speak_spread = 2.8, speak_nod = 0.25;
  f64 listen_every = 1.5, listen_spread = 3.0;
};

struct CharacterProfile {
  // ---- motor (behaviour/controller.cpp)
  f64 arms_at_ease = 0.82;      // relaxed arm tone (legacy 0.62)
  f64 neck_at_ease = 0.8;
  f64 arm_recruit = 1.15;       // arm tone while they hold, act or turn, at least (legacy 1)
  bool arm_activation = true;   // recruit quickly, release over a while (legacy: at once with the pose)
  ModeTone tone_reacting{1, 1, 0.8, 1, 1};
  ModeTone tone_falling{1, 0.35, 0.9, 0.8, 0.55};
  f64 tone_lying = 0.22, tone_lying_writhing = 0.75, tone_lying_unconscious = 0.06;
  f64 tone_rising_from = 0.25;  // (to 1 over the first second)
  f64 tone_dying = 0.9, tone_dying_arms = 0.8;
  f64 dying_legs_hold = 0.0;    // the legs' tone while a body tips over dying (legacy 0.75: stiff while upright)
  bool spin_by_mode = true;     // spin caps below by mode (legacy: 14 falling, dying or dead, else 80)
  f64 spin_lying = 4, spin_ground = 14, spin_reacting = 24, spin_busy = 60, spin_calm = 36;  // rad/s
  f64 ground_target_rate = 7.0;  // rad/s: joint targets of a body on the ground turn at most this fast (>= 30: unbounded, legacy)
  f64 foot_pin_force = 80.0;     // N: a planted foot's assist ...
  f64 foot_pin_effort = 600.0;   // ... plus this x the leg's effort (legacy 60, 400)
  f64 foot_pin_idle_share = 0.6; // (of the effort term while the foot does not strike; legacy 0.4)
  f64 foot_turn_damping = 12.0;  // a planted foot's turn damping (legacy 6)
  HitResponse hit_response = HitResponse::Momentum;
  IdleAutonomy idle;

  // ---- body (body/humanoid.cpp): arm muscles {natural frequency rad/s, damping ratio}
  std::array<std::array<f64, 2>, 3> arm_muscles{{{16, 1.05}, {19, 1.05}, {28, 1.15}}};  // shoulder, elbow, wrist (legacy {12,1}, {13,1}, {12,1.3})

  // ---- the body of its own (physics/rigid.cpp)
  bool solver_clamps = true;  // projection turns, pose spin and damper torque bounded per substep (legacy: off)

  // ---- damage (damage/*)
  DamageModel damage = DamageModel::Anatomical;
  bool gibs_carry_momentum = true;  // severed pieces move with their part and the wound's impulse (legacy: thrown at 2.5 m/s)
  bool blunt_passes_props = true;   // a prop takes what it can absorb of a blunt blow; the rest goes on (legacy: all of it)
  bool blast_from_source = true;    // a blast's fragments and overpressure come from its source (legacy: scripted limb crush)
  i32 max_injuries = 12;            // reaction injuries kept: past it they join others of their zone
  i32 max_wounds = 48;              // persistent wounds kept: beyond, a new one joins the nearest on its part
  f64 wound_merge_distance = 0;     // m: a new wound this close to one on the same part joins it
  i64 stain_work = 200000;          // cells a frame's staining visits at most (the rest waits its turn)
  f64 fracture_limb = 65, fracture_slender = 45, fracture_trunk = 160;  // J deposited in bone (damage/state.cpp)
  f64 shatter_limb = 220, shatter_trunk = 600;
  f64 crush_remove_energy = 1200;  // J: a crush above this removes the tissue it overcomes
  f64 knockout_head_energy = 300;  // J: blunt energy in the head above this knocks out
  f64 crush_contact_dv = 2.2;      // m/s: a world contact this hard on the body is a crush wound
};

CharacterProfile legacy_profile();

// By name ("arms_at_ease", "damage", "solver_clamps", ...): false for an unknown name or a value out
// of range. Enumerations and switches read and write as numbers.
bool profile_set(CharacterProfile& p, std::string_view name, f64 value);
bool profile_get(const CharacterProfile& p, std::string_view name, f64* value);

}  // namespace svx::anim
