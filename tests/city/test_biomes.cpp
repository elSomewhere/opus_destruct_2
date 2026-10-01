// svx_city tests — the biomes (voxel_city nature/biomes.js) against the reference (stage
// "biomes").
#include <doctest.h>

#include "nature/biomes.hpp"
#include "records.hpp"
#include "world/register_all.hpp"

using namespace svx::city;
using rec::Line;

TEST_CASE("city biomes: the registry, classification and ground conform to the reference (stage biomes)") {
  register_all();
  rec::Samples r(23);
  rec::Out out;
  for (const Biome& b : biomes().all()) {
    std::string label = b.label;
    for (char& c : label)
      if (c == ' ') c = '_';
    std::string trees;
    for (const auto& [k, w] : b.trees) trees += (trees.empty() ? "" : ",") + js::cat(k, ":", w);
    out << (Line() << "biome" << b.id << label << b.color << std::vector<double>{b.climate[0], b.climate[1]} << b.forest << b.meadow << b.farmland << trees
                   << int(b.floor) << b.pools << b.dunes);
  }
  for (int i = -5; i <= 55; ++i) {
    const double t = i / 50.0;
    Line ids, des;
    ids << "cl" << t;
    des << "de" << t;
    for (int j = -5; j <= 55; ++j) {
      const double m = j / 50.0;
      ids << classify_biome(t, m)->id;
      des << desertness(t, m);
    }
    out << ids << des;
  }
  for (int k = 0; k < 2000; ++k) {
    const double t = r() * 1.3 - 0.15;
    const double m = r() * 1.3 - 0.15;
    out << (Line() << "cr" << t << m << classify_biome(t, m)->id << desertness(t, m));
  }
  for (const Biome& b : biomes().all())
    for (int k = 0; k < 300; ++k) {
      BiomeGroundCtx c;
      c.patch = (r() - 0.5) * 2.4;
      c.p2 = (r() - 0.5) * 2.4;
      c.h = r();
      c.x = std::floor((r() - 0.5) * 1e5);
      c.y = std::floor((r() - 0.5) * 1e5);
      const std::array<uint16_t, 2> g = b.ground(c);
      out << (Line() << "gr" << b.id << c.patch << c.p2 << c.h << int(g[0]) << int(g[1]));
    }
  CHECK(rec::record("biomes", out.text()) == rec::recorded_digest("biomes"));
}
