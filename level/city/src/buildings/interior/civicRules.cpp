// svx_city — voxel_city buildings/interior/civicRules.js.
#include "buildings/interior/civicRules.hpp"

#include "buildings/interior/civicPrefabs.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

// ---- the room context's own parts (furnish.js RoomCtx)

FurnishCtx::FurnishCtx(const Room& room_, const Rect& rect, double zf_, Rng rng_, bool cars_)
    : cars(cars_), room(room_), r(rect), zf(zf_), rng(rng_), W(rect.x1 - rect.x0 + 1), H(rect.y1 - rect.y0 + 1) {}

std::array<double, 2> FurnishCtx::cell(char side, double a, double b) const {
  switch (side) {
    case 'N': return {r.x0 + a, r.y0 + b};
    case 'S': return {r.x1 - a, r.y1 - b};
    case 'W': return {r.x0 + b, r.y1 - a};
    default: return {r.x1 - b, r.y0 + a};
  }
}

std::vector<char> FurnishCtx::long_sides() {
  std::vector<char> out = W >= H ? rng.shuffle(std::vector<char>{'N', 'S'}) : rng.shuffle(std::vector<char>{'E', 'W'});
  if (W >= H)
    out.insert(out.end(), {'E', 'W'});
  else
    out.insert(out.end(), {'N', 'S'});
  return out;
}

std::vector<char> FurnishCtx::short_sides() { return W >= H ? rng.shuffle(std::vector<char>{'E', 'W'}) : rng.shuffle(std::vector<char>{'N', 'S'}); }

namespace {

inline std::vector<char> S(char side) { return {side}; }
inline char OPP(char side) { return side == 'N' ? 'S' : side == 'S' ? 'N' : side == 'E' ? 'W' : 'E'; }
inline PrefabOpts metal() {
  PrefabOpts o;
  o.metal = true;
  return o;
}

// Rows of an item across a room, facing the `front` side (seats, desks): steps of da along the
// side and db away from it, starting at (a0, b0).
struct RowsOpts {
  double a0 = 6, b0 = 12, da = 10, db = 10;
  std::optional<double> w = std::nullopt;  // (null)
  double a_end = 6, b_end = 6;
  PrefabOpts prefab = {};
};
double rows(FurnishCtx& c, const char* key, char front, const RowsOpts& o) {
  const double L = c.side_len(front);
  const double D = c.side_depth(front);
  const double iw = o.w.value_or(6);
  double n = 0;
  for (double b = o.b0; b + o.db - 2 < D - o.b_end; b += o.db)
    for (double a = o.a0; a + iw <= L - o.a_end; a += o.da) {
      FreeOpts f{.side = front, .a = a, .b = b, .pad = 0, .exact = true, .prefab = o.prefab};
      if (o.w && js::truthy(*o.w)) f.w = *o.w;
      if (c.free(key, f)) n += 1;
    }
  return n;
}

// The side of a room furthest from its doors (a stage, a screen, an altar goes there).
char far_side(FurnishCtx& c) {
  bool found = false;
  char best_side = 'N';
  double best_score = 0;
  for (const char side : {'N', 'S', 'E', 'W'}) {
    double doors = 0;
    for (double a = 0; a < c.side_len(side); a += 1)
      if (c.wall_behind(side, a) == "door") doors += 1;
    const double score = doors * 100 - c.side_len(side) * 0.1;
    if (!found || score < best_score) {
      found = true;
      best_side = side;
      best_score = score;
    }
  }
  return best_side;
}

// Paintings along every free wall stretch, hung at even spacing.
void hang_paintings(FurnishCtx& c, double n = 12) {
  for (const char side : {'N', 'E', 'S', 'W'}) {
    const double L = c.side_len(side);
    for (double a = 4; a + 10 < L - 3; a += c.rng.int_(14, 20)) {
      if (n <= 0) return;
      const double w = c.rng.int_(6, 11);
      if (c.wall("painting", {.sides = S(side), .w = w, .at = WallAt::at(a), .keep_depth = 2})) n -= 1;
    }
  }
}

// ------------------------------------------------------------ hospital
void ward(FurnishCtx& c) {
  // beds head to the long walls, curtains between them, a chair and a cart by each
  const std::vector<char> sides = c.long_sides();
  for (size_t i = 0; i < sides.size() && i < 2; ++i) {
    const char side = sides[i];
    const double L = c.side_len(side);
    for (double a = 4; a + 10 < L - 3; a += 16) {
      const auto bed = c.wall("hospitalBed", {.sides = S(side), .at = WallAt::at(a), .keep_depth = 8});
      if (!bed) continue;
      c.wall("bedCurtain", {.sides = S(side), .at = WallAt::at(bed->a + bed->w + 3)});
    }
  }
  c.wall("medCart", {.at = WallAt::random()});
  c.wall("basin", {.at = WallAt::random()});
}

void emergency(FurnishCtx& c) {
  ward(c);
  c.wall("shelfUnit", {.at = WallAt::random(), .prefab = metal()});
}

void exam(FurnishCtx& c) {
  c.wall("examCouch", {.at = WallAt::start()});
  c.wall("desk", {.at = WallAt::end()});
  c.wall("shelfUnit", {.at = WallAt::random(), .prefab = metal()});
  c.wall("basin", {.at = WallAt::random()});
}

void operating(FurnishCtx& c) {
  c.free("opTable", {.range = 10});
  for (int k = 0; k < 3; ++k) c.wall("medCart", {.at = WallAt::random()});
  c.wall("shelfUnit", {.at = WallAt::random(), .prefab = metal()});
}

void radiology(FurnishCtx& c) {
  c.free("xray", {.range = 10});
  c.wall("desk", {.at = WallAt::start()});
}

void waiting(FurnishCtx& c) {
  for (const char side : c.long_sides()) {
    const double w = js::min(28, c.side_len(side) - 10);
    if (w >= 8) c.wall("waitingChairs", {.sides = S(side), .w = w, .at = WallAt::center()});
  }
  c.wall("plant", {.at = WallAt::start()});
  c.wall("plant", {.at = WallAt::end()});
}

void nurses(FurnishCtx& c) {
  const char side = c.long_sides()[0];
  PrefabOpts p;
  p.screen = false;
  c.wall("serviceCounter", {.sides = S(side), .w = js::min(20, c.side_len(side) - 8), .at = WallAt::center(), .prefab = p});
  c.wall("shelfUnit", {.at = WallAt::random(), .prefab = metal()});
  c.wall("medCart", {.at = WallAt::random()});
}

// ------------------------------------------------------------ police / fire / civic offices
void police_desk(FurnishCtx& c) {
  const char side = far_side(c);
  PrefabOpts p;
  p.top = MAT::POLICE_BLUE;
  c.wall("serviceCounter", {.sides = S(side), .w = js::min(24, c.side_len(side) - 8), .at = WallAt::center(), .prefab = p});
  const char s2 = OPP(side);
  c.wall("waitingChairs", {.sides = S(s2), .w = js::min(16, c.side_len(s2) - 12), .at = WallAt::start()});
  c.wall("plant", {.at = WallAt::random()});
}

void cell(FurnishCtx& c) {
  PrefabOpts p;
  p.steel = true;
  c.wall("bunk", {.at = WallAt::start(), .prefab = p});
  c.wall("steelToilet", {.at = WallAt::end()});
}

void interview(FurnishCtx& c) {
  c.free("diningTable", {.range = 4});
  c.wall("shelfUnit", {.at = WallAt::random()});
}

void briefing(FurnishCtx& c) {
  const char side = far_side(c);
  c.wall("whiteboard", {.sides = S(side), .at = WallAt::center(), .keep_depth = 2});
  rows(c, "schoolDesk", side, {.b0 = 12, .da = 9, .db = 11});
}

void evidence(FurnishCtx& c) {
  for (int k = 0; k < 6; ++k)
    if (!c.wall("shelfUnit", {.at = WallAt::random(), .prefab = metal()})) break;
  c.wall("boxes", {.at = WallAt::random()});
}

void locker_room(FurnishCtx& c) {
  for (int k = 0; k < 4; ++k)
    if (!c.wall("lockers", {.at = WallAt::random()})) break;
  c.free("bench", {.range = 8});
}

void garage_bay(FurnishCtx& c) {
  // painted bays, a vehicle in each (when vehicles are on), equipment on the walls
  const char side = c.W >= c.H ? 'N' : 'W';
  const double L = c.side_len(side);
  auto line = [&](double x0, double y0, double x1, double y1) { c.boxes.push_back({x0, y0, c.zf - 1, x1, y1, c.zf - 1, MAT::LINE_YELLOW}); };
  for (double a = 26; a < L - 8; a += 26) {
    const auto p0 = c.cell(side, a, 4);
    const auto p1 = c.cell(side, a, c.side_depth(side) - 5);
    line(js::min(p0[0], p1[0]), js::min(p0[1], p1[1]), js::max(p0[0], p1[0]), js::max(p0[1], p1[1]));
  }
  const bool fire = c.room.fire;
  for (double a = 4; a + 22 < L; a += 26) {
    if (c.cars) c.free(fire ? "fireTruck" : "car", {.side = OPP(side), .a = a + 2, .b = 6, .pad = 0, .exact = true});
  }
  for (int k = 0; k < 3; ++k) c.wall(fire ? "hoseRack" : "shelfUnit", {.sides = S(side == 'N' ? 'S' : 'E'), .at = WallAt::random(), .prefab = metal()});
}

void dorm(FurnishCtx& c) {
  const std::vector<char> sides = c.long_sides();
  for (size_t i = 0; i < sides.size() && i < 2; ++i) {
    PrefabOpts p;
    p.double_ = true;
    for (int k = 0; k < 4; ++k)
      if (!c.wall("bunk", {.sides = S(sides[i]), .at = WallAt::random(), .prefab = p})) break;
  }
  c.wall("lockers", {.at = WallAt::random()});
}

void council(FurnishCtx& c) {
  const char side = far_side(c);
  PrefabOpts p;
  p.screen = false;
  p.top = MAT::WOOD_DARK;
  c.wall("serviceCounter", {.sides = S(side), .w = js::min(30, c.side_len(side) - 10), .at = WallAt::center(), .prefab = p});
  PrefabOpts seat;
  seat.seat = MAT::LEATHER;
  rows(c, "seatRow", side, {.b0 = 16, .da = 30, .db = 7, .w = js::min(24, c.side_len(side) - 14), .prefab = seat});
  c.wall("plant", {.at = WallAt::start()});
}

void registry(FurnishCtx& c) {
  c.rule("office");
  c.wall("shelfUnit", {.at = WallAt::random()});
}

// ------------------------------------------------------------ museum / gallery
void exhibit(FurnishCtx& c) {
  // vitrines along the walls, display cases in rows, a skeleton in a big hall
  for (int k = 0; k < 6; ++k)
    if (!c.wall("vitrine", {.at = WallAt::random(), .keep_depth = 10})) break;
  if (c.W >= 60 && c.H >= 50 && c.rng.chance(0.6))
    c.free("skeleton", {.side = c.W >= c.H ? 'E' : 'N', .range = 8});
  else
    c.free("sculpture", {.range = 8});
  for (double b = 14; b + 5 < c.H - 12; b += 16)
    for (double a = 14; a + 7 < c.W - 12; a += 18) c.free("displayCase", {.a = a, .b = b, .exact = true});
  c.free("bench", {.range = 20});
}

void gallery(FurnishCtx& c) {
  hang_paintings(c, 14);
  const double n = c.area() > 1500 ? 3 : 1;
  for (double k = 0; k < n; k += 1) c.free("sculpture", {.a = js::round(c.W * (0.3 + 0.2 * k)), .range = 20});
  if (c.area() > 900) c.free("bench", {.range = 6});
}

void museum_shop(FurnishCtx& c) {
  PrefabOpts p;
  p.goods_list = std::vector<uint16_t>{MAT::BOOKS, MAT::GOODS, MAT::ART_BLUE};
  for (int k = 0; k < 4; ++k)
    if (!c.wall("goodsShelf", {.at = WallAt::random(), .prefab = p})) break;
  c.wall("checkout", {.at = WallAt::start()});
}

void cloakroom(FurnishCtx& c) {
  const char side = far_side(c);
  PrefabOpts p;
  p.screen = false;
  c.wall("serviceCounter", {.sides = S(side), .w = js::min(20, c.side_len(side) - 8), .at = WallAt::center(), .prefab = p});
  c.free("clothesRack", {.side = side, .b = 1, .range = 6});
}

// ------------------------------------------------------------ venues
void auditorium(FurnishCtx& c) {
  // a stage at the far wall, rows of seats facing it, a mixing desk at the back
  const char side = far_side(c);
  const double L = c.side_len(side);
  const double d = js::min(28, js::max(12, js::round(c.side_depth(side) * 0.22)));
  PrefabOpts p;
  p.band = !c.room.classical;
  p.piano = c.room.classical;
  p.curtain = MAT::VELVET_RED;
  c.wall("stage", {.sides = S(side), .w = L - 4, .d = d, .at = WallAt::center(), .keep_depth = 8, .prefab = p});
  const double w = js::min(36, std::floor((L - 14) / 2));
  if (w >= 9) {
    rows(c, "seatRow", side, {.a0 = 4, .b0 = d + 8, .da = w + 6, .db = 6, .w = w, .b_end = 14});
  }
  c.free("mixingDesk", {.side = OPP(side), .b = 4, .range = 10});
}

void venue_floor(FurnishCtx& c) {
  // a club: stage in a corner, bar along a side wall, standing room, a few tables
  const char side = far_side(c);
  const double L = c.side_len(side);
  const double d = js::min(18, js::max(10, js::round(c.side_depth(side) * 0.25)));
  PrefabOpts p;
  p.band = true;
  p.curtain = MAT::STAGE_BLACK;
  c.wall("stage", {.sides = S(side), .w = js::min(L - 6, 44), .d = d, .at = WallAt::center(), .keep_depth = 8, .prefab = p});
  for (const char s2 : {'E', 'W', 'N', 'S'}) {
    if (s2 == side || s2 == OPP(side)) continue;
    const double w = js::min(30, c.side_len(s2) - d - 12);
    if (w >= 10 && c.wall("barCounter", {.sides = S(s2), .w = w, .at = WallAt::end()})) break;
  }
  for (double k = 0; k < 4; k += 1) c.free("cafeTable", {.side = OPP(side), .a = 8 + k * 10, .b = 6, .range = 10});
}

void cinema(FurnishCtx& c) {
  const char side = far_side(c);
  const double L = c.side_len(side);
  PrefabOpts p;
  p.h = 26;
  c.wall("projectionScreen", {.sides = S(side), .w = L - 8, .at = WallAt::center(), .keep_depth = 8, .prefab = p});
  const double w = js::min(40, L - 14);
  rows(c, "seatRow", side, {.a0 = js::round((L - w) / 2), .b0 = 16, .da = 100, .db = 6, .w = w, .b_end = 8});
}

void foyer_bar(FurnishCtx& c) {
  const char side = c.long_sides()[0];
  c.wall("barCounter", {.sides = S(side), .w = js::min(28, c.side_len(side) - 10), .at = WallAt::center()});
  c.wall("bottleShelf", {.sides = S(OPP(side)), .at = WallAt::center()});
  for (double k = 0; k < 5; k += 1)
    c.free("cafeTable", {.a = 8 + std::fmod(k * 13, js::max(8, c.W - 16)), .b = 8 + std::fmod(k * 7, js::max(8, c.H - 16)), .range = 20});
}

void dressing(FurnishCtx& c) {
  for (int k = 0; k < 3; ++k)
    if (!c.wall("barberChair", {.at = WallAt::random()})) break;
  c.free("clothesRack", {.range = 10});
  c.wall("sofa", {.at = WallAt::random()});
}

// ------------------------------------------------------------ supermarket / market
void sales(FurnishCtx& c) {
  // fridge cabinets along the back, aisles of gondolas, produce by the entrance, checkouts at the front
  const char front = c.room.front.value_or('N');
  const char back = OPP(front);
  const double L = c.side_len(back);
  for (double a = 4; a + 10 < L - 4; a += 11) c.wall("fridgeCase", {.sides = S(back), .at = WallAt::at(a), .keep_depth = 10});
  const double D = c.side_depth(front);
  for (double a = 8; a + 12 < L - 8; a += 14) c.free("checkout", {.side = front, .a = a, .b = 6, .pad = 1, .exact = true});
  for (double a = 10; a + 16 < L - 10; a += 22)
    for (double b = 26; b + 5 < D - 16; b += 13) c.free("gondola", {.side = front, .a = a, .b = b, .pad = 1, .exact = true});
  c.free("produceStand", {.side = front, .a = 6, .b = 18, .range = 6});
  c.free("freezerChest", {.side = front, .a = L - 20, .b = 18, .range = 6});
  c.wall("trolleys", {.sides = S(front), .at = WallAt::end()});
}

void market_hall(FurnishCtx& c) {
  // stalls in rows: counters of bread, meat, fish, produce, flowers
  const char side = c.W >= c.H ? 'N' : 'W';
  const double L = c.side_len(side);
  const double D = c.side_depth(side);
  const uint16_t goods[] = {MAT::FISH, MAT::MEAT, MAT::BREAD, MAT::PRODUCE_RED, MAT::PRODUCE_GREEN, MAT::FLOWER_YELLOW};
  size_t k = 0;
  for (double b = 10; b + 6 < D - 10; b += 18)
    for (double a = 8; a + 16 < L - 8; a += 22) {
      PrefabOpts p;
      p.goods = goods[k++ % 6];
      c.free("marketCounter", {.side = side, .a = a, .b = b, .pad = 1, .exact = true, .prefab = p});
    }
}

// ------------------------------------------------------------ shops (ground-floor tenants)
void bakery(FurnishCtx& c) {
  const char side = far_side(c);
  PrefabOpts p;
  p.goods = MAT::BREAD;
  c.wall("shopCounter", {.sides = S(side), .w = js::min(20, c.side_len(side) - 8), .at = WallAt::center(), .prefab = p});
  PrefabOpts g;
  g.goods_list = std::vector<uint16_t>{MAT::BREAD};
  c.wall("goodsShelf", {.at = WallAt::random(), .prefab = g});
  c.free("cafeTable", {.range = 10});
}

void butcher(FurnishCtx& c) {
  const char side = far_side(c);
  PrefabOpts p;
  p.goods = MAT::MEAT;
  c.wall("shopCounter", {.sides = S(side), .w = js::min(22, c.side_len(side) - 8), .at = WallAt::center(), .prefab = p});
  c.wall("fridgeCase", {.at = WallAt::random()});
}

void fishmonger(FurnishCtx& c) {
  const char side = far_side(c);
  PrefabOpts p;
  p.goods = MAT::FISH;
  c.wall("shopCounter", {.sides = S(side), .w = js::min(22, c.side_len(side) - 8), .at = WallAt::center(), .prefab = p});
  c.wall("fridgeCase", {.at = WallAt::random()});
}

void pharmacy(FurnishCtx& c) {
  const char side = far_side(c);
  PrefabOpts p;
  p.screen = false;
  p.top = MAT::PLASTIC_WHITE;
  c.wall("serviceCounter", {.sides = S(side), .w = js::min(18, c.side_len(side) - 10), .at = WallAt::center(), .prefab = p});
  PrefabOpts g;
  g.goods_list = std::vector<uint16_t>{MAT::PLASTIC_WHITE, MAT::CURTAIN_BLUE, MAT::MEDICAL_GREEN};
  g.frame = MAT::PLASTIC_WHITE;
  for (int k = 0; k < 4; ++k)
    if (!c.wall("goodsShelf", {.at = WallAt::random(), .prefab = g})) break;
}

void bookshop(FurnishCtx& c) {
  for (int k = 0; k < 6; ++k)
    if (!c.wall("bookshelf", {.at = WallAt::random()})) break;
  c.free("diningTable", {.range = 8});
  c.wall("checkout", {.at = WallAt::start()});
}

void clothing(FurnishCtx& c) {
  for (double k = 0; k < 4; k += 1) c.free("clothesRack", {.a = 6 + k * 12, .b = js::round(c.H / 2), .range = 24});
  c.wall("dresser", {.at = WallAt::random()});
  c.wall("checkout", {.at = WallAt::start()});
}

void florist(FurnishCtx& c) {
  for (int k = 0; k < 3; ++k) c.wall("flowerBuckets", {.at = WallAt::random()});
  c.wall("plant", {.at = WallAt::start()});
  c.wall("plant", {.at = WallAt::end()});
  c.wall("checkout", {.at = WallAt::random()});
}

void hardware(FurnishCtx& c) {
  PrefabOpts p;
  p.metal = true;
  p.goods = MAT::METAL_PANEL_DARK;
  for (int k = 0; k < 5; ++k)
    if (!c.wall("shelfUnit", {.at = WallAt::random(), .prefab = p})) break;
  c.free("gondola", {.range = 12});
  c.wall("checkout", {.at = WallAt::start()});
}

void barber(FurnishCtx& c) {
  const char side = c.long_sides()[0];
  for (int k = 0; k < 3; ++k)
    if (!c.wall("barberChair", {.sides = S(side), .at = WallAt::random()})) break;
  c.wall("waitingChairs", {.sides = S(OPP(side)), .w = 12, .at = WallAt::start()});
}

void pub(FurnishCtx& c) {
  const char side = c.long_sides()[0];
  c.wall("barCounter", {.sides = S(side), .w = js::min(28, c.side_len(side) - 10), .at = WallAt::center()});
  c.wall("bottleShelf", {.sides = S(OPP(side)), .at = WallAt::center()});
  if (c.area() > 900) c.free("poolTable", {.range = 12});
  for (double k = 0; k < 5; k += 1)
    c.free("cafeTable", {.a = 6 + std::fmod(k * 11, js::max(8, c.W - 12)), .b = 6 + std::fmod(k * 9, js::max(8, c.H - 12)), .range = 18});
}

void bank(FurnishCtx& c) {
  const char side = far_side(c);
  c.wall("serviceCounter", {.sides = S(side), .w = js::min(24, c.side_len(side) - 8), .at = WallAt::center()});
  c.wall("atm", {.sides = S(OPP(side)), .at = WallAt::start()});
  c.wall("plant", {.at = WallAt::random()});
  c.free("bench", {.range = 8});
}

void laundromat(FurnishCtx& c) {
  const std::vector<char> sides = c.long_sides();
  for (size_t i = 0; i < sides.size() && i < 2; ++i)
    for (int k = 0; k < 5; ++k)
      if (!c.wall("washer", {.sides = S(sides[i]), .at = WallAt::start()})) break;
  c.free("bench", {.range = 6});
}

void grocery(FurnishCtx& c) {
  for (int k = 0; k < 3; ++k)
    if (!c.wall("fridgeCase", {.at = WallAt::random()})) break;
  for (int k = 0; k < 4; ++k)
    if (!c.wall("goodsShelf", {.at = WallAt::random()})) break;
  c.free("produceStand", {.range = 10});
  c.wall("checkout", {.at = WallAt::start()});
}

void produkty(FurnishCtx& c) {
  // a Soviet-style grocery: long counters, goods on the shelves behind them
  const char side = far_side(c);
  PrefabOpts g;
  g.goods_list = std::vector<uint16_t>{MAT::BOTTLES, MAT::GOODS, MAT::BREAD};
  c.wall("goodsShelf", {.sides = S(side), .at = WallAt::start(), .prefab = g});
  c.wall("goodsShelf", {.sides = S(side), .at = WallAt::end(), .prefab = g});
  PrefabOpts p;
  p.goods = MAT::MEAT;
  c.free("marketCounter", {.side = side, .b = 6, .range = 8, .prefab = p});
  c.wall("checkout", {.sides = S(OPP(side)), .at = WallAt::start()});
}

void souvenir(FurnishCtx& c) {
  PrefabOpts g;
  g.goods_list = std::vector<uint16_t>{MAT::ART_BLUE, MAT::ART_CRIMSON, MAT::GOODS, MAT::BOOKS};
  for (int k = 0; k < 4; ++k)
    if (!c.wall("goodsShelf", {.at = WallAt::random(), .prefab = g})) break;
  c.free("clothesRack", {.range = 10});
  c.wall("checkout", {.at = WallAt::start()});
}

void kiosk(FurnishCtx& c) {
  PrefabOpts p;
  p.screen = false;
  c.wall("serviceCounter", {.w = js::min(14, c.side_len('N') - 6), .at = WallAt::center(), .prefab = p});
  PrefabOpts g;
  g.goods_list = std::vector<uint16_t>{MAT::BOOKS, MAT::GOODS, MAT::BOTTLES};
  c.wall("goodsShelf", {.at = WallAt::random(), .prefab = g});
  c.wall("fridgeCase", {.at = WallAt::random()});
}

// ------------------------------------------------------------ hotel / culture
void hotel_room(FurnishCtx& c) {
  c.rule("bedroom");
  c.wall("tvUnit", {.at = WallAt::random()});
  c.wall("armchair", {.at = WallAt::random()});
}

void clubroom(FurnishCtx& c) {
  c.free("meetingTable", {.w = js::max(10, js::min(c.W - 16, 30)), .d = js::max(6, js::min(c.H - 16, 10)), .range = 4});
  c.wall("bookshelf", {.at = WallAt::random()});
  if (c.rng.chance(0.4)) c.wall("tvUnit", {.at = WallAt::random()});
}

void dance_hall(FurnishCtx& c) {
  const char side = c.long_sides()[0];
  c.wall("danceBarre", {.sides = S(side), .w = c.side_len(side) - 10, .at = WallAt::center(), .keep_depth = 4});
  c.free("grandPiano", {.side = OPP(side), .a = 6, .b = 4, .range = 6});
}

// ------------------------------------------------------------ warehouses
void self_storage(FurnishCtx& c) {
  const char side = c.W >= c.H ? 'N' : 'W';
  const double L = c.side_len(side);
  const double D = c.side_depth(side);
  for (double b = 10; b + 12 < D - 8; b += 22) c.free("storageUnits", {.side = side, .a = 8, .b = b, .w = L - 16, .exact = true});
}

void cold_store(FurnishCtx& c) {
  c.rule("warehouse");
  for (int k = 0; k < 3; ++k) c.free("freezerChest", {.range = 30});
}

void distribution(FurnishCtx& c) {
  const char side = c.W >= c.H ? 'N' : 'W';
  const double L = c.side_len(side);
  const double D = c.side_depth(side);
  c.free("conveyor", {.side = side, .a = 10, .b = 14, .w = L - 20, .exact = true});
  for (double b = 28; b + 9 < D - 10; b += 34)
    for (double a = 10; a + 22 < L - 10; a += 26) {
      PrefabOpts p;
      p.h = 34;
      c.free("rack", {.side = side, .a = a, .b = b, .pad = 0, .exact = true, .prefab = p});
    }
  for (int k = 0; k < 8; ++k) {
    const double a = c.rng.int_(4, js::max(5, L - 12));
    const double b = c.rng.int_(4, js::max(5, D - 12));
    c.free("crates", {.a = a, .b = b, .range = 40});
  }
}

void timber_yard(FurnishCtx& c) {
  const char side = c.W >= c.H ? 'N' : 'W';
  const double L = c.side_len(side);
  const double D = c.side_depth(side);
  for (double b = 10; b + 8 < D - 8; b += 16)
    for (double a = 8; a + 20 < L - 8; a += 26) c.free("lumberStack", {.side = side, .a = a, .b = b, .exact = true});
}

// ------------------------------------------------------------ churches
void orthodox_nave(FurnishCtx& c) {
  // an icon screen across the east end, candle stands, no pews
  const char side = far_side(c);
  c.wall("icon", {.sides = S(side), .w = c.side_len(side) - 4, .at = WallAt::center(), .keep_depth = 6});
  for (int k = 0; k < 3; ++k) c.free("candleStand", {.range = 14});
}

// ---- RULE_PREFABS
using Boxes = std::vector<PrefabBox>;

Boxes market_counter(Rng& rng, const PrefabOpts& opt) {
  const uint16_t goods = opt.goods ? *opt.goods : static_cast<uint16_t>(MAT::PRODUCE_GREEN);
  const uint16_t awnings[] = {MAT::AWNING_RED, MAT::AWNING_GREEN, MAT::AWNING_BLUE, MAT::AWNING_STRIPE};
  return {
      {0, 15, 0, 5, 0, 5, MAT::WOOD_MED},
      {0, 15, 0, 5, 6, 6, goods},
      {0, 0, 0, 0, 7, 16, MAT::WOOD_DARK},
      {15, 15, 0, 0, 7, 16, MAT::WOOD_DARK},
      {0, 15, 0, 5, 17, 17, rng.pick(awnings)},
  };
}

Boxes candle_stand(Rng&, const PrefabOpts&) {
  return {
      {1, 1, 1, 1, 0, 6, MAT::GOLD},
      {0, 2, 0, 2, 7, 7, MAT::GOLD},
      {0, 2, 0, 2, 8, 8, MAT::LAMP_LIGHT},
  };
}

}  // namespace

const std::vector<CivicRule>& civic_rules() {
  static const std::vector<CivicRule> table = {
      {"ward", ward},
      {"emergency", emergency},
      {"exam", exam},
      {"operating", operating},
      {"radiology", radiology},
      {"waiting", waiting},
      {"nurses", nurses},
      {"policeDesk", police_desk},
      {"cell", cell},
      {"interview", interview},
      {"briefing", briefing},
      {"evidence", evidence},
      {"lockerRoom", locker_room},
      {"garageBay", garage_bay},
      {"dorm", dorm},
      {"council", council},
      {"registry", registry},
      {"exhibit", exhibit},
      {"gallery", gallery},
      {"museumShop", museum_shop},
      {"cloakroom", cloakroom},
      {"auditorium", auditorium},
      {"venueFloor", venue_floor},
      {"cinema", cinema},
      {"foyerBar", foyer_bar},
      {"dressing", dressing},
      {"sales", sales},
      {"marketHall", market_hall},
      {"bakery", bakery},
      {"butcher", butcher},
      {"fishmonger", fishmonger},
      {"pharmacy", pharmacy},
      {"bookshop", bookshop},
      {"clothing", clothing},
      {"florist", florist},
      {"hardware", hardware},
      {"barber", barber},
      {"pub", pub},
      {"bank", bank},
      {"laundromat", laundromat},
      {"grocery", grocery},
      {"produkty", produkty},
      {"souvenir", souvenir},
      {"kiosk", kiosk},
      {"hotelRoom", hotel_room},
      {"clubroom", clubroom},
      {"danceHall", dance_hall},
      {"selfStorage", self_storage},
      {"coldStore", cold_store},
      {"distribution", distribution},
      {"timberYard", timber_yard},
      {"orthodoxNave", orthodox_nave},
  };
  return table;
}

FurnishRule civic_rule(std::string_view type) {
  for (const CivicRule& r : civic_rules())
    if (type == r.type) return r.run;
  return nullptr;
}

const std::vector<Prefab>& rule_prefabs() {
  static const std::vector<Prefab> table = {
      {.id = "marketCounter", .w = 16, .d = 6, .free = true, .pad = 3, .build = market_counter},
      {.id = "candleStand", .w = 3, .d = 3, .free = true, .pad = 3, .build = candle_stand},
  };
  return table;
}

const Prefab* rule_prefab(std::string_view key) {
  for (const Prefab& p : rule_prefabs())
    if (key == p.id) return &p;
  return nullptr;
}

const Prefab* furnish_prefab(std::string_view key) {
  if (const Prefab* p = rule_prefab(key)) return p;
  if (const Prefab* p = civic_prefab(key)) return p;
  return prefab(key);
}

}  // namespace svx::city
