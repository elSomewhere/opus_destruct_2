// svx_anim — damped springs for secondary motion (hit flinches, recoil, lags, smoothing): exact
// integration of x'' = -w^2 (x - target) - 2 z w x', stable at any time step.
#pragma once

#include "svx/anim/math.hpp"

namespace svx::anim {

// One spring coordinate advanced by dt: its new position and velocity.
inline void spring_step(f64& x, f64& v, f64 target, f64 omega, f64 zeta, f64 dt) {
  if (dt <= 0.0) return;
  const f64 y = x - target;
  if (zeta >= 1.0) {
    // (critically - or over - damped: taken as critical)
    const f64 e = exp(-omega * dt);
    const f64 c = v + omega * y;
    const f64 ny = (y + c * dt) * e;
    const f64 nv = (c - omega * (y + c * dt)) * e;
    x = ny + target;
    v = nv;
    return;
  }
  const f64 wd = omega * std::sqrt(1.0 - zeta * zeta);
  const f64 e = exp(-zeta * omega * dt);
  const f64 cs = cos(wd * dt), sn = sin(wd * dt);
  const f64 b = (v + zeta * omega * y) / wd;
  const f64 ny = e * (y * cs + b * sn);
  const f64 nv = e * ((v + zeta * omega * y) * cs - y * wd * sn) - zeta * omega * ny;
  x = ny + target;
  v = nv;
}

// A scalar spring.
struct Spring {
  f64 x = 0.0, v = 0.0, omega = 12.0, zeta = 1.0;
  Spring() = default;
  Spring(f64 omega_, f64 zeta_, f64 x_ = 0.0) : x(x_), omega(omega_), zeta(zeta_) {}
  f64 update(f64 target, f64 dt) {
    spring_step(x, v, target, omega, zeta, dt);
    return x;
  }
  void kick(f64 dv) { v += dv; }
};

// A 3-vector spring.
struct Spring3 {
  V3 x, v;
  f64 omega = 12.0, zeta = 1.0;
  Spring3() = default;
  Spring3(f64 omega_, f64 zeta_) : omega(omega_), zeta(zeta_) {}
  const V3& update(const V3& target, f64 dt) {
    for (int a = 0; a < 3; ++a) spring_step(x[a], v[a], target[a], omega, zeta, dt);
    return x;
  }
  void kick(const V3& dv, f64 k = 1.0) { v += dv * k; }
  void reset(const V3& to = V3{}) {
    x = to;
    v = V3{};
  }
};

}  // namespace svx::anim
