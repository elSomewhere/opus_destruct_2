// Collision-aware root requests shared by tools and game hosts. Feet still choose
// their own treads; this only prevents the requested pelvis walking through a wall.
#pragma once
#include "svx/anim/character.hpp"
namespace svx::anim {
struct TravelState {
  V3 root;
  f64 yaw = kPi / 2, speed = 0, vertical_speed = 0;
  bool blocked = false;
  void update(Character& c, const CollisionWorld& world, f64 wanted_speed, f64 wanted_yaw, f64 dt) {
    const V3 motion = c.take_root_motion();
    if (c.controlled()) {
      root += motion; yaw = c.motion.root_yaw; speed = 0; vertical_speed = 0;
      return;
    }
    const bool low = c.motion.input.stance == Stance::Prone || c.capabilities().mobility >= Mobility::Crawl;
    const f64 turn = low ? .65 : 2.4;
    yaw += clamp(wrap_angle(wanted_yaw - yaw), -turn * dt, turn * dt);
    f64 target = c.motion.transitioning() || int(c.motion.input.stance) >= 3 ? 0 :
        std::min(wanted_speed, c.capabilities().max_speed) / (1 + .35 * c.motion.load_fraction);
    const f64 k = c.motion.k;
    const V3 forward{cos(yaw), sin(yaw), 0};
    const f64 probe = .28 * k + std::max(speed, target) * .25;
    const V3 ahead = root + forward * probe;
    const auto ground = world.ground_height(ahead.x, ahead.y, root.z + .32 * k, root.z - .65 * k);
    blocked = !ground;
    for (f64 height : {low ? .18 : .5, low ? .3 : 1.1}) {
      const f64 hit = world.raycast(root + V3{0, 0, height * k}, forward, probe);
      blocked |= hit >= 0;
    }
    if (blocked) target = 0;
    if (ground && std::abs(*ground - root.z) > .08 * k)
      target = std::min(target, (std::abs(*ground - root.z) > .16 * k ? .65 : 1.0) * k);
    speed += clamp(target - speed, -4 * dt, 2.5 * dt);
    V3 next = root + forward * (speed * dt);
    const auto support = world.ground_height(next.x, next.y, root.z + .30 * k, root.z - 1.2 * k);
    if (support) {
      // The motion plan smooths pelvis height. Keep this physical root on its
      // support, including the first stair; never integrate through a riser.
      next.z = *support; vertical_speed = 0;
    } else {
      vertical_speed -= 9.81 * dt; next.z += vertical_speed * dt;
    }
    root = next;
  }
};
}  // namespace svx::anim
