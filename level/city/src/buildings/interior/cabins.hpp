// svx_city — forest cabins (hytte) and wooden churches (voxel_city buildings/interior/cabins.js).
//
// A cabin is one floor, no stair, two strips across the width:
//
//   [ bath  | entry  | kitchen ]   front door into the entry
//   [ bed 1 |                  ]
//   [-------|      living      ]
//   [ bed 2 |                  ]
//
// The side strip holds the bathroom at the front and one or two bedrooms behind it, all opening
// onto the entry or the living room beside them; the main strip has the entry at the front door,
// the kitchen beside it when the strip is wide enough (else behind the living room) and the living
// room. Mirrored for half the cabins. The layout is a pure function of the footprint, so the
// envelope knows where the door is before the plan exists.
#pragma once

#include <string>
#include <vector>

#include "buildings/archetypes.hpp"
#include "buildings/interior/plan.hpp"
#include "core/hash.hpp"
#include "core/rect.hpp"

namespace svx::city {

// A cabin's room: its key (bath, bed1, bed2, entry, kitchen, living), type and rect (canonical).
struct CabinRoom {
  std::string key{}, type{};
  Rect rect{};
};
struct CabinLayout {
  std::vector<CabinRoom> rooms{};
  double entrance_u = 0;  // the front door's middle
};
// cabinLayout(U, V, mirror): the room rects and front door of a U x V cabin.
CabinLayout cabin_layout(double U, double V, bool mirror);

// planCabin({env, rng, pb}): a cabin's one floor into pb.
void plan_cabin(const Envelope& env, Rng& rng, PlanBuilder& pb);

// planChurch({env, pb}): a wooden church - the tower base is the vestibule (the front door), one
// wide door leads into the open nave (pews and an altar; an Orthodox church's icon screen under
// onion domes). The tower rect overlaps the nave's front wall, so the two share a 1-cell partition.
void plan_church(const Envelope& env, PlanBuilder& pb);

}  // namespace svx::city
