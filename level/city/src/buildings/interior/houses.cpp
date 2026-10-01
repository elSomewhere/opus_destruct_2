// svx_city — voxel_city buildings/interior/houses.js.
#include "buildings/interior/houses.hpp"

#include <string>
#include <utility>
#include <vector>

#include "buildings/interior/common.hpp"
#include "buildings/interior/stairs.hpp"
#include "buildings/interior/units.hpp"

namespace svx::city {

namespace {

constexpr double M = 8;
constexpr double kHallW = 10;  // HALL_W
constexpr double EXT_T = FloorGrid::EXT_T;

struct HouseFloorCtx {
  const Envelope& env;
  Rng& rng;
  const Rect& inner;
  const HouseColumns& cols;
  const Stair* stair;
  double f;
  const std::string& kind;
  const UnitStyle& style;
  double nF;
  double front_door_margin;
};

DoorOpts door(double width, double margin = js::kNaN) {
  DoorOpts o;
  o.width = width;
  if (margin == margin) o.margin = margin;
  return o;
}
DoorOpts opening_center(double width) {
  DoorOpts o;
  o.kind = "opening";
  o.width = width;
  o.place = "center";
  return o;
}

Room* find_type(const std::vector<std::shared_ptr<Room>>& rooms, const char* type) {
  for (const auto& r : rooms)
    if (r->type == type) return r.get();
  return nullptr;
}

void plan_house_floor(FloorGrid& grid, const HouseFloorCtx& c) {
  const Rect& inner = c.inner;
  const HouseColumns& cols = c.cols;
  const std::string& kind = c.kind;
  const UnitStyle& style = c.style;
  Rng& rng = c.rng;
  auto mat = [&](const std::string& type) {
    RoomProps p;
    p.paint = type == "bath" || type == "wc" ? style.tile : style.paint;
    p.floor_mat = floor_for(type, style);
    return p;
  };
  const Rect hall_rect{cols.hall[0], inner.y0, cols.hall[1], inner.y1};
  RoomProps hp = mat("hall");
  hp.floor_mat = kind == "basement" ? std::string("FLOOR_CONCRETE") : style.wood;
  const std::shared_ptr<Room> hall = grid.add_room("hall", {hall_rect}, hp);
  const bool has_stair = c.stair && c.f >= c.stair->f0 && c.f <= c.stair->f1;
  // stair column
  const double sc_x0 = cols.stair_col[0];
  const double sc_x1 = cols.stair_col[1];
  std::vector<std::shared_ptr<Room>> small_rooms;
  if (has_stair) {
    const std::shared_ptr<Room> s_room = add_stair_room(grid, *c.stair);
    s_room->paint = style.paint;
    s_room->floor_mat = style.wood;
    DoorOpts so;
    so.kind = "opening";
    so.width = 8;
    so.leaf = "none";
    stair_door(grid, *s_room, *c.stair, hall.get(), so);
    const Rect front{sc_x0, inner.y0, sc_x1, c.stair->rect.y0 - 2};
    const Rect back{sc_x0, c.stair->rect.y1 + 2, sc_x1, inner.y1};
    const std::string front_type = kind == "ground" ? "wc" : kind == "basement" ? "storage" : "closet";
    static const char* const kBackGround[] = {"laundry", "pantry"};
    const std::string back_type = kind == "ground" ? std::string(rng.pick(kBackGround)) : kind == "basement" ? "mechanical" : "bath";
    if (front.y1 - front.y0 + 1 >= 10) small_rooms.push_back(grid.add_room(front_type, {front}, mat(front_type)));
    if (back.y1 - back.y0 + 1 >= 10) small_rooms.push_back(grid.add_room(back_type, {back}, mat(back_type)));
  } else {
    const std::vector<std::string> types =
        kind == "ground" ? std::vector<std::string>{"closet", "wc", "laundry"} : std::vector<std::string>{"closet", "bath"};
    const std::vector<std::array<double, 2>> pieces = split_length(inner.y0, inner.y1, 14 * 1, 12, rng);
    for (size_t k = 0; k < pieces.size() && k < 3; ++k) {
      const std::string& type = types[small_rooms.size() % types.size()];
      small_rooms.push_back(grid.add_room(type, {{sc_x0, pieces[k][0], sc_x1, pieces[k][1]}}, mat(type)));
    }
  }
  for (const auto& r : small_rooms)
    if (!grid.add_door(*r, hall.get(), door(6, 1))) r->type = "shaft";

  // rooms column
  const double rc_x0 = cols.rooms[0];
  const double rc_x1 = cols.rooms[1];
  const double Vi = inner.y1 - inner.y0 + 1;
  using Program = std::vector<std::pair<std::string, double>>;
  Program program;
  if (kind == "basement")
    program = {{"storage", 1}, {"mechanical", 0.6}};
  else if (kind == "ground")
    program = Vi > 13 * M ? Program{{"living", 1.3}, {"dining", 0.9}, {"kitchen", 1}} : Program{{"living", 1.2}, {"kitchen", 1}};
  else if (c.f == c.nF - 1 && c.nF >= 3 && rng.chance(0.5))
    program = {{"bedroom", 1.2}, {"study", 0.8}, {"bath", 0.55}};
  else
    program = Vi > 12 * M ? Program{{"bedroom", 1.1}, {"bath", 0.55}, {"bedroom", 1}} : Program{{"bedroom", 1}, {"bedroom", 1}};
  double total = 0;
  for (const auto& p : program) total = total + p.second;
  const double avail = Vi - (static_cast<double>(program.size()) - 1);
  double v = inner.y0;
  std::vector<std::shared_ptr<Room>> rooms;
  for (size_t k = 0; k < program.size(); ++k) {
    const std::string& type = program[k].first;
    const double w = program[k].second;
    const bool last = k == program.size() - 1;
    const double d = last ? inner.y1 - v + 1 : js::max(14.0, js::round((avail * w) / total));
    rooms.push_back(grid.add_room(type, {{rc_x0, v, rc_x1, v + d - 1}}, mat(type)));
    v += d + 1;
  }
  for (const auto& r : rooms) {
    const bool small = r->type == "bath" || r->type == "wc";
    if (!grid.add_door(*r, hall.get(), door(small ? 6 : 7))) grid.add_door(*r, hall.get(), door(6, 1));
  }
  // open kitchen / dining connections
  Room* liv = find_type(rooms, "living");
  Room* din = find_type(rooms, "dining");
  Room* kit = find_type(rooms, "kitchen");
  if (liv && din) grid.add_door(*liv, din, opening_center(14));
  if (din && kit) grid.add_door(*din, kit, opening_center(12));
  if (liv && kit && !din) grid.add_door(*liv, kit, opening_center(12));

  if (kind == "ground") {
    DoorOpts fo;
    fo.kind = "entrance";
    fo.width = 8;
    fo.margin = c.front_door_margin;
    fo.place = "near";
    fo.near = DoorNear{cols.entrance_u, 0};
    fo.leaf = "wood";
    grid.add_door(*hall, nullptr, fo);
    // back door to the garden from the kitchen side
    const std::shared_ptr<Room>& back_room = rooms.back();
    DoorOpts bo;
    bo.kind = "entrance";
    bo.width = 7;
    bo.place = "near";
    bo.near = DoorNear{(rc_x0 + rc_x1) / 2, inner.y1 + 3};
    bo.leaf = "glass";
    grid.add_door(*back_room, nullptr, bo);
  }
}

// very narrow: a single room per floor, a cottage without a stair
void fallback_house(const Envelope& env, PlanBuilder& pb, const Rect& inner) {
  const std::shared_ptr<FloorGrid> grid = pb.new_grid(0);
  RoomProps p;
  p.paint = "PAINT_WHITE";
  p.floor_mat = "FLOOR_OAK";
  const std::shared_ptr<Room> r = grid->add_room("studio", {inner}, p);
  DoorOpts o;
  o.kind = "entrance";
  o.width = 8;
  o.place = "near";
  o.near = DoorNear{env.U / 2, 0};
  grid->add_door(*r, nullptr, o);
  pb.add_floor(0, grid, "ground");
}

}  // namespace

HouseColumns house_columns(double U, bool mirror) {
  const double i0 = EXT_T;
  const double i1 = U - 1 - EXT_T;
  const double sw = 2 * kHouseLane + 1;
  std::array<double, 2> stair_col{i0, i0 + sw - 1};
  std::array<double, 2> hall{stair_col[1] + 2, stair_col[1] + 1 + kHallW};
  std::array<double, 2> rooms{hall[1] + 2, i1};
  if (mirror) {
    auto flip = [&](const std::array<double, 2>& ab) { return std::array<double, 2>{i0 + i1 - ab[1], i0 + i1 - ab[0]}; };
    stair_col = flip(stair_col);
    hall = flip(hall);
    rooms = flip(rooms);
  }
  HouseColumns c;
  c.stair_col = stair_col;
  c.hall = hall;
  c.rooms = rooms;
  c.entrance_u = js::round((hall[0] + hall[1]) / 2);
  return c;
}

void plan_house(const Envelope& env, Rng& rng, PlanBuilder& pb, double front_door_margin) {
  const double nF = env.floors;
  const std::vector<Rect>& tier0 = tier_rects(env, 0);
  if (tier0.empty()) SVX_FAIL("houses: an envelope without a ground floor");
  const Rect rect = tier0[0];
  const Rect inner{rect.x0 + EXT_T, rect.y0 + EXT_T, rect.x1 - EXT_T, rect.y1 - EXT_T};
  const HouseColumns cols = house_columns(env.U, env.mirror);
  if (cols.rooms[1] - cols.rooms[0] + 1 < 3 * M) return fallback_house(env, pb, inner);
  const double f_bottom = -env.basements;
  const bool multi = nF + env.basements > 1;
  std::shared_ptr<Stair> stair;
  if (multi) {
    double max_h = js::truthy(env.basements) ? env.basement_h : 0;
    for (double h : env.story_h) max_h = js::max(max_h, h);
    const StairDims sd = stair_dims(max_h, kHouseLane, kHouseLanding);
    const double Vi = inner.y1 - inner.y0 + 1;
    const double sv0 = inner.y0 + js::max(0.0, js::round((Vi - sd.L) / 2));
    MakeStairOpts so;
    so.rect = {cols.stair_col[0], sv0, cols.stair_col[1], js::min(inner.y1, sv0 + sd.L - 1)};
    so.axis = 'v';
    so.dir = 1;
    so.lane_low = !env.mirror;
    so.lane = kHouseLane;
    so.landing = kHouseLanding;
    so.f0 = f_bottom;
    so.f1 = nF - 1;
    so.open = true;
    stair = pb.add_stair(make_stair(so));
  }
  const UnitStyle style = pick_unit_style(rng);
  for (double f = f_bottom; f < nF; f += 1) {
    const std::shared_ptr<FloorGrid> grid = pb.new_grid(f);
    const std::string kind = f < 0 ? "basement" : f == 0 ? "ground" : "upper";
    Rng frng = rng.fork(js::cat("hf", f));
    plan_house_floor(*grid, HouseFloorCtx{env, frng, inner, cols, stair.get(), f, kind, style, nF, front_door_margin});
    pb.add_floor(f, grid, kind);
  }
}

}  // namespace svx::city
