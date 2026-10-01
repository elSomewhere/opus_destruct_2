// svx_city — apartment buildings: walk-ups, mid-rises, residential towers, panel slabs and towers,
// wharf houses and town houses of flats (voxel_city buildings/interior/apartments.js).
//
// Two circulation systems, chosen from the typical floor's depth:
//
//   corridor  a double-loaded corridor along the building, cores (stair + elevator) in the back
//             zone opening onto the corridor
//   sections  point access: each section has a stair hall at the front and a stair behind it;
//             units left and right of it
//
// The core layout is derived from the top (smallest) tier so it stacks through every floor,
// basements and podiums included. Ground floors hold flats, a lobby, or shops (retail programs);
// a residential tower's podium floors are open offices; the typical residential floors share one
// grid, with balcony doors where the style asks for them.
#pragma once

#include <memory>
#include <string>

#include "buildings/archetypes.hpp"
#include "buildings/interior/grid.hpp"
#include "buildings/interior/plan.hpp"
#include "core/hash.hpp"
#include "core/rect.hpp"

namespace svx::city {

// planApartmentBuilding({env, rng, pb}).
void plan_apartment_building(const Envelope& env, Rng& rng, PlanBuilder& pb);

// shopWithBackroom(grid, ctx, rect, backCirc, kindOverride): a shop - its sales floor with a street
// door (its kind by the town flavor's tenants, env's flavor: shops, cafés, bars, services), behind
// it when the rect is deep and wide enough a back room (with a WC beside it when wider) and a door
// from the back room to back_circ. Draws from rng (the kind unless kind_override gives it, the
// finishes). Returns the shop room.
std::shared_ptr<Room> shop_with_backroom(FloorGrid& grid, const Envelope* env, Rng& rng, const Rect& rect, const Room* back_circ,
                                         const std::string* kind_override = nullptr);

}  // namespace svx::city
