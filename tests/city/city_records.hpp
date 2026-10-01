// svx_city tests — the records of the city stages (cellnet, streets, townplan): cell networks,
// their edges, roads, blocks and sub-cells as lines (tools/procgen_ref/lib/city.mjs is the Node
// twin), the cells a world's records cover (stages/cellnet.mjs cellsOf) and the worlds of the
// city stages (lib/worlds.mjs cityWorlds).
#pragma once

#include <array>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "city/blockPoly.hpp"
#include "city/cellNetwork.hpp"
#include "network/arterials.hpp"
#include "records.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"
#include "world/island.hpp"
#include "worlds.hpp"

namespace svx::city::test {

// A string field the port keeps as "" where JS has null or undefined ("-").
inline std::string fo(const std::string& s) { return s.empty() ? std::string("-") : s; }
inline std::string fo(const std::optional<std::string>& s) { return s ? (s->empty() ? std::string("\"\"") : *s) : std::string("-"); }
inline std::string frect(const Rect& r) { return js::cat(r.x0, ",", r.y0, ",", r.x1, ",", r.y1); }
inline std::string fside(const RoadSide& s) { return js::cat(fo(s.cls), "/", s.hr, "/", fo(s.id)); }
inline std::string fsides(const BlockSides& s) { return fside(s.N) + ";" + fside(s.E) + ";" + fside(s.S) + ";" + fside(s.W); }
inline std::string fpts(const std::vector<RoadPt>& pts) {
  std::string out;
  for (size_t i = 0; i < pts.size(); ++i) out += js::cat(i ? ";" : "", pts[i].x, ",", pts[i].y);
  return out;
}
inline std::string fpoly(const Poly* p) {
  if (!p) return "-";
  std::string out;
  for (size_t i = 0; i < p->size(); ++i) out += js::cat(i ? ";" : "", (*p)[i].x, ",", (*p)[i].y, ",", fside((*p)[i].side));
  return out;
}
inline std::string fcuts(const std::vector<BlockCut>* cuts) {
  if (!cuts) return "-";
  std::string out;
  for (size_t i = 0; i < cuts->size(); ++i) {
    const BlockCut& k = (*cuts)[i];
    out += js::cat(i ? ";" : "", k.nx, ",", k.ny, ",", k.c, ",", fo(k.cls), ",", k.hr, ",", fo(k.id));
  }
  return out;
}

inline rec::Line edge_line(const std::string& tag, const EdgeInfo& e) {
  rec::Line l;
  l << tag << e.axis << e.line << e.span << e.fixed << e.s0 << e.s1 << fo(e.cls) << (e.spec ? e.spec->cls : std::string("-")) << e.hr << e.wob << fo(e.paving)
    << e.u << fpts(e.pts);
  return l;
}

inline rec::Line road_line(const Road& r) {
  rec::Line l;
  l << "road" << r.id << r.cell << r.cls << r.hc << r.hr << r.corner << r.median << r.parking << r.lanes << r.lane << r.sidewalk << r.shoulder
    << (r.home ? js::cat((*r.home)[0], ",", (*r.home)[1]) : std::string("-")) << fo(r.arterial_edge) << fo(r.paving)
    << (r.diagonal ? js::cat(r.diagonal->f, ",", r.diagonal->k) : std::string("-")) << fo(r.sub) << fo(r.strip) << fpts(r.pts);
  return l;
}

inline rec::Line block_line(const Block& b) {
  rec::Line l;
  l << "block" << b.id << b.cell << b.sub << b.district << frect(b.rect) << fsides(b.sides) << frect(b.prop) << b.u << b.core << fpoly(b.poly ? &*b.poly : nullptr)
    << fcuts(b.poly ? &b.cuts : nullptr);
  return l;
}

inline rec::Line sub_line(const SubCell& s) {
  rec::Line l;
  l << "sub" << s.id << frect(s.rect) << fsides(s.sides) << s.district << s.u << s.core << fo(s.settlement) << s.fringe;
  return l;
}

inline void net_lines(rec::Out& out, const CellNet& net) {
  out << (rec::Line() << "cell" << net.id << net.i << net.j << frect(net.rect) << net.roads.size() << net.blocks.size() << net.subcells.size());
  out << edge_line("edgeW", net.edges.W) << edge_line("edgeE", net.edges.E) << edge_line("edgeN", net.edges.N) << edge_line("edgeS", net.edges.S);
  for (const auto& r : net.roads) out << road_line(*r);
  for (const Block& b : net.blocks) out << block_line(b);
  for (const SubCell& s : net.subcells) out << sub_line(s);
}

// The worlds of the city stages: all_worlds(), then the other angled presets.
inline std::vector<WorldCase> city_worlds() {
  std::vector<WorldCase> out = all_worlds();
  const WorldSpec extra[] = {
      {"angledInfiniteCity", "angledInfiniteCity", ""},
      {"angledNordicTown:skerry", "angledNordicTown", "skerry"},
      {"angledNordicTown:fjord", "angledNordicTown", "fjord"},
      {"angledNordicTown:forest", "angledNordicTown", "forest"},
  };
  for (const WorldSpec& w : extra) out.push_back({w.key, world_overrides(w)});
  return out;
}

using Cell = std::array<double, 2>;

// The cells a world's records cover, drawn from r (stages/cellnet.mjs cellsOf): round the spawn,
// towns' centres, middles, edges and outskirts, villages, open country, a lap away, an island.
inline std::vector<Cell> cells_of(const World& w, rec::Samples& r) {
  constexpr double kPi = 3.141592653589793;
  const ArterialGrid& A = *w.arterials;
  const MacroFields& F = *w.fields;
  std::vector<Cell> out;
  std::set<std::pair<double, double>> seen;
  auto add = [&](double i, double j) {
    if (seen.insert({i, j}).second) out.push_back({i, j});
  };
  auto at = [&](double x, double y) {
    const CellIJ c = A.cell_at(js::round(x), js::round(y));
    add(c.i, c.j);
  };
  const CellIJ c0 = A.cell_at(0, 0);
  for (int dj = -1; dj <= 1; ++dj)
    for (int di = -1; di <= 1; ++di) add(c0.i + di, c0.j + dj);
  std::vector<const Settlement*> towns = F.settlements_in({-120000, -120000, 120000, 120000});
  if (towns.size() > 6) towns.resize(6);
  for (const Settlement* s : towns)
    for (const double d : {0.0, 0.45, 0.85, 1.25, 1.8}) {
      const double a = r() * 2 * kPi;
      at(s->x + js::cos(a) * d * s->radius, s->y + js::sin(a) * d * s->radius);
    }
  std::vector<const Settlement*> villages = F.villages_in({-60000, -60000, 60000, 60000});
  if (villages.size() > 6) villages.resize(6);
  for (const Settlement* v : villages)
    for (const double d : {0.0, 1.1}) {
      const double a = r() * 2 * kPi;
      at(v->x + js::cos(a) * d * v->radius, v->y + js::sin(a) * d * v->radius);
    }
  for (int k = 0; k < 6; ++k) {
    const double x = (r() - 0.5) * 400000;
    const double y = (r() - 0.5) * 400000;
    at(x, y);
  }
  if (A.n != 0) {
    const double laps[3][2] = {{1, 0}, {-1, 1}, {2, -1}};
    for (const auto& l : laps) add(c0.i + l[0] * A.n, c0.j + l[1] * A.n + l[0]);
  }
  if (F.island) {
    const Rect b = F.island->bounds();
    for (int k = 0; k < 10; ++k) {
      const double x = (b.x0 + r() * (b.x1 - b.x0)) * 8;
      const double y = (b.y0 + r() * (b.y1 - b.y0)) * 8;
      at(x, y);
    }
  }
  return out;
}

}  // namespace svx::city::test
