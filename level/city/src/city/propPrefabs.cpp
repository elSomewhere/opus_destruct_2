// svx_city — voxel_city city/propPrefabs.js.
#include "city/propPrefabs.hpp"

#include <initializer_list>

#include "buildings/interior/prefabs.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

namespace {

using Boxes = std::vector<PrefabBox>;

inline PrefabBox B(double a0, double a1, double b0, double b1, double z0, double z1, uint16_t m) { return {a0, a1, b0, b1, z0, z1, m}; }
inline uint16_t pick(Rng& rng, std::initializer_list<uint16_t> items) {
  return items.begin()[static_cast<size_t>(std::floor(rng.next() * static_cast<double>(items.size())))];
}

Boxes streetlight(Rng&, const PropOpts& o) {
  const double h = o.h.value_or(52);
  const double reach = o.reach.value_or(12);
  const uint16_t pole = o.pole ? *o.pole : static_cast<uint16_t>(MAT::POLE_METAL);
  return {
      B(-1, 1, -1, 1, 0, 1, MAT::CONCRETE_DARK),
      B(0, 0, 0, 0, 2, h, pole),
      B(0, 0, 1, reach, h, h, pole),
      B(-1, 1, reach - 3, reach, h - 1, h - 1, MAT::LAMP_LIGHT),
      B(-1, 1, reach - 3, reach, h + 1, h + 1, pole),
  };
}

Boxes park_lamp(Rng&, const PropOpts&) {
  return {B(0, 0, 0, 0, 0, 26, MAT::POLE_GREEN), B(-1, 1, -1, 1, 27, 29, MAT::LAMP_LIGHT), B(-1, 1, -1, 1, 30, 30, MAT::POLE_GREEN)};
}

Boxes signal(Rng& rng, const PropOpts& o) {
  const double reach = o.reach.value_or(28);
  Boxes out = {B(-1, 1, -1, 1, 0, 1, MAT::CONCRETE_DARK), B(0, 0, 0, 0, 2, 44, MAT::SIGNAL_BOX), B(0, 0, 1, reach, 44, 44, MAT::SIGNAL_BOX)};
  for (const double b : {js::round(reach * 0.55), reach}) {
    out.push_back(B(-1, 1, b - 1, b, 36, 43, MAT::SIGNAL_BOX));
    out.push_back(B(-1, 1, b - 1, b - 1, 41, 42, rng.chance(0.5) ? MAT::SIGNAL_RED : MAT::SIGNAL_BOX));
    out.push_back(B(-1, 1, b - 1, b - 1, 37, 38, rng.chance(0.5) ? MAT::SIGNAL_GREEN : MAT::SIGNAL_BOX));
  }
  // pedestrian signal + push button
  out.push_back(B(0, 0, -1, -1, 18, 21, MAT::SIGNAL_BOX));
  out.push_back(B(0, 0, -1, -1, 19, 19, MAT::SIGNAL_AMBER));
  return out;
}

Boxes delineator(Rng&, const PropOpts&) {
  // white roadside post with a reflector facing the traffic
  return {B(0, 0, 0, 0, 0, 7, MAT::PLASTIC_WHITE), B(0, 0, 0, 0, 5, 6, MAT::SIGNAL_AMBER), B(0, 0, 0, 0, 7, 7, MAT::METAL_BLACK)};
}

Boxes hydrant(Rng&, const PropOpts&) { return {B(0, 1, 0, 1, 0, 4, MAT::HYDRANT), B(-1, 2, 0, 1, 3, 3, MAT::HYDRANT), B(0, 1, 0, 1, 5, 5, MAT::METAL_BLACK)}; }

Boxes bench(Rng&, const PropOpts&) {
  return {B(0, 11, 0, 3, 3, 3, MAT::WOOD_MED), B(0, 11, 0, 0, 4, 6, MAT::WOOD_MED), B(1, 1, 0, 3, 0, 2, MAT::METAL_BLACK), B(10, 10, 0, 3, 0, 2, MAT::METAL_BLACK)};
}

Boxes bin(Rng&, const PropOpts&) { return {B(0, 2, 0, 2, 0, 6, MAT::BIN_GREEN), B(0, 2, 0, 2, 7, 7, MAT::METAL_BLACK)}; }

// a headstone, now and then a wooden grave cross (churchyards)
Boxes gravestone(Rng& rng, const PropOpts&) {
  if (rng.chance(0.3)) return {B(1, 1, 0, 0, 0, 7, MAT::WOOD_DARK), B(0, 2, 0, 0, 5, 5, MAT::WOOD_DARK)};
  const uint16_t m = pick(rng, {MAT::GRANITE, MAT::ROCK_DARK, MAT::STONE});
  const double h = rng.int_(3, 6);
  return {B(0, 3, 0, 0, 0, h, m), B(0, 3, -1, 1, 0, 0, m)};
}

Boxes mailbox(Rng&, const PropOpts&) { return {B(0, 3, 0, 2, 0, 1, MAT::METAL_BLACK), B(0, 3, 0, 2, 2, 8, MAT::SIGN_BLUE)}; }

Boxes sign_post(Rng& rng, const PropOpts&) {
  return {B(0, 0, 0, 0, 0, 22, MAT::POLE_METAL), B(-3, 3, 0, 0, 20, 21, MAT::SIGN_GREEN), B(0, 0, -3, 3, 22, 23, MAT::SIGN_GREEN),
          B(-2, 2, 1, 1, 14, 17, rng.chance(0.5) ? MAT::SIGN_RED : MAT::SIGN_WHITE)};
}

Boxes bus_shelter(Rng&, const PropOpts&) {
  return {
      B(0, 23, -8, -8, 0, 20, MAT::GLASS),         B(0, 0, -8, -1, 0, 20, MAT::GLASS),    B(23, 23, -8, -1, 0, 20, MAT::GLASS),
      B(-1, 24, -9, 1, 21, 21, MAT::METAL_PANEL_DARK), B(2, 21, -7, -5, 3, 3, MAT::WOOD_MED), B(26, 26, 0, 0, 0, 24, MAT::POLE_METAL),
      B(25, 27, 0, 0, 20, 24, MAT::SIGN_BLUE),
  };
}

Boxes bollard(Rng&, const PropOpts&) { return {B(0, 0, 0, 0, 0, 5, MAT::BOLLARD)}; }

Boxes planter(Rng&, const PropOpts&) {
  return {B(0, 7, 0, 3, 0, 3, MAT::CONCRETE_LIGHT), B(1, 6, 1, 2, 3, 4, MAT::BUSH), B(2, 5, 1, 2, 5, 5, MAT::FLOWER_RED)};
}

// parked car: a = along the street (length), b = across
Boxes car(Rng& rng, const PropOpts&) {
  Boxes out;
  for (const PrefabBox& q : car_boxes(rng)) out.push_back({q.b0, q.b1, q.a0, q.a1, q.z0, q.z1, q.m});
  return out;
}

Boxes fountain(Rng&, const PropOpts&) {
  Boxes out;
  for (double r = 0; r <= 20; r += 1) {
    const double w = js::round(std::sqrt(js::max(0, 400 - r * r)));
    out.push_back(B(-w, w, r, r, 0, 2, MAT::PLAZA_STONE_DARK));
    out.push_back(B(-w, w, -r, -r, 0, 2, MAT::PLAZA_STONE_DARK));
  }
  for (double r = 0; r <= 18; r += 1) {
    const double w = js::round(std::sqrt(js::max(0, 324 - r * r)));
    out.push_back(B(-w, w, r, r, 1, 2, MAT::WATER));
    out.push_back(B(-w, w, -r, -r, 1, 2, MAT::WATER));
  }
  out.push_back(B(-2, 2, -2, 2, 0, 10, MAT::LIMESTONE));
  out.push_back(B(-4, 4, -4, 4, 10, 11, MAT::LIMESTONE));
  out.push_back(B(-1, 1, -1, 1, 12, 16, MAT::WATER));
  return out;
}

Boxes playground(Rng&, const PropOpts&) {
  Boxes out = {B(0, 47, 0, 39, -1, -1, MAT::RUBBER_MAT)};
  // swing set
  out.push_back(B(4, 4, 4, 4, 0, 18, MAT::PLAY_RED));
  out.push_back(B(20, 20, 4, 4, 0, 18, MAT::PLAY_RED));
  out.push_back(B(4, 20, 4, 4, 18, 18, MAT::PLAY_RED));
  out.push_back(B(8, 10, 3, 5, 5, 5, MAT::PLAY_BLUE));
  out.push_back(B(14, 16, 3, 5, 5, 5, MAT::PLAY_BLUE));
  // slide tower
  out.push_back(B(28, 36, 20, 28, 0, 1, MAT::PLAY_YELLOW));
  out.push_back(B(28, 36, 20, 28, 12, 12, MAT::WOOD_MED));
  const double posts[4][2] = {{28, 20}, {36, 20}, {28, 28}, {36, 28}};
  for (const auto& p : posts) out.push_back(B(p[0], p[0], p[1], p[1], 0, 20, MAT::PLAY_BLUE));
  out.push_back(B(28, 36, 20, 28, 20, 21, MAT::PLAY_RED));
  for (double k = 0; k < 12; k += 1) out.push_back(B(30, 34, 29 + k, 29 + k, 11 - k, 11 - k, MAT::PLAY_YELLOW));
  // sandbox
  out.push_back(B(4, 16, 22, 34, 0, 0, MAT::WOOD_LIGHT));
  out.push_back(B(5, 15, 23, 33, -1, 0, MAT::SAND_BOX));
  return out;
}

// an old-town lantern: a black iron post with a four-sided lamp (a runs along the street)
Boxes old_lamp(Rng&, const PropOpts&) {
  return {
      B(-1, 1, -1, 1, 0, 1, MAT::GRANITE),       B(0, 0, 0, 0, 2, 28, MAT::METAL_BLACK),   B(-1, 1, -1, 1, 22, 22, MAT::METAL_BLACK),
      B(-1, 1, -1, 1, 29, 29, MAT::METAL_BLACK), B(-1, 1, -1, 1, 30, 32, MAT::LAMP_LIGHT), B(-2, 2, -2, 2, 33, 33, MAT::METAL_BLACK),
      B(-1, 1, -1, 1, 34, 34, MAT::METAL_BLACK),
  };
}

// a statue on a granite plinth in the middle of a square
Boxes monument(Rng& rng, const PropOpts&) {
  Boxes out = {B(-10, 10, -10, 10, 0, 1, MAT::GRANITE_LIGHT), B(-7, 7, -7, 7, 2, 3, MAT::GRANITE), B(-4, 4, -4, 4, 4, 16, MAT::GRANITE_LIGHT),
               B(-5, 5, -5, 5, 17, 17, MAT::GRANITE)};
  // the figure: a dark bronze body, head and a raised arm (or an obelisk)
  if (rng.chance(0.3)) {
    for (double k = 0; k < 20; k += 1) {
      const double w = js::max(0, 3 - std::floor(k / 7));
      out.push_back(B(-w, w, -w, w, 18 + k, 18 + k, MAT::GRANITE));
    }
    return out;
  }
  const uint16_t m = MAT::METAL_PANEL_DARK;
  out.push_back(B(-2, 2, -1, 1, 18, 20, m));
  out.push_back(B(-2, 2, -2, 2, 21, 29, m));
  out.push_back(B(-1, 1, -1, 1, 30, 32, m));
  out.push_back(B(3, 3, 0, 0, 26, 34, m));
  out.push_back(B(-3, -3, 0, 0, 22, 27, m));
  return out;
}

// a market stall: a trestle table with crates under a striped canvas roof
Boxes market_stall(Rng& rng, const PropOpts&) {
  const uint16_t cloth = pick(rng, {MAT::AWNING_RED, MAT::AWNING_GREEN, MAT::AWNING_BLUE, MAT::AWNING_STRIPE});
  Boxes out;
  const double legs[4][2] = {{0, 0}, {23, 0}, {0, 15}, {23, 15}};
  for (const auto& l : legs) out.push_back(B(l[0], l[0], l[1], l[1], 0, 17, MAT::WOOD_MED));
  out.push_back(B(1, 22, 2, 9, 6, 6, MAT::WOOD_LIGHT));
  for (double a = -1; a <= 24; a += 1) out.push_back(B(a, a, -1, 16, 18, 18, (js::sar(a, 1) & 1) ? cloth : static_cast<uint16_t>(MAT::AWNING_STRIPE)));
  out.push_back(B(3, 7, 3, 7, 7, 8, pick(rng, {MAT::GOODS, MAT::CARDBOARD, MAT::FLOWER_YELLOW, MAT::FLOWER_RED})));
  out.push_back(B(10, 14, 3, 7, 7, 8, pick(rng, {MAT::GOODS, MAT::LEAVES_LIGHT, MAT::FLOWER_PURPLE})));
  out.push_back(B(16, 20, 3, 7, 7, 7, MAT::PALLET));
  return out;
}

// an allotment hut (kolonihage / dacha shed): boards, a door, a window, a pitched roof
Boxes allotment_hut(Rng& rng, const PropOpts&) {
  const uint16_t wall = pick(rng, {MAT::CLAD_FALU, MAT::CLAD_GREEN, MAT::CLAD_OCHRE, MAT::CLAD_WHITE, MAT::CLAD_GREYBLUE, MAT::WOOD_WEATHERED});
  const uint16_t roof = pick(rng, {MAT::ROOF_BLACK_METAL, MAT::CORRUGATED, MAT::CORRUGATED_RUST, MAT::ROOF_SHINGLE});
  Boxes out = {B(0, 23, 0, 19, 0, 0, MAT::WOOD_WEATHERED), B(0, 23, 0, 19, 1, 17, wall), B(9, 14, 20, 20, 1, 15, MAT::DOOR_WOOD), B(3, 6, 20, 20, 7, 11, MAT::GLASS),
               B(17, 20, 20, 20, 7, 11, MAT::GLASS)};
  // pitched roof along a, ridge in the middle of b
  for (double k = 0; k <= 11; k += 1) out.push_back(B(-1, 24, -1 + k, 20 - k, 18 + k, 18 + k, roof));
  return out;
}

// a low picket fence post pair (used in runs)
Boxes picket(Rng&, const PropOpts&) { return {B(0, 0, 0, 0, 0, 6, MAT::FENCE_WHITE)}; }

// a pile of rubble: broken concrete, bricks and a rusty drum
Boxes rubble(Rng& rng, const PropOpts&) {
  Boxes out;
  const double n = rng.int_(4, 8);
  for (double k = 0; k < n; k += 1) {
    const double a = rng.int_(-10, 8);
    const double b = rng.int_(-10, 8);
    const double h = rng.int_(1, 5);
    const double a1 = a + rng.int_(2, 6);
    const double b1 = b + rng.int_(2, 6);
    const uint16_t m = pick(rng, {MAT::CONCRETE_DARK, MAT::CONCRETE, MAT::BRICK_RED, MAT::GRAVEL, MAT::CINDERBLOCK});
    out.push_back(B(a, a1, b, b1, 0, h, m));
  }
  if (rng.chance(0.5)) out.push_back(B(10, 13, -3, 0, 0, 7, MAT::STEEL_RUST));
  return out;
}

// a woodpile under a lean-to roof against a back fence
Boxes woodpile(Rng&, const PropOpts&) {
  return {B(0, 23, 0, 6, 0, 10, MAT::WOOD_MED), B(0, 23, 0, 0, 11, 13, MAT::WOOD_DARK), B(-1, 24, -1, 8, 14, 14, MAT::CORRUGATED_RUST)};
}

// a boathouse (naust): a steep-roofed shed of tarred or red boards, its gable doors to the water (+b)
Boxes boathouse(Rng& rng, const PropOpts&) {
  const uint16_t wall = pick(rng, {MAT::CLAD_FALU, MAT::CLAD_FALU, MAT::LOG_TARRED, MAT::WOOD_WEATHERED, MAT::CLAD_WHITE});
  const uint16_t roof = pick(rng, {MAT::ROOF_BLACK_TILE, MAT::ROOF_SOD, MAT::ROOF_BLACK_METAL, MAT::ROOF_SLATE});
  Boxes out = {B(-18, 18, 0, 63, -16, 0, MAT::STONE), B(-18, 18, 0, 63, 1, 22, wall), B(-10, 10, 63, 63, 1, 18, MAT::WOOD_DARK),
               B(-17, 17, 1, 62, 1, 1, MAT::WOOD_WEATHERED)};
  for (double k = 0; k <= 20; k += 1) out.push_back(B(-20 + k, 20 - k, -1, 64, 23 + k, 23 + k, k == 20 ? static_cast<uint16_t>(MAT::WOOD_DARK) : roof));
  // gable wall triangle at both ends
  for (double k = 0; k < 19; k += 1) {
    out.push_back(B(-18 + k, 18 - k, 0, 0, 23 + k, 23 + k, wall));
    out.push_back(B(-18 + k, 18 - k, 63, 63, 23 + k, 23 + k, wall));
  }
  return out;
}

// a lighthouse: a white round tower with red bands, a gallery and a lantern
Boxes lighthouse(Rng&, const PropOpts&) {
  Boxes out;
  for (double z = 0; z < 120; z += 1) {
    const double r = 14 - std::floor(z / 22);
    const bool band = std::fmod(std::floor(z / 14), 3) == 1;
    for (double a = -r; a <= r; a += 1) {
      const double w = js::round(std::sqrt(r * r - a * a));
      out.push_back(B(a, a, -w, w, z, z, band ? MAT::PAINT_RED : MAT::PLASTER_WHITE));
    }
  }
  out.push_back(B(-12, 12, -12, 12, 120, 121, MAT::METAL_BLACK));
  const double posts[4][2] = {{-12, -12}, {12, -12}, {-12, 12}, {12, 12}};
  for (const auto& p : posts) out.push_back(B(p[0], p[0], p[1], p[1], 122, 128, MAT::RAILING));
  out.push_back(B(-6, 6, -6, 6, 122, 134, MAT::LAMP_LIGHT));
  out.push_back(B(-7, 7, -7, 7, 135, 137, MAT::PAINT_RED));
  out.push_back(B(-3, 3, -3, 3, 138, 142, MAT::PAINT_RED));
  out.push_back(B(0, 0, 0, 0, 143, 150, MAT::METAL_BLACK));
  return out;
}

// a cairn (varde) on a fell top: a stack of flat stones
Boxes cairn(Rng& rng, const PropOpts&) {
  Boxes out;
  const double h = rng.int_(10, 16);
  for (double z = 0; z < h; z += 1) {
    const double r = js::max(1, js::round(5 - (z * 4) / h));
    out.push_back(B(-r, r, -r, r, z, z, std::fmod(z, 3) == 0 ? MAT::ROCK_DARK : MAT::GRANITE));
  }
  return out;
}

// a fish-drying rack (hjell): posts and rails, stockfish hanging in rows
Boxes fish_rack(Rng& rng, const PropOpts&) {
  Boxes out;
  const double L = rng.int_(48, 96);
  for (double a = 0; a <= L; a += 16) {
    out.push_back(B(a, a, -6, -6, 0, 26, MAT::WOOD_WEATHERED));
    out.push_back(B(a, a, 6, 6, 0, 26, MAT::WOOD_WEATHERED));
    out.push_back(B(a, a, -6, 6, 26, 26, MAT::WOOD_WEATHERED));
  }
  out.push_back(B(0, L, 0, 0, 24, 24, MAT::WOOD_WEATHERED));
  for (double a = 1; a < L; a += 2)
    if (rng.chance(0.8)) out.push_back(B(a, a, -1, 1, 16, 23, MAT::WOOD_LIGHT));
  return out;
}

}  // namespace

const std::vector<PropDef>& props() {
  static const std::vector<PropDef> table = [] {
    std::vector<PropDef> t = {
        {"streetlight", streetlight},
        {"parkLamp", park_lamp},
        {"signal", signal},
        {"delineator", delineator},
        {"hydrant", hydrant},
        {"bench", bench},
        {"bin", bin},
        {"gravestone", gravestone},
        {"mailbox", mailbox},
        {"signPost", sign_post},
        {"busShelter", bus_shelter},
        {"bollard", bollard},
        {"planter", planter},
        {"car", car},
        {"fountain", fountain},
        {"playground", playground},
        {"oldLamp", old_lamp},
        {"monument", monument},
        {"marketStall", market_stall},
        {"allotmentHut", allotment_hut},
        {"picket", picket},
        {"rubble", rubble},
        {"woodpile", woodpile},
        {"boathouse", boathouse},
        {"lighthouse", lighthouse},
        {"cairn", cairn},
        {"fishRack", fish_rack},
    };
    // Object.assign(PROPS, INDUSTRY_PROPS): a key already present is replaced in place (none is)
    for (const PropDef& d : industry_props()) {
      bool replaced = false;
      for (PropDef& e : t)
        if (std::string_view(e.id) == d.id) {
          e = d;
          replaced = true;
        }
      if (!replaced) t.push_back(d);
    }
    return t;
  }();
  return table;
}

const PropDef* prop(std::string_view kind) {
  for (const PropDef& d : props())
    if (kind == d.id) return &d;
  return nullptr;
}

}  // namespace svx::city
