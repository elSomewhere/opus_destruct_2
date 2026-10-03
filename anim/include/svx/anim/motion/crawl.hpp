// Free hands pulling a prone body: planted in world space, one recovering at a
// time. The plan owns contact intent; the physical hand may yield or miss it.
#pragma once
#include <array>
#include "svx/anim/physics/collision.hpp"

namespace svx::anim {

struct CrawlHand {
  V3 pos, lift, target;
  bool active = false, planted = false;
  f64 progress = 1, age = 0;
};

class CrawlHands {
 public:
  std::array<CrawlHand, 2> hands{};

  void reset() { hands = {}; next_ = 0; }

  void update(f64 dt, const V3& root, f64 yaw, f64 k, const V3& velocity,
              const std::array<bool, 2>& usable, const std::array<V3, 2>& initial, const CollisionWorld& collision) {
    const Quat q = qz(yaw - kPi / 2), inv = conj(q);
    const f64 speed = hypot2(velocity.x, velocity.y);
    bool swinging = false;
    for (size_t side = 0; side < 2; ++side) {
      auto& h = hands[side];
      if (!usable[side]) { h = {}; continue; }
      if (!h.active) {
        const V3 at = initial[side];
        const auto z = collision.ground_height(at.x, at.y, root.z + .28 * k, root.z - .3 * k);
        if (!z) continue;
        h.pos = {at.x, at.y, *z + .06 * k};
        h.active = h.planted = true;
        h.progress = 1;
      }
      if (h.planted && !collision.ground_height(h.pos.x, h.pos.y, h.pos.z + .12 * k, h.pos.z - .18 * k)) {
        h = {};
        continue;
      }
      h.age += dt;
      if (!h.planted) {
        h.progress = std::min(1.0, h.progress + dt / .38);
        const f64 u = h.progress, w = u * u * u * (10 + u * (-15 + 6 * u));
        h.pos = vlerp(h.lift, h.target, w);
        h.pos.z += .075 * k * 16 * u * u * (1 - u) * (1 - u);
        if (u >= 1) { h.planted = true; h.age = 0; }
        else swinging = true;
      }
    }
    if (swinging) return;
    // Replant the trailing hand. A short double-support interval lets the new
    // hand take the load before the other lifts, including a turn in place.
    for (size_t n = 0; n < 2; ++n) {
      const size_t side = (next_ + n) % 2;
      auto& h = hands[side];
      if (!h.active || h.age < .16) continue;
      const auto& other = hands[1 - side];
      if (other.active && other.age < .16) continue;
      const V3 local = rotate(inv, h.pos - root);
      const f64 x = (side == 0 ? -.22 : .22) * k;
      if (local.y > .5 * k && local.y < .82 * k && std::abs(local.x - x) < .13 * k) continue;
      const V3 wanted = root + rotate(q, V3{x, (.68 + std::min(.08, speed * .2)) * k, 0});
      const auto z = collision.ground_height(wanted.x, wanted.y, root.z + .28 * k, root.z - .3 * k);
      if (!z) continue;  // A hand cannot pull against a hole.
      h.lift = h.pos;
      h.target = {wanted.x, wanted.y, *z + .06 * k};
      h.progress = 0;
      h.planted = false;
      next_ = 1 - side;
      break;
    }
  }

 private:
  size_t next_ = 0;
};

}  // namespace svx::anim
