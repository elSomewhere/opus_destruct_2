// svx_city — the terrain height model (voxel_city terrain/terrain.js).
//
//   natural(x, y)  the registered landforms in order (terrain/landforms): continent, hills,
//                  mountain ranges up to ~6 km, desert mesas and dunes, canyons, forest ravines ...
//   city           settlements sit on graded land at the lowland height of their centre (weighted
//                  where towns meet) plus gentle relief
//   sample(x, y)   the blend of the two by urbanization, and the landforms' hints
//
// Heights come out in VOXELS (floating point); z = 0 is sea level.
//
// Every call runs the landform stack in a context of its own (TerrainCtx: the position, the
// lazy climate, desertness and coast type, the hints the landforms leave), so a sample is a pure
// function of (world, x, y, arguments): the same in any call order, with any cache sizes and on
// any thread. The reference reuses one context object per Terrain (`this.ctx`), and sample()
// reads its coast and ruggedness back after nested calls that run the stack again in it - a
// settlement's base height made on first use, the world's portGrade hook (lakes sampling the
// terrain) - so there the first sample that makes one of those takes values from elsewhere. The
// port isolates the nested calls (docs/CITY.md §6); the conformance stages make the base heights
// first, so both agree.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/noise.hpp"
#include "core/value.hpp"
#include "terrain/landforms.hpp"
#include "world/chart.hpp"

namespace svx::city {

class MacroFields;
class IslandPlan;
struct Settlement;
struct Urban;

// A landform's stream channel (a canyon floor's stream, a ravine's or a creek's): the distance
// (m) from its centre line, its half width (m), the extra depth of its bed (m) and whether it
// carries water (a dry sandy wash in true desert).
struct Stream {
  double d = 0, half = 0, extra = 0;
  bool wet = true;
};

// Terrain.sample()'s record.
struct TerrainSample {
  double h = 0;        // height (voxels)
  double natural = 0;  // the natural (landform) height (voxels)
  double u = 0, core = 0;
  const Settlement* settlement = nullptr;
  double grade = 0;     // the city grading's weight
  double mountain = 0;  // 0..1
  double canyon = 0;    // 0..1 depth of a canyon cut
  double ravine = 0;    // 0..1
  double outcrop = 0;   // 0..1 bare granite of an outcrop / shore slab (open country only)
  std::optional<Stream> stream;
  double rough = 0;     // metres of small-scale roughness in h (land cover reads slopes without it)
  double rugged = 0;    // 0..1 how rugged the open country is (landforms' ruggedness; 0 in towns)
  double coast = 0;     // island mode: distance to the shore (m, positive inland); Infinity elsewhere
};

// config.terrain as the landforms and the terrain read it (once).
struct TerrainCfg {
  double lowland_base, continent_scale, continent_amplitude, hill_amplitude, hill_scale, detail_amplitude, detail_scale;
  double mountain_uplift, mountain_base, mountain_height, mountain_scale, mountain_detail, mountain_detail_scale;
  double valley_scale, valley_depth, mountain_gain, gully_depth, plateau_height, mesa_step, canyon_scale, canyon_depth;
  double ravine_scale, ravine_depth, rugged, relief_knoll_scale, relief_hummock_scale, relief_knoll, relief_hummock, relief_bump;
  double creek_scale, city_relief, city_relief_scale;
  explicit TerrainCfg(const Value& t);
};

class Terrain;

// The landform stack's context for one run (JS: terrain.ctx): the position, urbanization,
// mountainness, lazy climate / desertness / coast type, and the hints the landforms leave.
struct TerrainCtx {
  // A fresh context of a Terrain (its constants; nothing computed).
  explicit TerrainCtx(const Terrain& t);

  const Terrain* terrain = nullptr;
  const IslandPlan* island = nullptr;  // island mode: the plan (null elsewhere)
  const TerrainCfg* cfg = nullptr;
  double torus_r = 0;  // the torus chart's radius (0 elsewhere)

  double x = 0, y = 0;                       // voxels
  double fx = 0, fy = 0, fz = 0, fw = js::kNaN;  // field point (fw: torus charts only; NaN elsewhere)
  double u = 0;                              // urbanization
  double mountain = 0;                       // 0..1 (faded towards towns)
  double h = 0;                              // metres
  double lowland = 0;                        // the height before the mountains (m)
  double ridge = 0, canyon = 0, ravine = 0;  // hints
  double outcrop = 0;                        // 0..1 bare rock of a granite outcrop
  double valley = 0, rough = 0;
  double channel = 0;                        // 0..1 how close to a stream / ravine / canyon channel
  double rugged_ = -1;                       // (JS _rugged: landforms' ruggedness, lazy)
  std::optional<Stream> stream;
  double prox = 0;
  double coast = js::kInf;                   // island mode: the coast distance (m, positive inland)
  double cliff_ = -1;                        // (lazy coast type)
  bool clim_ = false;
  double clim_t = 0, clim_m = 0;             // (lazy climate)
  double desert_ = -1;                       // (lazy desertness)

  // island mode: the coast type at (x, y), lazily
  double cliff();
  // the climate at (x, y): { t, m }, lazily
  void climate(double* t, double* m);
  // desertness of the climate, lazily
  double desert();
  // chart point (m) -> field point (landforms that sample elsewhere, e.g. gullies)
  FieldPoint to_field(double xm, double ym) const;
};

// The landforms' memos, one per thread and per Terrain: values that are pure functions of their
// key (the gullies' kernels of an 80 m lattice cell), kept only to be made once. Nothing read
// from them depends on what was asked before.
struct LandformMemo {
  struct GullyKernel {
    double x, y, nx, ny, a, p1, p2;
  };
  struct Cell {
    double i, j;
    bool operator==(const Cell& o) const { return i == o.i && j == o.j; }
  };
  struct CellHash {
    size_t operator()(const Cell& c) const;
  };
  std::unordered_map<Cell, GullyKernel, CellHash> gully;
};

class Terrain {
 public:
  Terrain(const Value& config, std::shared_ptr<const Chart> chart, std::shared_ptr<const MacroFields> fields);
  ~Terrain();
  Terrain(const Terrain&) = delete;
  Terrain& operator=(const Terrain&) = delete;

  // Runs the landform stack at voxel (x, y) in context c (reset first); returns the height (m)
  // and leaves the hints in c. prox: the settlement proximity (computed when not given).
  double natural(TerrainCtx& c, double x, double y, double u, double fx, double fy, double fz, std::optional<double> prox = std::nullopt,
                 double fw = js::kNaN) const;
  // The same in a context of its own (the height only).
  double natural(double x, double y, double u, double fx, double fy, double fz, std::optional<double> prox = std::nullopt,
                 double fw = js::kNaN) const;
  // The lowland height (m) at a settlement centre: its graded city level (made once, in a context
  // of its own, and cached on the record).
  double settlement_base(const Settlement& s) const;
  double city_meters(double fx, double fy, double fz, double fw, const Urban& ur) const;
  // The full sample. Pass a precomputed urban sample to avoid evaluating it again; raw skips the
  // world's port grading.
  TerrainSample sample(double x, double y, const Urban* urban = nullptr, bool raw = false) const;
  double height(double x, double y) const;

  // The world's hook (createWorld): harbour towns ease down to their lake. Set once, before any
  // generation; null: none. (Terrain samples it makes run in contexts of their own.)
  std::function<double(double x, double y, double h)> port_grade;

  const Value& config() const { return config_; }
  const Chart& chart() const { return *chart_; }
  const MacroFields& fields() const { return *fields_; }
  const TerrainCfg& cfg() const { return cfg_; }
  double sea_level = 0;  // voxels

  // The landform stack, by order.
  struct Form {
    const Landform* lf;
    LandformState st;
  };
  const std::vector<Form>& forms() const { return forms_; }

  // This thread's landform memos of this Terrain.
  LandformMemo& memo() const;

 private:
  Value config_;
  std::shared_ptr<const Chart> chart_;
  std::shared_ptr<const MacroFields> fields_;
  TerrainCfg cfg_;
  SimplexNoise n_city_;
  std::vector<Form> forms_;
  std::shared_ptr<const char> token_;  // (the identity of this Terrain for the threads' memos)
};

}  // namespace svx::city
