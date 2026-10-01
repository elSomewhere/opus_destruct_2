// svx_city tests — ids of structural keys (voxel_city svx/ids.js) against the reference (stage
// "ids" of tools/procgen_ref).
#include <doctest.h>

#include "records.hpp"
#include "svx/ids.hpp"

using namespace svx::city;
using rec::Line;

TEST_CASE("city ids: idOf is the reference's (stage ids)") {
  rec::Out out;
  out << (Line() << "fixed" << id_of() << id_of("") << id_of("hw", "e12", 1, 0, 3, 7) << id_of("hwramp", "e3", 2, 1) << id_of("C3_-2/r5", "lane", 0, 1, -1, 4)
                 << id_of("C0_0/r1", "park", 2, 1, 17) << id_of("C0_0/r1", "walk", 1, true) << id_of("C0_0/r1", "cross", 3, "a")
                 << id_of(0.1 + 0.2, -0.0, 1e21, 1e-7, -5.5) << id_of(std::string(300, 'x')));
  rec::Samples r(11);
  auto word = [&] {
    std::string s;
    const int n = static_cast<int>(std::floor(r() * 14));
    for (int i = 0; i < n; ++i) s += static_cast<char>(32 + static_cast<int>(std::floor(r() * 95)));
    return s;
  };
  for (int i = 0; i < 4000; ++i) {
    const int n = static_cast<int>(std::floor(r() * 8));
    std::vector<std::string> keys;
    for (int k = 0; k < n; ++k) {
      const double t = r();
      if (t < 0.4)
        keys.push_back(word());
      else if (t < 0.7)
        keys.push_back(js::num(std::floor((r() - 0.5) * 2e6)));
      else if (t < 0.9)
        keys.push_back(js::num((r() - 0.5) * 1e3));
      else
        keys.push_back(js::str(r() < 0.5));
    }
    out << (Line() << "id" << n << id_of_list(keys));
  }
  CHECK(rec::record("ids", out.text()) == rec::recorded_digest("ids"));
  CHECK(id_of("a", 1) == id_of_list({"a", "1"}));
  CHECK(id_of() < 4503599627370496.0);
}
