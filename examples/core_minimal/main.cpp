// The smallest host of the physics core: build a world, knock out its supports, step it, and
// read back what happened. Links svx_core only (no meshing, no game).
//
//   ./build/native-release/examples/svx_core_minimal
#include <cstdio>

#include "svx/world/world.hpp"

using namespace svx;

int main() {
  // 1. A voxel grid (h = 0.125 m): an anchored rock plate, a 1 m x 1 m brick tower 6 m tall on
  //    it, and a concrete lintel across to a second tower.
  VoxelGrid g;
  g.h = 0.125;
  auto box = [&](IVec3 lo, IVec3 hi, Vox v) {
    for (i32 x = lo[0]; x < hi[0]; ++x)
      for (i32 y = lo[1]; y < hi[1]; ++y) g.fill_column(x, y, lo[2], hi[2], v);
  };
  box({-16, -16, -4}, {64, 32, 0}, make_vox(MaterialId::Rock, true));
  box({0, 0, 0}, {8, 8, 48}, make_vox(MaterialId::Masonry, false));
  box({32, 0, 0}, {40, 8, 48}, make_vox(MaterialId::Masonry, false));
  box({0, 0, 48}, {40, 8, 52}, make_vox(MaterialId::Concrete, false));
  g.compact();

  // 2. The world: load, then the design pass (members that would not carry their own weight
  //    are strengthened, so the level stands as built).
  World world;
  world.load(std::move(g));
  world.bake();
  std::printf("designed: max utilization %.2f, %lld voxels strengthened\n", world.design_report().max_utilization,
              static_cast<long long>(world.design_report().strengthened_voxels));

  // 3. Commands: blow out the foot of the first tower.
  world.blast({0.5, 0.5, 0.5}, 0.6, 2e5);

  // 4. Step it (60 ticks per second) and read back the events.
  int pieces = 0, cracks = 0, dust = 0;
  for (int t = 0; t < 600; ++t) {
    world.tick();
    for (const WorldEvent& e : world.take_events()) {
      if (e.kind == WorldEvent::Kind::PieceAdded) ++pieces;
      if (e.kind == WorldEvent::Kind::Crack) ++cracks;
      if (e.kind == WorldEvent::Kind::Dust) ++dust;
    }
    // (a renderer would mesh world.take_changed_chunks() and draw world.pieces() here)
    world.take_changed_chunks();
  }
  const WorldStats s = world.stats();
  std::printf("after 10 s: %d pieces made (%d alive, %d awake), %d cracks, %d dust puffs, %lld bonds broken\n", pieces,
              s.bodies, s.awake, cracks, dust, static_cast<long long>(s.bonds_broken));
  for (const PieceState& p : world.pieces())
    if (p.voxels > 2000)
      std::printf("  piece %lld: %d voxels, %.0f kg at (%.2f %.2f %.2f)%s\n", static_cast<long long>(p.id), p.voxels, p.mass,
                  p.pos.x, p.pos.y, p.pos.z, p.asleep ? " (asleep)" : "");

  // 5. Persistence: the changes as a delta against the base world.
  std::printf("delta: %zu bytes\n", world.save_delta().size());
  return 0;
}
