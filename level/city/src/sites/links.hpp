// svx_city — site links (voxel_city sites/links.js): deep service-road tunnels joining
// neighbouring sites (research complexes, mountain strongholds, military bases) into one
// underground network, Black Mesa style.
//
// Every site kind that can be linked offers a port (SiteDef::port): a point and floor level deep
// inside its complex (a tram station, the deepest level) where a link tunnel may start. Two sites
// on neighbouring lattice cells are linked (by chance) with an L-shaped route between their
// ports. The floor profile runs straight from port to port but never comes closer than COVER to
// the surface (a grade-limited envelope under the terrain), so a link dives under valleys and
// stays in the rock. Links whose ports are too far apart, or too different in height for the
// grade, are skipped.
//
// The tunnel rasterizes after the sites (order 4.5): its bore clears a passage through anything it
// crosses, but its lining only fills solid rock, so a link opens straight into the halls and rooms
// on its way. Links are products of the layer's cache (pure functions of the world and the
// lattice cell): any thread may ask for any.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/cache.hpp"
#include "core/rect.hpp"
#include "world/World.hpp"
#include "world/sites.hpp"

namespace svx::city {

// A straight leg of a link: from p to q along x or y, starting at arc length s0, len long,
// dir_sign its direction on its axis; rect its bounds padded by the tunnel's half width.
struct LinkSeg {
  Point2 p{}, q{};
  bool along_x = true;
  double s0 = 0, len = 0, dir_sign = 0;
  Rect rect{};
};

// A link: its id ("link:<a's id>><b's id>"), its two sites, their ports, its legs, length, floor
// profile (an Int32Array: one level every STEP of arc length), its lowest and highest profile
// levels before the ports were set, and its bounds.
struct SiteLink {
  std::string id;
  std::shared_ptr<const Site> a, b;
  SitePort A{}, B{};
  std::vector<LinkSeg> segs;
  double len = 0;
  std::vector<int32_t> prof;
  double zlo = 0, zhi = 0;
  Rect bb{};
};

class SiteLinks {
 public:
  // (world.sites must be set: the links ask it for the sites)
  explicit SiteLinks(const World& world);
  SiteLinks(const SiteLinks&) = delete;
  SiteLinks& operator=(const SiteLinks&) = delete;

  // The link between lattice cells (a, b) and its neighbour in direction dir (0: +a, 1: +b), or
  // null.
  std::shared_ptr<const SiteLink> link(double a, double b, double dir) const;
  // Links whose route passes near a rect (their bounds overlap it).
  std::vector<std::shared_ptr<const SiteLink>> near(const Rect& rect) const;
  // Floor z of a link at arc length s.
  static double z_at(const SiteLink& L, double s);
  // The viewer's map: each link near a rect as its id and the corners of its route.
  struct MapItem {
    std::string id;
    std::vector<Point2> pts;
  };
  std::vector<MapItem> map_data(const Rect& rect) const;

 private:
  std::shared_ptr<const SiteLink> build(double a, double b, double dir) const;
  struct Key {
    double a, b, dir;
    bool operator==(const Key& o) const { return a == o.a && b == o.b && dir == o.dir; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const;
  };
  const World& world_;
  mutable MemoCache<Key, SiteLink, KeyHash> cache_{256};
};

// siteLinkSource (order 4.5, LODs up to 2): the link tunnels.
// z range: the floor levels (less the lining, plus the clear height) over a rect; false: none.
bool site_links_z_range(const World& w, const Rect& rect, double* z0, double* z1);
// Rasterizes the links near a chunk: the bore clears, the lining fills only solid rock.
void site_links_rasterize(const World& w, ChunkBuffer& chunk);
// The feature source (createWorld adds it).
std::shared_ptr<const FeatureSource> site_link_source();

}  // namespace svx::city
