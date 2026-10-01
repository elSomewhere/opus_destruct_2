// svx_city — macro fields (voxel_city world/fields.js): continuous, pointwise functions over the
// whole world. They answer "what kind of place is this?" (urbanization, downtown-ness, moisture,
// temperature, settlement membership) without neighbourhood state, so they are valid anywhere on
// an infinite chart. Positions are world VOXEL coordinates (converted to metres inside).
//
// Settlements and villages sit on lattices (settlementCell, villageCell): each cell holds at most
// one, decided by hashes of its index; island mode replaces the lattices by the island plan's
// places (world/island). A settlement record is made once and kept for the world's lifetime (JS:
// a Map that never forgets), so a `const Settlement*` stays valid while the MacroFields lives and
// identity comparisons mean what they mean in JS. Records carry the lazy fields the reference
// caches on them (base_h: terrain's settlementBase, port_lake, plan).
//
// Thread-safe: everything is const; the settlement caches are made under a lock (a record is made
// outside it; the first stored wins).
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/cache.hpp"
#include "core/noise.hpp"
#include "core/rect.hpp"
#include "core/value.hpp"
#include "world/chart.hpp"
#include "world/wrap.hpp"

namespace svx::city {

class IslandPlan;
struct IslandSettlements;
struct Lake;      // nature/lakes (a later stage of the port)
struct TownPlan;  // city/townPlan (a later stage of the port)

// A town (settlement lattice, or an island's town) or a village / hamlet.
struct Settlement {
  std::string id;  // S{i}_{j}, V{i}_{j}; islands S0_0, S{n}_1000, V{n}_1000
  // The lattice cell. JS's `i` of a village is the string "v{i}" (never equal to a number, ToInt32
  // 0 when hashed): `i` here is the number, `village` tells the two apart (i_js()).
  double i = 0, j = 0;
  bool village = false;
  bool hamlet = false;   // (villages)
  double x = 0, y = 0;   // centre (voxels)
  double radius = 0;     // (voxels)
  double importance = 0;
  double style = 0;      // a uint32 hash
  double peak = 0;       // (villages) the urbanization at the centre
  // Canonical centre (a wrapping world hashes it; equal to x, y elsewhere). JS leaves cx / cy
  // undefined on island places, where `cx ?? x` reads x: the same as here.
  double cx = 0, cy = 0;
  double t = 0, m = 0;   // regional climate at the centre
  std::string flavor;    // (island places: config.world.island.flavor; "": none / null)
  bool island = false;   // (island places)

  // Lazily cached per record (fields.js lapCopy starts them over on a lap's copy):
  Lazy<double> base_h;                          // terrain/terrain settlementBase (metres)
  Lazy<std::shared_ptr<const Lake>> port_lake;  // nature/lakes portLakeOf
  Lazy<std::shared_ptr<const TownPlan>> plan;   // city/townPlan

  // JS's `i`: a number for towns, "v{i}" for villages.
  std::string i_js() const;
};

// One settlement's share of a point's urbanization (urban().parts: JS's [s, w] pairs).
struct UrbanPart {
  const Settlement* s = nullptr;
  double w = 0;
};

// urban(x, y): urbanization u in [0, 1], the core (downtown-ness), the dominant settlement (or
// null), every settlement's share (parts, in visit order), and the proximity to any (prox).
struct Urban {
  double u = 0, core = 0;
  const Settlement* settlement = nullptr;
  std::vector<UrbanPart> parts;
  double prox = 0;
};

class MacroFields {
 public:
  MacroFields(const Value& config, std::shared_ptr<const Chart> chart);
  ~MacroFields();
  MacroFields(const MacroFields&) = delete;
  MacroFields& operator=(const MacroFields&) = delete;

  const Value& config() const { return config_; }
  const Chart& chart() const { return *chart_; }
  // a wrapping world: settlement and village lattices repeat round it
  Wrap wrap;
  double n_town = 0, n_village = 0;
  // the domain offset of the mountain fields (pickMountainOffset; 0, 0 on an island)
  std::array<double, 2> m_off{0, 0};
  // island mode: one island in the sea, its settlements sited by the plan (null elsewhere)
  std::unique_ptr<IslandPlan> island;

  // Island settlements (towns, villages), sited once (island mode only).
  const IslandSettlements& island_settlements() const;
  // Distance (m) from a point (voxels) to the shore, positive on land: island mode only
  // (Infinity elsewhere, where the sea is simply terrain below sea level).
  double coast_distance(double x, double y) const;
  // Towns whose disk may touch a rect (voxels): the lattice's, or the island's.
  std::vector<const Settlement*> settlements_in(const Rect& r) const;
  // Villages and hamlets that may touch a rect (voxels).
  std::vector<const Settlement*> villages_in(const Rect& r) const;
  // The settlement anchored in macro cell (i, j), or null.
  const Settlement* settlement(double i, double j) const;
  // Highest mountainness over a town's foothill buffer (centre and a ring just beyond 2.6 radii;
  // metres).
  double mountains_around(double mx, double my, double radius) const;
  // The village or hamlet anchored in village-lattice cell (i, j), or null.
  const Settlement* village(double i, double j) const;
  std::vector<const Settlement*> nearest_villages(double x, double y) const;
  std::vector<const Settlement*> nearest_settlements(double x, double y) const;
  // Domain-warp factor of settlement distances at a point (shared by all settlements).
  double settlement_warp(double x, double y) const;
  // Domain-warped normalized distance to a settlement centre (0 centre, 1 edge), each place with
  // its own lobes; warp: settlement_warp(x, y), computed when not given.
  double settlement_distance(const Settlement& s, double x, double y) const;
  double settlement_distance(const Settlement& s, double x, double y, double warp) const;
  // Mid-scale noise (-1..1, ~250 m) that frays the edges of towns.
  double fringe_noise(double x, double y) const;
  // Urbanization in [0, 1] plus the dominant settlement.
  Urban urban(double x, double y) const;
  // 1 inside towns fading to 0 at about twice their radius: foothills round every town.
  double settlement_proximity(double x, double y) const;
  // Mountainness 0..1: long mountain belts plus isolated massifs (island: the highlands).
  double mountainness(double x, double y) const;
  // Moisture 0..1.
  double moisture(double x, double y) const;
  // Temperature 0..1 at sea level (the chart's latitude cools it where it has one).
  double temperature(double x, double y) const;
  // Low-frequency noise varying the district mix within a city (-1..1).
  double district_noise(double x, double y) const;
  // Industry: scattered industrial patches plus each town's industrial quarter.
  double industry_noise(double x, double y) const;
  double style_noise(double x, double y) const;

 private:
  struct Cfg;
  struct Cache;
  Value config_;
  std::shared_ptr<const Chart> chart_;
  std::unique_ptr<const Cfg> cfg_;
  std::unique_ptr<Cache> cache_;
  SimplexNoise n_warp_, n_urban_, n_moist_, n_temp_, n_district_, n_industry_, n_style_, n_belt_, n_massif_, n_belt_warp_, n_lobe_, n_fringe_;
  std::array<double, 2> pick_mountain_offset();
  std::shared_ptr<const Settlement> make_settlement(double i, double j) const;
  std::shared_ptr<const Settlement> make_village(double i, double j) const;
};

}  // namespace svx::city
