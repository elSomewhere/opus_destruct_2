// svx_city — the export (svx/city/source.hpp). A placeholder until the export is ported
// (voxel_city svx/source.js, world/partRaster.js, stream/queries.js probe: docs/CITY.md §5): the
// world and its extent are the export's; every chunk is air, there are no parts, no entities.
#include "svx/city/source.hpp"

#include <algorithm>
#include <cmath>

#include "core/value.hpp"
#include "world/World.hpp"

namespace svx::city {

struct Export::Impl {
  std::shared_ptr<World> world;
};

Export::Export(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Export::~Export() = default;

std::shared_ptr<const Export> Export::make(const WorldSpec& spec, std::string* error) {
  std::shared_ptr<World> w = make_world(spec, error);
  if (!w) return nullptr;
  auto impl = std::make_unique<Impl>();
  impl->world = std::move(w);
  return std::shared_ptr<const Export>(new Export(std::move(impl)));
}

std::shared_ptr<const World> Export::world() const { return impl_->world; }

void Export::extent(std::array<int, 3>& lo, std::array<int, 3>& hi) const {
  // (structvox's reach of voxel coordinates, svx/world/grid.hpp kVoxelLimit; an island's sea
  // reaches as far as the rest: PROCGEN_MERGE_PLAN.md §15 decision 7)
  constexpr int kLimit = (1 << 20) - 4096;
  const int lim = kLimit / kExportChunk - 1;
  const Value& w = impl_->world->config["world"];
  const bool island = w["mode"].str() == "island";
  const double peak = island ? w["island"]["peak"].num(0.0) : 6000.0;
  const int z_hi = std::min(lim, static_cast<int>(std::ceil(((peak + 800.0) * 8.0) / kExportChunk)));
  const int z_lo = -std::min(lim, static_cast<int>(std::ceil((600.0 * 8.0) / kExportChunk)));
  lo = {-lim, -lim, z_lo};
  hi = {lim, lim, z_hi};
}

void Export::column_range(int cx, int cy, int* z_lo, int* z_hi, uint8_t* below) const {
  (void)cx, (void)cy;
  *z_lo = 0;
  *z_hi = 0;
  *below = 0;
}

bool Export::chunk(int cx, int cy, int cz, ChunkData& out) const {
  (void)cx, (void)cy, (void)cz;
  out = ChunkData{};
  return false;
}

void Export::grids(int cx, int cy, int cz, std::vector<GridInfo>& out) const {
  (void)cx, (void)cy, (void)cz;
  out.clear();
}

bool Export::grid(uint32_t id, const std::array<int, 3>& near, GridInfo* info, std::vector<GridChunk>& out) const {
  (void)id, (void)near, (void)info;
  out.clear();
  return false;
}

uint64_t Export::region(int cx, int cy) const {
  return (uint64_t{1} << 52) + static_cast<uint64_t>((cx >> 3) & 0xffff) * 65536u + static_cast<uint64_t>((cy >> 3) & 0xffff);
}

bool Export::coarse(const std::array<int, 3>& lo, const std::array<int, 3>& n, int factor, std::vector<uint8_t>& out) const {
  (void)lo, (void)n, (void)factor, (void)out;
  return false;
}

bool Export::coarse_water(const std::array<int, 3>& lo, const std::array<int, 3>& n, int factor, std::vector<int32_t>& out) const {
  (void)lo, (void)n, (void)factor, (void)out;
  return false;
}

void Export::spawn(std::array<double, 3>& pos, std::array<double, 3>& dir) const {
  pos = {-0.0625, -0.0625, 0.0};
  dir = {1.0, 0.0, 0.0};
}

void Export::spawns_in(const std::array<double, 2>& lo, const std::array<double, 2>& hi, std::vector<SpawnInfo>& out) const {
  (void)lo, (void)hi;
  out.clear();
}

int64_t Export::memory_bytes() const { return 0; }

}  // namespace svx::city
