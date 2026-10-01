// svx_city — island mode (voxel_city world/island.js): the land is one island of
// `config.world.island.radius` in an endless sea. The plan is a set of pure functions of position
// (metres) plus a handful of settlements sited once per world:
//
//   coast(x, y)     approximate distance (m) to the shore, positive inland: a warped ellipse with
//                   bays, headlands and points, and fjords that wind into the highlands from the sea
//   highland(x, y)  0..1 mask of the fells / mountains on one side of the island
//                   (MacroFields::mountainness in island mode)
//   cliff(x, y)     0..1 coast type: 0 low shores with beaches, 1 cliffs
//   skerry(x,y,c)   metres of rock islets rising from the shelf
//
// The island's main town sits at the world origin (the spawn): the plan sites it on a sheltered
// stretch of lowland coast and shifts the whole island so it lands there. Smaller towns, villages
// and hamlets follow. (islandTerrain, the island's terrain scales, is config/defaults' island_terrain.)
//
// Immutable after construction except for its lazy products (settlements, harbour, trunk edges),
// each made once whichever thread asks first: any thread may use a plan.
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/cache.hpp"
#include "core/noise.hpp"
#include "core/rect.hpp"
#include "core/value.hpp"

namespace svx::city {

class MacroFields;
class World;
struct Settlement;

// A place sited by the plan (island-local metres).
struct IslandSite {
  std::string kind;  // town, smallTown, village, hamlet
  double lx = 0, ly = 0, radius = 0, importance = 0;
};

// The main town's harbour: the shore point nearest its centre (world metres, r its distance) and
// the direction from the town out to the water.
struct Harbour {
  double r = 0, x = 0, y = 0, dx = 0, dy = 0;
};

// The island's settlement records (MacroFields' shape, voxels): towns (the main town is S0_0 at the
// origin) and villages / hamlets.
struct IslandSettlements {
  std::vector<std::shared_ptr<const Settlement>> towns, villages;
};

// trunkEdges: the arterial-lattice edges ({axis, line, span}, city/cellNetwork's edgeInfo) that
// always carry a country road (JS: a Set of the strings `${axis}:${line}:${span}`).
class TrunkEdges {
 public:
  bool has(double axis, double line, double span) const { return set_.count(Key{axis + 0.0, line + 0.0, span + 0.0}) > 0; }
  void add(double axis, double line, double span);
  size_t size() const { return order_.size(); }
  // The edges in insertion order (the JS Set's iteration order): {axis, line, span} each.
  const std::vector<std::array<double, 3>>& items() const { return order_; }

 private:
  struct Key {
    double a, l, s;
    bool operator==(const Key& o) const { return a == o.a && l == o.l && s == o.s; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const;
  };
  std::unordered_set<Key, KeyHash> set_;
  std::vector<std::array<double, 3>> order_;
};

class IslandPlan {
 public:
  explicit IslandPlan(const Value& config);
  IslandPlan(const IslandPlan&) = delete;
  IslandPlan& operator=(const IslandPlan&) = delete;

  // config.world.island (the plan reads it as `cfg`), and the values the plan reads from it.
  Value cfg;
  double seed = 0;
  double R = 0;          // cfg.radius
  double roughness = 0;  // cfg.roughness ?? 0.5
  double highlands = 0;  // cfg.highlands ?? 0.45
  double fjords = 0;     // cfg.fjords ?? 0
  double cliffs = 0;     // cfg.cliffs ?? 0.35
  double skerries = 0;   // cfg.skerries ?? 0
  double shelf = 0;      // cfg.shelf ?? 2200 (landforms' islandBase)
  double town_rise = 0;  // cfg.townRise ?? 0 (terrain)
  double population = 0; // cfg.population ?? 20000
  std::string flavor;    // cfg.flavor ?? null ("": null)

  double theta = 0, a = 0, b = 0, cos_ = 0, sin_ = 0;  // the ellipse
  double hdx = 0, hdy = 0;                             // the direction the highlands lie in
  double fjord_count = 0, fjord_phase = 0;
  // island-local frame: world metres = local - origin (the main town at 0, 0)
  double ox = 0, oy = 0;
  double high_threshold = 0;  // (Infinity: no highlands)
  std::vector<IslandSite> sites;

  // Island-local coordinates (m) of a world point (m).
  std::array<double, 2> local(double x, double y) const { return {x + ox, y + oy}; }
  // Coast distance of the smooth island shape (no fjords), island-local metres.
  double shape(double lx, double ly) const;
  // Highland value before thresholding (0..~1.4): a tilt towards one side plus broad noise.
  double high_raw(double lx, double ly) const;
  // 0..1 highland mask (island-local metres).
  double highland_local(double lx, double ly) const;
  // The coast distance d cut by the fjords (island-local metres).
  double fjord_cut(double lx, double ly, double d) const;
  // Coast distance (m, positive inland) at a world point (metres).
  double coast(double x, double y) const { return fjord_cut(x + ox, y + oy, shape(x + ox, y + oy)); }
  // Highland mask at a world point (metres).
  double highland(double x, double y) const { return highland_local(x + ox, y + oy); }
  // Coast type 0 (beaches) .. 1 (cliffs) at a world point (metres).
  double cliff(double x, double y) const;
  // Metres of skerry rock above the shelf at a world point (metres) with coast distance c.
  double skerry(double x, double y, double c) const;

  // The settlement records (voxels): `fields` fills in the regional climate. Made once.
  const IslandSettlements& settlements(const MacroFields& fields) const;
  // The main town's harbour, or nothing. Made once.
  const std::optional<Harbour>& harbour() const;
  // Country trunk roads joining every place on the island (lazy, once): settlements linked into a
  // tree (each to its nearest already linked one), each link following the arterial grid by A*,
  // avoiding the sea and steep ground.
  const TrunkEdges& trunk_edges(const World& world) const;
  // Axis-aligned bounds (world metres) that contain the whole island and its skerries.
  Rect bounds() const;

 private:
  SimplexNoise n_warp_u_, n_warp_v_, n_bays_, n_points_, n_high_, n_fjord_, n_fjord_warp_, n_cliff_, n_skerry_;
  double calibrate_highlands() const;
  void site_settlements();
  Lazy<IslandSettlements> settlements_;
  Lazy<std::optional<Harbour>> harbour_;
  Lazy<TrunkEdges> trunk_;
};

}  // namespace svx::city
