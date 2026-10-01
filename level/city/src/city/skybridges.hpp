// svx_city — skybridges (voxel_city city/skybridges.js): enclosed glass walkways between office
// buildings that face each other across a street, at podium level (floor 3 up, above lamps,
// signals and street trees). Planned per cell once every envelope is known: the bridge is recorded
// on the cell plan (geometry) and on both envelopes (Envelope::sky_doors), so the lazily planned
// interiors carve a door exactly where the bridge meets the facade. Futuristic cities build many;
// modern downtowns a few.
//
// The feature source (skybridgeSource: id "skybridges", order 9, up to LOD 4) draws the bridges of
// the cells overlapping a chunk (bridgesNear: world.cellsOverlapping, each cell plan's skybridges
// whose rect overlaps): the functions here take the bridges near, which compose gathers from the
// cell plans (skybridges_in per cell).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "buildings/archetypes.hpp"
#include "core/placement.hpp"
#include "core/rect.hpp"
#include "voxel/chunk.hpp"
#include "world/World.hpp"

namespace svx::city {

// A skybridge (planSkybridges' record): from building `a` (facing south or east) to building `b`
// across the street, along x when A faces south; its world rect, its deck's level at each end, and
// its world box.
struct Skybridge {
  std::string id;  // `${plan.id}/sky${n}`
  std::string a, b;
  bool along_x = true;
  Rect rect{};
  double za = 0, zb = 0;
  Box3 bb{};  // {...rect, z0, z1}
};

// planSkybridges(world, plan, buildings, corridors): the skybridges of a cell (plan_id: its id)
// between its buildings (the cell plan's envelopes, in order: their sky_doors are set), off the
// highway corridors (corridor_hits(rect): does a corridor of the cell hit the rect -
// corridors.some(k => k.hitsRect(rect)); empty: none).
std::vector<Skybridge> plan_skybridges(const World& world, const std::string& plan_id, const std::vector<Envelope*>& buildings,
                                       const std::function<bool(const Rect&)>& corridor_hits);

// ---- skybridgeSource

constexpr const char* kSkybridgeSourceId = "skybridges";
constexpr double kSkybridgeSourceOrder = 9;
constexpr int kSkybridgeSourceMaxLod = 4;

// bridgesNear's test for one cell plan's bridges: those whose rect overlaps `rect`, in order.
std::vector<const Skybridge*> skybridges_in(const std::vector<Skybridge>& bridges, const Rect& rect);
// zRange over the bridges near a rect: [*z0, *z1], or false.
bool skybridge_z_range(const std::vector<const Skybridge*>& near, double* z0, double* z1);
// rasterize: a slab, glass sides with mullions, a roof with a light strip, for each bridge near
// the chunk's world box.
void rasterize_skybridges(const std::vector<const Skybridge*>& near, ChunkBuffer& chunk);

}  // namespace svx::city
