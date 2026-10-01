// svx_city — voxel_city buildings/interior/school.js.
#include "buildings/interior/school.hpp"

#include <memory>
#include <string>

#include "buildings/interior/common.hpp"
#include "buildings/interior/stairs.hpp"
#include "svx/base/types.hpp"

namespace svx::city {

namespace {

constexpr double EXT_T = FloorGrid::EXT_T;
constexpr double CORR = 24;  // corridor width (3 m)
constexpr double BAY = 72;   // classroom width target (9 m)

using Piece = std::array<double, 2>;

// STYLE: the finishes of a room type (classroom's for any other)
RoomProps style_of(const std::string& type) {
  RoomProps p;
  auto set = [&](const char* paint, const char* floor_mat) {
    p.paint = paint;
    p.floor_mat = floor_mat;
  };
  if (type == "gym")
    set("PAINT_BLUE", "FLOOR_PARQUET");
  else if (type == "cafeteria")
    set("PAINT_MINT", "FLOOR_TILE_GRAY");
  else if (type == "kitchen")
    set("WALL_TILE_WHITE", "FLOOR_TILE_WHITE");
  else if (type == "library")
    set("PAINT_SAGE", "FLOOR_CARPET_BLUE");
  else if (type == "office")
    set("PAINT_WHITE", "FLOOR_CARPET_GRAY");
  else if (type == "teachers")
    set("PAINT_PEACH", "FLOOR_LINOLEUM");
  else if (type == "restroom")
    set("WALL_TILE_WHITE", "FLOOR_TILE_GRAY");
  else if (type == "lobby")
    set("PAINT_WHITE", "FLOOR_TERRAZZO");
  else if (type == "corridor")
    set("PAINT_CREAM", "FLOOR_TERRAZZO");
  else
    set("PAINT_CREAM", "FLOOR_LINOLEUM");  // classroom
  return p;
}

DoorOpts door(double width) {
  DoorOpts o;
  o.width = width;
  return o;
}

}  // namespace

std::vector<Piece> end_safe(const std::vector<Piece>& pieces, double min_end) {
  std::vector<Piece> out = pieces;
  while (out.size() > 2 && out[0][1] - out[0][0] + 1 < min_end) {
    out[1][0] = out[0][0];
    out.erase(out.begin());
  }
  while (out.size() > 2 && out.back()[1] - out.back()[0] + 1 < min_end) {
    out[out.size() - 2][1] = out.back()[1];
    out.pop_back();
  }
  return out;
}

void plan_school(const Envelope& env, Rng& rng, PlanBuilder& pb) {
  const double nF = env.floors;
  if (env.story_h.empty()) SVX_FAIL("school: an envelope without stories");
  const double H = env.story_h[0];
  const std::vector<Rect>& fps = tier_rects(env, 0);
  if (fps.empty()) SVX_FAIL("school: an envelope without its ground floor");
  const Rect fp = fps[0];
  const Rect inner{fp.x0 + EXT_T, fp.y0 + EXT_T, fp.x1 - EXT_T, fp.y1 - EXT_T};
  const double Vi = inner.y1 - inner.y0 + 1;
  const double depth = std::floor((Vi - CORR - 2) / 2);
  const Rect band{inner.x0, inner.y0 + depth + 1, inner.x1, inner.y0 + depth + CORR};
  const Rect front{inner.x0, inner.y0, inner.x1, band.y0 - 2};
  const Rect back{inner.x0, band.y1 + 2, inner.x1, inner.y1};
  const StairDims sd = stair_dims(H);
  const double cc = js::round((band.y0 + band.y1) / 2);
  const double sy0 = cc - std::floor(sd.W / 2);
  const Rect left{inner.x0, sy0, inner.x0 + sd.L - 1, sy0 + sd.W - 1};
  const Rect right{inner.x1 - sd.L + 1, sy0, inner.x1, sy0 + sd.W - 1};
  std::vector<std::shared_ptr<Stair>> stairs;
  {
    MakeStairOpts so;
    so.rect = left;
    so.axis = 'u';
    so.dir = -1;
    so.lane_low = true;
    so.f0 = 0;
    so.f1 = nF;
    stairs.push_back(pb.add_stair(make_stair(so)));
    so.rect = right;
    so.dir = 1;
    so.lane_low = false;
    stairs.push_back(pb.add_stair(make_stair(so)));
  }
  // the stair cores sit at the ends of the corridor band; the room bands run the full length (the
  // end rooms reach past the stairs to the corridor) (the band beside a stair is too narrow to
  // walk: it stays solid wall)
  std::vector<Rect> corr_rects;
  for (const Rect& q : r_subtract_all({band}, {{left.x0 - 1, band.y0, left.x1 + 1, band.y1}, {right.x0 - 1, band.y0, right.x1 + 1, band.y1}}))
    if (q.x1 >= q.x0 && q.y1 >= q.y0) corr_rects.push_back(q);
  const double min_end = sd.L + 24;
  const std::vector<Piece> f_pieces = end_safe(split_length(front.x0, front.x1, BAY, 52, rng), min_end);
  const std::vector<Piece> b_pieces = end_safe(split_length(back.x0, back.x1, BAY, 52, rng), min_end);

  auto floor_plan = [&](double f) {
    const std::shared_ptr<FloorGrid> grid = pb.new_grid(f);
    std::vector<std::shared_ptr<Room>> stair_rooms;
    for (const auto& st : stairs) stair_rooms.push_back(add_stair_room(*grid, *st));
    const std::shared_ptr<Room> corridor = grid->add_room("corridor", corr_rects, style_of("corridor"));
    for (size_t k = 0; k < stair_rooms.size(); ++k) stair_door(*grid, *stair_rooms[k], *stairs[k], corridor.get());
    auto add = [&](const std::string& type, const Rect& rect) { return grid->add_room(type, {rect}, style_of(type)); };
    if (f == 0) {
      // street side: lobby in the middle (open to the corridor), office and restrooms beside it
      const double mid = std::floor(static_cast<double>(f_pieces.size()) / 2);
      for (size_t i = 0; i < f_pieces.size(); ++i) {
        const double k = static_cast<double>(i);
        const double a0 = f_pieces[i][0], a1 = f_pieces[i][1];
        const Rect r{a0, front.y0, a1, front.y1};
        const char* type = k == mid ? "lobby" : k == mid - 1 ? "office" : k == mid + 1 ? "restroom" : "classroom";
        const std::shared_ptr<Room> room = add(type, r);
        if (std::string(type) == "lobby") {
          DoorOpts o = door(16);
          o.kind = "entrance";
          o.place = "near";
          o.near = DoorNear{(a0 + a1) / 2, -12};
          o.leaf = "glass";
          grid->add_door(*room, nullptr, o);
          DoorOpts in = door(16);
          in.place = "center";
          in.kind = "opening";
          in.leaf = "none";
          grid->add_door(*room, corridor.get(), in);
        } else {
          grid->add_door(*room, corridor.get(), door(8));
        }
      }
      // yard side: gym (two bays), cafeteria + kitchen, classrooms
      size_t k = 0;
      if (b_pieces.size() >= 4) {
        const std::shared_ptr<Room> gym = add("gym", {b_pieces[0][0], back.y0, b_pieces[1][1], back.y1});
        grid->add_door(*gym, corridor.get(), door(12));
        DoorOpts o = door(12);
        o.kind = "entrance";
        o.place = "near";
        o.near = DoorNear{(b_pieces[0][0] + b_pieces[1][1]) / 2, env.V + 12};
        o.leaf = "metal";
        grid->add_door(*gym, nullptr, o);
        k = 2;
      }
      const std::shared_ptr<Room> caf = k < b_pieces.size() ? add("cafeteria", {b_pieces[k][0], back.y0, b_pieces[k][1], back.y1}) : nullptr;
      if (caf) grid->add_door(*caf, corridor.get(), door(12));
      if (caf && k + 1 < b_pieces.size()) {
        const std::shared_ptr<Room> kit = add("kitchen", {b_pieces[k + 1][0], back.y0, b_pieces[k + 1][1], back.y1});
        if (!grid->add_door(*kit, caf.get(), door(8))) grid->add_door(*kit, corridor.get(), door(8));
      }
      for (size_t m = k + 2; m < b_pieces.size(); ++m) {
        const std::shared_ptr<Room> room = add("classroom", {b_pieces[m][0], back.y0, b_pieces[m][1], back.y1});
        grid->add_door(*room, corridor.get(), door(8));
      }
    } else {
      const double lib = rng.int_(0, js::max(0.0, static_cast<double>(b_pieces.size()) - 2));
      for (size_t i = 0; i < f_pieces.size(); ++i) {
        const char* type = i == 0 ? "teachers" : i == f_pieces.size() - 1 ? "restroom" : "classroom";
        const std::shared_ptr<Room> room = add(type, {f_pieces[i][0], front.y0, f_pieces[i][1], front.y1});
        grid->add_door(*room, corridor.get(), door(8));
      }
      for (size_t m = 0; m < b_pieces.size(); ++m) {
        std::shared_ptr<Room> room;
        if (static_cast<double>(m) == lib && b_pieces.size() >= 3) {
          room = add("library", {b_pieces[m][0], back.y0, b_pieces[m + 1][1], back.y1});
          m += 1;
        } else {
          room = add("classroom", {b_pieces[m][0], back.y0, b_pieces[m][1], back.y1});
        }
        grid->add_door(*room, corridor.get(), door(8));
      }
    }
    return grid;
  };

  pb.add_floor(0, floor_plan(0), "school");
  const std::shared_ptr<FloorGrid> upper = nF > 1 ? floor_plan(1) : nullptr;
  for (double f = 1; f < nF; f += 1) pb.add_floor(f, upper, "school");
}

}  // namespace svx::city
