// svx_city tests — the road surface of a column (voxel_city network/roadSurface.js) against the
// reference (stage "roadsurface"): scripted road sets and the reference's own views (the roads
// stage roadview recorded: tools/procgen_ref/data/roadview.json).
#include <doctest.h>

#include "config/defaults.hpp"
#include "config/presets.hpp"
#include "network/roadSurface.hpp"
#include "network/roadView.hpp"
#include "records.hpp"
#include "road_inputs.hpp"
#include "world/wrap.hpp"

using namespace svx::city;
using rec::Line;

namespace {

// stages/roadsurface.mjs surfaceRecords
void surface_records(rec::Out& out, const RoadView& view, rec::Samples& r, RoadSample& rs, double seed, int n_along, int n_junctions) {
  if (view.segs.empty()) return;
  auto at = [&](double x, double y) {
    const double mode = r();
    const double R = mode < 0.4 ? 2 : mode < 0.8 ? 40 : view.max_reach + 44;
    const double reach = r() < 0.7 ? 0 : r() < 0.5 ? 8 : 44;
    sample_road_surface(view.near(Rect{x - R, y - R, x + R, y + R}), x, y, rs, seed, reach);
    out << (Line() << "rs" << x << y << R << reach << rs.kind << rs.mat << rs.dz << test::seg_ref(rs.seg) << rs.along << rs.side << rs.q << rs.sdf_c << rs.sdf_r);
  };
  for (int k = 0; k < n_along; ++k) {
    const RoadSeg& s = view.segs[static_cast<size_t>(std::floor(r() * static_cast<double>(view.segs.size())))];
    const double t = -30 + r() * (s.len + 60);
    const double lat = (r() - 0.5) * 2 * (s.hr + 40);
    double x = s.ax + s.dx * t - s.dy * lat;
    double y = s.ay + s.dy * t + s.dx * lat;
    if (r() < 0.5) {
      x = std::floor(x) + 0.5;
      y = std::floor(y) + 0.5;
    }
    at(x, y);
  }
  // the approaches of junctions
  std::vector<const RoadSeg*> with_jn;
  for (const RoadSeg& s : view.segs)
    if (!s.jn.empty()) with_jn.push_back(&s);
  if (with_jn.empty()) return;
  for (int g = 0; g < n_junctions; ++g) {
    const RoadSeg& s = *with_jn[static_cast<size_t>(std::floor(r() * static_cast<double>(with_jn.size())))];
    const RoadJunction& j = s.jn[static_cast<size_t>(std::floor(r() * static_cast<double>(s.jn.size())))];
    out << (Line() << "junction" << test::seg_ref(&s) << j.s << test::seg_ref(j.other));
    for (double a = -110; a <= 110; a += 4.5)
      for (double l = -(s.hr + 6); l <= s.hr + 6; l += 3) {
        const double x = std::floor(s.ax + s.dx * (j.s + a) - s.dy * l) + 0.5;
        const double y = std::floor(s.ay + s.dy * (j.s + a) + s.dx * l) + 0.5;
        at(x, y);
      }
  }
}

struct SurfaceView {
  const char* key;
  const char* id;
  const char* size;
  std::vector<std::array<double, 2>> cells;
};
// (stages/roadsurface.mjs VIEWS)
const std::vector<SurfaceView>& views() {
  static const std::vector<SurfaceView> v = {
      {"cities", "cities", "", {{0, 0}}},
      {"angledCities", "angledCities", "", {{0, 0}}},
      {"oldHarbourTown", "oldHarbourTown", "", {{0, 0}}},
      {"angledOldHarbourTown", "angledOldHarbourTown", "", {{0, 0}}},
      {"island:medium", "island", "medium", {{2, 1}}},
      {"wrapWorld:small", "wrapWorld", "small", {{77, 0}}},
      {"infiniteCity", "infiniteCity", "", {{3, -2}}},
  };
  return v;
}

}  // namespace

TEST_CASE("city roadsurface: the road surface conforms to the reference (stage roadsurface)") {
  rec::Samples r(43);
  rec::Out out;
  RoadSample rs = make_road_sample(0);
  for (int k = 0; k < 50; ++k) {
    const double ox = std::floor((r() - 0.5) * 20000);
    const double oy = std::floor((r() - 0.5) * 20000);
    const RoadView view(test::scripted_roads(r, k, ox, oy));
    rs.period = r() < 0.7 ? 0 : 1000 + std::floor(r() * 9000);
    const double seed = std::floor((r() - 0.5) * 2e9);
    out << (Line() << "set" << js::cat("S", k) << rs.period << seed);
    surface_records(out, view, r, rs, seed, 300, 2);
  }
  // (stars of 70 roads: more at a column than the reference's scratch of 64 holds)
  for (const int k : {0, 1}) {
    const RoadView view(test::star_roads(k, 70, 3000.0 * k, -2000, k == 1));
    rs.period = 0;
    out << (Line() << "set" << js::cat("T", k) << rs.period << 5);
    surface_records(out, view, r, rs, 5, 600, 2);
  }
  // (the views' roads: stage roadview's recording)
  const auto recorded = test::load_recorded("roadview");
  for (const SurfaceView& sv : views()) {
    const Value config = make_config(preset_config(sv.id, sv.size));
    const double seed = config["seed"].to_number();
    rs.period = wrap_of(config).size_v;
    const test::RecordedWorld& rw = *recorded.at(sv.key);
    for (const auto& c : sv.cells) {
      out << (Line() << "set" << js::cat(sv.key, ":", c[0], ",", c[1]) << rs.period << seed);
      const RoadView view(test::view_roads(rw, c[0], c[1]));
      surface_records(out, view, r, rs, seed, 2000, 4);
    }
  }
  CHECK(rec::record("roadsurface", out.text()) == rec::recorded_digest("roadsurface"));
}
