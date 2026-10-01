// svx_city tests — a building's chamfer (voxel_city buildings/chamfer.js) against the reference
// (stage "chamfer" of tools/procgen_ref).
#include <doctest.h>

#include <string>
#include <vector>

#include "buildings/chamfer.hpp"
#include "records.hpp"

using namespace svx::city;
using rec::Line;

TEST_CASE("city chamfer: cuts and hits conform to the reference (stage chamfer)") {
  rec::Samples r(3);
  rec::Out out;
  for (int i = 0; i < 80; ++i) {
    const double k = 1 + std::floor(r() * 4);
    const bool swap = r() < 0.5;
    const char side = r() < 0.5 ? 'L' : 'R';
    const Chamfer c{side, (swap ? 21 : 20) * k, (swap ? 20 : 21) * k};
    const double U = c.a + 4 + std::floor(r() * 70);
    const double V = c.b + 4 + std::floor(r() * 50);
    std::string runs;
    for (double v = 0; v < V; v += 1) {
      double u = 0;
      while (u < U) {
        const bool cut = chamfer_cut(c, U, u, v);
        double n = 0;
        while (u < U && chamfer_cut(c, U, u, v) == cut) {
          n += 1;
          u += 1;
        }
        if (!runs.empty()) runs += ',';
        runs += js::cat(cut ? 1 : 0, ":", n);
      }
    }
    out << (Line() << "ch" << i << c.side << c.a << c.b << U << V << runs);
    std::string hits;
    for (int q = 0; q < 60; ++q) {
      const double x0 = std::floor((r() - 0.3) * (U + 20));
      const double y0 = std::floor((r() - 0.3) * (V + 20));
      const double x1 = x0 + std::floor(r() * 30);
      const double y1 = y0 + std::floor(r() * 30);
      if (q) hits += ' ';
      hits += js::cat(x0, ",", y0, ",", x1, ",", y1, ":", chamfer_hits(c, U, {x0, y0, x1, y1}) ? 1 : 0);
    }
    out << (Line() << "hits" << i << hits);
  }
  CHECK(rec::record("chamfer", out.text()) == rec::recorded_digest("chamfer"));
}
