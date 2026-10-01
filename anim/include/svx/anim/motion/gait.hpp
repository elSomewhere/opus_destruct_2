// svx_anim — gait parameters as functions of speed and crouch: a human walk (inverted pendulum,
// double support) blends into a run (spring-mass, flight phase) between ~2 and ~3 m/s. The
// numbers follow normal-gait data: cadence and stride length rise with speed, the stance
// fraction (duty factor) falls from ~0.62 walking to ~0.33 sprinting.
#pragma once

#include "svx/anim/math.hpp"

namespace svx::anim {

struct GaitParams {
  f64 freq = 0.0;       // gait cycles per second (a cycle is two steps)
  f64 duty = 0.0;       // the fraction of the cycle a foot is on the ground
  f64 run = 0.0;        // 0 walking .. 1 running
  f64 lift = 0.0;       // peak height of the swinging foot (m)
  f64 bob = 0.0;        // vertical pelvis oscillation (m)
  f64 sway = 0.0;       // lateral pelvis sway (m)
  f64 hip_yaw = 0.0;    // pelvis yaw and roll amplitudes (rad)
  f64 hip_roll = 0.0;
  f64 lean = 0.0;       // forward trunk lean (rad)
  f64 arm_swing = 0.0;  // arm swing amplitude and elbow bend (rad)
  f64 elbow = 0.0;
  f64 sink = 0.0;       // knee bend: how far the pelvis sits below standing height (m)
};

// speed: ground speed (m/s); crouch: 0 standing .. 1 crouched; scale: the character's height
// scale (1 = 1.78 m: strides scale with leg length).
GaitParams gait_for(f64 speed, f64 crouch, f64 scale = 1.0);

}  // namespace svx::anim
