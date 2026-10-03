// svx_anim — immutable prop archetypes. Hosts keep instance state separately.
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "svx/anim/voxel/model.hpp"

namespace svx::anim {

struct NamedPropPoint {
  std::string id;
  V3 point;
};
struct PropSocket {
  std::string id;
  V3 point;
  Quat rotation;
  f64 retention = 350.0;  // N; strap or grip failure load
};
enum class ImpactClass : u8 { Edge, Point, Blunt };
struct ContactFeature {
  std::string id;
  ImpactClass impact = ImpactClass::Blunt;
  V3 a, b;
  f64 radius = 0.02, sharpness = 0.0;
  V3 normal{1, 0, 0};  // flat-side normal for an edge; ignored for points and blunt features
};
// How the motion holds and handles a prop, authored with its archetype: the motion reads this, never
// what the archetype is.
enum class HoldStyle : u8 {
  Free,        // in the hand as it hangs, or in its carry poses
  Aimed,       // aimed from the hand - at the eyes, or at arm's length; lowered, it hangs in the hand
  Shouldered,  // aimed along its length, its butt at the shoulder and the support hand forward, bladed
};
// Where the holding hand carries it, relative to the chest (per unit of the rig's height; x towards
// the holding hand's side), and the palm's pitch (rad; none: as the hand hangs).
struct CarryPose {
  bool on = false;
  V3 offset;
  std::optional<f64> palm;
};
struct PropHold {
  HoldStyle style = HoldStyle::Free;
  bool in_hand = false;  // kept in the hand's grip, not placed by the hold (a short blade)
  // a shot (Character::fire): the motion's kick, the arms' kick back and pitch, the impulse into
  // the hand (N s; held at the shoulder, 0.8 of it also into the chest)
  f64 recoil = .35, kick_back = 1.1, kick_pitch = 7.0, shot_impulse = 2.6;
  std::string guard = "guard";  // the guard pose action it is raised in (empty: not raised)
  std::string guard_two_hands;  // ... held in both hands (empty: guard)
  CarryPose ready;              // carried at the ready (MotionInput::carry Ready) ...
  bool ready_at_aim = false;    // ... and while aiming or at the hip too
  CarryPose two_hands;          // carried in both hands, not at the ready
  bool aims() const { return style != HoldStyle::Free; }
};
struct PropMaterial {
  f64 density = 700.0;     // kg/m^3
  f64 penetration = 2e6;   // J/m^3: the energy density removing its cells takes
  f64 fracture = 150.0;    // J: what a blunt blow spends deforming it; the rest passes through
};
struct Prop {
  std::string id, name;
  ModelPtr model;
  std::vector<std::string> tags;
  f64 mass = 1.0;
  V3 centre, inertia, dimensions;
  PropMaterial material;
  PropHold hold;
  std::vector<PropSocket> sockets;
  std::vector<NamedPropPoint> points;
  std::vector<ContactFeature> features;
  std::vector<std::string> attachments{"rightHand", "leftHand", "hip", "thigh"};
  // Named points and carry tuning are authored data; no identity is inspected by the holds.
  V3 grip, support, butt, tip, reload_point;
  Quat hanging_rotation = qx(-0.9);
  f64 ready_pitch = -0.5, ready_roll = 0.3;
  V3 ready_butt_offset{0.01, 0.0, -0.05};
  bool one_handed = false;
  bool has(std::string_view tag) const;
  bool satisfies(const std::vector<std::string>& requirements) const;
  bool long_gun() const { return hold.style == HoldStyle::Shouldered; }
  const PropSocket* socket(std::string_view id) const;
  const ContactFeature* feature(std::string_view id) const;
};
using PropPtr = std::shared_ptr<const Prop>;
constexpr f64 kDefaultVoxelSize = 1.0 / 32.0;
// The library's catalogue (data/content/props.json: rifle, smg, lmg, pistol, knife, dagger, machete,
// sword, baton, bat, phone, bottle, briefcase, suitcase, backpack, shoulder_bag, shopping_bag,
// rifle_scoped), and
// one of it by id (null: none). A host's own archetypes are data too (svx/anim/content.hpp).
const std::vector<PropPtr>& prop_catalog();
PropPtr prop_archetype(std::string_view id);
// Compatibility for stored Foundry weapon values 0..5 (zero is empty).
PropPtr legacy_prop(i32 weapon);
}  // namespace svx::anim
