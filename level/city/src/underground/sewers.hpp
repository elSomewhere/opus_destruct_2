// svx_city — the sewers (voxel_city underground/sewers.js): a walkable brick network 5 m under the
// streets of dense districts.
//
//   runs      straight vaulted tunnels under street centre lines, with a central sewage channel
//             and a walkway on each side
//   chambers  every junction, dead end and at most every 80 m; each has a ladder shaft up to a
//             manhole (some left open, fenced by cones)
//   halls     big pillared overflow halls where two collectors cross, with a stair up to a
//             service opening on the sidewalk
//
// Planning is per arterial cell from the roads the cell OWNS (the same ownership rule as street
// dressing), so the network is a pure function of the cell. Every split point of a run is a node
// whose invert height is a pure function of its position, which is what makes runs planned by
// different cells meet exactly. Arterials that carry a subway line have no sewer (the subway
// tunnel owns that corridor).
//
// A cell's plan reads the cell networks of its 3 x 3 neighbourhood (World::cell_net; roads compared
// as same_road, network/road.hpp), the street level (createWorld.js's world.streetLevel, given at
// construction: create_world gives World::street_level), the subway's stations and the rivers -
// pure functions all - and is kept in a cache (core/cache.hpp: any thread, the first to ask makes
// it; the reference keeps 48 in an LRU). The feature source (sewerSource: id "sewers", order 2, up
// to LOD 1) is sewer_z_range and sewer_rasterize, the latter over a view of the ground tile's
// columns (SewerColumns), for voxel/compose to wrap as a FeatureSource. Immutable but for its
// cache: any thread may query.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/cache.hpp"
#include "core/placement.hpp"
#include "core/rect.hpp"
#include "underground/subway.hpp"

namespace svx::city {

class World;
class ChunkBuffer;
struct Road;

// A run: a straight stretch of tunnel under a street's centre line between two nodes - along y
// (axis 0, x = fixed) or along x (axis 1, y = fixed) from l0 to l1 - with the walkway's z at each
// end and its street's class.
struct SewerRun {
  int axis = 0;
  double fixed = 0, l0 = 0, l1 = 0;
  double z0 = 0, z1 = 0;
  std::string cls;
};

// A node: a chamber (or a hall) where runs meet, end or split.
struct SewerNode {
  double x = 0, y = 0;
  double z = 0;   // the walkway (DEPTH below the street)
  double zr = 0;  // the street's z there (rounded)
  // the arms its channel runs along: "N", "S", "E", "W" in the order added (JS: a Set)
  std::string arms;
  bool hall = false;
  bool blocked = false;  // (always false on a plan's nodes: the blocked ones are dropped)
  double qx = 0, qy = 0;  // the quadrant (+-1) its ladder shaft sits in
  bool shaft = false;     // its own ladder shaft (staggered junctions share the smaller key's)
  bool open = false;      // the manhole left open (cones round it)
  double R = 0, H = 0;    // inner half size, ceiling height above the walkway
  std::optional<XYZ> stair_top;  // a hall's stair top on the sidewalk (JS: undefined elsewhere)
  // JS's key: `${x},${y}`
  std::string key() const;
};

// A hall stair's service opening on the sidewalk: the rect street props keep off, and the stair's
// top.
struct SewerOpening {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  XYZ top;
};

// A cell's sewers: its runs, nodes (in the order made), the boxes of a hall's stair (the subway's
// conventions: mode 2 only replaces solid ground), the openings and the bounds of everything
// (none: an empty plan).
struct SewerPlan {
  std::vector<SewerRun> runs;
  std::vector<SewerNode> nodes;
  std::vector<UndergroundBox> boxes;
  std::vector<SewerOpening> openings;
  std::optional<Box3> bb;
};

// mapData: the lines (each run once: [[x, y], [x, y]]) and the halls with their stairs' tops.
struct SewerMap {
  struct Hall {
    double x = 0, y = 0;
    XYZ top;
  };
  std::vector<std::array<std::array<double, 2>, 2>> lines;
  std::vector<Hall> halls;
};

// The columns of a ground tile (voxel/compose's tile) the sewers read: z, the ground height
// (voxels) of each of the kP x kP padded columns (index i + j kP), where a shaft's manhole sits.
struct SewerColumns {
  const int32_t* z = nullptr;
};

// The view of a tile that has the reference's z (a contiguous array).
template <class Tile>
SewerColumns sewer_columns(const Tile& t) {
  return {t.z.data()};
}

class Sewers {
 public:
  // cache_capacity: the cell plans kept (some 50 kB each; the reference keeps 48; results never
  // depend on it).
  Sewers(const World& world, StreetLevel street_level, size_t cache_capacity = 96);
  Sewers(const Sewers&) = delete;
  Sewers& operator=(const Sewers&) = delete;

  const World* world;
  StreetLevel street_level;

  // Does a road of cell (i, j) get a sewer? An arterial, collector or local street, straight and
  // axis-aligned, urban enough, and not an arterial edge carrying a subway line.
  bool eligible(const Road& road, double i, double j) const;
  // The plan of cell (i, j) (cached).
  std::shared_ptr<const SewerPlan> cell_plan(double i, double j) const;
  // The plan of cell (i, j), made (cell_plan caches it).
  std::shared_ptr<const SewerPlan> plan_cell(double i, double j) const;
  // Does a box (world voxels, with z) touch any subway station volume?
  bool hits_subway(const Box3& b) const;
  // Sewer plans of every cell that may reach into a rect (in the cells' order).
  std::vector<std::shared_ptr<const SewerPlan>> near(const Rect& rect) const;
  // Street props keep clear of stair openings and open manholes.
  bool blocks_surface(double x, double y) const;
  SewerMap map_data(const Rect& rect) const;
  // The nearest hall (with its stair top) to a point, for points of interest; null: none within
  // the plans near reach.
  std::shared_ptr<const SewerNode> nearest_hall(double x, double y, double reach = 8000) const;

 private:
  mutable MemoCache<uint64_t, SewerPlan> plans_;  // by cell_key(i, j)
};

// sewerSource
constexpr const char* kSewerSourceId = "sewers";
constexpr double kSewerSourceOrder = 2;
constexpr int kSewerSourceMaxLod = 1;

// sewerSource.zRange: the z range [*z0, *z1] (LOD 0 voxels) of the runs, chambers and stair boxes
// near a rect at a LOD (none beyond LOD 1), or false.
bool sewer_z_range(const World& world, const Rect& rect, int lod, double* z0, double* z1);
// sewerSource.rasterize: the sewers of a chunk (up to LOD 1). The columns' profiles - chamber
// interior > run interior > chamber wall > run wall, so tunnels open cleanly into chambers - then
// the ladder shafts (their manholes on the tile's ground: a null tile puts them at the street's z)
// and the stair boxes.
void sewer_rasterize(const World& world, ChunkBuffer& chunk, const SewerColumns* tile);

}  // namespace svx::city
