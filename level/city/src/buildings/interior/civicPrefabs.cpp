// svx_city — voxel_city buildings/interior/civicPrefabs.js.
#include "buildings/interior/civicPrefabs.hpp"

#include <initializer_list>

#include "voxel/materials.hpp"

namespace svx::city {

namespace {

using Boxes = std::vector<PrefabBox>;

inline PrefabBox B(double a0, double a1, double b0, double b1, double z0, double z1, uint16_t m) { return {a0, a1, b0, b1, z0, z1, m}; }
inline double num(const std::optional<double>& v) { return v ? *v : js::kNaN; }
inline bool not_false(const std::optional<bool>& v) { return !(v && !*v); }
inline bool truthy(const std::optional<bool>& v) { return v && *v; }
inline uint16_t pick(Rng& rng, std::initializer_list<uint16_t> items) {
  return items.begin()[static_cast<size_t>(std::floor(rng.next() * static_cast<double>(items.size())))];
}
inline uint16_t pick(Rng& rng, const std::vector<uint16_t>& items) {
  return items[static_cast<size_t>(std::floor(rng.next() * static_cast<double>(items.size())))];
}

constexpr uint16_t kArt[] = {MAT::ART_BLUE, MAT::ART_OCHRE, MAT::ART_CRIMSON, MAT::ART_TEAL, MAT::FABRIC_MUSTARD, MAT::PAINT_DARK, MAT::PLAZA_STONE};

// ---------------------------------------------------------------- hospital
Boxes hospital_bed(Rng& rng, const PrefabOpts&) {
  return {
      B(0, 7, 0, 16, 2, 2, MAT::METAL_CHROME),
      B(0, 0, 0, 0, 0, 1, MAT::METAL_BLACK),
      B(7, 7, 0, 0, 0, 1, MAT::METAL_BLACK),
      B(0, 0, 16, 16, 0, 1, MAT::METAL_BLACK),
      B(7, 7, 16, 16, 0, 1, MAT::METAL_BLACK),
      B(0, 7, 0, 0, 3, 7, MAT::PLASTIC_WHITE),
      B(0, 7, 1, 16, 3, 3, MAT::MATTRESS),
      B(0, 7, 6, 16, 4, 4, pick(rng, {MAT::BEDSHEET_WHITE, MAT::BEDSHEET_BLUE, MAT::MEDICAL_GREEN})),
      B(1, 6, 1, 3, 4, 4, MAT::PILLOW),
      B(0, 0, 5, 12, 4, 5, MAT::METAL_CHROME),
      B(7, 7, 5, 12, 4, 5, MAT::METAL_CHROME),
      // drip stand
      B(9, 9, 2, 2, 0, 15, MAT::METAL_CHROME),
      B(9, 9, 2, 2, 13, 14, MAT::GLASS),
  };
}

Boxes bed_curtain(Rng&, const PrefabOpts&) { return {B(0, 0, 0, 16, 1, 15, MAT::CURTAIN_BLUE), B(0, 0, 0, 16, 16, 16, MAT::METAL_CHROME)}; }

Boxes med_cart(Rng& rng, const PrefabOpts&) {
  return {B(0, 4, 0, 3, 1, 6, pick(rng, {MAT::PLASTIC_WHITE, MAT::CURTAIN_BLUE})), B(0, 4, 0, 3, 7, 7, MAT::METAL_CHROME), B(0, 0, 0, 0, 0, 0, MAT::METAL_BLACK),
          B(4, 4, 3, 3, 0, 0, MAT::METAL_BLACK)};
}

Boxes exam_couch(Rng&, const PrefabOpts&) {
  return {B(0, 5, 0, 14, 0, 3, MAT::PLASTIC_WHITE), B(0, 5, 0, 14, 4, 4, MAT::MEDICAL_GREEN), B(0, 5, 0, 3, 5, 6, MAT::MEDICAL_GREEN),
          B(0, 5, 2, 12, 5, 5, MAT::BEDSHEET_WHITE)};
}

Boxes op_table(Rng&, const PrefabOpts&) {
  return {
      B(2, 3, 6, 9, 0, 5, MAT::METAL_CHROME),
      B(0, 5, 0, 15, 6, 6, MAT::MEDICAL_GREEN),
      B(0, 5, 0, 15, 5, 5, MAT::METAL_CHROME),
      // surgical lamp hanging from the ceiling
      B(2, 3, 7, 8, 16, 21, MAT::METAL_CHROME),
      B(0, 5, 5, 10, 15, 15, MAT::STAGE_LIGHT),
      // instrument trolley and monitor stack
      B(-5, -2, 2, 5, 0, 7, MAT::METAL_CHROME),
      B(8, 11, 1, 4, 0, 12, MAT::PLASTIC_WHITE),
      B(8, 11, 1, 1, 9, 12, MAT::SCREEN),
  };
}

Boxes xray(Rng&, const PrefabOpts&) {
  return {
      B(3, 8, 0, 13, 0, 5, MAT::PLASTIC_WHITE), B(3, 8, 0, 13, 6, 6, MAT::BEDSHEET_WHITE), B(0, 1, 5, 7, 0, 18, MAT::PLASTIC_WHITE),
      B(0, 7, 5, 7, 17, 18, MAT::PLASTIC_WHITE), B(4, 7, 5, 8, 14, 16, MAT::METAL_PANEL),
  };
}

Boxes waiting_chairs(Rng& rng, const PrefabOpts& opt) {
  const double w = num(opt.w);
  const uint16_t c = pick(rng, {MAT::PLASTIC_BLACK, MAT::FABRIC_BLUE, MAT::PLASTIC_WHITE, MAT::FABRIC_RED});
  Boxes out = {B(0, w - 1, 1, 3, 3, 3, MAT::METAL_CHROME)};
  for (double a = 0; a + 2 < w; a += 4) {
    out.push_back(B(a, a + 2, 1, 3, 4, 4, c));
    out.push_back(B(a, a + 2, 0, 0, 5, 8, c));
  }
  out.push_back(B(0, 0, 1, 3, 0, 2, MAT::METAL_BLACK));
  out.push_back(B(w - 1, w - 1, 1, 3, 0, 2, MAT::METAL_BLACK));
  return out;
}

// a counter with a glass screen (police front desk, bank, pharmacy, ticket office)
Boxes service_counter(Rng& rng, const PrefabOpts& opt) {
  const double w = num(opt.w);
  const uint16_t top = opt.top ? *opt.top : pick(rng, {MAT::WOOD_DARK, MAT::LAMINATE_GRAY, MAT::WOOD_MED});
  Boxes out = {B(0, w - 1, 3, 5, 0, 7, top), B(0, w - 1, 3, 5, 8, 8, MAT::COUNTERTOP)};
  if (not_false(opt.screen)) out.push_back(B(0, w - 1, 4, 4, 9, 15, MAT::GLASS));
  for (double a = 2; a + 3 < w; a += 8) {
    out.push_back(B(a, a + 2, 1, 1, 5, 7, MAT::SCREEN));
    out.push_back(B(a, a + 2, 0, 1, 3, 3, MAT::FABRIC_GRAY));
  }
  return out;
}

// ---------------------------------------------------------------- police / fire
Boxes bunk(Rng& rng, const PrefabOpts& opt) {
  const uint16_t frame = truthy(opt.steel) ? static_cast<uint16_t>(MAT::METAL_PANEL) : static_cast<uint16_t>(MAT::METAL_BLACK);
  Boxes out = {B(0, 6, 0, 15, 2, 2, frame), B(0, 6, 0, 15, 3, 3, MAT::MATTRESS),
               B(0, 6, 5, 15, 4, 4, truthy(opt.steel) ? static_cast<uint16_t>(MAT::FABRIC_GRAY) : pick(rng, {MAT::BEDSHEET_BLUE, MAT::FABRIC_GRAY}))};
  if (truthy(opt.double_)) {
    out.push_back(B(0, 6, 0, 15, 10, 10, frame));
    out.push_back(B(0, 6, 0, 15, 11, 11, MAT::MATTRESS));
    out.push_back(B(0, 0, 0, 0, 0, 12, frame));
    out.push_back(B(6, 6, 15, 15, 0, 12, frame));
    out.push_back(B(0, 0, 15, 15, 0, 12, frame));
    out.push_back(B(6, 6, 0, 0, 0, 12, frame));
  } else {
    out.push_back(B(0, 0, 0, 0, 0, 1, frame));
    out.push_back(B(6, 6, 15, 15, 0, 1, frame));
  }
  return out;
}

Boxes steel_toilet(Rng&, const PrefabOpts&) {
  return {B(0, 3, 0, 1, 0, 6, MAT::METAL_CHROME), B(1, 2, 2, 4, 0, 3, MAT::METAL_CHROME), B(1, 2, 3, 3, 3, 3, 0)};
}

// a row of bars (the front of a holding cell)
Boxes cell_bars(Rng&, const PrefabOpts& opt) {
  const double w = num(opt.w);
  Boxes out = {B(0, w - 1, 0, 0, 17, 17, MAT::METAL_BLACK), B(0, w - 1, 0, 0, 0, 0, MAT::METAL_BLACK)};
  for (double a = 0; a < w; a += 2) out.push_back(B(a, a, 0, 0, 1, 16, MAT::METAL_BLACK));
  return out;
}

Boxes fire_truck(Rng&, const PrefabOpts&) {
  Boxes out = {
      B(0, 19, 2, 61, 2, 20, MAT::FIRE_RED),     B(1, 18, 0, 12, 2, 18, MAT::FIRE_RED),      B(2, 17, 0, 0, 10, 16, MAT::CAR_GLASS),
      B(2, 17, 0, 0, 5, 6, MAT::HEADLIGHT),      B(0, 19, 14, 58, 21, 22, MAT::METAL_CHROME), B(3, 16, 16, 56, 23, 24, MAT::METAL_PANEL),
      B(4, 15, 12, 12, 21, 22, MAT::SIGNAL_RED), B(0, 0, 20, 58, 6, 14, MAT::METAL_PANEL),    B(19, 19, 20, 58, 6, 14, MAT::METAL_PANEL),
  };
  for (const double b : {6.0, 44.0, 52.0}) {
    out.push_back(B(-1, 2, b, b + 5, 0, 5, MAT::TIRE));
    out.push_back(B(17, 20, b, b + 5, 0, 5, MAT::TIRE));
  }
  return out;
}

Boxes hose_rack(Rng&, const PrefabOpts&) {
  Boxes out = {B(0, 9, 0, 2, 0, 16, MAT::METAL_PANEL_DARK)};
  for (double z = 2; z <= 12; z += 5) out.push_back(B(1, 8, 1, 2, z, z + 3, MAT::FIRE_RED));
  out.push_back(B(1, 8, 0, 0, 14, 15, MAT::HAZARD_YELLOW));
  return out;
}

// ---------------------------------------------------------------- museum / gallery
Boxes painting(Rng& rng, const PrefabOpts& opt) {
  const double w = num(opt.w);
  const double h = js::max(5, js::min(12, js::round(w * rng.float_(0.5, 1.1))));
  const double z0 = 12 - std::floor(h / 2);
  const uint16_t frame = pick(rng, {MAT::GOLD, MAT::WOOD_DARK, MAT::METAL_BLACK, MAT::PLASTIC_WHITE});
  Boxes out = {B(0, w - 1, 0, 0, z0, z0 + h - 1, frame)};
  // the canvas: a field of colour with a band or a blob in a second colour
  const uint16_t c1 = rng.pick(kArt);
  const uint16_t c2 = rng.pick(kArt);
  out.push_back(B(1, w - 2, 0, 0, z0 + 1, z0 + h - 2, c1));
  if (w > 4 && h > 4) {
    const double a = rng.int_(1, w - 3);
    const double z = rng.int_(z0 + 1, z0 + h - 3);
    const double a1 = js::min(w - 2, a + rng.int_(1, 3));
    const double z1 = js::min(z0 + h - 2, z + rng.int_(1, 3));
    out.push_back(B(a, a1, 0, 0, z, z1, c2));
  }
  out.push_back(B(std::floor(w / 2), std::floor(w / 2), 0, 0, z0 - 2, z0 - 2, MAT::SIGN_WHITE));
  return out;
}

Boxes display_case(Rng& rng, const PrefabOpts&) {
  const uint16_t obj = pick(rng, {MAT::GOLD, MAT::CLAY, MAT::BONE, MAT::GRANITE, MAT::BOOKS, MAT::STEEL_RUST, MAT::DOME_GREEN});
  return {B(0, 6, 0, 4, 0, 5, MAT::WOOD_DARK), B(0, 6, 0, 4, 6, 9, MAT::GLASS), B(0, 6, 0, 4, 10, 10, MAT::METAL_BLACK), B(2, 4, 1, 3, 6, 6 + rng.int_(1, 2), obj)};
}

Boxes vitrine(Rng& rng, const PrefabOpts&) {
  Boxes out = {B(0, 11, 0, 3, 0, 4, MAT::WOOD_DARK), B(0, 11, 0, 3, 5, 15, MAT::GLASS), B(0, 11, 0, 3, 16, 16, MAT::WOOD_DARK), B(0, 11, 0, 0, 5, 15, MAT::PAINT_DARK)};
  for (double a = 1; a < 11; a += 3) {
    const double z1 = 5 + rng.int_(1, 5);
    const uint16_t m = pick(rng, {MAT::GOLD, MAT::CLAY, MAT::BONE, MAT::STEEL_RUST, MAT::GRANITE});
    out.push_back(B(a, a + 1, 1, 2, 5, z1, m));
  }
  out.push_back(B(1, 10, 1, 2, 10, 10, MAT::GLASS));
  return out;
}

Boxes sculpture(Rng& rng, const PrefabOpts&) {
  const uint16_t m = pick(rng, {MAT::LIMESTONE, MAT::GRANITE, MAT::METAL_PANEL_DARK, MAT::STEEL_RUST, MAT::GOLD, MAT::PLASTER_WHITE});
  Boxes out = {B(0, 3, 0, 3, 0, 5, MAT::PLASTER_WHITE)};
  const double kind = rng.int_(0, 2);
  if (kind == 0) {
    for (double z = 6; z < 16; z += 1) out.push_back(B(1 + (std::fmod(z, 3) == 0 ? 1 : 0), 2, 1, 2 + (std::fmod(z, 4) == 0 ? 1 : 0), z, z, m));
  } else if (kind == 1) {
    out.push_back(B(0, 3, 1, 2, 6, 7, m));
    out.push_back(B(1, 2, 0, 3, 8, 11, m));
    out.push_back(B(1, 2, 1, 2, 12, 14, m));
  } else {
    out.push_back(B(1, 2, 1, 2, 6, 10, m));
    out.push_back(B(0, 3, 0, 3, 11, 12, m));
    out.push_back(B(1, 1, 1, 1, 13, 17, m));
  }
  return out;
}

// a dinosaur skeleton on a low platform (local b runs head to tail)
Boxes skeleton(Rng&, const PrefabOpts&) {
  Boxes out = {B(0, 9, 0, 27, 0, 1, MAT::GRANITE_LIGHT)};
  const uint16_t bone = MAT::BONE;
  // legs
  const double legs[4][2] = {{2, 9}, {7, 9}, {2, 18}, {7, 18}};
  for (const auto& l : legs) {
    const double a = l[0], b = l[1];
    out.push_back(B(a, a, b, b, 2, 11, bone));
    out.push_back(B(a - 1, a + 1, b - 1, b + 1, 2, 2, bone));
  }
  // spine from the skull down the tail
  for (double b = 2; b < 27; b += 1) {
    const double h = b < 6 ? 16 : b < 20 ? 14 - js::max(0, std::fabs(b - 13) - 5) * 0 : 14 - (b - 20);
    out.push_back(B(4, 5, b, b, h, h, bone));
    if (b >= 7 && b <= 19 && std::fmod(b, 2) == 0) {
      out.push_back(B(2, 2, b, b, h - 5, h - 1, bone));
      out.push_back(B(7, 7, b, b, h - 5, h - 1, bone));
      out.push_back(B(3, 6, b, b, h - 6, h - 6, bone));
    }
  }
  // skull and neck
  out.push_back(B(3, 6, 0, 4, 15, 18, bone));
  out.push_back(B(4, 5, -2, 0, 15, 16, bone));
  out.push_back(B(4, 5, 5, 7, 14, 16, bone));
  return out;
}

// ---------------------------------------------------------------- venues
Boxes stage(Rng&, const PrefabOpts& opt) {
  const double w = num(opt.w);
  const double d = num(opt.d);
  Boxes out = {B(0, w - 1, 0, d - 1, 0, 3, MAT::STAGE_BLACK), B(0, w - 1, d - 1, d - 1, 0, 3, MAT::WOOD_DARK)};
  // backdrop curtain, speaker stacks, a truss with lights
  out.push_back(B(0, w - 1, 0, 0, 4, 20, opt.curtain ? *opt.curtain : static_cast<uint16_t>(MAT::VELVET_RED)));
  for (const double a : {1.0, w - 5}) {
    out.push_back(B(a, a + 3, d - 4, d - 1, 4, 13, MAT::PLASTIC_BLACK));
    out.push_back(B(a + 1, a + 2, d - 1, d - 1, 6, 11, MAT::METAL_PANEL_DARK));
  }
  out.push_back(B(0, w - 1, d - 2, d - 2, 21, 21, MAT::METAL_BLACK));
  for (double a = 2; a < w - 2; a += 5) out.push_back(B(a, a, d - 2, d - 2, 20, 20, MAT::STAGE_LIGHT));
  if (truthy(opt.band)) {
    // drum riser, a keyboard and mic stands
    const double c = std::floor(w / 2);
    out.push_back(B(c - 4, c + 3, 2, 7, 4, 5, MAT::STAGE_BLACK));
    out.push_back(B(c - 2, c + 1, 3, 6, 6, 8, MAT::METAL_CHROME));
    out.push_back(B(c - 3, c - 3, 4, 4, 6, 10, MAT::METAL_CHROME));
    out.push_back(B(c + 5, c + 9, 5, 6, 8, 8, MAT::PLASTIC_BLACK));
    out.push_back(B(c + 6, c + 6, 5, 5, 4, 7, MAT::METAL_BLACK));
    for (const double a : {c - 7, c, c + 7})
      if (a > 1 && a < w - 2) out.push_back(B(a, a, d - 5, d - 5, 4, 14, MAT::METAL_BLACK));
  }
  if (truthy(opt.piano)) {
    out.push_back(B(std::floor(w / 2) - 5, std::floor(w / 2) + 5, 4, 12, 7, 8, MAT::PLASTIC_BLACK));
    out.push_back(B(std::floor(w / 2) - 5, std::floor(w / 2) + 5, 4, 5, 4, 6, MAT::PLASTIC_BLACK));
    out.push_back(B(std::floor(w / 2) - 4, std::floor(w / 2) - 4, 8, 11, 4, 6, MAT::PLASTIC_BLACK));
  }
  return out;
}

Boxes seat_row(Rng&, const PrefabOpts& opt) {
  const double w = num(opt.w);
  const uint16_t c = opt.seat ? *opt.seat : static_cast<uint16_t>(MAT::VELVET_RED);
  Boxes out;
  for (double a = 0; a + 2 < w; a += 3) {
    out.push_back(B(a, a + 2, 1, 3, 3, 3, c));
    out.push_back(B(a, a + 2, 4, 4, 4, 7, c));
    out.push_back(B(a, a, 1, 4, 0, 4, MAT::METAL_BLACK));
  }
  return out;
}

Boxes projection_screen(Rng&, const PrefabOpts& opt) {
  const double w = num(opt.w);
  return {B(0, w - 1, 0, 0, 5, js::max(12, opt.h.value_or(20)), MAT::PROJECTION), B(0, w - 1, 0, 0, 4, 4, MAT::STAGE_BLACK)};
}

Boxes mixing_desk(Rng&, const PrefabOpts&) {
  return {B(0, 9, 0, 5, 0, 5, MAT::STAGE_BLACK), B(0, 9, 1, 4, 6, 6, MAT::METAL_PANEL_DARK), B(1, 8, 2, 3, 7, 7, MAT::SIGNAL_GREEN),
          B(0, 9, 5, 5, 6, 8, MAT::PLASTIC_BLACK)};
}

Boxes grand_piano(Rng&, const PrefabOpts&) {
  return {B(0, 11, 2, 13, 5, 7, MAT::PLASTIC_BLACK), B(0, 11, 0, 3, 5, 6, MAT::PLASTIC_BLACK), B(1, 10, 0, 1, 7, 7, MAT::PLASTIC_WHITE),
          B(1, 1, 4, 4, 0, 4, MAT::PLASTIC_BLACK),   B(10, 10, 4, 4, 0, 4, MAT::PLASTIC_BLACK), B(5, 5, 12, 12, 0, 4, MAT::PLASTIC_BLACK),
          B(4, 7, -4, -2, 3, 3, MAT::LEATHER)};
}

Boxes pool_table(Rng&, const PrefabOpts&) {
  return {B(0, 8, 0, 15, 5, 6, MAT::WOOD_DARK), B(1, 7, 1, 14, 6, 6, MAT::FABRIC_GREEN), B(1, 1, 1, 1, 0, 4, MAT::WOOD_DARK),
          B(7, 7, 14, 14, 0, 4, MAT::WOOD_DARK), B(1, 1, 14, 14, 0, 4, MAT::WOOD_DARK),  B(7, 7, 1, 1, 0, 4, MAT::WOOD_DARK),
          B(3, 5, 7, 8, 18, 18, MAT::LAMP_SHADE)};
}

Boxes bottle_shelf(Rng&, const PrefabOpts&) {
  Boxes out = {B(0, 11, 0, 1, 0, 5, MAT::WOOD_DARK), B(0, 11, 0, 0, 6, 16, MAT::MIRROR)};
  for (double z = 7; z <= 15; z += 4) {
    out.push_back(B(0, 11, 1, 1, z, z, MAT::WOOD_DARK));
    out.push_back(B(0, 11, 1, 1, z + 1, z + 2, MAT::BOTTLES));
  }
  return out;
}

// ---------------------------------------------------------------- shops
Boxes fridge_case(Rng& rng, const PrefabOpts&) {
  Boxes out = {B(0, 9, 0, 4, 0, 15, MAT::APPLIANCE_STEEL), B(0, 9, 4, 4, 2, 14, MAT::GLASS_TINT)};
  for (double z = 3; z <= 12; z += 3) out.push_back(B(1, 8, 1, 3, z, z + 1, pick(rng, {MAT::GOODS, MAT::BOTTLES, MAT::PLASTIC_WHITE, MAT::PRODUCE_RED})));
  out.push_back(B(0, 9, 3, 3, 15, 15, MAT::LIGHT_STRIP));
  return out;
}

Boxes freezer_chest(Rng&, const PrefabOpts&) {
  return {B(0, 11, 0, 4, 0, 5, MAT::PLASTIC_WHITE), B(1, 10, 1, 3, 6, 6, MAT::GLASS_TINT), B(1, 10, 1, 3, 4, 5, MAT::CARDBOARD)};
}

Boxes produce_stand(Rng& rng, const PrefabOpts&) {
  Boxes out = {B(0, 11, 0, 6, 0, 3, MAT::WOOD_LIGHT)};
  for (double a = 0; a < 12; a += 4) {
    const uint16_t m = pick(rng, {MAT::PRODUCE_GREEN, MAT::PRODUCE_RED, MAT::PRODUCE_YELLOW, MAT::BREAD, MAT::CLAY});
    out.push_back(B(a, a + 3, 0, 2, 4, 5, m));
    out.push_back(B(a, a + 3, 3, 6, 4, 6, pick(rng, {MAT::PRODUCE_GREEN, MAT::PRODUCE_RED, MAT::PRODUCE_YELLOW})));
  }
  return out;
}

Boxes trolleys(Rng&, const PrefabOpts&) {
  Boxes out;
  for (double b = 0; b < 12; b += 3) {
    out.push_back(B(0, 5, b, b + 5, 2, 6, MAT::METAL_CHROME));
    out.push_back(B(1, 4, b + 1, b + 4, 3, 5, 0));
  }
  return out;
}

// a glass counter with goods inside (bakery, butcher, fishmonger, deli)
Boxes shop_counter(Rng&, const PrefabOpts& opt) {
  const double w = num(opt.w);
  const uint16_t goods = opt.goods ? *opt.goods : static_cast<uint16_t>(MAT::BREAD);
  Boxes out = {B(0, w - 1, 2, 5, 0, 4, MAT::PLASTIC_WHITE), B(0, w - 1, 2, 5, 5, 8, MAT::GLASS), B(1, w - 2, 3, 4, 5, 5, goods), B(0, w - 1, 2, 3, 9, 9, MAT::COUNTERTOP)};
  out.push_back(B(w - 5, w - 3, 3, 4, 10, 10, MAT::PLASTIC_BLACK));
  return out;
}

Boxes goods_shelf(Rng& rng, const PrefabOpts& opt) {
  const uint16_t frame = opt.frame ? *opt.frame : pick(rng, {MAT::WOOD_LIGHT, MAT::WOOD_DARK, MAT::SHELF_METAL});
  const std::vector<uint16_t> goods = opt.goods_list ? *opt.goods_list : std::vector<uint16_t>{MAT::GOODS};
  Boxes out = {B(0, 0, 0, 2, 0, 15, frame), B(9, 9, 0, 2, 0, 15, frame), B(0, 9, 0, 0, 0, 15, frame)};
  for (double z = 0; z <= 12; z += 4) {
    out.push_back(B(0, 9, 0, 2, z, z, frame));
    out.push_back(B(1, 8, 1, 2, z + 1, z + 2, pick(rng, goods)));
  }
  return out;
}

Boxes clothes_rack(Rng& rng, const PrefabOpts&) {
  Boxes out = {B(0, 0, 1, 2, 0, 12, MAT::METAL_CHROME), B(9, 9, 1, 2, 0, 12, MAT::METAL_CHROME), B(0, 9, 1, 2, 12, 12, MAT::METAL_CHROME)};
  for (double a = 1; a < 9; a += 1)
    out.push_back(B(a, a, 0, 3, 5, 11, pick(rng, {MAT::CLOTHES, MAT::FABRIC_RED, MAT::FABRIC_BLUE, MAT::FABRIC_BEIGE, MAT::FABRIC_GRAY})));
  return out;
}

Boxes flower_buckets(Rng& rng, const PrefabOpts&) {
  Boxes out = {B(0, 11, 0, 2, 0, 2, MAT::WOOD_WEATHERED)};
  for (double a = 0; a < 12; a += 3) {
    out.push_back(B(a, a + 2, 0, 2, 3, 4, MAT::METAL_CHROME));
    out.push_back(
        B(a, a + 2, 0, 2, 5, 7, pick(rng, {MAT::FLOWER_RED, MAT::FLOWER_YELLOW, MAT::FLOWER_WHITE, MAT::FLOWER_PURPLE, MAT::FLOWER_PINK, MAT::PLANT})));
  }
  return out;
}

Boxes barber_chair(Rng&, const PrefabOpts&) {
  return {B(0, 6, 0, 0, 5, 14, MAT::MIRROR), B(0, 6, 0, 1, 4, 4, MAT::COUNTERTOP), B(2, 4, 4, 7, 3, 4, MAT::LEATHER), B(2, 4, 7, 7, 5, 9, MAT::LEATHER),
          B(3, 3, 5, 6, 0, 2, MAT::METAL_CHROME)};
}

Boxes lumber_stack(Rng& rng, const PrefabOpts&) {
  Boxes out;
  const double n = rng.int_(3, 7);
  for (double k = 0; k < n; k += 1) {
    out.push_back(B(0, 19, 0, 7, k * 2, k * 2 + 1, std::fmod(k, 2) != 0 ? MAT::WOOD_LIGHT : MAT::WOOD_MED));
    out.push_back(B(2, 3, 0, 7, k * 2 + 1, k * 2 + 1, MAT::WOOD_DARK));
  }
  return out;
}

Boxes conveyor(Rng& rng, const PrefabOpts& opt) {
  const double w = num(opt.w);
  Boxes out = {B(0, w - 1, 0, 3, 4, 4, MAT::PLASTIC_BLACK), B(0, w - 1, 0, 0, 5, 5, MAT::HAZARD_YELLOW), B(0, w - 1, 3, 3, 5, 5, MAT::HAZARD_YELLOW)};
  for (double a = 0; a < w; a += 8) {
    out.push_back(B(a, a, 0, 0, 0, 3, MAT::METAL_PANEL));
    out.push_back(B(a, a, 3, 3, 0, 3, MAT::METAL_PANEL));
  }
  for (double a = 3; a + 3 < w; a += rng.int_(6, 12)) out.push_back(B(a, a + 3, 1, 2, 5, 7, MAT::CARDBOARD));
  return out;
}

// self-storage: a block of lock-up units, doors on both long sides
Boxes storage_units(Rng& rng, const PrefabOpts& opt) {
  const double w = num(opt.w);
  Boxes out = {B(0, w - 1, 0, 11, 0, 17, MAT::CORRUGATED), B(0, w - 1, 0, 11, 18, 18, MAT::METAL_PANEL)};
  for (double a = 1; a + 6 < w; a += 8) {
    const uint16_t door = pick(rng, {MAT::ROLLUP_DOOR, MAT::CORRUGATED_BLUE, MAT::CORRUGATED_RUST, MAT::DOOR_METAL});
    out.push_back(B(a, a + 5, 0, 0, 0, 13, door));
    out.push_back(B(a, a + 5, 11, 11, 0, 13, door));
  }
  return out;
}

Boxes whiteboard(Rng&, const PrefabOpts&) { return {B(0, 15, 0, 0, 7, 14, MAT::PANEL_WHITE), B(0, 15, 0, 0, 6, 6, MAT::METAL_CHROME)}; }

Boxes dance_barre(Rng&, const PrefabOpts& opt) {
  const double w = num(opt.w);
  return {B(0, w - 1, 0, 0, 2, 16, MAT::MIRROR), B(0, w - 1, 1, 1, 7, 7, MAT::HANDRAIL_WOOD)};
}

Boxes atm(Rng&, const PrefabOpts&) {
  return {B(0, 4, 0, 2, 0, 12, MAT::METAL_PANEL), B(1, 3, 2, 2, 7, 9, MAT::SCREEN), B(1, 3, 2, 2, 5, 5, MAT::PLASTIC_BLACK)};
}

// an icon screen (iconostasis panel) of an orthodox church: gilded frames round dark panels
Boxes icon(Rng& rng, const PrefabOpts& opt) {
  const double w = num(opt.w);
  Boxes out = {B(0, w - 1, 0, 1, 0, 22, MAT::WOOD_DARK)};
  for (double a = 1; a + 3 < w; a += 5)
    for (const double z : {3.0, 12.0}) {
      out.push_back(B(a, a + 3, 1, 1, z, z + 7, MAT::GOLD));
      out.push_back(B(a + 1, a + 2, 1, 1, z + 1, z + 6, pick(rng, {MAT::ART_CRIMSON, MAT::ART_BLUE, MAT::ART_OCHRE})));
    }
  out.push_back(B(std::floor(w / 2) - 2, std::floor(w / 2) + 2, 1, 1, 0, 11, 0));
  return out;
}

}  // namespace

const std::vector<Prefab>& civic_prefabs() {
  static const std::vector<Prefab> table = {
      // hospital
      {.id = "hospitalBed", .w = 8, .d = 17, .d_extra = 0, .build = hospital_bed},
      {.id = "bedCurtain", .w = 1, .d = 17, .tall = true, .build = bed_curtain},
      {.id = "medCart", .w = 5, .d = 4, .build = med_cart},
      {.id = "examCouch", .w = 6, .d = 15, .build = exam_couch},
      {.id = "opTable", .w = 6, .d = 16, .free = true, .pad = 6, .build = op_table},
      {.id = "xray", .w = 12, .d = 14, .free = true, .pad = 4, .build = xray},
      {.id = "waitingChairs", .w = 0, .d = 4, .build = waiting_chairs},
      {.id = "serviceCounter", .w = 0, .d = 6, .build = service_counter},
      // police / fire
      {.id = "bunk", .w = 7, .d = 16, .build = bunk},
      {.id = "steelToilet", .w = 4, .d = 5, .build = steel_toilet},
      {.id = "cellBars", .w = 0, .d = 1, .build = cell_bars},
      {.id = "fireTruck", .w = 20, .d = 64, .free = true, .pad = 2, .build = fire_truck},
      {.id = "hoseRack", .w = 10, .d = 3, .tall = true, .build = hose_rack},
      // museum / gallery
      {.id = "painting", .w = 0, .d = 1, .tall = true, .build = painting},
      {.id = "displayCase", .w = 7, .d = 5, .free = true, .pad = 3, .build = display_case},
      {.id = "vitrine", .w = 12, .d = 4, .tall = true, .build = vitrine},
      {.id = "sculpture", .w = 4, .d = 4, .free = true, .pad = 4, .build = sculpture},
      {.id = "skeleton", .w = 10, .d = 28, .free = true, .pad = 4, .build = skeleton},
      // venues
      {.id = "stage", .w = 0, .d = 0, .build = stage},
      {.id = "seatRow", .w = 0, .d = 5, .free = true, .pad = 0, .build = seat_row},
      {.id = "projectionScreen", .w = 0, .d = 1, .build = projection_screen},
      {.id = "mixingDesk", .w = 10, .d = 6, .free = true, .pad = 3, .build = mixing_desk},
      {.id = "grandPiano", .w = 12, .d = 14, .free = true, .pad = 3, .build = grand_piano},
      {.id = "poolTable", .w = 9, .d = 16, .free = true, .pad = 5, .build = pool_table},
      {.id = "bottleShelf", .w = 12, .d = 2, .tall = true, .build = bottle_shelf},
      // shops
      {.id = "fridgeCase", .w = 10, .d = 5, .tall = true, .build = fridge_case},
      {.id = "freezerChest", .w = 12, .d = 5, .free = true, .pad = 4, .build = freezer_chest},
      {.id = "produceStand", .w = 12, .d = 7, .free = true, .pad = 4, .build = produce_stand},
      {.id = "trolleys", .w = 6, .d = 12, .build = trolleys},
      {.id = "shopCounter", .w = 0, .d = 6, .build = shop_counter},
      {.id = "goodsShelf", .w = 10, .d = 3, .tall = true, .build = goods_shelf},
      {.id = "clothesRack", .w = 10, .d = 4, .free = true, .pad = 3, .build = clothes_rack},
      {.id = "flowerBuckets", .w = 12, .d = 3, .build = flower_buckets},
      {.id = "barberChair", .w = 7, .d = 9, .build = barber_chair},
      {.id = "lumberStack", .w = 20, .d = 8, .free = true, .pad = 3, .build = lumber_stack},
      {.id = "conveyor", .w = 0, .d = 4, .free = true, .pad = 2, .build = conveyor},
      {.id = "storageUnits", .w = 0, .d = 12, .free = true, .pad = 0, .build = storage_units},
      {.id = "whiteboard", .w = 16, .d = 1, .build = whiteboard},
      {.id = "danceBarre", .w = 0, .d = 2, .tall = true, .build = dance_barre},
      {.id = "atm", .w = 5, .d = 3, .build = atm},
      {.id = "icon", .w = 0, .d = 2, .tall = true, .build = icon},
  };
  return table;
}

const Prefab* civic_prefab(std::string_view key) {
  for (const Prefab& p : civic_prefabs())
    if (key == p.id) return &p;
  return nullptr;
}

}  // namespace svx::city
