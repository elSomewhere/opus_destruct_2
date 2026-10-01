// svx_city — furniture and fixture prefabs at 12.5 cm resolution (voxel_city
// buildings/interior/prefabs.js).
//
// Local frame: a runs along the wall (0..w-1), b runs away from the wall into the room (0..d-1),
// z is the height above the finished floor (0 = the first air voxel). A prefab's build(rng, opt)
// returns boxes {a0, a1, b0, b1, z0, z1, m} (inclusive); a box with m = 0 carves. Heights: seat
// about z3 (0.45 m), table top z5 (0.75 m), counter top z6 (0.9 m).
//
// The tables (PREFABS here, CIVIC_PREFABS in civicPrefabs.hpp, RULE_PREFABS in civicRules.hpp)
// are constant: safe to read from any thread. furnish.js merges the three
// (Object.assign(PREFABS, CIVIC_PREFABS, RULE_PREFABS)): civicRules.hpp's furnish_prefab(key).
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "core/hash.hpp"

namespace svx::city {

// A prefab box (local a, b, z; inclusive; m a material of voxel/materials.hpp, 0 carves). The
// street props (city/propPrefabs.hpp) and the industrial props (city/industry.hpp) are made of
// them too.
struct PrefabBox {
  double a0 = 0, a1 = 0, b0 = 0, b1 = 0, z0 = 0, z1 = 0;
  uint16_t m = 0;
};

// A prefab's options (`opt`): furnish.js passes w and d (and a rule's `prefab` options over or
// under them). Absent: nullopt; how each prefab reads them is in brackets.
struct PrefabOpts {
  std::optional<double> w = std::nullopt, d = std::nullopt;  // [variable-size prefabs need them]
  std::optional<bool> monitor = std::nullopt;                 // desk [!== false]
  std::optional<bool> hood = std::nullopt, upper = std::nullopt;  // counterRun [!== false]
  std::optional<bool> screen = std::nullopt;                  // serviceCounter [!== false]
  std::optional<bool> metal = std::nullopt;                   // shelfUnit [truthy]
  std::optional<bool> steel = std::nullopt, double_ = std::nullopt;  // bunk [truthy] (double)
  std::optional<bool> band = std::nullopt, piano = std::nullopt;     // stage [truthy]
  std::optional<uint16_t> goods = std::nullopt;  // shelfUnit, shopCounter, marketCounter: a material [??]
  std::optional<std::vector<uint16_t>> goods_list = std::nullopt;  // goodsShelf: opt.goods, an array of materials [?? [GOODS]]
  std::optional<double> h = std::nullopt;         // rack [?? 40], projectionScreen [?? 20]
  std::optional<uint16_t> top = std::nullopt;     // serviceCounter [??]
  std::optional<uint16_t> curtain = std::nullopt; // stage [??]
  std::optional<uint16_t> seat = std::nullopt;    // seatRow [??]
  std::optional<uint16_t> frame = std::nullopt;   // goodsShelf [??]
};

using PrefabBuild = std::vector<PrefabBox> (*)(Rng& rng, const PrefabOpts& opt);

// A prefab: its footprint w x d (0: given by opt.w / opt.d), whether it is tall (kept off windows
// and walls with doors), free-standing (free) or flat (a rug: walked over), its pad (the clearance
// round a free-standing one: furnish reads pad ?? 2), dExtra (room in front of it: ?? 0), and its
// boxes.
struct Prefab {
  const char* id = nullptr;
  double w = 0, d = 0;
  bool tall = false, free = false, flat = false;
  std::optional<double> pad = std::nullopt, d_extra = std::nullopt;
  PrefabBuild build = nullptr;
};

// PREFABS, in the reference's key order.
const std::vector<Prefab>& prefabs();
// PREFABS[key], or null.
const Prefab* prefab(std::string_view key);

// A parked car (local a = the width across, b = the length).
std::vector<PrefabBox> car_boxes(Rng& rng);

}  // namespace svx::city
