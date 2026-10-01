// svx_city — voxel_city buildings/interior/civic.js.
#include "buildings/interior/civic.hpp"

#include <array>
#include <memory>
#include <utility>

#include "buildings/interior/apartments.hpp"
#include "buildings/interior/common.hpp"
#include "buildings/interior/offices.hpp"
#include "buildings/interior/school.hpp"
#include "buildings/interior/stairs.hpp"
#include "core/math.hpp"
#include "svx/base/types.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

namespace {

constexpr double EXT_T = FloorGrid::EXT_T;

using Piece = std::array<double, 2>;

// FINISH: finishes per room type (WHITE for any other)
struct Finish {
  const char* type;
  const char* paint;
  const char* floor_mat;
};
constexpr Finish kFinish[] = {
    {"foyer", "PAINT_CREAM", "FLOOR_MARBLE"},           {"lobby", "PAINT_WHITE", "FLOOR_TERRAZZO"},
    {"sales", "PAINT_WHITE", "FLOOR_TILE_GRAY"},        {"marketHall", "PAINT_CREAM", "FLOOR_TILE_TERRA"},
    {"auditorium", "PAINT_DARK", "FLOOR_CARPET_RED"},   {"cinema", "PAINT_DARK", "FLOOR_CARPET_BLUE"},
    {"venueFloor", "PAINT_DARK", "FLOOR_CONCRETE"},     {"foyerBar", "PAINT_TERRACOTTA", "FLOOR_PARQUET"},
    {"danceHall", "PAINT_CREAM", "FLOOR_PARQUET"},      {"dressing", "PAINT_PEACH", "FLOOR_LINOLEUM"},
    {"cloakroom", "PAINT_WHITE", "FLOOR_MARBLE"},       {"kiosk", "PAINT_WHITE", "FLOOR_TILE_GRAY"},
    {"storage", "PAINT_GRAY", "FLOOR_CONCRETE"},        {"breakroom", "PAINT_MINT", "FLOOR_LINOLEUM"},
    {"office", "PAINT_WHITE", "FLOOR_CARPET_GRAY"},     {"wc", "WALL_TILE_WHITE", "FLOOR_TILE_WHITE"},
    {"restroom", "WALL_TILE_WHITE", "FLOOR_TILE_GRAY"}, {"ward", "PAINT_MINT", "FLOOR_LINOLEUM"},
    {"emergency", "PAINT_MINT", "FLOOR_LINOLEUM"},      {"exam", "PAINT_WHITE", "FLOOR_LINOLEUM"},
    {"operating", "WALL_TILE_GREEN", "FLOOR_EPOXY"},    {"radiology", "PAINT_BLUE", "FLOOR_LINOLEUM"},
    {"waiting", "PAINT_SAGE", "FLOOR_LINOLEUM"},        {"nurses", "PAINT_WHITE", "FLOOR_LINOLEUM"},
    {"pharmacy", "PAINT_WHITE", "FLOOR_TILE_WHITE"},    {"policeDesk", "PAINT_BLUE", "FLOOR_TERRAZZO"},
    {"cell", "CONCRETE", "FLOOR_CONCRETE"},             {"interview", "PAINT_GRAY", "FLOOR_LINOLEUM"},
    {"briefing", "PAINT_WHITE", "FLOOR_CARPET_BLUE"},   {"evidence", "PAINT_GRAY", "FLOOR_CONCRETE"},
    {"lockerRoom", "PAINT_GRAY", "FLOOR_TILE_GRAY"},    {"garageBay", "CONCRETE", "FLOOR_EPOXY"},
    {"dorm", "PAINT_CREAM", "FLOOR_LINOLEUM"},          {"kitchen", "WALL_TILE_WHITE", "FLOOR_TILE_WHITE"},
    {"dining", "PAINT_PEACH", "FLOOR_OAK"},             {"exhibit", "PAINT_CREAM", "FLOOR_PARQUET"},
    {"gallery", "PAINT_WHITE", "FLOOR_OAK"},            {"museumShop", "PAINT_SAGE", "FLOOR_OAK"},
    {"cafe", "PAINT_TERRACOTTA", "FLOOR_OAK"},          {"restaurant", "PAINT_PEACH", "FLOOR_OAK"},
    {"pub", "WOOD_PANEL", "FLOOR_WALNUT"},              {"library", "PAINT_SAGE", "FLOOR_CARPET_BLUE"},
    {"study", "PAINT_CREAM", "FLOOR_CARPET_BEIGE"},     {"reception", "PAINT_WHITE", "FLOOR_MARBLE"},
    {"council", "WOOD_PANEL", "FLOOR_CARPET_RED"},      {"registry", "PAINT_CREAM", "FLOOR_PARQUET"},
    {"meeting", "PAINT_WHITE", "FLOOR_CARPET_BLUE"},    {"hotelRoom", "PAINT_CREAM", "FLOOR_CARPET_BEIGE"},
    {"clubroom", "PAINT_CREAM", "FLOOR_PARQUET"},       {"departmentFloor", "PAINT_WHITE", "FLOOR_TERRAZZO"},
};

RoomProps finish(const std::string& type) {
  RoomProps p;
  p.paint = "PAINT_WHITE";
  p.floor_mat = "FLOOR_TERRAZZO";
  for (const Finish& f : kFinish)
    if (type == f.type) {
      p.paint = f.paint;
      p.floor_mat = f.floor_mat;
      break;
    }
  return p;
}

Rect inset(const Rect& r, double d) { return {r.x0 + d, r.y0 + d, r.x1 - d, r.y1 - d}; }

// NaN as JS's undefined: `v ?? d`
double or_undef(double v, double d) { return v == v ? v : d; }

// Door to a neighbour, falling back to a narrower one ({width: 8, ...opts}, then {...opts, width: 6,
// margin: 1}); false when neither fits.
bool connect(FloorGrid& grid, const Room& room, const Room* to, const DoorOpts& opts = {}) {
  DoorOpts a = opts;
  if (!a.width) a.width = 8;
  if (grid.add_door(room, to, a)) return true;
  DoorOpts b = opts;
  b.width = 6;
  b.margin = 1;
  if (grid.add_door(room, to, b)) return true;
  return false;
}

double overlap(const Room& h, const Room& b) { return js::min(h.rects[0].x1, b.rects[0].x1) - js::max(h.rects[0].x0, b.rects[0].x0); }

// Split parts [[u0, u1] ...] with the ones out of the corridor's reach (behind an end stair) joined
// to their neighbour.
std::vector<Piece> off_corridor(const std::vector<Piece>& parts, const std::vector<Rect>& corr_rects) {
  double lo = js::kInf, hi = -js::kInf;
  for (const Rect& r : corr_rects) lo = js::min(lo, r.x0);
  for (const Rect& r : corr_rects) hi = js::max(hi, r.x1);
  lo = lo + 10;
  hi = hi - 10;
  std::vector<Piece> out = parts;
  while (out.size() > 1 && out[0][1] < lo) {
    out[0] = {out[0][0], out[1][1]};
    out.erase(out.begin() + 1);
  }
  while (out.size() > 1 && out.back()[0] > hi) {
    const size_t m = out.size();
    out[m - 2] = {out[m - 2][0], out[m - 1][1]};
    out.pop_back();
  }
  return out;
}

// Assign room specs to n pieces: those with `at` first (mid / start / end), then the rest in order;
// spans take consecutive pieces; free pieces get the fill types in turn. Returns [{from, to, spec}]
// in piece order.
struct Assigned {
  double from = 0, to = 0;
  CorridorSpec spec{};
};
std::vector<Assigned> assign(double n, const std::vector<CorridorSpec>& specs, const std::vector<std::string>& fill) {
  std::vector<bool> used(static_cast<size_t>(n), false);
  std::vector<Assigned> out;
  // (a wide room never takes every piece: the others need a place too)
  auto span_of = [&](const CorridorSpec& spec) { return js::max(1.0, js::min(or_undef(spec.span, 1), n > 1 ? n - 1 : 1)); };
  auto place = [&](const CorridorSpec& spec, double from) {
    const double span = span_of(spec);
    for (double s = from; s + span <= n; s += 1) {
      bool ok = true;
      for (double k = s; k < s + span; k += 1)
        if (used[static_cast<size_t>(k)]) ok = false;
      if (!ok) continue;
      for (double k = s; k < s + span; k += 1) used[static_cast<size_t>(k)] = true;
      out.push_back({s, s + span - 1, spec});
      return true;
    }
    return false;
  };
  auto place_exact = [&](const CorridorSpec& spec, double s, double span) {
    for (double k = s; k < s + span; k += 1)
      if (used[static_cast<size_t>(k)]) return false;
    for (double k = s; k < s + span; k += 1) used[static_cast<size_t>(k)] = true;
    out.push_back({s, s + span - 1, spec});
    return true;
  };
  auto try_at = [&](const CorridorSpec& spec) {
    const double span = span_of(spec);
    const double start = spec.at == "mid" ? js::max(0.0, std::floor((n - span) / 2)) : spec.at == "end" ? n - span : 0;
    std::vector<double> order{start};
    for (double d = 1; d < n; d += 1) {
      order.push_back(start + d);
      order.push_back(start - d);
    }
    for (double s : order)
      if (s >= 0 && s + span <= n && place_exact(spec, s, span)) return;
  };
  for (const CorridorSpec& spec : specs)
    if (!spec.at.empty()) try_at(spec);
  for (const CorridorSpec& spec : specs)
    if (spec.at.empty()) place(spec, 0);
  double k = 0;
  for (double i = 0; i < n; i += 1) {
    if (used[static_cast<size_t>(i)]) continue;
    CorridorSpec spec;
    spec.type = fill[static_cast<size_t>(std::fmod(k, static_cast<double>(fill.size())))];
    k += 1;
    used[static_cast<size_t>(i)] = true;
    out.push_back({i, i, spec});
  }
  js::sort(out, [](const Assigned& a, const Assigned& b) { return a.from - b.from; });
  return out;
}

DoorOpts near_door(const char* kind, double width, double u, double v, const char* leaf) {
  DoorOpts o;
  o.kind = kind;
  o.width = width;
  o.place = "near";
  o.near = DoorNear{u, v};
  o.leaf = leaf;
  return o;
}

}  // namespace

// ============================================================ hall buildings

void plan_hall(const Envelope& env, Rng& rng, PlanBuilder& pb, const HallProgram& P) {
  const double nF = env.floors;
  const std::vector<Rect>& fps = tier_rects(env, 0);
  if (fps.empty()) SVX_FAIL("civic: an envelope without its ground floor");
  const Rect inner = inset(fps[0], EXT_T);
  const double Vi = inner.y1 - inner.y0 + 1;
  const double Ui = inner.x1 - inner.x0 + 1;
  double max_h = 0;
  for (double h : env.story_h) max_h = js::max(max_h, h);
  std::optional<StairDims> sd;
  if (nF > 1) sd = stair_dims(max_h);
  const double fd = js::round(js::max(sd ? sd->W + 14 : 24, js::min(vx(or_undef(P.foyer, 5)), Vi * 0.34)));
  const double bd = !P.back.empty() ? js::round(js::max(26.0, js::min(vx(or_undef(P.back_depth, 4.5)), Vi * 0.28))) : 0;
  const Rect foyer_band{inner.x0, inner.y0, inner.x1, inner.y0 + fd - 1};
  std::optional<Rect> back_band;
  if (js::truthy(bd)) back_band = Rect{inner.x0, inner.y1 - bd + 1, inner.x1, inner.y1};
  const Rect hall_zone{inner.x0, foyer_band.y1 + 2, inner.x1, back_band ? back_band->y0 - 2 : inner.y1};
  // the stair in a front corner of the foyer band, its near end towards the middle
  const bool stair_left = rng.chance(0.5);
  std::shared_ptr<Stair> stair;
  std::optional<Rect> s_rect;
  if (sd) {
    s_rect = stair_left ? Rect{inner.x0, inner.y0, inner.x0 + sd->L - 1, inner.y0 + sd->W - 1} : Rect{inner.x1 - sd->L + 1, inner.y0, inner.x1, inner.y0 + sd->W - 1};
    MakeStairOpts so;
    so.rect = *s_rect;
    so.axis = 'u';
    so.dir = stair_left ? -1 : 1;
    so.lane_low = false;
    so.f0 = 0;
    so.f1 = nF - 1;
    stair = pb.add_stair(make_stair(so));
  }
  // side rooms at the other end of the foyer band
  const double side_w = js::round(js::min(vx(5), js::max(28.0, Ui * 0.14)));
  const double n_side = js::max(0.0, std::floor((Ui - (sd ? sd->L : 0) - 60) / (side_w + 1)));
  struct SideRect {
    std::string type;
    Rect rect;
  };
  std::vector<SideRect> side_rects;
  double edge = stair_left ? inner.x1 : inner.x0;
  // (in a deep foyer the side rooms leave a passage behind them)
  const double side_y1 = fd >= 40 ? foyer_band.y1 - 14 : foyer_band.y1;
  for (size_t i = 0; i < P.foyer_side.size() && static_cast<double>(i) < n_side; ++i) {
    const Rect r = stair_left ? Rect{edge - side_w + 1, foyer_band.y0, edge, side_y1} : Rect{edge, foyer_band.y0, edge + side_w - 1, side_y1};
    side_rects.push_back({P.foyer_side[i], r});
    edge = stair_left ? r.x0 - 2 : r.x1 + 2;
  }
  auto cut = [](const Rect& r) { return Rect{r.x0 - 1, r.y0 - 1, r.x1 + 1, r.y1 + 1}; };
  std::vector<Rect> foyer_rects;
  {
    std::vector<Rect> cutters;
    for (const SideRect& s : side_rects) cutters.push_back(cut(s.rect));
    if (s_rect) cutters.push_back(cut(*s_rect));
    for (const Rect& r : r_subtract_all({foyer_band}, cutters))
      if (r.x1 - r.x0 >= 3 && r.y1 - r.y0 >= 3) foyer_rects.push_back(r);
  }
  // halls side by side, backs rooms in pieces behind them
  const double halls_hi = P.halls.size() > 1 ? P.halls[1] : 1;
  const double halls_lo = !P.halls.empty() ? P.halls[0] : 1;
  const double n_halls = js::max(1.0, js::min(halls_hi, js::max(halls_lo, std::floor(Ui / vx(or_undef(P.hall_min_w, 9))))));
  const std::vector<Piece> hall_pieces = split_length(hall_zone.x0, hall_zone.x1, std::floor((Ui - (n_halls - 1)) / n_halls), 24, rng, 0.1);
  std::vector<Piece> back_pieces;
  if (back_band) back_pieces = split_length(back_band->x0, back_band->x1, js::round(vx(or_undef(P.back_w, 5))), 24, rng, 0.2);

  // ---- ground floor
  const std::shared_ptr<FloorGrid> g0 = pb.new_grid(0);
  const std::shared_ptr<Room> s_room0 = stair ? add_stair_room(*g0, *stair) : nullptr;
  const std::string foyer_type = P.foyer_type.empty() ? "foyer" : P.foyer_type;
  const std::shared_ptr<Room> foyer = g0->add_room(foyer_type, foyer_rects, finish(foyer_type));
  std::vector<std::shared_ptr<Room>> halls;
  for (const Piece& hp : hall_pieces) {
    RoomProps p = finish(P.hall);
    if (P.hall_no_windows) p.no_windows = true;
    if (P.hall_classical) p.classical = true;
    p.tall = nF > 1;
    p.front = 'N';
    halls.push_back(g0->add_room(P.hall, {{hp[0], hall_zone.y0, hp[1], hall_zone.y1}}, p));
  }
  std::vector<std::shared_ptr<Room>> sides;
  for (const SideRect& s : side_rects) sides.push_back(g0->add_room(s.type, {s.rect}, finish(s.type)));
  std::vector<std::shared_ptr<Room>> backs;
  for (size_t k = 0; k < back_pieces.size(); ++k) {
    const std::string& type = P.back[k % P.back.size()];
    backs.push_back(g0->add_room(type, {{back_pieces[k][0], back_band->y0, back_pieces[k][1], back_band->y1}}, finish(type)));
  }
  // entrance, halls off the foyer, side rooms off the foyer, back rooms off the halls
  std::vector<Rect> by_width = foyer_rects;
  js::sort(by_width, [](const Rect& a, const Rect& b) { return b.x1 - b.x0 - (a.x1 - a.x0); });
  if (by_width.empty()) SVX_FAIL("civic: a hall building without its foyer");
  const Rect fr = by_width[0];
  g0->add_door(*foyer, nullptr, near_door("entrance", js::min(16.0, fr.x1 - fr.x0 - 4), (fr.x0 + fr.x1) / 2, -12, "glass"));
  for (const auto& h : halls) {
    DoorOpts o;
    o.width = or_undef(P.hall_door, 14);
    o.kind = P.hall_open ? "opening" : "interior";
    o.leaf = P.hall_open ? "none" : "wood";
    o.place = "center";
    if (!(g0->add_door(*h, foyer.get(), o) || connect(*g0, *h, foyer.get()))) {
      const Room* to = halls[0] == h ? (halls.size() > 1 ? halls[1].get() : foyer.get()) : halls[0].get();
      connect(*g0, *h, to);
    }
  }
  for (const auto& s : sides)
    if (!connect(*g0, *s, foyer.get())) s->type = "shaft";
  DoorOpts metal;
  metal.leaf = "metal";
  for (const auto& b : backs) {
    std::vector<std::shared_ptr<Room>> under;
    for (const auto& h : halls)
      if (h->rects[0].x0 <= b->rects[0].x1 && h->rects[0].x1 >= b->rects[0].x0) under.push_back(h);
    js::sort(under, [&](const std::shared_ptr<Room>& p, const std::shared_ptr<Room>& q) { return overlap(*q, *b) - overlap(*p, *b); });
    const Room* ha = under.empty() ? nullptr : under[0].get();
    if (ha && connect(*g0, *b, ha, metal)) continue;
    bool any = false;
    for (const auto& h : halls)
      if (connect(*g0, *b, h.get(), metal)) {
        any = true;
        break;
      }
    if (!any) b->type = "shaft";
  }
  // the loading / stage door at the back
  if (!backs.empty() && !P.dock.empty()) {
    std::shared_ptr<Room> dock_room;
    for (const auto& b : backs)
      if (b->type != "shaft" && b->type != "wc") {
        dock_room = b;
        break;
      }
    if (!dock_room) dock_room = backs[0];
    const Rect r = dock_room->rects[0];
    const bool rollup = P.dock == "rollup" && r.x1 - r.x0 >= 36;
    if (rollup) {
      DoorOpts o = near_door("rollup", 28, (r.x0 + r.x1) / 2, env.V + 12, "rollup");
      o.margin = 4;
      o.height = 28;
      g0->add_door(*dock_room, nullptr, o);
    } else {
      g0->add_door(*dock_room, nullptr, near_door("entrance", 8, (r.x0 + r.x1) / 2, env.V + 12, "metal"));
    }
  }
  // fire exits from the end halls onto the side streets
  for (const std::shared_ptr<Room>* hp : {&halls.front(), &halls.back()}) {
    const Room& h = **hp;
    const Rect r = h.rects[0];
    const bool on_left = r.x0 == inner.x0;
    if (on_left || r.x1 == inner.x1) g0->add_door(h, nullptr, near_door("entrance", 8, on_left ? -12 : env.U + 12, (r.y0 + r.y1) / 2, "metal"));
  }
  if (s_room0) stair_door(*g0, *s_room0, *stair, foyer.get());
  pb.add_floor(0, g0, "ground");

  // ---- upper floor: a gallery / bar over the foyer, voids over the halls
  if (nF > 1) {
    const std::shared_ptr<FloorGrid> g1 = pb.new_grid(1);
    const std::shared_ptr<Room> s_room1 = add_stair_room(*g1, *stair);
    const size_t n_up = std::min(P.upper_side.size(), side_rects.size());
    std::vector<Rect> cutters;
    for (size_t k = 0; k < n_up; ++k) cutters.push_back(cut(side_rects[k].rect));
    cutters.push_back(cut(*s_rect));
    std::vector<Rect> land_rects;
    for (const Rect& r : r_subtract_all({foyer_band}, cutters))
      if (r.x1 - r.x0 >= 3 && r.y1 - r.y0 >= 3) land_rects.push_back(r);
    const std::string upper_foyer = P.upper_foyer.empty() ? "foyerBar" : P.upper_foyer;
    const std::shared_ptr<Room> land = g1->add_room(upper_foyer, land_rects, finish(upper_foyer));
    stair_door(*g1, *s_room1, *stair, land.get());
    for (size_t k = 0; k < n_up; ++k) {
      const std::string& t = P.upper_side[k];
      const std::shared_ptr<Room> room = g1->add_room(t, {side_rects[k].rect}, finish(t));
      if (!connect(*g1, *room, land.get())) room->type = "shaft";
    }
    for (const auto& h : halls) {
      RoomProps p;
      p.no_windows = P.hall_no_windows;
      g1->add_room("void", h->rects, p);
    }
    if (back_band) g1->add_room("shaft", {*back_band});
    pb.add_floor(1, g1, "upper");
  }
}

// ============================================================ corridor buildings

void plan_corridor(const Envelope& env, Rng& rng, PlanBuilder& pb, const CorridorProgram& P) {
  const double nF = env.floors;
  const std::vector<Rect>& fps = tier_rects(env, 0);
  if (fps.empty() || env.story_h.empty()) SVX_FAIL("civic: an envelope without its ground floor");
  const Rect inner = inset(fps[0], EXT_T);
  const double Vi = inner.y1 - inner.y0 + 1;
  double max_h0 = 0;
  for (double h : env.story_h) max_h0 = js::max(max_h0, h);
  // (the stairs sit in the corridor band: it is at least as wide as they are)
  const double CORR = js::max(js::round(vx(or_undef(P.corr, 3))), nF > 1 ? stair_dims(max_h0).W + 1 : 0);
  const double depth = std::floor((Vi - CORR - 2) / 2);
  const Rect band{inner.x0, inner.y0 + depth + 1, inner.x1, inner.y0 + depth + CORR};
  const Rect front{inner.x0, inner.y0, inner.x1, band.y0 - 2};
  const Rect back{inner.x0, band.y1 + 2, inner.x1, inner.y1};
  double max_h = 0;
  for (double h : env.story_h) max_h = js::max(max_h, h);
  std::vector<std::shared_ptr<Stair>> stairs;
  std::vector<Rect> corr_rects{band};
  double min_end = 24;
  if (nF > 1) {
    const StairDims sd = stair_dims(max_h);
    const double sy0 = band.y0 + std::floor((CORR - sd.W) / 2);
    const Rect left{inner.x0, sy0, inner.x0 + sd.L - 1, sy0 + sd.W - 1};
    const Rect right{inner.x1 - sd.L + 1, sy0, inner.x1, sy0 + sd.W - 1};
    const double top = env.roof.type == "flat" ? nF : nF - 1;
    MakeStairOpts so;
    so.rect = left;
    so.axis = 'u';
    so.dir = -1;
    so.lane_low = true;
    so.f0 = 0;
    so.f1 = top;
    stairs.push_back(pb.add_stair(make_stair(so)));
    so.rect = right;
    so.dir = 1;
    so.lane_low = false;
    stairs.push_back(pb.add_stair(make_stair(so)));
    corr_rects.clear();
    for (const Rect& q : r_subtract_all({band}, {{left.x0 - 1, band.y0, left.x1 + 1, band.y1}, {right.x0 - 1, band.y0, right.x1 + 1, band.y1}}))
      if (q.x1 >= q.x0 && q.y1 >= q.y0) corr_rects.push_back(q);
    min_end = sd.L + 24;
  }
  const double bay = js::round(vx(or_undef(P.bay, 6)));
  const std::vector<Piece> f_pieces = end_safe(split_length(front.x0, front.x1, bay, 28, rng), min_end);
  const std::vector<Piece> b_pieces = end_safe(split_length(back.x0, back.x1, bay, 28, rng), min_end);

  auto floor_plan = [&](double f) {
    const std::shared_ptr<FloorGrid> grid = pb.new_grid(f);
    std::vector<std::shared_ptr<Room>> stair_rooms;
    for (const auto& st : stairs) stair_rooms.push_back(add_stair_room(*grid, *st));
    RoomProps cp;
    cp.paint = P.corr_paint.empty() ? std::string("PAINT_CREAM") : P.corr_paint;
    cp.floor_mat = P.corr_floor.empty() ? std::string("FLOOR_TERRAZZO") : P.corr_floor;
    const std::shared_ptr<Room> corridor = grid->add_room("corridor", corr_rects, cp);
    for (size_t k = 0; k < stair_rooms.size(); ++k) stair_door(*grid, *stair_rooms[k], *stairs[k], corridor.get());
    // (f === 0 ? P.ground : f === 1 && P.first ? P.first : P.upper ?? P.first ?? P.ground)
    const std::optional<CorridorFloor>& chosen = f == 0 ? P.ground : f == 1 && P.first ? P.first : P.upper ? P.upper : P.first ? P.first : P.ground;
    if (!chosen) SVX_FAIL("civic: a corridor program without the floor's rooms");
    const CorridorFloor& prog = *chosen;
    static const std::vector<std::string> kOffice = {"office"};
    const std::vector<std::string>& fill = prog.fill ? *prog.fill : kOffice;
    struct Side {
      bool front;
      const std::vector<Piece>& pieces;
      const Rect& zone;
      const std::vector<CorridorSpec>& specs;
    };
    const Side sides[2] = {{true, f_pieces, front, prog.front}, {false, b_pieces, back, prog.back}};
    for (const Side& side : sides) {
      const std::vector<Assigned> specs = assign(static_cast<double>(side.pieces.size()), side.specs, fill);
      for (const Assigned& a : specs) {
        const Rect rect{side.pieces[static_cast<size_t>(a.from)][0], side.zone.y0, side.pieces[static_cast<size_t>(a.to)][1], side.zone.y1};
        // (split rooms, e.g. holding cells, stay at least ~3 m wide)
        const double n = js::max(1.0, js::min(or_undef(a.spec.split, 1), std::floor((rect.x1 - rect.x0 + 2) / 26)));
        std::vector<Piece> parts;
        if (n > 1)
          parts = off_corridor(split_length(rect.x0, rect.x1, std::floor((rect.x1 - rect.x0 + 1 - (n - 1)) / n), 16, rng, 0), corr_rects);
        else
          parts = {{rect.x0, rect.x1}};
        for (const Piece& pp : parts) {
          const double p0 = pp[0], p1 = pp[1];
          const std::string& t = a.spec.type;
          RoomProps props = finish(t);
          if (a.spec.fire) props.fire = true;
          props.front = side.front ? 'N' : 'S';
          const std::shared_ptr<Room> room = grid->add_room(t, {{p0, rect.y0, p1, rect.y1}}, props);
          const bool wide = p1 - p0 > 60;
          if (a.spec.lobby) {
            grid->add_door(*room, nullptr, near_door("entrance", js::min(16.0, p1 - p0 - 6), (p0 + p1) / 2, -12, "glass"));
            DoorOpts io;
            io.width = js::min(16.0, p1 - p0 - 6);
            io.place = "center";
            io.kind = "opening";
            io.leaf = "none";
            if (!grid->add_door(*room, corridor.get(), io)) connect(*grid, *room, corridor.get());
          } else {
            DoorOpts co;
            co.width = wide ? 12 : 8;
            co.leaf = "wood";  // (a.spec.leaf ?? "wood": no spec has a leaf)
            if (!connect(*grid, *room, corridor.get(), co)) {
              room->type = "shaft";
              continue;
            }
          }
          const std::string& ext = a.spec.ext;
          if (ext == "front" || ext == "back") {
            grid->add_door(*room, nullptr, near_door("entrance", 10, (p0 + p1) / 2, ext == "front" ? -12 : env.V + 12, ext == "front" ? "glass" : "metal"));
          } else if (ext == "rollupFront" || ext == "rollupBack") {
            const double w = js::min(28.0, p1 - p0 - 10);
            const double n_doors = js::max(1.0, std::floor((p1 - p0 + 1) / 40));
            for (double k = 0; k < n_doors; k += 1) {
              const double u = p0 + ((k + 0.5) * (p1 - p0)) / n_doors;
              DoorOpts o = near_door("rollup", w, u, ext == "rollupFront" ? -12 : env.V + 12, "rollup");
              o.margin = 3;
              o.height = js::min(30.0, env.story_h[0] - 6);
              const std::shared_ptr<Door> dr = grid->add_door(*room, nullptr, o);
              if (dr && a.spec.fire) dr->color = MAT::FIRE_RED;
            }
          }
        }
      }
    }
    return grid;
  };

  pb.add_floor(0, floor_plan(0), "ground");
  if (nF > 1) pb.add_floor(1, floor_plan(1), "upper");
  std::shared_ptr<FloorGrid> upper;
  for (double f = 2; f < nF; f += 1) {
    if (!upper) upper = floor_plan(f);
    pb.add_floor(f, upper, "upper");
  }
}

// ============================================================ small buildings

void plan_kiosk(const Envelope& env, Rng& rng, PlanBuilder& pb, const KioskProgram& P) {
  const std::shared_ptr<FloorGrid> g = pb.new_grid(0);
  const std::vector<Rect>& fps = tier_rects(env, 0);
  if (fps.empty()) SVX_FAIL("civic: an envelope without its ground floor");
  const Rect inner = inset(fps[0], EXT_T);
  const std::string kind = P.shop.empty() ? "grocery" : P.shop;
  shop_with_backroom(*g, &env, rng, inner, nullptr, &kind);
  pb.add_floor(0, g, "ground");
}

void plan_department_store(const Envelope& env, Rng& rng, PlanBuilder& pb) {
  const double nF = env.floors;
  const std::vector<Rect>& tops = tier_rects(env, nF - 1);
  if (tops.empty()) SVX_FAIL("civic: an envelope without its top floor");
  const Rect inner = inset(tops[0], EXT_T);
  double max_h = 0;
  for (double h : env.story_h) max_h = js::max(max_h, h);
  const StairDims sd = stair_dims(max_h);
  const OfficeCore core = office_core(inner, sd, nF);
  CoreLayout layout;
  for (const OfficeCoreComp& c : core.comps) {
    if (c.kind == "stair") {
      MakeStairOpts so;
      so.rect = c.rect;
      so.axis = 'v';
      so.dir = 1;
      so.lane_low = rng.chance(0.5);
      so.f0 = 0;
      so.f1 = nF;
      layout.stairs.push_back({c.rect, pb.add_stair(make_stair(so))});
    } else if (c.kind == "restroom") {
      layout.restrooms.push_back(c.rect);
    } else {
      for (double k = 0; k < core.n_elev; k += 1) {
        const double ex0 = c.rect.x0 + k * 17;
        const Rect rect{ex0, c.rect.y0, ex0 + 15, c.rect.y0 + 15};
        Elevator el;
        el.rect = rect;
        el.f0 = 0;
        el.f1 = nF - 1;
        el.door_side = 'N';
        layout.elevators.push_back({rect, pb.add_elevator(el)});
      }
      layout.shafts.push_back({c.rect.x0, c.rect.y0 + 17, c.rect.x1, c.rect.y1});
    }
  }
  std::shared_ptr<FloorGrid> typical;
  for (double f = 0; f < nF; f += 1) {
    if (f > 1 && typical) {
      pb.add_floor(f, typical, "store");
      continue;
    }
    const std::shared_ptr<FloorGrid> g = pb.new_grid(f);
    OpenFloorCores cores;
    for (const CoreLayoutStair& s : layout.stairs) {
      cores.stair_rooms.push_back(add_stair_room(*g, *s.stair));
      cores.stairs.push_back(s.stair.get());
    }
    for (const CoreLayoutElevator& e : layout.elevators) {
      RoomProps p;
      p.elevator = e.elev.id;
      cores.elev_rooms.push_back(g->add_room("elevator", {e.rect}, p));
    }
    for (const Rect& s : layout.shafts) g->add_room("shaft", {s});
    for (const Rect& r : layout.restrooms) cores.restrooms.push_back(g->add_room("restroom", {r}, finish("restroom")));
    const std::shared_ptr<Room> open = plan_open_floor(*g, env, rng, cores, "store");
    if (f == 0) g->add_door(*open, nullptr, near_door("entrance", 16, (inner.x0 + inner.x1) / 2, 0, "glass"));
    pb.add_floor(f, g, "store");
    if (f == 1) typical = g;
  }
}

}  // namespace svx::city
