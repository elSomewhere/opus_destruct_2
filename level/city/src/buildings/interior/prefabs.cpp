// svx_city — voxel_city buildings/interior/prefabs.js.
#include "buildings/interior/prefabs.hpp"

#include <initializer_list>

#include "voxel/materials.hpp"

namespace svx::city {

namespace {

using Boxes = std::vector<PrefabBox>;

inline PrefabBox B(double a0, double a1, double b0, double b1, double z0, double z1, uint16_t m) { return {a0, a1, b0, b1, z0, z1, m}; }
// (an option JS reads as a number: undefined is NaN in its arithmetic)
inline double num(const std::optional<double>& v) { return v ? *v : js::kNaN; }
inline bool not_false(const std::optional<bool>& v) { return !(v && !*v); }
inline bool truthy(const std::optional<bool>& v) { return v && *v; }
// rng.pick([...]) of a literal list
inline uint16_t pick(Rng& rng, std::initializer_list<uint16_t> items) {
  return items.begin()[static_cast<size_t>(std::floor(rng.next() * static_cast<double>(items.size())))];
}

constexpr uint16_t kFabrics[] = {MAT::FABRIC_GRAY, MAT::FABRIC_BLUE, MAT::FABRIC_RED, MAT::FABRIC_GREEN, MAT::FABRIC_BEIGE, MAT::FABRIC_MUSTARD, MAT::LEATHER};
constexpr uint16_t kWoods[] = {MAT::WOOD_LIGHT, MAT::WOOD_MED, MAT::WOOD_DARK, MAT::LAMINATE_WHITE};
constexpr uint16_t kSheets[] = {MAT::BEDSHEET_BLUE, MAT::BEDSHEET_GREEN, MAT::BEDSHEET_WHITE, MAT::FABRIC_BEIGE, MAT::FABRIC_RED};

Boxes bed_double(Rng& rng, const PrefabOpts&) {
  const uint16_t wood = rng.pick(kWoods);
  const uint16_t sheet = rng.pick(kSheets);
  return {
      B(0, 12, 0, 16, 0, 1, wood),           B(0, 12, 0, 0, 0, 6, wood),        B(0, 12, 1, 16, 2, 2, MAT::MATTRESS),
      B(0, 12, 5, 16, 3, 3, sheet),          B(1, 5, 1, 3, 3, 3, MAT::PILLOW),  B(7, 11, 1, 3, 3, 3, MAT::PILLOW),
  };
}

Boxes bed_single(Rng& rng, const PrefabOpts&) {
  const uint16_t wood = rng.pick(kWoods);
  return {
      B(0, 7, 0, 15, 0, 1, wood),
      B(0, 7, 0, 0, 0, 5, wood),
      B(0, 7, 1, 15, 2, 2, MAT::MATTRESS),
      B(0, 7, 5, 15, 3, 3, rng.pick(kSheets)),
      B(1, 6, 1, 3, 3, 3, MAT::PILLOW),
  };
}

Boxes nightstand(Rng& rng, const PrefabOpts&) {
  const uint16_t wood = rng.pick(kWoods);
  return {B(0, 3, 0, 2, 0, 3, wood), B(1, 2, 1, 1, 4, 5, MAT::LAMP_SHADE)};
}

Boxes wardrobe(Rng& rng, const PrefabOpts&) {
  const uint16_t wood = rng.pick(kWoods);
  return {B(0, 9, 0, 4, 0, 15, wood), B(4, 5, 4, 4, 7, 9, MAT::METAL_CHROME)};
}

Boxes dresser(Rng& rng, const PrefabOpts&) {
  const uint16_t wood = rng.pick(kWoods);
  return {B(0, 7, 0, 3, 0, 5, wood), B(1, 6, 0, 0, 8, 12, MAT::MIRROR)};
}

Boxes desk(Rng& rng, const PrefabOpts& opt) {
  const uint16_t wood = rng.pick(kWoods);
  Boxes out = {B(0, 9, 0, 4, 5, 5, wood), B(0, 0, 0, 4, 0, 4, wood), B(9, 9, 0, 4, 0, 4, wood)};
  if (not_false(opt.monitor)) {
    out.push_back(B(3, 6, 1, 1, 6, 9, MAT::SCREEN));
    out.push_back(B(4, 5, 2, 2, 6, 6, MAT::PLASTIC_BLACK));
  }
  // chair
  out.push_back(B(3, 6, 6, 9, 3, 3, MAT::FABRIC_GRAY));
  out.push_back(B(3, 6, 9, 9, 4, 7, MAT::FABRIC_GRAY));
  out.push_back(B(4, 5, 7, 8, 0, 2, MAT::METAL_BLACK));
  return out;
}

Boxes sofa(Rng& rng, const PrefabOpts&) {
  const uint16_t f = rng.pick(kFabrics);
  return {B(0, 15, 0, 6, 0, 2, f), B(0, 15, 0, 1, 3, 6, f), B(0, 0, 2, 6, 3, 4, f), B(15, 15, 2, 6, 3, 4, f), B(1, 14, 2, 6, 3, 3, f)};
}

Boxes armchair(Rng& rng, const PrefabOpts&) {
  const uint16_t f = rng.pick(kFabrics);
  return {B(0, 6, 0, 6, 0, 2, f), B(0, 6, 0, 1, 3, 6, f), B(0, 0, 2, 6, 3, 4, f), B(6, 6, 2, 6, 3, 4, f)};
}

Boxes tv_unit(Rng& rng, const PrefabOpts&) {
  const uint16_t wood = rng.pick(kWoods);
  return {B(0, 11, 0, 2, 0, 3, wood), B(1, 10, 1, 1, 4, 9, MAT::SCREEN_OFF), B(5, 6, 1, 1, 4, 4, MAT::PLASTIC_BLACK)};
}

Boxes bookshelf(Rng& rng, const PrefabOpts&) {
  const uint16_t wood = rng.pick(kWoods);
  Boxes out = {B(0, 6, 0, 2, 0, 15, wood)};
  for (double z = 1; z <= 13; z += 3) out.push_back(B(1, 5, 1, 2, z, z + 1, MAT::BOOKS));
  return out;
}

Boxes plant(Rng&, const PrefabOpts&) {
  return {B(0, 2, 0, 2, 0, 2, MAT::PLANT_POT), B(0, 2, 0, 2, 3, 7, MAT::PLANT), B(1, 1, 1, 1, 8, 9, MAT::PLANT)};
}

Boxes floor_lamp(Rng&, const PrefabOpts&) {
  return {B(0, 1, 0, 1, 0, 0, MAT::METAL_BLACK), B(0, 0, 0, 0, 1, 11, MAT::METAL_BLACK), B(0, 1, 0, 1, 12, 13, MAT::LAMP_SHADE)};
}

// variable width: counters with sink + stove, upper cabinets
Boxes counter_run(Rng& rng, const PrefabOpts& opt) {
  const double w = num(opt.w);
  const uint16_t cab = pick(rng, {MAT::LAMINATE_WHITE, MAT::WOOD_LIGHT, MAT::LAMINATE_GRAY, MAT::WOOD_DARK});
  const uint16_t top = pick(rng, {MAT::COUNTERTOP, MAT::COUNTERTOP_DARK});
  Boxes out = {B(0, w - 1, 0, 4, 0, 5, cab), B(0, w - 1, 0, 4, 6, 6, top)};
  if (w >= 12) {
    const double s = std::floor(w * 0.3);
    out.push_back(B(s, s + 3, 1, 3, 6, 6, 0));
    out.push_back(B(s, s + 3, 1, 3, 5, 5, MAT::METAL_CHROME));
    out.push_back(B(s + 1, s + 2, 0, 0, 7, 8, MAT::METAL_CHROME));
    const double k = std::floor(w * 0.65);
    out.push_back(B(k, k + 3, 0, 4, 0, 5, MAT::APPLIANCE_STEEL));
    out.push_back(B(k, k + 3, 1, 3, 6, 6, MAT::METAL_BLACK));
    if (not_false(opt.hood)) out.push_back(B(k, k + 3, 0, 2, 13, 15, MAT::APPLIANCE_STEEL));
  }
  if (not_false(opt.upper)) out.push_back(B(0, w - 1, 0, 2, 11, 15, cab));
  return out;
}

Boxes fridge(Rng&, const PrefabOpts&) { return {B(0, 4, 0, 4, 0, 14, MAT::APPLIANCE_STEEL), B(4, 4, 4, 4, 6, 10, MAT::METAL_CHROME)}; }

Boxes dining_table(Rng& rng, const PrefabOpts&) {
  const uint16_t wood = rng.pick(kWoods);
  const uint16_t chair = pick(rng, {MAT::WOOD_DARK, MAT::WOOD_MED, MAT::PLASTIC_WHITE, MAT::FABRIC_GRAY});
  Boxes out = {B(0, 9, 0, 6, 5, 5, wood), B(1, 1, 1, 1, 0, 4, wood), B(8, 8, 1, 1, 0, 4, wood), B(1, 1, 5, 5, 0, 4, wood), B(8, 8, 5, 5, 0, 4, wood)};
  for (const double a : {1.0, 6.0}) {
    out.push_back(B(a, a + 2, -3, -1, 3, 3, chair));
    out.push_back(B(a, a + 2, -3, -3, 4, 7, chair));
    out.push_back(B(a, a + 2, 7, 9, 3, 3, chair));
    out.push_back(B(a, a + 2, 9, 9, 4, 7, chair));
  }
  return out;
}

Boxes coffee_table(Rng& rng, const PrefabOpts&) {
  const uint16_t wood = rng.pick(kWoods);
  return {B(0, 7, 0, 4, 2, 2, wood), B(0, 0, 0, 0, 0, 1, wood), B(7, 7, 0, 0, 0, 1, wood), B(0, 0, 4, 4, 0, 1, wood), B(7, 7, 4, 4, 0, 1, wood)};
}

Boxes rug(Rng& rng, const PrefabOpts&) { return {B(0, 13, 0, 9, -1, -1, pick(rng, {MAT::RUG_RED, MAT::RUG_BLUE, MAT::RUG_BEIGE}))}; }

Boxes toilet(Rng&, const PrefabOpts&) { return {B(0, 3, 0, 1, 2, 6, MAT::CERAMIC), B(1, 2, 2, 5, 0, 2, MAT::CERAMIC), B(1, 2, 3, 4, 2, 2, 0)}; }

Boxes basin(Rng&, const PrefabOpts&) {
  return {B(0, 4, 0, 3, 5, 6, MAT::CERAMIC), B(1, 3, 1, 2, 6, 6, 0), B(2, 2, 1, 1, 0, 4, MAT::CERAMIC), B(1, 3, 0, 0, 8, 12, MAT::MIRROR),
          B(2, 2, 0, 0, 7, 7, MAT::METAL_CHROME)};
}

Boxes bathtub(Rng&, const PrefabOpts&) {
  return {B(0, 13, 0, 5, 0, 3, MAT::CERAMIC), B(1, 12, 1, 4, 1, 3, 0), B(1, 12, 1, 4, 1, 1, MAT::WATER), B(12, 12, 0, 0, 4, 9, MAT::METAL_CHROME)};
}

Boxes shower(Rng&, const PrefabOpts&) {
  return {B(0, 6, 0, 6, 0, 0, MAT::CERAMIC), B(0, 6, 6, 6, 1, 15, MAT::GLASS), B(6, 6, 0, 6, 1, 15, MAT::GLASS), B(3, 3, 0, 0, 13, 14, MAT::METAL_CHROME)};
}

Boxes washer(Rng&, const PrefabOpts&) { return {B(0, 4, 0, 4, 0, 6, MAT::PLASTIC_WHITE), B(1, 3, 4, 4, 2, 4, MAT::GLASS_TINT)}; }

Boxes shelf_unit(Rng& rng, const PrefabOpts& opt) {
  const uint16_t frame = truthy(opt.metal) ? static_cast<uint16_t>(MAT::SHELF_METAL) : rng.pick(kWoods);
  const uint16_t goods = opt.goods ? *opt.goods : static_cast<uint16_t>(MAT::GOODS);
  Boxes out = {B(0, 0, 0, 2, 0, 14, frame), B(7, 7, 0, 2, 0, 14, frame)};
  for (double z = 0; z <= 12; z += 4) {
    out.push_back(B(0, 7, 0, 2, z, z, frame));
    if (rng.chance(0.85)) out.push_back(B(1, 6, 0, 2, z + 1, z + 2, goods));
  }
  return out;
}

// free-standing double-sided shop shelf
Boxes gondola(Rng& rng, const PrefabOpts&) {
  Boxes out = {B(0, 15, 0, 4, 0, 0, MAT::SHELF_METAL), B(0, 15, 2, 2, 1, 11, MAT::SHELF_METAL)};
  for (double z = 1; z <= 9; z += 4) {
    out.push_back(B(0, 15, 0, 1, z, z, MAT::SHELF_METAL));
    out.push_back(B(0, 15, 3, 4, z, z, MAT::SHELF_METAL));
    out.push_back(B(0, 15, 0, 1, z + 1, z + 2, pick(rng, {MAT::GOODS, MAT::BOOKS, MAT::CARDBOARD})));
    out.push_back(B(0, 15, 3, 4, z + 1, z + 2, pick(rng, {MAT::GOODS, MAT::BOOKS, MAT::CARDBOARD})));
  }
  return out;
}

Boxes checkout(Rng&, const PrefabOpts&) {
  return {B(0, 9, 0, 4, 0, 6, MAT::WOOD_DARK), B(6, 8, 1, 3, 7, 8, MAT::PLASTIC_BLACK), B(1, 4, 1, 3, 7, 7, MAT::METAL_CHROME)};
}

// pupil's desk with a chair behind it (local b grows towards the back of the room)
Boxes school_desk(Rng&, const PrefabOpts&) {
  return {
      B(0, 5, 0, 3, 5, 5, MAT::LAMINATE_WHITE), B(0, 0, 0, 0, 0, 4, MAT::METAL_BLACK),   B(5, 5, 0, 0, 0, 4, MAT::METAL_BLACK),
      B(0, 0, 3, 3, 0, 4, MAT::METAL_BLACK),    B(5, 5, 3, 3, 0, 4, MAT::METAL_BLACK),   B(1, 4, 5, 7, 3, 3, MAT::PLASTIC_BLACK),
      B(1, 4, 7, 7, 4, 7, MAT::PLASTIC_BLACK),
  };
}

Boxes chalkboard(Rng&, const PrefabOpts& opt) {
  const double w = opt.w.value_or(24);
  return {B(0, w - 1, 0, 0, 7, 16, MAT::CHALKBOARD), B(0, w - 1, 0, 0, 6, 6, MAT::WOOD_MED), B(0, w - 1, 0, 0, 17, 17, MAT::WOOD_MED)};
}

Boxes hoop(Rng&, const PrefabOpts&) {
  return {B(0, 5, 0, 0, 20, 25, MAT::PANEL_WHITE), B(2, 3, 1, 4, 20, 20, MAT::COURT_ORANGE), B(2, 3, 0, 0, 16, 19, MAT::METAL_BLACK)};
}

Boxes cafe_table(Rng& rng, const PrefabOpts&) {
  const uint16_t t = pick(rng, {MAT::WOOD_LIGHT, MAT::LAMINATE_WHITE, MAT::METAL_BLACK});
  const uint16_t c = pick(rng, {MAT::WOOD_DARK, MAT::METAL_BLACK, MAT::FABRIC_RED});
  return {B(0, 3, 0, 3, 5, 5, t),  B(1, 2, 1, 2, 0, 4, MAT::METAL_BLACK), B(-3, -1, 1, 2, 3, 3, c),
          B(-3, -3, 1, 2, 4, 6, c), B(4, 6, 1, 2, 3, 3, c),               B(6, 6, 1, 2, 4, 6, c)};
}

// counter + stools
Boxes bar_counter(Rng&, const PrefabOpts& opt) {
  const double w = num(opt.w);
  Boxes out = {B(0, w - 1, 0, 4, 0, 7, MAT::WOOD_DARK), B(0, w - 1, 0, 4, 8, 8, MAT::COUNTERTOP_DARK)};
  out.push_back(B(1, 3, 1, 3, 9, 11, MAT::APPLIANCE_STEEL));
  for (double a = 1; a < w - 1; a += 4) {
    out.push_back(B(a, a + 1, 6, 7, 4, 4, MAT::FABRIC_RED));
    out.push_back(B(a, a, 6, 6, 0, 3, MAT::METAL_CHROME));
  }
  return out;
}

// cluster of 4 desks facing each other with chairs and monitors
Boxes office_desks(Rng& rng, const PrefabOpts&) {
  const uint16_t top = pick(rng, {MAT::LAMINATE_WHITE, MAT::WOOD_LIGHT, MAT::LAMINATE_GRAY});
  const uint16_t chair = pick(rng, {MAT::FABRIC_GRAY, MAT::FABRIC_BLUE, MAT::PLASTIC_BLACK});
  Boxes out;
  const struct {
    double a0;
    bool flip;
  } desks[] = {{0, false}, {12, false}, {0, true}, {12, true}};
  for (const auto& dk : desks) {
    const double a0 = dk.a0;
    const bool flip = dk.flip;
    const double b0 = flip ? 11 : 5;
    out.push_back(B(a0, a0 + 11, b0, b0 + 5, 5, 5, top));
    out.push_back(B(a0, a0, b0, b0, 0, 4, MAT::METAL_CHROME));
    out.push_back(B(a0 + 11, a0 + 11, b0 + 5, b0 + 5, 0, 4, MAT::METAL_CHROME));
    const double mb = flip ? b0 + 1 : b0 + 4;
    out.push_back(B(a0 + 4, a0 + 7, mb, mb, 6, 8, MAT::SCREEN));
    const double cb = flip ? b0 + 6 : b0 - 4;
    out.push_back(B(a0 + 4, a0 + 7, cb, cb + 3, 3, 3, chair));
    out.push_back(B(a0 + 5, a0 + 6, cb + 1, cb + 2, 0, 2, MAT::METAL_BLACK));
    out.push_back(flip ? B(a0 + 4, a0 + 7, cb + 3, cb + 3, 4, 7, chair) : B(a0 + 4, a0 + 7, cb, cb, 4, 7, chair));
  }
  out.push_back(B(0, 23, 10, 10, 6, 7, MAT::PANEL_WHITE));
  return out;
}

// chairs stick out 4 cells
Boxes meeting_table(Rng& rng, const PrefabOpts& opt) {
  const double w = num(opt.w);
  const double d = num(opt.d);
  const uint16_t top = pick(rng, {MAT::WOOD_DARK, MAT::WOOD_LIGHT, MAT::LAMINATE_WHITE});
  const uint16_t chair = pick(rng, {MAT::FABRIC_GRAY, MAT::LEATHER, MAT::PLASTIC_BLACK});
  Boxes out = {B(0, w - 1, 0, d - 1, 5, 5, top), B(2, 2, 2, d - 3, 0, 4, MAT::METAL_CHROME), B(w - 3, w - 3, 2, d - 3, 0, 4, MAT::METAL_CHROME)};
  for (double a = 1; a + 3 < w; a += 5) {
    out.push_back(B(a, a + 2, -4, -2, 3, 3, chair));
    out.push_back(B(a, a + 2, -4, -4, 4, 7, chair));
    out.push_back(B(a, a + 2, d + 1, d + 3, 3, 3, chair));
    out.push_back(B(a, a + 2, d + 3, d + 3, 4, 7, chair));
  }
  return out;
}

Boxes reception_desk(Rng&, const PrefabOpts&) {
  return {B(0, 15, 0, 1, 0, 7, MAT::WOOD_DARK), B(0, 1, 0, 5, 0, 7, MAT::WOOD_DARK), B(0, 15, 0, 1, 8, 8, MAT::COUNTERTOP), B(6, 8, 3, 3, 5, 7, MAT::SCREEN),
          B(6, 9, 3, 5, 3, 3, MAT::FABRIC_GRAY)};
}

Boxes bench(Rng&, const PrefabOpts&) {
  return {B(0, 11, 0, 3, 3, 3, MAT::WOOD_MED), B(1, 1, 0, 3, 0, 2, MAT::METAL_BLACK), B(10, 10, 0, 3, 0, 2, MAT::METAL_BLACK)};
}

Boxes mailboxes(Rng&, const PrefabOpts&) { return {B(0, 9, 0, 1, 4, 11, MAT::METAL_CHROME), B(0, 9, 0, 0, 3, 3, MAT::METAL_BLACK)}; }

// warehouse pallet rack
Boxes rack(Rng& rng, const PrefabOpts& opt) {
  const double h = opt.h.value_or(40);
  Boxes out;
  for (const double a : {0.0, 21.0})
    for (const double b : {0.0, 8.0}) out.push_back(B(a, a, b, b, 0, h, MAT::SHELF_ORANGE));
  for (double z = 0; z <= h - 10; z += 12) {
    out.push_back(B(0, 21, 0, 0, z + 10, z + 10, MAT::SHELF_METAL));
    out.push_back(B(0, 21, 8, 8, z + 10, z + 10, MAT::SHELF_METAL));
    for (const double a : {1.0, 11.0}) {
      if (!rng.chance(0.8)) continue;
      out.push_back(B(a, a + 8, 1, 7, z, z, MAT::PALLET));
      const double bh = rng.int_(3, 8);
      // (MAT.CRATE ?? MAT.CARDBOARD: the palette has no CRATE)
      out.push_back(B(a + 1, a + 7, 1, 7, z + 1, z + bh, pick(rng, {MAT::CARDBOARD, MAT::CARDBOARD, MAT::CARDBOARD, MAT::PLASTIC_WHITE})));
    }
  }
  return out;
}

// a stall pen: wooden rails on three sides, straw on the floor
Boxes stall(Rng&, const PrefabOpts&) {
  return {
      B(0, 19, 0, 23, 0, 0, MAT::HAY),        B(0, 0, 0, 23, 1, 10, MAT::WOOD_DARK),   B(19, 19, 0, 23, 1, 10, MAT::WOOD_DARK),
      B(0, 19, 23, 23, 8, 10, MAT::WOOD_DARK), B(0, 19, 23, 23, 3, 4, MAT::WOOD_DARK), B(0, 0, 23, 23, 1, 10, MAT::WOOD_DARK),
      B(19, 19, 23, 23, 1, 10, MAT::WOOD_DARK), B(4, 10, 1, 3, 1, 4, MAT::WOOD_MED),
  };
}

// square bales stacked two to three high
Boxes hay_stack(Rng& rng, const PrefabOpts&) {
  Boxes out;
  const double n = rng.int_(2, 3);
  for (double k = 0; k < n; k += 1) out.push_back(B(0, 11, 0, 8, k * 5, k * 5 + 4, MAT::HAY));
  if (rng.chance(0.5)) out.push_back(B(2, 9, 1, 7, n * 5, n * 5 + 4, MAT::HAY));
  return out;
}

Boxes crates(Rng& rng, const PrefabOpts&) {
  Boxes out = {B(0, 7, 0, 7, 0, 0, MAT::PALLET), B(0, 7, 0, 7, 1, rng.int_(4, 9), MAT::CARDBOARD)};
  if (rng.chance(0.5)) out.push_back(B(1, 5, 1, 5, 10, 13, MAT::CARDBOARD));
  return out;
}

Boxes machine(Rng& rng, const PrefabOpts&) {
  const uint16_t c = pick(rng, {MAT::CONTAINER_BLUE, MAT::CONTAINER_GREEN, MAT::HAZARD_YELLOW, MAT::METAL_PANEL});
  return {B(0, 15, 0, 11, 0, 9, c), B(2, 13, 2, 9, 10, 12, MAT::METAL_PANEL_DARK), B(3, 5, -1, -1, 4, 7, MAT::SCREEN), B(12, 13, 4, 5, 13, 22, MAT::PIPE),
          B(-8, -1, 4, 7, 4, 4, MAT::METAL_BLACK)};
}

Boxes car(Rng& rng, const PrefabOpts&) { return car_boxes(rng); }

Boxes stall_toilet(Rng&, const PrefabOpts&) {
  return {B(0, 0, 0, 10, 0, 14, MAT::LAMINATE_GRAY), B(8, 8, 0, 10, 0, 14, MAT::LAMINATE_GRAY), B(3, 5, 0, 1, 2, 6, MAT::CERAMIC), B(4, 4, 2, 5, 0, 2, MAT::CERAMIC)};
}

Boxes lockers(Rng&, const PrefabOpts&) {
  Boxes out = {B(0, 11, 0, 2, 0, 15, MAT::METAL_PANEL)};
  for (double a = 2; a < 12; a += 3) out.push_back(B(a, a, 0, 0, 0, 15, MAT::METAL_PANEL_DARK));
  return out;
}

Boxes boxes(Rng& rng, const PrefabOpts&) { return {B(0, 5, 0, 4, 0, rng.int_(2, 5), MAT::CARDBOARD), B(1, 3, 1, 3, 6, 8, MAT::CARDBOARD)}; }

// church: a pew (backrest at b = 0, facing +b) and an altar with a cloth and a cross
Boxes pew(Rng&, const PrefabOpts& opt) {
  const double w = opt.w.value_or(16);
  return {B(0, w - 1, 1, 3, 3, 3, MAT::WOOD_DARK), B(0, w - 1, 0, 0, 0, 7, MAT::WOOD_DARK), B(0, 0, 1, 3, 0, 5, MAT::WOOD_DARK), B(w - 1, w - 1, 1, 3, 0, 5, MAT::WOOD_DARK)};
}

Boxes altar(Rng&, const PrefabOpts&) {
  return {B(0, 13, 1, 4, 0, 6, MAT::WOOD_DARK), B(0, 13, 1, 4, 7, 7, MAT::BEDSHEET_WHITE), B(6, 7, 0, 0, 0, 16, MAT::WOOD_DARK), B(4, 9, 0, 0, 13, 13, MAT::WOOD_DARK)};
}

}  // namespace

std::vector<PrefabBox> car_boxes(Rng& rng) {
  const uint16_t body = pick(rng, {MAT::CAR_RED, MAT::CAR_BLUE, MAT::CAR_WHITE, MAT::CAR_BLACK, MAT::CAR_SILVER, MAT::CAR_SILVER, MAT::CAR_GREEN, MAT::CAR_YELLOW});
  const double w = 14;
  const double l = rng.int_(30, 36);
  Boxes out = {
      B(0, w - 1, 2, l - 3, 2, 6, body),
      B(1, w - 2, 0, 1, 2, 5, body),
      B(1, w - 2, l - 2, l - 1, 2, 5, body),
      B(1, w - 2, 9, l - 10, 7, 10, body),
      B(2, w - 3, 8, 8, 7, 9, MAT::CAR_GLASS),
      B(2, w - 3, l - 9, l - 9, 7, 9, MAT::CAR_GLASS),
      B(1, 1, 10, l - 11, 7, 9, MAT::CAR_GLASS),
      B(w - 2, w - 2, 10, l - 11, 7, 9, MAT::CAR_GLASS),
      B(2, 4, 0, 0, 4, 5, MAT::HEADLIGHT),
      B(w - 5, w - 3, 0, 0, 4, 5, MAT::HEADLIGHT),
      B(2, 4, l - 1, l - 1, 4, 5, MAT::TAILLIGHT),
      B(w - 5, w - 3, l - 1, l - 1, 4, 5, MAT::TAILLIGHT),
  };
  for (const double b : {5.0, l - 8}) {
    out.push_back(B(-0.0, 1, b, b + 3, 0, 3, MAT::TIRE));
    out.push_back(B(w - 2, w - 1, b, b + 3, 0, 3, MAT::TIRE));
  }
  return out;
}

const std::vector<Prefab>& prefabs() {
  static const std::vector<Prefab> table = {
      {.id = "bedDouble", .w = 13, .d = 17, .build = bed_double},
      {.id = "bedSingle", .w = 8, .d = 16, .build = bed_single},
      {.id = "nightstand", .w = 4, .d = 3, .build = nightstand},
      {.id = "wardrobe", .w = 10, .d = 5, .tall = true, .build = wardrobe},
      {.id = "dresser", .w = 8, .d = 4, .build = dresser},
      {.id = "desk", .w = 10, .d = 5, .d_extra = 5, .build = desk},
      {.id = "sofa", .w = 16, .d = 7, .build = sofa},
      {.id = "armchair", .w = 7, .d = 7, .build = armchair},
      {.id = "tvUnit", .w = 12, .d = 3, .build = tv_unit},
      {.id = "bookshelf", .w = 7, .d = 3, .tall = true, .build = bookshelf},
      {.id = "plant", .w = 3, .d = 3, .build = plant},
      {.id = "floorLamp", .w = 2, .d = 2, .build = floor_lamp},
      {.id = "counterRun", .w = 0, .d = 5, .build = counter_run},
      {.id = "fridge", .w = 5, .d = 5, .tall = true, .build = fridge},
      {.id = "diningTable", .w = 10, .d = 7, .free = true, .pad = 3, .build = dining_table},
      {.id = "coffeeTable", .w = 8, .d = 5, .free = true, .build = coffee_table},
      {.id = "rug", .w = 14, .d = 10, .free = true, .flat = true, .build = rug},
      {.id = "toilet", .w = 4, .d = 6, .build = toilet},
      {.id = "basin", .w = 5, .d = 4, .build = basin},
      {.id = "bathtub", .w = 14, .d = 6, .build = bathtub},
      {.id = "shower", .w = 7, .d = 7, .build = shower},
      {.id = "washer", .w = 5, .d = 5, .build = washer},
      {.id = "shelfUnit", .w = 8, .d = 3, .tall = true, .build = shelf_unit},
      {.id = "gondola", .w = 16, .d = 5, .free = true, .pad = 5, .build = gondola},
      {.id = "checkout", .w = 10, .d = 5, .build = checkout},
      {.id = "schoolDesk", .w = 6, .d = 8, .free = true, .pad = 1, .build = school_desk},
      {.id = "chalkboard", .w = 24, .d = 1, .build = chalkboard},
      {.id = "hoop", .w = 6, .d = 5, .build = hoop},
      {.id = "cafeTable", .w = 4, .d = 4, .free = true, .pad = 4, .build = cafe_table},
      {.id = "barCounter", .w = 0, .d = 8, .build = bar_counter},
      {.id = "officeDesks", .w = 24, .d = 22, .free = true, .pad = 2, .build = office_desks},
      {.id = "meetingTable", .w = 0, .d = 0, .free = true, .pad = 5, .build = meeting_table},
      {.id = "receptionDesk", .w = 16, .d = 6, .free = true, .build = reception_desk},
      {.id = "bench", .w = 12, .d = 4, .build = bench},
      {.id = "mailboxes", .w = 10, .d = 2, .build = mailboxes},
      {.id = "rack", .w = 22, .d = 9, .tall = true, .build = rack},
      {.id = "stall", .w = 20, .d = 24, .build = stall},
      {.id = "hayStack", .w = 12, .d = 9, .build = hay_stack},
      {.id = "crates", .w = 8, .d = 8, .free = true, .pad = 3, .build = crates},
      {.id = "machine", .w = 16, .d = 12, .free = true, .pad = 6, .build = machine},
      {.id = "car", .w = 15, .d = 34, .free = true, .pad = 2, .build = car},
      {.id = "stallToilet", .w = 9, .d = 11, .build = stall_toilet},
      {.id = "lockers", .w = 12, .d = 3, .tall = true, .build = lockers},
      {.id = "boxes", .w = 6, .d = 5, .build = boxes},
      {.id = "pew", .w = 16, .d = 5, .build = pew},
      {.id = "altar", .w = 14, .d = 5, .build = altar},
  };
  return table;
}

const Prefab* prefab(std::string_view key) {
  for (const Prefab& p : prefabs())
    if (key == p.id) return &p;
  return nullptr;
}

}  // namespace svx::city
