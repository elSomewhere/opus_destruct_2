// svx_city tests — the terrain and its landform stack (voxel_city terrain/terrain.js,
// terrain/landforms.js) against the reference (stage "terrain").
#include <doctest.h>

#include "records.hpp"
#include "terrain/terrain.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"
#include "worlds.hpp"

using namespace svx::city;
using rec::Line;

namespace {

void stream_of(Line& l, const std::optional<Stream>& s) {
  if (s)
    l << std::vector<double>{s->d, s->half, s->extra, s->wet ? 1.0 : 0.0};
  else
    l << rec::kUndef;
}

}  // namespace

TEST_CASE("city terrain: the terrain conforms to the reference (stage terrain)") {
  rec::Samples r(37);
  rec::Out out;
  for (const test::WorldCase& ws : test::all_worlds()) {
    const World w(ws.overrides);
    const Terrain& T = *w.terrain;
    std::string forms;
    for (const Terrain::Form& f : T.forms()) forms += (forms.empty() ? "" : ",") + f.lf->id;
    out << (Line() << "terrain" << ws.key << T.sea_level << forms << TerrainCtx(T).torus_r);
    const std::vector<test::Point> pts = test::sample_points(w, r);
    for (const test::Point& p : pts) {
      const double x = p[0], y = p[1];
      const TerrainSample s = T.sample(x, y);
      Line l;
      l << "s" << x << y << s.h << s.natural << s.u << s.core;
      if (s.settlement)
        l << s.settlement->id;
      else
        l << rec::kUndef;
      l << s.grade << s.mountain << s.canyon << s.ravine << s.outcrop;
      stream_of(l, s.stream);
      l << s.rough << s.rugged << s.coast;
      out << l;
    }
    const double n = static_cast<double>(pts.size());
    for (int k = 0; k < 200; ++k) {
      const test::Point& p = pts[static_cast<size_t>(std::floor(r() * n))];
      const double x = p[0], y = p[1];
      const Urban ur = w.fields->urban(x, y);
      const TerrainSample a = T.sample(x, y, &ur);
      const TerrainSample b = T.sample(x + 3, y - 5, nullptr, true);
      const double h = T.height(x - 7, y + 2);
      out << (Line() << "s2" << x << y << a.h << a.rugged << a.coast << b.h << b.natural << b.rugged << b.coast << h);
    }
    for (int k = 0; k < 300; ++k) {
      const test::Point& p = pts[static_cast<size_t>(std::floor(r() * n))];
      const double x = p[0], y = p[1];
      const double u = r() * 0.4;
      std::optional<double> prox;
      if (!(r() < 0.5)) prox = r();
      const FieldPoint f = w.chart->to_field(x * 0.125, y * 0.125);
      TerrainCtx c(T);
      const double h = T.natural(c, x, y, u, f.x, f.y, f.z, prox, f.w);
      Line l;
      l << "nat" << x << y << u;
      if (prox)
        l << *prox;
      else
        l << rec::kUndef;
      l << h << c.lowland << c.mountain << c.ridge << c.canyon << c.ravine << c.outcrop << c.valley << c.rough << c.channel << c.rugged_;
      stream_of(l, c.stream);
      l << c.prox << c.coast << c.cliff_;
      if (c.clim_)
        l << std::vector<double>{c.clim_t, c.clim_m};
      else
        l << rec::kUndef;
      l << c.desert_;
      out << l;
    }
    std::vector<const Settlement*> near = w.fields->settlements_in({-160000, -160000, 160000, 160000});
    for (const Settlement* v : w.fields->villages_in({-80000, -80000, 80000, 80000})) near.push_back(v);
    for (const Settlement* s : near) out << (Line() << "base" << s->id << T.settlement_base(*s));
  }
  CHECK(rec::record("terrain", out.text()) == rec::recorded_digest("terrain"));
}
