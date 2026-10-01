// svx_city tests — the road network's inputs (tools/procgen_ref/lib/roads.mjs is the Node twin):
// scripted road sets drawn from rec::Samples, and the reference's own roads and waters recorded
// by a stage (tools/procgen_ref/data/<stage>.json), served to a World through World::cell_roads
// and World::wet_source while the cell networks and the waters are later stages of the port.
#pragma once

#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/js.hpp"
#include "core/value.hpp"
#include "network/road.hpp"
#include "network/roadView.hpp"
#include "records.hpp"
#include "world/World.hpp"
#include "world/caches.hpp"

namespace svx::city::test {

// ---- scripted road sets (lib/roads.mjs scriptedRoads)

inline const std::vector<std::string>& script_classes() {
  static const std::vector<std::string> c = {"highway", "arterial", "collector", "local", "village", "alley", "lane", "pedestrian", "rural", "path", "bogus"};
  return c;
}

// A scripted road set (index k) in a 1200 x 1200 area round (ox, oy), drawn from r.
inline RoadList scripted_roads(rec::Samples& r, int k, double ox = 0, double oy = 0) {
  constexpr double kPi = 3.141592653589793;
  static const char* const kStrips[4] = {"grass", "pits", "none", nullptr};
  const double n = 4 + std::floor(r() * 28);
  const bool angled = r() < 0.35;
  std::vector<std::shared_ptr<Road>> roads;
  auto undef = [&](double p) { return r() < p; };
  for (int q = 0; q < n; ++q) {
    auto road = std::make_shared<Road>();
    road->cls = script_classes()[static_cast<size_t>(std::floor(r() * static_cast<double>(script_classes().size())))];
    const double hc0 = r() < 0.15 ? 0 : 8 + std::floor(r() * 52);
    const double walk = std::floor(r() * 44) - 4;
    road->id = js::cat("S", k, "/r", q);
    road->cell = js::cat("S", k);
    road->hc = hc0;
    road->hr = hc0 + js::max(0.0, walk);
    const double corner = std::floor(r() * 70);
    if (!undef(0.02)) road->corner = corner;
    const double median = r() < 0.6 ? 0 : 6 + std::floor(r() * 20);
    if (!undef(0.08)) road->median = median;
    const double parking = r() < 0.6 ? 0 : 14 + std::floor(r() * 8);
    if (!undef(0.08)) road->parking = parking;
    const double lanes = 1 + std::floor(r() * 6);
    if (!undef(0.08)) road->lanes = lanes;
    const double lane = 20 + std::floor(r() * 10);
    if (!undef(0.08)) road->lane = lane;
    if (!undef(0.08)) road->sidewalk = walk;
    const double shoulder = r() < 0.6 ? 0 : 4 + std::floor(r() * 12);
    if (!undef(0.08)) road->shoulder = shoulder;
    if (r() < 0.2) road->paving = "cobble";
    const char* strip = kStrips[static_cast<size_t>(std::floor(r() * 4))];
    if (strip) road->strip = strip;
    if (angled && r() < 0.85) {
      const double hi = std::floor(r() * 5) - 2;
      const double hj = std::floor(r() * 5) - 2;
      road->home = std::array<double, 2>{hi, hj};
    }
    // the centre line: from a free point, or from a point in an earlier road's carriageway
    double x, y;
    if (q > 0 && r() < 0.35) {
      const Road& o = *roads[static_cast<size_t>(std::floor(r() * q))];
      const PPoint& p = o.pts.front();
      const PPoint& e = o.pts.back();
      const double t = r();
      const double lat = (r() - 0.5) * 2 * (o.hc + 4);
      const double L = js::or_(js::hypot(e.x - p.x, e.y - p.y), 1);
      x = p.x + (e.x - p.x) * t - ((e.y - p.y) / L) * lat;
      y = p.y + (e.y - p.y) * t + ((e.x - p.x) / L) * lat;
    } else {
      x = ox + r() * 1200;
      y = oy + r() * 1200;
    }
    if (r() < 0.5) {
      x = js::round(x);
      y = js::round(y);
    }
    road->pts.push_back({x, y});
    const double m = 1 + std::floor(r() * 4);
    double a = r() * 2 * kPi;
    for (int s = 0; s < m; ++s) {
      const double kind = r();
      if (kind < 0.12) {
        // a degenerate piece: the same point again
        road->pts.push_back({x, y});
        continue;
      }
      if (kind < 0.5)
        a = std::floor(r() * 4) * (kPi / 2);
      else
        a += (r() - 0.5) * 1.2;
      const double len = 40 + r() * 500;
      x += js::cos(a) * len;
      y += js::sin(a) * len;
      if (kind < 0.5) {
        x = js::round(x);
        y = js::round(y);
      }
      road->pts.push_back({x, y});
    }
    roads.push_back(road);
  }
  return RoadList(roads.begin(), roads.end());
}

// ---- records (lib/roads.mjs segRef, segFields, junctionFields)

inline std::string seg_ref(const RoadSeg* s) { return s ? js::cat(s->road->id, "#", s->idx) : std::string("-"); }

// A number JS may hold undefined (NaN here): "-" for it.
inline void undef_or(rec::Line& l, double v) {
  if (v != v)
    l << rec::kUndef;
  else
    l << v;
}

inline void seg_fields(rec::Line& l, const RoadSeg& s) {
  l << seg_ref(&s) << s.ax << s.ay << s.bx << s.by;
  undef_or(l, s.az);
  undef_or(l, s.bz);
  l << s.len << s.dx << s.dy << s.s0 << s.hc << s.hr << s.sidewalk << s.parking << s.median << s.lanes << s.lane << s.shoulder << *s.cls << s.rank
    << s.first << s.last << std::vector<double>{s.bbox.x0, s.bbox.y0, s.bbox.x1, s.bbox.y1};
}

inline void junction_fields(rec::Line& l, const RoadJunction& j) {
  l << j.s << j.hc << j.hr << *j.cls << j.rank << j.c_ends << j.s_ends << j.cw << j.signal << seg_ref(j.other) << j.t_other;
}

// ---- recorded inputs (lib/roads.mjs recorded / writeInputs)

struct WetKey {
  double x, y, m;
  bool operator==(const WetKey& o) const { return std::memcmp(this, &o, sizeof(WetKey)) == 0; }
};
struct WetKeyHash {
  size_t operator()(const WetKey& k) const {
    uint64_t h = 1469598103934665603ull;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(&k);
    for (size_t i = 0; i < sizeof(WetKey); ++i) h = (h ^ p[i]) * 1099511628211ull;
    return static_cast<size_t>(h);
  }
};

// One world's recorded inputs: the roads of the cells the stage read (one object per road, as a
// World's views share them) and where the waters answered yes.
struct RecordedWorld {
  std::unordered_map<uint64_t, RoadList> cells;
  std::unordered_set<WetKey, WetKeyHash> wet;

  const RoadList& roads(double i, double j) const {
    auto it = cells.find(cell_key(i, j));
    if (it == cells.end()) SVX_FAIL("recorded roads: the stage never read this cell");
    return it->second;
  }
  // (only the questions the reference answered yes are recorded: the others are dry)
  bool is_wet(double x, double y, double m) const { return wet.count(WetKey{x, y, m}) != 0; }
};

inline Value read_json_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) SVX_FAIL("recorded inputs: missing file");
  std::stringstream ss;
  ss << in.rdbuf();
  Value v;
  std::string err;
  if (!Value::parse_json(ss.str(), &v, &err)) SVX_FAIL("recorded inputs: bad JSON");
  return v;
}

// The recorded inputs of a stage: data/<stage>.json, by world key.
inline std::map<std::string, std::shared_ptr<const RecordedWorld>> load_recorded(const std::string& stage) {
  const Value root = read_json_file(std::string(SVX_SOURCE_DIR) + "/tools/procgen_ref/data/" + stage + ".json");
  std::map<std::string, std::shared_ptr<const RecordedWorld>> out;
  for (const Value::Member& wm : root.members()) {
    auto rw = std::make_shared<RecordedWorld>();
    const Value& classes = wm.second["classes"];
    const Value& specs = wm.second["specs"];
    for (const Value& c : wm.second["cells"].items()) {
      const double i = c[size_t{0}].to_number();
      const double j = c[size_t{1}].to_number();
      const std::string& cell = c[size_t{2}].str();
      RoadList roads;
      for (const Value& rv : c[size_t{3}].items()) {
        auto road = std::make_shared<Road>();
        road->id = js::cat(cell, "/r", rv[size_t{0}].to_number());
        road->cell = cell;
        const size_t cls = static_cast<size_t>(rv[size_t{1}].to_number());
        road->cls = classes[cls].str();
        const Value& pts = rv[size_t{2}];
        for (size_t p = 0; p + 1 < pts.size(); p += 2) road->pts.push_back({pts[p].to_number(), pts[p + 1].to_number()});
        const Value& sp = specs[cls];
        road->hc = sp[size_t{0}].to_number();
        road->hr = sp[size_t{1}].to_number();
        road->corner = sp[size_t{2}].to_number();
        road->median = sp[size_t{3}].to_number();
        road->parking = sp[size_t{4}].to_number();
        road->lanes = sp[size_t{5}].to_number();
        road->lane = sp[size_t{6}].to_number();
        road->sidewalk = sp[size_t{7}].to_number();
        road->shoulder = sp[size_t{8}].to_number();
        const Value& ex = rv[size_t{3}];
        if (ex.is_object()) {
          if (ex.has("e")) road->arterial_edge = ex["e"].str();
          if (ex.has("p")) road->paving = ex["p"].str();
          if (ex.has("d")) road->diagonal = RoadDiagonal{ex["d"][size_t{0}].to_number(), ex["d"][size_t{1}].to_number()};
          if (ex.has("u")) road->sub = ex["u"].is_number() ? js::cat(cell, "/s", ex["u"].to_number()) : ex["u"].str();
          if (ex.has("s")) road->strip = ex["s"].str();
          if (ex.has("h")) road->home = std::array<double, 2>{ex["h"][size_t{0}].to_number(), ex["h"][size_t{1}].to_number()};
        }
        roads.push_back(road);
      }
      rw->cells[cell_key(i, j)] = std::move(roads);
    }
    for (const Value& e : wm.second["wet"].items()) rw->wet.insert(WetKey{e[size_t{0}].to_number(), e[size_t{1}].to_number(), e[size_t{2}].to_number()});
    out[wm.first] = rw;
  }
  return out;
}

// Makes a World read a stage's recorded roads and waters.
inline void use_recorded(World& w, std::shared_ptr<const RecordedWorld> rw) {
  w.cell_roads = [rw](double i, double j) { return rw->roads(i, j); };
  w.wet_source = [rw](double x, double y, double m) { return rw->is_wet(x, y, m); };
}

// The roads of the view of cell (i, j) (World.js roadView: the 3 x 3 cells, row by row).
inline RoadList view_roads(const RecordedWorld& rw, double i, double j) {
  RoadList roads;
  for (double dj = -1; dj <= 1; dj += 1)
    for (double di = -1; di <= 1; di += 1) {
      const RoadList& c = rw.roads(i + di, j + dj);
      roads.insert(roads.end(), c.begin(), c.end());
    }
  return roads;
}

}  // namespace svx::city::test
