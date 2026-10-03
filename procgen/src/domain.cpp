#include "svx/procgen/domain.hpp"
#include <algorithm>

namespace svx {
void load_domain(Game& game, const DomainConfig& config) {
  constexpr f64 h = .125;
  u32 seed = config.seed;
  auto random = [&]() { seed = seed * 1664525u + 1013904223u; return f64(seed) / 4294967296.0; };
  const f64 difficulty = std::clamp(config.difficulty, 0.0, 1.0);
  VoxelGrid grid;
  grid.h = h;
  const Vox floor = make_vox(MaterialId::Rock, true);
  for (i32 x = -64; x < 64; ++x)
    for (i32 y = -64; y < 96; ++y) {
      i32 top = 0;
      const bool stairs = config.terrain == DomainTerrain::Stairs || (config.terrain == DomainTerrain::Mixed && x < 16);
      if (stairs && y >= 12 && y < 60 && std::abs(x) < 24) {
        const i32 step = std::min((y - 12) / 4, (59 - y) / 4);
        top = (1 + i32(difficulty > .65)) * (step + 1);
      }
      const bool rough = config.terrain == DomainTerrain::Rough || (config.terrain == DomainTerrain::Mixed && x >= 16);
      if (rough && (std::abs(x) > 8 || std::abs(y) > 8)) {
        // Broad patches have room for a sole; sub-voxel white noise does not.
        u32 patch = config.seed ^ (u32((x + 64) / 4) * 73856093u) ^ (u32((y + 64) / 4) * 19349663u);
        patch ^= patch >> 13; patch *= 1274126177u;
        top = i32(patch % u32(2 + i32(3 * difficulty)));
      }
      grid.fill_column(x, y, -2, top + 1, floor);
    }
  if (config.terrain == DomainTerrain::Obstacles || config.terrain == DomainTerrain::Mixed) {
    for (i32 n = 0; n < 12 + i32(20 * difficulty); ++n) {
      const i32 x = i32(random() * 96) - 48, y = 18 + i32(random() * 66);
      if (config.terrain == DomainTerrain::Mixed && std::abs(x) < 25) continue;
      const i32 z = 1 + i32(random() * (1 + 2 * difficulty));
      for (i32 a = 0; a < 3 + n % 5; ++a)
        for (i32 b = 0; b < 2; ++b) grid.fill_column(x + a, y + b, 1, z + 1, floor);
    }
  }
  grid.compact(); grid.lo = {-64, -64, -2}; grid.hi = {64, 96, 128};
  game.load(std::move(grid), {0, 0, h / 2}, {0, 1, 0});
  game.bake();
  if (config.terrain == DomainTerrain::Mixed && config.loose_objects > 0) {
    MoverDef lift;
    lift.kind = MoverDef::Kind::Floor;lift.z0=1;lift.z1=9;lift.rows=0;lift.vox=floor;
    for(i32 x=-42;x<-26;++x)for(i32 y=20;y<36;++y)lift.cols.push_back({x,y});
    MoverMove cycle;cycle.type=MoverMove::Type::Cycle;cycle.target=8;cycle.back=0;cycle.speed=2;cycle.wait=2;
    lift.moves.push_back(cycle);game.activate_mover(game.add_mover(lift));
  }
  for (i32 n = 0; n < std::clamp(config.loose_objects, 0, 32); ++n) {
    Drop drop;
    drop.desc.frame.origin = {(n % 2 ? -1 : 1) * (1.2 + random() * 3), 2 + random() * 7, 2 + random()};
    drop.voxels.h = h;
    for (i32 x = -1; x <= 1; ++x)
      for (i32 y = -1; y <= 1; ++y) drop.voxels.fill_column(x, y, -1, 2, make_vox(MaterialId::Wood, false));
    drop.voxels.compact(); drop.voxels.lo = {-1, -1, -1}; drop.voxels.hi = {2, 2, 2};
    game.add_drop(std::move(drop));
  }
}
}  // namespace svx
