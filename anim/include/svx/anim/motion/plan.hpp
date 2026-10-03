// svx_anim — the motion plan: what the body means to do, as a pose (the intent the physical
// body's muscles track).
//
// The host moves the character (root position on the ground and facing yaw, each frame) and says
// what it is doing: stance, crouch, weapon carry, aim and look targets, mood, talking, guard; it
// starts actions (strikes, reloads, gestures). The body's behaviours add what the situation asks
// for through `control`: a hand to a wound, on a wall or out to break a fall, arms out for
// balance, the trunk ducking or folding, the head turned away, a limp, balance steps, a pelvis
// where the physics has it.
//
// The plan is built in layers:
// 1. Stances (stances.hpp): standing, kneeling, prone and crawling, sitting on a seat or the
//    ground, lying (and getting up from it); transitions blend through intermediate stances.
// 2. Locomotion (feet.hpp): the gait clock and the foot planter (planted feet, heel-toe roll,
//    ground under every step, obstacles cleared), balance steps. A personal style (style.hpp),
//    footfall compression, lean into acceleration, banking into turns, a tactical walk while
//    aiming, a limp.
// 3. Trunk and head: aim and look spread over spine, chest, neck and head; peeking; posture;
//    breathing; behaviour offsets.
// 4. Actions (actions.hpp): a posture layer (guard, idle poses, talking) and a one-shot layer
//    (strikes that land on their target, blocks, reloads, gestures, fidgets).
// 5. Arms (arms.hpp): the held weapon with both hands on it; guard, moods, stance hands, the
//    swing; action hands; behaviour hand tasks on top.
// 6. Legs by IK last, so every pelvis motion bends the knees with the feet where they are.
//
// Output: `pose` (local), `world` (world transforms; `prev_world` of the frame before), the held
// prop's transform, the feet, and events (a strike landing, a reload done).
//
// A plan works on its own pose (the arm rig holds pointers to it): it is neither copied nor
// moved.
#pragma once
#include "svx/anim/motion/crawl.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "svx/anim/ik.hpp"
#include "svx/anim/damage/capabilities.hpp"
#include "svx/anim/characters/attachments.hpp"
#include "svx/anim/motion/actions.hpp"
#include "svx/anim/profile.hpp"
#include "svx/anim/motion/arms.hpp"
#include "svx/anim/motion/feet.hpp"
#include "svx/anim/motion/gait.hpp"
#include "svx/anim/motion/stances.hpp"
#include "svx/anim/rig.hpp"
#include "svx/anim/motion/style.hpp"
#include "svx/anim/physics/collision.hpp"
#include "svx/anim/spring.hpp"

namespace svx::anim {

enum class Mood : u8 { Normal, Panic, Cower, Surrender };
// In a conversation: speaking (gestures) or listening (nods).
enum class Talk : u8 { None, Speak, Listen };

// Where a knee points in the rest pose (the legs twist with their knees).
constexpr V3 kKneeRest{0, 1, 0};

// A seat to sit on (world). The character's root stays where it stood, in front of it.
struct SeatInfo {
  V3 pos;  // the seat surface centre
  bool backrest = false;
  std::optional<f64> desk_height;  // the desk top above the floor (m), for sitting at a desk
  std::optional<SitVariant> variant;
};

// A chair for a character of size k (1 ~ 1.78 m) standing at `root`, facing `yaw`: its seat
// behind the heels at a chair's height, with a backrest, and a desk in front for SitVariant::Desk.
SeatInfo chair_behind(const V3& root, f64 yaw, f64 k, SitVariant variant);

struct MotionInput {
  f64 crouch = 0.0;  // 0 standing .. 1 crouched (standing stance)
  Stance stance = Stance::Stand;
  std::optional<SeatInfo> seat;
  GroundVariant ground_variant = GroundVariant::KneesUp;
  Carry carry = Carry::Relaxed;  // how the weapon is held (ignored without one)
  std::optional<V3> aim_at;      // a world point to aim the weapon (and the eyes) at
  std::optional<V3> look_at;     // a world point to look at (when not aiming)
  Mood mood = Mood::Normal;
  bool airborne = false;  // off the ground (falling, jumping): legs tuck, feet are not planted
  f64 lean = 0.0;         // peek around a corner: -1 left .. 1 right
  Talk talk = Talk::None;
  bool guard = false;  // fists up (a fight)
  bool idle = true;    // pick idle postures and fidgets by itself when standing still
};

// A hand sent somewhere by a behaviour (world).
struct ArmTask {
  V3 target;                 // the palm target
  std::optional<Quat> rot;   // the hand rotation (the canonical fist frame: knuckles +y, palm -z); none: as planned
  std::optional<V3> pole;    // the elbow direction; none: out and down
  f64 weight = 0.0;          // 0..1 over the planned arm
};

// What the body's behaviours ask of the plan this frame (reset by them every frame).
class PlanControl {
 public:
  std::array<std::optional<ArmTask>, 2> arms;  // hand tasks, left and right
  // extra trunk and head rotations (euler x, y, z in the joints' frames, rad)
  V3 spine, chest, neck, head;
  f64 fold = 0.0;    // forward fold of the trunk (gut wound, pain), rad
  f64 crouch = 0.0;  // added crouch 0..1
  f64 shrug = 0.0;   // shoulders drawn up (0..1)
  // limp per leg (0..1) and pain (0..1: a hunch, slower)
  std::array<f64, 2> limp{0.0, 0.0};
  f64 pain = 0.0;
  // where the eyes go instead (world), with a weight
  std::optional<V3> look;
  f64 look_weight = 0.0;
  // The pelvis where the physics has it (world), blended in by `pelvis_weight`: across the ground
  // always, its height only with `pelvis_height` (else the plan says how high the legs hold it).
  std::optional<V3> pelvis_pos;
  Quat pelvis_rot;
  f64 pelvis_weight = 0.0;
  bool pelvis_height = false;
  bool hold_feet = false;  // no gait steps: the feet move only by balance steps
  f64 relax_legs = 0.0;    // legs loose (0..1): bent at hip and knee rather than reaching for the feet
  f64 care = 1.0;          // how much care the steps get (obstacle clearance), 0..1
  bool busy = false;       // no idle picks (postures, fidgets) of the plan's own
  // (limp, pain and pelvis_rot are kept)
  void reset();
};

struct AnimEvent {
  std::string name;
  std::string action;
  Limb limb = Limb::None;
  std::string feature;
  u64 prop_instance = 0;
  V3 pos;                    // the world position of the limb (fist, foot, blade tip, tip) at the event
  std::optional<V3> target;  // the action's target (world), if any
};

class MotionPlan {
 public:
  MotionPlan(SkeletonPtr skeleton, const CollisionWorld* collision, f64 seed = 1.0);
  MotionPlan(const MotionPlan&) = delete;
  // What it does by itself standing still (the character's profile's; its first waits restart).
  void set_autonomy(const IdleAutonomy& autonomy);
  const IdleAutonomy& autonomy() const { return autonomy_; }
  MotionPlan& operator=(const MotionPlan&) = delete;

  SkeletonPtr skeleton;
  // proportions (of the skeleton: the height scale, the leg's length, the pelvis at rest)
  f64 rest_pelvis_z = 0.0;
  f64 k = 1.0;
  f64 leg_len = 0.0;
  Pose pose;
  ModelFK fk;
  WorldPose world;
  WorldPose prev_world;
  MotionInput input;
  Capabilities capabilities;
  PlanControl control;  // what the behaviours ask for (they fill it before every update)
  CrawlHands crawl_hands;
  const CollisionWorld* collision;
  Attachments props;
  HeldPropView weapon{props};
  f64 load_fraction = 0.0;
  V3 load_lean;
  std::string action_refusal;
  u64 action_serial = 0;
  GaitStyle style;
  std::vector<AnimEvent> events;  // events of the last updates (take them with take_events)
  // the character's root: ground position and facing (radians, 0 = +x, CCW; +y is yaw pi/2)
  V3 root_pos;
  f64 root_yaw = 0.0;
  f64 time = 0.0;
  // the held prop's transform (world) after update, and how it is held
  V3 weapon_pos;
  Quat weapon_rot;
  bool weapon_in_hand = false;  // the prop follows one hand (knife, lowered pistol, released long gun)
  i32 weapon_hand = H::handR;   // mirrored knife actions use H::handL
  // How hard an action drives each limb this frame (0..1; left hand, right hand, left foot, right
  // foot), and whether it strikes with it: the body tenses those muscles (a punch is not thrown
  // with a relaxed arm).
  std::array<f64, 4> effort{0.0, 0.0, 0.0, 0.0};
  std::array<bool, 4> striking{false, false, false, false};
  std::array<f64, 4> strike_weight{};  // contact window; effort can remain high in the windup
  GaitParams gait;
  V3 velocity;  // the smoothed world velocity
  Stance stance = Stance::Stand;  // the settled stance (while a transition runs: the one it comes from)
  FootPlanner feet_planner;
  ArmRig arms;
  WeaponHold hold;

  // The forward unit vector (world) of a facing yaw.
  static V3 forward(f64 yaw) { return V3{cos(yaw), sin(yaw), 0}; }
  Quat root_rot() const { return qz(root_yaw - kPi / 2.0); }
  f64 phase() const { return feet_planner.phase; }

  // Puts the character at `pos` facing `yaw`, feet planted in the default stance.
  void place(const V3& pos, f64 yaw);
  // The character's root this frame (the ground position under it, facing yaw).
  void set_root(const V3& pos, f64 yaw);
  // Moves the root without the motion counting as walking (the body was carried there: a shove,
  // a stagger, a fall).
  void carry_root(const V3& pos, f64 yaw);
  // Busy with a one-shot action.
  f64 action_time() const { return act_ ? act_->time : 0; }
  bool busy() const { return act_ && !act_->done(); }
  // The running one-shot action (empty: none).
  std::string_view action_name() const { return act_ && !act_->done() ? std::string_view(act_->def->name) : std::string_view(); }
  // The held posture (guard, idle pose, talk; empty: none).
  std::string_view pose_action_name() const { return pose_act_ && !pose_act_->done() ? std::string_view(pose_act_->def->name) : std::string_view(); }
  // Mid stance transition (or lying): the host should not move the root.
  bool transitioning() const { return stance_p_ < 1.0 || !stance_queue_.empty() || stance == Stance::Down; }
  // Lying down, or getting up from it.
  bool down() const {
    return lying_ || stance == Stance::Down || stance_to_ == Stance::Down || (!stance_queue_.empty() && get_up_run_);
  }
  // Starts a one-shot action (strike, block, reload, gesture, fidget), optionally aimed at a world
  // `target` (strikes). A held posture (guard, idle pose) goes on the pose layer. False if the body
  // cannot (down, mid transition), or there is no such action.
  bool play(std::string_view name, std::optional<V3> target = std::nullopt, f64 rate = 1.0);
  // Whether play would take the action now (false: why, as action_refusal would say). Nothing
  // changes.
  bool can_play(const ActionDef& def, std::string* why = nullptr) const;
  // The stance the plan goes to: the host's (input.stance) as far as the body allows it - kneeling
  // when the legs only half carry it, prone when they do not (crawling, immobile). The input stays
  // the host's.
  Stance effective_stance() const {
    if (capabilities.mobility == Mobility::Crawl || capabilities.mobility == Mobility::Immobile) return Stance::Prone;
    if (capabilities.mobility == Mobility::Kneel) return Stance::Kneel;
    return input.stance;
  }
  // Stops the running one-shot action (a hit interrupts it).
  void interrupt(bool hard = true);
  // Updates the target of the running action (a strike follows a moving opponent).
  void aim_action(const V3& target);
  // The events since the last call.
  std::vector<AnimEvent> take_events();
  // (the same, appended to `out`: a host that keeps its buffer allocates nothing)
  void take_events(std::vector<AnimEvent>& out);
  // The recoil of one shot.
  void fire(f64 strength = 1.0);
  // Rebind the held prop to the current planned hand without advancing motion.
  void refresh_prop();
  // Lies on the ground where the body lies (root on the ground under the pelvis, `yaw` the way the
  // feet point from the head for a body on its back, the head's way face down): the plan holds
  // the lying pose until get_up.
  void lie(const V3& root, f64 yaw, bool back);
  // A fall to lying played by the plan alone (no physics: retro frames, a body far away).
  void fall(bool back);
  // Gets up from lying: through sitting (or pushing up from the front), kneeling, to the host's
  // stance.
  void get_up(f64 rate = 1.0);
  // The progress of a stance transition 0..1 (1: settled).
  f64 stance_progress() const { return stance_p_; }
  // The ground's rise ahead along the motion (rise over run, smoothed; about 0.33 up a flight of
  // stairs, negative down one). Hosts slow down on it as people do.
  f64 slope() const { return slope_s_.x; }
  // Lying (or last lay) on the back rather than face down.
  bool lying_on_back() const { return down_back_; }
  // The stance being blended to.
  Stance stance_target() const { return stance_to_; }

  // Advances the plan by dt (s; at most 0.05 at a time): the pose the character means to have this
  // frame (pose, world; prev_world keeps the last), the feet, the held prop, the events.
  void update(f64 dt);

  // The world position of a limb's striking point (in the plan's pose).
  V3 limb_pos(Limb limb = Limb::None) const;
  // The world position of a point on the held prop (prop space), its tip say.
  V3 prop_point(const V3& p) const { return weapon_pos + rotate(weapon_rot, p); }
  // The skin matrix of the held prop (its model's single bone; 16 floats).
  void write_prop_skin(f32* out) const { write_rigid(out, weapon_pos, weapon_rot, V3{}); }
  // The feet (world), for debugging and tests.
  std::array<FootState, 2> foot_state() const { return feet_planner.state(); }
  // The world position between the eyes (in the plan's pose).
  V3 eyes() const;
  // The palm of a hand (world, in the plan's pose).
  V3 palm(Side side) const;

 private:
  struct TaskState {
    bool on = false;
    V3 pos, vel;
    std::optional<Quat> rot;
    std::optional<V3> pole;
  };
  struct PendingEvent {
    const ActionEvent* e;
    const ActionDef* def;
    std::optional<V3> target;
  };
  struct ArmReturn {
    std::array<Quat, 3> rotation{};
    std::array<V3, 3> velocity{};
    f64 weight = 0.0;
    bool releasing = false;
    bool initialized = false;
  };

  void step_in(const ActionDef& def, const V3& target, f64 rate);
  void prepare_support(const ActionDef& def, f64 duration);
  void set_pose_action(const ActionDef* def);
  void begin_transition(Stance to);
  void update_stance(f64 dt);
  f64 weight_of(Stance s) const;
  StanceSample& sample_stance(Stance s, StanceSample& out, const StanceSample& standing);
  void update_actions(f64 dt);
  std::string_view idle_pose_for(bool listening) const;
  void flush_events();
  void arm_swing(f64 dt, const GaitParams& g, bool moving, f64 speed, f64 breathe, f64 idle);
  void rest_hands(const StanceSample& s, f64 w);
  void action_hands(const ChannelFrame& ch, f64 w, const ActionPlayer& player, bool held_prop);
  void mood_arms(f64 w, f64 phase);
  void prop_in_hand(i32 b);
  // A world point in model space this frame (the root at the smoothed ground height).
  V3 to_model(const V3& w) const { return rotate(inv_, w - origin_); }

  // proportions
  Dims dims_;

  // locomotion state
  V3 last_pos_;
  f64 last_yaw_ = 0.0;
  bool placed_ = false;
  Spring3 vel_spring_{9.0, 1.0};
  V3 accel_;
  f64 yaw_rate_ = 0.0;
  Spring vis_z_{16.0, 1.0};
  Spring crouch_s_{7.0, 1.0};
  Spring lower_yaw_{5.0, 1.0};
  Spring hips_yaw_{6.0, 1.0};  // where the hips point relative to the facing (with the planted feet when standing)
  Spring pelvis_z_{26.0, 1.0};
  Spring impact_{15.0, 0.45};
  Spring trunk_lean_{6.0, 0.55};
  Spring bank_{6.0, 0.8};
  Spring slope_s_{5.0, 1.0};  // the ground's rise ahead along the motion (smoothed, rise over run; stairs up ~0.33)
  Spring swing_amp_{5.0, 1.0};
  Spring foot_width_{8.0, 1.0, 1.0};
  Spring foot_toe_out_{8.0, 1.0, 0.12};
  Spring aim_w_{8.0, 1.0};
  Spring mood_w_{6.0, 1.0};
  Spring air_w_{8.0, 1.0};
  Spring lean_s_{7.0, 1.0};
  Spring aim_yaw_{8.0, 1.0};
  Spring aim_pitch_{9.0, 1.0};
  Spring head_yaw_{5.5, 0.9};
  Spring head_pitch_{5.5, 0.9};
  Spring shift_{1.5, 1.0};
  Spring look_w_{9.0, 1.0};
  // the footfall nod of the head, the recoil of the chest (small secondary motion of the plan)
  Spring nod_{11.0, 0.42};
  Spring recoil_{14.0, 0.5};
  std::array<Spring, 2> task_w_{Spring(16.0, 1.0), Spring(16.0, 1.0)};
  std::array<TaskState, 2> task_s_;  // each hand's behaviour task as followed (smoothed; kept while it fades out)
  Mood mood_kind_ = Mood::Normal;
  f64 seed_;
  Rng rng_;

  // stances
  Stance stance_to_ = Stance::Stand;
  f64 stance_p_ = 1.0;
  f64 stance_dur_ = 0.5;
  std::vector<Stance> stance_queue_;
  // lying: on the back (else face down), and whether it stays down
  bool down_back_ = true;
  bool lying_ = false;
  bool prone_roll_ = false;
  StanceSample s_a_, s_b_, s_out_, s_c_;
  f64 crawl_phase_ = 0.0;
  bool get_up_run_ = false;
  f64 get_up_rate_ = 1.0;  // how briskly it gets up (1: the stances' own times; quicker unhurt, slower hurt)

  // actions
  std::optional<ActionPlayer> pose_act_;
  std::optional<ActionPlayer> act_;
  const ActionDef* manual_pose_ = nullptr;
  std::array<ArmReturn, 2> arm_return_{};
  ChannelFrame ch_pose_, ch_act_;
  IdleAutonomy autonomy_;
  f64 idle_time_ = 0.0;
  f64 next_fidget_ = 6.0;
  f64 next_idle_pose_ = 3.0;
  f64 next_gesture_ = 1.0;
  std::string_view idle_choice_;
  f64 armed_idle_ = 0.0;
  std::vector<PendingEvent> pending_events_;
  std::vector<const ActionEvent*> crossed_;

  // this frame's model space (update)
  Quat inv_;
  V3 origin_;
};

}  // namespace svx::anim
