// svx_anim — one place for prop identity, attachment occupancy and loose items.
#pragma once
#include <array>
#include <functional>
#include <map>
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
  i32 charges = 0;
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
  f64 support_gap_time = 0;  // a second hand trying, but unable, to retain its grip
  ReleaseReason last_release = ReleaseReason::Voluntary;
  u64 loose_since = 0;  // (the registry's serial when it went loose: the longest loose go first)
  ModelPtr damaged_model;
  u32 geometry_version = 0;
  f64 retained_mass = 0;  // a severed hand that still grips this item
  f64 mass_fraction = 1;  // of the archetype's material still there (damage carves it away)
  Gib* loose_body = nullptr;
  const VoxelModel& model() const { return *(damaged_model ? damaged_model : archetype->model); }
  f64 mass() const { return archetype->mass * mass_fraction + retained_mass; }
  V3 centre_of_mass() const;  // (damaged: of its cells, cached per geometry_version)

 private:
  mutable u32 centre_version_ = 0;
  mutable V3 centre_;
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
struct PropRegistryOptions {
  i32 max_loose = 256;  // loose instances kept: beyond, the longest loose goes (Gone)
  f64 kill_z = -200.0;  // a loose instance below this height goes (m)
};
class PropRegistry {
 public:
  explicit PropRegistry(const CollisionWorld* collision = nullptr, const PropRegistryOptions& o = {});
  PropRegistryOptions options;
  // Archetypes by id, as instances and their records name them: the host's own (define, or an
  // instance made from one), then `resolver`, then the built-in catalogue (prop_catalog). A record
  // whose archetype does not resolve is refused.
  void define(PropPtr archetype);
  PropPtr archetype(std::string_view id) const;
  std::function<PropPtr(std::string_view id)> resolver;
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
  // One loose instance's record (record_loose's, for one: what a region keeps of it) and back.
  std::vector<u8> record_item(const PropInstance& p) const;
  PropInstancePtr restore_item(std::span<const u8> bytes);
  void collision(const CollisionWorld* c) { loose_.collision = c; }
  i64 memory_bytes() const;

 private:
  u64 next_ = 1;
  u64 released_ = 0;  // (a serial: which loose instance went loose first)
  std::map<u64, PropInstancePtr> items_;
  std::map<std::string, PropPtr, std::less<>> defined_;
  GibSystem loose_;
  void bound_loose();
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
