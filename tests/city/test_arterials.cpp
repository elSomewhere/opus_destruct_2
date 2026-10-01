// svx_city tests — the arterial grid, World::cell_at / cells_overlapping and the road classes
// (voxel_city network/arterials.js, network/roadClasses.js) against the reference (stage
// "arterials").
#include <doctest.h>

#include "config/defaults.hpp"
#include "network/arterials.hpp"
#include "network/roadClasses.hpp"
#include "records.hpp"
#include "world/World.hpp"
#include "worlds.hpp"

using namespace svx::city;
using rec::Line;

TEST_CASE("city arterials: the arterial grid and road classes conform to the reference (stage arterials)") {
  rec::Samples r(17);
  rec::Out out;
  for (const test::WorldSpec& ws : test::worlds()) {
    const Value cfg = make_config(test::world_overrides(ws));
    const ArterialGrid A(cfg);
    out << (Line() << "grid" << ws.key << A.seed << A.spacing << A.jitter << A.n);
    Line l0, l1;
    l0 << "l0";
    l1 << "l1";
    for (int i = -40; i <= 40; ++i) {
      l0 << A.line(0, i);
      l1 << A.line(1, i);
    }
    out << l0 << l1;
    for (int k = 0; k < 40; ++k) {
      const double i = std::floor((r() - 0.5) * 2e5);
      out << (Line() << "lf" << i << A.line(0, i) << A.line(1, i) << A.line_at(0, i) << A.line_at(1, i) << A.canon(i));
    }
    for (int k = 0; k < 300; ++k) {
      const double x = js::round((r() - 0.5) * 6e5);
      const double y = js::round((r() - 0.5) * 6e5);
      const CellIJ c = A.cell_at(x, y);
      const Rect rc = A.cell_rect(c.i, c.j);
      out << (Line() << "c" << x << y << c.i << c.j << rc.x0 << rc.y0 << rc.x1 << rc.y1 << A.index_at(0, x + 0.5) << A.index_at(1, y - 0.25));
    }
    if (cfg["world"]["mode"].str() != "island") {
      const World w(test::world_overrides(ws));
      for (int k = 0; k < 20; ++k) {
        const double x0 = js::round((r() - 0.5) * 2e5);
        const double y0 = js::round((r() - 0.5) * 2e5);
        const double dx = std::floor(r() * 20000);
        const double dy = std::floor(r() * 20000);
        const Rect rect{x0, y0, x0 + dx, y0 + dy};
        const CellIJ a = w.cell_at(rect.x0, rect.y1);
        Line l;
        l << "co" << a.i << a.j;
        for (const CellIJ& c : w.cells_overlapping(rect)) l << c.i << c.j;
        out << l;
      }
    }
    const RoadSpecs specs = road_specs(cfg);
    for (const RoadSpec& s : specs.all())
      out << (Line() << "spec" << s.cls << s.cls << s.lanes << s.lane << s.median << s.parking << s.shoulder << s.sidewalk << s.hc << s.hr << s.corner << s.paved);
  }
  for (const char* cls : kRoadClasses) {
    Line l;
    l << "rank" << cls;
    const double rank = class_rank(cls);
    if (rank == rank)
      l << rank;
    else
      l << rec::kUndef;
    out << l;
  }
  {
    Line l;
    l << "rank" << "nope";
    if (class_rank("nope") == class_rank("nope"))
      l << class_rank("nope");
    else
      l << rec::kUndef;
    out << l;
  }
  CHECK(rec::record("arterials", out.text()) == rec::recorded_digest("arterials"));
}
