// svx_city tests — site kinds for the stages of the site layer and the site links
// (tools/procgen_ref/lib/sitekinds.mjs is the Node twin). The reference's kinds need the cell
// network, the archetypes and the kit's surface structures, which come later; these stand in for
// them with the same kinds of placement and complexes of the same shapes: testBase (a level pad,
// one sector from a bunker, bounds beyond the blend rect, a chequered ground, port: the deepest
// level's anteroom), testCampus (four sectors and a tram; port: the nearest station), testHold
// (a custom placement with a round apron, a ramp, the site's pad and a road path; a ground by
// pad; two sectors, a tram and a custom vault), testRuin (a few walls, neither ground nor port).
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "complexes.hpp"
#include "core/hash.hpp"
#include "core/math.hpp"
#include "sites/complex.hpp"
#include "sites/kit.hpp"
#include "terrain/terrain.hpp"
#include "voxel/chunk.hpp"
#include "voxel/materials.hpp"
#include "world/sites.hpp"

namespace svx::city::test {

// The station of a complex's tram nearest a point.
inline std::optional<SitePort> nearest_station(const Complex& u, const Point2* toward) {
  if (!u.tram || u.tram->stations.empty()) return std::nullopt;
  std::optional<SitePort> best;
  for (const TramStation& st : u.tram->stations) {
    const double x = js::round((st.hall.x0 + st.hall.x1) / 2);
    const double y = js::round((st.hall.y0 + st.hall.y1) / 2);
    const double d = toward ? js::hypot(toward->x - x, toward->y - y) : 0;
    if (!best || d < best->d) best = SitePort{x, y, u.tram->z, d};
  }
  return best;
}

inline Rng site_rng(double seed, int32_t mask = 0) { return Rng(static_cast<double>(js::to_int32(seed) ^ mask)); }

// ------------------------------------------------------------ testBase

struct BasePlan : SitePlan {
  Rect bunker{};
  double levels = 0;
};

inline SiteStructure base_structure(const World&, const Site& site) {
  Rng rng = site_rng(site.seed, 0x77);
  const BasePlan& p = static_cast<const BasePlan&>(*site.plan);
  BoxLists lists;
  const Rect& r = site.rect;
  const double z = site.pad_z;
  box(lists.details, r.x0, r.y0, z + 1, r.x1, r.y0, z + 22, MAT::CHAINLINK);
  box(lists.details, r.x1, r.y1, z + 1, r.x0, r.y1, z + 22, MAT::CHAINLINK);
  const Rect& bk = p.bunker;
  box(lists.shells, bk.x0, bk.y0, z + 1, bk.x1, bk.y1, z + 40, MAT::CONCRETE_DARK);
  box(lists.carves, bk.x0 + 4, bk.y0 + 4, z + 2, bk.x1 - 4, bk.y1 - 4, z + 36, 0);
  ComplexSpec spec;
  ComplexSectorSpec s;
  s.bounds = {r.x0 + vx(8), r.y0 + vx(8), r.x1 - vx(8), r.y1 - vx(8)};
  s.z0 = z + 1 - vx(14);
  s.levels = p.levels;
  s.theme = "military";
  s.rooms = std::array<double, 2>{9, 14};
  spec.sectors.push_back(s);
  ComplexEntrySpec e;
  e.rect = complex_shaft_rect(js::round((bk.x0 + bk.x1) / 2), bk.y0 + 12);
  e.z_top = z + 1;
  e.sector = 0;
  e.dir = -1;
  e.open_top = true;
  spec.entries.push_back(e);
  auto under = std::make_shared<Complex>(plan_complex(rng, spec));
  emit_complex(lists, *under, rng);
  SiteStructure st = finish_structure(std::move(lists));
  st.under = std::move(under);
  return st;
}

inline SiteDef test_base_kind() {
  SiteDef d;
  d.id = "testBase";
  d.label = "Test base";
  d.frequency = 0.4;
  d.min_u = 0;
  d.max_u = 0.04;
  d.size = {230, 180};
  d.margin = 45;
  d.max_relief = 30;
  d.plan = [](const World&, const Site& site) -> std::shared_ptr<const SitePlan> {
    Rng rng(site.seed);
    const Rect& r = site.rect;
    const double W = r.x1 - r.x0;
    const double H = r.y1 - r.y0;
    auto p = std::make_shared<BasePlan>();
    p->bunker = {js::round(r.x0 + W * 0.72), js::round(r.y0 + H * 0.05), js::round(r.x0 + W * 0.98), js::round(r.y0 + H * 0.32)};
    p->levels = rng.int_(2, 3);
    const Rect& b = site.blend;
    p->bounds = Rect{b.x0 - vx(30), b.y0, b.x1, b.y1 + vx(20)};
    return p;
  };
  d.structure = base_structure;
  d.ground = [](const Site&, double x, double y, SiteGround& out) {
    if (!out.inside) return;
    out.mat = (js::to_int32(static_cast<double>(js::sar(x, 5)) + js::sar(y, 5)) & 1) ? MAT::CONCRETE : MAT::CONCRETE_LIGHT;
    out.sub = MAT::CONCRETE;
  };
  d.port = [](const World& w, const Site& site, const Point2*) -> std::optional<SitePort> {
    const Complex& u = *site.structure(w).under;
    const ComplexLevel& lv = u.levels.back();
    const ComplexRoom& q = lv.rooms[0];
    return SitePort{js::round((q.x0 + q.x1) / 2), js::round((q.y0 + q.y1) / 2), lv.zf, js::kNaN};
  };
  return d;
}

// ------------------------------------------------------------ testCampus

struct CampusPlan : SitePlan {
  std::vector<Rect> portals;
  std::vector<std::string> themes;
  std::array<double, 4> levels{};
};

inline SiteStructure campus_structure(const World&, const Site& site) {
  Rng rng = site_rng(site.seed, 0x77);
  const CampusPlan& p = static_cast<const CampusPlan&>(*site.plan);
  BoxLists lists;
  const Rect& r = site.rect;
  const double z = site.pad_z;
  for (const Rect& q : p.portals) {
    box(lists.shells, q.x0, q.y0, z + 1, q.x1, q.y1, z + 40, MAT::CONCRETE_DARK);
    box(lists.carves, q.x0 + 4, q.y0 + 4, z + 2, q.x1 - 4, q.y1 - 4, z + 36, 0);
  }
  const double pad = vx(10);
  const double mx = js::round((r.x0 + r.x1) / 2);
  const double my = js::round((r.y0 + r.y1) / 2);
  const std::vector<Rect> quads = {
      {r.x0 + pad, r.y0 + pad, mx - pad / 2, my - pad / 2},
      {mx + pad / 2, r.y0 + pad, r.x1 - pad, my - pad / 2},
      {r.x0 + pad, my + pad / 2, mx - pad / 2, r.y1 - pad},
      {mx + pad / 2, my + pad / 2, r.x1 - pad, r.y1 - pad},
  };
  const double top = z + 1;
  ComplexSpec spec;
  for (size_t k = 0; k < quads.size(); ++k) {
    ComplexSectorSpec s;
    s.bounds = quads[k];
    s.z0 = top - vx(16) - static_cast<double>(k) * vx(9);
    s.levels = p.levels[k];
    s.theme = p.themes[k];
    s.rooms = std::array<double, 2>{8, 12};
    spec.sectors.push_back(s);
  }
  double deepest = js::kInf;
  for (const ComplexSectorSpec& s : spec.sectors) deepest = js::min(deepest, s.z0 - (s.levels - 1) * kLevelGap);
  std::vector<double> seen;
  for (const Rect& q : p.portals) {
    const double cx = js::round((q.x0 + q.x1) / 2);
    double sector = -1;
    for (size_t k = 0; k < quads.size(); ++k)
      if (cx >= quads[k].x0 && cx <= quads[k].x1 && q.y0 >= quads[k].y0 && q.y0 <= quads[k].y1) {
        sector = static_cast<double>(k);
        break;
      }
    if (sector < 0) sector = 0;
    bool dup = false;
    for (const double s : seen)
      if (s == sector) dup = true;
    if (dup) continue;
    seen.push_back(sector);
    ComplexEntrySpec e;
    e.rect = complex_shaft_rect(cx, q.y0 + 12);
    e.z_top = top;
    e.sector = sector;
    e.dir = -1;
    e.open_top = true;
    spec.entries.push_back(e);
  }
  spec.tram_z = deepest - kLevelGap;
  auto under = std::make_shared<Complex>(plan_complex(rng, spec));
  emit_complex(lists, *under, rng);
  SiteStructure st = finish_structure(std::move(lists));
  st.under = std::move(under);
  return st;
}

inline SiteDef test_campus_kind() {
  SiteDef d;
  d.id = "testCampus";
  d.label = "Test campus";
  d.frequency = 0.2;
  d.min_u = 0;
  d.max_u = 0.05;
  d.size = {340, 280};
  d.margin = 50;
  d.max_relief = 40;
  d.plan = [](const World&, const Site& site) -> std::shared_ptr<const SitePlan> {
    Rng rng(site.seed);
    const Rect& r = site.rect;
    const double iw = r.x1 - r.x0;
    const double ih = r.y1 - r.y0;
    auto p = std::make_shared<CampusPlan>();
    const double at[2][2] = {{0.18, 0.38}, {0.66, 0.4}};
    for (const auto& f : at) {
      const double x = js::round(r.x0 + iw * f[0]);
      const double y = js::round(r.y0 + ih * f[1]);
      p->portals.push_back({x, y, x + vx(12), y + vx(15)});
    }
    p->themes = rng.shuffle(std::vector<std::string>{"lab", "containment", "power", "barracks"});
    for (double& l : p->levels) l = rng.int_(2, 3);
    return p;
  };
  d.structure = campus_structure;
  d.port = [](const World& w, const Site& site, const Point2* toward) { return nearest_station(*site.structure(w).under, toward); };
  return d;
}

// ------------------------------------------------------------ testHold

constexpr double kTestRoadHalf = 28;  // vx(3.5)

struct HoldPlaced : SitePlaced {
  double side = 1;
};
struct HoldPlan : SitePlan {
  std::array<double, 2> levels{};
};

inline std::shared_ptr<const SitePlaced> hold_place(const World& w, const SiteCandidate& cand) {
  const Rect& rect = cand.rect;
  const double seed = cand.seed;
  const double cx = js::round((rect.x0 + rect.x1) / 2);
  const double cy = js::round((rect.y0 + rect.y1) / 2);
  const Terrain& T = *w.terrain;
  const TerrainSample tc = T.sample(cx, cy);
  if (tc.mountain < 0.2 || tc.u > 0.01) return nullptr;
  const double side = hash_float(seed, 1) < 0.5 ? 1 : -1;
  const double ax0 = side > 0 ? rect.x1 + vx(10) : rect.x0 - vx(54);
  const Rect apron{ax0, cy - vx(28), ax0 + vx(44), cy + vx(28)};
  const double zf = js::round(T.sample((apron.x0 + apron.x1) / 2, (apron.y0 + apron.y1) / 2).h);
  const double zr = js::round(tc.h);
  const double rx0 = side > 0 ? rect.x1 - vx(20) : apron.x1 - vx(10);
  const Rect ramp{rx0, cy - vx(4), rx0 + vx(30), cy + vx(4)};
  std::vector<SitePathPoint> pts;
  double x = side > 0 ? apron.x1 : apron.x0;
  double y = cy;
  for (double k = 0; k < 14; k += 1) {
    pts.push_back({x, y, js::round(T.sample(x, y).h)});
    x += side * vx(8);
    y += js::round((hash_float(seed, 10 + k) - 0.5) * vx(8));
  }
  double x0 = js::kInf, y0 = js::kInf, x1 = -js::kInf, y1 = -js::kInf;
  for (const SitePathPoint& q : pts) {
    x0 = js::min(x0, q.x);
    y0 = js::min(y0, q.y);
    x1 = js::max(x1, q.x);
    y1 = js::max(y1, q.y);
  }
  SitePad road;
  road.rect = {x0 - kTestRoadHalf, y0 - kTestRoadHalf, x1 + kTestRoadHalf, y1 + kTestRoadHalf};
  road.z = zf;
  road.path = std::move(pts);
  road.half = kTestRoadHalf;
  road.margin = vx(9);
  road.round = true;
  road.road = true;
  auto out = std::make_shared<HoldPlaced>();
  out->pad_z = zf;
  out->footprint = {js::min(rect.x0, apron.x0, ramp.x0, road.rect.x0), js::min(rect.y0, apron.y0, ramp.y0, road.rect.y0),
                    js::max(rect.x1, apron.x1, ramp.x1, road.rect.x1), js::max(rect.y1, apron.y1, ramp.y1, road.rect.y1)};
  SitePad pa;
  pa.rect = apron;
  pa.z = zf;
  pa.margin = vx(22);
  pa.round = true;
  SitePad pr;
  pr.rect = ramp;
  pr.z = zr;
  pr.margin = vx(10);
  pr.ramp = SiteRamp{'x', ramp.x0, ramp.x1, side > 0 ? zr : zf, side > 0 ? zf : zr};
  SitePad ps;
  ps.rect = rect;
  ps.z = zr;
  ps.margin = cand.margin;
  out->pads = {pa, pr, ps, road};
  out->side = side;
  return out;
}

// A vault over the site's rect at its pad level: a floor, the air above, a lining at its edge.
inline SiteVolume hold_volume(const Site& site) {
  const Rect r = site.rect;
  const double z = site.pad_z;
  const double H = vx(10);
  SiteVolume v;
  v.bb = {r.x0, r.y0, z - 1, r.x1, r.y1, z + H + 1};
  v.rasterize = [r, z, H](ChunkBuffer& chunk) {
    if (chunk.lod > 2) return;
    uint16_t* data = chunk.data.data();
    for (int j = 0; j < kP; ++j)
      for (int i = 0; i < kP; ++i) {
        const double x = chunk.wx(i);
        const double y = chunk.wy(j);
        if (x < r.x0 || x > r.x1 || y < r.y0 || y > r.y1) continue;
        const bool edge = x < r.x0 + 16 || x > r.x1 - 16 || y < r.y0 + 16 || y > r.y1 - 16;
        for (int k = 0; k < kP; ++k) {
          const double zz = chunk.wz(k);
          const int idx = i + j * kP + k * kP2;
          if (zz == z)
            data[idx] = MAT::FLOOR_CONCRETE;
          else if (zz > z && zz <= z + H) {
            if (!edge)
              data[idx] = 0;
            else if (data[idx] != 0)
              data[idx] = MAT::CONCRETE;
          }
        }
      }
  };
  return v;
}

inline SiteStructure hold_structure(const World&, const Site& site) {
  Rng rng = site_rng(site.seed, 0x77);
  const HoldPlan& p = static_cast<const HoldPlan&>(*site.plan);
  BoxLists lists;
  lists.custom.push_back(hold_volume(site));
  const double cx = js::round(site.center.x);
  const double cy = js::round(site.center.y);
  const double half_l = vx(90);
  const double half_w = vx(70);
  const std::vector<Rect> quads = {{cx + vx(6), cy - half_w, cx + half_l, cy + half_w}, {cx - half_l, cy - half_w, cx - vx(6), cy + half_w}};
  const double top = site.pad_z;
  ComplexSpec spec;
  for (size_t k = 0; k < quads.size(); ++k) {
    ComplexSectorSpec s;
    s.bounds = quads[k];
    s.z0 = top - vx(18) - static_cast<double>(k) * vx(8);
    s.levels = p.levels[k];
    s.theme = k == 0 ? "military" : "power";
    s.rooms = std::array<double, 2>{8, 12};
    spec.sectors.push_back(s);
  }
  double deepest = js::kInf;
  for (const ComplexSectorSpec& s : spec.sectors) deepest = js::min(deepest, s.z0 - (s.levels - 1) * vx(15));
  const double px = cx - vx(30);
  const double py = cy + vx(20);
  double sector = -1;
  for (size_t k = 0; k < quads.size(); ++k)
    if (px >= quads[k].x0 && px <= quads[k].x1 && py >= quads[k].y0 && py <= quads[k].y1) {
      sector = static_cast<double>(k);
      break;
    }
  if (sector < 0) sector = 0;
  ComplexEntrySpec e;
  e.rect = complex_shaft_rect(px, py);
  e.z_top = top + 1;
  e.sector = sector;
  e.dir = -1;
  e.open_top = true;
  spec.entries.push_back(e);
  spec.tram_z = deepest - vx(15);
  auto under = std::make_shared<Complex>(plan_complex(rng, spec));
  emit_complex(lists, *under, rng);
  SiteStructure st = finish_structure(std::move(lists));
  st.under = std::move(under);
  return st;
}

inline SiteDef test_hold_kind() {
  SiteDef d;
  d.id = "testHold";
  d.label = "Test hold";
  d.frequency = 0.25;
  d.min_u = 0;
  d.max_u = 0.02;
  d.size = {190, 190};
  d.margin = 30;
  d.place = hold_place;
  d.plan = [](const World&, const Site& site) -> std::shared_ptr<const SitePlan> {
    Rng rng(site.seed);
    const Rect& fp = site.placed->footprint;
    auto p = std::make_shared<HoldPlan>();
    p->levels[0] = rng.int_(2, 3);
    p->levels[1] = rng.int_(2, 3);
    p->bounds = Rect{fp.x0 - vx(8), fp.y0 - vx(8), fp.x1 + vx(8), fp.y1 + vx(8)};
    return p;
  };
  d.structure = hold_structure;
  d.ground = [](const Site&, double, double, SiteGround& out) {
    const SitePad* pad = out.pad;
    if (pad && pad->road) {
      if (out.inside) {
        out.mat = out.path_c < 0.7 && std::fmod(out.path_s, 48) < 24 ? MAT::LINE_YELLOW : out.path_c > kTestRoadHalf - 1.2 ? MAT::LINE_WHITE : MAT::ASPHALT_WORN;
        out.sub = MAT::GRAVEL;
      } else {
        out.mat = out.natural - out.z > vx(1) ? MAT::ROCK : MAT::GRAVEL;
        out.sub = out.mat;
      }
      return;
    }
    if (pad && pad->ramp) {
      out.mat = MAT::PAVER_GRAY;
      out.sub = MAT::GRAVEL;
      return;
    }
    if (std::fabs(out.z - out.natural) > 2) {
      out.mat = out.natural - out.z > vx(1) ? MAT::ROCK : MAT::GRAVEL;
      out.sub = out.mat;
    }
  };
  d.port = [](const World& w, const Site& site, const Point2* toward) { return nearest_station(*site.structure(w).under, toward); };
  return d;
}

// ------------------------------------------------------------ testRuin

inline SiteDef test_ruin_kind() {
  SiteDef d;
  d.id = "testRuin";
  d.label = "Test ruin";
  d.frequency = 0.1;
  d.min_u = 0;
  d.max_u = 0.3;
  d.size = {100, 80};
  d.plan = [](const World&, const Site&) -> std::shared_ptr<const SitePlan> { return std::make_shared<SitePlan>(); };
  d.structure = [](const World&, const Site& site) {
    BoxLists lists;
    const Rect& r = site.rect;
    const double z = site.pad_z;
    box(lists.details, r.x0, r.y0, z + 1, r.x0 + 3, r.y1, z + 30, MAT::STONE);
    box(lists.details, r.x1, r.y0, z + 1, r.x1 - 3, r.y0 + 40, z + 18, MAT::STONE);
    box(lists.carves, r.x0, r.y0 + 20, z + 2, r.x0 + 3, r.y0 + 30, z + 20, 0);
    return finish_structure(std::move(lists));
  };
  return d;
}

// The kinds in SITES' order (lib/sitekinds.mjs SITE_KINDS).
inline const std::vector<SiteDef>& test_site_kinds() {
  static const std::vector<SiteDef> kinds = {test_base_kind(), test_campus_kind(), test_hold_kind(), test_ruin_kind()};
  return kinds;
}
inline std::vector<const SiteDef*> test_site_defs() {
  std::vector<const SiteDef*> out;
  for (const SiteDef& d : test_site_kinds()) out.push_back(&d);
  return out;
}

}  // namespace svx::city::test
