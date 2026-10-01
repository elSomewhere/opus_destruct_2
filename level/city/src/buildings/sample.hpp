// svx_city — sample buildings of an archetype staged on a world's real, graded lots in place of
// the buildings the lots carry (voxel_city buildings/sample.js), for tests and debug renders of
// archetypes no district places yet. A staged envelope keeps its lot's building id, and the world
// serves it in place of the original (envelopesIn), so chunks, plans and the walker see it like any
// other building; the lot keeps its original ground and yard dressing.
//
// The reference reaches into the world (world.cellPlan, world.envelopesIn, world.buildingPlans).
// Until the cell plan is ported, stage_archetype reads a world through StageWorld: its seed and
// config, a cell's lots and buildings, the staged buildings (StagedEnvelopes: what the World's
// envelopes_in will consult) and a hook that drops a building's cached plan.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "buildings/archetypes.hpp"
#include "city/districts.hpp"
#include "city/lots.hpp"
#include "core/rect.hpp"
#include "core/value.hpp"

namespace svx::city {

// The buildings staged on a world (stagedMap's Map: id -> envelope, in staging order).
class StagedEnvelopes {
 public:
  bool has(const std::string& id) const;
  // (Map.set: an id staged again keeps its place)
  void set(std::shared_ptr<const Envelope> env);
  std::vector<std::shared_ptr<const Envelope>> all() const;
  // envelopesIn(rect) once buildings are staged: the world's envelopes (base) but the staged ids,
  // then every staged envelope whose bounds overlap rect, in staging order.
  std::vector<std::shared_ptr<const Envelope>> envelopes_in(const Rect& rect, const std::vector<std::shared_ptr<const Envelope>>& base) const;

 private:
  mutable std::mutex m_;
  std::vector<std::shared_ptr<const Envelope>> list_;
};

// A cell plan as stage_archetype reads it: its lots, in order, and its buildings (buildingById).
struct StagePlan {
  std::vector<Lot> lots{};
  std::vector<std::shared_ptr<const Envelope>> buildings{};
  // plan.buildingById.get(id) (a Map of the buildings: the last of an id), or null
  const Envelope* building_by_id(const std::string& id) const;
};

// What stage_archetype reads of a world and what it changes.
struct StageWorld {
  double seed = 0;
  const Value* config = nullptr;
  std::function<std::shared_ptr<const StagePlan>(double i, double j)> cell_plan{};  // world.cellPlan(i, j)
  StagedEnvelopes* staged = nullptr;                                                 // the world's staged map
  std::function<void(const std::string& id)> drop_plan{};  // world.buildingPlans?.map?.delete(id) (empty: none)
};

// stageArchetype's opts.
struct StageOpts {
  double n = 3;                       // the number of buildings to stage
  const District* district = nullptr;  // the envelopes' district (null: the lot's district id, 2-3 floors)
  std::optional<std::vector<std::string>> from{};  // the archetypes of the buildings it replaces (none: any)
  double radius = 3;                   // the cell search radius round (0, 0)
  // a canonical sub-rect of a lot to build on (a cabin-sized plot at the front of a large lot), or
  // none to skip the lot (empty: the whole lot)
  std::function<std::optional<Rect>(const Lot& lot, const Frame& lf)> lot_rect{};
};

// stageArchetype(world, archetypeId, styleId, opts): stages up to n buildings of an archetype and
// style on the lots of the cells round (0, 0), ring by ring, in place of the buildings standing
// there (each from its own stream: (seed, lot id, "stage", archetype)); returns them.
std::vector<std::shared_ptr<const Envelope>> stage_archetype(const StageWorld& world, const std::string& archetype_id, const std::string& style_id,
                                                             const StageOpts& opts = {});

}  // namespace svx::city
