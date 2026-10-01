// svx_city tests — conformance records (docs/CITY.md §Conformance): the C++ twin of
// tools/procgen_ref/lib/rec.mjs. A stage's records are text lines, numbers printed as JavaScript
// prints them; a stage conforms when its text is the reference's, byte for byte.
//
// check_stage(name, text) compares the text's SHA-256 with the digest recorded for the stage in
// tests/city/conformance.txt (tools/procgen_ref/record.sh writes it from the reference). With
// SVX_CITY_RECORDS=DIR the text is also written to DIR/<name>.txt, to diff with the reference's
// (node tools/procgen_ref/dump.mjs <name> > ref.txt; tools/procgen_ref/diff.sh).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/js.hpp"
#include "sha256.hpp"

namespace svx::city::rec {

struct Undef {};
constexpr Undef kUndef{};

inline std::string f(double v) { return js::num(v); }
inline std::string f(int v) { return js::num(v); }
inline std::string f(unsigned v) { return js::num(v); }
inline std::string f(long v) { return js::num(static_cast<double>(v)); }
inline std::string f(long long v) { return js::num(static_cast<double>(v)); }
inline std::string f(unsigned long v) { return js::num(static_cast<double>(v)); }
inline std::string f(unsigned long long v) { return js::num(static_cast<double>(v)); }
inline std::string f(bool b) { return b ? "1" : "0"; }
inline std::string f(const std::string& s) { return s.empty() ? "\"\"" : s; }
inline std::string f(const char* s) { return f(std::string(s)); }
inline std::string f(char c) { return std::string(1, c); }
inline std::string f(Undef) { return "-"; }
template <class T>
std::string f(const std::vector<T>& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ',';
    s += f(v[i]);
  }
  return s + "]";
}

// A record: its fields, space-separated.
class Line {
 public:
  template <class T>
  Line& operator<<(const T& v) {
    if (!s_.empty()) s_ += ' ';
    s_ += f(v);
    return *this;
  }
  // a run of fields from a sequence
  template <class It>
  Line& seq(It a, It b) {
    for (; a != b; ++a) *this << *a;
    return *this;
  }
  const std::string& str() const { return s_; }

 private:
  std::string s_;
};

class Out {
 public:
  Out& operator<<(const Line& l) {
    text_ += l.str();
    text_ += '\n';
    return *this;
  }
  const std::string& text() const { return text_; }

 private:
  std::string text_;
};

// The LCG of rec.mjs's samples().
class Samples {
 public:
  explicit Samples(uint32_t seed) : s_(seed) {}
  double operator()() {
    s_ = s_ * 1664525u + 1013904223u;
    return s_ / 4294967296.0;
  }

 private:
  uint32_t s_;
};

// The recorded digest of a stage ("" when none is recorded).
inline std::string recorded_digest(const std::string& stage) {
  std::ifstream in(std::string(SVX_SOURCE_DIR) + "/tests/city/conformance.txt");
  std::string name, digest;
  while (in >> name >> digest)
    if (name == stage) return digest;
  return "";
}

// Writes the text where SVX_CITY_RECORDS asks; returns the stage's digest (16 hex digits).
inline std::string record(const std::string& stage, const std::string& text) {
  if (const char* dir = std::getenv("SVX_CITY_RECORDS")) {
    std::ofstream(std::string(dir) + "/" + stage + ".txt", std::ios::binary) << text;
  }
  return sha256_hex(text).substr(0, 16);
}

}  // namespace svx::city::rec
