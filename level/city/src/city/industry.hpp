// svx_city — heavy industry block programs (voxel_city city/industry.js): tank farms (storage
// tanks in bunded pads, pipe racks, a process unit with distillation columns and a flare stack)
// and container yards (stacked shipping containers in blocks with gantry cranes); harbour quays
// (portQuay, quaySide) with ship-to-shore cranes and moored ships; the industrial prop prefabs.
// One deterministic layout per space is shared by the ground surface (landscape) and the props
// (dressing).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "buildings/interior/prefabs.hpp"
#include "core/cache.hpp"
#include "core/hash.hpp"
#include "core/math.hpp"
#include "core/rect.hpp"
#include "world/World.hpp"

namespace svx::city {

// A straight quay of a port yard: along axis 'x' (the quay line x = c) or 'y' (y = c); sign +1
// when the water lies towards +axis; level the water's level (voxels).
struct Quay {
  char axis = 'x';
  double sign = 1;
  double c = 0;
  double level = 0;
};

// ---- layouts (layoutOf)
struct IndustryTank {
  double x = 0, y = 0, r = 0, h = 0, pad = 0;
};
struct IndustryRack {  // a pipe rack along x
  double x0 = 0, x1 = 0, y = 0;
};
struct IndustryRackV {  // the process unit's rack, along y
  double x = 0, y0 = 0, y1 = 0;
};
struct IndustryColumn {
  double x = 0, y = 0, r = 0, h = 0;
};
struct IndustryFlare {
  double x = 0, y = 0, h = 0;
};
struct ContainerStackRec {
  double x = 0, y = 0, n = 0, seed = 0;
};
struct GantryCraneRec {
  double x = 0, y = 0, span = 0;
};
struct ContainerBlock {
  double y0 = 0, y1 = 0;
  std::vector<ContainerStackRec> stacks{};
  std::vector<GantryCraneRec> cranes{};
};
// A space's layout: kind "tankFarm" (cell .. flare) or "containerYard" (blocks, lane).
struct IndustryLayout {
  std::string kind{};
  // tankFarm
  double cell = 0, x0 = 0, y0 = 0, nx = 0, ny = 0;
  std::vector<IndustryTank> tanks{};
  std::vector<IndustryRack> racks{};
  std::optional<IndustryRackV> racks_v = std::nullopt;
  std::optional<Rect> process = std::nullopt;
  std::vector<IndustryColumn> columns{};
  std::optional<IndustryFlare> flare = std::nullopt;
  // containerYard
  std::vector<ContainerBlock> blocks{};
  double lane = 0;
};

// The fields of a cell plan's open space (city/cellPlan.js: {id, kind, rect, quay, ...}) that
// industry.js reads. The cell plan's space record (a later stage of the port) derives from it;
// `layout` is industry.js's WeakMap cache (the layout made on first use, once).
struct IndustrySpace {
  std::string id{};    // `${block.id}/o`
  std::string kind{};  // "tankFarm" | "containerYard" | any other (no layout)
  Rect rect{};
  std::optional<Quay> quay = std::nullopt;  // (port districts: portQuay)
  Lazy<std::optional<IndustryLayout>> layout{};
};

// layoutOf(space): the space's layout, null for kinds that have none.
const IndustryLayout* industry_layout(const IndustrySpace& space);

// The ground surface of a heavy-industry space at world column (x, y): the material for out.mat
// and true when handled (false, nothing written, for a space without a layout).
bool industry_surface(const IndustrySpace& space, double x, double y, uint16_t& mat);

// Is (x, y) on the water side of a quay line (> 0), on it (0) or inland (< 0)?
inline double quay_side(const Quay& q, double x, double y) { return q.sign * ((q.axis == 'x' ? x : y) - q.c); }

// The straight quay of a port yard that a big lake (or the sea at an island town's harbour)
// reaches into: the water side (the axis direction towards the water) beyond the line where half
// the yard's cross-section is water becomes a dredged basin; the land side stays level. Null when
// there is none. `world` answers shore_near(x, y, max_dist) -> std::optional<Shore> and
// open_water_at(x, y) -> bool (World's, or a test's).
template <class W>
std::optional<Quay> port_quay(const W& world, const Rect& rect) {
  const double cx = (rect.x0 + rect.x1) / 2;
  const double cy = (rect.y0 + rect.y1) / 2;
  // a big lake's shore, or the sea at an island town's harbour
  const std::optional<Shore> sh = world.shore_near(cx, cy, vx(500));
  if (!sh) return std::nullopt;
  const char axis = std::fabs(sh->nx) > std::fabs(sh->ny) ? 'x' : 'y';
  const double sign = js::or_(-js::sign(axis == 'x' ? sh->nx : sh->ny), 1);
  const double a0 = axis == 'x' ? rect.x0 : rect.y0;
  const double a1 = axis == 'x' ? rect.x1 : rect.y1;
  const double b0 = axis == 'x' ? rect.y0 : rect.x0;
  const double b1 = axis == 'x' ? rect.y1 : rect.x1;
  const double step = vx(6);
  bool found = false;
  double c = 0;
  // walk from the land side towards the water; the quay goes where the lake takes over
  for (double t = 0; t <= a1 - a0; t += step) {
    const double a = sign > 0 ? a0 + t : a1 - t;
    double wet = 0;
    double n = 0;
    for (double b = b0; b <= b1; b += step) {
      n += 1;
      if (axis == 'x' ? world.open_water_at(a, b) : world.open_water_at(b, a)) wet += 1;
    }
    if (wet * 2 >= n) {
      c = a;
      found = true;
      break;
    }
  }
  if (!found) return std::nullopt;
  // keep a working apron of at least 40 m on land; where the lake covers the land side as well,
  // the yard is reclaimed (filled) out to 60 m
  if ((sign > 0 ? c - a0 : a1 - c) < vx(40)) {
    if (a1 - a0 < vx(90)) return std::nullopt;
    c = sign > 0 ? a0 + vx(60) : a1 - vx(60);
  }
  return Quay{axis, sign, js::round(c), sh->level};
}

// The props of dressing.js: addProp(kind, x, y, z, ax, ay, bx, by, extra) places a prop of
// city/propPrefabs.hpp (a its along axis, b its across axis in the world) with its options.
struct PropOpts {
  std::optional<double> h = std::nullopt;        // streetlight [52], silo, storageTank, processColumn, flareStack
  std::optional<double> reach = std::nullopt;    // streetlight [12], signal [28]
  std::optional<uint16_t> pole = std::nullopt;   // streetlight [POLE_METAL]
  std::optional<double> r = std::nullopt;        // silo, storageTank, processColumn
  std::optional<double> seed = std::nullopt;     // cargoShip [1], containerStack
  std::optional<double> n = std::nullopt;        // garageRow, containerStack
  std::optional<double> hw = std::nullopt;       // bund
  std::optional<double> len = std::nullopt;      // pipeRack
  std::optional<double> span = std::nullopt;     // gantryCrane
  std::optional<bool> plinth = std::nullopt;     // (world/landmarks.js: a granite plinth under it)
};

// Harbour front: ship-to-shore cranes along a port yard's quay with their booms over the basin,
// and a cargo ship moored alongside each. add_prop(kind, x, y, z, ax, ay, bx, by, extra). (The
// world is not read: the reference passes it.)
template <class W, class AddProp>
void dress_port(const W& world, const IndustrySpace& space, double z, AddProp&& add_prop) {
  (void)world;
  if (!space.quay) return;
  const Quay& q = *space.quay;
  const Rect& r = space.rect;
  const double bx = q.axis == 'x' ? q.sign : 0;
  const double by = q.axis == 'x' ? 0 : q.sign;
  const double ax = -by;
  const double ay = bx;
  const double b0 = q.axis == 'x' ? r.y0 : r.x0;
  const double b1 = q.axis == 'x' ? r.y1 : r.x1;
  for (double b = b0 + vx(40); b <= b1 - vx(40); b += vx(95)) {
    const double x = q.axis == 'x' ? q.c : b;
    const double y = q.axis == 'x' ? b : q.c;
    add_prop("stsCrane", x - bx * vx(2), y - by * vx(2), z, ax, ay, bx, by, PropOpts{});
    PropOpts ship;
    ship.seed = hash32(x, y, 5);
    add_prop("cargoShip", x + bx * vx(14), y + by * vx(14), q.level + 1, ax, ay, bx, by, ship);
  }
}

// The props of a heavy-industry space via dressing's add_prop(kind, x, y, z, ax, ay, bx, by,
// extra); nothing stands in the water where a lake or river cuts into the yard (`world` answers
// is_wet(x, y, margin_m) -> bool) or beyond 16 m inland of its quay.
template <class W, class AddProp>
void dress_industry(const W& world, const IndustrySpace& space, double z, AddProp&& add_prop) {
  const IndustryLayout* L = industry_layout(space);
  if (!L) return;
  auto dry = [&](double x, double y) { return !world.is_wet(x, y, 4) && !(space.quay && quay_side(*space.quay, x, y) > -vx(16)); };
  auto add = [&](std::string_view kind, double x, double y, double ax, double ay, double bx, double by, const PropOpts& extra) {
    if (dry(x, y)) add_prop(kind, x, y, z, ax, ay, bx, by, extra);
  };
  if (L->kind == "tankFarm") {
    for (const IndustryTank& t : L->tanks) {
      PropOpts tank;
      tank.r = t.r;
      tank.h = t.h;
      add("storageTank", t.x, t.y, 1, 0, 0, 1, tank);
      PropOpts bund;
      bund.hw = t.pad;
      add("bund", t.x, t.y, 1, 0, 0, 1, bund);
    }
    for (const IndustryRack& k : L->racks) {
      PropOpts o;
      o.len = k.x1 - k.x0;
      add("pipeRack", k.x0, k.y, 1, 0, 0, 1, o);
    }
    // the unit's rack runs along its length (a = +y, b = -x)
    if (L->racks_v) {
      PropOpts o;
      o.len = L->racks_v->y1 - L->racks_v->y0;
      add("pipeRack", L->racks_v->x, L->racks_v->y0, 0, 1, -1, 0, o);
    }
    for (const IndustryColumn& c : L->columns) {
      PropOpts o;
      o.r = c.r;
      o.h = c.h;
      add("processColumn", c.x, c.y, 1, 0, 0, 1, o);
    }
    if (L->flare) {
      PropOpts o;
      o.h = L->flare->h;
      add("flareStack", L->flare->x, L->flare->y, 1, 0, 0, 1, o);
    }
    return;
  }
  for (const ContainerBlock& b : L->blocks) {
    for (const ContainerStackRec& s : b.stacks) {
      PropOpts o;
      o.n = s.n;
      o.seed = s.seed;
      add("containerStack", s.x, s.y, 1, 0, 0, 1, o);
    }
    for (const GantryCraneRec& c : b.cranes) {
      PropOpts o;
      o.span = c.span;
      add("gantryCrane", c.x, c.y, 1, 0, 0, 1, o);
    }
  }
}

// ---- prefabs (INDUSTRY_PROPS): the same frame as propPrefabs.hpp's.
using PropBuild = std::vector<PrefabBox> (*)(Rng& rng, const PropOpts& o);
struct PropDef {
  const char* id = nullptr;
  PropBuild build = nullptr;
};
// INDUSTRY_PROPS, in the reference's key order.
const std::vector<PropDef>& industry_props();

}  // namespace svx::city
