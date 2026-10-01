// svx_city — voxel_city buildings/interior/industrial.js.
#include "buildings/interior/industrial.hpp"

#include <array>
#include <vector>

#include "buildings/interior/common.hpp"
#include "buildings/interior/stairs.hpp"
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
RoomProps finish(const char* paint, const char* floor_mat) {
  RoomProps p;
  p.paint = paint;
  p.floor_mat = floor_mat;
  return p;
}

// HALLS: the hall of an industrial building by its program (its finishes), or null
const RoomProps* hall_finish(const std::string& program) {
  static const RoomProps kConcreteEpoxy = finish("CONCRETE", "FLOOR_EPOXY");
  static const RoomProps kBarn = finish("WOOD_DARK", "FLOOR_CONCRETE");
  static const RoomProps kSelfStorage = finish("PAINT_GRAY", "FLOOR_CONCRETE");
  static const RoomProps kColdStore = finish("PAINT_WHITE", "FLOOR_EPOXY");
  static const RoomProps kTimberYard = finish("CONCRETE", "FLOOR_CONCRETE");
  if (program == "warehouse" || program == "factory" || program == "distribution") return &kConcreteEpoxy;
  if (program == "barn") return &kBarn;
  if (program == "selfStorage") return &kSelfStorage;
  if (program == "coldStore") return &kColdStore;
  if (program == "timberYard") return &kTimberYard;
  return nullptr;
}

}  // namespace

void plan_industrial(const Envelope& env, Rng& rng, PlanBuilder& pb) {
  const std::vector<Rect>& rects = tier_rects(env, 0);
  if (rects.empty() || env.story_h.empty()) SVX_FAIL("industrial: an envelope without its ground floor");
  const Rect rect = rects[0];
  const Rect inner{rect.x0 + EXT_T, rect.y0 + EXT_T, rect.x1 - EXT_T, rect.y1 - EXT_T};
  const double H = env.story_h[0];
  const double z0 = pb.z(0);
  const double ceil_a = 28;  // annex ground level story (3.5 m)
  const double mezz_h = js::min(28.0, H - ceil_a);
  const StairDims sd = stair_dims(ceil_a);
  const double drawn_w = js::round(rng.float_(10, 14) * M);
  const double aw = js::min(inner.x1 - inner.x0 - 6 * M, js::max(sd.L + 2 + 3 * M, drawn_w));
  const double ad = js::round(rng.float_(6.5, 8.5) * M);
  const bool left = rng.chance(0.5);
  const double ax0 = left ? inner.x0 : inner.x1 - aw + 1;
  const Rect annex{ax0, inner.y0, ax0 + aw - 1, inner.y0 + ad - 1};

  // stair along u at the back of the annex; near landing towards the annex middle
  const Rect s_rect = left ? Rect{annex.x1 - sd.L + 1, annex.y1 - sd.W + 1, annex.x1, annex.y1} : Rect{annex.x0, annex.y1 - sd.W + 1, annex.x0 + sd.L - 1, annex.y1};
  MakeStairOpts so;
  so.rect = s_rect;
  so.axis = 'u';
  so.dir = left ? 1 : -1;
  so.lane_low = true;
  so.f0 = 0;
  so.f1 = 1;
  const std::shared_ptr<Stair> stair = pb.add_stair(make_stair(so));
  stair->flights = std::vector<StairFlight>{{0, z0, ceil_a}};

  // ---- hall floor
  const std::shared_ptr<FloorGrid> g0 = pb.new_grid(0);
  const std::shared_ptr<Room> s_room0 = add_stair_room(*g0, *stair);
  const Rect front_row{annex.x0, annex.y0, annex.x1, s_rect.y0 - 2};
  // reception spans the stair's near landing (so the stair door works); rooms on either side
  const double near_u = left ? s_rect.x0 + 4 : s_rect.x1 - 4;
  const std::array<double, 2> land = left ? std::array<double, 2>{s_rect.x0, s_rect.x0 + 8} : std::array<double, 2>{s_rect.x1 - 8, s_rect.x1};
  const Rect rec{js::max(front_row.x0, land[0] - 10), front_row.y0, js::min(front_row.x1, land[1] + 10), front_row.y1};
  std::vector<std::shared_ptr<Room>> annex_rooms;
  RoomProps rp = finish("PAINT_WHITE", "FLOOR_LINOLEUM");
  rp.ceiling = ceil_a;
  const std::shared_ptr<Room> reception = g0->add_room("reception", {rec}, rp);
  annex_rooms.push_back(reception);
  std::vector<std::array<double, 2>> side_pieces;
  if (rec.x0 - 2 - front_row.x0 + 1 >= 12) side_pieces.push_back({front_row.x0, rec.x0 - 2});
  if (front_row.x1 - (rec.x1 + 2) + 1 >= 12) side_pieces.push_back({rec.x1 + 2, front_row.x1});
  for (const auto& sp : side_pieces) {
    for (const auto& ab : split_length(sp[0], sp[1], 4 * M, 12, rng)) {
      bool has_wc = false;
      for (const auto& r : annex_rooms)
        if (r->type == "wc") has_wc = true;
      const std::string type = has_wc ? "office" : "wc";
      RoomProps p = type == "wc" ? finish("WALL_TILE_WHITE", "FLOOR_TILE_WHITE") : finish("PAINT_WHITE", "FLOOR_LINOLEUM");
      p.ceiling = ceil_a;
      annex_rooms.push_back(g0->add_room(type, {{ab[0], front_row.y0, ab[1], front_row.y1}}, p));
    }
  }
  // the leftover strip beside the stair (if any) joins the reception row as storage
  const Rect beside = left ? Rect{annex.x0, s_rect.y0, s_rect.x0 - 2, annex.y1} : Rect{s_rect.x1 + 2, s_rect.y0, annex.x1, annex.y1};
  if (beside.x1 - beside.x0 + 1 >= 10) {
    RoomProps p = finish("PAINT_GRAY", "FLOOR_CONCRETE");
    p.ceiling = ceil_a;
    annex_rooms.push_back(g0->add_room("storage", {beside}, p));
  }
  s_room0->ceiling = ceil_a;

  const std::vector<Rect> hall_rects = r_subtract_all({inner}, {{annex.x0 - 1, annex.y0 - 1, annex.x1 + 1, annex.y1 + 1}});
  const std::string hall_type = hall_finish(env.program.ground) ? env.program.ground : "warehouse";
  const std::shared_ptr<Room> hall = g0->add_room(hall_type, hall_rects, *hall_finish(hall_type));
  stair_door(*g0, *s_room0, *stair, reception.get());
  if (reception->rects.empty()) SVX_FAIL("industrial: a reception without its rect");
  {
    DoorOpts o = door(8);
    o.kind = "entrance";
    o.place = "near";
    o.near = DoorNear{(reception->rects[0].x0 + reception->rects[0].x1) / 2, 0};
    o.leaf = "glass";
    g0->add_door(*reception, nullptr, o);
  }
  {
    DoorOpts o = door(8);
    o.leaf = "metal";
    if (!g0->add_door(*reception, hall.get(), o)) g0->add_door(*reception, hall.get(), door(7, 1));
  }
  for (const auto& r : annex_rooms) {
    if (r == reception) continue;
    if (g0->add_door(*r, reception.get(), door(7)) || g0->add_door(*r, hall.get(), door(7)) || g0->add_door(*r, reception.get(), door(6, 1))) continue;
    // a room we cannot open becomes solid fit-out
    r->type = "shaft";
  }
  // loading doors on the front facade + a personnel door at the back
  if (hall_rects.empty()) SVX_FAIL("industrial: a hall without rects");
  const Rect* hall_front = &hall_rects[0];
  for (const Rect& r : hall_rects)
    if (r.y0 == inner.y0) {
      hall_front = &r;
      break;
    }
  const double n_docks = js::max(1.0, js::min(4.0, std::floor((hall_front->x1 - hall_front->x0) / (8 * M))));
  for (double k = 0; k < n_docks; k += 1) {
    const double u = hall_front->x0 + ((k + 0.5) * (hall_front->x1 - hall_front->x0)) / n_docks;
    DoorOpts o = door(28, 4);
    o.kind = "rollup";
    o.place = "near";
    o.near = DoorNear{u, 0};
    o.leaf = "rollup";
    o.height = 32;
    g0->add_door(*hall, nullptr, o);
  }
  {
    DoorOpts o = door(8);
    o.kind = "entrance";
    o.place = "near";
    o.near = DoorNear{(inner.x0 + inner.x1) / 2, inner.y1 + 3};
    o.leaf = "metal";
    g0->add_door(*hall, nullptr, o);
  }
  FloorExtra e0;
  e0.low_regions.push_back({{annex.x0 - 1, annex.y0 - 1, annex.x1 + 1, annex.y1 + 1}, ceil_a});
  pb.add_floor(0, g0, "industrial", e0);

  // ---- mezzanine (annex upper level)
  const Rect annex_outer{annex.x0 - EXT_T, annex.y0 - EXT_T, annex.x1 + EXT_T, annex.y1 + EXT_T};
  const std::shared_ptr<FloorGrid> g1 = std::make_shared<FloorGrid>(env.U, env.V, std::vector<Rect>{annex_outer});
  const std::shared_ptr<Room> s_room1 = add_stair_room(*g1, *stair);
  const Rect landing_rect{js::max(front_row.x0, near_u - 12), front_row.y0, js::min(front_row.x1, near_u + 12), front_row.y1};
  const std::shared_ptr<Room> landing = g1->add_room("hall", {landing_rect}, finish("PAINT_WHITE", "FLOOR_LINOLEUM"));
  stair_door(*g1, *s_room1, *stair, landing.get());
  const std::vector<Rect> rest = r_subtract_all({front_row}, {{landing_rect.x0 - 1, landing_rect.y0, landing_rect.x1 + 1, landing_rect.y1}});
  for (const Rect& r : rest) {
    if (r.x1 - r.x0 + 1 < 12) continue;
    static const char* const kTypes[] = {"office", "meeting", "office"};
    const std::string type = rng.pick(kTypes);
    const std::shared_ptr<Room> room = g1->add_room(type, {r}, finish("PAINT_WHITE", "FLOOR_CARPET_GRAY"));
    if (!g1->add_door(*room, landing.get(), door(7))) g1->add_door(*room, landing.get(), door(6, 1));
  }
  if (beside.x1 - beside.x0 + 1 >= 10) {
    const std::shared_ptr<Room> room = g1->add_room("storage", {beside}, finish("PAINT_GRAY", "FLOOR_CONCRETE"));
    if (!g1->add_door(*room, landing.get(), door(6, 1))) {
      // reachable through the stair landing zone only if adjacent; otherwise mark as shaft
      room->type = "shaft";
    }
  }
  FloorExtra e1;
  e1.z = z0 + ceil_a;
  e1.height = mezz_h;
  e1.mezzanine = true;
  pb.add_floor(1, g1, "mezzanine", e1);
}

}  // namespace svx::city
