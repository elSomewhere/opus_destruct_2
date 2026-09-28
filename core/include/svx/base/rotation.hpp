// structvox — rotations from and to rotation vectors, with the bundled deterministic math (the
// same bits on every platform): kinematic drives, joints' angles.
#pragma once

#include "svx/base/dmath.hpp"
#include "svx/base/vec.hpp"

namespace svx {

// The rotation vector (axis x angle) of a unit quaternion, the short way round.
inline V3 rotation_vector(Quat d) {
  if (d.w < 0.0) d = Quat{-d.x, -d.y, -d.z, -d.w};
  const f64 s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  if (!(s > 0.0)) return V3{};
  return V3{d.x, d.y, d.z} * (2.0 * dm::atan2(s, d.w) / s);
}

// The rotation of rotation vector r.
inline Quat rotation_of(const V3& r) {
  const f64 a = norm(r);
  if (!(a > 0.0)) return Quat{};
  const f64 s = dm::sin(0.5 * a) / a;
  return Quat{r.x * s, r.y * s, r.z * s, dm::cos(0.5 * a)};
}

// The signed angle from a to b about the unit axis n (a and b need not be square to it).
inline f64 angle_about(const V3& a, const V3& b, const V3& n) { return dm::atan2(dot(cross(a, b), n), dot(a, b)); }

}  // namespace svx
