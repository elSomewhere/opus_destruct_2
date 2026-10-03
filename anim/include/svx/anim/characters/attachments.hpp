// svx_anim — one place for prop identity, attachment occupancy and loose items.
#pragma once
#include <map>
#include <array>
#include <span>
#include "svx/anim/characters/props.hpp"
#include "svx/anim/physics/debris.hpp"
namespace svx::anim {
enum class AttachPoint : u8 { RightHand, LeftHand, Back, Shoulder, Hip, Thigh, Chest, Head, Arms, Count };
enum class WieldStyle : u8 { OneHand, TwoHands, Reverse, Hanging, Worn, Stowed };
enum class ReleaseReason : u8 { Voluntary, Wrenched, GripFailed, KnockedOut, Death, BreakingFall, AnchorLost, StrapCut, HandDamaged };
enum class PropLocation : u8 { Attached, Loose, Gone };
const char* attachment_name(AttachPoint point);
const char* release_name(ReleaseReason reason);
bool hand_point(AttachPoint point);
i32 attachment_bone(AttachPoint point);
struct WieldProfile {
  bool left_handed = false, practised = true;
};
struct PropState {
  f64 condition = 1.0, strap = 1.0;
  i32 ammunition = 0;
  std::vector<std::string> contents;
};
struct PropInstance {
  u64 id = 0;
  PropPtr archetype;
  PropState state;
  PropLocation location = PropLocation::Loose;
  u32 character = 0;
  AttachPoint point = AttachPoint::RightHand;
  std::string socket = "primary";
  WieldStyle style = WieldStyle::OneHand;
  V3 pos, velocity, angular;  // model origin; centre-of-mass velocity; world angular velocity
  Quat rotation;
  V3 swing, swing_velocity, previous_velocity, previous_angular, filtered_acceleration, filtered_angular;
  f64 strength = 1, load = 0, impulse_load = 0;
  ReleaseReason last_release = ReleaseReason::Voluntary;
  ModelPtr damaged_model;
  u32 geometry_version = 0;
  f64 retained_mass = 0;  // a severed hand that still grips this item
  Gib* loose_body = nullptr;
  const VoxelModel& model() const { return *(damaged_model ? damaged_model : archetype->model); }
  V3 centre_of_mass() const;
};
using PropInstancePtr = std::shared_ptr<PropInstance>;
struct AttachmentEvent {
  u64 instance = 0;
  AttachPoint point;
  bool attached = false;
  ReleaseReason reason;
};
struct LoadoutEntry {
  std::string archetype;
  AttachPoint point = AttachPoint::RightHand;
  std::string socket = "primary";
  WieldStyle style = WieldStyle::OneHand;
};
class PropRegistry {
 public:
  explicit PropRegistry(const CollisionWorld* collision = nullptr);
  PropInstancePtr create(PropPtr archetype);
  PropInstancePtr get(u64 id) const;
  PropInstancePtr restore(const PropInstancePtr& saved);
  std::vector<PropInstancePtr> nearby(const V3& point, f64 radius) const;
  const std::map<u64, PropInstancePtr>& all() const { return items_; }
  void release(const PropInstancePtr& item);
  void reclaim(const PropInstancePtr& item);
  void retire(const PropInstancePtr& item);
  void update(f64 dt);
  std::vector<u8> record_loose() const;
  bool restore_loose(std::span<const u8> bytes);
  void collision(const CollisionWorld* c) { loose_.collision = c; }

 private:
  u64 next_ = 1;
  std::map<u64, PropInstancePtr> items_;
  GibSystem loose_;
};
class Attachments {
 public:
  std::shared_ptr<PropRegistry> registry;
  u64 revision = 0;
  u32 owner = 0;
  WieldProfile wield;
  std::array<PropInstancePtr, size_t(AttachPoint::Count)> slots{};
  std::vector<AttachmentEvent> events;
  std::string refusal;
  Attachments();
  // Layout validation does not allocate an instance or change its ownership.
  bool accepts(const Prop& archetype, AttachPoint point, std::string_view socket, WieldStyle style);
  bool attach(const PropInstancePtr& item, AttachPoint point, std::string_view socket, WieldStyle style);
  bool regrip(AttachPoint from, AttachPoint to, std::string_view socket, WieldStyle style, ReleaseReason reason = ReleaseReason::Voluntary);
  PropInstancePtr detach(AttachPoint point, ReleaseReason reason);
  PropInstancePtr at(AttachPoint point) const;
  PropInstancePtr held() const;
  bool free_hand(bool left) const;
  bool occupied(AttachPoint point) const;
};
// Source-compatible view of the primary held archetype. It stores no second pointer.
class HeldPropView {
 public:
  explicit HeldPropView(Attachments& attachments) : a_(&attachments) {}
  operator PropPtr() const {
    const auto p = a_->held();
    return p ? p->archetype : PropPtr{};
  }
  explicit operator bool() const { return bool(a_->held()); }
  const Prop& operator*() const { return *a_->held()->archetype; }
  const Prop* operator->() const { return a_->held()->archetype.get(); }
  HeldPropView& operator=(const PropPtr& p);
  void reset();

 private:
  Attachments* a_;
};
}  // namespace svx::anim
