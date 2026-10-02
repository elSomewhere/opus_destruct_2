// svx_anim — injuries: where a body was hurt and how badly, and what that does to how it holds
// itself.
//
// Every wound is kept on the physical body part it hit (a point in the part's frame), with a
// severity that stings at first and settles into a lasting share: a leg wound leaves a limp and a
// weak knee, an arm wound a weak arm, a trunk wound a hunch and pain; hands go to fresh wounds and
// keep pressing on serious ones.
#pragma once

#include <deque>

#include "svx/anim/math.hpp"

namespace svx::anim {

enum class HitKind : u8 { Bullet, Blunt, Blade, Blast };
enum class Zone : u8 { Head, Chest, Gut, Pelvis, ArmL, ArmR, LegL, LegR };
const char* zone_name(Zone z);

struct HitInfo {
  V3 point, dir;  // where the blow lands and the direction it travels (world)
  f64 force = 1.0;  // ~1 a rifle round or a punch; 0.6 a pistol round; 1.8 a kick; 2.5 a shotgun; blasts up to 6
  HitKind kind = HitKind::Bullet;
  i32 bone = -1;  // the rig bone struck, if known (-1: the nearest to the point)
  f64 impulse_ns = -1.0; // explicit momentum transfer in N s; negative selects the kind's default
};

// Default momentum is separate from injury severity. A bullet does not throw
// the character backwards with an extra, randomly selected whole-body shove.
inline f64 default_hit_impulse(HitKind kind, f64 force) {
  const f64 f = std::isfinite(force) ? clamp(force, 0.0, 8.0) : 0.0;
  return kind == HitKind::Bullet ? 5.0 * std::sqrt(f) : kind == HitKind::Blunt ? 34.0 * f : kind == HitKind::Blade ? 9.0 * f : 55.0 * f;
}

struct Injury {
  i32 part = 0;  // physical body part and the wound's point on it (body frame, relative to its centre of mass)
  V3 local;
  V3 normal;  // surface normal at the wound (body frame)
  Zone zone = Zone::Chest;
  HitKind kind = HitKind::Bullet;
  f64 severity = 0.0;  // 0..1 now (stings, then settles to `lasting`)
  f64 lasting = 0.0;
  f64 age = 0.0;        // seconds since it happened
  f64 hold_until = 0.0;  // a hand stays on it until this age (s)
};

Zone zone_of_part(i32 part);

// The injuries of one body and their summary.
class Injuries {
 public:
  std::deque<Injury> list;
  // summaries (0..1), updated by `update`
  f64 legL = 0.0, legR = 0.0, armL = 0.0, armR = 0.0, trunk = 0.0, head = 0.0;
  f64 pain = 0.0;  // overall pain (0..1): hunched, slower, breathing hard
  void add(const Injury& i);
  void update(f64 dt);
  const Injury* to_hold() const;  // the wound a hand should be on now (the worst fresh or serious one), or null
};

}  // namespace svx::anim
