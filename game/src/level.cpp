#include "svx/game/level.hpp"

#include <utility>

namespace svx {

void add_grids(World& world, std::vector<LevelGrid>&& grids) {
  for (LevelGrid& lg : grids) world.add_grid(lg.frame, std::move(lg.grid));
  grids.clear();
}

void load_level(Game& game, Level&& level) {
  game.load(std::move(level.grid), level.spawn_pos, level.spawn_dir);
  World& world = game.world();
  add_grids(world, std::move(level.grids));
  for (const JointDesc& j : level.joints) world.add_joint(j);
  for (Drop& d : level.drops) game.add_drop(std::move(d));
  level.joints.clear();
  level.drops.clear();
}

}  // namespace svx
