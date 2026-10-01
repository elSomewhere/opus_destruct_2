// svx_city tests — a parking garage's ramps as pitched parts (voxel_city buildings/garageRamps.js)
// against the reference (stage "garageramps"): the parts of scripted ramps and their content in their
// own lattices.
#include <doctest.h>

#include "buildings/archetypes.hpp"
#include "buildings/garageRamps.hpp"
#include "buildings/styles.hpp"
#include "network/arterials.hpp"
#include "records.hpp"
#include "shell_records.hpp"
#include "voxel/chunk.hpp"
#include "world/register_all.hpp"

using namespace svx::city;
using namespace svx::city::test;
using rec::Line;

namespace {

// stages/garageramps.mjs partFields
void part_fields(Line& l, const Part& p) {
  const Placement& pl = p.placement;
  const LocalBox& e = p.extent;
  const Box3& a = p.aabb;
  l << p.key << p.id << std::vector<double>{p.cell[0], p.cell[1]} << p.kind << std::vector<double>{pl.origin.x, pl.origin.y, pl.origin.z} << pl.yaw << pl.yaw2
    << pl.pitch << pl.roll << pl.anchored << pl.priority << std::vector<double>(pl.m.begin(), pl.m.end()) << pl.d
    << std::vector<double>{e.u0, e.v0, e.w0, e.u1, e.v1, e.w1} << std::vector<double>{a.x0, a.y0, a.z0, a.x1, a.y1, a.z1}
    << std::vector<double>{p.home.cx, p.home.cy, p.home.cz} << std::vector<double>{p.base.x, p.base.y, p.base.z} << p.reach << p.anchored << p.priority << p.env
    << std::vector<double>{p.ramp->f, p.ramp->W, p.ramp->Lu};
}

}  // namespace

TEST_CASE("city garage ramps: parts and their lattices are the reference's (stage garageramps)") {
  register_all();
  rec::Out out;
  rec::Samples r(71);
  const std::vector<District> DS = district_list();
  std::vector<std::string> styles;
  for (const Style& s : style_registry().all()) styles.push_back(s.id);
  const Archetype& garage = archetype_registry().get("garage");
  double k = 0;
  for (int n = 0; n < 80; ++n) {
    const World& w = shell_world(static_cast<size_t>(std::floor(r() * 3)));
    const std::string& style = styles[static_cast<size_t>(std::floor(r() * static_cast<double>(styles.size())))];
    const District& d = DS[static_cast<size_t>(std::floor(r() * static_cast<double>(DS.size())))];
    ShellCase sc = shell_envelope(r, garage, &style, d, w, (k += 1));
    out << (Line() << env_line(sc.env));
    if (!sc.env) continue;
    const Envelope& env = *sc.env;
    const CellIJ c = w.cell_at((env.R.x0 + env.R.x1) / 2, (env.R.y0 + env.R.y1) / 2);
    PartCell cell;
    cell.i = c.i;
    cell.j = c.j;
    cell.ci = w.arterials->canon(c.i);
    cell.cj = w.arterials->canon(c.j);
    cell.rect = w.arterials->cell_rect(c.i, c.j);
    const Rect fp = tier_rects(env, 0)[0];
    for (int q = 0; q < 4; ++q) {
      const double RW = 24 + std::floor(r() * 16);
      const double L = 60 + std::floor(r() * 160);
      const bool left = r() < 0.5;
      const double f = std::floor(r() * js::max(1.0, env.floors - 1));
      const int pitch = static_cast<int>(1 + std::floor(r() * 6));
      const double y = r();
      const double x0 = left ? fp.x0 + 4 : fp.x1 - 4 - RW + 1;
      const double y0 = fp.y0 + 4 + std::floor(y * js::max(1.0, fp.y1 - fp.y0 - L - 8));
      const Rect rect{x0, y0, x0 + RW - 1, y0 + L - 1};
      const Part part = ramp_part(cell, env, rect, f, pitch);
      Line pl;
      pl << "ramp";
      part_fields(pl, part);
      out << pl;
      const LocalBox& e = part.extent;
      const std::vector<std::array<double, 3>> pts = {
          {e.u0, e.v0, e.w0},
          {e.u1, e.v1, e.w1},
          {(e.u0 + e.u1) / 2, (e.v0 + e.v1) / 2, 0},
      };
      for (const ChunkAt& ca : chunks_at({0, 1, 2, 3}, pts)) {
        ChunkBuffer ch(static_cast<int>(ca[0]), ca[1], ca[2], ca[3]);
        rasterize_ramp_part(w, part, ch);
        Line l;
        l << "c" << ca[0] << ca[1] << ca[2] << ca[3];
        chunk_digest(l, ch);
        out << l;
      }
    }
  }
  CHECK(rec::record("garageramps", out.text()) == rec::recorded_digest("garageramps"));
}
