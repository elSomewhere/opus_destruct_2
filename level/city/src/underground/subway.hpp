// svx_city — the subway (voxel_city underground/subway.js): lines run under selected arterial
// lines of the global grid, stations sit under arterial intersections. Vertical lines (axis 0) run
// at 14 m depth, horizontal lines (axis 1) at 22 m so crossings never collide; where two lines
// cross, the deep station connects to the shallow station's mezzanine through a long transfer
// stair and passage.
//
// Station anatomy (in a line-local frame: c across, l along the line):
//   platform hall   96 m x 16 m, island platform, two tracks, tiled walls
//   mezzanine       20 m x 10 m hall 7 m below the street
//   stairs          platform -> mezzanine; mezzanine -> two sidewalk entrances
// Everything is boxes (shell, carve, detail) in world coordinates, plus a per-column tunnel
// rasterizer between stations.
//
// A station is a pure function of its node (axis, i, j) and the world - with the street level it
// is built against (createWorld.js's world.streetLevel, given at construction: create_world gives
// World::street_level) - kept in a cache (core/cache.hpp: any thread, the first to ask makes it;
// the reference keeps 64 in an LRU). The feature source (subwaySource: id "subway", order 3, up to
// LOD 2) is subway_z_range and subway_rasterize, for voxel/compose to wrap as a FeatureSource.
// Immutable but for its cache: any thread may query.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "core/cache.hpp"
#include "core/placement.hpp"
#include "core/rect.hpp"
#include "network/roadClasses.hpp"

namespace svx::city {

class World;
class ChunkBuffer;

// The street surface level (voxels) at a point: createWorld.js's world.streetLevel (the nearest
// road's graded level, else the terrain; World::street_level), which the subway and the sewers
// read. (A function the World gives them: the conformance stages give the terrain's height.)
using StreetLevel = std::function<double(double x, double y)>;

// A box of a station or of a sewer hall's stair: an inclusive world-voxel box filled with material
// m (0: carved) in mode `mode` (ChunkBuffer::fill_box: 0 overwrite, 2 only solid ground - the tile
// shells, which so never refill carved space). JS: {x0, x1, y0, y1, z0, z1, m, mode}.
struct UndergroundBox {
  double x0 = 0, x1 = 0, y0 = 0, y1 = 0, z0 = 0, z1 = 0;
  uint16_t m = 0;
  int mode = 0;
};

// A station: its node (line `i` of axis `axis` at the cross line `j`; x, y its world position),
// the platform's surface z and the street's z at the node (voxels), its boxes (in the order made:
// later ones write over earlier ones) and their bounds.
struct Station {
  int axis = 0;
  double i = 0, j = 0;
  double x = 0, y = 0;
  double zp = 0, zs = 0;
  std::vector<UndergroundBox> boxes;
  Box3 bb{};  // (JS's bb: never null - every station has its hall)
};

// An active tunnel span (tunnelsNear): its line (fixed: x for axis 0, y for axis 1), its extent
// along it (l0 .. l1: from a hall's end, or a node without a station), whether a station ends it
// at l0 / l1 (the tracks spread to the platform there) and its track bed z at l0 / l1.
struct Tunnel {
  int axis = 0;
  double fixed = 0, l0 = 0, l1 = 0;
  bool s0 = false, s1 = false;
  double z0 = 0, z1 = 0;
};

// mapData: the lines (a tunnel span's two ends, met by the station halls) and the stations.
struct SubwayMap {
  struct Line {
    std::array<std::array<double, 2>, 2> pts;
  };
  struct Stop {
    double x = 0, y = 0;
    int axis = 0;
  };
  std::vector<Line> lines;
  std::vector<Stop> stations;
};

class Subway {
 public:
  // cache_capacity: the stations kept per axis (some 80 kB each, a node without one a few bytes;
  // the reference keeps 64 in all; results never depend on it).
  Subway(const World& world, StreetLevel street_level, size_t cache_capacity = 64);
  Subway(const Subway&) = delete;
  Subway& operator=(const Subway&) = delete;

  const World* world;
  StreetLevel street_level;
  // config.subway (read once): lineEvery, minUrbanization
  double line_every = 0, min_urbanization = 0;
  // roadSpecs(config).arterial (the street a station's entrances open beside)
  RoadSpec art;

  // Does line i of an axis carry a subway line?
  bool line_exists(int axis, double i) const;
  // Is the span of line (axis, i) between cross lines j and j + 1 urban enough for a line?
  bool span_active(int axis, double i, double j) const;
  // Is there a station on line (axis, i) at cross line j (an active span on either side, no river)?
  bool has_station(int axis, double i, double j) const;
  // No station where a river crosses its footprint (the mezzanine and the street entrances sit
  // above the river bed); trains run straight through.
  bool river_blocked(int axis, double i, double j) const;
  // The node of line (axis, i) at cross line j.
  Point2 node_xy(int axis, double i, double j) const;
  // The street's z (voxels, rounded) at a point.
  double street_z(double x, double y) const;
  // Platform surface z of the station of line (axis, i) at node j.
  double platform_z(int axis, double i, double j) const;
  // The station of line (axis, i) at node j, or null (cached).
  std::shared_ptr<const Station> station(int axis, double i, double j) const;
  // The station, made (station() caches it).
  std::shared_ptr<const Station> build_station(int axis, double i, double j) const;
  // Stations whose bounds overlap a rect (voxels): axis 0's lines, then axis 1's, each by line
  // and node.
  std::vector<std::shared_ptr<const Station>> stations_near(const Rect& rect) const;
  // Active tunnel spans near a rect.
  std::vector<Tunnel> tunnels_near(const Rect& rect) const;
  // Is (x, y) above an entrance / station opening (street props should avoid it)?
  bool blocks_surface(double x, double y) const;
  SubwayMap map_data(const Rect& rect) const;

 private:
  mutable MemoCache<uint64_t, Station> stations_v_;  // axis 0, by cell_key(i, j)
  mutable MemoCache<uint64_t, Station> stations_h_;  // axis 1
};

// subwaySource
constexpr const char* kSubwaySourceId = "subway";
constexpr double kSubwaySourceOrder = 3;
constexpr int kSubwaySourceMaxLod = 2;

// subwaySource.zRange: the z range [*z0, *z1] (LOD 0 voxels) of the stations and tunnels near a
// rect, or false. (A World without a subway: false - JS installs the source only with one.)
bool subway_z_range(const World& world, const Rect& rect, int lod, double* z0, double* z1);
// subwaySource.rasterize: the stations' boxes and the running tunnels of a chunk.
void subway_rasterize(const World& world, ChunkBuffer& chunk);

}  // namespace svx::city
