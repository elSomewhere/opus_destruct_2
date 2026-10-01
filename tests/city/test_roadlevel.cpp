// svx_city tests — road levels (voxel_city network/roadLevel.js) against the reference (stage
// "roadlevel"): road profiles, segment levels, the nearest road's level and the street level, on
// the reference's own roads (tools/procgen_ref/data).
#include <doctest.h>

#include "config/presets.hpp"
#include "network/arterials.hpp"
#include "network/roadLevel.hpp"
#include "network/roadView.hpp"
#include "records.hpp"
#include "road_inputs.hpp"
#include "world/World.hpp"

using namespace svx::city;
using rec::Line;

namespace {

struct LevelCase {
  const char* key;
  const char* id;  // a preset, or JSON overrides
  const char* size;
  double i, j;
};
// (stages/roadlevel.mjs LEVELS)
const std::vector<LevelCase>& levels() {
  static const std::vector<LevelCase> v = {
      {"cities", "cities", "", 0, 0},
      {"nordicTown:fjord", "nordicTown", "fjord", 0, 0},
      {"oldHarbourTown", "oldHarbourTown", "", 0, 0},
      {"island:medium", "island", "medium", 1, 1},
      {"angledCities", "angledCities", "", 0, 0},
      {"angledOldHarbourTown", "angledOldHarbourTown", "", 0, 0},
      {"angledNoRamps", R"({"seed":77,"world":{"mode":"island","island":{"radius":2600,"population":5000,"towns":0},"angles":{"enabled":true,"features":{"ramps":false}}}})",
       "", 0, 0},
      {"wrapWorld:small", "wrapWorld", "small", 77, 0},
  };
  return v;
}

Value config_of(const char* id, const char* size) {
  if (id[0] != '{') return preset_config(id, size);
  Value v;
  if (!Value::parse_json(id, &v)) SVX_FAIL("roadlevel: bad JSON");
  return v;
}

void prof_fields(Line& l, const RoadProfile& p) {
  std::vector<double> z(p.z.begin(), p.z.end());
  l << p.L << p.n << z;
  if (p.knots)
    l << p.ks << p.kz;
  else
    l << rec::kUndef << rec::kUndef;
}

// stages/roadlevel.mjs nearSeg
std::array<double, 2> near_seg(const std::vector<const RoadSeg*>& segs, const Rect& rect, rec::Samples& r) {
  const RoadSeg& s = *segs[static_cast<size_t>(std::floor(r() * static_cast<double>(segs.size())))];
  const double t = r() * s.len;
  const double lat = (r() - 0.5) * 2 * (s.hr + 30);
  const double x = s.ax + s.dx * t - s.dy * lat;
  const double y = s.ay + s.dy * t + s.dx * lat;
  return {js::max(rect.x0, js::min(rect.x1, x)), js::max(rect.y0, js::min(rect.y1, y))};
}

}  // namespace

TEST_CASE("city roadlevel: road levels conform to the reference (stage roadlevel)") {
  rec::Samples r(47);
  rec::Out out;
  const auto recorded = test::load_recorded("roadlevel");
  for (const LevelCase& lc : levels()) {
    World w(config_of(lc.id, lc.size));
    const std::shared_ptr<const test::RecordedWorld> rw = recorded.at(lc.key);
    test::use_recorded(w, rw);
    const std::string net_id = js::cat("C", w.arterials->canon(lc.i), "_", w.arterials->canon(lc.j));
    const RoadList& net_roads = rw->roads(lc.i, lc.j);
    const std::shared_ptr<const RoadView> view = w.road_view(lc.i, lc.j);
    out << (Line() << "world" << lc.key << net_id << static_cast<double>(net_roads.size()) << static_cast<double>(view->segs.size()));
    for (const RoadPtr& road : net_roads) {
      Line l;
      l << "prof" << road->id;
      prof_fields(l, road_profile(w, *road));
      out << l;
    }
    std::vector<const RoadSeg*> own;
    for (const RoadSeg& s : view->segs)
      if (s.road->cell == net_id) own.push_back(&s);
    for (const RoadSeg* sp : own) {
      const RoadSeg& s = *sp;
      Line l;
      l << "seg" << test::seg_ref(&s);
      for (double a = 0; a < s.len; a += 12) l << segment_level(w, s, a);
      l << segment_level(w, s, s.len);
      out << l;
      for (const RoadJunctionRef& e : s.rj->list) {
        const double at = e.at - s.s0;
        Line lj;
        lj << "jlv" << test::seg_ref(&s) << test::seg_ref(e.seg) << e.at;
        for (const double o : {-60.0, -30.0, -14.0, -7.0, -2.0, 0.0, 2.0, 7.0, 14.0, 30.0, 60.0}) {
          const double a = at + o;
          if (a >= -20 && a <= s.len + 20) lj << a << segment_level(w, s, a);
        }
        out << lj;
      }
    }
    // (the angled world: segments of the east neighbour's roads, levelled from its own view)
    int n_east = 0;
    for (const RoadSeg& s : view->segs) {
      if (!s.road->home || (*s.road->home)[0] != lc.i + 1 || (*s.road->home)[1] != lc.j || n_east >= 12) continue;
      n_east += 1;
      Line l;
      l << "east" << test::seg_ref(&s);
      for (const double f : {0.0, 0.25, 0.5, 0.75, 1.0}) l << segment_level(w, s, s.len * f);
      out << l;
    }
    // (points in the cell, 200 voxels in from its edges)
    const Rect rc = w.arterials->cell_rect(lc.i, lc.j);
    const Rect inner{rc.x0 + 200, rc.y0 + 200, rc.x1 - 200, rc.y1 - 200};
    if (!own.empty()) {
      for (int k = 0; k < 1200; ++k) {
        std::array<double, 2> p;
        if (r() < 0.3) {
          const double x = inner.x0 + r() * (inner.x1 - inner.x0);
          const double y = inner.y0 + r() * (inner.y1 - inner.y0);
          p = {x, y};
        } else {
          p = near_seg(own, inner, r);
        }
        const double reach = r() < 0.5 ? 8 : 24;
        const std::optional<RoadLevel> q = road_level_at(w, *view, p[0], p[1], reach);
        Line l;
        l << "at" << p[0] << p[1] << reach;
        if (q)
          l << q->z << test::seg_ref(q->seg) << q->along << q->dist << q->sidewalk;
        else
          l << rec::kUndef;
        out << l;
      }
      for (int k = 0; k < 300; ++k) {
        const std::array<double, 2> p = near_seg(own, inner, r);
        out << (Line() << "street" << p[0] << p[1] << w.street_level(p[0], p[1]));
      }
    }
    for (const RoadPtr& road : net_roads)
      if (road->prof0.ready()) {
        Line l;
        l << "prof0" << road->id;
        prof_fields(l, *road->prof0.get([]() -> std::shared_ptr<const RoadProfile> { SVX_FAIL("prof0: not made"); }));
        out << l;
      }
  }
  CHECK(rec::record("roadlevel", out.text()) == rec::recorded_digest("roadlevel"));
}
