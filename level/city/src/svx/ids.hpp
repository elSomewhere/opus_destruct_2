// svx_city — ids of structural keys (voxel_city svx/ids.js): a 52-bit id the same everywhere,
// exact as a JS number (a double here) and as a u64.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/hash.hpp"
#include "core/js.hpp"

namespace svx::city {

namespace detail {
struct IdState {
  uint32_t a = 0x9e3779b1u;
  uint32_t b = 0x85ebca77u;
  void key(std::string_view s) {
    // (String(k)'s UTF-16 code units: the generator's keys are ASCII)
    for (const unsigned char ch : s) {
      a = hash32(a, ch, 17);
      b = hash32(b, ch, 29);
    }
    a = hash32(a, 0x2f, 3);
    b = hash32(b, 0x2f, 5);
  }
  double id() const { return static_cast<double>(a & 0xfffffu) * 4294967296.0 + static_cast<double>(b); }
};
}  // namespace detail

// idOf(...keys): each key as String(key) prints it (numbers as JS prints them, booleans "true" /
// "false"), e.g. id_of(road_id, "lane", li, pi, dir, k).
template <class... K>
double id_of(const K&... keys) {
  detail::IdState st;
  (st.key(js::str(keys)), ...);
  return st.id();
}

// idOf(...keys) of keys already strings.
inline double id_of_list(const std::vector<std::string>& keys) {
  detail::IdState st;
  for (const std::string& k : keys) st.key(k);
  return st.id();
}

}  // namespace svx::city
