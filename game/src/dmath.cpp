// Ports of fdlibm 5.3 (s_sin.c, s_cos.c, k_sin.c, k_cos.c, e_rem_pio2.c medium case, s_atan.c,
// e_atan2.c, e_exp.c, e_log.c).
//
// Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
// Developed at SunSoft, a Sun Microsystems, Inc. business.
// Permission to use, copy, modify, and distribute this software is freely granted, provided
// that this notice is preserved.
#include "svx/game/dmath.hpp"

#include <bit>
#include <cmath>
#include <limits>

namespace svx::dm {

namespace {

inline i32 hi(f64 x) { return static_cast<i32>(std::bit_cast<u64>(x) >> 32); }
inline u32 lo(f64 x) { return static_cast<u32>(std::bit_cast<u64>(x)); }
inline f64 with_hi(f64 x, i32 h) {
  const u64 b = (static_cast<u64>(static_cast<u32>(h)) << 32) | lo(x);
  return std::bit_cast<f64>(b);
}
inline f64 from_words(u32 h, u32 l) { return std::bit_cast<f64>((static_cast<u64>(h) << 32) | l); }

// |x| <= pi/4, y the tail of x, iy = 0 if y is 0
f64 k_sin(f64 x, f64 y, int iy) {
  constexpr f64 S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
                S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
                S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
  const i32 ix = hi(x) & 0x7fffffff;
  if (ix < 0x3e400000 && static_cast<int>(x) == 0) return x;  // |x| < 2^-27
  const f64 z = x * x;
  const f64 v = z * x;
  const f64 r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
  if (iy == 0) return x + v * (S1 + z * r);
  return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

f64 k_cos(f64 x, f64 y) {
  constexpr f64 C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
  const i32 ix = hi(x) & 0x7fffffff;
  if (ix < 0x3e400000 && static_cast<int>(x) == 0) return 1.0;  // |x| < 2^-27
  const f64 z = x * x;
  const f64 r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
  if (ix < 0x3FD33333) return 1.0 - (0.5 * z - (z * r - x * y));  // |x| < 0.3
  const f64 qx = ix > 0x3fe90000 ? 0.28125 : from_words(static_cast<u32>(ix - 0x00200000), 0);
  const f64 hz = 0.5 * z - qx;
  const f64 a = 1.0 - qx;
  return a - (hz - (z * r - x * y));
}

// x = n pi/2 + (y0 + y1), |y0 + y1| <= pi/4 (Cody-Waite with a 3-part pi/2: exact enough for
// |x| < 2^19 pi/2; larger arguments stay deterministic but lose accuracy)
int rem_pio2(f64 x, f64& y0, f64& y1) {
  constexpr f64 invpio2 = 6.36619772367581382433e-01, pio2_1 = 1.57079632673412561417e+00,
                pio2_1t = 6.07710050650619224932e-11, pio2_2 = 6.07710050630396597660e-11,
                pio2_2t = 2.02226624879595063154e-21, pio2_3 = 2.02226624871116645580e-21,
                pio2_3t = 8.47842766036889956997e-32;
  const i32 hx = hi(x);
  const i32 ix = hx & 0x7fffffff;
  const f64 t = std::fabs(x);
  const int n = static_cast<int>(t * invpio2 + 0.5);
  const f64 fn = static_cast<f64>(n);
  f64 r = t - fn * pio2_1;
  f64 w = fn * pio2_1t;  // first round, good to 85 bits
  const i32 j = ix >> 20;
  y0 = r - w;
  i32 i = j - ((hi(y0) >> 20) & 0x7ff);
  if (i > 16) {  // second iteration, good to 118 bits
    f64 tt = r;
    w = fn * pio2_2;
    r = tt - w;
    w = fn * pio2_2t - ((tt - r) - w);
    y0 = r - w;
    i = j - ((hi(y0) >> 20) & 0x7ff);
    if (i > 49) {  // third iteration, 151 bits
      tt = r;
      w = fn * pio2_3;
      r = tt - w;
      w = fn * pio2_3t - ((tt - r) - w);
      y0 = r - w;
    }
  }
  y1 = (r - y0) - w;
  if (hx < 0) {
    y0 = -y0;
    y1 = -y1;
    return -n;
  }
  return n;
}

}  // namespace

f64 sin(f64 x) {
  const i32 ix = hi(x) & 0x7fffffff;
  if (ix <= 0x3fe921fb) return k_sin(x, 0.0, 0);
  if (ix >= 0x7ff00000) return x - x;  // inf or NaN
  f64 y0, y1;
  const int n = rem_pio2(x, y0, y1);
  switch (n & 3) {
    case 0: return k_sin(y0, y1, 1);
    case 1: return k_cos(y0, y1);
    case 2: return -k_sin(y0, y1, 1);
    default: return -k_cos(y0, y1);
  }
}

f64 cos(f64 x) {
  const i32 ix = hi(x) & 0x7fffffff;
  if (ix <= 0x3fe921fb) return k_cos(x, 0.0);
  if (ix >= 0x7ff00000) return x - x;
  f64 y0, y1;
  const int n = rem_pio2(x, y0, y1);
  switch (n & 3) {
    case 0: return k_cos(y0, y1);
    case 1: return -k_sin(y0, y1, 1);
    case 2: return -k_cos(y0, y1);
    default: return k_sin(y0, y1, 1);
  }
}

f64 atan(f64 x) {
  constexpr f64 atanhi[] = {4.63647609000806093515e-01, 7.85398163397448278999e-01, 9.82793723247329054082e-01,
                            1.57079632679489655800e+00};
  constexpr f64 atanlo[] = {2.26987774529616870924e-17, 3.06161699786838301793e-17, 1.39033110312309984516e-17,
                            6.12323399573676603587e-17};
  constexpr f64 aT[] = {3.33333333333329318027e-01,  -1.99999999998764832476e-01, 1.42857142725034663711e-01,
                        -1.11111104054623557880e-01, 9.09088713343650656196e-02,  -7.69187620504482999495e-02,
                        6.66107313738753120669e-02,  -5.83357013379057348645e-02, 4.97687799461593236017e-02,
                        -3.65315727442169155270e-02, 1.62858201153657823623e-02};
  const i32 hx = hi(x);
  const i32 ix = hx & 0x7fffffff;
  int id;
  if (ix >= 0x44100000) {  // |x| >= 2^66
    if (ix > 0x7ff00000 || (ix == 0x7ff00000 && lo(x) != 0)) return x + x;  // NaN
    return hx > 0 ? atanhi[3] + atanlo[3] : -atanhi[3] - atanlo[3];
  }
  if (ix < 0x3fdc0000) {  // |x| < 0.4375
    if (ix < 0x3e200000) return x;  // |x| < 2^-29
    id = -1;
  } else {
    x = std::fabs(x);
    if (ix < 0x3ff30000) {  // |x| < 1.1875
      if (ix < 0x3fe60000) {  // 7/16 <= |x| < 11/16
        id = 0;
        x = (2.0 * x - 1.0) / (2.0 + x);
      } else {  // 11/16 <= |x| < 19/16
        id = 1;
        x = (x - 1.0) / (x + 1.0);
      }
    } else if (ix < 0x40038000) {  // |x| < 2.4375
      id = 2;
      x = (x - 1.5) / (1.0 + 1.5 * x);
    } else {  // 2.4375 <= |x| < 2^66
      id = 3;
      x = -1.0 / x;
    }
  }
  const f64 z = x * x;
  const f64 w = z * z;
  const f64 s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
  const f64 s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
  if (id < 0) return x - x * (s1 + s2);
  const f64 r = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
  return hx < 0 ? -r : r;
}

f64 atan2(f64 y, f64 x) {
  constexpr f64 pi_o_4 = 7.8539816339744827900E-01, pi_o_2 = 1.5707963267948965580E+00,
                pi = 3.1415926535897931160E+00, pi_lo = 1.2246467991473531772E-16;
  if (std::isnan(x) || std::isnan(y)) return x + y;
  const i32 hx = hi(x), hy = hi(y);
  const i32 ix = hx & 0x7fffffff, iy = hy & 0x7fffffff;
  const u32 lx = lo(x), ly = lo(y);
  if (x == 1.0) return atan(y);
  const int m = ((hy >> 31) & 1) | ((hx >> 30) & 2);  // 2 sign(x) + sign(y)
  if ((iy | ly) == 0) {  // y = 0
    switch (m) {
      case 0:
      case 1: return y;
      case 2: return pi;
      default: return -pi;
    }
  }
  if ((ix | lx) == 0) return hy < 0 ? -pi_o_2 : pi_o_2;  // x = 0
  if (ix == 0x7ff00000) {  // x = inf
    if (iy == 0x7ff00000) {
      switch (m) {
        case 0: return pi_o_4;
        case 1: return -pi_o_4;
        case 2: return 3.0 * pi_o_4;
        default: return -3.0 * pi_o_4;
      }
    }
    switch (m) {
      case 0: return 0.0;
      case 1: return -0.0;
      case 2: return pi;
      default: return -pi;
    }
  }
  if (iy == 0x7ff00000) return hy < 0 ? -pi_o_2 : pi_o_2;  // y = inf
  const i32 k = (iy - ix) >> 20;
  f64 z;
  if (k > 60)
    z = pi_o_2 + 0.5 * pi_lo;  // |y / x| > 2^60
  else if (hx < 0 && k < -60)
    z = 0.0;  // |y| / x < -2^60
  else
    z = atan(std::fabs(y / x));
  switch (m) {
    case 0: return z;
    case 1: return -z;
    case 2: return pi - (z - pi_lo);
    default: return (z - pi_lo) - pi;
  }
}

f64 exp(f64 x) {
  constexpr f64 o_threshold = 7.09782712893383973096e+02, u_threshold = -7.45133219101941108420e+02,
                ln2HI[2] = {6.93147180369123816490e-01, -6.93147180369123816490e-01},
                ln2LO[2] = {1.90821492927058770002e-10, -1.90821492927058770002e-10},
                invln2 = 1.44269504088896338700e+00, P1 = 1.66666666666666019037e-01,
                P2 = -2.77777777770155933842e-03, P3 = 6.61375632143793436117e-05,
                P4 = -1.65339022054652515390e-06, P5 = 4.13813679705723846039e-08,
                twom1000 = 9.33263618503218878990e-302, halF[2] = {0.5, -0.5};
  u32 hx = static_cast<u32>(hi(x));
  const int xsb = static_cast<int>((hx >> 31) & 1);
  hx &= 0x7fffffff;
  if (hx >= 0x40862E42) {  // |x| >= 709.78...
    if (hx >= 0x7ff00000) {
      if (((hx & 0xfffff) | lo(x)) != 0) return x + x;  // NaN
      return xsb == 0 ? x : 0.0;
    }
    if (x > o_threshold) return std::numeric_limits<f64>::infinity();
    if (x < u_threshold) return 0.0;
  }
  f64 hi_ = 0.0, lo_ = 0.0;
  int k = 0;
  if (hx > 0x3fd62e42) {  // |x| > 0.5 ln2
    if (hx < 0x3FF0A2B2) {  // and |x| < 1.5 ln2
      hi_ = x - ln2HI[xsb];
      lo_ = ln2LO[xsb];
      k = 1 - xsb - xsb;
    } else {
      k = static_cast<int>(invln2 * x + halF[xsb]);
      const f64 t = k;
      hi_ = x - t * ln2HI[0];
      lo_ = t * ln2LO[0];
    }
    x = hi_ - lo_;
  } else if (hx < 0x3e300000) {  // |x| < 2^-28
    return 1.0 + x;
  }
  const f64 t = x * x;
  const f64 c = x - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
  if (k == 0) return 1.0 - ((x * c) / (c - 2.0) - x);
  const f64 y = 1.0 - ((lo_ - (x * c) / (2.0 - c)) - hi_);
  if (k >= -1021) return with_hi(y, hi(y) + (k << 20));
  return with_hi(y, hi(y) + ((k + 1000) << 20)) * twom1000;
}

f64 log(f64 x) {
  constexpr f64 ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10,
                two54 = 1.80143985094819840000e+16, Lg1 = 6.666666666666735130e-01, Lg2 = 3.999999999940941908e-01,
                Lg3 = 2.857142874366239149e-01, Lg4 = 2.222219843214978396e-01, Lg5 = 1.818357216161805012e-01,
                Lg6 = 1.531383769920937332e-01, Lg7 = 1.479819860511658591e-01;
  i32 hx = hi(x);
  const u32 lx = lo(x);
  int k = 0;
  if (hx < 0x00100000) {  // x < 2^-1022
    if (((hx & 0x7fffffff) | static_cast<i32>(lx)) == 0) return -std::numeric_limits<f64>::infinity();
    if (hx < 0) return std::numeric_limits<f64>::quiet_NaN();
    k -= 54;
    x *= two54;  // subnormal: scale up
    hx = hi(x);
  }
  if (hx >= 0x7ff00000) return x + x;
  k += (hx >> 20) - 1023;
  hx &= 0x000fffff;
  const i32 i0 = (hx + 0x95f64) & 0x100000;
  x = with_hi(x, hx | (i0 ^ 0x3ff00000));  // normalize x or x / 2
  k += i0 >> 20;
  const f64 f = x - 1.0;
  if ((0x000fffff & (2 + hx)) < 3) {  // |f| < 2^-20
    if (f == 0.0) {
      if (k == 0) return 0.0;
      const f64 dk = k;
      return dk * ln2_hi + dk * ln2_lo;
    }
    const f64 R = f * f * (0.5 - 0.33333333333333333 * f);
    if (k == 0) return f - R;
    const f64 dk = k;
    return dk * ln2_hi - ((R - dk * ln2_lo) - f);
  }
  const f64 s = f / (2.0 + f);
  const f64 dk = k;
  const f64 z = s * s;
  i32 i = hx - 0x6147a;
  const f64 w = z * z;
  const i32 j = 0x6b851 - hx;
  const f64 t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
  const f64 t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
  i |= j;
  const f64 R = t2 + t1;
  if (i > 0) {
    const f64 hfsq = 0.5 * f * f;
    if (k == 0) return f - (hfsq - s * (hfsq + R));
    return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);
  }
  if (k == 0) return f - s * (f - R);
  return dk * ln2_hi - ((s * (f - R) - dk * ln2_lo) - f);
}

f64 pow(f64 x, f64 y) {
  if (y == 0.0) return 1.0;
  if (x == 1.0) return 1.0;
  if (std::isnan(x) || std::isnan(y)) return x + y;
  if (x == 0.0) return y > 0.0 ? 0.0 : std::numeric_limits<f64>::infinity();
  if (x < 0.0) {
    const f64 yi = std::floor(y);
    if (yi != y || std::fabs(y) > 1e9) return std::numeric_limits<f64>::quiet_NaN();
    const f64 r = exp(y * log(-x));
    return (static_cast<i64>(yi) & 1) ? -r : r;
  }
  return exp(y * log(x));
}

}  // namespace svx::dm
