// Repeatable character courses built from the same voxels and pieces as a level.
#pragma once
#include "svx/game/game.hpp"

namespace svx {
enum class DomainTerrain : u8 { Flat, Stairs, Rough, Obstacles, Mixed };
struct DomainConfig {
  u32 seed = 7;
  DomainTerrain terrain = DomainTerrain::Mixed;
  f64 difficulty = .5;  // 0..1: riser height, roughness and obstacle density
  i32 loose_objects = 6;
};
// Replaces a level. The centre starts clear; +Y crosses the course. The seed
// describes geometry, never wall-clock time or thread scheduling.
void load_domain(Game& game, const DomainConfig& config);
}  // namespace svx
