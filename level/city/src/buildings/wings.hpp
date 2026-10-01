// svx_city — wings, corner bays, canted bays and chamfers (voxel_city buildings/wings.js,
// ANGLED_WORLD_PLAN.md S5): masses a building adds to its main one, each an oriented part of its
// own at a yaw of the table, cast into the main mass rather than merely touching it: its back face
// lies inside the main footprint, past the wall, and the main mass owns what the two share
// (structvox priority: a building's above its wings'). So in the world grid a wing draws only what
// lies outside the main footprint, and in its own lattice it holds that and the band of the main
// wall it is cast into (world/partRaster). Upper floors only (floor 1 up to the top floor, one
// less under a pitched roof): the street, the doors and the ground stay the building's.
//
//   corner   a bay across the building's street corner, facing it at 43.6 or 46.4 degrees (the
//            20-21-29 triple: no table yaw is 45), over the corner of its sidewalks
//   bay      a canted bay on the front facade over one window bay, turned 12.7 or 16.3 degrees,
//            projecting 0.25 m at one end, 1.2 m at most at the other
//   wing     a lot cut back by a slanted street: the upper floors fill the corner between the
//            building and the street, facing it at its exact yaw
//   chamfer  a flat-roofed building's street corner cut off (buildings/chamfer: 43.6 or 46.4
//            degrees to both streets, 29 k cells), every floor from the ground up: the floor plans
//            lose the cut, a slab on the cut line carries the facade and its roof's parapet. It
//            owns what it shares with the building (the stepped wall of the cut behind it), so its
//            priority is above the world grid's
//
// A wing hangs over a sidewalk only of a street its own cell owns (whose dressing keeps street
// trees clear of it), keeps a metre from the kerb and within its lot's frontage, and never over a
// carriageway. A turned building gets the canted bays of its front, turned by the exact product of
// its yaw and the bay's (a triple of its own, no table yaw); corner bays and wings to a slanted
// street are the square buildings'. (The record: buildings/wing.hpp. nearWing is the interior
// planners'.)
#pragma once

#include <string>
#include <vector>

#include "buildings/archetypes.hpp"
#include "buildings/wing.hpp"
#include "city/lots.hpp"
#include "core/hash.hpp"
#include "core/placement.hpp"
#include "voxel/chunk.hpp"
#include "world/World.hpp"

namespace svx::city {

struct Block;  // (city/cellNetwork.hpp)
class RoadView;

// EMBED: cells of the main wall a wing is cast into, and how far inside the main footprint its back
// corners lie at least.
constexpr double kEmbed = 2;

// planWings(world, env, lot, block, view, cellId, rng): the wings a building may carry, in the
// order a cell grants them (a corner bay or a chamfer, a wing to a slanted street, a canted bay),
// two at most: geometry only, checked clear of the streets and in its lot (the cell checks other
// buildings and grants the parts). `lot` the building's lot (null: none), `block` its block (its
// cuts; null: none), `view` the cell's road view, `cell_id` the cell's id (the roads it owns),
// `rng` the building's own stream (Rng::from(seed, env.id, "wings")). The wings come without key
// and part (the cell plan sets them).
std::vector<Wing> plan_wings(const World& world, const Envelope& env, const Lot* lot, const Block* block, const RoadView& view, const std::string& cell_id, Rng& rng);

// wingPlacement(w): the placement of a wing's lattice (its w the world's z).
Placement wing_placement(const Wing& w);

// rasterizeWing(world, env, w, chunk): a wing in the world grid (the building source, grid mode):
// its cells outside the main footprint of their floor (what the main mass does not own), every
// LOD; a chamfer's slab over the stepped wall of the cut (it owns what they share).
void rasterize_wing(const World& world, const Envelope& env, const Wing& w, ChunkBuffer& chunk);

// rasterizeWingPart(world, env, w, chunk): a wing's content in its own lattice (world/partRaster,
// parts mode; the chunk's coordinates are the wing's u, v and the world's z): the same cells, and
// where the main footprint holds its cell's centre the band of the main wall it is cast into (EMBED
// cells in from the footprint's edge) solid, deeper in nothing (the main mass owns it).
void rasterize_wing_part(const World& world, const Envelope& env, const Wing& w, ChunkBuffer& chunk);

}  // namespace svx::city
