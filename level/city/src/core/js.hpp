// svx_city — the JavaScript semantics the generator's port reproduces (docs/CITY.md §Porting).
//
// The city generator is a port of voxel_city (reference/voxel_city, pinned at 4ed8e16), and it
// reproduces that generator bit for bit. Where JavaScript's numbers, operators and library
// functions differ from C++'s, the port goes through this header:
//
//   - every JS number is a double; integer-valued doubles are exact up to 2^53;
//   - Math.round rounds half towards +infinity (std::round: away from zero): js::round;
//   - Math.hypot is V8's (normalized Kahan summation): js::hypot;
//   - Math.sin, cos, atan, atan2, exp, log are V8's ports of fdlibm, bit for bit the core's
//     svx::dm (checked against Node over 400,000 inputs); Math.pow and Math.log2 are fdlibm's
//     e_pow.c and FreeBSD's e_log2.c, here (dm::pow is exp(y log x): not the same bits);
//   - x | 0, x >>> 0, Math.imul, <<, >>: ToInt32 / ToUint32 arithmetic (no signed overflow);
//   - String(number) / template literals: the shortest round-trip form (js::num);
//   - Array.prototype.sort is V8's TimSort (js::sort): stable, and the same order as V8's even
//     for a comparator that is not consistent (NaN, random, boolean results);
//   - typed array stores: Float32Array rounds to float, Uint8/Uint16/Int32Array wrap (js::u8 ..).
//
// Nothing here reads the platform's libm for a transcendental function.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "svx/base/dmath.hpp"

namespace svx::city::js {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ---- Math

// Math.round: the integer closest to x, halves towards +infinity; -0 for x in [-0.5, -0].
inline double round(double x) {
  if (!(std::fabs(x) < 4503599627370496.0)) return x;  // NaN, infinities, |x| >= 2^52: integers already
  const double r = std::floor(x);
  const double t = (x - r >= 0.5) ? r + 1.0 : r;
  return (t == 0.0 && std::signbit(x)) ? -0.0 : t;
}
inline double floor(double x) { return std::floor(x); }
inline double ceil(double x) { return std::ceil(x); }
inline double trunc(double x) { return std::trunc(x); }
inline double abs(double x) { return std::fabs(x); }
inline double sqrt(double x) { return std::sqrt(x); }  // (correctly rounded everywhere)
// Math.sign: -1, 0 (-0, +0 kept), 1; NaN for NaN.
inline double sign(double x) { return x > 0 ? 1.0 : x < 0 ? -1.0 : x; }

// Math.max / Math.min of two (NaN if either is NaN; max(+0, -0) is +0, min is -0).
inline double max(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) return kNaN;
  if (a == b) return std::signbit(a) ? b : a;
  return a > b ? a : b;
}
inline double min(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) return kNaN;
  if (a == b) return std::signbit(a) ? a : b;
  return a < b ? a : b;
}
template <class... T>
inline double max(double a, double b, T... rest) {
  return max(max(a, b), static_cast<double>(rest)...);
}
template <class... T>
inline double min(double a, double b, T... rest) {
  return min(min(a, b), static_cast<double>(rest)...);
}

inline double sin(double x) { return svx::dm::sin(x); }
inline double cos(double x) { return svx::dm::cos(x); }
inline double atan(double x) { return svx::dm::atan(x); }
inline double atan2(double y, double x) { return svx::dm::atan2(y, x); }
inline double exp(double x) { return svx::dm::exp(x); }
inline double log(double x) { return svx::dm::log(x); }
double pow(double x, double y);  // (also the ** operator)
double log2(double x);

// Math.hypot(a, b, ...): V8's algorithm (the largest magnitude scaled out, Kahan summation).
double hypot(double a, double b);
double hypot(double a, double b, double c);
double hypot(const double* v, int n);

// ---- numbers as integers

// ToInt32 / ToUint32 (x | 0, x >>> 0): modulo 2^32, NaN and infinities 0.
inline int32_t to_int32(double x) {
  if (!std::isfinite(x)) return 0;
  if (x >= -2147483648.0 && x <= 2147483647.0) return static_cast<int32_t>(x);  // (truncates)
  double t = std::fmod(std::trunc(x), 4294967296.0);
  if (t < 0) t += 4294967296.0;
  return static_cast<int32_t>(static_cast<uint32_t>(t));
}
inline uint32_t to_uint32(double x) { return static_cast<uint32_t>(to_int32(x)); }
inline int32_t imul(double a, double b) {
  return static_cast<int32_t>(to_uint32(a) * to_uint32(b));
}
// a << b, a >> b, a >>> b (b taken mod 32)
inline int32_t shl(double a, double b) { return static_cast<int32_t>(to_uint32(a) << (to_uint32(b) & 31)); }
inline int32_t sar(double a, double b) { return to_int32(a) >> (to_uint32(b) & 31); }
inline uint32_t shr(double a, double b) { return to_uint32(a) >> (to_uint32(b) & 31); }

// Typed array stores: Uint8Array, Uint16Array, Int8, Int16, Int32Array, Uint32Array (modulo),
// Float32Array (rounded to float).
inline uint8_t u8(double x) { return static_cast<uint8_t>(to_uint32(x)); }
inline uint16_t u16(double x) { return static_cast<uint16_t>(to_uint32(x)); }
inline int8_t i8(double x) { return static_cast<int8_t>(static_cast<uint8_t>(to_uint32(x))); }
inline int16_t i16(double x) { return static_cast<int16_t>(static_cast<uint16_t>(to_uint32(x))); }
inline int32_t i32(double x) { return to_int32(x); }
inline uint32_t u32(double x) { return to_uint32(x); }
inline float f32(double x) { return static_cast<float>(x); }
// Uint8ClampedArray: clamped to 0..255, rounded half to even.
inline uint8_t u8c(double x) {
  if (!(x > 0)) return 0;
  if (x >= 255) return 255;
  return static_cast<uint8_t>(std::nearbyint(x));
}

// The % operator on numbers (the dividend's sign; exact).
inline double mod(double a, double b) { return std::fmod(a, b); }

// x || d, x ?? d for numbers: || replaces 0, -0 and NaN; an "undefined" number is NaN here.
inline bool truthy(double x) { return x == x && x != 0; }
inline double or_(double x, double d) { return truthy(x) ? x : d; }
inline bool is_undefined(double x) { return x != x; }

// ---- strings

// String(number): the shortest round-trip decimal form, as ECMAScript's Number::toString.
std::string num(double x);
inline std::string str(const std::string& s) { return s; }
inline std::string str(const char* s) { return s; }
inline std::string str(std::string_view s) { return std::string(s); }
inline std::string str(double x) { return num(x); }
inline std::string str(int x) { return num(x); }
inline std::string str(long x) { return num(static_cast<double>(x)); }
inline std::string str(long long x) { return num(static_cast<double>(x)); }
inline std::string str(unsigned x) { return num(x); }
inline std::string str(unsigned long x) { return num(static_cast<double>(x)); }
inline std::string str(unsigned long long x) { return num(static_cast<double>(x)); }
inline std::string str(bool b) { return b ? "true" : "false"; }
inline std::string str(char c) { return std::string(1, c); }
// A template literal: `${a}${b}...`.
template <class... T>
std::string cat(const T&... parts) {
  std::string s;
  (s += ... += str(parts));
  return s;
}
// Number(string) for the decimal integers ids hold (NaN when it is not one).
double parse_number(std::string_view s);

// String comparison as JS's < on strings (UTF-16 code units; the generator's strings are ASCII).
inline int compare(std::string_view a, std::string_view b) { return a.compare(b) < 0 ? -1 : a == b ? 0 : 1; }

// ---- sort

namespace detail {
// V8's TimSort (third_party/v8/builtins/array-sort.tq): run detection, binary insertion sort up
// to the minimum run, galloping merges. cmp(a, b) returns a number; < 0: a before b.
template <class T, class Cmp>
class TimSort {
 public:
  TimSort(std::vector<T>& a, Cmp& cmp) : a_(a), cmp_(cmp) {}
  void run();

 private:
  std::vector<T>& a_;
  Cmp& cmp_;
  std::vector<T> tmp_;
  int min_gallop_ = 7;
  struct Run {
    int base, len;
  };
  std::vector<Run> runs_;
  // (SortCompare: ToNumber of the comparator's result, NaN as +0)
  double c(const T& x, const T& y) {
    const double v = static_cast<double>(cmp_(x, y));
    return v != v ? 0.0 : v;
  }
  static int min_run(int n) {
    int r = 0;
    while (n >= 64) {
      r |= n & 1;
      n >>= 1;
    }
    return n + r;
  }
  int count_and_make_run(int lo, int hi);
  void binary_insertion_sort(int lo, int start, int hi);
  void merge_collapse();
  void merge_force_collapse();
  void merge_at(int i);
  int gallop_left(const T& key, const T* base, int len, int hint);
  int gallop_right(const T& key, const T* base, int len, int hint);
  void merge_low(int base_a, int len_a, int base_b, int len_b);
  void merge_high(int base_a, int len_a, int base_b, int len_b);
};
}  // namespace detail

// arr.sort(cmp): in place, V8's order.
template <class T, class Cmp>
void sort(std::vector<T>& a, Cmp cmp) {
  if (a.size() < 2) return;
  detail::TimSort<T, Cmp> ts(a, cmp);
  ts.run();
}
// arr.sort() of strings (JS's default comparison: by UTF-16 code units).
void sort_strings(std::vector<std::string>& a);

}  // namespace svx::city::js

#include "core/js_sort.inl"
