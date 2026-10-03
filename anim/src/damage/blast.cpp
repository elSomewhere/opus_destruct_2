#include <numeric>

#include "svx/anim/character.hpp"

namespace svx::anim {

// A blast from its source (docs/WOUNDS.md, Blast). `fragments` fragments of `mass` thrown at
// `speed` evenly over the sphere: those within the body's solid angle hit it - the parts nearest the
// blast first - slowing to nothing at the reach (3.5 radius); inside the radius it meets as many as
// at the radius, and past kMaxFragmentHits the hits share the rest's mass. `pressure` is the peak (absolute) pressure at `radius`: its excess over the
// ambient - level inside the radius, falling as the inverse square of the distance beyond it, gone
// at the reach - loads what it meets over the positive phase (2 ms
// per metre of radius): a crush of the trunk and of each limb, and the push of the whole body (the
// pressure reflected off it: kReflection times the incident) - and within half the radius its wind
// tears off the nearest limbs (blast_tear). Every part of the effect scales with the source: an
// empty one (no overpressure, no fragment energy) does nothing.
constexpr f64 kAmbientPressure = 101325;  // Pa
constexpr i32 kMaxFragmentHits = 64;
constexpr f64 kReflection = 2.3;
WoundResult Character::blast_damage(const DamageDescriptor& source) {
  WoundResult out;
  DamageDescriptor d = source;
  d.direction = vnorm(d.direction);
  const f64 reach = d.radius * 3.5;
  const f64 distance = vdist(d.point, bounds_center());
  const bool fragments = d.fragments > 0 && d.speed > 0;
  const f64 peak = std::max(0.0, d.pressure - kAmbientPressure);
  if ((peak <= 0 && !fragments) || distance >= reach) return out;
  const bool was_alive = alive();
  const f64 before = health;
  const f64 k = motion.k;
  auto overpressure = [&](f64 r) {
    if (r >= reach) return 0.0;
    const f64 q = d.radius / std::max(r, d.radius);
    return peak * q * q;
  };
  auto add = [&](const WoundResult& r) {
    out.impulse += r.impulse;
    out.absorbed_energy += r.absorbed_energy;
    out.blocked = out.blocked || r.blocked;
    out.removed.insert(out.removed.end(), r.removed.begin(), r.removed.end());
    out.gibs.insert(out.gibs.end(), r.gibs.begin(), r.gibs.end());
  };
  if (fragments) {
    const f64 area = .7 * k * k;  // the body's frontal area (m^2)
    const f64 near = std::max(distance, d.radius);
    const f64 solid = std::min(2 * kPi, area / std::max(near * near, 1e-6));
    const i32 all = std::min(d.fragments, i32(std::floor(d.fragments * solid / (4 * kPi) + .5)));
    const i32 hits = std::min(all, kMaxFragmentHits);
    const f64 share = hits > 0 ? f64(all) / hits : 0;  // (fragments a hit stands for)
    std::array<i32, kBodyCount> order;
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](i32 a, i32 b) {
      return vdist(pose.p[size_t(kBodyBone[size_t(a)])], d.point) < vdist(pose.p[size_t(kBodyBone[size_t(b)])], d.point);
    });
    // (a steel sphere of the fragment's mass)
    const f64 diameter = clamp(dm::cbrt(6 * d.mass / (kPi * 7800)), .001, .05);
    for (i32 i = 0; i < hits; ++i) {
      const i32 part = order[size_t(i % kBodyCount)];
      if (behaviours.lost[size_t(part)]) continue;
      const V3 at = pose.p[size_t(kBodyBone[size_t(part)])];
      DamageDescriptor f;
      f.kind = DamageKind::Projectile;
      f.mass = std::min(10000.0, d.mass * share);
      f.diameter = diameter;
      f.speed = std::min(3000.0, d.speed * std::max(0.0, 1 - vdist(at, d.point) / reach));
      if (f.speed <= 0) continue;
      f.point = d.point;
      f.direction = vnorm(at - d.point, d.direction);
      f.bone = kBodyBone[size_t(part)];
      if (auto hit = raycast(f.point, f.direction, reach)) {
        f.point = hit->point;
        add(damage(f));
      }
    }
  }
  // (the wind's tear as hard as the impulse at the radius: 6 kJ for a rocket's or a grenade's,
  // 114 kPa m)
  if (peak > 0) add(blast_tear(d, distance, 6000 * std::min(4.0, peak * d.radius / 114000)));
  if (peak > 0) {
    const f64 phase = .002 * d.radius;
    auto crush = [&](i32 part, f64 area, f64 mass) {
      const i32 bone = kBodyBone[size_t(part)];
      const V3 at = part == B::chest || part == B::spine || part == B::pelvis || part == B::head ? pose.p[size_t(bone)]
                                                                                               : vlerp(pose.p[size_t(bone)], pose.tail(bone), .5);
      const f64 impulse = overpressure(vdist(at, d.point)) * area * k * k * phase;
      DamageDescriptor c;
      c.kind = DamageKind::Crush;
      c.point = at;
      c.direction = vnorm(at - d.point, d.direction);
      c.mass = mass;
      c.speed = std::min(3000.0, impulse / mass);
      c.area = area * k * k;
      c.bone = bone;
      if (c.energy() >= 1e-3) add(damage(c));  // (below a millijoule: nothing to speak of)
    };
    crush(B::chest, .25, 8);
    for (i32 part = B::upperarmL; part < kBodyCount; ++part)
      if (!behaviours.lost[size_t(part)]) crush(part, .03, std::max(.2, body.parts[size_t(part)]->mass));
    // the whole body thrown (blast_push fades its speed with the distance to its reach)
    const f64 throw_speed = overpressure(distance) * kReflection * .7 * k * k * phase / std::max(1.0, body.total_mass);
    if (throw_speed > 0) blast_push(d.point, reach, throw_speed / std::max(.05, 1 - distance / reach));
  }
  out.killed = was_alive && !alive();
  out.damage = std::max(0.0, before - health);
  return out;
}

}  // namespace svx::anim
