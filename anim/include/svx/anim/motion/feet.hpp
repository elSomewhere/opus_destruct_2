// svx_anim — the foot planner: where the feet go, in the world.
//
// A gait clock drives the walk and the run: a foot in stance stays where it landed (no sliding at
// any speed or turn rate); a swinging foot lands where the hip will be at mid-stance of the next
// step, on the ground the CollisionWorld finds (stairs, rubble). Standing, the feet take
// unhurried corrective and turning steps.
//
// On top of the clock, the body's balance can take steps of its own (`step`): a foot is sent to a
// point now, with a duration, and the clock waits (a stagger, a catch after a trip, a step away
// from a blast). A swing clears what lies on its path: the planner samples the ground along it
// and lifts the foot over the highest point, with a margin that shrinks with inattention
// (running, panic), so a foot can still catch on a kerb or on rubble (the body's physics finds
// out).
#pragma once

#include <array>
#include <optional>
#include <span>

#include "svx/anim/motion/gait.hpp"
#include "svx/anim/motion/style.hpp"
#include "svx/anim/physics/collision.hpp"

namespace svx::anim {

struct Foot {
  i32 side = -1;  // -1 left, 1 right
  // the rig bones of the leg
  i32 thigh = 0, shin = 0, foot = 0, toe = 0;
  f64 offset = 0.0;  // the phase offset in the gait cycle
  bool planted = true;
  bool held = false;    // taken over by an action (a kick): the planter leaves it alone
  bool forced = false;  // a step the body's balance asked for (not the gait's)
  bool unloaded = false;  // this leg cannot bear weight; it cannot catch a balance step
  // where the foot rests (sole, world) and its heading
  V3 pos;
  f64 yaw = 0.0;
  // the swing: from `lift`, carried with the body from `lift_root`, to `target` over `swing` 0..1
  V3 lift, lift_root;
  f64 lift_yaw = 0.0;
  f64 lift_pitch = 0.0;
  f64 swing = 0.0;
  f64 swing_rate = 1.0;
  V3 target;
  f64 target_yaw = 0.0;
  f64 clear = 0.0;  // the extra height the swing clears (obstacles on its path)
  // the ankle (world) and its pitch (heel-toe roll) this frame
  V3 ankle;
  f64 pitch = 0.0;
  f64 since = 1.0;  // seconds since it last landed
};

struct FeetDims {
  f64 k = 1.0;
  f64 leg_len = 0.0;
  f64 ankle_h = 0.0;
  f64 ball_fwd = 0.0;
  f64 heel_back = 0.0;
  f64 foot_x = 0.0;
};

// What the planner needs to know each frame.
struct FeetContext {
  V3 root;
  f64 body_yaw = 0.0;  // the heading of the lower body
  V3 vel;
  f64 speed = 0.0;
  GaitParams gait;
  bool moving = false;
  bool airborne = false;
  f64 crouch = 0.0;
  GaitStyle style;
  std::array<f64, 2> control{1, 1};  // foot clearance falls with motor control
  std::array<f64, 2> support{1, 1};
  std::array<V3, 2> hips;  // the hip joints (world) this frame (a foot stays within the leg's reach)
  bool hold = false;       // the balance has the feet: the gait lifts none of them (only forced steps)
  f64 care = 1.0;          // 0..1: how much attention the steps get (clearance over obstacles)
  f64 ground_z = 0.0;      // the ground height of the character's root (reference for ground queries)
};

struct FootBones {
  i32 thigh = 0, shin = 0, foot = 0, toe = 0;
};

// A foot as tests and debugging see it.
struct FootState {
  bool planted = true;
  V3 pos, ankle;
  f64 yaw = 0.0;
  bool forced = false;
};

class FootPlanner {
 public:
  FootPlanner(const FeetDims& dims, const CollisionWorld* collision, const std::array<FootBones, 2>& bones);
  std::array<Foot, 2> feet;  // left, right
  FeetDims dims;
  const CollisionWorld* collision;
  // Things lying about (spheres, world): a swing clears them too, as far as it sees them (the
  // host keeps the list alive while it is set).
  std::span<const Obstacle> obstacles;
  f64 phase = 0.0;
  bool stepping = false;                // standing still, correcting the feet (unhurried steps)
  std::array<bool, 2> landed{false, false};  // landed this update (footfalls: the body settles onto the leg)

  // Where a foot stands in the default stance around a root.
  V3 nominal(const Foot& f, const V3& root, f64 yaw, f64 crouch, const GaitStyle& style) const;
  f64 ground(f64 x, f64 y, f64 z_ref, f64 fallback) const;
  // Both feet planted in the default stance around the root.
  void reset(const V3& root, f64 yaw, f64 crouch, const GaitStyle& style);
  // Plants the feet where the body's feet are (after a fall, a scramble): soles and headings.
  void place_at(const std::array<V3, 2>& soles, const std::array<f64, 2>& yaws);
  // A step the balance asks for: foot i swings from where it is to `target` (world sole) in
  // `duration` seconds, heading `yaw` (none: it keeps its heading). Sent again mid-swing, it
  // re-targets the swing (the landing point follows the body).
  void step(i32 i, const V3& target, f64 duration, std::optional<f64> yaw = std::nullopt);
  // A swing stopped short (the foot caught on something): it lands where it is.
  void plant_now(i32 i, const V3& at);
  // The height to clear over the path from a to b (m above the higher end, with a margin by care).
  f64 clearance(const V3& a, const V3& b, f64 care) const;
  // The ankle position (world) of a foot planted at `plant` with heading `yaw` and heel-toe pitch.
  V3 ankle_from_plant(const V3& plant, f64 yaw, f64 pitch) const;
  // Advances the gait clock (walking), or the corrective steps (standing). Returns the phase
  // before the advance.
  f64 advance_clock(f64 dt, const FeetContext& c, const std::array<f64, 2>& limp);
  // Moves the feet for this frame.
  void update(f64 dt, const FeetContext& c, f64 prev_phase);
  // The feet (world), for debugging and tests.
  std::array<FootState, 2> state() const;

 private:
  // Stairs and ledges: a foot lands flat on one tread, never across an edge, and a step rises or
  // drops no more than a body takes in one (two stair steps up; a shorter stride when the landing
  // would be higher or lower). Moves `tgt` back along the path from the lift-off.
  void on_tread(const Foot& f, V3& tgt, f64 yaw) const;
  bool was_airborne_ = false;
  bool settle_layout_ = false;
};

}  // namespace svx::anim
