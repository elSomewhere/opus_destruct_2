// svx_city — landforms (voxel_city terrain/landforms.js): the terrain height is composed by a
// stack of registered landforms, each a pure function of position that edits ctx.h (metres) in
// `order`. A landform may also leave hints for land cover (canyon walls, ravine floors, stream
// channels). Adding a new kind of terrain means registering one entry.
//
//   noises         the noises its init(seedNoise) asks for, in order (each a SimplexNoise seeded
//                  with deriveSeed(seed, "terrain.<name>")): the per-world state
//   apply(ctx, st) edits ctx.h; ctx carries x, y (voxels), fx, fy, fz, fw (field metres), u
//                  (urbanization), mountain (0..1), cfg (config.terrain) and climate() (lazy t, m)
//
// The reference registers: continent, hills, mountains, plateau, glacialValleys, gullies, mesas,
// dunes, canyons, ravines, relief, roughness, outcrops, creeks (Terrain sorts them by order).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/noise.hpp"
#include "world/registry.hpp"

namespace svx::city {

struct TerrainCtx;

// A landform's per-world state: its noises, in the order of its `noises`.
struct LandformState {
  std::vector<SimplexNoise> n;
};

struct Landform {
  std::string id;
  double order = 0;
  std::vector<std::string> noises;
  std::function<void(TerrainCtx& ctx, const LandformState& st)> apply;
};

// LANDFORMS, in registration order.
const Registry<Landform>& landforms();
// The registry for registering (register_all, and code adding landforms before generation).
Registry<Landform>& landforms_mut();
// Registers the reference's landforms (terrain/landforms.js), in its order.
void register_landforms();

// How rugged the open country is (0..1), cached in the context: st is the relief landform's state
// (its 4th noise, "reliefRugged").
double ruggedness(TerrainCtx& ctx, const LandformState& st);

}  // namespace svx::city
