// svx_city — farmland (voxel_city nature/farmland.js): the fields of open country round towns,
// villages and farms.
//
// The land is cut into farm blocks of BLOCK (300 m); each block is split into 2-6 strips of
// varying width across its own direction, and a strip now and then once more across, so fields
// come in long parcels of different sizes rather than a checkerboard. Every field has a crop by
// climate (wheat, rapeseed, maize and potatoes where it is mild; hay meadows, pasture and potatoes
// in the north) and shows the season (young green shoots in spring, rapeseed in flower, golden
// wheat in summer, stubble and ploughed soil in autumn). Fields are separated by grass margins;
// some boundaries carry a hedgerow (shrubs and trees: nature/forest) or, in the north, a low wall
// of cleared field stones.
//
// Pure functions of position (canonical in a wrapping world); immutable, so any thread may use
// one. (The reference hashes some keys with five or six arguments: its hash32 reads four, so
// those are the four-argument hashes here, e.g. a field's key is its strip's cut hash.)
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "world/wrap.hpp"

namespace svx::city {

class World;

// Farmland.fieldAt's record: the field's key (a uint32 hash), whether its rows run along x, the
// position across / along its strip (a, b: voxels), the distance to the nearest field boundary
// (edge, voxels) and that boundary's key (edge_key, a uint32 hash).
struct Field {
  double key = 0;
  bool along_x = false;
  double a = 0, b = 0;
  double edge = 0;
  double edge_key = 0;
};

// Farmland.ground's [top, sub, bump] (bump: voxels of a field wall above the ground).
struct FieldGround {
  uint16_t top = 0, sub = 0;
  double bump = 0;
};

class Farmland {
 public:
  explicit Farmland(const World& world);

  double seed = 0;
  Wrap wrap;
  std::string season;  // seasonOf(config).id

  // The field at a world column.
  Field field_at(double x0, double y0) const;
  // The crop of a field (key: field_at().key) at local temperature t: wheat, barley, rapeseed,
  // potato, maize, hay, pasture or fallow.
  std::string_view crop(double key, double t) const;
  // Ground of a field column for climate t at world column (x, y).
  FieldGround ground(const Field& f, double t, double x, double y) const;
  // Hedgerow at a column of field f: 0..1, high within a metre of a field boundary that carries a
  // hedge (a third of them where it is mild, fewer in the north).
  double hedge(const Field& f, double t) const;
};

}  // namespace svx::city
