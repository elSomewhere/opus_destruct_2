// svx_city — voxel_city buildings/source.js.
#include "buildings/source.hpp"

#include "buildings/massing.hpp"
#include "buildings/wings.hpp"
#include "core/js.hpp"

namespace svx::city {

namespace {

// world.config.world.angles?.partsMode === "separate"
bool separate_parts(const World& world) {
  const Value& m = world.config["world"]["angles"]["partsMode"];
  return m.is_string() && m.str() == "separate";
}

}  // namespace

bool building_z_range(const World& world, const EnvelopeList& envs, double* z0, double* z1) {
  double lo = js::kInf;
  double hi = -js::kInf;
  // (parts mode: a turned building, a part of its own, is drawn in its lattice)
  const bool separate = separate_parts(world);
  for (const auto& env : envs) {
    if (separate && !env->part.empty()) continue;
    if (env->bottom_z < lo) lo = env->bottom_z;
    if (env->top_z > hi) hi = env->top_z;
  }
  if (lo == js::kInf) return false;
  *z0 = lo;
  *z1 = hi;
  return true;
}

void rasterize_buildings(const World& world, const EnvelopeList& envs, ChunkBuffer& chunk, const VoxelizeBuildingFn& voxelize_building) {
  const bool separate = separate_parts(world);
  for (const auto& env : envs) {
    if (separate && !env->part.empty()) continue;
    if (chunk.lod == 0 && voxelize_building)
      voxelize_building(*env, chunk);
    else
      voxelize_massing(world, *env, chunk);
    if (!separate)
      for (const Wing& w : env->wings) rasterize_wing(world, *env, w, chunk);
  }
}

}  // namespace svx::city
