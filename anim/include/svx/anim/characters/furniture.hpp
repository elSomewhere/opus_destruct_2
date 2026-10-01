// svx_anim — voxel furniture to sit at: benches, chairs and desks (one-bone props, their origin on
// the floor under the seat's centre, facing +y: a sitter faces +y). `seat` gives the sitting
// surface and `kind` what the animator does there.
#pragma once

#include <optional>

#include "svx/anim/characters/props.hpp"

namespace svx::anim {

enum class FurnitureKind : u8 { Bench, Chair, Desk };

struct Furniture {
  ModelPtr model;
  Palette palette{};
  V3 seat;                         // the sitting surface's centre, relative to the prop origin
  std::optional<f64> desk_height;  // (desks) the desk top's height, m above the floor
  bool backrest = false;           // a backrest to lean on
  FurnitureKind kind = FurnitureKind::Bench;
  // Where a sitter stands before sitting down: this far (m) in front of the seat, along +y (none:
  // 0.38; closer at a table, so the body stays clear of the table top).
  std::optional<f64> approach;
};

const Palette& furniture_palette();

// A park bench (1.6 m) with a backrest.
Furniture make_bench(f64 voxel_size = kDefaultVoxelSize);
// A chair (seat 0.46 m).
Furniture make_chair(f64 voxel_size = kDefaultVoxelSize);
// A desk (0.74 m) with a chair behind it: the sitter faces +y across the desk top.
Furniture make_desk(f64 voxel_size = kDefaultVoxelSize);
// A café table (0.74 m) with a chair and an open laptop: the sitter faces +y across the table and
// works at it (the desk way of sitting). Its edge is 0.4 m in front of the seat, so a sitter can
// step in between chair and table before sitting down.
Furniture make_cafe_table(f64 voxel_size = kDefaultVoxelSize);

}  // namespace svx::anim
