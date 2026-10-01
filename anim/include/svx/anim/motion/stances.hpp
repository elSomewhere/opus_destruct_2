// svx_anim — body stances besides standing: kneeling, prone (and crawling), sitting on a seat
// (upright, leaning back, legs crossed, elbows on the knees, at a desk), sitting on the ground
// (cross-legged, knees up, legs out) and lying knocked down (on the back or the front).
//
// A stance is sampled as a StanceSample in model space (the character's root frame): the pelvis
// transform, base rotations of the trunk joints, the feet (ankle targets, foot rotations, knee
// poles, toe bend) and where free hands rest. Samples blend linearly, so a transition is a blend
// of two stances over time (the plan runs the transitions, through intermediate stances: stand
// -> kneel -> prone). Aim, look, actions and reactions are layered on top.
#pragma once

#include <array>
#include <optional>
#include <vector>

#include "svx/anim/math.hpp"

namespace svx::anim {

enum class Stance : u8 { Stand, Kneel, Prone, Sit, Ground, Down };
enum class SitVariant : u8 { Upright, LeanBack, CrossLegs, ElbowsOnKnees, Desk };
enum class GroundVariant : u8 { Cross, KneesUp, LegsOut };

struct FootPose {
  V3 ankle;
  Quat rot;
  V3 pole{0, 1, 0};
  f64 toe = 0.0;  // toe bend (rad, local x)
};

struct StanceSample {
  V3 pelvis_pos;
  Quat pelvis_rot, spine, chest, neck, head;
  std::array<FootPose, 2> feet;               // left, right
  std::array<std::optional<V3>, 2> hands;     // where free hands rest (model-space palm targets); none: they hang free
  f64 turn = 1.0;                             // how much the trunk may turn to aim / look (1 standing)
};

// A sample at rest (the pelvis at the origin, joints straight, hands free).
inline StanceSample new_sample() { return StanceSample{}; }

// out = a + (b - a) w (quaternions nlerp'd; free hands blend when both rest).
StanceSample& blend_samples(const StanceSample& a, const StanceSample& b, f64 w, StanceSample& out);

struct Dims {
  f64 k = 1.0;  // height scale (1 = 1.78 m)
  f64 ankle_h = 0.0;
  f64 foot_x = 0.0;
};

// Kneeling on the right knee, left foot forward (a firing position).
StanceSample& kneel_sample(const Dims& d, StanceSample& out);
// Prone, face down along +y, raised on the elbows. `crawl` 0..1 blends in the crawl cycle at
// phase `phase` (a commando crawl: knee drawn up on one side, the opposite elbow forward).
StanceSample& prone_sample(const Dims& d, f64 crawl, f64 phase, StanceSample& out);

struct SeatModel {
  V3 pos;                   // the seat surface centre (model space)
  bool backrest = false;
  std::optional<f64> desk;  // the desk top height (model z)
  SitVariant variant = SitVariant::Upright;
};

// Sitting on a seat behind the root (the feet stay where the character stood).
StanceSample& sit_sample(const Dims& d, const SeatModel& seat, f64 t, StanceSample& out);
// Sitting on the ground at the root.
StanceSample& ground_sample(const Dims& d, GroundVariant variant, f64 t, StanceSample& out);
// Lying knocked down: on the back (head towards -y) or face down (head towards +y).
StanceSample& down_sample(const Dims& d, bool back, f64 t, StanceSample& out);

// Transition durations (s).
f64 transition_time(Stance from, Stance to);
// Intermediate stances from `from` to `to` through the stance graph (the path excludes `from`).
std::vector<Stance> stance_route(Stance from, Stance to);

}  // namespace svx::anim
