// structvox procgen — the city generator as a game's world (docs/CITY.md §7, the adapter of
// docs/PROCGEN_MERGE_PLAN.md §11 phase 2): svx_city's export (svx/city/source.hpp: a preset's world
// in plain arrays and records) copied into the engine's types. Its chunks with their seams (loose
// props and furniture) and layers - "look" (regenerable: the city's appearances), "water" - its
// column content ranges, regions, oriented grids (the angled world's parts), the far tier's coarse
// voxels and water, its roads (svx/procgen/city_roads.hpp), its entities as spawn records, and the
// city's materials (registered when it is made) with their fire facets.
#pragma once

#include <array>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "svx/city/source.hpp"
#include "svx/game/source.hpp"
#include "svx/procgen/city_roads.hpp"

namespace svx {

class CityWorldSource final : public GameSource {
 public:
  explicit CityWorldSource(std::shared_ptr<const city::Export> e);

  // ChunkSource
  bool generate(const IVec3& chunk, std::vector<Vox>& out) const override;
  IVec3 chunk_lo() const override { return lo_; }
  IVec3 chunk_hi() const override { return hi_; }
  void column_range(i32 cx, i32 cy, i32* z_lo, i32* z_hi, Vox* below) const override;
  bool generate_seams(const IVec3& chunk, std::vector<u8>& out) const override;
  u64 region(const IVec3& chunk) const override;
  bool generate_layer(const IVec3& chunk, const std::string& layer, std::vector<u8>& out) const override;
  std::vector<SourceGrid> grids(const IVec3& chunk) const override;
  bool generate_grid(u32 id, VoxelGrid& out) const override;
  i64 memory_bytes() const override;

  // GameSource
  V3 spawn_pos() const override { return spawn_pos_; }
  V3 spawn_dir() const override { return spawn_dir_; }
  const RoadNetwork* roads() const override { return roads_.get(); }
  std::shared_ptr<const AppearanceTable> appearances() const override { return appearances_; }
  void spawns_in(const V3& lo, const V3& hi, std::vector<SpawnRecord>& out) const override;
  void fire_materials(FireSystem& fire) const override;
  bool coarse(const IVec3& lo, const IVec3& n, i32 factor, std::vector<Vox>& out) const override;
  bool coarse_water(const IVec3& lo, const IVec3& n, i32 factor, std::vector<i32>& out) const override;

  const city::Export& city() const { return *export_; }

 private:
  // A chunk's export (generate, its layers and seams are asked one after another: the last few
  // are kept).
  std::shared_ptr<const city::ChunkData> chunk_data(const IVec3& c) const;

  std::shared_ptr<const city::Export> export_;
  std::unique_ptr<CityRoadNetwork> roads_;
  std::shared_ptr<const AppearanceTable> appearances_;
  IVec3 lo_{0, 0, 0}, hi_{0, 0, 0};
  V3 spawn_pos_, spawn_dir_{1, 0, 0};
  static constexpr size_t kRecent = 96;
  mutable std::mutex mu_;
  mutable std::list<std::pair<u64, std::shared_ptr<const city::ChunkData>>> recent_;
  mutable std::unordered_map<u64, std::list<std::pair<u64, std::shared_ptr<const city::ChunkData>>>::iterator> recent_at_;
  // (the homes of the grids handed out: the export finds a part from its id and a chunk near it)
  static constexpr size_t kHomes = 8192;
  mutable std::list<std::pair<u32, IVec3>> homes_;
  mutable std::unordered_map<u32, std::list<std::pair<u32, IVec3>>::iterator> home_at_;
};

// A city world from a preset's parameters (docs/PRESETS.md: "preset", "size", "season", "config"
// - voxel_city's config overrides) and a seed; null and *error when they are not one.
std::shared_ptr<CityWorldSource> make_city_world(const std::string& params_json, u64 seed, std::string* error);

}  // namespace svx
