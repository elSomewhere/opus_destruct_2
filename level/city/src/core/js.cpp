// svx_city — the JavaScript semantics of core/js.hpp: Math.pow (V8's base::ieee754::pow: fdlibm
// 5.3 e_pow.c with V8's own last step), Math.log2 (FreeBSD e_log2.c, as V8's base::ieee754::log2), V8's
// Math.hypot, and ECMAScript's Number::toString.
//
// e_pow.c, e_log2.c, k_log.h: Copyright (C) 1993, 2004 by Sun Microsystems, Inc. All rights
// reserved. Developed at SunSoft, a Sun Microsystems, Inc. business. Permission to use, copy,
// modify, and distribute this software is freely granted, provided that this notice is
// preserved.
#include "core/js.hpp"

#include <bit>
#include <charconv>
#include <cstdlib>

namespace svx::city::js {

namespace {

inline int32_t hi_word(double x) { return static_cast<int32_t>(std::bit_cast<uint64_t>(x) >> 32); }
inline uint32_t lo_word(double x) { return static_cast<uint32_t>(std::bit_cast<uint64_t>(x)); }
inline double set_lo(double x, uint32_t l) {
  return std::bit_cast<double>((std::bit_cast<uint64_t>(x) & 0xFFFFFFFF00000000ull) | l);
}
inline double set_hi(double x, int32_t h) {
  return std::bit_cast<double>((static_cast<uint64_t>(static_cast<uint32_t>(h)) << 32) |
                               (std::bit_cast<uint64_t>(x) & 0xFFFFFFFFull));
}

// fdlibm s_scalbn.c
double scalbn(double x, int n) {
  constexpr double two54 = 1.80143985094819840000e+16, twom54 = 5.55111512312578270212e-17,
                   huge = 1.0e+300, tiny = 1.0e-300;
  int32_t hx = hi_word(x);
  const uint32_t lx = lo_word(x);
  int32_t k = (hx & 0x7ff00000) >> 20;
  if (k == 0) {
    if ((lx | (hx & 0x7fffffff)) == 0) return x;
    x *= two54;
    hx = hi_word(x);
    k = ((hx & 0x7ff00000) >> 20) - 54;
    if (n < -50000) return tiny * x;
  }
  if (k == 0x7ff) return x + x;
  k = k + n;
  if (k > 0x7fe) return huge * std::copysign(huge, x);
  if (k > 0) return set_hi(x, (hx & static_cast<int32_t>(0x800fffff)) | (k << 20));
  if (k <= -54) {
    if (n > 50000) return huge * std::copysign(huge, x);
    return tiny * std::copysign(tiny, x);
  }
  k += 54;
  x = set_hi(x, (hx & static_cast<int32_t>(0x800fffff)) | (k << 20));
  return x * twom54;
}

}  // namespace

double pow(double x, double y) {
  static constexpr double bp[] = {1.0, 1.5}, dp_h[] = {0.0, 5.84962487220764160156e-01},
                          dp_l[] = {0.0, 1.35003920212974897128e-08};
  constexpr double zero = 0.0, one = 1.0, two = 2.0, two53 = 9007199254740992.0, huge = 1.0e300,
                   tiny = 1.0e-300,
                   L1 = 5.99999999999994648725e-01, L2 = 4.28571428578550184252e-01,
                   L3 = 3.33333329818377432918e-01, L4 = 2.72728123808534006489e-01,
                   L5 = 2.30660745775561754067e-01, L6 = 2.06975017800338417784e-01,
                   P1 = 1.66666666666666019037e-01, P2 = -2.77777777770155933842e-03,
                   P3 = 6.61375632143793436117e-05, P4 = -1.65339022054652515390e-06,
                   P5 = 4.13813679705723846039e-08, lg2 = 6.93147180559945286227e-01,
                   lg2_h = 6.93147182464599609375e-01, lg2_l = -1.90465429995776804525e-09,
                   ovt = 8.0085662595372944372e-0017, cp = 9.61796693925975554329e-01,
                   cp_h = 9.61796700954437255859e-01, cp_l = -7.02846165095275826516e-09,
                   ivln2 = 1.44269504088896338700e+00, ivln2_h = 1.44269502162933349609e+00,
                   ivln2_l = 1.92596299112661746887e-08;
  double z, ax, z_h, z_l, p_h, p_l, y1, t1, t2, r, s, t, u, v, w;
  int32_t i, j, k, yisint, n;
  int32_t hx = hi_word(x), hy = hi_word(y);
  uint32_t lx = lo_word(x), ly = lo_word(y);
  int32_t ix = hx & 0x7fffffff, iy = hy & 0x7fffffff;

  if ((iy | static_cast<int32_t>(ly)) == 0) return one;  // x**0 = 1
  // NaNs (ECMAScript: 1**NaN is NaN too)
  if (ix > 0x7ff00000 || ((ix == 0x7ff00000) && (lx != 0)) || iy > 0x7ff00000 ||
      ((iy == 0x7ff00000) && (ly != 0)))
    return x + y;
  yisint = 0;
  if (hx < 0) {
    if (iy >= 0x43400000) {
      yisint = 2;
    } else if (iy >= 0x3ff00000) {
      k = (iy >> 20) - 0x3ff;
      if (k > 20) {
        j = static_cast<int32_t>(ly >> (52 - k));
        if ((static_cast<uint32_t>(j) << (52 - k)) == ly) yisint = 2 - (j & 1);
      } else if (ly == 0) {
        j = iy >> (20 - k);
        if ((j << (20 - k)) == iy) yisint = 2 - (j & 1);
      }
    }
  }
  if (ly == 0) {
    if (iy == 0x7ff00000) {  // y is +-inf
      if (((ix - 0x3ff00000) | static_cast<int32_t>(lx)) == 0) return y - y;  // (-1)**+-inf: NaN (ECMAScript)
      if (ix >= 0x3ff00000) return (hy >= 0) ? y : zero;
      return (hy < 0) ? -y : zero;
    }
    if (iy == 0x3ff00000) return hy < 0 ? one / x : x;  // y is +-1
    if (hy == 0x40000000) return x * x;                // y is 2
    if (hy == 0x3fe00000) {                            // y is 0.5
      if (hx >= 0) return std::sqrt(x);
    }
  }
  ax = std::fabs(x);
  if (lx == 0) {
    if (ix == 0x7ff00000 || ix == 0 || ix == 0x3ff00000) {
      z = ax;
      if (hy < 0) z = one / z;
      if (hx < 0) {
        if (((ix - 0x3ff00000) | yisint) == 0)
          z = (z - z) / (z - z);
        else if (yisint == 1)
          z = -z;
      }
      return z;
    }
  }
  n = static_cast<int32_t>((static_cast<uint32_t>(hx) >> 31) - 1);
  if ((n | yisint) == 0) return (x - x) / (x - x);
  s = one;
  if ((n | (yisint - 1)) == 0) s = -one;
  if (iy > 0x41e00000) {
    if (iy > 0x43f00000) {
      if (ix <= 0x3fefffff) return (hy < 0) ? huge * huge : tiny * tiny;
      if (ix >= 0x3ff00000) return (hy > 0) ? huge * huge : tiny * tiny;
    }
    if (ix < 0x3fefffff) return (hy < 0) ? s * huge * huge : s * tiny * tiny;
    if (ix > 0x3ff00000) return (hy > 0) ? s * huge * huge : s * tiny * tiny;
    t = ax - one;
    w = (t * t) * (0.5 - t * (0.3333333333333333333333 - t * 0.25));
    u = ivln2_h * t;
    v = t * ivln2_l - w * ivln2;
    t1 = u + v;
    t1 = set_lo(t1, 0);
    t2 = v - (t1 - u);
  } else {
    double ss, s2, s_h, s_l, t_h, t_l;
    n = 0;
    if (ix < 0x00100000) {
      ax *= two53;
      n -= 53;
      ix = hi_word(ax);
    }
    n += ((ix) >> 20) - 0x3ff;
    j = ix & 0x000fffff;
    ix = j | 0x3ff00000;
    if (j <= 0x3988E)
      k = 0;
    else if (j < 0xBB67A)
      k = 1;
    else {
      k = 0;
      n += 1;
      ix -= 0x00100000;
    }
    ax = set_hi(ax, ix);
    u = ax - bp[k];
    v = one / (ax + bp[k]);
    ss = u * v;
    s_h = ss;
    s_h = set_lo(s_h, 0);
    t_h = zero;
    t_h = set_hi(t_h, ((ix >> 1) | 0x20000000) + 0x00080000 + (k << 18));
    t_l = ax - (t_h - bp[k]);
    s_l = v * ((u - s_h * t_h) - s_h * t_l);
    s2 = ss * ss;
    r = s2 * s2 * (L1 + s2 * (L2 + s2 * (L3 + s2 * (L4 + s2 * (L5 + s2 * L6)))));
    r += s_l * (s_h + ss);
    s2 = s_h * s_h;
    t_h = 3.0 + s2 + r;
    t_h = set_lo(t_h, 0);
    t_l = r - ((t_h - 3.0) - s2);
    u = s_h * t_h;
    v = s_l * t_h + t_l * ss;
    p_h = u + v;
    p_h = set_lo(p_h, 0);
    p_l = v - (p_h - u);
    z_h = cp_h * p_h;
    z_l = cp_l * p_h + p_l * cp + dp_l[k];
    t = static_cast<double>(n);
    t1 = (((z_h + z_l) + dp_h[k]) + t);
    t1 = set_lo(t1, 0);
    t2 = z_l - (((t1 - t) - dp_h[k]) - z_h);
  }
  y1 = y;
  y1 = set_lo(y1, 0);
  p_l = (y - y1) * t1 + y * t2;
  p_h = y1 * t1;
  z = p_l + p_h;
  j = hi_word(z);
  i = static_cast<int32_t>(lo_word(z));
  if (j >= 0x40900000) {
    if (((j - 0x40900000) | i) != 0) return s * huge * huge;
    if (p_l + ovt > z - p_h) return s * huge * huge;
  } else if ((j & 0x7fffffff) >= 0x4090cc00) {
    if (((j - static_cast<int32_t>(0xc090cc00)) | i) != 0) return s * tiny * tiny;
    if (p_l <= z - p_h) return s * tiny * tiny;
  }
  i = j & 0x7fffffff;
  k = (i >> 20) - 0x3ff;
  n = 0;
  if (i > 0x3fe00000) {
    n = j + (0x00100000 >> (k + 1));
    k = ((n & 0x7fffffff) >> 20) - 0x3ff;
    t = zero;
    t = set_hi(t, n & ~(0x000fffff >> k));
    n = ((n & 0x000fffff) | 0x00100000) >> (20 - k);
    if (j < 0) n = -n;
    p_h -= t;
  }
  t = p_l + p_h;
  t = set_lo(t, 0);
  u = t * lg2_h;
  v = (p_l - (t - p_h)) * lg2 + t * lg2_l;
  z = u + v;
  w = v - (z - u);
  t = z * z;
  t1 = z - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
  // (V8's port divides by the whole difference: not fdlibm's (z t1) / (t1 - 2) - (w + z w), but what
  // Math.pow returns, and so what this must)
  r = (z * t1) / ((t1 - two) - (w + z * w));
  z = one - (r - z);
  j = hi_word(z);
  j += static_cast<int32_t>(static_cast<uint32_t>(n) << 20);
  if ((j >> 20) <= 0)
    z = scalbn(z, n);
  else
    z = set_hi(z, j);
  return s * z;
}

double log2(double x) {
  constexpr double two54 = 1.80143985094819840000e+16, ivln2hi = 1.44269504072144627571e+00,
                   ivln2lo = 1.67517131648865118353e-10, Lg1 = 6.666666666666735130e-01,
                   Lg2 = 3.999999999940941908e-01, Lg3 = 2.857142874366239149e-01,
                   Lg4 = 2.222219843214978396e-01, Lg5 = 1.818357216161805012e-01,
                   Lg6 = 1.531383769920937332e-01, Lg7 = 1.479819860511658591e-01;
  int32_t hx = hi_word(x);
  const uint32_t lx = lo_word(x);
  int32_t k = 0;
  if (hx < 0x00100000) {
    if (((hx & 0x7fffffff) | static_cast<int32_t>(lx)) == 0) return -kInf;
    if (hx < 0) return kNaN;
    k -= 54;
    x *= two54;
    hx = hi_word(x);
  }
  if (hx >= 0x7ff00000) return x + x;
  if (hx == 0x3ff00000 && lx == 0) return 0.0;
  k += (hx >> 20) - 1023;
  hx &= 0x000fffff;
  const int32_t i = (hx + 0x95f64) & 0x100000;
  x = set_hi(x, hx | (i ^ 0x3ff00000));
  k += (i >> 20);
  const double y = static_cast<double>(k);
  const double f = x - 1.0;
  const double hfsq = 0.5 * f * f;
  // k_log1p(f)
  const double s = f / (2.0 + f);
  const double z = s * s;
  const double w = z * z;
  const double t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
  const double t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
  const double R = t2 + t1;
  const double r = s * (hfsq + R);
  double hi = f - hfsq;
  hi = set_lo(hi, 0);
  const double lo = (f - hi) - hfsq + r;
  double val_hi = hi * ivln2hi;
  double val_lo = (lo + hi) * ivln2lo + lo * ivln2hi;
  const double ww = y + val_hi;
  val_lo += (y - ww) + val_hi;
  val_hi = ww;
  return val_lo + val_hi;
}

double hypot(const double* v, int n) {
  if (n == 0) return 0.0;
  bool nan = false;
  double maxv = 0.0;
  double abs[8];
  std::vector<double> big;
  double* a = abs;
  if (n > 8) {
    big.resize(static_cast<size_t>(n));
    a = big.data();
  }
  for (int i = 0; i < n; ++i) {
    if (std::isnan(v[i])) {
      nan = true;
      a[i] = 0.0;
    } else {
      a[i] = std::fabs(v[i]);
      if (a[i] > maxv) maxv = a[i];
    }
  }
  if (maxv == kInf) return kInf;
  if (nan) return kNaN;
  if (maxv == 0) return 0.0;
  double sum = 0.0, comp = 0.0;
  for (int i = 0; i < n; ++i) {
    const double q = a[i] / maxv;
    const double summand = q * q - comp;
    const double prelim = sum + summand;
    comp = (prelim - sum) - summand;
    sum = prelim;
  }
  return std::sqrt(sum) * maxv;
}

double hypot(double a, double b) {
  const double v[2] = {a, b};
  return hypot(v, 2);
}

double hypot(double a, double b, double c) {
  const double v[3] = {a, b, c};
  return hypot(v, 3);
}

std::string num(double x) {
  if (x != x) return "NaN";
  if (x == 0) return "0";
  if (x == kInf) return "Infinity";
  if (x == -kInf) return "-Infinity";
  std::string out;
  if (x < 0) {
    out = "-";
    x = -x;
  }
  // integers below 2^53 print plainly (the common case: ids, voxel coordinates)
  if (x < 9007199254740992.0 && x == std::floor(x)) {
    char b[24];
    auto r = std::to_chars(b, b + sizeof b, static_cast<uint64_t>(x));
    out.append(b, r.ptr);
    return out;
  }
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::scientific);
  std::string_view sv(buf, static_cast<size_t>(res.ptr - buf));
  const size_t epos = sv.find('e');
  std::string digits;
  for (size_t q = 0; q < epos; ++q)
    if (sv[q] != '.') digits += sv[q];
  const int e = std::atoi(std::string(sv.substr(epos + 1)).c_str());
  const int k = static_cast<int>(digits.size());
  const int n = e + 1;
  if (k <= n && n <= 21) {
    out += digits;
    out.append(static_cast<size_t>(n - k), '0');
  } else if (0 < n && n <= 21) {
    out += digits.substr(0, static_cast<size_t>(n));
    out += '.';
    out += digits.substr(static_cast<size_t>(n));
  } else if (-6 < n && n <= 0) {
    out += "0.";
    out.append(static_cast<size_t>(-n), '0');
    out += digits;
  } else {
    out += digits[0];
    if (k > 1) {
      out += '.';
      out += digits.substr(1);
    }
    out += 'e';
    out += (n - 1 >= 0) ? '+' : '-';
    out += std::to_string(n - 1 >= 0 ? n - 1 : 1 - n);
  }
  return out;
}

double parse_number(std::string_view s) {
  if (s.empty()) return 0.0;
  double v = 0.0;
  auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return kNaN;
  return v;
}

void sort_strings(std::vector<std::string>& a) {
  sort(a, [](const std::string& x, const std::string& y) { return static_cast<double>(compare(x, y)); });
}

}  // namespace svx::city::js
