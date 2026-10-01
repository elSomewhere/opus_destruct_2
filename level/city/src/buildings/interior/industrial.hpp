// svx_city — warehouses, factories and barns (voxel_city buildings/interior/industrial.js): one
// tall hall with loading doors, plus a two-level office annex in a front corner. The annex's upper
// level is a mezzanine floor record stacked inside the hall; the hall floor is capped to the annex
// ceiling height over the annex footprint (a low region).
#pragma once

#include "buildings/archetypes.hpp"
#include "buildings/interior/plan.hpp"
#include "core/hash.hpp"

namespace svx::city {

// planIndustrial({env, rng, pb}): the hall by the ground program (warehouse, factory, barn,
// distribution, selfStorage, coldStore, timberYard: their finishes; anything else a warehouse).
void plan_industrial(const Envelope& env, Rng& rng, PlanBuilder& pb);

}  // namespace svx::city
