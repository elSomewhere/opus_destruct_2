// svx_city tests — heavy-industry layouts, quays and their props (voxel_city city/industry.js)
// against the reference (stage "industry" of tools/procgen_ref), in the stage's scripted worlds.
#include <doctest.h>

#include <string>
#include <vector>

#include "city/industry.hpp"
#include "prefab_records.hpp"
#include "records.hpp"

using namespace svx::city;
using rec::Line;

namespace {

// a lake's half-plane nx x + ny y > c with a ragged edge (the stage's waterWorld)
struct WaterWorld {
  double nx = 0, ny = 0, c = 0, level = 0;
  bool none = false;
  double seed = 0;
  std::optional<Shore> shore_near(double x, double y, double d) const {
    if (none) return std::nullopt;
    Shore s;
    s.level = level;
    s.dist = nx * x + ny * y - c - d;
    s.nx = -nx;
    s.ny = -ny;
    return s;
  }
  bool open_water_at(double x, double y) const { return nx * x + ny * y - c + (static_cast<double>(hash32(seed, x, y) & 63u) - 32) > 0; }
  bool is_wet(double x, double y, double m) const { return nx * x + ny * y - c + m * 8 > 0; }
};
// ({}: a world without isWet)
struct DryWorld {
  bool is_wet(double, double, double) const { return false; }
};

WaterWorld water_world(rec::Samples& r) {
  WaterWorld w;
  w.nx = r() - 0.5;
  w.ny = r() - 0.5;
  w.c = (r() - 0.5) * 3000;
  w.level = std::floor(r() * 40);
  w.none = r() < 0.15;
  w.seed = std::floor(r() * 1e6);
  return w;
}

const char* const kKinds[5] = {"tankFarm", "containerYard", "parking", "tankFarm", "containerYard"};

}  // namespace

TEST_CASE("city industry: layouts, quays and props conform to the reference (stage industry)") {
  rec::Samples r(31);
  rec::Out out;
  for (int i = 0; i < 120; ++i) {
    const std::string kind = kKinds[i % 5];
    const double x0 = std::floor((r() - 0.5) * 20000);
    const double y0 = std::floor((r() - 0.5) * 20000);
    const double w = 40 + std::floor(r() * 1500);
    const double h = 40 + std::floor(r() * 1500);
    const double z = std::floor(r() * 200);
    IndustrySpace space;
    space.id = js::cat("C", i % 9, "_", -static_cast<double>(i % 5), "/b", i, "/o");
    space.kind = kind;
    space.rect = {x0, y0, x0 + w, y0 + h};
    out << (Line() << "space" << i << space.id << kind << x0 << y0 << x0 + w << y0 + h << z);
    std::vector<Line> props;
    auto add_prop = [&](std::vector<Line>& to) {
      return [&to](std::string_view k, double x, double y, double zz, double ax, double ay, double bx, double by, const PropOpts& extra) {
        to.push_back(Line() << std::string(k) << x << y << zz << ax << ay << bx << by << irec::prop_opts(extra));
      };
    };
    dress_industry(DryWorld{}, space, z, add_prop(props));
    out << (Line() << "dry" << static_cast<double>(props.size()));
    for (const Line& l : props) out << l;
    std::string surf;
    for (int k = 0; k < 160; ++k) {
      const double x = x0 - 6 + std::floor(r() * (w + 12));
      const double y = y0 - 6 + std::floor(r() * (h + 12));
      uint16_t mat = 0;
      const bool handled = industry_surface(space, x, y, mat);
      if (k) surf += ' ';
      surf += js::cat(handled ? 1 : 0, handled ? js::num(mat) : std::string("-1"));
    }
    out << (Line() << "surf" << surf);
    const WaterWorld world = water_world(r);
    const std::optional<Quay> q = port_quay(world, space.rect);
    out << (Line() << "quay" << world.nx << world.ny << world.c << world.level << world.none << world.seed
                   << (q ? js::cat("[", q->axis, ",", q->sign, ",", q->c, ",", q->level, "]") : std::string("-")));
    if (q) {
      Line l;
      l << "qs";
      for (int k = 0; k < 12; ++k) {
        const double x = x0 + r() * w;
        const double y = y0 + r() * h;
        l << quay_side(*q, x, y);
      }
      out << l;
    }
    IndustrySpace wet;
    wet.id = space.id;
    wet.kind = kind;
    wet.rect = space.rect;
    wet.quay = q;
    std::vector<Line> props2;
    dress_industry(world, wet, z, add_prop(props2));
    dress_port(world, wet, z, add_prop(props2));
    out << (Line() << "wet" << static_cast<double>(props2.size()));
    for (const Line& l : props2) out << l;
  }
  CHECK(rec::record("industry", out.text()) == rec::recorded_digest("industry"));
}

TEST_CASE("city industry: a layout is made once per space") {
  IndustrySpace s;
  s.id = "C0_0/b1/o";
  s.kind = "tankFarm";
  s.rect = {0, 0, 900, 700};
  const IndustryLayout* a = industry_layout(s);
  REQUIRE(a);
  CHECK(a == industry_layout(s));
  CHECK(a->kind == "tankFarm");
  s.kind = "parking";
  IndustrySpace t;
  t.kind = "parking";
  CHECK(industry_layout(t) == nullptr);
}
