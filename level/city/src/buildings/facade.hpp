// svx_city — facade rules shared by every LOD (voxel_city buildings/facade.js).
//
// A facade is described per building side by a bay grid (window centres every `bay` voxels,
// aligned to that side's length) and per floor by a window band (sill .. head). Interior
// planners align partition walls to the same bay grid, so the windows seen from far away are the
// windows found inside.
#pragma once

#include <string>

#include "buildings/styles.hpp"

namespace svx::city {

struct Envelope;  // (buildings/archetypes.hpp)

// WIN: the class of a facade voxel.
namespace WIN {
enum : int {
  WALL = 0,
  GLASS = 1,
  FRAME = 2,
  SILL = 3,
  LINTEL = 4,
  STOREFRONT = 5,
  SPANDREL = 6,
  JOINT = 7,
  // nordic styles: board seams, window casings, corner boards (trim)
  SEAM = 8,
  CASING = 9,
  CORNER = 10,
};
}  // namespace WIN

// A building's resolved style and facade parameters (buildingLook's record).
struct BuildingLook {
  ResolvedStyle style{};
  double bay = 0, win_w = 0, sill = 0, head = 0;  // (voxels)
  std::string type{};                             // the window system: punched, ribbon, curtain, open
  bool lintel = false;
  bool storefront = false;  // a shop front on the ground floor
  double lit_seed = 0;      // (which windows are lit at night)
  int joint = -1;           // prefab panels' joint material (-1: null)
  bool casing = false, transom = false;
  bool boards = false;      // any of the nordic wall treatments (seams, corner boards)
  double seed = 0;          // (the world seed it was made with)
};

// buildingLook(env, seed): the building's look, made from (seed, env.id, "look") the first time and
// kept on the envelope. The reference keeps it per envelope whatever the seed (an envelope belongs
// to one world); asking an envelope's look with another seed than the first is a programming
// error here (SVX_FAIL), so a look never depends on which seed asked first.
const BuildingLook& building_look(const Envelope& env, double seed);
// (the same, made afresh)
BuildingLook make_building_look(const Envelope& env, double seed);

// Classifies a facade voxel (a WIN class): len the length of the facade side (voxels), t the
// position along it (0 .. len - 1), zr the height above the floor slab's bottom (0: the slab's
// bottom), H the story height (voxels), floor the floor index (0: ground).
int facade_cell(const BuildingLook& look, double len, double t, double zr, double H, double floor);
// A plain wall cell, or its board seam or corner board in the nordic styles.
int wall_class(const BuildingLook& look, double len, double t, double zr);
// The material of a facade class for this building (-1: JS's null, the seam of a style without
// one).
int facade_material(const BuildingLook& look, int cls, double floor, double zr = 99);

}  // namespace svx::city
