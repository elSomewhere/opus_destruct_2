// svx_city — the site kit (voxel_city sites/kit.js): boxes and structures, and the surface
// structures the site kinds share (military bases, research complexes, mountain strongholds).
//
// Sites (world/sites) and their underground complexes (sites/complex) build their geometry as
// lists of boxes in world voxels: shells (written over solid only: walls in the rock), carves
// (material 0: the voids) and details (floors, props, stairs), each box {x0..z1, m, mode} with
// mode 0 overwrite, 1 only air, 2 only solid (voxel/chunk fill_box). A site's structure is those
// boxes in that order behind a spatial grid, with custom volumes (a rock cavern) rasterized
// before them.
//
// The surface structures write boxes into a list (details unless noted): a perimeter fence with
// a gate, a guard booth and barrier, watchtowers, radar masts, radomes, dish arrays, fuel tanks,
// trucks, and portal blocks (a concrete bunker over a stair shaft: shells, carves and details).
// plan_gate picks the side of a site's gate: the one facing the nearest road of its arterial
// cell.
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/geom2d.hpp"
#include "core/hash.hpp"
#include "core/placement.hpp"
#include "core/rect.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

class ChunkBuffer;
class World;
struct Complex;
struct Site;

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

// A building lot a site kind lays out in its plan (planned into an envelope by its surface hook):
// the archetype, the lot's rect, the side its front faces and its style ("": the kind's own).
struct SiteLot {
  std::string kind;
  Rect rect{};
  char front = 'N';
  std::string style;
};

// A painted helipad: its centre and radius.
struct SiteHelipad {
  double x = 0, y = 0, r = 0;
};

// The ground of a helipad at (x, y) inside its radius: the ring and the H in white, the rest dark
// concrete (the kinds' ground hooks share it).
inline uint16_t helipad_mat(const SiteHelipad& h, double x, double y, double dh) {
  const double hx = std::fabs(x - h.x);
  const double hy = std::fabs(y - h.y);
  const bool is_h = dh > h.r - 3 || (hy < 28 && (std::fabs(hx - 20) < 3 || (hx < 20 && hy < 3)));  // vx(3.5), vx(2.5)
  return is_h ? MAT::LINE_WHITE : MAT::CONCRETE_DARK;
}

// planGate's product: the side of the site's rect the gate is on ('N', 'S', 'W', 'E'), the gap
// in the fence (a line along that side: y0 == y1 on N and S, x0 == x1 on W and E) and the
// driveway from the arterial cell's edge through it.
struct SiteGate {
  char gate_side = 'S';
  Rect gate{};
  Rect drive{};
};

// planGate(world, site, driveHalf): the gate faces the nearest road of the site's arterial cell
// (the side whose edge has a road class and the least room to the cell's edge; S without one),
// 8 m wide in the middle of that side, the driveway (driveHalf either side of its centre line)
// from the cell's edge to 12 m inside the fence.
SiteGate plan_gate(const World& world, const Site& site, double drive_half = 28);

// The sides a fence leaves open (JS: skip, { N: true }).
struct FenceSkip {
  bool N = false, S = false, W = false, E = false;
};

// Perimeter fence (chain link, a barbed top, posts every 24 voxels) h voxels high round r on the
// ground z, with a gap at the gate on gate_side; the skipped sides stay open.
void fence(std::vector<SiteBox>& out, const Rect& r, double z, char gate_side, const Rect& gate, double h = 22, const FenceSkip& skip = {});
// A guard booth and a barrier arm just inside the gate.
void gate_booth(std::vector<SiteBox>& out, const Rect& gate, char gate_side, double z);
// Watchtowers at the four corners of a rect.
void watchtowers(std::vector<SiteBox>& out, const Rect& r, double z);
// A radar mast at (x, y).
void radar_mast(std::vector<SiteBox>& out, double x, double y, double z);
// A white radome (a sphere of radius R on a drum).
void radome(std::vector<SiteBox>& out, double x, double y, double z, double R = 48);
// A row of n tilted satellite dishes on pedestals, gap voxels apart, from (x0, y).
void dish_array(std::vector<SiteBox>& out, double x0, double y, double z, double n = 3, double gap = 64);
// n fuel tanks in a row from (x0, y).
void fuel_tanks(std::vector<SiteBox>& out, double x0, double y, double z, double n = 3);
// A truck at (x, y), its colour drawn from rng.
void truck(std::vector<SiteBox>& out, double x, double y, double z, Rng& rng);
// A portal block: a thick concrete bunker over a stair shaft (shells), its hollow (carves), a
// stepped roof, a blast door opening on its south side and a sign (details); its floor is laid
// in the shells pass, so the shaft can open through it.
void portal_block(BoxLists& lists, const Rect& bk, double z, uint16_t accent = MAT::HAZARD_YELLOW);

}  // namespace svx::city
