// svx_city — diagonal boulevards (voxel_city city/diagonals.js, ANGLED_WORLD_PLAN.md S1): global
// lines at a fixed exact yaw laid over the arterial grid, which itself stays as it is
// (network/arterials). Like the arterial lines, line k of a family depends only on its index, so
// every cell clips the lines crossing it on its own and the pieces of a line in two cells meet
// exactly on their shared edge.
//
// Two families cross each other at 73.7 degrees and the arterial grid at 36.9 and 53.1, the
// 3-4-5 triple and its mirror: steep enough that every crossing is a proper junction
// (network/roadView). With direction (c, s) / r a line is { p : -s x + c y = D } in integers;
// D = r x (signed distance of the line from the origin, voxels), about `angles.diagonalSpacing`
// apart. The piece of a line in a cell is built where it runs through a town (city/cellNetwork).
#pragma once

#include <array>
#include <vector>

#include "core/rect.hpp"
#include "core/value.hpp"

namespace svx::city {

// A family of lines: its index f, its table yaw (core/placement) and that yaw's exact direction
// (c, s) / r.
struct DiagonalFamily {
  double f = 0;
  int yaw = 0;
  double c = 1, s = 0, r = 1;
};

// DIAGONAL_FAMILIES: [nearestYaw(4, 3), nearestYaw(4, -3)].
const std::array<DiagonalFamily, 2>& diagonal_families();

// The offset D (an integer, r x voxels) of line k of family `fam`.
double diagonal_offset(double seed, const DiagonalFamily& fam, double k, double spacing);

// Do diagonals exist in this world (angled streets on a flat chart)?
bool diagonals_on(const Value& config);

// The piece of line k of family fam crossing a cell: a -> b along the family's direction.
struct DiagonalPiece {
  const DiagonalFamily* fam = nullptr;
  double k = 0, D = 0;
  Point2 a, b;
};

// The pieces of diagonal lines crossing cell rect `rect` ([x0, x1) x [y0, y1), arterial line
// positions). Pure: no neighbour asked.
std::vector<DiagonalPiece> diagonal_pieces(const Value& config, const Rect& rect);

}  // namespace svx::city
