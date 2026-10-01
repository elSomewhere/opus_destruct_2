// structvox game — what a generated world means (docs/PROCGEN_MERGE_PLAN.md §10.8): plan-level
// queries a generator answers from its plans, without voxelizing - its buildings and their
// entrances, its furniture and what it affords, the zones a point is in - for AI and gameplay. A
// streamed world's source gives them (GameSource::semantics; its entities: GameSource::spawns_in).
// World metres throughout; records in a stable order.
#pragma once

#include <string>
#include <vector>

#include "svx/base/types.hpp"
#include "svx/base/vec.hpp"

namespace svx {

// A door to the street: where it is (on the ground, at the threshold), the way out of it, and the
// street it opens onto ("": none known).
struct Entrance {
  V3 pos;
  V3 facing;
  std::string street;
};

struct BuildingInfo {
  std::string id;
  std::vector<V3> footprint;     // its outline at the ground (counter-clockwise; z: the ground level)
  std::vector<f64> floors;       // the levels of its floors (z of each floor's top, bottom up; basements first)
  std::string program;           // "residential", "office", "shop:<kind>", "civic:<kind>", "industrial", ...
  std::vector<Entrance> entrances;
};

// What a piece of furniture affords and where a person does it from (a seat, a bed's side, a desk's
// chair, a counter's front).
enum class Affordance : u8 { Sit, Sleep, Work, Eat, Open, Climb };
struct Use {
  Affordance kind = Affordance::Sit;
  V3 pos;          // where the person is (feet)
  f64 yaw = 0.0;   // which way they face
};

struct FurnitureInfo {
  u64 id = 0;            // stable (the loose object's: the same piece of furniture, the same id)
  std::string prefab;    // its kind ("sofa", "bed", "desk", ...)
  V3 pos;                // its place (its footprint's centre, on the floor)
  f64 yaw = 0.0;
  std::string building;  // the building it stands in ("": outdoors)
  std::vector<Use> uses;
};

// The zones a point is in ("": none there).
struct ZoneInfo {
  std::string district, settlement, flavor;
};

class WorldSemantics {
 public:
  virtual ~WorldSemantics() = default;
  // The buildings whose footprints reach into the box (x, y).
  virtual void buildings_in(const V3& lo, const V3& hi, std::vector<BuildingInfo>& out) const = 0;
  // The furniture in the box (x, y, z).
  virtual void furniture_in(const V3& lo, const V3& hi, std::vector<FurnitureInfo>& out) const = 0;
  virtual ZoneInfo zone_at(const V3& p) const = 0;
  // (Designed, built with the AI work: a building's rooms - floor, type, outline, doors and what
  // they connect, stairs and lifts - and places by kind: bus stops, benches, counters, squares,
  // parks, markets, churches, stations; anchors for people: homes and workplaces.)
};

}  // namespace svx
