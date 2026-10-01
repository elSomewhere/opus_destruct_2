// svx_city — network/arterials.hpp (voxel_city network/arterials.js), and World.js's cellAt and
// cellsOverlapping.
#include "network/arterials.hpp"

#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"

namespace svx::city {

ArterialGrid::ArterialGrid(const Value& config) {
  seed = config["seed"].to_number();
  spacing = vx(config["city"]["arterialSpacing"].to_number());
  jitter = config["city"]["arterialJitter"].to_number();
  wrap = Wrap(config);
  n = wrap.count(config["city"]["arterialSpacing"].to_number());
}

double ArterialGrid::line(int axis, double index) const {
  if (js::truthy(n)) {
    const double c = Wrap::canon(index, n);
    return line_at(axis, c) + Wrap::lap(index, n) * wrap.size_v;
  }
  return line_at(axis, index);
}

double ArterialGrid::line_at(int axis, double index) const {
  const double j = (hash_float(seed, index, axis == 0 ? 7001 : 7919) - 0.5) * 2 * jitter * spacing;
  return js::round((index * spacing + j) / 8) * 8;
}

double ArterialGrid::index_at(int axis, double v) const {
  double i = std::floor(v / spacing);
  while (line(axis, i) > v) i -= 1;
  while (line(axis, i + 1) <= v) i += 1;
  return i;
}

CellIJ World::cell_at(double x, double y) const { return arterials->cell_at(x, y); }

std::vector<CellIJ> World::cells_overlapping(const Rect& r) const {
  const CellIJ a = arterials->cell_at(r.x0, r.y0);
  const CellIJ b = arterials->cell_at(r.x1, r.y1);
  std::vector<CellIJ> out;
  for (double j = a.j; j <= b.j; j += 1)
    for (double i = a.i; i <= b.i; i += 1) out.push_back({i, j});
  return out;
}

}  // namespace svx::city
