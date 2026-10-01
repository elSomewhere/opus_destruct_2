// svx_city — the site kit's boxes and structures (voxel_city sites/kit.js: box, finishStructure).
//
// Sites (world/sites) and their underground complexes (sites/complex) build their geometry as
// lists of boxes in world voxels: shells (written over solid only: walls in the rock), carves
// (material 0: the voids) and details (floors, props, stairs), each box {x0..z1, m, mode} with
// mode 0 overwrite, 1 only air, 2 only solid (voxel/chunk fill_box). A site's structure is those
// boxes in that order behind a spatial grid, with custom volumes (a rock cavern) rasterized
// before them.
//
// (The kit's surface structures - fences, gates, towers, radomes, tanks, trucks, portal blocks -
// and planGate come with the site kinds: sites/militaryBase, researchComplex, mountainBase.)
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "core/geom2d.hpp"
#include "core/placement.hpp"

namespace svx::city {

class ChunkBuffer;
struct Complex;

// A box of a site's geometry: inclusive world voxel bounds, a material (0 carves), a fill mode.
struct SiteBox {
  double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
  uint16_t m = 0;
  int mode = 0;
};

// A custom volume: its bounds and a rasterizer writing a chunk directly (custom: { bb,
// rasterize(chunk) }).
struct SiteVolume {
  Box3 bb{};
  std::function<void(ChunkBuffer& chunk)> rasterize;
};

// The lists a site's builders write ({ shells, carves, details, custom }).
struct BoxLists {
  std::vector<SiteBox> shells, carves, details;
  std::vector<SiteVolume> custom;
};

// box(list, x0, y0, z0, x1, y1, z1, m, mode): a box with its x and y bounds ordered.
inline void box(std::vector<SiteBox>& list, double x0, double y0, double z0, double x1, double y1, double z1, uint16_t m, int mode = 0) {
  list.push_back({js::min(x0, x1), js::min(y0, y1), z0, js::max(x0, x1), js::max(y0, y1), z1, m, mode});
}

// finishStructure's product: the boxes (shells, then carves, then details: a box's index is its
// order), a spatial grid of their indices by footprint (cells of 128 voxels), the bounds of boxes
// and custom volumes (none when there are neither), and the custom volumes. `under` is the
// underground complex the site planned with its structure (JS: site.plan.under), if any.
struct SiteStructure {
  std::vector<SiteBox> boxes;
  SpatialGrid<uint32_t> grid{128};
  std::optional<Box3> bb;
  std::vector<SiteVolume> custom;
  std::shared_ptr<const Complex> under;
};

SiteStructure finish_structure(BoxLists lists);

// Rasterizes a structure into a chunk as the sites' feature source does: the custom volumes
// whose bounds meet the chunk, then the boxes over the chunk in their order, leaving out those
// wholly below `deep` (the source's coarse LODs: rooms sealed in the rock below the ground).
void rasterize_structure(const SiteStructure& st, ChunkBuffer& chunk, double deep);

}  // namespace svx::city
