// svx_city tests — the macro fields (voxel_city world/fields.js) against the reference (stage
// "fields").
#include <doctest.h>

#include "records.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"
#include "worlds.hpp"

using namespace svx::city;
using rec::Line;

namespace {

void rec_line(rec::Out& out, const char* tag, const Settlement* s) {
  Line l;
  l << tag;
  if (s)
    test::settlement_fields(l, *s);
  else
    l << rec::kUndef;
  out << l;
}

void id_or_undef(Line& l, const Settlement* s) {
  if (s)
    l << s->id;
  else
    l << rec::kUndef;
}

}  // namespace

TEST_CASE("city fields: the macro fields conform to the reference (stage fields)") {
  rec::Samples r(29);
  rec::Out out;
  for (const test::WorldCase& ws : test::all_worlds()) {
    const World w(ws.overrides);
    const MacroFields& F = *w.fields;
    out << (Line() << "fields" << ws.key << F.n_town << F.n_village << std::vector<double>{F.m_off[0], F.m_off[1]} << (F.island != nullptr) << F.wrap.on);
    for (const Settlement* s : F.settlements_in({-400000, -400000, 400000, 400000})) rec_line(out, "town", s);
    for (const Settlement* v : F.villages_in({-200000, -200000, 200000, 200000})) rec_line(out, "village", v);
    for (int j = -3; j <= 3; ++j)
      for (int i = -3; i <= 3; ++i) {
        Line l;
        l << "cell" << i << j;
        id_or_undef(l, F.settlement(i, j));
        id_or_undef(l, F.village(i, j));
        out << l;
        rec_line(out, "lapT", F.settlement(i + 2 * F.n_town, j - F.n_town));
        rec_line(out, "lapV", F.village(i - F.n_village, j + 3 * F.n_village));
      }
    for (int n = 0; n < 9; ++n) {
      Line l;
      l << "isl" << n;
      id_or_undef(l, F.settlement(n, n == 0 ? 0 : 1000));
      id_or_undef(l, F.village(n, 1000));
      out << l;
    }
    for (const test::Point& p : test::sample_points(w, r)) {
      const double x = p[0], y = p[1];
      const Urban u = F.urban(x, y);
      std::string parts;
      for (const UrbanPart& part : u.parts) parts += (parts.empty() ? "" : ",") + js::cat(part.s->id, ":", part.w);
      Line lu;
      lu << "u" << x << y << u.u << u.core;
      id_or_undef(lu, u.settlement);
      lu << parts << u.prox;
      out << lu;
      out << (Line() << "f" << F.settlement_proximity(x, y) << F.mountainness(x, y) << F.moisture(x, y) << F.temperature(x, y) << F.district_noise(x, y)
                     << F.industry_noise(x, y) << F.style_noise(x, y) << F.fringe_noise(x, y) << F.settlement_warp(x, y) << F.coast_distance(x, y));
      const std::vector<const Settlement*> near = F.nearest_settlements(x, y);
      const std::vector<const Settlement*> vil = F.nearest_villages(x, y);
      Line ln;
      ln << "ns";
      for (const Settlement* s : near) ln << s->id;
      ln << "|";
      for (const Settlement* v : vil) ln << v->id;
      out << ln;
      const Settlement* s0 = !near.empty() ? near[0] : !vil.empty() ? vil[0] : nullptr;
      if (s0)
        out << (Line() << "sd" << s0->id << F.settlement_distance(*s0, x, y) << F.settlement_distance(*s0, x, y, 1.1)
                       << F.mountains_around(x / 8, y / 8, 900));
    }
    for (int k = 0; k < 30; ++k) {
      const double x0 = js::round((r() - 0.5) * 6e5);
      const double y0 = js::round((r() - 0.5) * 6e5);
      const double dx = std::floor(r() * 2e5);
      const double dy = std::floor(r() * 2e5);
      const Rect rect{x0, y0, x0 + dx, y0 + dy};
      Line l;
      l << "in";
      for (const Settlement* s : F.settlements_in(rect)) l << s->id;
      l << "|";
      for (const Settlement* v : F.villages_in(rect)) l << v->id;
      out << l;
    }
  }
  CHECK(rec::record("fields", out.text()) == rec::recorded_digest("fields"));
}
