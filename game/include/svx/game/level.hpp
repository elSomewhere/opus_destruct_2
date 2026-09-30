// structvox game — a bounded level as a game loads it: its world grid, its oriented grids, the
// joints between them and the objects dropped in when play starts. Whatever makes levels (the
// procedural generators of svx_procgen, a map importer, an editor) hands the game one of these;
// the game knows nothing of how it was made.
#pragma once

#include <vector>

#include "svx/base/vec.hpp"
#include "svx/game/game.hpp"
#include "svx/world/grid.hpp"
#include "svx/world/joint_desc.hpp"
#include "svx/world/world.hpp"

namespace svx {

// A grid of the world's placed at its own origin and rotation (docs/GRIDS.md).
struct LevelGrid {
  GridFrame frame;
  VoxelGrid grid;
};

struct Level {
  VoxelGrid grid;
  std::vector<LevelGrid> grids;  // added after the world grid, in order (World::add_grid: ids 1, 2, ...)
  // Joints (their Grid anchors: the ids the grids get as above): the machines' and the hanging
  // parts' (docs/MOTION.md).
  std::vector<JointDesc> joints;
  // Objects dropped in when play starts (crates on a turntable).
  std::vector<Drop> drops;
  V3 spawn_pos{0, 0, 0};  // feet, metres
  V3 spawn_dir{1, 0, 0};
};

// Adds a level's oriented grids to the world it was loaded into (after World::load or Game::load
// took its grid), in order.
void add_grids(World& world, std::vector<LevelGrid>&& grids);

// Loads a level into a game: its grid, its oriented grids, joints and drops.
void load_level(Game& game, Level&& level);

}  // namespace svx
