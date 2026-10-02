// svx_anim — injuries and physiology stay behind the capability seam.
#pragma once
#include "svx/anim/damage/capabilities.hpp"
#include "svx/anim/damage/descriptor.hpp"
#include "svx/anim/damage/mechanics.hpp"
#include "svx/anim/damage/anatomy.hpp"
#include "svx/anim/physics/debris.hpp"
#include "svx/anim/behaviour/injuries.hpp"
namespace svx::anim {
enum class BoneState : u8 { Intact, Fractured, Shattered };
enum class DeathCause : u8 { None, Brain, Heart, BloodLoss, HighSpine, MassiveTrauma };
struct PartPhysiology {
  f64 flesh = 1, muscle = 1, vessel = 0, bleeding = 0, pain = 0, nerve = 1;
  BoneState bone = BoneState::Intact;
  bool lost = false;
};
struct PersistentWound {
  i32 part = 0, bone = 0;
  V3 rest, normal;
  f64 bleeding = 0, age = 0, pain = 0;
  bool arterial = false;
};
struct PhysiologySnapshot {
  std::array<PartPhysiology, 16> parts;
  std::vector<PersistentWound> wounds;
  f64 blood = 5, shock = 0, consciousness = 1, breathing = 1, adrenaline = 0;
  DeathCause cause = DeathCause::None;
};
class DamageState {
 public:
  const Capabilities& capabilities() const { return override_ ? *override_ : cap_; }
  void override_capabilities(std::optional<Capabilities> value) { override_ = value; }
  void reaction(i32 part, const V3& local, const V3& normal, HitKind kind, f64 force);
  void old_wound(i32 part, f64 severity);
  void lost(i32 part);
  void update(f64 dt, i32 pressed_part = -1);
  void bleed(f64 dt, f64 time, const WorldPose& pose, GibSystem& effects) const;
  void apply(const VoxelModel& model, const WorldPose& pose, const DamageDescriptor& descriptor, const WoundMechanics& result);
  bool stain(VoxelModel& model, const WorldPose& pose, f64 dt, f64 time) const;
  PhysiologySnapshot inspect() const { return state_; }  // tooling snapshot, never a motor input
  std::vector<u8> record() const;
  bool restore(std::span<const u8> data);
  f64 health_fraction() const {
    f64 function = 1, burden = 0;
    for (const auto& p : state_.parts) {
      function = std::min(function, p.lost ? 0.0 : std::min(p.muscle, p.flesh));
      burden += 2 - p.muscle - p.flesh;
    }
    return clamp(state_.blood / 5 * (1 - state_.shock * .5) * (.65 + .35 * function) * exp(-.18 * burden), 0.0, 1.0);
  }
  size_t wound_count() const { return injuries_.list.size() + state_.wounds.size(); }

 private:
  Injuries injuries_;
  PhysiologySnapshot state_;
  bool physical_wounds_ = false;
  void physical_capabilities();
  Capabilities cap_;
  std::optional<Capabilities> override_;
};
}  // namespace svx::anim
