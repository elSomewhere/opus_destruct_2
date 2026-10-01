// svx_city — the city as a structvox world's source (voxel_city svx/source.js createSvxSource,
// with the export's changes of docs/PROCGEN_MERGE_PLAN.md §7.3): the same calls as a structvox
// ChunkSource and GameSource, in plain arrays and records a host copies into its own types - the
// adapter in svx_procgen (svx/procgen/city_source.hpp) does - so that svx_city needs nothing of
// the engine's.
//
// Conventions (structvox's, docs/GRIDS.md):
//   - a chunk is 32^3 voxels, index (x * 32 + y) * 32 + z; world voxel (x, y, z) is the city's
//     voxel (x, y, z), its centre at h (x, y, z) metres (h = 0.125 m), z up;
//   - a voxel byte is structvox's Vox: 0 air, else (1 + physics class) | 0x80 anchored
//     (svx/city/materials.hpp: the classes; a class's looks in the "look" layer as 1 + the look's
//     index within its class);
//   - plants are the foliage class (decorative to the engine); liquids are air with 255 in the
//     "water" layer; props and furniture by attachment (data/city/props.json): fixed ones bond as
//     structure, loose ones have seams on every outer face, entities are not voxels but spawn
//     records (spawns_in);
//   - an oriented grid's voxel p is centred at origin + R(rot) (voxel_size p).
//
// Every call is a pure function of the world and its arguments, may come from several threads at
// once, and keeps the generator's caches within their byte budget (memory_bytes).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "svx/city/world.hpp"

namespace svx::city {

inline constexpr int kExportChunk = 32;
inline constexpr int kExportChunkVox = kExportChunk * kExportChunk * kExportChunk;
inline constexpr int32_t kExportNoWater = INT32_MIN;

// A chunk of the world grid.
struct ChunkData {
  std::vector<uint8_t> vox;    // kExportChunkVox bytes (empty: all air)
  std::vector<uint8_t> look;   // 1 + the voxel's look within its class (0: none); empty: none
  std::vector<uint8_t> water;  // 255 where water stands; empty: none
  std::vector<uint8_t> seams;  // bit a: the voxel's face towards +axis a does not bond (towards the
                               // next chunk's voxels too); empty: none
  bool any = false;            // a solid voxel
};

// A part of the angled world (a turned building row, a pitched road piece, a ramp, a wing) as a
// structvox oriented grid: its voxel p centred at origin + R(rot) (voxel_size p).
struct GridInfo {
  uint32_t id = 0;                         // stable, not 0, below 2^30
  std::array<double, 3> origin{};          // metres
  std::array<double, 4> rot{0, 0, 0, 1};   // quaternion (x, y, z, w)
  double voxel_size = 0.125;
  int priority = 0;
  bool anchored = false;                   // (a road piece: its slab rests on the ground)
  std::string kind, key;                   // ("road", "building", "ramp", "wing"; the part's key)
  std::array<int, 3> home{};               // the world chunk it is at home in
};

// A chunk of a grid's own lattice (32^3 of its voxels from 32 c).
struct GridChunk {
  std::array<int, 3> c{};
  std::vector<uint8_t> vox, look, seams;   // as ChunkData's (look, seams empty: none)
};

// An entity the city places (a parked car; a fire engine in its station's bay, an ambulance at
// its hospital's porch, a police car in its garage; a base's lorries and tanks): no voxels, a
// record the host spawns its vehicle from.
struct SpawnInfo {
  uint64_t id = 0;                  // stable, below 2^52
  std::string kind;                 // data/city/props.json's entity kind: "car", "fire_truck", ...
  std::array<double, 3> pos{};      // metres: the middle of its footprint, on the ground
  double yaw = 0;                   // radians: its heading, from +x towards +y
  std::string context;              // where it stands: "parked", "station:fire", "base", ...
};

class Export {
 public:
  // The export of a world (its parts apart from the world grid, as createSvxSource makes it);
  // null and *error for a spec make_world refuses.
  static std::shared_ptr<const Export> make(const WorldSpec& spec, std::string* error = nullptr);
  ~Export();

  // Its world (for the roads: svx/city/roads.hpp).
  std::shared_ptr<const World> world() const;

  // The world's extent in chunks: [lo, hi) per axis (an island's sea reaches as far as the rest).
  void extent(std::array<int, 3>& lo, std::array<int, 3>& hi) const;
  // The chunks of column (cx, cy) that hold content: [z_lo, z_hi); below them the column is
  // uniformly `below` (the anchored ground's byte), above them air.
  void column_range(int cx, int cy, int* z_lo, int* z_hi, uint8_t* below) const;
  // A chunk of the world grid; false: nothing in it (air, no water).
  bool chunk(int cx, int cy, int cz, ChunkData& out) const;
  // The parts at home in a chunk.
  void grids(int cx, int cy, int cz, std::vector<GridInfo>& out) const;
  // A part's record and its chunks (those holding voxels); `near` a chunk near its home (the
  // part is found from its id's bits and the cell nearest it). False: no such part.
  bool grid(uint32_t id, const std::array<int, 3>& near, GridInfo* info, std::vector<GridChunk>& out) const;
  // The region a chunk's changes are remembered with: the city block of the building most of its
  // column holds, else of its middle, else 8 x 8 columns (a key below 2^53).
  uint64_t region(int cx, int cy) const;
  // The far tier: n cells of factor^3 voxels from voxel lo (index (x * n1 + y) * n2 + z), the world
  // grid at the LOD of that size (factor a power of two, 2 .. 256).
  bool coarse(const std::array<int, 3>& lo, const std::array<int, 3>& n, int factor, std::vector<uint8_t>& out) const;
  // The far tier's water: over n[0] x n[1] columns of factor^2 voxels from lo, the z of the top
  // water voxel of the sea or a lake in each (kExportNoWater: none; index x * n1 + y). False:
  // none anywhere.
  bool coarse_water(const std::array<int, 3>& lo, const std::array<int, 3>& n, int factor, std::vector<int32_t>& out) const;
  // Where a player starts: at the origin on the ground (metres), facing +x.
  void spawn(std::array<double, 3>& pos, std::array<double, 3>& dir) const;
  // The entities in a box (metres, x and y), in a stable order (by id).
  void spawns_in(const std::array<double, 2>& lo, const std::array<double, 2>& hi, std::vector<SpawnInfo>& out) const;
  // What its caches hold (bytes).
  int64_t memory_bytes() const;

  struct Impl;

 private:
  explicit Export(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace svx::city
