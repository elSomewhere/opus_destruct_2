// svx_city — the buildings' feature source (voxel_city buildings/source.js, buildingSource: id
// "buildings", order 10, up to LOD 6). LOD 0 chunks get full interiors once a building plan exists
// (createWorld's world.voxelizeBuilding: buildings/interior); coarser LODs, and a World without
// interiors, use the massing voxelizer. In parts mode (world.angles.partsMode "separate") a turned
// building, a part of its own (Envelope::part), is left out: it is drawn in its lattice
// (world/partRaster). A building's wings (buildings/wings) follow it, in the world grid unless
// parts mode leaves them to their lattices.
//
// The reference reads the envelopes from the world (world.envelopesIn: the cell plans). Until the
// cell plan is ported the functions take them (the envelopes overlapping the rect, in
// envelopesIn's order); the Rect overloads read World::envelopes_in (city/cellPlan), and compose
// wraps them as a FeatureSource.
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "buildings/archetypes.hpp"
#include "core/rect.hpp"
#include "voxel/chunk.hpp"
#include "world/World.hpp"

namespace svx::city {

constexpr const char* kBuildingSourceId = "buildings";
constexpr double kBuildingSourceOrder = 10;
constexpr int kBuildingSourceMaxLod = 6;

using EnvelopeList = std::vector<std::shared_ptr<const Envelope>>;

// world.voxelizeBuilding (createWorld: the interiors, buildings/interior/voxelize), or empty (a
// World of World.js: the massing at every LOD).
using VoxelizeBuildingFn = std::function<void(const Envelope& env, ChunkBuffer& chunk)>;

// buildingSource.zRange over `envs` (the envelopes overlapping the rect): [*z0, *z1] the LOD 0 z range
// of those the world grid draws, or false.
bool building_z_range(const World& world, const EnvelopeList& envs, double* z0, double* z1);
// buildingSource.rasterize over `envs` (the envelopes overlapping the chunk's world box): each
// building's interior at LOD 0 (voxelize_building) or its massing, then its wings in grid mode.
void rasterize_buildings(const World& world, const EnvelopeList& envs, ChunkBuffer& chunk, const VoxelizeBuildingFn& voxelize_building = nullptr);

// (the reference's: the envelopes from the world's cell plans)
inline bool building_z_range(const World& world, const Rect& rect, double* z0, double* z1) { return building_z_range(world, world.envelopes_in(rect), z0, z1); }
inline void rasterize_buildings(const World& world, ChunkBuffer& chunk, const VoxelizeBuildingFn& voxelize_building) {
  const Box3 b = chunk.world_box();
  rasterize_buildings(world, world.envelopes_in(Rect{b.x0, b.y0, b.x1, b.y1}), chunk, voxelize_building);
}

}  // namespace svx::city
