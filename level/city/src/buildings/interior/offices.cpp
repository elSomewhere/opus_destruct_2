// svx_city — voxel_city buildings/interior/offices.js.
#include "buildings/interior/offices.hpp"

#include <utility>

#include "buildings/interior/apartments.hpp"
#include "buildings/interior/common.hpp"
#include "svx/base/types.hpp"

namespace svx::city {

namespace {

constexpr double M = 8;
constexpr double EXT_T = FloorGrid::EXT_T;

DoorOpts door(double width, double margin = js::kNaN) {
  DoorOpts o;
  o.width = width;
  if (margin == margin) o.margin = margin;
  return o;
}

// Door in the front facade exactly where a skybridge meets the building.
void sky_door(FloorGrid& grid, const Envelope& env, const Room& open, const SkyDoor& d) {
  const Frame& frame = envelope_frame(env);
  const Rect& R = env.R;
  const Rect span = d.span_x ? Rect{d.s0, R.y0, d.s1, R.y1} : Rect{R.x0, d.s0, R.x1, d.s1};
  const Rect c = frame.rect_from_world(span);
  const double width = c.x1 - c.x0 + 1;
  DoorOpts o;
  o.kind = "sky";
  o.width = width;
  o.margin = 1;
  o.place = "near";
  o.near = DoorNear{(c.x0 + c.x1 + 1) / 2, -12};
  o.leaf = "glass";
  o.height = 20;
  const std::shared_ptr<Door> placed = grid.add_door(open, nullptr, o);
  if (placed && std::fabs(placed->u0 - c.x0) > 1) placed->misplaced = true;
}

OpenFloorCores paint_office_core(FloorGrid& grid, const CoreLayout& layout, bool basement) {
  OpenFloorCores c;
  for (const CoreLayoutStair& s : layout.stairs) {
    c.stair_rooms.push_back(add_stair_room(grid, *s.stair));
    c.stairs.push_back(s.stair.get());
  }
  for (const CoreLayoutElevator& e : layout.elevators) {
    RoomProps p;
    p.elevator = e.elev.id;
    c.elev_rooms.push_back(grid.add_room("elevator", {e.rect}, p));
  }
  for (const Rect& s : layout.shafts) grid.add_room("shaft", {s});
  for (const Rect& r : layout.restrooms) {
    RoomProps p;
    p.paint = "WALL_TILE_WHITE";
    p.floor_mat = "FLOOR_TILE_GRAY";
    c.restrooms.push_back(grid.add_room(basement ? "storage" : "restroom", {r}, p));
  }
  return c;
}

// OPEN_STYLE: the open room of a floor by its kind
struct OpenStyle {
  const char* type;
  const char* paint;
  const char* floor_mat;
};
const OpenStyle& open_style(const std::string& kind) {
  static const OpenStyle kOffice{"openOffice", "PAINT_WHITE", "FLOOR_CARPET_GRAY"};
  static const OpenStyle kLobby{"lobby", "PAINT_WHITE", "FLOOR_MARBLE"};
  static const OpenStyle kParking{"parking", "CONCRETE", "FLOOR_CONCRETE"};
  static const OpenStyle kStore{"departmentFloor", "PAINT_WHITE", "FLOOR_TERRAZZO"};
  if (kind == "office") return kOffice;
  if (kind == "lobby") return kLobby;
  if (kind == "parking") return kParking;
  if (kind == "store") return kStore;
  SVX_FAIL("offices: an open floor of no known kind");
}

}  // namespace

OfficeCore office_core(const Rect& inner, const StairDims& sd, double nF) {
  const double Ui = inner.x1 - inner.x0 + 1;
  const double Vi = inner.y1 - inner.y0 + 1;
  const double n_elev = nF <= 5 ? 1 : nF <= 14 ? 2 : nF <= 28 ? 3 : 4;
  const bool compact = Ui < 26 * M || Vi < sd.L + 2 * 5 * M;
  std::vector<std::pair<const char*, double>> parts;
  if (compact)
    parts = {{"stair", sd.W}, {"restroom", 20}, {"elevators", n_elev * 17 - 1}};
  else
    parts = {{"stair", sd.W}, {"restroom", 20}, {"elevators", n_elev * 17 - 1}, {"restroom", 20}, {"stair", sd.W}};
  double sum = 0;
  for (const auto& p : parts) sum = sum + p.second;
  const double Wc = sum + static_cast<double>(parts.size()) - 1;
  const double D = sd.L;
  const double x0 = inner.x0 + std::floor((Ui - Wc) / 2);
  const double y0 = inner.y0 + std::floor((Vi - D) / 2);
  OfficeCore core;
  double x = x0;
  for (const auto& p : parts) {
    core.comps.push_back({p.first, {x, y0, x + p.second - 1, y0 + D - 1}});
    x += p.second + 1;
  }
  core.rect = {x0, y0, x - 2, y0 + D - 1};
  core.n_elev = n_elev;
  return core;
}

void plan_office_building(const Envelope& env, Rng& rng, PlanBuilder& pb) {
  const double nF = env.floors;
  const std::vector<Rect>& tops = tier_rects(env, nF - 1);
  if (tops.empty()) SVX_FAIL("offices: an envelope without its top floor");
  const Rect top = tops[0];
  const Rect inner{top.x0 + EXT_T, top.y0 + EXT_T, top.x1 - EXT_T, top.y1 - EXT_T};
  double max_h = js::truthy(env.basements) ? env.basement_h : 0;
  for (double h : env.story_h) max_h = js::max(max_h, h);
  const StairDims sd = stair_dims(max_h);
  const OfficeCore core = office_core(inner, sd, nF);
  const double f_bottom = -env.basements;
  CoreLayout layout;
  for (const OfficeCoreComp& c : core.comps) {
    if (c.kind == "stair") {
      MakeStairOpts so;
      so.rect = c.rect;
      so.axis = 'v';
      so.dir = 1;
      so.lane_low = rng.chance(0.5);
      so.f0 = f_bottom;
      so.f1 = nF;
      layout.stairs.push_back({c.rect, pb.add_stair(make_stair(so))});
    } else if (c.kind == "restroom") {
      layout.restrooms.push_back(c.rect);
    } else {
      // elevator bank: shafts at the front, machine/shaft room behind
      for (double k = 0; k < core.n_elev; k += 1) {
        const double ex0 = c.rect.x0 + k * 17;
        const Rect rect{ex0, c.rect.y0, ex0 + 15, c.rect.y0 + 15};
        Elevator el;
        el.rect = rect;
        el.f0 = f_bottom;
        el.f1 = nF - 1;
        el.door_side = 'N';
        layout.elevators.push_back({rect, pb.add_elevator(el)});
      }
      layout.shafts.push_back({c.rect.x0, c.rect.y0 + 17, c.rect.x1, c.rect.y1});
    }
  }
  for (double f = f_bottom; f < 0; f += 1) {
    const std::shared_ptr<FloorGrid> grid = pb.new_grid(f);
    const OpenFloorCores cores = paint_office_core(*grid, layout, f < 0);
    plan_open_floor(*grid, env, rng, cores, "parking");
    pb.add_floor(f, grid, "parking");
  }
  const std::shared_ptr<FloorGrid> g0 = pb.new_grid(0);
  {
    const OpenFloorCores cores = paint_office_core(*g0, layout, false);
    plan_open_floor(*g0, env, rng, cores, "lobby");
  }
  pb.add_floor(0, g0, "ground");
  std::shared_ptr<FloorGrid> typical;
  std::vector<Rect> tier_key;  // (JSON.stringify(tierRects(env, f)): the rects' values)
  for (double f = 1; f < nF; f += 1) {
    std::vector<const SkyDoor*> doors;
    for (const SkyDoor& d : env.sky_doors)
      if (d.floor == f) doors.push_back(&d);
    if (!doors.empty()) {
      // skybridge floor: its own grid with a glass door where the bridge lands
      const std::shared_ptr<FloorGrid> g = pb.new_grid(f);
      Rng frng = rng.fork(js::cat("office", f));
      const OpenFloorCores cores = paint_office_core(*g, layout, false);
      const std::shared_ptr<Room> open = plan_open_floor(*g, env, frng, cores, "office");
      for (const SkyDoor* d : doors) sky_door(*g, env, *open, *d);
      pb.add_floor(f, g, "office");
      continue;
    }
    const std::vector<Rect>& key = tier_rects(env, f);
    if (!typical || key != tier_key) {
      typical = pb.new_grid(f);
      tier_key = key;
      Rng frng = rng.fork(js::cat("office", f));
      const OpenFloorCores cores = paint_office_core(*typical, layout, false);
      plan_open_floor(*typical, env, frng, cores, "office");
    }
    pb.add_floor(f, typical, "office");
  }
}

std::shared_ptr<Room> plan_open_floor(FloorGrid& grid, const Envelope& env, Rng& rng, const OpenFloorCores& cores, const std::string& kind) {
  const std::vector<Rect> inners = grid.inner_rects();
  if (inners.empty()) SVX_FAIL("offices: an open floor without a footprint");
  const Rect inner = inners[0];
  std::vector<Rect> taken;
  for (const auto& room : grid.rooms)
    for (const Rect& r : room->rects) taken.push_back({r.x0 - 1, r.y0 - 1, r.x1 + 1, r.y1 + 1});
  std::optional<Rect> core_box;
  if (!taken.empty()) {
    Rect a = taken[0];
    for (size_t i = 1; i < taken.size(); ++i) {
      const Rect& r = taken[i];
      a = {js::min(a.x0, r.x0), js::min(a.y0, r.y0), js::max(a.x1, r.x1), js::max(a.y1, r.y1)};
    }
    core_box = a;
  }
  struct Enclosed {
    std::shared_ptr<Room> room;
    bool own = false;
  };
  std::vector<Enclosed> enclosed;
  const OpenStyle& st = open_style(kind);

  if (kind == "office" || kind == "lobby") {
    // end zones along the side facades
    const double ew = js::round(rng.float_(3.6, 4.6) * M);
    for (const char side : {'W', 'E'}) {
      const Rect zone = side == 'W' ? Rect{inner.x0, inner.y0, inner.x0 + ew - 1, inner.y1} : Rect{inner.x1 - ew + 1, inner.y0, inner.x1, inner.y1};
      if (core_box && r_overlaps(*core_box, Rect{zone.x0 - 6 * M, zone.y0, zone.x1 + 6 * M, zone.y1})) continue;
      if (zone.y1 - zone.y0 + 1 < 6 * M) continue;
      if (kind == "lobby") {
        // shops at the front corner, back-of-house behind
        const double shop_depth = js::min(js::round((zone.y1 - zone.y0) * 0.55), 9 * M);
        const Rect shop_rect{zone.x0, zone.y0, zone.x1, zone.y0 + shop_depth - 1};
        const Rect boh{zone.x0, shop_rect.y1 + 2, zone.x1, zone.y1};
        const std::shared_ptr<Room> shop = shop_with_backroom(grid, &env, rng, shop_rect, nullptr);
        enclosed.push_back({shop, true});
        static const char* const kBoh[] = {"mailroom", "security", "storage"};
        const std::string type = rng.pick(kBoh);
        RoomProps p;
        p.paint = "PAINT_GRAY";
        p.floor_mat = "FLOOR_LINOLEUM";
        enclosed.push_back({grid.add_room(type, {boh}, p), false});
      } else {
        static const std::vector<std::pair<std::string, double>> kTypes = {{"meeting", 3}, {"office", 4}, {"breakroom", 1}, {"storage", 0.5}};
        const double target = js::round(rng.float_(3.2, 4.8) * M);
        for (const auto& piece : split_length(zone.y0, zone.y1, target, 3 * M, rng)) {
          const std::string type = rng.weighted(kTypes);
          RoomProps p;
          p.paint = type == "breakroom" ? "PAINT_MINT" : "PAINT_WHITE";
          p.floor_mat = type == "breakroom" ? "FLOOR_LINOLEUM" : "FLOOR_CARPET_BLUE";
          enclosed.push_back({grid.add_room(type, {{zone.x0, piece[0], zone.x1, piece[1]}}, p), false});
        }
      }
      taken.push_back({zone.x0 - 1, zone.y0 - 1, zone.x1 + 1, zone.y1 + 1});
    }
  }

  std::vector<Rect> free;
  for (const Rect& r : r_subtract_all({inner}, taken))
    if (r.x1 >= r.x0 && r.y1 >= r.y0) free.push_back(r);
  RoomProps op;
  op.paint = st.paint;
  op.floor_mat = st.floor_mat;
  const std::shared_ptr<Room> open = grid.add_room(st.type, free, op);
  for (const Enclosed& e : enclosed) {
    if (e.own) continue;
    if (!grid.add_door(*e.room, open.get(), door(8))) grid.add_door(*e.room, open.get(), door(6, 1));
  }
  for (const Enclosed& e : enclosed) {
    if (!e.own) continue;
    // shops may also open to the lobby
    DoorOpts o = door(8);
    o.leaf = "glass";
    grid.add_door(*e.room, open.get(), o);
  }
  if (cores.stairs.size() != cores.stair_rooms.size()) SVX_FAIL("offices: a stair room without its stair");
  for (size_t k = 0; k < cores.stair_rooms.size(); ++k) stair_door(grid, *cores.stair_rooms[k], *cores.stairs[k], open.get());
  for (const auto& er : cores.elev_rooms) {
    DoorOpts o = door(8);
    o.kind = "elevator";
    o.place = "center";
    o.leaf = "elevator";
    grid.add_door(*er, open.get(), o);
  }
  for (const auto& rr : cores.restrooms) {
    if (!grid.add_door(*rr, open.get(), door(7))) grid.add_door(*rr, open.get(), door(6, 1));
  }
  if (kind == "lobby") {
    DoorOpts o = door(16);
    o.kind = "entrance";
    o.place = "near";
    o.near = DoorNear{(inner.x0 + inner.x1) / 2, 0};
    o.leaf = "glass";
    grid.add_door(*open, nullptr, o);
  }
  return open;
}

}  // namespace svx::city
