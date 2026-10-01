// svx_city tests — the apartment unit planner (voxel_city buildings/interior/units.js) against the
// reference (stage "units" of tools/procgen_ref): corridors, stair-hall sections and single units
// on scripted floors.
#include <doctest.h>

#include <algorithm>
#include <functional>

#include "building_records.hpp"
#include "buildings/interior/common.hpp"
#include "buildings/interior/grid.hpp"
#include "buildings/interior/units.hpp"
#include "records.hpp"

using namespace svx::city;
using namespace svx::city::test;
using rec::Line;

namespace {

const char* const kTypes[] = {"bath", "wc", "kitchen", "laundry", "closet", "storage", "living", "bedroom", "foyer", "hall", "studio", "nope"};
const char kUnitSides[4] = {'N', 'S', 'E', 'W'};

// One unit planned (or its fallback studio), as a record.
Line place(FloorGrid& grid, Rng& rng, const Rect& rect, char entry, const Room& circ, const std::optional<std::string>& unit_id, const std::optional<DoorNear>& near) {
  const std::vector<char> facades = facade_sides_of(grid, rect);
  UnitOpts opts;
  opts.rng = &rng;
  opts.unit = unit_id;
  opts.entry_near = near;
  const std::optional<UnitPlan> res = plan_unit(grid, rect, entry, circ, facades, opts);
  std::string out = "-";
  if (res) {
    out = res->template_ + "/";
    for (size_t k = 0; k < res->rooms.size(); ++k) out += js::cat(k ? "," : "", res->rooms[k]->id);
    out += js::cat("/", res->entry_door->id);
  }
  std::string fac(facades.begin(), facades.end());
  Line l;
  l << "u" << entry << rect_str(rect) << (fac.empty() ? std::string("-") : fac) << (near ? js::cat(near->u, ",", near->v) : std::string("-")) << fo(unit_id) << out
    << rng.next();
  if (!res) {
    RoomProps p;
    p.unit = unit_id;
    p.paint = "PAINT_WHITE";
    p.floor_mat = "FLOOR_OAK";
    const auto room = grid.add_room("studio", {rect}, p);
    DoorOpts o;
    o.width = 8;
    o.kind = "entry";
    if (!grid.add_door(*room, &circ, o)) {
      DoorOpts o2;
      o2.width = 6;
      o2.margin = 1;
      o2.kind = "entry";
      grid.add_door(*room, &circ, o2);
    }
  }
  return l;
}

std::array<double, 2> cell(char side, double a, double b, double A, double B) {
  if (side == 'N') return {a, b};
  if (side == 'S') return {a, B - 1 - b};
  if (side == 'W') return {b, a};
  return {B - 1 - b, a};
}
Rect rect_of(char side, double a0, double b0, double a1, double b1, double A, double B) {
  const auto p = cell(side, a0, b0, A, B);
  const auto q = cell(side, a1, b1, A, B);
  return {js::min(p[0], q[0]), js::min(p[1], q[1]), js::max(p[0], q[0]), js::max(p[1], q[1])};
}

double extra_of(rec::Samples& r) {
  const double dr = r();
  const double dy = r();
  return dr < 0.2 ? door_extra_of(static_cast<int>(std::floor(dy * 132))) : 0;
}

}  // namespace

TEST_CASE("city units: apartment units are the reference's (stage units)") {
  rec::Samples r(41);
  rec::Out out;
  // ---- styles and floors
  for (int s = 0; s < 24; ++s) {
    Rng rng(500 + s * 7907);
    const UnitStyle st = pick_unit_style(rng);
    Line l;
    l << "st" << st.paint << st.tile << st.wood << st.wet;
    for (const char* t : kTypes) l << floor_for(t, st);
    out << (l << rng.next());
  }
  // ---- double-loaded corridors
  for (int i = 0; i < 70; ++i) {
    const double U = 120 + std::floor(r() * 300);
    const double V = 110 + std::floor(r() * 130);
    FloorGrid g(U, V, {{0, 0, U - 1, V - 1}});
    g.door_extra = extra_of(r);
    const Rect inner{2, 2, U - 3, V - 3};
    const double cw = 10 + std::floor(r() * 8);
    const double cy0 = inner.y0 + 40 + std::floor(r() * (V - 4 - cw - 80));
    RoomProps cp;
    cp.paint = "PAINT_CREAM";
    cp.floor_mat = "FLOOR_TERRAZZO";
    const auto corr = g.add_room("corridor", {{inner.x0, cy0, inner.x1, cy0 + cw - 1}}, cp);
    Rng rng(std::floor(r() * 4294967296.0));
    const double mode = r();
    out << (Line() << "fa" << i << U << V << cw << cy0 << g.door_extra << mode);
    double unit_no = 0;
    const std::pair<Rect, char> zones[2] = {{{inner.x0, inner.y0, inner.x1, cy0 - 2}, 'S'}, {{inner.x0, cy0 + cw + 1, inner.x1, inner.y1}, 'N'}};
    for (const auto& [zone, entry] : zones) {
      const double target = js::round(rng.float_(7.5, 10.5) * 8);
      for (const auto& p : split_length(zone.x0, zone.x1, target, 5 * 8, rng)) {
        const Rect rect{p[0], zone.y0, p[1], zone.y1};
        std::optional<DoorNear> near;
        if (mode < 0.3) near = DoorNear{js::round((p[0] + p[1]) / 2) + 3, entry == 'S' ? cy0 : cy0 + cw - 1};
        std::optional<std::string> id;
        if (mode < 0.8) id = js::cat(entry, unit_no);
        unit_no += 1;
        out << place(g, rng, rect, entry, *corr, id, near);
      }
    }
    grid_records(out, "a", g);
    out << (Line() << "ae" << rng.next());
  }
  // ---- stair-hall sections
  for (int i = 0; i < 60; ++i) {
    const double U = 100 + std::floor(r() * 260);
    const double V = 60 + std::floor(r() * 90);
    FloorGrid g(U, V, {{0, 0, U - 1, V - 1}});
    g.door_extra = extra_of(r);
    const Rect inner{2, 2, U - 3, V - 3};
    const double hw = 20 + std::floor(r() * 16);
    const double c0 = inner.x0 + 30 + std::floor(r() * (U - 4 - hw - 60));
    const double c1 = c0 + hw - 1;
    const Rect hall{c0, inner.y0, c1, inner.y1};
    RoomProps hp;
    hp.paint = "PAINT_GRAY";
    hp.floor_mat = "FLOOR_TERRAZZO";
    const auto hall_room = g.add_room("hall", {hall}, hp);
    Rng rng(std::floor(r() * 4294967296.0));
    out << (Line() << "fb" << i << U << V << c0 << c1 << g.door_extra);
    const DoorNear near{(hall.x0 + hall.x1) / 2, (hall.y0 + hall.y1) / 2};
    double unit_no = 0;
    const std::pair<Rect, char> zones[2] = {{{inner.x0, inner.y0, c0 - 2, inner.y1}, 'E'}, {{c1 + 2, inner.y0, inner.x1, inner.y1}, 'W'}};
    for (const auto& [zone, entry] : zones) {
      if (zone.x1 - zone.x0 + 1 < 24) continue;
      out << place(g, rng, zone, entry, *hall_room, js::cat("s", unit_no), near);
      unit_no += 1;
    }
    grid_records(out, "b", g);
    out << (Line() << "be" << rng.next());
  }
  // ---- single units
  for (int i = 0; i < 400; ++i) {
    const double w = 24 + std::floor(r() * 100);
    const double dpt = 24 + std::floor(r() * 100);
    const char side = kUnitSides[static_cast<int>(std::floor(r() * 4))];
    const double cw = 8 + std::floor(r() * 6);
    auto pad = [&] { return r() < 0.5 ? 0.0 : 4 + std::floor(r() * 16); };
    const double pad_l = pad();
    const double pad_r = pad();
    const double pad_b = pad();
    const double A = 2 + pad_l + w + pad_r + 2;
    const double B = 2 + cw + 1 + dpt + pad_b + 2;
    const bool ns = side == 'N' || side == 'S';
    const double gu = ns ? A : B, gv = ns ? B : A;
    FloorGrid g(gu, gv, {{0, 0, gu - 1, gv - 1}});
    g.door_extra = extra_of(r);
    RoomProps hp;
    hp.paint = "PAINT_GRAY";
    hp.floor_mat = "FLOOR_TERRAZZO";
    const auto circ = g.add_room("hall", {rect_of(side, 2, 2, A - 3, 2 + cw - 1, A, B)}, hp);
    const Rect rect = rect_of(side, 2 + pad_l, 2 + cw + 1, 2 + pad_l + w - 1, 2 + cw + dpt, A, B);
    const bool wrong = r() < 0.05;
    int si = 0;
    while (kUnitSides[si] != side) ++si;
    const char entry = wrong ? kUnitSides[(si + 1) % 4] : side;
    const double nr = r();
    std::optional<DoorNear> near;
    if (nr < 0.3) near = DoorNear{rect.x0 + std::floor(nr * 40), rect.y0 + std::floor(nr * 30)};
    Rng rng(std::floor(r() * 4294967296.0));
    out << (Line() << "fc" << i << w << dpt << side << cw << pad_l << pad_r << pad_b << g.door_extra << entry);
    out << place(g, rng, rect, entry, *circ, js::cat("c", i), near);
    grid_records(out, "c", g);
  }
  CHECK(rec::record("units", out.text()) == rec::recorded_digest("units"));
}

TEST_CASE("city units: a unit's rooms are reachable from its entry door") {
  // a 12 x 9 m flat on a corridor, the facade opposite the entry: a band unit, every room joined
  FloorGrid g(100, 120, {{0, 0, 99, 119}});
  const auto corr = g.add_room("corridor", {{2, 2, 97, 13}});
  Rng rng(3);
  UnitOpts opts;
  opts.rng = &rng;
  opts.unit = "N0";
  const Rect rect{2, 15, 97, 117};
  const std::optional<UnitPlan> res = plan_unit(g, rect, 'N', *corr, facade_sides_of(g, rect), opts);
  REQUIRE(res);
  CHECK(res->entry_door->b == corr->id);
  const DoorGraph adj = g.door_graph();
  // every room reaches the corridor through doors
  std::vector<double> seen = {corr->id};
  for (size_t k = 0; k < seen.size(); ++k)
    if (const std::vector<double>* nbs = adj.get(seen[k]))
      for (double nb : *nbs)
        if (nb >= 0 && std::find(seen.begin(), seen.end(), nb) == seen.end()) seen.push_back(nb);
  for (const auto& room : res->rooms) CHECK(std::find(seen.begin(), seen.end(), room->id) != seen.end());
  for (const auto& room : res->rooms) CHECK(room->unit == std::optional<std::string>("N0"));
}
