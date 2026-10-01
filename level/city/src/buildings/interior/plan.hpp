// svx_city — a building's interior plan (voxel_city buildings/interior/plan.js): the builder the
// floor planners fill (PlanBuilder), the plan it builds (BuildingPlan), planBuilding (the dispatch
// to every planner by archetype, then the street in front of each street door), validatePlan and
// roomAtCell.
//
// BuildingPlan: the full interior of one building, made the first time a LOD 0 chunk needs it
// (World::building_plan, the World's cache of plans):
//
//   floors     {index, z, height, grid, kind} sorted by z (basements < 0)
//   stairs     U-stairs with explicit flights {f, z0, H} and their floor span
//   ramps      straight ramps {rect, f, H} rising (+v) from floor f to f + 1
//   links      room connections that are not doors or stairs (ramps)
//   elevators  shafts with their floor span
//   issues     validation problems (unreachable rooms), expected empty
//
// Floors with one layout share one FloorGrid (typical floors), which keeps tall towers cheap. A plan
// is made by one thread and only read once it is returned (shared_ptr<const BuildingPlan>).
//
// What JS keeps that the port leaves out: plan.env (the envelope the plan was made for: the caller
// holds it; nothing reads it) and plan.extras (never filled by any planner).
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "buildings/archetypes.hpp"
#include "buildings/interior/grid.hpp"
#include "buildings/interior/stairs.hpp"
#include "core/hash.hpp"

namespace svx::city {

class World;

// A part of a floor under a lower ceiling (industrial.js lowRegions: the hall floor over the
// office annex): its rect (canonical) and the height there.
struct LowRegion {
  Rect rect{};
  double height = 0;
};

// A straight ramp (garage.js): its rect, rising towards +v from floor f to f + 1 over H voxels; the
// table pitch of a pitched ramp (0: none - JS leaves the key out); its id (its index in the plan).
struct Ramp {
  Rect rect{};
  double f = 0, H = 0;
  double pitch = 0;
  double id = 0;
};

// An elevator: its car's shaft rect (canonical), the floors f0..f1 it serves, the side its doors
// face (doorSide), its id (its index in the plan; an elevator room's `elevator`).
struct Elevator {
  Rect rect{};
  double f0 = 0, f1 = 0;
  char door_side = 'N';
  double id = 0;
};

// A room connection that is not a door or a stair (plan.links' [[fa, ra], [fb, rb], {ramp}]): room
// ra of floor fa and room rb of floor fb, joined by the ramp of id `ramp`.
struct PlanLink {
  double fa = 0, ra = 0, fb = 0, rb = 0;
  double ramp = 0;
};

// A validation problem ({floor, room, type, msg}): a room people should reach that is not reached.
struct PlanIssue {
  double floor = 0, room = 0;
  std::string type{}, msg{};
};

// A floor of the plan ({index, z, height, grid, kind, ...extra}): floors of one layout may share a
// grid.
struct PlanFloor {
  double index = 0;   // (negative: basements)
  double z = 0;       // the bottom of its slab (floor_z, or the planner's: a mezzanine's)
  double height = 0;  // its story height (floor_height, or the planner's; NaN: undefined)
  std::shared_ptr<FloorGrid> grid{};
  std::string kind{};  // "ground", "upper", "basement", "residential", "office", "garage" ...
  std::vector<LowRegion> low_regions{};  // lowRegions (empty: none)
  bool mezzanine = false;                // (the upper level of an industrial building's annex)
  // (build) the ramps this floor draws: its own (rising from it) and the one arriving from below
  // (JS sets f.ramps only when there is one: empty, none)
  std::vector<Ramp> ramps{};
};

// addFloor's `extra`: what a planner gives a floor beyond its index, grid and kind (JS spreads it
// over the record: a z or height given replaces floor_z's and floor_height's).
struct FloorExtra {
  std::optional<double> z{}, height{};
  std::vector<LowRegion> low_regions{};
  bool mezzanine = false;
};

// A building's interior plan (PlanBuilder.build's record).
struct BuildingPlan {
  std::vector<PlanFloor> floors;               // by z, then index
  std::vector<std::shared_ptr<Stair>> stairs;  // (by id)
  std::vector<Ramp> ramps;                     // (by id)
  std::vector<PlanLink> links;
  std::vector<Elevator> elevators;             // (by id)
  std::vector<PlanIssue> issues;
  // floorByIndex.get(index): the first floor of an index (in floors' order), or null.
  const PlanFloor* floor_by_index(double index) const;
  PlanFloor* floor_by_index(double index);
};

class PlanBuilder {
 public:
  PlanBuilder(const Envelope& env, Rng& rng) : env(env), rng(rng) {}

  const Envelope& env;
  Rng& rng;
  std::vector<PlanFloor> floors;
  std::vector<std::shared_ptr<Stair>> stairs;
  std::vector<Elevator> elevators;
  std::vector<Ramp> ramps;
  std::vector<PlanLink> links;
  std::vector<PlanIssue> issues;

  double z(double f) const { return floor_z(env, f); }
  double h(double f) const { return floor_height(env, f); }
  // A grid of floor f: the footprint of its tier, the chamfered corner cut off from the ground
  // floor up, the doors of a turned building wider (door_extra_of).
  std::shared_ptr<FloorGrid> new_grid(double f) const;
  // Adds floor `index` (its z and height from the envelope unless extra gives them); the reference
  // stays valid until the next add_floor.
  PlanFloor& add_floor(double index, std::shared_ptr<FloorGrid> grid, const std::string& kind, const FloorExtra& extra = {});
  // Adds a stair: its id, and a flight per floor f0 .. f1 - 1 when it has none.
  std::shared_ptr<Stair> add_stair(Stair st);
  // Adds a ramp (its id set); returns it.
  Ramp add_ramp(Ramp r);
  // Adds an elevator (its id set); returns it.
  Elevator add_elevator(Elevator el);
  // build(): the plan - the floors sorted by z then index, each floor's ramps - validated
  // (validate_plan). The builder's lists move into it.
  BuildingPlan build();
};

// validatePlan(plan): a flood fill through rooms and doors from the exterior doors of the ground
// floor (elevator doors closed); stairwells connect vertically, links (ramps) both ways, a room's
// linkTo to its room above. Every room people should reach (not a shaft, an elevator, a mechanical
// shaft or a void) that is not reached is an issue ("unreachable"), appended to plan.issues, which
// it returns.
const std::vector<PlanIssue>& validate_plan(BuildingPlan& plan);

// PLANNERS[archetype]: plans an envelope into pb with the planner of its archetype (houses, cabins,
// churches, apartments, offices, towers (by their upper program), industrial halls, garages,
// schools, town houses (by their upper program) and the civic buildings' CIVIC_PLANNERS); false
// when the archetype has none (pb unchanged).
bool run_planner(const Envelope& env, Rng& rng, PlanBuilder& pb);
bool has_planner(const std::string& archetype);

// planBuilding(world, env): the interior plan of a building - the planner of its archetype on the
// stream (seed, env.id, "interior"), built and validated, then the street in front of each of the
// ground floor's street doors (a door whose street is off the building's level records it,
// door.street; one a walker cannot step up to is raised to it, door.sill). Null when the
// archetype has no planner. (World::building_plan caches it.)
std::shared_ptr<const BuildingPlan> plan_building(const World& world, const Envelope& env);

// roomAtCell(floor, u, v): the room at canonical cell (u, v) of a floor, else whether it is a door
// (JS: the room, "door" or null).
struct RoomAtCell {
  const Room* room = nullptr;
  bool door = false;
};
RoomAtCell room_at_cell(const PlanFloor& floor, double u, double v);

}  // namespace svx::city
