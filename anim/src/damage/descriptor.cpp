#include "svx/anim/damage/descriptor.hpp"
namespace svx::anim {
bool DamageDescriptor::valid() const {
  if (size_t(kind) > size_t(DamageKind::Thermal) || size_t(construction) > 2 || bone < -1 || bone > 22 || fragments < 0 || fragments > 256) return false;
  for (f64 v : {point.x,  point.y, point.z, direction.x, direction.y, direction.z, edge_a.x,  edge_a.y,     edge_a.z, edge_b.x, edge_b.y,
                edge_b.z, mass,    speed,   diameter,    area,        sharpness,   alignment, swept_length, duration, radius,   pressure, impact_scale})
    if (!std::isfinite(v)) return false;
  return mass > 0 && mass <= 10000 && speed >= 0 && speed <= 3000 && diameter > 0 && diameter <= 1 && area > 0 && area <= 10 && sharpness >= 0 &&
         sharpness <= 1 && alignment >= 0 && alignment <= 1 && swept_length >= 0 && swept_length <= 5 && duration > 0 && duration <= 10 && radius > 0 &&
         radius <= 100 && pressure >= 0 && pressure <= 1e8 && impact_scale >= 1 && impact_scale <= 50 && norm(direction) > 1e-8;
}
}  // namespace svx::anim
