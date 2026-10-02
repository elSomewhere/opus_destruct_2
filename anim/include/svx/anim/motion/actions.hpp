// svx_anim — actions: authored motions layered on the procedural body (strikes, blocks, reloads,
// gestures, idle poses and fidgets).
//
// An action is a set of keyframed channels (curve.hpp) over its duration. Channels do not store
// joint angles of the whole body: they steer the procedural rig. Hand and foot IK targets, the
// reach towards a target (so a punch lands where the opponent's jaw is), trunk and head offsets,
// the pelvis, the held weapon. So actions blend with walking, aiming, reactions and each other,
// work at any body size, and mirror left/right.
//
// Frames: hand targets are in the CHEST frame (origin at the chest joint, axes rotating with the
// chest; x right, y forward, z up, metres at height scale 1). Foot targets are in model space
// (the character's root frame). Euler angles are degrees in joint frames (x: + tips an upright
// bone back / swings a hanging limb forward; y: roll, + to the left side down for an upright
// bone; z: yaw, + turns left). Hand orientations are relative to the canonical fist: knuckles
// forward (+y), palm down (-z).
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "svx/anim/characters/props.hpp"
#include "svx/anim/curve.hpp"

namespace svx::anim {

// (a channel is a scalar - weights, strikes, crouch, look - or a 3-vector)
enum class Channel : u8 {
  HandR,
  HandL,
  HandRw,
  HandLw,
  HandRrot,
  HandLrot,
  ElbowR,
  ElbowL,
  StrikeR,
  StrikeL,
  FootR,
  FootL,
  FootRw,
  FootLw,
  StrikeFootR,
  StrikeFootL,
  Spine,
  Chest,
  Neck,
  Head,
  ClavR,
  ClavL,
  Pelvis,
  PelvisRot,
  Crouch,
  Weapon,
  WeaponPos,
  WeaponRot,
  Look,
  FootRpole,
  FootLpole,
};
constexpr int kChannelCount = 31;

// (None: no limb - the chest)
enum class Limb : u8 { None, HandR, HandL, FootR, FootL, Blade, Muzzle };

struct ActionEvent {
  f64 t = 0.0;
  std::string name;
  Limb limb = Limb::None;
};

// Pose: held postures (idles, guard, talk); Act: one-shots on top.
enum class ActionLayer : u8 { Act, Pose };

struct ActionDef {
  std::string name;
  f64 duration = 0.0;
  bool loop = false;
  f64 fade_in = 0.12, fade_out = 0.15;  // blend in / out (s)
  std::vector<ActionEvent> events;
  ActionLayer layer = ActionLayer::Act;
  bool targeted = false;  // aimed at a target point (strikes)
  // How far (m, horizontally from the root, at body scale 1) the strike lands without moving in:
  // a target further away makes the body step into it (the pelvis drives forward with the
  // strike; punches also lean the trunk in). 0: none.
  f64 reach = 0.0;
  // Kicks: the foot's pitch (rad) at the strike (-0.6 pointed, > 0 toes up: a push kick).
  std::optional<f64> kick_pitch;
  // Needs a prop in the right hand (a knife).
  std::optional<PropKind> prop;
  f64 target_height = 1.5;  // untargeted strikes aim here, metres at height scale 1
  i32 lead_side = 0;       // combat support step: -1 left lead, +1 right lead; 0 no step
  f64 support_turn = 0.12; // outward angle of the lead foot (rad)
  bool left_handed = false; // mirrored prop actions carry the knife in the left hand

  // Sets a channel's keys (and builds its track); later keys replace earlier ones. At most 3
  // values per key are kept.
  void set(Channel c, std::vector<Key> keys);
  bool drives(Channel c) const { return (driven_ >> u32(c)) & 1u; }
  u32 driven() const { return driven_; }  // (bit per channel)
  const std::vector<Key>& keys(Channel c) const { return keys_[size_t(c)]; }
  const Track& track(Channel c) const { return tracks_[size_t(c)]; }

 private:
  u32 driven_ = 0;
  std::array<std::vector<Key>, kChannelCount> keys_;
  std::array<Track, kChannelCount> tracks_;
};

// The left/right mirror image of an action (positions x -> -x, rotations y, z -> -y, -z), named
// `name` (default: the action's name + ".m").
ActionDef mirror_action(const ActionDef& def);
ActionDef mirror_action(const ActionDef& def, std::string name);

// Sampled channel values of one frame (a channel not driven is absent).
struct ChannelFrame {
  u32 driven = 0;  // (bit per channel)
  std::array<std::array<f64, 3>, kChannelCount> v{};
  bool has(Channel c) const { return (driven >> u32(c)) & 1u; }
  // The channel's values; null when absent.
  const f64* get(Channel c) const { return has(c) ? v[size_t(c)].data() : nullptr; }
  // Component i of a channel (0 when absent).
  f64 at(Channel c, int i = 0) const { return has(c) ? v[size_t(c)][size_t(i)] : 0.0; }
  void clear() { driven = 0; }
};

// A running action: time, fade, target.
class ActionPlayer {
 public:
  explicit ActionPlayer(const ActionDef* def_, std::optional<V3> target_ = std::nullopt, f64 rate_ = 1.0)
      : def(def_), rate(rate_), target(target_) {}
  const ActionDef* def;
  f64 time = 0.0;
  f64 rate = 1.0;
  std::optional<V3> target;  // the world target point (strikes)
  f64 preparation = 0.0;    // optional lead-in at the first pose before clip time zero
  bool done() const;
  // The blend weight now (fade in, fade out at the end or after stop()).
  f64 weight() const;
  void stop();
  // Advances; the events crossed are appended to `crossed` (when given).
  void advance(f64 dt, std::vector<const ActionEvent*>* crossed = nullptr);
  // Samples every channel into `out` (the channels the action drives).
  ChannelFrame& sample(ChannelFrame& out) const;

 private:
  f64 stop_at_ = -1.0;  // stopping: fading out from here
  bool stopping_ = false;
  f64 stop_weight_ = 0.0;
  i32 fired_to_ = -1;
};

// ---- the library

// Fists up by the chin, elbows in (the fighting guard, as held by "guard").
constexpr V3 kGuardR{0.13, 0.22, 0.3};
constexpr V3 kGuardL{-0.11, 0.26, 0.33};

// Every action of the library and its mirror image (name + ".m"), in that order.
const std::vector<ActionDef>& actions();
// An action of the library by name (null: there is none).
const ActionDef* action_def(std::string_view name);

inline constexpr std::array<std::string_view, 6> kStrikes = {"jab", "cross", "hook", "uppercut", "frontKick", "roundhouse"};
inline constexpr std::array<std::string_view, 4> kKnifeAttacks = {"stab", "slash", "gutStab", "forehandSlash"};
// What an armed body does in a pause (the weapon hand stays on the weapon).
inline constexpr std::array<std::string_view, 5> kArmedFidgets = {"adjustHelmet", "wipeBrow", "rollShoulders", "checkWeapon", "lookAround"};
inline constexpr std::array<std::string_view, 6> kIdlePoses = {"armsCrossed", "pockets", "handsOnHips", "handsBehind", "handsFolded", "phone"};
inline constexpr std::array<std::string_view, 4> kFidgets = {"checkWatch", "scratchHead", "stretch", "rubNeck"};
inline constexpr std::array<std::string_view, 4> kGestures = {"gestureOpen", "gesturePoint", "shrug", "handOnChest"};

}  // namespace svx::anim
