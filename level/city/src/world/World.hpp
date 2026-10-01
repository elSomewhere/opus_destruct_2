// svx_city — the World: the single entry point to generation (voxel_city world/World.js and
// world/createWorld.js).
//
// Everything is lazy and cached by structural key, and every product is a pure function of
// (config, key): caches can be dropped at any time and any thread can rebuild any piece
// independently.
//
//   fields / terrain         pointwise, no cache
//   arterial grid            pure functions of a line's index
//   cell network (stage 1)   roads, districts and blocks per arterial cell
//   road view                the junction-annotated roads of a 3 x 3 neighbourhood
//   cell plan (stage 2)      lots, building envelopes, open spaces
//   building plan            a full interior, made on its first LOD 0 request
//
// Ownership in C++: products come out of the caches as shared_ptr<const T>. Anything that keeps
// a pointer into a product beyond the call that fetched it keeps the product alive (an aliasing
// shared_ptr), since the cache may drop it. Each World method is defined by the module that
// implements it (World::cell_plan in city/cellPlan.cpp, World::road_view in
// network/roadView.cpp, ...), so a partial port links what it has.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/cache.hpp"
#include "core/rect.hpp"
#include "core/value.hpp"

namespace svx::city {

class World;
class ChunkBuffer;
struct GroundTile;

// A feature source (compose.js): rasterized into chunks after the ground pass, in `order`.
// z_range (optional) is the LOD 0 z range its content may take in a column tile's padded rect,
// so the streamer knows which chunks of a column hold content; max_lod (-1: every LOD).
struct FeatureSource {
  std::string id;
  double order = 0;
  int max_lod = -1;
  // [z0, z1] or nothing
  std::function<bool(const World&, const Rect& rect, int lod, const GroundTile& tile, double* z0, double* z1)> z_range;
  std::function<void(const World&, ChunkBuffer& chunk, const GroundTile& tile)> rasterize;
};

// Subsystems (each its module's).
class Chart;
class MacroFields;
class Terrain;
class ArterialGrid;
class LandCover;
class Rivers;
class Lakes;
class Caves;
class Landmarks;
class Forest;
class Boulders;
class HighwayNetwork;
class Subway;
class Sewers;
class SiteLayer;
class SiteLinks;
struct CellNet;
struct CellPlan;
class RoadView;
struct Road;  // network/road.hpp
struct BuildingPlan;
struct Dressing;
struct Envelope;

// A cell of the arterial lattice.
struct CellIJ {
  double i = 0, j = 0;
};

// A shore a harbour can face (world.shoreNear): its level (voxels), the distance to the water's
// edge (voxels, negative in the water), the unit vector pointing inland, and its lake (or none:
// the island's sea).
struct Shore {
  double level = 0, dist = 0, nx = 0, ny = 0;
  const void* lake = nullptr;  // (a Lake of nature/lakes.hpp, or null for the sea)
};

class World {
 public:
  // World.js: the configuration merged (makeConfig), the chart, the macro fields, the terrain and
  // the arterial grid. create_world (createWorld.js) installs the rest.
  explicit World(const Value& config_overrides);
  ~World();
  World(const World&) = delete;
  World& operator=(const World&) = delete;

  Value config;
  double seed = 0;
  std::shared_ptr<Chart> chart;
  std::shared_ptr<MacroFields> fields;
  std::shared_ptr<Terrain> terrain;
  std::shared_ptr<ArterialGrid> arterials;

  // createWorld.js
  std::shared_ptr<LandCover> land_cover;
  std::shared_ptr<Rivers> rivers;
  std::shared_ptr<Lakes> lakes;
  std::shared_ptr<Caves> caves;
  std::shared_ptr<Landmarks> landmarks;  // (islands only)
  std::shared_ptr<Forest> forest;
  std::shared_ptr<Boulders> boulders;
  std::shared_ptr<HighwayNetwork> highways;  // (null: highways.enabled false)
  std::shared_ptr<Subway> subway;            // (null: subway.enabled false)
  std::shared_ptr<Sewers> sewers;
  std::shared_ptr<SiteLayer> sites;
  std::shared_ptr<SiteLinks> site_links;

  // Feature sources rasterized into chunks after the ground pass, in order (stable).
  std::vector<std::shared_ptr<const FeatureSource>> feature_sources;
  void add_feature_source(std::shared_ptr<const FeatureSource> src);

  // ---- World.js
  std::shared_ptr<const CellNet> cell_net(double i, double j) const;     // city/cellNetwork.cpp
  std::shared_ptr<const CellPlan> cell_plan(double i, double j) const;   // city/cellPlan.cpp
  CellIJ cell_at(double x, double y) const;                              // network/arterials.cpp
  std::vector<CellIJ> cells_overlapping(const Rect& r) const;            // network/arterials.cpp
  std::shared_ptr<const RoadView> road_view(double i, double j) const;   // network/roadView.cpp
  // The envelope of an id ("C{i}_{j}/b../l../B"), or null.
  std::shared_ptr<const Envelope> envelope(const std::string& id) const; // city/cellPlan.cpp
  // Building envelopes overlapping a world rect (in the cells' order, each cell's own order).
  std::vector<std::shared_ptr<const Envelope>> envelopes_in(const Rect& r) const;  // city/cellPlan.cpp

  // ---- createWorld.js (world/createWorld.cpp unless noted)
  bool sea_at(double x, double y, double margin_m = 0) const;
  bool is_wet(double x, double y, double margin_m = 2) const;
  bool sea_hits_rect(const Rect& r, double margin_m = 6) const;
  double sea_share(const Rect& r) const;
  bool sea_hits_seg(double ax, double ay, double bx, double by, double margin_m = 4) const;
  bool open_water_at(double x, double y) const;
  std::optional<Shore> shore_near(double x, double y, double max_dist) const;
  double street_level(double x, double y) const;                          // (needs roads: network/roadLevel.cpp)
  bool water_hits_rect(const Rect& r, double margin_m = 6) const;
  std::shared_ptr<const Dressing> dressing(double i, double j) const;     // city/dressing.cpp
  std::shared_ptr<const BuildingPlan> building_plan(const Envelope& env) const;  // buildings/interior/plan.cpp
  void voxelize_building(const Envelope& env, ChunkBuffer& chunk) const;  // buildings/interior/voxelize.cpp
  bool blocks_surface(double x, double y) const;

  // ---- where the road network's inputs come from (network/roadView.cpp)
  // The roads of cell (i, j)'s network (World.js: cellNet(i, j).roads), which road_view gathers
  // for the 3 x 3 cells round a cell: the cell networks install it (city/cellNetwork.cpp:
  // cell_net(i, j)->roads); a test may serve given roads instead (tools/procgen_ref/data).
  std::function<std::vector<std::shared_ptr<const Road>>(double i, double j)> cell_roads;
  // Water answered from elsewhere (a test serving the reference's answers): is_wet asks it first
  // when it is set.
  std::function<bool(double x, double y, double margin_m)> wet_source;

  // ---- caches (World.js's LRUs; createWorld's dressing cache; keys as their modules make them)
  struct Caches;
  Caches& caches() const { return *caches_; }

 private:
  std::unique_ptr<Caches> caches_;
};

// createWorld(config): a World with the default feature sources installed.
std::shared_ptr<World> create_world(const Value& config);

}  // namespace svx::city
