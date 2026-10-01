// svx_city — the coarse building voxelizer (voxel_city buildings/massing.js): solid massing with
// the shared facade rules, flat roofs with parapets, pitched roofs, setbacks, annexes (garages,
// petrol canopies, sign pylons), church steeples and onion domes. Used for every LOD above 0 (and
// as a fallback); point-sampled through the chunk writer, so it works at any resolution.
//
// Snow on roofs (world/season): the season's snow cover (0..1) at the building's local
// temperature (its town's climate cooled by the height of its ground floor), or a climate's
// explicit cover, lies on the outermost surface layer of pitched, flat and annex roofs, in the
// coarse massing and in the LOD 0 interiors alike. Eaves and verge rows and parapet caps stay
// bare so roof outlines still read; below 1 a deterministic share of the roof surface is bare, in
// patches of a few metres (smooth value noise of world x, y, so every LOD agrees).
//
// Everything here only reads the envelope (its frame and look are made once, from any thread:
// envelope_frame, building_look) and writes the chunk it is given.
#pragma once

#include "buildings/archetypes.hpp"
#include "voxel/chunk.hpp"
#include "world/World.hpp"

namespace svx::city {

// roofSnowCover(world, env): the snow cover (0..1) of the building's roofs, made once per envelope
// (Envelope::snow_cache) unless the envelope carries one (Envelope::snow: a turned building's
// envelope in its part's lattice takes the cover where the building stands).
double roof_snow_cover(const World& world, const Envelope& env);

// snowAt(seed, cover, x, y): is the roof surface at world column (x, y) under snow for this cover?
bool snow_at(double seed, double cover, double x, double y);

// voxelizeMassing(world, env, chunk): the building's shell (walls with their facades, floor slabs,
// roofs, annexes, basements, a plinth, a steeple and domes) into a chunk at any LOD.
void voxelize_massing(const World& world, const Envelope& env, ChunkBuffer& chunk);

// pitchedRoofOnly(world, env, chunk): pitched roofs and annexes only (the LOD 0 interiors draw
// everything else).
void pitched_roof_only(const World& world, const Envelope& env, ChunkBuffer& chunk);

}  // namespace svx::city
