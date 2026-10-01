// svx_city — voxel_city buildings/interior/apartments.js.
#include "buildings/interior/apartments.hpp"

#include <array>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "buildings/interior/common.hpp"
#include "buildings/interior/offices.hpp"
#include "buildings/interior/stairs.hpp"
#include "buildings/interior/units.hpp"
#include "buildings/styles.hpp"
#include "svx/base/types.hpp"

namespace svx::city {

namespace {

constexpr double M = 8;
constexpr double EXT_T = FloorGrid::EXT_T;

using Interval = std::array<double, 2>;

DoorOpts door(double width, double margin = js::kNaN) {
  DoorOpts o;
  o.width = width;
  if (margin == margin) o.margin = margin;
  return o;
}
RoomProps finish(const char* paint, const char* floor_mat) {
  RoomProps p;
  p.paint = paint;
  p.floor_mat = floor_mat;
  return p;
}

// buildings/wings.js nearWing(env, r, pad = 16): does a canonical rect of a building (a balcony's
// reach) come within `pad` of one of its wings? (It reads only the wings' canonical rects.)
bool near_wing(const Envelope& env, const Rect& r, double pad = 16) {
  for (const Wing& w : env.wings)
    if (w.canon.x0 - pad <= r.x1 && w.canon.x1 + pad >= r.x0 && w.canon.y0 - pad <= r.y1 && w.canon.y1 + pad >= r.y0) return true;
  return false;
}

// ---------------------------------------------------------------- cores

struct AptStair {
  Rect rect{};
  std::shared_ptr<Stair> stair{};
};
struct AptElevator {
  Rect rect{};
  std::optional<Rect> shaft{};
  Elevator elev{};
};
struct AptSection {
  double a0 = 0, a1 = 0, c0 = 0, c1 = 0;
  Rect hall{};
  bool single = false;
  double stair_index = 0;
  std::shared_ptr<Room> hall_room{};  // (each floor's, set as the floor is planned)
};
struct AptLayout {
  std::string mode{};  // "corridor" | "sections"
  Rect corridor{};     // (corridor mode)
  std::vector<AptStair> stairs{};
  std::vector<AptElevator> elevators{};
  std::vector<Interval> core_intervals{};  // (corridor mode)
  std::vector<AptSection> sections{};      // (sections mode)
};

struct Ctx {
  const Envelope& env;
  Rng& rng;
  PlanBuilder& pb;
  const Rect& inner;
  AptLayout& layout;
  double f;
};

std::optional<AptLayout> corridor_cores(const Rect& inner, const StairDims& sd, bool lift, Rng& rng) {
  const double cw = 13;
  const double Vi = inner.y1 - inner.y0 + 1;
  double cv0 = inner.y0 + std::floor((Vi - cw) / 2);
  // keep the back zone deep enough for the stair run
  const double max_cv1 = inner.y1 - sd.L - 1;
  if (cv0 + cw - 1 > max_cv1) cv0 = max_cv1 - cw + 1;
  const double cv1 = cv0 + cw - 1;
  const double Ui = inner.x1 - inner.x0 + 1;
  const double core_w = lift ? sd.W + 1 + 16 : sd.W;
  const double n_cores = Ui > 44 * M ? 2 : 1;
  AptLayout layout;
  layout.mode = "corridor";
  for (double k = 0; k < n_cores; k += 1) {
    const double center = n_cores == 1 ? inner.x0 + Ui * (0.5 + rng.float_(-0.12, 0.12)) : inner.x0 + Ui * (k == 0 ? 0.24 : 0.76);
    double c0 = js::round(center - core_w / 2);
    c0 = js::max(inner.x0 + 5 * M, js::min(inner.x1 - 5 * M - core_w, c0));
    const Rect stair_rect{c0, cv1 + 2, c0 + sd.W - 1, inner.y1};
    layout.stairs.push_back({stair_rect, nullptr});
    if (lift) {
      const double ex0 = c0 + sd.W + 1;
      AptElevator e;
      e.rect = {ex0, cv1 + 2, ex0 + 15, cv1 + 2 + 15};
      e.shaft = Rect{ex0, cv1 + 2 + 17, ex0 + 15, inner.y1};
      layout.elevators.push_back(e);
    }
    layout.core_intervals.push_back({c0, c0 + core_w - 1});
  }
  layout.corridor = {inner.x0, cv0, inner.x1, cv1};
  return layout;
}

std::optional<AptLayout> section_cores(const Rect& inner, const StairDims& sd, bool lift, Rng& rng) {
  const double Ui = inner.x1 - inner.x0 + 1;
  const double Vi = inner.y1 - inner.y0 + 1;
  if (Vi < sd.L + 1 + 12) return std::nullopt;
  const double core_w = lift ? sd.W + 1 + 16 : sd.W;
  const double n_sec = js::max(1.0, js::round(Ui / (19 * M)));
  const std::vector<Interval> secs = split_length(inner.x0, inner.x1, std::floor((Ui - (n_sec - 1)) / n_sec), 8 * M, rng, 0.1);
  AptLayout layout;
  layout.mode = "sections";
  for (const Interval& s : secs) {
    const double a0 = s[0], a1 = s[1];
    const double w = a1 - a0 + 1;
    double c0;
    bool single = false;
    if (w >= core_w + 2 + 2 * 5 * M) {
      const double mid = (a0 + a1 - core_w) / 2;
      c0 = js::round(mid + rng.float_(-0.08, 0.08) * w);
    } else {
      single = true;
      c0 = rng.chance(0.5) ? a0 : a1 - core_w + 1;
    }
    c0 = js::max(a0, js::min(a1 - core_w + 1, c0));
    const Rect stair_rect{c0, inner.y1 - sd.L + 1, c0 + sd.W - 1, inner.y1};
    layout.stairs.push_back({stair_rect, nullptr});
    if (lift) {
      const double ex0 = c0 + sd.W + 1;
      AptElevator el;
      el.rect = {ex0, stair_rect.y0, ex0 + 15, stair_rect.y0 + 15};
      el.shaft = Rect{ex0, stair_rect.y0 + 17, ex0 + 15, inner.y1};
      layout.elevators.push_back(el);
    }
    AptSection sec;
    sec.a0 = a0;
    sec.a1 = a1;
    sec.c0 = c0;
    sec.c1 = c0 + core_w - 1;
    sec.hall = {c0, inner.y0, c0 + core_w - 1, stair_rect.y0 - 2};
    sec.single = single;
    sec.stair_index = static_cast<double>(layout.stairs.size()) - 1;
    layout.sections.push_back(sec);
  }
  return layout;
}

struct Cores {
  std::vector<std::shared_ptr<Room>> stair_rooms{}, elev_rooms{};
};

Cores paint_cores(FloorGrid& grid, const AptLayout& layout, bool with_elevators = true) {
  Cores c;
  for (const AptStair& s : layout.stairs) c.stair_rooms.push_back(add_stair_room(grid, *s.stair));
  if (with_elevators) {
    for (const AptElevator& e : layout.elevators) {
      RoomProps p;
      p.elevator = e.elev.id;
      c.elev_rooms.push_back(grid.add_room("elevator", {e.rect}, p));
      if (e.shaft) grid.add_room("shaft", {*e.shaft});
    }
  }
  return c;
}

void connect_cores(FloorGrid& grid, const AptLayout& layout, const Cores& cores, const std::function<const Room*(size_t k)>& circ_for) {
  for (size_t k = 0; k < layout.stairs.size(); ++k) {
    const Room* circ = circ_for(k);
    if (circ) stair_door(grid, *cores.stair_rooms[k], *layout.stairs[k].stair, circ);
  }
  for (size_t k = 0; k < layout.elevators.size(); ++k) {
    const Room* circ = circ_for(k);
    if (circ) {
      DoorOpts o = door(8);
      o.kind = "elevator";
      o.place = "center";
      o.leaf = "elevator";
      grid.add_door(*cores.elev_rooms[k], circ, o);
    }
  }
}

// planOpenFloor's cores for an apartment core: its stair rooms (the layout's stairs), elevator rooms
// and no restrooms
OpenFloorCores open_cores(const AptLayout& layout, const Cores& cores) {
  OpenFloorCores c;
  c.stair_rooms = cores.stair_rooms;
  for (const AptStair& s : layout.stairs) c.stairs.push_back(s.stair.get());
  c.elev_rooms = cores.elev_rooms;
  return c;
}

// ---------------------------------------------------------------- units

void place_unit(FloorGrid& grid, Rng& rng, const Rect& rect, char entry, const Room& circ, const std::string& unit_id, const std::optional<DoorNear>& near) {
  const std::vector<char> facades = facade_sides_of(grid, rect);
  UnitOpts o;
  o.rng = &rng;
  o.unit = unit_id;
  o.entry_near = near;
  if (plan_unit(grid, rect, entry, circ, facades, o)) return;
  // fallback: a single room that is guaranteed reachable
  RoomProps p = finish("PAINT_WHITE", "FLOOR_OAK");
  p.unit = unit_id;
  const std::shared_ptr<Room> r = grid.add_room("studio", {rect}, p);
  DoorOpts d1 = door(8);
  d1.kind = "entry";
  if (!grid.add_door(*r, &circ, d1)) {
    DoorOpts d2 = door(6, 1);
    d2.kind = "entry";
    grid.add_door(*r, &circ, d2);
  }
}

void corridor_units(FloorGrid& grid, const Ctx& ctx, const Room& corr, bool front, bool back, const std::vector<Interval>& skip) {
  const Rect& inner = ctx.inner;
  const AptLayout& layout = ctx.layout;
  Rng& rng = ctx.rng;
  struct Zone {
    Rect rect;
    char entry;
    std::vector<Interval> blocked;
  };
  std::vector<Zone> zones;
  if (front) zones.push_back({{inner.x0, inner.y0, inner.x1, layout.corridor.y0 - 2}, 'S', skip});
  if (back) zones.push_back({{inner.x0, layout.corridor.y1 + 2, inner.x1, inner.y1}, 'N', layout.core_intervals});
  double unit_no = 0;
  for (const Zone& z : zones) {
    for (const Interval& iv : free_intervals(z.rect.x0, z.rect.x1, z.blocked)) {
      const double a0 = iv[0], a1 = iv[1];
      if (a1 - a0 + 1 < 3 * M) {
        const std::shared_ptr<Room> r = grid.add_room("storage", {{a0, z.rect.y0, a1, z.rect.y1}}, finish("PAINT_GRAY", "FLOOR_CONCRETE"));
        if (!grid.add_door(*r, &corr, door(6, 1))) r->type = "shaft";
        continue;
      }
      const double target = js::round(rng.float_(7.5, 10.5) * M);
      for (const Interval& pc : split_length(a0, a1, target, 5 * M, rng)) {
        const Rect rect{pc[0], z.rect.y0, pc[1], z.rect.y1};
        const std::string id = js::cat(z.entry, unit_no);
        unit_no += 1;
        place_unit(grid, rng, rect, z.entry, corr, id, std::nullopt);
      }
    }
  }
}

struct SectionZone {
  Rect rect;
  char entry;
};
std::vector<SectionZone> section_zones(const AptSection& sec, const Rect& inner) {
  std::vector<SectionZone> out;
  if (sec.c0 - 2 >= sec.a0 + 3 * M) out.push_back({{sec.a0, inner.y0, sec.c0 - 2, inner.y1}, 'E'});
  if (sec.a1 >= sec.c1 + 2 + 3 * M) out.push_back({{sec.c1 + 2, inner.y0, sec.a1, inner.y1}, 'W'});
  return out;
}

void section_units(FloorGrid& grid, const Ctx& ctx) {
  double unit_no = 0;
  for (const AptSection& sec : ctx.layout.sections) {
    for (const SectionZone& z : section_zones(sec, ctx.inner)) {
      const std::string id = js::cat("s", unit_no);
      unit_no += 1;
      place_unit(grid, ctx.rng, z.rect, z.entry, *sec.hall_room, id, DoorNear{(sec.hall.x0 + sec.hall.x1) / 2, (sec.hall.y0 + sec.hall.y1) / 2});
    }
  }
}

// ---------------------------------------------------------------- retail / service

void shops_in_zone(FloorGrid& grid, const Ctx& ctx, const Rect& zone, const std::vector<Rect>& blocked_rects, const Room* lobby) {
  std::vector<Interval> blocked;
  for (const Rect& r : blocked_rects) blocked.push_back({r.x0, r.x1});
  for (const Interval& iv : free_intervals(zone.x0, zone.x1, blocked)) {
    const double a0 = iv[0], a1 = iv[1];
    if (a1 - a0 + 1 < 3 * M) {
      const std::shared_ptr<Room> st = grid.add_room("storage", {{a0, zone.y0, a1, zone.y1}}, finish("PAINT_GRAY", "FLOOR_CONCRETE"));
      DoorOpts out = door(7, 1);
      out.kind = "entrance";
      out.leaf = "metal";
      if (!(lobby && grid.add_door(*st, lobby, door(6, 1))) && !grid.add_door(*st, nullptr, out)) st->type = "shaft";
      continue;
    }
    const double target = js::round(ctx.rng.float_(6, 11) * M);
    for (const Interval& pc : split_length(a0, a1, target, 4 * M, ctx.rng)) shop_with_backroom(grid, &ctx.env, ctx.rng, {pc[0], zone.y0, pc[1], zone.y1}, nullptr);
  }
}

void service_rooms(FloorGrid& grid, const Ctx& ctx, const Room& corr, const Rect& zone, const std::vector<Interval>& blocked) {
  static const char* const kTypes[] = {"bike", "storage", "laundry", "mechanical", "storage", "trash"};
  double k = 0;
  for (const Interval& iv : free_intervals(zone.x0, zone.x1, blocked)) {
    for (const Interval& pc : split_length(iv[0], iv[1], 5 * M, 20, ctx.rng)) {
      const std::shared_ptr<Room> r = grid.add_room(kTypes[static_cast<size_t>(std::fmod(k, 6))], {{pc[0], zone.y0, pc[1], zone.y1}}, finish("PAINT_GRAY", "FLOOR_CONCRETE"));
      // (a sliver too narrow for a door, beside a stair in a narrow house, is a service shaft)
      DoorOpts o = door(7);
      o.leaf = "metal";
      if (!grid.add_door(*r, &corr, o) && !grid.add_door(*r, &corr, door(6, 1))) r->type = "shaft";
      k += 1;
    }
  }
}

// Balcony doors: living rooms on a facade get a glass door onto a balcony (the slab and railing are
// emitted by fixtures). The chance comes from the building's style.
void add_balconies(FloorGrid& grid, const Envelope& env, Rng& rng) {
  const Style* style = style_registry().maybe(env.style);
  const double p = style ? style->balcony : 0;
  const bool want = Rng(std::floor(rng.next() * 2147483648.0)).chance(js::min(0.95, p * 1.6));
  if (!want) return;
  for (const auto& room : grid.rooms) {
    if (room->type != "living" && room->type != "studio") continue;
    if (room->rects.empty()) SVX_FAIL("apartments: a living room without rects");
    const Rect r = room->rects[0];
    const bool front = r.y0 < 6;
    // (the angled world's wings, S5: no balcony where one is cast into the facade)
    if (!env.wings.empty() && near_wing(env, {r.x0, front ? -12 : env.V, r.x1, front ? 0 : env.V + 12})) continue;
    DoorOpts o = door(8);
    o.kind = "balcony";
    o.leaf = "glass";
    o.place = "near";
    o.near = DoorNear{(r.x0 + r.x1) / 2, front ? -12 : env.V + 12};
    grid.add_door(*room, nullptr, o);
  }
}

// ---------------------------------------------------------------- floors

std::shared_ptr<FloorGrid> residential_floor(const Ctx& ctx) {
  AptLayout& layout = ctx.layout;
  const std::shared_ptr<FloorGrid> grid = ctx.pb.new_grid(ctx.f);
  const Cores cores = paint_cores(*grid, layout);
  if (layout.mode == "corridor") {
    const std::shared_ptr<Room> corr = grid->add_room("corridor", {layout.corridor}, finish("PAINT_CREAM", "FLOOR_CARPET_RED"));
    connect_cores(*grid, layout, cores, [&](size_t) { return corr.get(); });
    corridor_units(*grid, ctx, *corr, true, true, {});
  } else {
    for (AptSection& sec : layout.sections) sec.hall_room = grid->add_room("landing", {sec.hall}, finish("PAINT_WHITE", "FLOOR_TERRAZZO"));
    // stairs[k] and elevators[k] both belong to sections[k]
    connect_cores(*grid, layout, cores, [&](size_t k) { return layout.sections.at(k).hall_room.get(); });
    section_units(*grid, ctx);
  }
  return grid;
}

std::shared_ptr<FloorGrid> ground_floor(const Ctx& ctx) {
  const Envelope& env = ctx.env;
  AptLayout& layout = ctx.layout;
  const Rect& inner = ctx.inner;
  const std::shared_ptr<FloorGrid> grid = ctx.pb.new_grid(0);
  const Cores cores = paint_cores(*grid, layout);
  const std::string program = env.archetype == "tower" ? std::string("officeLobby") : env.program.ground;
  if (env.archetype == "tower") {
    // lobbyOpenFloor
    plan_open_floor(*grid, env, ctx.rng, open_cores(layout, cores), "lobby");
    return grid;
  }
  if (layout.mode == "corridor") {
    const std::shared_ptr<Room> corr = grid->add_room("corridor", {layout.corridor}, finish("PAINT_CREAM", "FLOOR_TERRAZZO"));
    connect_cores(*grid, layout, cores, [&](size_t) { return corr.get(); });
    // lobby in front of the first core
    const double c0 = layout.core_intervals[0][0], c1 = layout.core_intervals[0][1];
    const Rect lob{js::max(inner.x0, c0 - 12), inner.y0, js::min(inner.x1, c1 + 12), layout.corridor.y0 - 2};
    const std::shared_ptr<Room> lobby = grid->add_room("lobby", {lob}, finish("PAINT_WHITE", "FLOOR_MARBLE"));
    {
      DoorOpts o = door(js::min(24.0, lob.x1 - lob.x0 - 4));
      o.kind = "opening";
      o.place = "center";
      grid->add_door(*lobby, corr.get(), o);
    }
    {
      DoorOpts o = door(12);
      o.kind = "entrance";
      o.place = "near";
      o.near = DoorNear{(lob.x0 + lob.x1) / 2, 0};
      o.leaf = "glass";
      grid->add_door(*lobby, nullptr, o);
    }
    if (program == "retail") {
      shops_in_zone(*grid, ctx, {inner.x0, inner.y0, inner.x1, layout.corridor.y0 - 2}, {lob}, lobby.get());
      service_rooms(*grid, ctx, *corr, {inner.x0, layout.corridor.y1 + 2, inner.x1, inner.y1}, layout.core_intervals);
    } else {
      corridor_units(*grid, ctx, *corr, true, true, {{lob.x0, lob.x1}});
    }
    return grid;
  }
  // sections: the stair hall is the entrance lobby of each section
  for (AptSection& sec : layout.sections) {
    sec.hall_room = grid->add_room("lobby", {sec.hall}, finish("PAINT_WHITE", "FLOOR_TERRAZZO"));
    DoorOpts o = door(10);
    o.kind = "entrance";
    o.place = "near";
    o.near = DoorNear{(sec.hall.x0 + sec.hall.x1) / 2, 0};
    o.leaf = "wood";
    grid->add_door(*sec.hall_room, nullptr, o);
  }
  // (layout.sections[Math.min(k, layout.sections.length - 1)])
  const size_t n_sec = layout.sections.size();
  connect_cores(*grid, layout, cores, [&](size_t k) { return layout.sections[k < n_sec - 1 ? k : n_sec - 1].hall_room.get(); });
  if (program == "retail") {
    for (const AptSection& sec : layout.sections)
      for (const SectionZone& z : section_zones(sec, inner)) shop_with_backroom(*grid, &env, ctx.rng, z.rect, sec.hall_room.get());
  } else {
    section_units(*grid, ctx);
  }
  return grid;
}

std::shared_ptr<FloorGrid> basement_floor(const Ctx& ctx) {
  AptLayout& layout = ctx.layout;
  const Rect& inner = ctx.inner;
  const std::shared_ptr<FloorGrid> grid = ctx.pb.new_grid(ctx.f);
  const Cores cores = paint_cores(*grid, layout);
  if (layout.mode == "corridor") {
    const std::shared_ptr<Room> corr = grid->add_room("corridor", {layout.corridor}, finish("PAINT_GRAY", "FLOOR_CONCRETE"));
    connect_cores(*grid, layout, cores, [&](size_t) { return corr.get(); });
    struct Zone {
      Rect rect;
      std::vector<Interval> blocked;
    };
    const Zone zones[2] = {{{inner.x0, inner.y0, inner.x1, layout.corridor.y0 - 2}, {}}, {{inner.x0, layout.corridor.y1 + 2, inner.x1, inner.y1}, layout.core_intervals}};
    double k = 0;
    for (const Zone& z : zones) {
      for (const Interval& iv : free_intervals(z.rect.x0, z.rect.x1, z.blocked)) {
        for (const Interval& pc : split_length(iv[0], iv[1], 4 * M, 20, ctx.rng)) {
          const double m = std::fmod(k, 7);
          const char* type = m == 3 ? "mechanical" : m == 5 ? "laundry" : "storage";
          const std::shared_ptr<Room> r = grid->add_room(type, {{pc[0], z.rect.y0, pc[1], z.rect.y1}}, finish("CONCRETE", "FLOOR_CONCRETE"));
          // (a sliver beside a core too narrow for a door is a service shaft)
          DoorOpts o = door(7);
          o.kind = "interior";
          o.leaf = "metal";
          if (!grid->add_door(*r, corr.get(), o) && !grid->add_door(*r, corr.get(), door(6, 1))) r->type = "shaft";
          k += 1;
        }
      }
    }
    return grid;
  }
  for (AptSection& sec : layout.sections) sec.hall_room = grid->add_room("hall", {sec.hall}, finish("PAINT_GRAY", "FLOOR_CONCRETE"));
  // (layout.sections[Math.min(k, layout.sections.length - 1)])
  const size_t n_sec = layout.sections.size();
  connect_cores(*grid, layout, cores, [&](size_t k) { return layout.sections[k < n_sec - 1 ? k : n_sec - 1].hall_room.get(); });
  for (const AptSection& sec : layout.sections) {
    for (const SectionZone& z : section_zones(sec, inner)) {
      const std::shared_ptr<Room> r = grid->add_room("storage", {z.rect}, finish("CONCRETE", "FLOOR_CONCRETE"));
      DoorOpts o = door(7);
      o.leaf = "metal";
      grid->add_door(*r, sec.hall_room.get(), o);
    }
  }
  return grid;
}

std::shared_ptr<FloorGrid> podium_floor(const Ctx& ctx) {
  const std::shared_ptr<FloorGrid> grid = ctx.pb.new_grid(ctx.f);
  const Cores cores = paint_cores(*grid, ctx.layout);
  plan_open_floor(*grid, ctx.env, ctx.rng, open_cores(ctx.layout, cores), "office");
  return grid;
}

// SHOP_KINDS: ground-floor tenants by town flavor (shops, cafés, bars, services); each kind is a
// room type with its own furnishing (furnish.js, civicRules.js)
using Weighted = std::vector<std::pair<std::string, double>>;
const Weighted& shop_kinds(const std::string& group) {
  static const Weighted kDefault = {{"retail", 3},    {"cafe", 2},        {"restaurant", 1.2}, {"bakery", 1},   {"pharmacy", 0.7}, {"bookshop", 0.6},
                                    {"clothing", 1},  {"florist", 0.5},   {"hardware", 0.4},   {"barber", 0.6}, {"pub", 1},        {"bank", 0.5},
                                    {"laundromat", 0.3}, {"grocery", 1}, {"gallery", 0.35},   {"venueFloor", 0.25}, {"kiosk", 0.3}};
  static const Weighted kNordic = {{"retail", 2},     {"cafe", 2},        {"bakery", 1.5},   {"fishmonger", 0.8}, {"pharmacy", 0.7},  {"bookshop", 0.6},
                                   {"clothing", 0.8}, {"florist", 0.5},   {"hardware", 0.5}, {"barber", 0.5},     {"pub", 0.8},       {"bank", 0.4},
                                   {"grocery", 1.2},  {"souvenir", 0.5},  {"gallery", 0.3},  {"venueFloor", 0.2}, {"restaurant", 0.8}};
  static const Weighted kSoviet = {{"produkty", 3}, {"pharmacy", 1.2}, {"kiosk", 1},    {"bakery", 1},   {"hardware", 0.8}, {"barber", 0.8},
                                   {"clothing", 0.6}, {"cafe", 0.8},    {"bookshop", 0.4}, {"bank", 0.4}, {"butcher", 0.6},  {"laundromat", 0.3}};
  if (group == "nordic") return kNordic;
  if (group == "soviet") return kSoviet;
  return kDefault;
}
// FLAVOR_SHOPS[flavor] ?? "default"
std::string flavor_shops(const std::string& flavor) {
  if (flavor == "nordic" || flavor == "nordicHarbour" || flavor == "harbourTown") return "nordic";
  if (flavor == "nordicBleak" || flavor == "soviet") return "soviet";
  return "default";
}

}  // namespace

void plan_apartment_building(const Envelope& env, Rng& rng, PlanBuilder& pb) {
  const double nF = env.floors;
  const std::vector<Rect>& tops = tier_rects(env, nF - 1);
  if (tops.empty()) SVX_FAIL("apartments: an envelope without its top floor");
  const Rect top = tops[0];
  const Rect inner{top.x0 + EXT_T, top.y0 + EXT_T, top.x1 - EXT_T, top.y1 - EXT_T};
  const double Vi = inner.y1 - inner.y0 + 1;
  double max_h = js::truthy(env.basements) ? env.basement_h : 0;
  for (double h : env.story_h) max_h = js::max(max_h, h);
  const StairDims sd = stair_dims(max_h);
  const bool lift = nF > 5;
  const bool corridor = Vi >= sd.L + 13 + 2 + 5 * M && Vi >= 118;
  std::optional<AptLayout> made = corridor ? corridor_cores(inner, sd, lift, rng) : section_cores(inner, sd, lift, rng);
  if (!made) return;
  AptLayout& layout = *made;

  // stairs / elevators span basements .. roof (flat roofs: up to a bulkhead; under a pitched roof
  // the stair ends on the top floor)
  const double f_bottom = -env.basements;
  const double f_stair_top = env.roof.type == "flat" ? nF : nF - 1;
  for (AptStair& s : layout.stairs) {
    MakeStairOpts so;
    so.rect = s.rect;
    so.axis = 'v';
    so.dir = 1;
    so.lane_low = rng.chance(0.5);
    so.f0 = f_bottom;
    so.f1 = f_stair_top;
    s.stair = pb.add_stair(make_stair(so));
  }
  for (AptElevator& e : layout.elevators) {
    Elevator el;
    el.rect = e.rect;
    el.f0 = f_bottom;
    el.f1 = nF - 1;
    el.door_side = 'N';
    e.elev = pb.add_elevator(el);
  }

  const double first_apt = env.archetype == "tower" ? env.podium_floors : 1;
  // (ctxBase's unitStyle: drawn, never read)
  pick_unit_style(rng);

  // basements
  for (double f = f_bottom; f < 0; f += 1) pb.add_floor(f, basement_floor(Ctx{env, rng, pb, inner, layout, f}), "basement");
  // ground
  pb.add_floor(0, ground_floor(Ctx{env, rng, pb, inner, layout, 0}), "ground");
  // podium (towers): open office floors around the same cores
  std::shared_ptr<FloorGrid> podium_grid;
  for (double f = 1; f < first_apt; f += 1) {
    if (!podium_grid) podium_grid = podium_floor(Ctx{env, rng, pb, inner, layout, f});
    pb.add_floor(f, podium_grid, "office");
  }
  // typical residential floors share one grid
  std::shared_ptr<FloorGrid> typical;
  for (double f = first_apt; f < nF; f += 1) {
    if (!typical) {
      Rng trng = rng.fork("typical");
      typical = residential_floor(Ctx{env, trng, pb, inner, layout, f});
      Rng brng = rng.fork("balconies");
      add_balconies(*typical, env, brng);
    }
    pb.add_floor(f, typical, "residential");
  }
}

std::shared_ptr<Room> shop_with_backroom(FloorGrid& grid, const Envelope* env, Rng& rng, const Rect& rect, const Room* back_circ, const std::string* kind_override) {
  const double depth = rect.y1 - rect.y0 + 1;
  const double width = rect.x1 - rect.x0 + 1;
  // String(ctx.env?.flavor ?? "").replace(/Village$/, "")
  std::string flavor = env ? env->flavor : std::string();
  static const std::string kVillage = "Village";
  if (flavor.size() >= kVillage.size() && flavor.compare(flavor.size() - kVillage.size(), kVillage.size(), kVillage) == 0) flavor.resize(flavor.size() - kVillage.size());
  const std::string kind = kind_override ? *kind_override : rng.weighted(shop_kinds(flavor_shops(flavor)));
  const bool with_back = depth >= 7 * M && width >= 4 * M;
  const double sales_end = with_back ? rect.y0 + js::round(depth * 0.66) : rect.y1;
  static const char* const kPaints[] = {"PAINT_WHITE", "PAINT_GRAY", "PAINT_TERRACOTTA", "PAINT_SAGE"};
  static const char* const kFloors[] = {"FLOOR_TILE_GRAY", "FLOOR_CONCRETE", "FLOOR_TERRAZZO", "FLOOR_OAK"};
  RoomProps sp;
  sp.paint = rng.pick(kPaints);
  sp.floor_mat = rng.pick(kFloors);
  sp.shop = true;
  const std::shared_ptr<Room> shop = grid.add_room(kind, {{rect.x0, rect.y0, rect.x1, sales_end}}, sp);
  DoorOpts front = door(10);
  front.kind = "shopfront";
  front.place = "center";
  front.leaf = "glass";
  if (!grid.add_door(*shop, nullptr, front)) {
    DoorOpts o = door(8);
    o.kind = "shopfront";
    o.place = "auto";
    o.leaf = "glass";
    grid.add_door(*shop, nullptr, o);
  }
  if (with_back) {
    const Rect back{rect.x0, sales_end + 2, rect.x1, rect.y1};
    if (width >= 5 * M) {
      const double wc_w = 12;
      const std::shared_ptr<Room> wc = grid.add_room("wc", {{back.x0, back.y0, back.x0 + wc_w - 1, back.y1}}, finish("WALL_TILE_WHITE", "FLOOR_TILE_WHITE"));
      const std::shared_ptr<Room> br = grid.add_room("backroom", {{back.x0 + wc_w + 1, back.y0, back.x1, back.y1}}, finish("PAINT_GRAY", "FLOOR_CONCRETE"));
      if (!grid.add_door(*br, shop.get(), door(7))) grid.add_door(*br, shop.get(), door(6, 1));
      if (!grid.add_door(*wc, br.get(), door(6, 1))) grid.add_door(*wc, shop.get(), door(6, 1));
      if (back_circ) {
        DoorOpts o = door(7);
        o.leaf = "metal";
        grid.add_door(*br, back_circ, o);
      }
    } else {
      const std::shared_ptr<Room> br = grid.add_room("backroom", {back}, finish("PAINT_GRAY", "FLOOR_CONCRETE"));
      grid.add_door(*br, shop.get(), door(6, 1));
    }
  }
  return shop;
}

}  // namespace svx::city
