// svx_city — voxel_city buildings/interior/cabins.js.
#include "buildings/interior/cabins.hpp"

#include <array>
#include <memory>

#include "buildings/interior/units.hpp"
#include "core/math.hpp"

namespace svx::city {

namespace {

constexpr double EXT_T = FloorGrid::EXT_T;

using Range = std::array<double, 2>;
Rect rect_of(const Range& x, const Range& y) { return {x[0], y[0], x[1], y[1]}; }

DoorOpts door(double width, double margin = js::kNaN, const char* place = nullptr) {
  DoorOpts o;
  o.width = width;
  if (margin == margin) o.margin = margin;
  if (place) o.place = place;
  return o;
}

}  // namespace

CabinLayout cabin_layout(double U, double V, bool mirror) {
  const double i0 = EXT_T;
  const double i1 = U - 1 - EXT_T;
  const double j0 = EXT_T;
  const double j1 = V - 1 - EXT_T;
  const double Ui = i1 - i0 + 1;
  const double Vi = j1 - j0 + 1;
  const double side = clamp(js::round(Ui * 0.42), 20, 26);
  const Range main_x{i0 + side + 1, i1};
  const double main_w = main_x[1] - main_x[0] + 1;
  const Range side_x{i0, i0 + side - 1};
  const double front = clamp(js::round(Vi * 0.24), 14, 17);
  const Range front_y{j0, j0 + front - 1};
  const Range rest_y{j0 + front + 1, j1};
  const double rest_len = rest_y[1] - rest_y[0] + 1;
  CabinLayout out;
  std::vector<CabinRoom>& rooms = out.rooms;
  rooms.push_back({"bath", "bath", rect_of(side_x, front_y)});
  // bedrooms down the side strip
  if (rest_len >= 2 * 20 + 1) {
    const double mid = rest_y[0] + std::floor((rest_len - 1) / 2);
    rooms.push_back({"bed1", "bedroom", rect_of(side_x, {rest_y[0], mid - 1})});
    rooms.push_back({"bed2", "bedroom", rect_of(side_x, {mid + 1, rest_y[1]})});
  } else {
    rooms.push_back({"bed1", "bedroom", rect_of(side_x, rest_y)});
  }
  // main strip: entry (+ kitchen) at the front, living room behind
  if (main_w >= 12 + 1 + 20) {
    const double ew = clamp(js::round(main_w * 0.4), 12, 16);
    rooms.push_back({"entry", "foyer", rect_of({main_x[0], main_x[0] + ew - 1}, front_y)});
    rooms.push_back({"kitchen", "kitchen", rect_of({main_x[0] + ew + 1, main_x[1]}, front_y)});
    rooms.push_back({"living", "living", rect_of(main_x, rest_y)});
  } else {
    const double kd = clamp(js::round(rest_len * 0.4), 14, 22);
    rooms.push_back({"entry", "foyer", rect_of(main_x, front_y)});
    rooms.push_back({"living", "living", rect_of(main_x, {rest_y[0], rest_y[1] - kd - 1})});
    rooms.push_back({"kitchen", "kitchen", rect_of(main_x, {rest_y[1] - kd + 1, rest_y[1]})});
  }
  if (mirror)
    for (CabinRoom& r : rooms) {
      const double x0 = i0 + i1 - r.rect.x1;
      const double x1 = i0 + i1 - r.rect.x0;
      r.rect.x0 = x0;
      r.rect.x1 = x1;
    }
  const CabinRoom* entry = nullptr;
  for (const CabinRoom& r : rooms)
    if (r.key == "entry") {
      entry = &r;
      break;
    }
  out.entrance_u = js::round((entry->rect.x0 + entry->rect.x1) / 2);
  return out;
}

void plan_cabin(const Envelope& env, Rng& rng, PlanBuilder& pb) {
  const CabinLayout layout = cabin_layout(env.U, env.V, env.mirror);
  const UnitStyle style = pick_unit_style(rng);
  const bool panelled = rng.chance(0.75);
  auto mat = [&](const std::string& type) {
    RoomProps p;
    p.paint = type == "bath" ? style.tile : panelled ? std::string("PINE_PANEL") : style.paint;
    p.floor_mat = type == "bath" ? style.wet : floor_for(type, style);
    return p;
  };
  const std::shared_ptr<FloorGrid> grid = pb.new_grid(0);
  // R[key]
  std::vector<std::pair<std::string, std::shared_ptr<Room>>> R;
  for (const CabinRoom& r : layout.rooms) R.emplace_back(r.key, grid->add_room(r.type, {r.rect}, mat(r.type)));
  auto get = [&](const char* key) -> Room* {
    for (const auto& p : R)
      if (p.first == key) return p.second.get();
    return nullptr;
  };
  Room* entry = get("entry");
  Room* bath = get("bath");
  Room* living = get("living");
  Room* kitchen = get("kitchen");
  Room* bed1 = get("bed1");
  {
    DoorOpts o;
    o.kind = "entrance";
    o.width = 8;
    o.place = "near";
    o.near = DoorNear{layout.entrance_u, 0};
    o.leaf = "wood";
    grid->add_door(*entry, nullptr, o);
  }
  if (!grid->add_door(*bath, entry, door(6, 1))) grid->add_door(*bath, living, door(6, 1));
  if (!grid->add_door(*entry, living, door(7, js::kNaN, "center"))) grid->add_door(*entry, living, door(6, 1));
  {
    DoorOpts o;
    o.kind = "opening";
    o.width = 12;
    o.place = "center";
    if (!grid->add_door(*kitchen, living, o))
      if (!grid->add_door(*kitchen, living, door(7, 1))) grid->add_door(*kitchen, entry, door(7, 1));
  }
  for (const char* key : {"bed1", "bed2"}) {
    Room* bed = get(key);
    if (!bed) continue;
    if (!grid->add_door(*bed, living, door(7)))
      if (!grid->add_door(*bed, kitchen, door(7))) grid->add_door(*bed, bed1, door(7, 1));
  }
  pb.add_floor(0, grid, "ground");
}

void plan_church(const Envelope& env, PlanBuilder& pb) {
  const std::vector<Rect>& rects = tier_rects(env, 0);
  if (rects.size() < 2) SVX_FAIL("cabins: a church without its nave and tower");
  const Rect nave = rects[0];
  const Rect tower = rects[1];
  const std::shared_ptr<FloorGrid> grid = pb.new_grid(0);
  RoomProps p;
  p.paint = "PAINT_WHITE";
  p.floor_mat = "FLOOR_OAK";
  const std::shared_ptr<Room> vest = grid->add_room("foyer", {{tower.x0 + EXT_T, tower.y0 + EXT_T, tower.x1 - EXT_T, nave.y0}}, p);
  // (an Orthodox church, under onion domes: an icon screen, candle stands, no pews)
  const bool orthodox = env.steeple && env.steeple->dome && *env.steeple->dome != 0;
  const std::shared_ptr<Room> hall = grid->add_room(orthodox ? "orthodoxNave" : "nave", {{nave.x0 + EXT_T, nave.y0 + EXT_T, nave.x1 - EXT_T, nave.y1 - EXT_T}}, p);
  const double cu = (tower.x0 + tower.x1) / 2;
  DoorOpts eo;
  eo.kind = "entrance";
  eo.width = 10;
  eo.place = "near";
  eo.near = DoorNear{cu, 0};
  eo.leaf = "wood";
  grid->add_door(*vest, nullptr, eo);
  DoorOpts io;
  io.width = 10;
  io.place = "center";
  io.leaf = "wood";
  if (!grid->add_door(*vest, hall.get(), io)) grid->add_door(*vest, hall.get(), door(7, 1));
  pb.add_floor(0, grid, "ground");
}

}  // namespace svx::city
