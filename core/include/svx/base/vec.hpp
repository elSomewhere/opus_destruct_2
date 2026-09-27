// structvox — small f64 vector / quaternion / 3x3 matrix helpers (no transcendental functions:
// bit-identical native and WASM).
#pragma once

#include <array>
#include <cmath>

#include "svx/base/types.hpp"

namespace svx {

struct V3 {
  f64 x = 0, y = 0, z = 0;
  constexpr V3() = default;
  constexpr V3(f64 a, f64 b, f64 c) : x(a), y(b), z(c) {}
  f64& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
  f64 operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};

inline V3 operator+(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator-(const V3& a) { return {-a.x, -a.y, -a.z}; }
inline V3 operator*(const V3& a, f64 s) { return {a.x * s, a.y * s, a.z * s}; }
inline V3 operator*(f64 s, const V3& a) { return {a.x * s, a.y * s, a.z * s}; }
inline V3& operator+=(V3& a, const V3& b) {
  a.x += b.x;
  a.y += b.y;
  a.z += b.z;
  return a;
}
inline V3& operator-=(V3& a, const V3& b) {
  a.x -= b.x;
  a.y -= b.y;
  a.z -= b.z;
  return a;
}
inline V3& operator*=(V3& a, f64 s) {
  a.x *= s;
  a.y *= s;
  a.z *= s;
  return a;
}
inline f64 dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline f64 norm2(const V3& a) { return dot(a, a); }
inline f64 norm(const V3& a) { return std::sqrt(dot(a, a)); }
inline V3 normalized(const V3& a) {
  const f64 n = norm(a);
  return n > 0 ? a * (1.0 / n) : V3{0, 0, 0};
}

// Symmetric or general 3x3, row-major.
struct M3 {
  std::array<f64, 9> m{0, 0, 0, 0, 0, 0, 0, 0, 0};
  static M3 identity() {
    M3 r;
    r.m = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    return r;
  }
  f64& operator()(int r, int c) { return m[3 * r + c]; }
  f64 operator()(int r, int c) const { return m[3 * r + c]; }
};
inline V3 operator*(const M3& A, const V3& v) {
  return {A.m[0] * v.x + A.m[1] * v.y + A.m[2] * v.z, A.m[3] * v.x + A.m[4] * v.y + A.m[5] * v.z,
          A.m[6] * v.x + A.m[7] * v.y + A.m[8] * v.z};
}
inline M3 operator*(const M3& A, const M3& B) {
  M3 R;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) R.m[3 * r + c] = A.m[3 * r] * B.m[c] + A.m[3 * r + 1] * B.m[3 + c] + A.m[3 * r + 2] * B.m[6 + c];
  return R;
}
inline M3 transpose(const M3& A) {
  M3 R;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) R.m[3 * r + c] = A.m[3 * c + r];
  return R;
}
inline bool inverse(const M3& A, M3& R) {
  const auto& a = A.m;
  const f64 det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
  if (!(std::abs(det) > 1e-300)) return false;
  const f64 id = 1.0 / det;
  R.m = {(a[4] * a[8] - a[5] * a[7]) * id, (a[2] * a[7] - a[1] * a[8]) * id, (a[1] * a[5] - a[2] * a[4]) * id,
         (a[5] * a[6] - a[3] * a[8]) * id, (a[0] * a[8] - a[2] * a[6]) * id, (a[2] * a[3] - a[0] * a[5]) * id,
         (a[3] * a[7] - a[4] * a[6]) * id, (a[1] * a[6] - a[0] * a[7]) * id, (a[0] * a[4] - a[1] * a[3]) * id};
  return true;
}

// Unit quaternion (x, y, z, w).
struct Quat {
  f64 x = 0, y = 0, z = 0, w = 1;
};
inline Quat operator*(const Quat& a, const Quat& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat conj(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Quat qnormalized(const Quat& q) {
  const f64 n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (!(n > 0)) return Quat{};
  const f64 s = 1.0 / n;
  return {q.x * s, q.y * s, q.z * s, q.w * s};
}
inline V3 rotate(const Quat& q, const V3& v) {
  const V3 u{q.x, q.y, q.z};
  const V3 t = cross(u, v) * 2.0;
  return v + t * q.w + cross(u, t);
}
inline V3 rotate_inv(const Quat& q, const V3& v) { return rotate(conj(q), v); }
inline M3 to_matrix(const Quat& q) {
  const f64 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const f64 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  const f64 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  M3 R;
  R.m = {1 - 2 * (yy + zz), 2 * (xy - wz),     2 * (xz + wy),  //
         2 * (xy + wz),     1 - 2 * (xx + zz), 2 * (yz - wx),  //
         2 * (xz - wy),     2 * (yz + wx),     1 - 2 * (xx + yy)};
  return R;
}
// q advanced by angular velocity w (world) over dt: first-order update + normalization.
inline Quat integrate(const Quat& q, const V3& w, f64 dt) {
  const Quat wq{w.x, w.y, w.z, 0.0};
  const Quat d = wq * q;
  return qnormalized({q.x + 0.5 * dt * d.x, q.y + 0.5 * dt * d.y, q.z + 0.5 * dt * d.z, q.w + 0.5 * dt * d.w});
}

inline V3 to_v3(const std::array<f64, 3>& a) { return {a[0], a[1], a[2]}; }
inline std::array<f64, 3> to_arr(const V3& a) { return {a.x, a.y, a.z}; }

}  // namespace svx
