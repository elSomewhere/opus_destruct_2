// svx_city tests — the charts and the wrapping world's lattice helpers (voxel_city world/chart.js,
// world/wrap.js) against the reference (stage "chart").
#include <doctest.h>

#include "config/defaults.hpp"
#include "records.hpp"
#include "world/chart.hpp"
#include "world/wrap.hpp"
#include "worlds.hpp"

using namespace svx::city;
using rec::Line;

namespace {

Value chart_cfg(const char* kind, double size = js::kNaN, int latitude = -1, double radius = js::kNaN, double face = js::kNaN) {
  Value v = Value::object();
  if (kind) v.set("chart", kind);
  if (size == size) v.set("size", size);
  if (latitude >= 0) v.set("latitude", latitude != 0);
  if (radius == radius) v.set("planet", Value::object({{"radius", radius}, {"face", face}}));
  return v;
}

// A value or "-" (JS: undefined).
template <class T>
void opt(Line& l, bool has, T v) {
  if (has)
    l << v;
  else
    l << rec::kUndef;
}

}  // namespace

TEST_CASE("city chart: charts and wrap helpers conform to the reference (stage chart)") {
  const std::vector<Value> charts = {
      chart_cfg(nullptr),
      chart_cfg("flat"),
      chart_cfg("torus", 48000),
      chart_cfg("torus", 96000),
      chart_cfg("torus", 192000),
      chart_cfg("torus", 30000, 0),
      chart_cfg("torus"),
      chart_cfg("cube", js::kNaN, -1, 240000, 0),
      chart_cfg("cube", js::kNaN, -1, 240000, 1),
      chart_cfg("cube", js::kNaN, -1, 240000, 2),
      chart_cfg("cube", js::kNaN, -1, 240000, 3),
      chart_cfg("cube", js::kNaN, -1, 240000, 4),
      chart_cfg("cube", js::kNaN, -1, 240000, 5),
      chart_cfg("cube", js::kNaN, -1, 50000, 3),
      chart_cfg("cube"),
  };
  rec::Samples r(7);
  rec::Out out;
  for (const Value& cfg : charts) {
    const Chart c = make_chart(cfg);
    const bool torus = c.kind == Chart::Kind::Torus, cube = c.kind == Chart::Kind::Cube;
    Line head;
    head << "chart" << c.id;
    opt(head, torus, c.R);
    opt(head, cube, c.half);
    opt(head, cube, c.radius);
    opt(head, cube, c.face);
    head << c.has_latitude_fn();
    out << head;
    static constexpr double kScales[4] = {10, 1000, 100000, 1e7};
    for (int k = 0; k < 400; ++k) {
      const double scale = kScales[k % 4];
      const double x = (r() - 0.5) * 2 * scale;
      const double y = (r() - 0.5) * 2 * scale;
      const FieldPoint f = c.to_field(x, y);
      Line l;
      l << "p" << x << y << f.x << f.y << f.z;
      opt(l, torus, f.w);
      l << c.contains(x, y) << c.edge_distance(x, y);
      const std::optional<double> lat = c.latitude(f.x, f.y, f.z, f.w);
      opt(l, lat.has_value(), lat ? *lat : 0);
      out << l;
    }
    for (int k = 0; k < 20; ++k) {
      const double x = js::round((r() - 0.5) * 2e6);
      const double y = js::round((r() - 0.5) * 2e6);
      const FieldPoint f = c.to_field(x + 3 * c.size, y - 2 * c.size);
      Line l;
      l << "lap" << x << y << f.x << f.y << f.z;
      opt(l, torus, f.w);
      out << l;
    }
  }
  for (const test::WorldSpec& ws : test::worlds()) {
    const Value cfg = make_config(test::world_overrides(ws));
    const Wrap W(cfg);
    Line head;
    head << "wrap" << ws.key << make_chart(cfg["world"]).id << W.on << W.size << W.size_v;
    for (const double s : {500.0, 620.0, 3600.0, 9000.0, 1e5, 7.0, 0.0}) head << W.count(s);
    out << head;
    for (int k = 0; k < 60; ++k) {
      const double ns[5] = {0, 1, 7, W.count(620), W.count(9000)};
      const double n = ns[k % 5];
      const double i = std::floor((r() - 0.5) * 400);
      const double x = (r() - 0.5) * 4e6;
      out << (Line() << "w" << n << i << W.canon(i, n) << W.lap(i, n) << x << W.v(x) << W.vi(x) << W.v(js::round(x)) << W.vi(js::round(x)));
    }
  }
  CHECK(rec::record("chart", out.text()) == rec::recorded_digest("chart"));
}
