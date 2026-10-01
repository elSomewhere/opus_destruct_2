// svx_city tests — road views (voxel_city network/roadView.js) against the reference (stage
// "roadview"): scripted road sets and the reference's own roads (tools/procgen_ref/data).
#include <doctest.h>

#include <unordered_set>

#include "network/roadView.hpp"
#include "records.hpp"
#include "road_inputs.hpp"

using namespace svx::city;
using rec::Line;

namespace {

// stages/roadview.mjs viewRecords
void view_records(rec::Out& out, const RoadView& view, rec::Samples& r, const std::string& label) {
  out << (Line() << "view" << label << static_cast<double>(view.segs.size()) << view.max_reach);
  std::unordered_set<const Road*> seen;
  double x0 = js::kInf, y0 = js::kInf, x1 = -js::kInf, y1 = -js::kInf;
  for (const RoadSeg& s : view.segs) {
    Line l;
    l << "seg";
    test::seg_fields(l, s);
    out << l;
    for (const RoadJunction& j : s.jn) {
      Line lj;
      lj << "jn";
      test::junction_fields(lj, j);
      out << lj;
    }
    if (seen.insert(s.road.get()).second) {
      Line lr;
      lr << "rj" << test::seg_ref(&s);
      for (const RoadJunctionRef& e : s.rj->list) lr << js::cat(test::seg_ref(e.seg), ":", static_cast<double>(e.j - e.seg->jn.data()), ":", e.at);
      out << lr;
    }
    x0 = js::min(x0, s.bbox.x0);
    y0 = js::min(y0, s.bbox.y0);
    x1 = js::max(x1, s.bbox.x1);
    y1 = js::max(y1, s.bbox.y1);
  }
  if (view.segs.empty()) return;
  for (int k = 0; k < 60; ++k) {
    const double qx = x0 + r() * (x1 - x0);
    const double qy = y0 + r() * (y1 - y0);
    const double w = r() * 300;
    const double h = r() * 300;
    Line l;
    l << "near" << qx << qy << w << h;
    for (const RoadSeg* s : view.near(Rect{qx, qy, qx + w, qy + h})) l << test::seg_ref(s);
    out << l;
  }
}

struct ViewCase {
  const char* key;
  std::vector<std::array<double, 2>> cells;
};
// (stages/roadview.mjs VIEWS)
const std::vector<ViewCase>& views() {
  static const std::vector<ViewCase> v = {
      {"cities", {{0, 0}, {1, -1}}},       {"angledCities", {{0, 0}, {-1, 1}}}, {"oldHarbourTown", {{0, 0}}}, {"angledOldHarbourTown", {{0, 0}}},
      {"island:medium", {{0, 0}, {2, 1}}}, {"nordicTown:fjord", {{0, 0}}},     {"wrapWorld:small", {{77, 0}}}, {"infiniteCity", {{3, -2}}},
  };
  return v;
}

}  // namespace

TEST_CASE("city roadview: road views conform to the reference (stage roadview)") {
  rec::Samples r(41);
  rec::Out out;
  for (int k = 0; k < 160; ++k) {
    const double ox = std::floor((r() - 0.5) * 20000);
    const double oy = std::floor((r() - 0.5) * 20000);
    const RoadView view(test::scripted_roads(r, k, ox, oy));
    view_records(out, view, r, js::cat("S", k));
  }
  const auto recorded = test::load_recorded("roadview");
  for (const ViewCase& vc : views()) {
    const test::RecordedWorld& rw = *recorded.at(vc.key);
    for (const auto& c : vc.cells) {
      const RoadView view(test::view_roads(rw, c[0], c[1]));
      view_records(out, view, r, js::cat(vc.key, ":", c[0], ",", c[1]));
    }
  }
  CHECK(rec::record("roadview", out.text()) == rec::recorded_digest("roadview"));
}
