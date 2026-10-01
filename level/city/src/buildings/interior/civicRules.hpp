// svx_city — furnishing rules of civic rooms and shops (voxel_city buildings/interior/civicRules.js).
//
// A rule furnishes one rect of a room through the room context of furnish.js (RoomCtx: wall,
// free and opposite placements with door clearance and a walker check). That context is
// FurnishCtx here: the fields and the pure helpers the rules read, and the placements as virtual
// methods that furnish.js's port implements (its RoomCtx derives from FurnishCtx). makeCivicRules
// (RULES) is civic_rules(): the rules by room type, in the reference's key order; where a civic
// rule builds on one of furnish.js's base rules (RULES.office, RULES.bedroom, RULES.warehouse) it
// calls c.rule(type), which runs RULES[type] on the same context.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "buildings/interior/grid.hpp"
#include "buildings/interior/prefabs.hpp"
#include "core/hash.hpp"
#include "core/rect.hpp"

namespace svx::city {

// What wall() and free() placed: {key, side, a, b, w, d} (local to the side's frame).
struct Placed {
  std::string key{};
  char side = 'N';
  double a = 0, b = 0, w = 0, d = 0;
};

// RoomCtx.wall's `at`: "center" | "start" | "end" | "random" | a number (anything else draws a
// random position like "random").
struct WallAt {
  enum Kind : uint8_t { Center, Start, End, Random, Number };
  Kind kind = Center;
  double value = 0;
  static WallAt center() { return {Center, 0}; }
  static WallAt start() { return {Start, 0}; }
  static WallAt end() { return {End, 0}; }
  static WallAt random() { return {Random, 0}; }
  static WallAt at(double a) { return {Number, a}; }
};

// wall(key, opts): sides (in preference order; absent: rng.shuffle(N, E, S, W)), w, d, at
// [center], keepDepth [6], pad [0], prefab (the prefab's options).
struct WallOpts {
  std::optional<std::vector<char>> sides = std::nullopt;
  std::optional<double> w = std::nullopt, d = std::nullopt;
  std::optional<WallAt> at = std::nullopt;
  std::optional<double> keep_depth = std::nullopt;
  std::optional<double> pad = std::nullopt;
  std::optional<PrefabOpts> prefab = std::nullopt;
};

// free(key, opts): side [N], a, b (the target; absent: centred), w, d, pad [prefab's pad ?? 2],
// range [12], exact (truthy: range 0), prefab.
struct FreeOpts {
  std::optional<char> side = std::nullopt;
  std::optional<double> a = std::nullopt, b = std::nullopt;
  std::optional<double> w = std::nullopt, d = std::nullopt;
  std::optional<double> pad = std::nullopt;
  std::optional<double> range = std::nullopt;
  std::optional<bool> exact = std::nullopt;
  std::optional<PrefabOpts> prefab = std::nullopt;
};

// The room context of a furnishing rule (furnish.js's RoomCtx, the parts the rules use): one rect
// `r` of a room, W x H cells, its floor zf, its own stream (furnishFloor forks one per rect), and
// the boxes it emits.
class FurnishCtx {
 public:
  FurnishCtx(const Room& room, const Rect& rect, double zf, Rng rng, bool cars);
  virtual ~FurnishCtx() = default;
  FurnishCtx(const FurnishCtx&) = delete;
  FurnishCtx& operator=(const FurnishCtx&) = delete;

  bool cars;  // (world.config.vehicles.parked)
  const Room& room;
  Rect r;
  double zf;
  Rng rng;
  double W, H;
  std::vector<CanonBox> boxes;

  double side_len(char side) const { return side == 'N' || side == 'S' ? W : H; }
  double side_depth(char side) const { return side == 'N' || side == 'S' ? H : W; }
  // local (side, a, b) -> canonical cell
  std::array<double, 2> cell(char side, double a, double b) const;
  double area() const { return W * H; }
  // The long sides in a random order, then the short ones (draws).
  std::vector<char> long_sides();
  // The short sides in a random order (draws).
  std::vector<char> short_sides();

  // What's behind the wall at local a on `side`: "ext" | "door" | "wall" | "open".
  virtual std::string_view wall_behind(char side, double a) const = 0;
  // A prefab against a wall; null when it fits nowhere.
  virtual std::optional<Placed> wall(std::string_view key, const WallOpts& opts) = 0;
  // A free-standing prefab at a target point (local to side's frame); null when it fits nowhere.
  virtual std::optional<Placed> free(std::string_view key, const FreeOpts& opts) = 0;
  // RULES[type](this): one of furnish.js's base rules on this context.
  virtual void rule(std::string_view type) = 0;
};

using FurnishRule = void (*)(FurnishCtx& c);
struct CivicRule {
  const char* type = nullptr;
  FurnishRule run = nullptr;
};

// makeCivicRules(RULES): the civic rules, in the reference's key order.
const std::vector<CivicRule>& civic_rules();
// The civic rule of a room type, or null.
FurnishRule civic_rule(std::string_view type);

// RULE_PREFABS (marketCounter, candleStand), in the reference's key order.
const std::vector<Prefab>& rule_prefabs();
const Prefab* rule_prefab(std::string_view key);

// furnish.js's PREFABS after Object.assign(PREFABS, CIVIC_PREFABS, RULE_PREFABS): a key of any
// of the three tables (later tables win; no key is in two), or null.
const Prefab* furnish_prefab(std::string_view key);

}  // namespace svx::city
