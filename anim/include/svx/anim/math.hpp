// svx_anim — math for animation (docs/ANIM.md): the core's V3 and Quat (svx/base/vec.hpp) with
// the helpers the motion, the body and the behaviours are written with. Quaternions are
// Hamilton, active (v' = q v q*), q1 * q2 applies q2 first. Model space: +x right, +y forward, +z
// up; a facing yaw of 0 is +x.
//
// Deterministic: the transcendental functions are the core's bundled ones (svx::dm), so a
// character does the same on every platform, as the physics does; its randomness is seeded.
#pragma once

#include <cmath>
#include <cstdint>

#include "svx/base/dmath.hpp"
#include "svx/base/vec.hpp"

namespace svx::anim {

constexpr f64 kPi = 3.141592653589793;
constexpr f64 kTau = 6.283185307179586;
constexpr f64 kDeg = kPi / 180.0;

// ---- scalars

inline f64 clamp(f64 x, f64 lo, f64 hi) { return x < lo ? lo : x > hi ? hi : x; }
inline f64 lerp(f64 a, f64 b, f64 t) { return a + (b - a) * t; }
inline f64 smoothstep(f64 e0, f64 e1, f64 x) {
  const f64 t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}
// A smooth 0..1..0 bump over t in [0, 1].
inline f64 bump(f64 t) {
  const f64 s = clamp(t, 0.0, 1.0);
  return 16.0 * s * s * (1.0 - s) * (1.0 - s);
}
// An angle into (-pi, pi].
inline f64 wrap_angle(f64 a) {
  f64 x = std::fmod(a, kTau);
  if (x > kPi) x -= kTau;
  else if (x <= -kPi) x += kTau;
  return x;
}
inline f64 fract(f64 x) { return x - std::floor(x); }
inline f64 sign(f64 x) { return x > 0.0 ? 1.0 : x < 0.0 ? -1.0 : 0.0; }
inline f64 hypot2(f64 a, f64 b) { return std::sqrt(a * a + b * b); }
inline f64 hypot3(f64 a, f64 b, f64 c) { return std::sqrt(a * a + b * b + c * c); }
inline f64 sin(f64 x) { return dm::sin(x); }
inline f64 cos(f64 x) { return dm::cos(x); }
inline f64 atan2(f64 y, f64 x) { return dm::atan2(y, x); }
inline f64 exp(f64 x) { return dm::exp(x); }
inline f64 pow(f64 x, f64 y) { return dm::pow(x, y); }  // (x >= 0)
inline f64 acos(f64 x) {
  x = clamp(x, -1.0, 1.0);
  return dm::atan2(std::sqrt((1.0 - x) * (1.0 + x)), x);
}
inline f64 asin(f64 x) {
  x = clamp(x, -1.0, 1.0);
  return dm::atan2(x, std::sqrt((1.0 - x) * (1.0 + x)));
}

// ---- vectors

inline V3 vlerp(const V3& a, const V3& b, f64 t) { return a + (b - a) * t; }
inline f64 vdist(const V3& a, const V3& b) { return norm(a - b); }
// The unit vector along a; `fallback` when a is (nearly) zero.
inline V3 vnorm(const V3& a, const V3& fallback = V3{0, 0, 1}) {
  const f64 l = norm(a);
  return l < 1e-12 ? fallback : a * (1.0 / l);
}
// The part of a square to the unit vector n.
inline V3 vreject(const V3& a, const V3& n) { return a - n * dot(a, n); }
// a + b k
inline V3 vmadd(const V3& a, const V3& b, f64 k) { return a + b * k; }

// ---- quaternions

inline f64 qdot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline Quat qnormalize(const Quat& a) {
  const f64 l = std::sqrt(qdot(a, a));
  if (l < 1e-12) return Quat{};
  return Quat{a.x / l, a.y / l, a.z / l, a.w / l};
}
// A turn of `angle` about the unit `axis`.
inline Quat qaxis(const V3& axis, f64 angle) {
  const f64 s = sin(angle / 2.0);
  return Quat{axis.x * s, axis.y * s, axis.z * s, cos(angle / 2.0)};
}
inline Quat qx(f64 angle) { return Quat{sin(angle / 2.0), 0.0, 0.0, cos(angle / 2.0)}; }
inline Quat qy(f64 angle) { return Quat{0.0, sin(angle / 2.0), 0.0, cos(angle / 2.0)}; }
inline Quat qz(f64 angle) { return Quat{0.0, 0.0, sin(angle / 2.0), cos(angle / 2.0)}; }
// A joint rotation from angles in the joint's frame (x right, y forward, z up): roll about y
// first, then pitch about x, then yaw about z (qz qx qy). +x pitch swings a hanging limb
// forward, +y roll swings it to -x, +z yaw turns it to the left.
inline Quat qeuler(f64 x, f64 y, f64 z) {
  const f64 cx = cos(x / 2.0), sx = sin(x / 2.0);
  const f64 cy = cos(y / 2.0), sy = sin(y / 2.0);
  const f64 cz = cos(z / 2.0), sz = sin(z / 2.0);
  const f64 ax = cz * sx, ay = sz * sx, az = sz * cx, aw = cz * cx;
  return Quat{ax * cy - az * sy, aw * sy + ay * cy, az * cy + ax * sy, aw * cy - ay * sy};
}
// Normalized linear interpolation along the shorter arc.
inline Quat qnlerp(const Quat& a, const Quat& b, f64 t) {
  const f64 s = qdot(a, b) < 0.0 ? -1.0 : 1.0;
  return qnormalize(Quat{a.x + (s * b.x - a.x) * t, a.y + (s * b.y - a.y) * t, a.z + (s * b.z - a.z) * t, a.w + (s * b.w - a.w) * t});
}
// Spherical linear interpolation along the shorter arc.
inline Quat qslerp(const Quat& a, const Quat& b, f64 t) {
  f64 d = qdot(a, b), s = 1.0;
  if (d < 0.0) {
    d = -d;
    s = -1.0;
  }
  if (d > 0.9995) return qnlerp(a, b, t);
  const f64 th = acos(d), sn = sin(th);
  const f64 wa = sin((1.0 - t) * th) / sn, wb = s * sin(t * th) / sn;
  return Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}
// The shortest turn taking unit vector a to unit vector b.
inline Quat qfrom_to(const V3& a, const V3& b) {
  const f64 d = dot(a, b);
  if (d < -0.999999) {
    V3 ax{0.0, -a.z, a.y};
    if (norm2(ax) < 1e-8) ax = V3{a.z, 0.0, -a.x};
    ax = normalized(ax);
    return Quat{ax.x, ax.y, ax.z, 0.0};
  }
  const V3 c = cross(a, b);
  return qnormalize(Quat{c.x, c.y, c.z, 1.0 + d});
}
// The rotation vector (axis x angle) of q, the short way round; and back.
inline V3 qlog(Quat q) {
  if (q.w < 0.0) q = Quat{-q.x, -q.y, -q.z, -q.w};
  const f64 s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
  if (s < 1e-9) return V3{2.0 * q.x, 2.0 * q.y, 2.0 * q.z};
  const f64 k = 2.0 * atan2(s, q.w) / s;
  return V3{q.x * k, q.y * k, q.z * k};
}
inline Quat qexp(const V3& r) {
  const f64 a = norm(r);
  if (a < 1e-9) return qnormalize(Quat{r.x / 2.0, r.y / 2.0, r.z / 2.0, 1.0});
  const f64 s = sin(a / 2.0) / a;
  return Quat{r.x * s, r.y * s, r.z * s, cos(a / 2.0)};
}
// The rotation vector of q t^-1: how far q is turned past t (world).
inline V3 qerror(const Quat& q, const Quat& t) { return qlog(q * conj(t)); }
// The rotation whose columns are the right-handed orthonormal basis (x, y, z).
inline Quat qfrom_basis(const V3& x, const V3& y, const V3& z) {
  const f64 m00 = x.x, m10 = x.y, m20 = x.z, m01 = y.x, m11 = y.y, m21 = y.z, m02 = z.x, m12 = z.y, m22 = z.z;
  const f64 tr = m00 + m11 + m22;
  Quat q;
  if (tr > 0.0) {
    const f64 s = std::sqrt(tr + 1.0) * 2.0;
    q = Quat{(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s};
  } else if (m00 > m11 && m00 > m22) {
    const f64 s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
    q = Quat{0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
  } else if (m11 > m22) {
    const f64 s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
    q = Quat{(m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s};
  } else {
    const f64 s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
    q = Quat{(m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s};
  }
  return qnormalize(q);
}
// A joint rotation mirrored across the character's sagittal plane (x -> -x).
inline Quat qmirror_x(const Quat& a) { return Quat{a.x, -a.y, -a.z, a.w}; }
// The heading (yaw about +z) of the rotated +y axis.
inline f64 qyaw(const Quat& q) {
  const V3 f = rotate(q, V3{0, 1, 0});
  return atan2(f.y, f.x) - kPi / 2.0;
}

// ---- randomness (seeded: a character's own)

// A 32-bit hash of a lattice point to [0, 1).
inline f64 hash3(i32 i, i32 j, i32 k, i32 seed = 0) {
  u32 h = (static_cast<u32>(i) * 0x27d4eb2du) ^ (static_cast<u32>(j) * 0x165667b1u) ^ (static_cast<u32>(k) * 0x9e3779b1u) ^ (static_cast<u32>(seed) * 0x85ebca6bu);
  h = (h ^ (h >> 15)) * 0x2c1b3c6du;
  h = (h ^ (h >> 12)) * 0x297a2d39u;
  return static_cast<f64>(h ^ (h >> 15)) / 4294967296.0;
}

// A small seeded generator (mulberry32).
class Rng {
 public:
  explicit Rng(u32 seed) : s_(seed != 0 ? seed : 0x9e3779b9u) {}
  explicit Rng(f64 seed) : Rng(to_u32(seed)) {}
  // (a number as a 32-bit seed: truncated, modulo 2^32)
  static u32 to_u32(f64 x) {
    if (!std::isfinite(x)) return 0;
    f64 m = std::fmod(std::trunc(x), 4294967296.0);
    if (m < 0.0) m += 4294967296.0;
    return static_cast<u32>(m);
  }
  // Uniform in [0, 1).
  f64 next() {
    u32 t = (s_ += 0x6d2b79f5u);
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return static_cast<f64>(t ^ (t >> 14)) / 4294967296.0;
  }
  f64 range(f64 a, f64 b) { return a + (b - a) * next(); }
  i32 integer(i32 a, i32 b) { return a + static_cast<i32>(std::floor(next() * (b - a + 1))); }
  template <class T, size_t N>
  const T& pick(const T (&list)[N]) {
    return list[static_cast<size_t>(std::floor(next() * static_cast<f64>(N)))];
  }
  template <class V>
  const typename V::value_type& pick(const V& list) {
    return list[static_cast<size_t>(std::floor(next() * static_cast<f64>(list.size())))];
  }
  bool chance(f64 p) { return next() < p; }
  u32 state() const { return s_; }

 private:
  u32 s_;
};

// Smooth 3D value noise in [0, 1) with feature size `scale` (m).
inline f64 value_noise(f64 x, f64 y, f64 z, f64 scale, i32 seed = 0) {
  const f64 fx = x / scale, fy = y / scale, fz = z / scale;
  const f64 ix = std::floor(fx), iy = std::floor(fy), iz = std::floor(fz);
  const f64 tx = fx - ix, ty = fy - iy, tz = fz - iz;
  const f64 sx = tx * tx * (3.0 - 2.0 * tx), sy = ty * ty * (3.0 - 2.0 * ty), sz = tz * tz * (3.0 - 2.0 * tz);
  f64 v = 0.0;
  for (int c = 0; c < 8; ++c) {
    const int dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
    const f64 w = (dx ? sx : 1.0 - sx) * (dy ? sy : 1.0 - sy) * (dz ? sz : 1.0 - sz);
    v += w * hash3(static_cast<i32>(ix) + dx, static_cast<i32>(iy) + dy, static_cast<i32>(iz) + dz, seed);
  }
  return v;
}

// ---- matrices (skinning output)

// The rigid transform x -> q (x - pivot) + t, column-major (16 floats at out): a bone's skin
// matrix, its rest head `pivot`, now at `t` turned by `q`.
inline void write_rigid(f32* out, const V3& t, const Quat& q, const V3& pivot = V3{}) {
  const f64 x = q.x, y = q.y, z = q.z, w = q.w;
  const f64 r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
  const f64 r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
  const f64 r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
  out[0] = static_cast<f32>(r00);
  out[1] = static_cast<f32>(r10);
  out[2] = static_cast<f32>(r20);
  out[3] = 0.0f;
  out[4] = static_cast<f32>(r01);
  out[5] = static_cast<f32>(r11);
  out[6] = static_cast<f32>(r21);
  out[7] = 0.0f;
  out[8] = static_cast<f32>(r02);
  out[9] = static_cast<f32>(r12);
  out[10] = static_cast<f32>(r22);
  out[11] = 0.0f;
  out[12] = static_cast<f32>(t.x - (r00 * pivot.x + r01 * pivot.y + r02 * pivot.z));
  out[13] = static_cast<f32>(t.y - (r10 * pivot.x + r11 * pivot.y + r12 * pivot.z));
  out[14] = static_cast<f32>(t.z - (r20 * pivot.x + r21 * pivot.y + r22 * pivot.z));
  out[15] = 1.0f;
}

}  // namespace svx::anim
