// svx_city — the mountain stronghold (voxel_city sites/mountainBase.js): a mountain hollowed out
// into a base.
//
// On a mountain flank a concrete portal with blast doors stands on an apron cut into the slope
// (fenced, with a helipad), a service road winding down the flank from its gate. A vehicle tunnel
// runs level into the mountain to a vast vaulted cavern (shotcrete walls, steel arch ribs,
// hanging floodlights: a custom volume) holding a small town of real buildings (office blocks and
// a hangar with full interiors: the kind's surface hook, on the cavern floor), trucks, fuel tanks
// and a portal block over the stairs down to a two-sector complex (military and power, or lab and
// containment themes) whose sectors a tram joins.
//
// Placement is the kind's own (place): the site needs high mountain ground, a flank that drops
// off within about 600 m in one of the four axis directions with enough rock over the cavern's
// vault and along the tunnel, and a service road that makes it down the flank (up to ~5 km: its
// terrain samples reach that far). A site of this kind is a Site whose placement is a
// MountainPlaced and whose plan is a MountainBasePlan; register_mountain_base() registers its
// district ("stronghold") and its kind ("mountainBase") in that order (register_all calls it
// after the research complex's).
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "core/noise.hpp"
#include "core/rect.hpp"
#include "sites/kit.hpp"
#include "world/sites.hpp"

namespace svx::city {

// A stronghold's local frame: its centre and portal side (u: the unit vector from the cavern's
// centre toward the portal, v: the lateral one; DIRS).
struct MountainFrame {
  double cx = 0, cy = 0;
  char side = 'E';
  std::array<double, 2> u{}, v{};
  MountainFrame() = default;
  MountainFrame(double cx, double cy, char side);
  // (u, v) to the world, rounded.
  Point2 to_world(double u, double v) const;
  // The world rect spanning two frame points.
  Rect rect(double u0, double v0, double u1, double v1) const;
  // The world side a lot faces when its front looks toward -v / +v (sign -1 / +1), or toward +u.
  char facing(double sign) const;
  char facing_u(double sign) const;
};

// Where the service road leaves the apron (pickGate): the frame point (u, v), the world side of
// the apron that edge is on, the road's first heading (out: the outer edge's u, or +-v), the
// world point and how far the ground 10 m beyond it is from the apron's level.
struct MountainGate {
  double u = 0, v = 0;
  char side = 'N';
  std::array<double, 2> out{};
  double x = 0, y = 0, err = 0;
};

// place's product: the pads (the apron, then the road), the road's pad, the gate, the portal
// side, the tunnel's length to the portal (dist), the cavern's half length L, half width Wd and
// height H, its half extents along x and y (A, B), the apron, the tunnel and the cavern's rect.
struct MountainPlaced : SitePlaced {
  std::vector<SitePad> road;
  MountainGate gate{};
  char side = 'N';
  double dist = 0, L = 0, Wd = 0, H = 0, A = 0, B = 0;
  Rect apron{}, tunnel{}, cav_rect{};
};

// The cavern: its centre, half extents along x and y, vault height, wall height (Hw), floor level
// and the noise roughening its walls and vault.
struct Cavern {
  double cx = 0, cy = 0, A = 0, B = 0, H = 0, Hw = 0, zf = 0;
  std::shared_ptr<const SimplexNoise> noise;
};

// cavernR: the normalized radius of a column (< 1 inside the cavern), with rough walls.
double cavern_r(const Cavern& cav, double x, double y);
// cavernCeil: the ceiling (the voxel z of the last air voxel) of a column at normalized radius r.
double cavern_ceil(const Cavern& cav, double x, double y, double r);

// The stronghold's plan: its flavor ("stronghold" or "research": the complex's themes, the
// accent colour), frame, portal side, the apron's gate side and gap, the cavern, the cavern's
// main road, the building lots on the cavern floor (each with its style), the yard and its
// portal block (portals: that one), the helipad, the apron, the tunnel, the tunnel's length,
// the sectors' themes and levels; clear: the apron and its blend margin; bounds: the footprint
// with the road and the apron's margin.
struct MountainBasePlan : SitePlan {
  std::string flavor;
  MountainFrame frame{};
  char side = 'N';
  char gate_side = 'N';
  Rect gate{};
  std::shared_ptr<const Cavern> cavern;
  Rect road{};
  std::vector<SiteLot> lots;
  Rect yard{}, portal{};
  std::vector<Rect> portals;
  SiteHelipad helipad{};
  Rect apron{}, tunnel{};
  double dist = 0;
  std::vector<std::string> themes;
  uint16_t accent = 0;
  std::array<double, 2> levels{};
};

// sites/mountainBase.js's registrations: DISTRICTS "stronghold", SITES "mountainBase".
void register_mountain_base();

}  // namespace svx::city
