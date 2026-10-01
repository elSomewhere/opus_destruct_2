// svx_city tests — the island plans (voxel_city world/island.js) against the reference (stage
// "island").
#include <doctest.h>

#include "config/defaults.hpp"
#include "records.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"
#include "world/island.hpp"
#include "worlds.hpp"

using namespace svx::city;
using rec::Line;

TEST_CASE("city island: island plans conform to the reference (stage island)") {
  rec::Samples r(31);
  rec::Out out;
  for (const test::WorldCase& ws : test::all_worlds()) {
    if (make_config(ws.overrides)["world"]["mode"].str() != "island") continue;
    const World w(ws.overrides);
    const IslandPlan& P = *w.fields->island;
    out << (Line() << "plan" << ws.key << P.R << P.theta << P.a << P.b << P.cos_ << P.sin_ << P.hdx << P.hdy << P.fjord_count << P.fjord_phase << P.ox
                   << P.oy << P.high_threshold);
    for (const IslandSite& s : P.sites) out << (Line() << "site" << s.kind << s.lx << s.ly << s.radius << s.importance);
    const IslandSettlements& st = P.settlements(*w.fields);
    for (const auto* list : {&st.towns, &st.villages})
      for (const auto& s : *list) {
        Line l;
        l << "place";
        test::settlement_fields(l, *s);
        out << l;
      }
    const std::optional<Harbour>& h = P.harbour();
    if (h)
      out << (Line() << "harbour" << h->r << h->x << h->y << h->dx << h->dy);
    else
      out << (Line() << "harbour" << rec::kUndef);
    const Rect b = P.bounds();
    out << (Line() << "bounds" << b.x0 << b.y0 << b.x1 << b.y1);
    for (int k = 0; k < 3000; ++k) {
      const double x = b.x0 + r() * (b.x1 - b.x0);
      const double y = b.y0 + r() * (b.y1 - b.y0);
      const double d = (r() - 0.5) * 6000;
      const auto [lx, ly] = P.local(x, y);
      const double c = P.coast(x, y);
      out << (Line() << "pt" << x << y << lx << ly << P.shape(lx, ly) << P.high_raw(lx, ly) << P.highland_local(lx, ly) << P.fjord_cut(lx, ly, d) << c
                     << P.highland(x, y) << P.cliff(x, y) << P.skerry(x, y, c) << P.skerry(x, y, -d / 4));
    }
    const TrunkEdges& edges = P.trunk_edges(w);
    Line l;
    l << "trunk" << double(edges.size());
    for (const auto& e : edges.items()) l << js::cat(e[0], ":", e[1], ":", e[2]);
    out << l;
  }
  CHECK(rec::record("island", out.text()) == rec::recorded_digest("island"));
}
