// svx_city — the global arterial lines (voxel_city network/arterials.js). Vertical lines
// x = X(i) and horizontal lines y = Y(j) with per-line jitter; their crossings define the
// "arterial cells" that are the fundamental, independently planned unit of the city. Each line
// depends only on its index, so any cell can be planned without its neighbours and all shared
// edges agree exactly. (World::cell_at and World::cells_overlapping are defined here.)
#pragma once

#include "core/rect.hpp"
#include "core/value.hpp"
#include "world/World.hpp"
#include "world/wrap.hpp"

namespace svx::city {

class ArterialGrid {
 public:
  explicit ArterialGrid(const Value& config);

  double seed = 0;
  double spacing = 0;  // voxels
  double jitter = 0;
  // a wrapping world has n lines round it (0: unbounded)
  Wrap wrap;
  double n = 0;

  // Position (voxels, a multiple of 8) of vertical line i (axis 0) or horizontal line j (axis 1).
  double line(int axis, double index) const;
  // The line of a canonical index (no laps).
  double line_at(int axis, double index) const;
  // Canonical line index (a wrapping world's lines repeat every n).
  double canon(double index) const { return Wrap::canon(index, n); }
  // Index of the line at or before coordinate v.
  double index_at(int axis, double v) const;
  // Cell (i, j) containing the point.
  CellIJ cell_at(double x, double y) const { return {index_at(0, x), index_at(1, y)}; }
  // Centre-line rect of cell (i, j): {x0, y0, x1, y1} (x1 / y1 = the next lines).
  Rect cell_rect(double i, double j) const { return {line(0, i), line(1, j), line(0, i + 1), line(1, j + 1)}; }
};

}  // namespace svx::city
