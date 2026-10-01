// svx_city — a parking garage's ramps as pitched parts (voxel_city buildings/garageRamps.js, the
// angled world's S4 for the decks of a garage): a garage the cell marks `pitchedRamps` plans each
// ramp at the gentlest table grade that fits (interior/garage: its run H c / s), and a ramp the
// budget grants is one slab in a lattice climbing along it, a smooth plane from one deck to the
// next where the world grid steps (interior/voxelize's rampColumn draws the rest, and every ramp in
// grid mode). Structure, not ground: not anchored, cast between the decks the world grid holds,
// which own the ends it shares with them (it yields to the world grid).
//
// Its lattice: u up the ramp (the garage's canonical +v), v across it (canonical -u, from the
// ramp's right edge), w up from its surface; the corner of local cell (0, 0, 0) at canonical
// (x1 + 1, y0), at the height of the lower deck's floor surface.
#pragma once

#include "buildings/archetypes.hpp"
#include "core/rect.hpp"
#include "voxel/chunk.hpp"
#include "world/World.hpp"
#include "world/parts.hpp"

namespace svx::city {

// rampPart(cell, env, r): the part of a ramp of garage `env` - the plan's ramp {rect (canonical),
// f (the floor it rises from), pitch (its table pitch, 1 .. 6)} - index 0 until granted; its
// env and ramp {f, W, Lu} set.
Part ramp_part(const PartCell& cell, const Envelope& env, const Rect& rect, double f, int pitch);

// rasterizeRampPart(world, part, chunk): a ramp part's content chunk of its lattice
// (world/partRaster): its surface, the slab under it, its kerbs; at a coarse LOD a cell is the
// slab's where its range meets it.
void rasterize_ramp_part(const World& world, const Part& part, ChunkBuffer& chunk);

}  // namespace svx::city
