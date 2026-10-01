// svx_city — multi-storey parking garages (voxel_city buildings/interior/garage.js).
//
// Open decks around a stack of straight ramps along one side wall: the ramp of deck f climbs from
// the front cross aisle to the back cross aisle of deck f + 1, so every deck repeats the same
// circulation (up the ramp, back along the main aisle, up the next ramp). A stair core in the back
// corner on the other side serves pedestrians. Stall rows (parked cars, painted lines) and pillar
// lines fill the deck between the aisles (the deck room's stalls, pillars, ramp_rect, deck_h).
//
// Plan features beyond rooms: the plan's ramps ({rect, f, H, pitch}: rising towards +v from floor
// f to f + 1; pitch the table pitch of a pitched ramp, env.pitched_ramps) and links (the decks a
// ramp joins).
#pragma once

#include "buildings/archetypes.hpp"
#include "buildings/interior/plan.hpp"
#include "core/hash.hpp"

namespace svx::city {

// planGarage({env, rng, pb}).
void plan_garage(const Envelope& env, Rng& rng, PlanBuilder& pb);

}  // namespace svx::city
