// svx_city — deterministic hashing and seeded random streams (voxel_city core/hash.js).
//
// Every generated entity derives its own seed from the world seed plus a stable structural key
// (cell coordinates, entity ids); nothing shares one sequential stream across unrelated
// entities, so generation order never changes results.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/js.hpp"

namespace svx::city {

// murmur3's finalizer (mix32: a uint32 as a double in JS; here the same 32 bits).
inline uint32_t mix32(uint32_t h) {
  h ^= h >> 16;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  h *= 0xc2b2ae35u;
  h ^= h >> 16;
  return h;
}
inline uint32_t mix32(double h) { return mix32(js::to_uint32(h)); }

// hash32(a, b, c, d): any four numbers (ToInt32 of each) into a uint32.
inline uint32_t hash32(double a, double b = 0, double c = 0, double d = 0) {
  uint32_t h = mix32(static_cast<uint32_t>(js::to_int32(a)) ^ 0x9e3779b9u);
  h = mix32(h ^ static_cast<uint32_t>(js::imul(js::to_int32(b), 0x27d4eb2d)));
  h = mix32(h ^ static_cast<uint32_t>(js::imul(js::to_int32(c), 0x165667b1)));
  h = mix32(h ^ static_cast<uint32_t>(js::imul(js::to_int32(d), 0x61c88647)));
  return h;
}
inline double hash_float(double a, double b = 0, double c = 0, double d = 0) {
  return hash32(a, b, c, d) / 4294967296.0;
}

// FNV-1a over a string's UTF-16 code units (the generator's strings are ASCII), then mixed.
inline uint32_t hash_string(std::string_view s) {
  uint32_t h = 2166136261u;
  for (unsigned char ch : s) {
    h ^= ch;
    h *= 16777619u;
  }
  return mix32(h);
}

// A part of a seed (deriveSeed's ...parts): a number, or anything else as String(part) hashes it
// (a JS part that is undefined hashes "undefined", null "null", a boolean "true" / "false").
struct SeedPart {
  bool is_num = true;
  double num = 0;
  std::string_view str;
  SeedPart(double v) : num(v) {}                          // NOLINT
  SeedPart(int v) : num(v) {}                             // NOLINT
  SeedPart(unsigned v) : num(v) {}                        // NOLINT
  SeedPart(long v) : num(static_cast<double>(v)) {}       // NOLINT
  SeedPart(long long v) : num(static_cast<double>(v)) {}  // NOLINT
  SeedPart(unsigned long v) : num(static_cast<double>(v)) {}       // NOLINT
  SeedPart(unsigned long long v) : num(static_cast<double>(v)) {}  // NOLINT
  SeedPart(const char* s) : is_num(false), str(s) {}      // NOLINT
  SeedPart(const std::string& s) : is_num(false), str(s) {}  // NOLINT
  SeedPart(std::string_view s) : is_num(false), str(s) {}    // NOLINT
};

// deriveSeed(root, ...parts): `derive_seed(world, "building", id)` is the canonical pattern.
inline uint32_t derive_seed_step(uint32_t h, const SeedPart& p) {
  uint32_t v;
  if (p.is_num)
    v = mix32(static_cast<uint32_t>(js::to_int32(p.num))) ^ mix32(static_cast<uint32_t>(js::to_int32(std::floor(p.num * 65536))));
  else
    v = hash_string(p.str);
  return mix32(h ^ v ^ (h << 6));
}
template <class... P>
inline uint32_t derive_seed(double root, const P&... parts) {
  uint32_t h = mix32(static_cast<uint32_t>(js::to_int32(root)) ^ 0x5bd1e995u);
  ((h = derive_seed_step(h, SeedPart(parts))), ...);
  return h;
}

// sfc32: the generators' vocabulary of draws; keep it stable (every layout depends on call order
// within one entity).
class Rng {
 public:
  explicit Rng(double seed) {
    const uint32_t s = js::to_uint32(seed);
    a_ = s;
    b_ = mix32(s ^ 0xdeadbeefu);
    c_ = mix32(s ^ 0x41c64e6du);
    d_ = 1;
    for (int i = 0; i < 12; ++i) next();
  }
  template <class... P>
  static Rng from(double root, const P&... parts) {
    return Rng(derive_seed(root, parts...));
  }
  // float in [0, 1)
  double next() {
    const uint32_t t = a_ + b_ + d_;
    d_ = d_ + 1;
    a_ = b_ ^ (b_ >> 9);
    b_ = c_ + (c_ << 3);
    c_ = (c_ << 21) | (c_ >> 11);
    c_ = c_ + t;
    return t / 4294967296.0;
  }
  double float_(double min = 0, double max = 1) { return min + (max - min) * next(); }
  // integer in [min, max] inclusive
  double int_(double min, double max) { return min + std::floor(next() * (max - min + 1)); }
  int ii(double min, double max) { return static_cast<int>(int_(min, max)); }
  bool chance(double p) { return next() < p; }
  double sign() { return next() < 0.5 ? -1.0 : 1.0; }
  template <class T>
  const T& pick(const std::vector<T>& items) {
    return items[static_cast<size_t>(std::floor(next() * static_cast<double>(items.size())))];
  }
  template <class T, size_t N>
  const T& pick(const T (&items)[N]) {
    return items[static_cast<size_t>(std::floor(next() * static_cast<double>(N)))];
  }
  size_t pick_index(size_t n) { return static_cast<size_t>(std::floor(next() * static_cast<double>(n))); }
  // weighted([[value, weight], ...]): the value whose cumulative weight passes a draw.
  template <class T>
  const T& weighted(const std::vector<std::pair<T, double>>& pairs) {
    double total = 0;
    for (const auto& p : pairs) total += p.second;
    double r = next() * total;
    for (const auto& p : pairs) {
      if (r < p.second) return p.first;
      r -= p.second;
    }
    return pairs.back().first;
  }
  // weighted over items with a weight accessor (objects with .weight).
  template <class T, class W>
  size_t weighted_index(const std::vector<T>& items, W weight) {
    double total = 0;
    for (const auto& p : items) total += weight(p);
    double r = next() * total;
    for (size_t i = 0; i < items.size(); ++i) {
      const double w = weight(items[i]);
      if (r < w) return i;
      r -= w;
    }
    return items.size() - 1;
  }
  template <class T>
  std::vector<T> shuffle(const std::vector<T>& items) {
    std::vector<T> out = items;
    for (size_t i = out.size(); i-- > 1;) {
      const size_t j = static_cast<size_t>(std::floor(next() * static_cast<double>(i + 1)));
      std::swap(out[i], out[j]);
    }
    return out;
  }
  // approximately normal: a sum of uniforms
  double gauss(double mean = 0, double sd = 1) {
    const double u = next() + next() + next() + next() - 2;
    return mean + u * sd * 0.8660254;
  }
  // an independent child stream keyed by a label
  template <class P>
  Rng fork(const P& label) {
    return Rng(derive_seed(static_cast<double>(static_cast<int32_t>(a_ ^ c_)), label));
  }
  // (state, for tests)
  uint32_t a() const { return a_; }

 private:
  uint32_t a_, b_, c_, d_;
};

}  // namespace svx::city
