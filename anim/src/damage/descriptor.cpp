#include "svx/anim/damage/descriptor.hpp"
namespace svx::anim {
V3 DamageDescriptor::pellet(i32 i) const {
  const V3 dir = vnorm(direction);
  const V3 u = vnorm(cross(V3{0, 0, 1}, dir), V3{1, 0, 0}), v = cross(dir, u);  // (a shot along -y: +x, +z)
  u64 z = u64(seed) + 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  const f64 phase = seed ? f64((z ^ (z >> 31)) >> 11) * (2 * kPi / 9007199254740992.0) : 0.0;
  const f64 angle = phase + i * 2.399963229728653, radius = spread * std::sqrt(f64(i) / std::max(1, pellets));
  return vnorm(dir + u * (cos(angle) * radius) + v * (sin(angle) * radius));
}
bool DamageDescriptor::valid() const {
  if (size_t(kind) > size_t(DamageKind::Thermal) || size_t(construction) > 2 || bone < -1 || bone > 22 || fragments < 0 || fragments > 100000 || pellets < 1 ||
      pellets > 64)
    return false;
  for (f64 v : {point.x,  point.y, point.z, direction.x, direction.y, direction.z, edge_a.x,  edge_a.y,     edge_a.z, edge_b.x, edge_b.y,
                edge_b.z, mass,    speed,   diameter,    area,        sharpness,   alignment, swept_length, duration, radius,   pressure, impact_scale, spread})
    if (!std::isfinite(v)) return false;
  return mass > 0 && mass <= 10000 && speed >= 0 && speed <= 3000 && diameter > 0 && diameter <= 1 && area > 0 && area <= 10 && sharpness >= 0 &&
         sharpness <= 1 && alignment >= 0 && alignment <= 1 && swept_length >= 0 && swept_length <= 5 && duration > 0 && duration <= 10 && radius > 0 &&
         radius <= 100 && pressure >= 0 && pressure <= 1e8 && impact_scale >= 1 && impact_scale <= 50 && spread >= 0 && spread <= .5 && norm(direction) > 1e-8;
}
}  // namespace svx::anim
