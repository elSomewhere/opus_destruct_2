// svx_city tests — shared inputs of the building shell stages (massing, wings, garageramps,
// skybridges, grading): the C++ twin of tools/procgen_ref/lib/shells.mjs. The worlds a shell is
// drawn in (every season, explicit snow covers), scripted envelopes of every archetype, the
// chunks round a box and their digests.
#pragma once

#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "building_records.hpp"
#include "city/blockPoly.hpp"
#include "city/cellNetwork.hpp"
#include "core/placement.hpp"
#include "network/arterials.hpp"
#include "network/road.hpp"
#include "network/roadClasses.hpp"
#include "network/roadLevel.hpp"
#include "network/roadView.hpp"
#include "terrain/terrain.hpp"
#include "voxel/chunk.hpp"
#include "world/World.hpp"

namespace svx::city::test {

// lib/shells.mjs SHELL_WORLDS
inline const std::vector<const char*>& shell_worlds() {
  static const std::vector<const char*> w = {
      R"({"seed":1})",
      R"({"seed":2,"world":{"season":"winter"}})",
      R"({"seed":3,"world":{"season":"spring","climate":{"temperature":0.2}}})",
      R"({"seed":4,"world":{"season":"autumn","climate":{"temperature":0.1,"temperatureVar":0.02}}})",
      R"({"seed":5,"world":{"climate":{"snowCover":0.6}}})",
      R"({"seed":6,"world":{"season":"winter","climate":{"snowCover":1.4}}})",
      R"({"seed":7,"world":{"season":"winter","climate":{"temperature":0.05}}})",
  };
  return w;
}

// World k of shell_worlds() (a World of World.js, made once).
inline const World& shell_world(size_t k) {
  static std::vector<std::unique_ptr<World>> ws(shell_worlds().size());
  if (!ws[k]) {
    Value v;
    if (!Value::parse_json(shell_worlds()[k], &v)) SVX_FAIL("shells: a world's JSON does not parse");
    ws[k] = std::make_unique<World>(v);
  }
  return *ws[k];
}

// a flavor's church dome materials (and a list with a name the palette lacks)
inline const std::vector<std::vector<std::string>>& shell_domes() {
  static const std::vector<std::vector<std::string>> d = {{"GOLD", "DOME_GREEN", "DOME_BLUE"}, {"DOME_SHINGLE", "DOME_GREEN"}, {"NOPE"}};
  return d;
}

struct ShellCase {
  Lot lot;
  std::optional<Envelope> env;
};

// shellEnvelope(r, a, style, d, w, k): an envelope of archetype a in style (null: the district's
// pick) on a scripted lot of a size it fits.
inline ShellCase shell_envelope(rec::Samples& r, const Archetype& a, const std::string* style, const District& d, const World& w, double k) {
  double U = 0, V = 0;
  for (int t = 0; t < 40; ++t) {
    U = 40 + std::floor(r() * 560);
    V = 40 + std::floor(r() * 560);
    if (a.fits(U, V)) break;
  }
  ShellCase out;
  out.lot = scripted_lot(r, U, V, k, d.id);
  EnvelopeExtra extra;
  extra.u = r();
  extra.core = r() * 1.2;
  extra.ground_z = std::floor(r() * 3000) - 200;
  extra.config = &w.config;
  if (r() < 0.2) extra.chapel = true;
  if (r() < 0.5) extra.dome = shell_domes()[static_cast<size_t>(std::floor(r() * static_cast<double>(shell_domes().size())))];
  if (r() < 0.4) extra.pitched_civic = 0.6;
  Rng rng(std::floor(r() * 4294967296.0));
  out.env = style ? plan_building_envelope_as(out.lot, a.id, *style, d, rng, extra) : plan_building_envelope(out.lot, d, rng, extra);
  return out;
}

// chunkDigest: FNV-1a over a chunk's voxels, and the count of the solid ones.
inline void chunk_digest(rec::Line& l, const ChunkBuffer& c) {
  uint32_t h = 2166136261u;
  double n = 0;
  for (const uint16_t v : c.data) {
    h = (h ^ v) * 16777619u;
    if (v) n += 1;
  }
  l << h << n;
}

// chunksAt(lods, pts): the chunks holding the points, each once per LOD, in the points' order.
using ChunkAt = std::array<double, 4>;  // lod, cx, cy, cz
inline std::vector<ChunkAt> chunks_at(const std::vector<int>& lods, const std::vector<std::array<double, 3>>& pts) {
  std::vector<ChunkAt> out;
  for (int lod : lods) {
    const double E = static_cast<double>(32 << lod);
    std::vector<ChunkAt> seen;
    for (const auto& p : pts) {
      const ChunkAt c{static_cast<double>(lod), std::floor(p[0] / E), std::floor(p[1] / E), std::floor(p[2] / E)};
      bool dup = false;
      for (const ChunkAt& s : seen) dup = dup || s == c;
      if (dup) continue;
      seen.push_back(c);
      out.push_back(c);
    }
  }
  return out;
}

// ---- sites: lots on scripted streets (lib/shells.mjs wingSite)

inline const std::vector<std::string>& site_classes() {
  static const std::vector<std::string> c = {"local", "collector", "arterial", "village", "pedestrian", "lane", "alley"};
  return c;
}

struct WingSite {
  Lot lot;
  std::optional<Block> block;  // (none: null)
  std::optional<Envelope> env;
  std::string cell_id;
  std::shared_ptr<const RoadView> view;
};

// The roads a test's World serves by cell (World::cell_roads).
using CellRoads = std::map<std::pair<double, double>, RoadList>;
inline void serve_cell_roads(World& w, const CellRoads& roads) {
  w.cell_roads = [&roads](double i, double j) {
    auto it = roads.find({i, j});
    return it == roads.end() ? RoadList{} : it->second;
  };
}

// wingSite(r, w, cellRoads, i, j, k, DS, styles, specs)
inline WingSite wing_site(rec::Samples& r, const World& w, CellRoads& cell_roads, double i, double j, double k, const std::vector<District>& DS,
                          const std::vector<std::string>& styles, const RoadSpecs& specs) {
  static const char* const kWinged[] = {"walkup", "midrise", "office", "rowhouse", "townhouse"};
  static const char* const kUnwinged[] = {"house", "panelSlab", "garage", "school"};
  static const double kGaps[] = {0, 0, 1, 3};
  const std::vector<std::string>& CLASSES = site_classes();
  WingSite site;
  const Rect rc = w.arterials->cell_rect(i, j);
  const double cx = js::round((rc.x0 + rc.x1) / 2);
  const double cy = js::round((rc.y0 + rc.y1) / 2);
  const std::string cell_id = js::cat("C", i, "_", j);
  site.cell_id = cell_id;
  const bool winged = r() < 0.85;
  const std::string aid = winged ? kWinged[static_cast<size_t>(std::floor(r() * 5))] : kUnwinged[static_cast<size_t>(std::floor(r() * 4))];
  const Archetype& a = archetype_registry().get(aid);
  double U = 0, V = 0;
  for (int t = 0; t < 60; ++t) {
    U = 48 + std::floor(r() * 300);
    V = 64 + std::floor(r() * 300);
    if (a.fits(U, V)) break;
  }
  const bool turned = r() < 0.25;
  const double fi = std::floor(r() * 4);
  const int yaw = static_cast<int>(std::floor(r() * 132));
  Lot& lot = site.lot;
  lot.id = js::cat(cell_id, "/b", k, "/l0");
  lot.corner = false;
  lot.micro = false;
  lot.block = js::cat(cell_id, "/b", k);
  if (turned) {
    lot.front = nominal_front(yaw);
    Turn t;
    t.yaw = yaw;
    t.origin = {cx - 100, cy - 100};
    t.ou = 0;
    t.ov = 0;
    t.U = U;
    t.V = V;
    lot.turn = t;
    lot.rect = lot_frame_of(lot.turn, Rect{}, lot.front).R;
  } else {
    lot.front = "NESW"[static_cast<int>(fi)];
    const bool ns = lot.front == 'N' || lot.front == 'S';
    const double x0 = cx - std::floor((ns ? U : V) / 2);
    const double y0 = cy - std::floor((ns ? V : U) / 2);
    lot.rect = {x0, y0, x0 + (ns ? U : V) - 1, y0 + (ns ? V : U) - 1};
  }
  const Frame frame = lot_frame_of(lot.turn, lot.rect, lot.front);
  RoadList roads;
  auto street = [&](std::vector<RoadPt> pts, const std::string& cls, bool owned) {
    const RoadSpec& s = *specs.get(cls);
    auto road = std::make_shared<Road>();
    road->id = js::cat(cell_id, "/r", static_cast<double>(roads.size()));
    road->cell = owned ? cell_id : std::string("C999_999");
    road->cls = cls;
    road->pts = std::move(pts);
    road->hc = s.hc;
    road->hr = s.hr;
    road->corner = s.corner;
    road->median = s.median;
    road->parking = s.parking;
    road->lanes = s.lanes;
    road->lane = s.lane;
    road->sidewalk = s.sidewalk;
    road->shoulder = s.shoulder;
    road->ci = i;
    road->cj = j;
    roads.push_back(road);
  };
  // the streets along its sides
  for (const char cs : {'F', 'L', 'R', 'B'}) {
    const double p = r();
    const bool alley = r() < 0.6;
    const size_t ci = static_cast<size_t>(std::floor(r() * static_cast<double>(CLASSES.size())));
    const double gap = kGaps[static_cast<size_t>(std::floor(r() * 4))];
    const double ext = 200 + std::floor(r() * 400);
    const bool owned = r() < 0.85;
    if (!(cs == 'F' ? p < 0.92 : cs == 'B' ? p < 0.3 : p < 0.6)) continue;
    const std::string cls = cs == 'B' && alley ? std::string("alley") : CLASSES[ci];
    const double o = gap + specs.get(cls)->hr;
    std::vector<RoadPt> pts;
    if (turned) {
      auto P = [&](double u, double v) {
        const XY xy = frame.point_to_world(u, v);
        return RoadPt{xy[0], xy[1]};
      };
      if (cs == 'F')
        pts = {P(-ext, -o), P(U + ext, -o)};
      else if (cs == 'B')
        pts = {P(-ext, V + o), P(U + ext, V + o)};
      else if (cs == 'L')
        pts = {P(-o, -ext), P(-o, V + ext)};
      else
        pts = {P(U + o, -ext), P(U + o, V + ext)};
    } else {
      const Rect& R = lot.rect;
      const char ws = frame.world_side(cs);
      if (ws == 'N')
        pts = {{R.x0 - ext, R.y0 - o}, {R.x1 + 1 + ext, R.y0 - o}};
      else if (ws == 'S')
        pts = {{R.x0 - ext, R.y1 + 1 + o}, {R.x1 + 1 + ext, R.y1 + 1 + o}};
      else if (ws == 'W')
        pts = {{R.x0 - o, R.y0 - ext}, {R.x0 - o, R.y1 + 1 + ext}};
      else
        pts = {{R.x1 + 1 + o, R.y0 - ext}, {R.x1 + 1 + o, R.y1 + 1 + ext}};
      if (cls != "alley") lot.frontages.push_back(LotFrontage{ws, cls, false, std::nullopt});
    }
    street(std::move(pts), cls, owned);
  }
  // a slanted street cutting a front corner off (plain lots)
  if (!(r() < 0.5)) {
    Block b;
    b.id = lot.block;
    site.block = std::move(b);
  }
  const double sp = r();
  const double sc = r();
  const double sy = r();
  const double sq = r();
  const double sd = r();
  const double sh = r();
  const std::string scls = CLASSES[static_cast<size_t>(std::floor(r() * 5))];
  const bool sowned = r() < 0.85;
  if (!turned && sp < 0.35) {
    const Rect R = lot.rect;
    const XY wc = frame.to_world(sc < 0.5 ? 0 : frame.U - 1, 0);
    const double qx = wc[0] == R.x0 ? R.x0 : R.x1 + 1;
    const double qy = wc[1] == R.y0 ? R.y0 : R.y1 + 1;
    const Yaw& Y = yaws()[static_cast<size_t>(1 + std::floor(sy * 32) + 33 * std::floor(sq * 4))];
    const double dx = Y.c / Y.r;
    const double dy = Y.s / Y.r;
    double nx = -dy;
    double ny = dx;
    if (nx * ((R.x0 + R.x1 + 1) / 2 - qx) + ny * ((R.y0 + R.y1 + 1) / 2 - qy) < 0) {
      nx = -nx;
      ny = -ny;
    }
    if (sh < 0.1) nx += 1e-4;
    const double hr = specs.get(scls)->hr;
    const double c = nx * qx + ny * qy + vx(2) + std::floor(sd * vx(8));
    BlockCut cut;
    cut.nx = nx;
    cut.ny = ny;
    cut.c = c;
    cut.cls = scls;
    cut.hr = hr;
    cut.id = js::cat(cell_id, "/r", static_cast<double>(roads.size()));
    const std::optional<TrimResult> t = trim_to_cuts(R, {cut});
    if (t) {
      const double t0 = (c - hr - (nx * qx + ny * qy)) / (nx * nx + ny * ny);
      const double px = qx + nx * t0;
      const double py = qy + ny * t0;
      street({{px - dx * 700, py - dy * 700}, {px + dx * 700, py + dy * 700}}, scls, sowned);
      lot.whole = true;
      lot.whole_rect = R;
      lot.rect = t->rect;
      for (const TrimmedSide& tr : t->trimmed) {
        if (!tr.cls || *tr.cls == "alley") continue;
        std::vector<LotFrontage> kept;
        for (const LotFrontage& q : lot.frontages)
          if (q.side != tr.side) kept.push_back(q);
        kept.push_back(LotFrontage{tr.side, *tr.cls, true, tr.id});
        lot.frontages = std::move(kept);
      }
      Block b;
      b.id = lot.block;
      b.cuts = {cut};
      site.block = std::move(b);
    }
  }
  const double wh = r();
  if (!lot.whole && !turned && wh < 0.1) lot.whole = true;
  lot.corner = lot.frontages.size() >= 2;
  const District& d = DS[static_cast<size_t>(std::floor(r() * static_cast<double>(DS.size())))];
  lot.district = d.id;
  const std::string& style = styles[static_cast<size_t>(std::floor(r() * static_cast<double>(styles.size())))];
  cell_roads[{i, j}] = roads;
  site.view = w.road_view(i, j);
  // (cellPlan.js lotGround: the street's level at the lot's frontage point)
  const Rect& R = lot.rect;
  XY fp;
  if (turned)
    fp = lot_frame_of(lot.turn, lot.rect, lot.front).point_to_world(U / 2, -4);
  else if (lot.front == 'N')
    fp = {(R.x0 + R.x1) / 2, R.y0 - 4};
  else if (lot.front == 'S')
    fp = {(R.x0 + R.x1) / 2, R.y1 + 4};
  else if (lot.front == 'W')
    fp = {R.x0 - 4, (R.y0 + R.y1) / 2};
  else
    fp = {R.x1 + 4, (R.y0 + R.y1) / 2};
  const std::optional<RoadLevel> lv = road_level_at(w, *site.view, fp[0], fp[1], 24);
  const double ground_z = lv ? js::round(lv->z) + (lv->sidewalk ? 1 : 0) : js::round(w.terrain->sample(fp[0], fp[1]).h) + 1;
  EnvelopeExtra extra;
  extra.u = r();
  extra.core = r();
  extra.ground_z = ground_z;
  extra.config = &w.config;
  Rng rng(std::floor(r() * 4294967296.0));
  site.env = plan_building_envelope_as(lot, aid, style, d, rng, extra);
  // (scripted, beyond the archetypes: upper floors set back from the sides, or cantilevered over
  // the front)
  const double tm = r();
  std::optional<Envelope>& env = site.env;
  if (env && env->roof.type == "flat" && env->floors >= 4 && env->tiers.size() == 1 && tm < 0.2) {
    const Rect m = env->tiers[0].rects[0];
    const Rect up = tm < 0.1 ? Rect{m.x0 + 12, m.y0, m.x1 - 12, m.y1} : Rect{m.x0, m.y0 - 12, m.x1, m.y1};
    env->tiers = {EnvTier{0, 1, {m}}, EnvTier{2, env->floors - 1, {up}}};
  }
  return site;
}

// lotLine(tag, lot, block)
inline std::string lot_line(const char* tag, const Lot& lot, const Block* block) {
  const std::string turn = lot.turn ? js::cat(lot.turn->yaw, "/", lot.turn->origin.x, "/", lot.turn->origin.y, "/", lot.turn->U, "/", lot.turn->V) : "-";
  std::string fronts;
  for (size_t q = 0; q < lot.frontages.size(); ++q) {
    const LotFrontage& f = lot.frontages[q];
    fronts += js::cat(q ? ";" : "", std::string(1, f.side), f.cls);
    if (f.slant) fronts += "@" + (f.slant_id ? *f.slant_id : std::string("null"));
  }
  if (fronts.empty()) fronts = "-";
  const std::string whole = lot.whole_rect ? js::cat(lot.whole_rect->x0, ",", lot.whole_rect->y0, ",", lot.whole_rect->x1, ",", lot.whole_rect->y1)
                            : lot.whole    ? std::string("true")
                                           : std::string("-");
  std::string cuts = "-";
  if (block && !block->cuts.empty()) {
    cuts.clear();
    for (size_t q = 0; q < block->cuts.size(); ++q) {
      const BlockCut& k = block->cuts[q];
      cuts += js::cat(q ? ";" : "", k.nx, ",", k.ny, ",", k.c, ",", k.cls ? *k.cls : std::string(), ",", k.hr, ",", k.id ? *k.id : std::string());
    }
  } else if (block) {
    cuts = "none";
  }
  return (rec::Line() << tag << lot.id << rect_str(lot.rect) << lot.front << turn << fronts << whole << cuts << lot.corner << lot.district).str();
}

}  // namespace svx::city::test
