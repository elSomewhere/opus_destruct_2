// svx_city tests — the site kinds (voxel_city sites/militaryBase.js, researchComplex.js,
// mountainBase.js) and the site kit (sites/kit.js) against the reference (stage "sitekinds" of
// tools/procgen_ref): the kit's pieces on their own, then on create_world's worlds (and one World
// of World.js) the sites of the lattice cells round the origin and of a few beyond - placement,
// plans, surface envelopes, ground, structures and complexes, ports, the site source's z ranges
// and chunks -; and the same on four threads, each in an order of its own.
#include <doctest.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "building_records.hpp"
#include "complexes.hpp"
#include "records.hpp"
#include "sites/militaryBase.hpp"
#include "sites/mountainBase.hpp"
#include "sites/researchComplex.hpp"
#include "voxel/chunk.hpp"
#include "voxel/materials.hpp"
#include "world/sites.hpp"
#include "worlds.hpp"

using namespace svx::city;
using rec::Line;

namespace {

// Worlds with parked vehicles (trucks at the sites), probed round the origin only
// (stages/sitekinds.mjs PARKED_WORLDS).
const std::vector<test::ExtraWorld>& parked_worlds() {
  static const std::vector<test::ExtraWorld> w = {
      {"parkedSpawnNear", R"({"seed":8,"terrain":{"spawnMountains":[1,2]},"vehicles":{"parked":true}})"},
      {"parkedMountains", R"({"seed":4,"terrain":{"mountainBelt":[0,0.01]},"vehicles":{"parked":true}})"},
  };
  return w;
}

std::vector<double> rv(const Rect& q) { return {q.x0, q.y0, q.x1, q.y1}; }
// (an array's toString: its elements joined by commas)
std::string rs(const Rect& q) { return js::cat(q.x0, ",", q.y0, ",", q.x1, ",", q.y1); }
std::string pt(const Point2& p) { return js::cat(p.x, ",", p.y); }
std::string heli(const SiteHelipad& h) { return js::cat(h.x, ",", h.y, ",", h.r); }
std::string join(const std::vector<std::string>& v, const char* sep) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) s += (i ? sep : "") + v[i];
  return s;
}
std::string ids(const std::vector<std::shared_ptr<const Site>>& list) {
  std::vector<std::string> v;
  for (const auto& s : list) v.push_back(s->id);
  return v.empty() ? "none" : join(v, ",");
}

void rect_fields(Line& l, const Rect& q) { l << q.x0 << q.y0 << q.x1 << q.y1; }

// ---- the kit on its own (kitLines)

void kit_lines(rec::Out& out, rec::Samples& r) {
  const char sides[4] = {'N', 'S', 'W', 'E'};
  for (int k = 0; k < 16; ++k) {
    const double x0 = js::round((r() - 0.5) * 20000);
    const double y0 = js::round((r() - 0.5) * 20000);
    const double x1 = x0 + 200 + std::floor(r() * 900);
    const double y1 = y0 + 200 + std::floor(r() * 900);
    const Rect rect{x0, y0, x1, y1};
    const double z = js::round((r() - 0.5) * 400);
    const char side = sides[k % 4];
    const bool ns = side == 'N' || side == 'S';
    const double mid = ns ? js::round((rect.x0 + rect.x1) / 2) : js::round((rect.y0 + rect.y1) / 2);
    const double at = side == 'N' ? rect.y0 : side == 'S' ? rect.y1 : side == 'W' ? rect.x0 : rect.x1;
    // (the gate mid-side; from 8 on at a corner: the side's start, from 12 its end)
    const double lo = k < 8 ? mid - 32 : k < 12 ? (ns ? rect.x0 : rect.y0) - 10 : (ns ? rect.x1 : rect.y1) - 54;
    const Rect gate = ns ? Rect{lo, at, lo + 64, at} : Rect{at, lo, at, lo + 64};
    std::vector<SiteBox> boxes;
    char open = 0;
    if (k >= 4) {
      open = sides[(k + 1) % 4];
      FenceSkip skip;
      skip.N = open == 'N';
      skip.S = open == 'S';
      skip.W = open == 'W';
      skip.E = open == 'E';
      fence(boxes, rect, z, side, gate, 22 + (k % 3) * 2, skip);
    } else {
      fence(boxes, rect, z, side, gate);
    }
    gate_booth(boxes, gate, side, z);
    Line l;
    l << "kit" << "fence" << side;
    rect_fields(l, rect);
    l << z;
    if (open)
      l << open;
    else
      l << rec::kUndef;
    out << l;
    test::box_lines(out, "B", boxes);
  }
  const double x = js::round((r() - 0.5) * 20000);
  const double y = js::round((r() - 0.5) * 20000);
  const double z = js::round((r() - 0.5) * 400);
  const Rect rect{x, y, x + 640, y + 480};
  struct Piece {
    const char* id;
    std::function<void(std::vector<SiteBox>&)> fn;
  };
  const std::vector<Piece> pieces = {
      {"watchtowers", [&](std::vector<SiteBox>& o) { watchtowers(o, rect, z); }},
      {"radarMast", [&](std::vector<SiteBox>& o) { radar_mast(o, x, y, z); }},
      {"radome", [&](std::vector<SiteBox>& o) { radome(o, x, y, z); }},
      {"radome44", [&](std::vector<SiteBox>& o) { radome(o, x, y, z, 44); }},
      {"dishArray", [&](std::vector<SiteBox>& o) { dish_array(o, x, y, z); }},
      {"dishArray72", [&](std::vector<SiteBox>& o) { dish_array(o, x, y, z, 3, 72); }},
      {"fuelTanks", [&](std::vector<SiteBox>& o) { fuel_tanks(o, x, y, z); }},
      {"fuelTanks2", [&](std::vector<SiteBox>& o) { fuel_tanks(o, x, y, z, 2); }},
  };
  for (const Piece& p : pieces) {
    std::vector<SiteBox> boxes;
    p.fn(boxes);
    out << (Line() << "kit" << p.id << x << y << z);
    test::box_lines(out, "B", boxes);
  }
  Rng rng(4242);
  for (int k = 0; k < 4; ++k) {
    std::vector<SiteBox> boxes;
    truck(boxes, x + k * 40, y, z, rng);
    out << (Line() << "kit" << "truck" << k);
    test::box_lines(out, "B", boxes);
  }
  for (const int accent : {-1, static_cast<int>(MAT::SIGN_BLUE)}) {
    BoxLists lists;
    if (accent < 0)
      portal_block(lists, rect, z);
    else
      portal_block(lists, rect, z, static_cast<uint16_t>(accent));
    Line l;
    l << "kit" << "portalBlock";
    if (accent < 0)
      l << rec::kUndef;
    else
      l << accent;
    out << l;
    test::box_lines(out, "S", lists.shells);
    test::box_lines(out, "C", lists.carves);
    test::box_lines(out, "D", lists.details);
  }
}

// ---- a site's records (siteLines)

void pad_line(rec::Out& out, const SitePad& p) {
  Line l;
  l << "pad";
  rect_fields(l, p.rect);
  l << p.z << p.margin;
  if (p.round)
    l << true;
  else
    l << rec::kUndef;
  if (p.ramp)
    l << "ramp";
  else
    l << rec::kUndef;
  if (!p.path.empty()) {
    std::string s = js::cat(p.path.size(), ":");
    for (size_t k = 0; k < p.path.size(); ++k) s += js::cat(k ? ";" : "", p.path[k].x, ",", p.path[k].y, ",", p.path[k].z);
    l << s << p.half;
  } else {
    l << rec::kUndef << rec::kUndef;
  }
  if (p.road)
    l << true;
  else
    l << rec::kUndef;
  out << l;
}

std::string lots_str(const std::vector<SiteLot>& lots) {
  std::vector<std::string> v;
  for (const SiteLot& l : lots) v.push_back(js::cat(l.kind, "/", rs(l.rect), "/", l.front, "/", l.style.empty() ? std::string("-") : l.style));
  return join(v, " ");
}

void site_lines(rec::Out& out, const Site& s) {
  {
    Line l;
    l << "site" << s.id << s.type << s.a << s.b << s.cell.i << s.cell.j;
    rect_fields(l, s.cell_rect);
    rect_fields(l, s.rect);
    rect_fields(l, s.blend);
    l << s.margin << s.pad_z << s.seed << s.center.x << s.center.y;
    if (s.plan->bounds)
      l << rv(*s.plan->bounds);
    else
      l << rec::kUndef;
    l << rv(s.placed->footprint) << s.pads().size();
    out << l;
  }
  for (const SitePad& p : s.pads()) pad_line(out, p);
  if (s.type == "mountainBase") {
    const MountainPlaced& pl = static_cast<const MountainPlaced&>(*s.placed);
    const MountainGate& g = pl.gate;
    out << (Line() << "placed" << pl.side << pl.dist << pl.L << pl.Wd << pl.H << pl.A << pl.B << rv(pl.apron) << rv(pl.tunnel) << rv(pl.cav_rect)
                   << js::cat(g.u, ",", g.v, ",", g.side, ",", g.out[0], ":", g.out[1], ",", g.x, ",", g.y, ",", g.err) << pl.road.size());
    const MountainBasePlan& p = static_cast<const MountainBasePlan&>(*s.plan);
    const Cavern& c = *p.cavern;
    std::vector<std::string> clear;
    for (const Rect& q : *p.clear) clear.push_back(rs(q));
    out << (Line() << "plan" << p.flavor << p.frame.side << p.side << p.gate_side << rv(p.gate) << std::vector<double>{c.cx, c.cy, c.A, c.B, c.H, c.Hw, c.zf} << rv(p.road)
                   << rv(p.yard) << rv(p.portal) << p.portals.size() << heli(p.helipad) << rv(p.apron) << rv(p.tunnel) << p.dist << join(p.themes, ",") << p.accent
                   << join(clear, ";") << rv(*p.bounds) << std::vector<double>{p.levels[0], p.levels[1]});
    out << (Line() << "lots" << lots_str(p.lots));
  } else if (s.type == "militaryBase") {
    const MilitaryBasePlan& p = static_cast<const MilitaryBasePlan&>(*s.plan);
    out << (Line() << "plan" << p.gate_side << rv(p.gate) << rv(*p.drive) << rv(p.ring) << p.ring_w << rv(p.inner) << heli(p.helipad) << rv(p.bunker) << rv(p.apron)
                   << rv(*p.bounds) << p.levels);
    out << (Line() << "lots" << lots_str(p.lots));
  } else {
    const ResearchComplexPlan& p = static_cast<const ResearchComplexPlan&>(*s.plan);
    std::vector<std::string> portals, radomes;
    for (const Rect& q : p.portals) portals.push_back(rs(q));
    for (const Point2& q : p.radomes) radomes.push_back(pt(q));
    out << (Line() << "plan" << p.gate_side << rv(p.gate) << rv(*p.drive) << rv(p.ring) << p.ring_w << rv(p.inner) << heli(p.helipad) << join(portals, ";")
                   << join(radomes, ";") << pt(p.dishes) << pt(p.tanks) << join(p.themes, ",") << rv(*p.bounds)
                   << std::vector<double>{p.levels[0], p.levels[1], p.levels[2], p.levels[3]});
    out << (Line() << "lots" << lots_str(p.lots));
  }
}

Rect grow(const Rect& q, double m) { return {q.x0 - m, q.y0 - m, q.x1 + m, q.y1 + m}; }

// Points (voxels) to ask the ground at, over a site's pads and features (probes; draws from r).
std::vector<std::array<double, 2>> probes(const Site& s, rec::Samples& r) {
  std::vector<std::array<double, 2>> pts;
  auto in_rect = [&](const Rect& q, int n) {
    for (int k = 0; k < n; ++k) {
      const double x = js::round(q.x0 + r() * (q.x1 - q.x0));
      const double y = js::round(q.y0 + r() * (q.y1 - q.y0));
      pts.push_back({x, y});
    }
  };
  in_rect(s.plan->bounds ? *s.plan->bounds : s.blend, 8);
  if (s.type == "mountainBase") {
    const MountainBasePlan& p = static_cast<const MountainBasePlan&>(*s.plan);
    const SiteHelipad& h = p.helipad;
    in_rect({h.x - h.r, h.y - h.r, h.x + h.r, h.y + h.r}, 6);
    in_rect(p.apron, 8);
    in_rect(grow(p.apron, 176), 8);
    // the lane on the tunnel's axis, its yellow line
    const bool along_x = p.side == 'E' || p.side == 'W';
    for (int k = 0; k < 2; ++k) {
      const double t = r();
      if (along_x)
        pts.push_back({js::round(p.apron.x0 + t * (p.apron.x1 - p.apron.x0)), std::floor(s.center.y)});
      else
        pts.push_back({std::floor(s.center.x), js::round(p.apron.y0 + t * (p.apron.y1 - p.apron.y0))});
    }
    // the road: its centre line, its edges and its verges
    const std::vector<SitePathPoint>& path = static_cast<const MountainPlaced&>(*s.placed).road[0].path;
    for (int k = 0; k < 4; ++k) {
      const size_t m = static_cast<size_t>(std::floor(r() * static_cast<double>(path.size() - 1)));
      const SitePathPoint& a = path[m];
      const SitePathPoint& b = path[m + 1];
      const double L = js::or_(js::hypot(b.x - a.x, b.y - a.y), 1);
      const double nx = -(b.y - a.y) / L;
      const double ny = (b.x - a.x) / L;
      for (const double off : {0.0, 27.5, -27.5, 40.0}) pts.push_back({a.x + nx * off, a.y + ny * off});
    }
  } else {
    const SitePlan& base = *s.plan;
    const SiteHelipad& h = s.type == "militaryBase" ? static_cast<const MilitaryBasePlan&>(base).helipad : static_cast<const ResearchComplexPlan&>(base).helipad;
    in_rect({h.x - h.r, h.y - h.r, h.x + h.r, h.y + h.r}, 6);
    in_rect(s.rect, 10);
    const Rect& d = *base.drive;
    in_rect(d, 3);
    const char gs = s.type == "militaryBase" ? static_cast<const MilitaryBasePlan&>(base).gate_side : static_cast<const ResearchComplexPlan&>(base).gate_side;
    const bool ns = gs == 'N' || gs == 'S';
    const double t = r();
    if (ns)
      pts.push_back({(d.x0 + d.x1) / 2, js::round(d.y0 + t * (d.y1 - d.y0))});
    else
      pts.push_back({js::round(d.x0 + t * (d.x1 - d.x0)), (d.y0 + d.y1) / 2});
    if (s.type == "militaryBase") {
      const MilitaryBasePlan& p = static_cast<const MilitaryBasePlan&>(base);
      in_rect(p.ring, 4);
      for (const SiteLot& l : p.lots) in_rect(grow(l.rect, 40), 2);
      in_rect(p.bunker, 2);
      in_rect(p.apron, 2);
    } else {
      const ResearchComplexPlan& p = static_cast<const ResearchComplexPlan&>(base);
      in_rect(p.ring, 4);
      for (const SiteLot& l : p.lots) in_rect(grow(l.rect, 40), 2);
      for (const Rect& q : p.portals) in_rect(grow(q, 32), 2);
    }
  }
  return pts;
}

// Points (x, y, z voxels) worth a chunk: the complex, the surface structures, the cavern
// (chunkPoints).
std::vector<std::array<double, 3>> chunk_points(const Site& s, const Complex& u) {
  std::vector<std::array<double, 3>> pts = test::complex_points(u);
  const double z = s.pad_z;
  pts.push_back({js::round(s.center.x), js::round(s.center.y), z});
  if (s.type == "mountainBase") {
    const MountainBasePlan& p = static_cast<const MountainBasePlan&>(*s.plan);
    pts.push_back({js::round((p.gate.x0 + p.gate.x1) / 2), js::round((p.gate.y0 + p.gate.y1) / 2), z + 10});
    pts.push_back({p.helipad.x, p.helipad.y, z});
    const Cavern& c = *p.cavern;
    const Point2 a = p.frame.to_world(p.dist, 0);
    pts.push_back({a.x, a.y, z + 20});
    pts.push_back({p.portal.x0, p.portal.y0, c.zf + 20});
    pts.push_back({p.road.x0, p.road.y0, c.zf + 30});
  } else if (s.type == "militaryBase") {
    const MilitaryBasePlan& p = static_cast<const MilitaryBasePlan&>(*s.plan);
    pts.push_back({js::round((p.gate.x0 + p.gate.x1) / 2), js::round((p.gate.y0 + p.gate.y1) / 2), z + 10});
    pts.push_back({p.helipad.x, p.helipad.y, z});
    pts.push_back({p.bunker.x0, p.bunker.y0, z + 20});
    pts.push_back({s.rect.x0 + 6, s.rect.y0 + 6, z + 70});
    pts.push_back({p.apron.x1 - 32, p.apron.y0 + 32, z + 120});
  } else {
    const ResearchComplexPlan& p = static_cast<const ResearchComplexPlan&>(*s.plan);
    pts.push_back({js::round((p.gate.x0 + p.gate.x1) / 2), js::round((p.gate.y0 + p.gate.y1) / 2), z + 10});
    pts.push_back({p.helipad.x, p.helipad.y, z});
    pts.push_back({p.portals[0].x0, p.portals[0].y0, z + 20});
    pts.push_back({s.rect.x0 + 6, s.rect.y0 + 6, z + 70});
    pts.push_back({p.radomes[0].x, p.radomes[0].y, z + 40});
    pts.push_back({p.dishes.x, p.dishes.y, z + 30});
  }
  return pts;
}

// A ground-filled chunk at (x, y, z) with the site source rasterized into it (chunkLine).
void chunk_line(rec::Out& out, const World& w, const Site& s, int lod, double x, double y, double z, double surface) {
  const double e = static_cast<double>(32 << lod);
  const double cx = std::floor(x / e);
  const double cy = std::floor(y / e);
  const double cz = std::floor(z / e);
  ChunkBuffer ch = test::ground_chunk(lod, cx, cy, cz, surface);
  const double s0 = static_cast<double>(1 << lod);
  const double bx = (cx * 32 - 1) * s0;
  const double by = (cy * 32 - 1) * s0;
  const Rect rect{bx, by, bx + 34 * s0 - 1, by + 34 * s0 - 1};
  double z0 = 0, z1 = 0;
  const bool has = site_source_z_range(w, rect, &z0, &z1);
  site_source_rasterize(w, ch, s.pad_z - 6);
  Line l;
  l << "ch" << s.id << lod << cx << cy << cz;
  if (has)
    l << std::vector<double>{z0, z1};
  else
    l << rec::kUndef;
  const int count = ch.count_non_air();
  l << count << test::chunk_digest(ch.data);
  out << l;
}

std::string ground_line(const SiteLayer& L, double x, double y, double nat) {
  const std::optional<SiteGround> g = L.ground(x, y, nat);
  if (!g) return (Line() << "g" << x << y << nat << rec::kUndef).str();
  Line l;
  l << "g" << x << y << nat << g->z << g->mat << g->sub << g->site->id << static_cast<double>(g->pad - g->site->pads().data()) << g->natural << g->inside;
  if (!g->pad->path.empty())
    l << std::vector<double>{g->path_z, g->path_s, g->path_c};
  else
    l << rec::kUndef;
  return l.str();
}

std::string structure_line(const World& w, const Site& s) {
  const SiteStructure& st = s.structure(w);
  rec::Out boxes;
  for (const SiteBox& q : st.boxes) boxes << (Line() << q.x0 << q.y0 << q.z0 << q.x1 << q.y1 << q.z1 << q.m << q.mode);
  const Complex& u = *st.under;
  rec::Out cl;
  test::complex_lines(cl, u);
  Line l;
  l << "st" << s.id << st.boxes.size() << test::text_digest(boxes.text()) << std::vector<double>{st.bb->x0, st.bb->y0, st.bb->z0, st.bb->x1, st.bb->y1, st.bb->z1}
    << st.custom.size() << js::cat(u.sectors.size(), "/", u.levels.size(), "/", u.shafts.size(), "/", u.ladders.size(), "/", u.tram ? u.tram->stations.size() : 0)
    << test::text_digest(cl.text());
  return l.str();
}

std::string port_line(const World& w, const Site& s, const Point2* toward) {
  const std::optional<SitePort> q = s.def->port(w, s, toward);
  Line l;
  l << "port" << s.id;
  if (toward)
    l << pt(*toward);
  else
    l << rec::kUndef;
  if (q)
    l << js::cat("[", q->x, ",", q->y, ",", q->z, ",", q->d == q->d ? js::num(q->d) : std::string("-"), "]");
  else
    l << rec::kUndef;
  return l.str();
}

std::string surface_line(const SiteSurface& e) {
  Line l;
  l << "surf" << e.lot.id << rv(e.lot.rect) << e.lot.front << e.lot.district << e.lot.ground_z << e.lot.block << e.lot.cell << e.lot.underground;
  return l.str() + " " + test::env_line(e.env);
}

// Lattice cells beyond a world's window where a stronghold's flank search meets what the window's
// do not (stages/sitekinds.mjs FAR_CELLS).
const std::vector<std::pair<std::string, std::vector<std::array<double, 2>>>>& far_cells() {
  static const std::vector<std::pair<std::string, std::vector<std::array<double, 2>>>> c = {
      {"islandDesert", {{4, 5}}},
      {"allMountains", {{-5, 0}, {5, 1}}},
  };
  return c;
}

// The sites of a world's lattice cells round the origin (and its far cells) and their records
// (worldLines).
void world_lines(rec::Out& out, const std::string& key, const World& w, double span, rec::Samples& r) {
  const SiteLayer& L = *w.sites;
  out << (Line() << "world" << key << L.cell << L.n << span);
  std::vector<std::array<double, 2>> cells;
  for (double b = -span; b <= span; b += 1)
    for (double a = -span; a <= span; a += 1) cells.push_back({a, b});
  for (const auto& fc : far_cells())
    if (fc.first == key) cells.insert(cells.end(), fc.second.begin(), fc.second.end());
  std::vector<std::shared_ptr<const Site>> found;
  for (const auto& c : cells) {
    std::shared_ptr<const Site> s = L.site_at(c[0], c[1]);
    if (!s) {
      out << (Line() << "none" << c[0] << c[1]);
      continue;
    }
    site_lines(out, *s);
    found.push_back(std::move(s));
  }
  for (const std::shared_ptr<const Site>& sp : found) {
    const Site& s = *sp;
    // the surface envelopes (the cell plan's)
    for (const SiteSurface& e : s.def->surface(w, s)) out << (Line() << surface_line(e));
    out << (Line() << "cell" << s.id << ids(L.sites_in_cell(s.cell.i, s.cell.j)));
    // the ground over its pads and features
    for (const auto& p : probes(s, r)) {
      const double nat = s.pad_z + js::round((r() - 0.5) * 160);
      out << (Line() << ground_line(L, p[0], p[1], nat));
    }
    // the structure and its complex
    out << (Line() << structure_line(w, s));
    // its link port, toward nowhere and toward a point
    const double tx = js::round(s.center.x + (r() - 0.5) * 80000);
    const double ty = js::round(s.center.y + (r() - 0.5) * 80000);
    const Point2 toward{tx, ty};
    out << (Line() << port_line(w, s, nullptr));
    out << (Line() << port_line(w, s, &toward));
    // the site source's z ranges round it, and its chunks
    const Rect sb = s.plan->bounds ? *s.plan->bounds : s.blend;
    const double corners[5][2] = {{sb.x0, sb.y0}, {sb.x1, sb.y0}, {sb.x0, sb.y1}, {sb.x1, sb.y1}, {js::round(s.center.x), js::round(s.center.y)}};
    for (const auto& q : corners) {
      const Rect rect{q[0] - 40, q[1] - 40, q[0] + 40, q[1] + 40};
      double z0 = 0, z1 = 0;
      Line zl;
      zl << "zc" << s.id;
      rect_fields(zl, rect);
      if (site_source_z_range(w, rect, &z0, &z1))
        zl << std::vector<double>{z0, z1};
      else
        zl << rec::kUndef;
      out << zl;
    }
    const std::vector<std::array<double, 3>> pts = chunk_points(s, *s.structure(w).under);
    for (const int lod : {0, 0, 0, 1, 2, 3, 5}) {
      const auto& p = pts[static_cast<size_t>(std::floor(r() * static_cast<double>(pts.size())))];
      const double e = static_cast<double>(32 << lod);
      const double jx = std::floor((r() - 0.5) * e * 0.5);
      const double jy = std::floor((r() - 0.5) * e * 0.5);
      const double jz = std::floor((r() - 0.5) * e * 0.5);
      chunk_line(out, w, s, lod, p[0] + jx, p[1] + jy, p[2] + jz, s.pad_z);
    }
    if (s.type == "mountainBase") {
      // the cavern: its floor, its vault, its walls (LODs 0 to 3), under the mountain's rock
      const Cavern& c = *static_cast<const MountainBasePlan&>(*s.plan).cavern;
      const double at[10][4] = {
          {0, c.cx, c.cy, c.zf},
          {0, c.cx, c.cy, c.zf + c.H - 6},
          {0, c.cx + c.A * 0.6, c.cy + c.B * 0.3, c.zf + c.Hw + (c.H - c.Hw) * 0.6},
          {0, c.cx - c.A * 0.55, c.cy - c.B * 0.2, c.zf + c.H * 0.8},
          {0, c.cx - c.A, c.cy, c.zf + 4},
          {0, c.cx, c.cy + c.B, c.zf + 4},
          {0, c.cx + c.A, c.cy, c.zf + 50},
          {1, c.cx, c.cy, c.zf},
          {2, c.cx, c.cy, c.zf + c.H / 2},
          {3, c.cx, c.cy, c.zf},
      };
      for (const auto& q : at) chunk_line(out, w, s, static_cast<int>(q[0]), q[1], q[2], q[3], c.zf + c.H + 320);
    }
  }
}

}  // namespace

namespace {

// What a site is, as text, its parts made in an order of the caller's (rot rotates them): its
// placement and plan, its surface envelopes, the ground at a grid of points over its bounds, its
// structure and complex, its ports, and chunks over it (its cavern's too). Empty: no site.
std::string site_signature(const World& w, double a, double b, size_t rot) {
  const std::shared_ptr<const Site> sp = w.sites->site_at(a, b);
  if (!sp) return "none";
  const Site& s = *sp;
  std::vector<std::function<std::string()>> parts = {
      [&] {
        rec::Out out;
        site_lines(out, s);
        return out.text();
      },
      [&] {
        std::string t;
        for (const SiteSurface& e : s.def->surface(w, s)) t += surface_line(e) + "\n";
        return t;
      },
      [&] {
        std::string t;
        const Rect q = s.plan->bounds ? *s.plan->bounds : s.blend;
        for (double j = 0; j <= 6; j += 1)
          for (double i = 0; i <= 6; i += 1)
            t += ground_line(*w.sites, js::round(q.x0 + (q.x1 - q.x0) * i / 6), js::round(q.y0 + (q.y1 - q.y0) * j / 6), s.pad_z + i * 7 - j * 11) + "\n";
        if (s.type == "mountainBase")
          for (const SitePathPoint& p : static_cast<const MountainPlaced&>(*s.placed).road[0].path) t += ground_line(*w.sites, p.x, p.y, p.z + 9) + "\n";
        return t;
      },
      [&] { return structure_line(w, s); },
      [&] {
        const Point2 toward{s.center.x + 30000, s.center.y - 20000};
        return port_line(w, s, nullptr) + "\n" + port_line(w, s, &toward);
      },
      [&] {
        rec::Out out;
        const Complex& u = *s.structure(w).under;
        const ComplexRoom& q = u.levels[0].rooms[0];
        chunk_line(out, w, s, 0, s.center.x, s.center.y, s.pad_z, s.pad_z);
        chunk_line(out, w, s, 0, q.x0 + 4, q.y0 + 4, u.levels[0].zf + 4, s.pad_z);
        chunk_line(out, w, s, 2, s.center.x, s.center.y, s.pad_z - 64, s.pad_z);
        if (s.type == "mountainBase") {
          const Cavern& c = *static_cast<const MountainBasePlan&>(*s.plan).cavern;
          chunk_line(out, w, s, 0, c.cx + c.A * 0.4, c.cy, c.zf + c.H - 10, c.zf + c.H + 320);
          chunk_line(out, w, s, 1, c.cx, c.cy - c.B * 0.5, c.zf, c.zf + c.H + 320);
        }
        return out.text();
      },
  };
  std::vector<std::string> got(parts.size());
  for (size_t k = 0; k < parts.size(); ++k) {
    const size_t q = (k + rot) % parts.size();
    got[q] = parts[q]();
  }
  std::string t;
  for (const std::string& g : got) t += g + "\n";
  return t;
}

}  // namespace

TEST_CASE("city sitekinds: the kinds' sites are the same on any thread, in any order") {
  // planetNorth: military bases, research complexes and strongholds round the origin (stage
  // sitekinds), and a cell without a site
  const Value overrides = preset_config("planetNorth", "");
  const std::vector<std::array<double, 2>> cells = {{-1, -3}, {0, -2}, {2, -2}, {1, 1}, {0, 0}, {1, 2}, {-2, 0}};
  // one thread, in order
  const std::shared_ptr<World> w1 = create_world(overrides);
  std::vector<std::string> want;
  for (const auto& c : cells) want.push_back(site_signature(*w1, c[0], c[1], 0));
  int kinds = 0;
  for (const auto& c : cells) {
    const auto s = w1->sites->site_at(c[0], c[1]);
    if (s) kinds |= s->type == "militaryBase" ? 1 : s->type == "researchComplex" ? 2 : 4;
  }
  CHECK(kinds == 7);
  // four threads on a fresh world, each in an order of its own (cells and parts)
  const std::shared_ptr<World> w2 = create_world(overrides);
  std::vector<std::vector<std::string>> got(4, std::vector<std::string>(cells.size()));
  std::vector<std::thread> threads;
  for (size_t t = 0; t < 4; ++t)
    threads.emplace_back([&, t] {
      for (size_t k = 0; k < cells.size(); ++k) {
        const size_t q = t % 2 ? cells.size() - 1 - (k + t) % cells.size() : (k * 3 + t) % cells.size();
        got[t][q] = site_signature(*w2, cells[q][0], cells[q][1], t + k);
      }
    });
  for (std::thread& th : threads) th.join();
  for (size_t t = 0; t < 4; ++t) CHECK(got[t] == want);
}

TEST_CASE("city sitekinds: the site kinds and the site kit conform to the reference (stage sitekinds)") {
  rec::Samples r(53);
  rec::Out out;
  kit_lines(out, r);
  for (const test::WorldCase& c : test::all_worlds()) world_lines(out, c.key, *create_world(c.overrides), 3, r);
  for (const test::ExtraWorld& e : parked_worlds()) {
    Value v;
    REQUIRE(Value::parse_json(e.json, &v));
    world_lines(out, e.key, *create_world(v), 1, r);
  }
  // a World of World.js with the kinds: no highways or water to keep sites off, no is_wet (a
  // stronghold's road runs on)
  {
    Value v;
    REQUIRE(Value::parse_json(R"({"seed":4,"terrain":{"mountainBelt":[0,0.01]}})", &v));
    World w(v);
    w.sites = std::make_shared<SiteLayer>(w);
    world_lines(out, "bareMountains", w, 1, r);
  }
  CHECK(rec::record("sitekinds", out.text()) == rec::recorded_digest("sitekinds"));
}
