// svx_anim — the body's behaviours: the character's motor intelligence, in the manner of
// NaturalMotion's Euphoria. The motion plan says what the character means to do; the physical
// body is what it is made of; the behaviours sit between them, read the body's senses (balance,
// contacts, what is within reach, what hit it) and decide, every frame, how the muscles work: how
// tense each region is, how much the legs hold the body up and where the feet step, where the
// hands go, where the head turns.
//
// Modes:
// - animated: the body tracks the plan (muscles and assists strong), with its own secondary
//   motion; blows it can take in its stride are absorbed.
// - reacting: knocked off the plan (a hit, a shove, a blast, a caught foot). Balance takes over:
//   the legs hold the body up but no longer steer it; the ground pushes back through the feet's
//   centre of pressure (the inverted pendulum), and the feet step where the capture point says (a
//   stagger is a run of such steps), arms out; a hand reaches for a wall within reach and holds
//   on. Back in balance and still, it hands back to the plan.
// - falling: balance lost: the legs give, the hands go out to break the fall (on the ground or a
//   wall), the head is kept off the ground.
// - lying: on the ground, limp but alive (knocked down, knocked out), holding a wound.
// - rising: gathering and getting up through sitting or pushing up, kneeling, standing, the legs
//   taking the weight back.
// - dying: the muscles fade (the legs first): the knees buckle, a hand goes to the wound, a last
//   step or two, the arms half catch the fall; near a wall the body slumps against it.
// - dead: no muscle; the body settles and sleeps.
//
// Reflexes on top of any mode: flinching from what lands close (the head turns away and ducks,
// the shoulders come up, a hand comes up between the face and the danger), holding a wound, the
// posture of injuries (a limp, a weak arm, a hunch), stuns (a struck limb goes limp for a moment).
//
// A frame: prepare (senses, modes, the plan's controls), the plan's update, then drive: the
// muscles and assists from the plan (drive_pre), the physics step - here (drive) or the world's
// tick (the deep path) - and what the body felt in it (drive_post).
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "svx/anim/behaviour/injuries.hpp"
#include "svx/anim/behaviour/senses.hpp"
#include "svx/anim/body/humanoid.hpp"
#include "svx/anim/motion/plan.hpp"

namespace svx::anim {

enum class BodyMode : u8 { Animated, Reacting, Falling, Lying, Rising, Dying, Dead };
const char* body_mode_name(BodyMode m);

enum class PerceptionKind : u8 { Impact, Whiz, Blast, Blow };

// Something the body perceives close by: a round smacking in, a whiz past the head, a blast.
struct Perception {
  V3 point;
  f64 strength = 0.0;  // 0..1 (and more for blasts)
  PerceptionKind kind = PerceptionKind::Impact;
};

enum class BraceWhy : u8 { Balance, Lean, Fall, Slump };

struct Brace {
  Surface surface;
  i32 hand = 0;  // 0 left hand, 1 right
  V3 target;     // the palm's target (world)
  V3 normal;
  BraceWhy why = BraceWhy::Balance;
  bool holding = false;  // the hand has arrived and holds on
  f64 age = 0.0;
};

void set_arms_at_ease(f64 t);  // (tuning)

class Behaviours {
 public:
  Behaviours(MotionPlan& plan, HumanoidBody& body, f64 seed = 1.0);

  MotionPlan& plan;
  HumanoidBody& body;
  Injuries injuries;
  Surroundings surroundings;
  SupportPolygon support;
  BodyMode mode = BodyMode::Animated;
  f64 mode_time = 0.0;  // seconds in the current mode
  bool physical = false;  // the physical body is simulated (else the plan's pose is shown as is)
  bool alive = true;
  bool conscious = true;
  // balance
  V3 com, com_vel, capture;
  f64 balance_error = 0.0;  // distance of the capture point outside the support (negative: inside)
  f64 ground_z = 0.0;
  i32 steps = 0;          // stagger steps taken in this reaction
  bool airborne = false;  // no foot on the ground
  f64 nerves = 0.0;       // nerves 0..1: rattled by what lands close (a lower, hunched body; quicker flinches)
  // where the body's root is (ground under the body) when the body leads, and its heading
  V3 body_root;
  f64 body_yaw = 0.0;
  V3 root_motion;  // root displacement the host still has to apply (the body moved it)
  std::optional<Brace> brace;  // the last brace (for tests and debugging)
  std::array<f64, kRegionCount> region_tone{1, 1, 1, 1, 1, 1};  // tone per region this frame (for tests and debugging)
  std::string lost_why;  // why the last reaction ended in a fall (debugging)
  std::array<bool, kBodyCount> lost{};  // body parts shot off (a limb and all below it)
  bool legless = false;  // a leg is gone: the body will not stand again (it can still crawl)
  bool writhing = false;  // down and writhing in pain (see collapse)

  f64 k() const { return plan.k; }
  bool leading() const { return mode != BodyMode::Animated; }  // the body leads the root (the host follows it)
  bool needs_physics() const;  // something is going on that needs the physics (not calm)

  // ---- events
  // A blow on the living body (world): an impulse where it lands (the physics answers it), a limp
  // moment of the struck part, a wound to hold, a flinch; the balance deals with the rest.
  Zone hit(const HitInfo& info, i32 part, const WorldPose& pose);
  void bumped(f64 j);  // another body bumped into this one (N s): hard enough, the balance has to answer it
  void perceive(const Perception& p);  // something close by: a flinch, stronger the closer and the bigger it is
  // A shove of the whole body (a blast's push, a collision): `dv` is the velocity change of the
  // trunk (m/s, world); limbs flail after it.
  void push(const V3& dv, f64 stun_all = 0.0);
  // A foot catches (the host's say, or an obstacle): the swinging foot stops dead (or the next one
  // to swing, within half a second).
  void trip();
  // A limb shot off at `part` (it and everything below it): the body has no use of it. The parts
  // stay in the simulation (the joints need them) but touch nothing and weigh next to nothing. A
  // leg gone: the body goes down and does not stand again.
  void lose_limb(i32 part);
  // Too badly hurt to stand: the legs give, the body goes down and writhes (clutching the wound,
  // curling up, rocking) for `seconds`, then struggles back up.
  void collapse(f64 seconds);
  void knock_out(f64 seconds);  // the body drops and stays down `seconds`
  // Death: the muscles fade over `collapse` seconds (0: at once, a head shot or a blast), the last
  // wound held while they last.
  void die(f64 collapse);

  // ---- the frame
  // Senses, changes mode, and says what the plan should do this frame. `pose` is the body's pose
  // of the last frame (physical or planned).
  void prepare(f64 dt, const WorldPose& pose);
  // Muscle targets, tone and assists from the plan, the physics step, and what came of it; writes
  // `out` (the shallow path: the body's own step).
  void drive(f64 dt, WorldPose& out);
  void drive_pre(f64 dt);                // (the muscles and assists for the step)
  void drive_post(f64 dt, WorldPose& out);  // (after the step: the pose, bumps, trips, the dead settling)
  V3 take_root_motion();  // the root displacement the body made (hosts move their character by it)

 private:
  struct Threat {
    V3 point;
    f64 amount = 0.0;
    f64 age = 0.0;
    f64 hold = 0.0;  // hold time before it fades
  };

  f64 w0_ = 3.5;  // the pendulum's natural frequency (sqrt(g / h), 1/s)
  f64 balanced_for_ = 0.0;
  f64 low_for_ = 0.0;
  std::vector<Threat> threats_;
  std::array<f32, kBodyCount> stun_{};  // per-body stun (0..1: tone lost), recovering
  f64 daze_ = 0.0;   // whole-body daze (0..1)
  f64 shock_ = 0.0;  // the shock of a hit (0..1): the whole body slack for a moment
  f64 down_until_ = 0.0;
  std::optional<V3> hit_from_;  // where the last blow came from, and when (the head turns to look for it)
  f64 hit_at_ = -99.0;
  f64 lost_for_ = 0.0;  // how long the body has been beyond saving (reacting)
  f64 dying_for_ = 0.6;
  bool dying_head_ = false;
  f64 writhe_seed_ = 0.0;
  f64 die_z_ = 0.0;  // the pelvis height the dying body sinks from
  f64 tension_ = 0.0;
  Rng rng_;
  std::array<f64, 2> grip_{0, 0};  // hand tasks' physical grip (0..1) this frame, left and right
  std::array<f64, 2> catch_t_{0, 0};
  f64 step_cooldown_ = 0.0;
  f64 upset_ = 0.0;
  f64 time_ = 0.0;
  bool root_init_ = false;
  f64 react_t_ = 0.0;
  f64 trip_pending_ = 0.0;  // a trip waiting for the next swing (s left)
  std::array<f64, 2> snag_until_{0, 0};  // a caught foot bears no weight and cannot step until then (time)
  bool force_react_ = false;
  f64 startle_at_ = -99.0;  // when the last startled step was taken
  // the flinch as one smoothed response: how much, from where, which side, how long under fire
  f64 flinch_i_ = 0.0;
  V3 flinch_dir_{0, 1, 0};
  f64 flinch_side_ = 1.0;
  f64 under_fire_ = 0.0;
  std::array<V3, 2> hand_vel_{};  // the hands' planned velocities (smoothed), for their grips
  // arms out for balance (0..1, smoothed) and what the balance wants of them now
  f64 flail_ = 0.0, flail_want_ = 0.0;
  std::array<f64, 2> blocked_for_{0, 0};  // how long each swinging foot has been caught on something (s)
  std::array<std::optional<V3>, 2> landed_;  // where each hand landed breaking a fall (none: still in the air)
  // sensing
  f64 sense_dt_ = 0.0;
  bool had_com_ = false;
  V3 plan_pelvis_vel_, plan_pelvis_prev_;  // the plan's pelvis velocity (smoothed like the body's)
  // this frame's region tone and the planted legs (drive_pre, for drive_post)
  std::vector<f64> pts_;  // (scratch: support points)

  void set_mode(BodyMode m);
  void stun_part(i32 part, f64 s);
  void catch_foot(i32 i);
  void sense(const WorldPose& pose);
  V3 sole_pos(i32 i) const;
  V3 body_root_or_plan() const;
  void carry_root();
  void modes(f64 dt, const WorldPose& pose);
  void carry_body(f64 ex, f64 ey, f64 ez, f64 amount);
  void enter_lying(const WorldPose& pose);
  void align_lying(const WorldPose& pose, bool only_if_changed = false);
  f64 leg_strength() const;
  f64 tilt(const WorldPose& pose) const;
  void balance(f64 dt);
  void arms_out(f64 w, f64 dt);
  void catch_fall(f64 dt, const WorldPose& pose);
  void bracing(f64 dt, const WorldPose& pose);
  void flinch(f64 dt, const WorldPose& pose);
  void hold_wound(f64 dt, const WorldPose& pose);
  f64 arm_held(i32 i) const;
  void centre_of_pressure(f64 legs);
  void detect_trips(const WorldPose& pose, f64 dt);
};

// A hand rotation (the canonical fist frame: knuckles +y, palm -z) with the palm facing `palm`
// and the fingers towards `fingers` (as far as it allows).
Quat palm_frame(const V3& palm, const V3& fingers);

}  // namespace svx::anim
