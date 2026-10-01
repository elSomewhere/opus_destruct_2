// svx_city — the landmarks of the open country on an island (voxel_city world/landmarks.js,
// world/island): a lighthouse on the headland nearest the harbour, boathouses (naust) in little
// rows along the shore near every place, fish-drying racks by the fishing hamlets, cairns (varder)
// on the fell tops. The island plans them once, as props (city/propPrefabs) standing on the natural
// ground; the `landmarks` feature source rasterizes them and trees keep clear of them (blocks).
//
// create_world installs one on an island World (World::landmarks). Planned lazily, once, whichever
// thread asks first; immutable after.
//
// The open-ground test (free: no road, lot, urban space or water) reads the road views and the cell
// plans (plan.lotAt, plan.spaceAt). The cell plan is a later stage of the port: until it is, its half
// of the test is Landmarks::plan_occupied (city/cellPlan installs it; a test serves its own), and a
// test may serve the whole test's answers (free_source). Both must be set before the first plan.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/cache.hpp"
#include "core/geom2d.hpp"
#include "core/placement.hpp"
#include "core/rect.hpp"
#include "voxel/chunk.hpp"

namespace svx::city {

class World;

// A landmark (makeProp's record): a prop placed in the world - its boxes (world voxels, inclusive;
// m 0: none), their bounds and its footprint.
struct LandmarkBox {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0, z0 = 0, z1 = 0;
  uint16_t m = 0;
};
struct Landmark {
  std::string kind;  // lighthouse, boathouse, fishRack, cairn
  std::vector<LandmarkBox> boxes;
  Box3 bb;
  Rect foot;
};

class Landmarks {
 public:
  explicit Landmarks(const World& world);
  Landmarks(const Landmarks&) = delete;
  Landmarks& operator=(const Landmarks&) = delete;

  // All landmarks (planned lazily, once), in their planning order.
  const std::vector<Landmark>& all() const;
  // near(rect): the landmarks whose bounds overlap a rect (the grid's order).
  std::vector<const Landmark*> near(const Rect& rect) const;
  // blocks(x, y, margin): is (x, y) under a landmark's footprint (plus a margin, voxels)?
  bool blocks(double x, double y, double margin = 16) const;
  // free(x, y): open natural ground at (x, y): no road, lot, urban space or water.
  bool free(double x, double y) const;

  // The cell plan's half of free(x, y): is (x, y) on a lot or an urban space of its cell's plan
  // (world.cellPlan(c.i, c.j): plan.lotAt(x, y) || plan.spaceAt(x, y))? Unset: free() fails.
  std::function<bool(double x, double y)> plan_occupied;
  // free(x, y) served from elsewhere (a test replaying the reference's answers); unset: the World's.
  std::function<bool(double x, double y)> free_source;

 private:
  struct Plan {
    std::vector<Landmark> items;
    SpatialGrid<uint32_t> grid{512};
  };
  const World& world_;
  Lazy<Plan> plan_;
  const Plan& plan() const;
  Plan make_plan() const;
};

// ---- landmarkSource (with landmarks: an island World)

constexpr const char* kLandmarkSourceId = "landmarks";
constexpr double kLandmarkSourceOrder = 8.5;
constexpr int kLandmarkSourceMaxLod = 6;

// zRange: the LOD 0 z range [*z0, *z1] of the landmarks near a rect, or false (none, or a World
// without landmarks).
bool landmark_z_range(const World& world, const Rect& rect, double* z0, double* z1);
// rasterize: the landmarks' boxes into a chunk (small things drop out at LOD 3 and above, the
// lighthouse and the boathouses stay).
void rasterize_landmarks(const World& world, ChunkBuffer& chunk);

}  // namespace svx::city
