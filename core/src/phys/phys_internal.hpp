// structvox — helpers shared by the rigid solver's parts (rigid.cpp, wheel.cpp): lattice
// lookups and casts (not part of the public API).
#pragma once

#include <algorithm>
#include <cmath>

#include "svx/base/vec.hpp"
#include "svx/world/grid.hpp"

namespace svx::phys_detail {

inline IVec3 voxel_of(const V3& s, f64 inv_h) {
  return {static_cast<i32>(std::floor(s.x * inv_h + 0.5)), static_cast<i32>(std::floor(s.y * inv_h + 0.5)),
          static_cast<i32>(std::floor(s.z * inv_h + 0.5))};
}

// x^k for x in [0, 1], k >= 0, by basic arithmetic only: the same bits on every platform
// (std::pow's last bit is the library's, and the result is state). ln x by the atanh series of
// its mantissa, e^-y by Taylor of a small part, squared up.
inline f64 pow01(f64 x, f64 k) {
  if (!(x > 0.0)) return k > 0.0 ? 0.0 : 1.0;
  if (x >= 1.0 || !(k > 0.0)) return 1.0;
  f64 m = x;
  int e = 0;
  while (m < 0.5) {  // (exact)
    m *= 2.0;
    ++e;
  }
  const f64 z = (m - 1.0) / (m + 1.0), z2 = z * z;  // (|z| <= 1/3)
  f64 term = z, series = 0.0;
  for (int i = 0; i < 40; ++i) {
    series += term / static_cast<f64>(2 * i + 1);
    term *= z2;
  }
  f64 y = k * (static_cast<f64>(e) * 0.69314718055994530942 - 2.0 * series);  // -k ln x >= 0
  if (!(y < 745.0)) return 0.0;
  int sq = 0;
  while (y > 0.0625) {
    y *= 0.5;
    ++sq;
  }
  f64 r = 1.0, t = 1.0;
  for (int i = 1; i <= 14; ++i) {
    t *= -y / static_cast<f64>(i);
    r += t;
  }
  for (int i = 0; i < sq; ++i) r *= r;
  return r;
}

inline u64 mix64(u64 x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// The face of least penetration of point s (in a voxel frame) inside solid voxel p that leads to
// air, per `solid`: normal (axis, sign) and depth. Returns false if every face is buried.
template <class Solid>
bool exit_face(const V3& s, const IVec3& p, f64 h, Solid&& solid, int* axis, int* sign, f64* depth) {
  f64 best = 1e300;
  bool found = false;
  for (int a = 0; a < 3; ++a)
    for (int sg = -1; sg <= 1; sg += 2) {
      IVec3 nb = p;
      nb[a] += sg;
      if (solid(nb)) continue;
      const f64 face = h * (p[a] + 0.5 * sg);
      const f64 d = sg > 0 ? face - s[a] : s[a] - face;
      if (d < best) {
        best = d;
        *axis = a;
        *sign = sg;
        found = true;
      }
    }
  *depth = std::max(0.0, best);
  return found;
}

// The first solid voxel on the segment from s along the unit direction d, within tmax (a lattice
// of voxel size h): the distance to where it enters it, and the face it enters by (axis, and the
// side the segment comes from: the face's normal out of the voxel). The voxel s is in is not
// tested (it is not solid, or the caller has a contact for it).
template <class Solid>
bool first_solid(const V3& s, const V3& d, f64 tmax, f64 h, Solid&& solid, f64* t_hit, int* axis, int* sign, IVec3* hit) {
  IVec3 v = voxel_of(s, 1.0 / h);
  int step[3];
  f64 tnext[3], tdelta[3];
  for (int a = 0; a < 3; ++a) {
    if (d[a] > 0.0) {
      step[a] = 1;
      tnext[a] = (h * (v[a] + 0.5) - s[a]) / d[a];
      tdelta[a] = h / d[a];
    } else if (d[a] < 0.0) {
      step[a] = -1;
      tnext[a] = (h * (v[a] - 0.5) - s[a]) / d[a];
      tdelta[a] = -h / d[a];
    } else {
      step[a] = 0;
      tnext[a] = tdelta[a] = 1e300;
    }
  }
  for (int n = 0; n < 64; ++n) {
    const int a = tnext[0] < tnext[1] ? (tnext[0] < tnext[2] ? 0 : 2) : (tnext[1] < tnext[2] ? 1 : 2);
    const f64 t = tnext[a];
    if (t > tmax) return false;
    v[a] += step[a];
    tnext[a] += tdelta[a];
    if (solid(v)) {
      *t_hit = std::max(0.0, t);
      *axis = a;
      *sign = -step[a];
      *hit = v;
      return true;
    }
  }
  return false;
}

}  // namespace svx::phys_detail
