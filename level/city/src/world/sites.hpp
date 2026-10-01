// svx_city — sites (voxel_city world/sites.js): rare, self-contained special places (military
// bases, research facilities, mountain strongholds; later villages, ruins, airports ...) anchored
// on a coarse lattice. A site owns a rect inside one arterial cell (clear of the cell's boundary
// roads), levels the ground to its pads, and provides through its kind (SITES):
//
//   place(world, cand)       optional custom placement: null to reject, or { padZ, pads,
//                            footprint } (default: one level pad over the rect, rejected on
//                            steep ground)
//   plan(world, site)        the site's plan, made with the site (its bounds, if any, are what
//                            sitesNear tests)
//   structure(world, site)   its geometry as boxes and custom volumes (sites/kit), made once
//   ground(site, x, y, out)  the surface material inside its pads (optional)
//   port(world, site, to)    where a link tunnel may start (optional; sites/links)
//
// (The kinds' surface(world, site), the envelopes a cell plan merges, and pois come with the
// kinds and the cell plan.)
//
// Sites are products of the layer's cache (a pure function of the world and the lattice cell):
// any thread may ask for any site; a site's structure is made once, by the first to ask. A
// path pad's distance query leaves nothing on the pad (JS keeps p._z, p._s, p._c there): ground()
// returns them.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/cache.hpp"
#include "core/rect.hpp"
#include "sites/kit.hpp"
#include "world/World.hpp"
#include "world/registry.hpp"
#include "world/wrap.hpp"

namespace svx::city {

struct Site;

// A ramp: a pad's level rising linearly along `axis` ('x' or 'y') from z0 at a0 to z1 at a1.
struct SiteRamp {
  char axis = 'x';
  double a0 = 0, a1 = 0, z0 = 0, z1 = 0;
};

// A point of a path pad: x, y, z (voxels).
struct SitePathPoint {
  double x = 0, y = 0, z = 0;
};

// A pad a site levels the ground to: a flat rect (z), a ramped one, or a ribbon of half width
// `half` along a polyline (path: a road on a slope; its rect bounds it), blended into the natural
// ground over `margin` voxels; round: the blend ends `margin` from the pad (not at the margin
// rect's corners); road: a road's pad (the kinds' ground reads it).
struct SitePad {
  Rect rect{};
  double z = 0;
  double margin = 0;
  bool round = false;
  std::optional<SiteRamp> ramp;
  std::vector<SitePathPoint> path;
  double half = 0;
  bool road = false;
};

// A path pad's nearest point to (x, y): d the distance beyond the ribbon's edge (pathDistance),
// z the level there, s the arc length, c the distance to the centre line (JS: p._z, p._s, p._c).
struct PathHit {
  double d = 0, z = 0, s = 0, c = 0;
};
PathHit path_distance(const SitePad& p, double x, double y);

// padDistance: distance (voxels) from (x, y) to a pad's footprint (0 inside).
double pad_distance(const SitePad& p, double x, double y);

// def.place's candidate: the site's rect, its arterial cell's rect, its margin and a seed.
struct SiteCandidate {
  Rect rect{};
  Rect cell_rect{};
  double margin = 0;
  double seed = 0;
};

// def.place's product: the pad level, the pads and the footprint (a kind derives its own record
// from it to keep what else it places).
struct SitePlaced {
  double pad_z = 0;
  std::vector<SitePad> pads;
  Rect footprint{};
  virtual ~SitePlaced() = default;
};

// def.plan's product (a kind derives its own): the bounds the site covers (none: its blend rect).
struct SitePlan {
  std::optional<Rect> bounds;
  virtual ~SitePlan() = default;
};

// A link tunnel's port (def.port): a point and floor level deep inside the site's complex (d: the
// distance a kind measured to pick it; NaN where it gives none).
struct SitePort {
  double x = 0, y = 0, z = 0;
  double d = js::kNaN;
};

// ground()'s record (JS's out): the levelled ground's z, its surface and subsurface materials (0:
// the natural ones), the site and pad, the natural z, whether (x, y) lies on the pad itself, and,
// on a path pad, its level, arc length and centre-line distance there (NaN elsewhere).
struct SiteGround {
  double z = 0;
  uint16_t mat = 0, sub = 0;
  std::shared_ptr<const Site> site;
  const SitePad* pad = nullptr;
  double natural = 0;
  bool inside = false;
  double path_z = js::kNaN, path_s = js::kNaN, path_c = js::kNaN;
};

// A site kind (SITES): its placement rules (frequency: its share of the lattice cells, in
// registration order; the urbanization window of its arterial cell; its size in metres; its
// margin and the relief its default pad allows, metres: ?? 60, ?? 25) and its hooks (header).
struct SiteDef {
  std::string id, label;
  double frequency = 0;
  double min_u = 0, max_u = 0;
  std::array<double, 2> size{};
  std::optional<double> margin, max_relief;
  std::function<std::shared_ptr<const SitePlaced>(const World& w, const SiteCandidate& cand)> place;
  std::function<std::shared_ptr<const SitePlan>(const World& w, const Site& site)> plan;
  std::function<SiteStructure(const World& w, const Site& site)> structure;
  std::function<void(const Site& site, double x, double y, SiteGround& out)> ground;
  std::function<std::optional<SitePort>(const World& w, const Site& site, const Point2* toward)> port;
};

// SITES: read-only once register_all() has run; _mut for registration only (the site kinds).
const Registry<SiteDef>& site_registry();
Registry<SiteDef>& site_registry_mut();

// A site: its id ("site:<kind>:<ca>_<cb>", the canonical lattice cell), kind, lattice cell (a, b),
// arterial cell and its rect, its rect (inclusive), blend rect (the rect and its margin), margin,
// pad level, placement (pads: placed->pads), seed, centre and plan.
struct Site {
  std::string id;
  std::string type;
  const SiteDef* def = nullptr;
  double a = 0, b = 0;
  CellIJ cell{};
  Rect cell_rect{}, rect{}, blend{};
  double margin = 0, pad_z = 0;
  std::shared_ptr<const SitePlaced> placed;
  double seed = 0;
  Point2 center{};
  std::shared_ptr<const SitePlan> plan;

  const std::vector<SitePad>& pads() const { return placed->pads; }
  // def.structure's product, made once (JS: site.plan.structure ??=).
  const SiteStructure& structure(const World& w) const;

 private:
  Lazy<SiteStructure> structure_{};
};

class SiteLayer {
 public:
  // The kinds of SITES.
  explicit SiteLayer(const World& world);
  // Other kinds, in this order (tests); they must outlive the layer.
  SiteLayer(const World& world, std::vector<const SiteDef*> defs);
  SiteLayer(const SiteLayer&) = delete;
  SiteLayer& operator=(const SiteLayer&) = delete;

  double cell = 0;  // the lattice spacing (voxels; config.world.siteCell)
  Wrap wrap;
  double n = 0;     // site cells round a wrapping world (0: unbounded)

  // The site anchored at lattice cell (a, b), or null.
  std::shared_ptr<const Site> site_at(double a, double b) const;
  // Sites whose bounds (plan.bounds ?? blend) overlap a world rect, row by row.
  std::vector<std::shared_ptr<const Site>> sites_near(const Rect& rect) const;
  // Sites anchored in arterial cell (i, j).
  std::vector<std::shared_ptr<const Site>> sites_in_cell(double i, double j) const;
  // The ground override at (x, y): a levelled pad and its blend ramp (the nearest pad wins where
  // blend zones overlap), or nothing outside every site.
  std::optional<SiteGround> ground(double x, double y, double natural_z) const;
  struct MapItem {
    std::string id, type;
    Rect rect{};
    Point2 center{};
  };
  std::vector<MapItem> map_data(const Rect& rect) const;
  // The nearest sites to a point (the viewer's points of interest): n at most, within
  // radius_cells lattice cells.
  struct Nearest {
    std::string id, type;
    double x = 0, y = 0, d = 0, z = 0;
  };
  std::vector<Nearest> nearest(double x, double y, double n = 4, double radius_cells = 3) const;

 private:
  std::shared_ptr<const Site> build(double a, double b) const;
  const World& world_;
  std::vector<const SiteDef*> defs_;
  double face_margin_ = 0, sea_level_ = 0;
  mutable MemoCache<uint64_t, Site> cache_{256};
};

// siteSource (order 4, LODs up to 7): every site's structure through its spatial grid.
// z range: of the boxes and custom volumes over a rect (only the tiles over a complex's deep
// parts build its chunks); false: none.
bool site_source_z_range(const World& w, const Rect& rect, double* z0, double* z1);
// Rasterizes the sites near a chunk; tile_z_min: its column tile's lowest ground (at coarse
// LODs, from 3, the rooms and tunnels sealed in the rock below it are left out).
void site_source_rasterize(const World& w, ChunkBuffer& chunk, double tile_z_min);
constexpr double kSiteSourceOrder = 4;
constexpr int kSiteSourceMaxLod = 7;

}  // namespace svx::city
