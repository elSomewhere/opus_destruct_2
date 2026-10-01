// svx_city — the builder of a building's interior plan (voxel_city buildings/interior/plan.js
// PlanBuilder): the floors, grids and stairs the floor planners make for one envelope.
//
// Ported so far: what the house and cabin planners (houses.hpp, cabins.hpp) use - new_grid,
// add_floor, add_stair and the floor heights. The port of plan.js adds the rest of the builder
// (ramps, elevators, links, extras, build and validatePlan) and planBuilding.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "buildings/archetypes.hpp"
#include "buildings/interior/grid.hpp"
#include "buildings/interior/stairs.hpp"
#include "core/hash.hpp"

namespace svx::city {

// A floor of the plan ({index, z, height, grid, kind}): floors of one layout may share a grid.
struct PlanFloor {
  double index = 0;   // (negative: basements)
  double z = 0;       // the bottom of its slab (floor_z)
  double height = 0;  // its story height (floor_height)
  std::shared_ptr<FloorGrid> grid{};
  std::string kind{};  // "ground", "upper", "basement" ...
};

class PlanBuilder {
 public:
  PlanBuilder(const Envelope& env, Rng& rng) : env(env), rng(rng) {}

  const Envelope& env;
  Rng& rng;
  std::vector<PlanFloor> floors;
  std::vector<std::shared_ptr<Stair>> stairs;

  double z(double f) const { return floor_z(env, f); }
  double h(double f) const { return floor_height(env, f); }
  // A grid of floor f: the footprint of its tier, the chamfered corner cut off from the ground
  // floor up, the doors of a turned building wider (door_extra_of).
  std::shared_ptr<FloorGrid> new_grid(double f) const;
  // Adds floor `index` (its z and height from the envelope); the reference stays valid until the
  // next add_floor.
  PlanFloor& add_floor(double index, std::shared_ptr<FloorGrid> grid, const std::string& kind);
  // Adds a stair: its id, and a flight per floor f0 .. f1 - 1 when it has none.
  std::shared_ptr<Stair> add_stair(Stair st);
};

}  // namespace svx::city
