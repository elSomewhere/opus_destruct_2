#include "svx/doom/slenderness.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace svx::doom {

namespace {

f64 percentile(std::vector<f64> v, f64 p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const f64 idx = p * static_cast<f64>(v.size() - 1);
  const size_t lo = static_cast<size_t>(std::floor(idx));
  const size_t hi = std::min(v.size() - 1, lo + 1);
  return v[lo] + (idx - static_cast<f64>(lo)) * (v[hi] - v[lo]);
}

// Structural run of column c covering z, or nullptr.
const Run* run_at(const ColumnGrid& g, i32 c, i32 z) {
  for (const Run* r = g.begin(c); r != g.end(c); ++r)
    if (r->z0 <= z && z < r->z1) return r->kind == RunKind::Structural ? r : nullptr;
  return nullptr;
}

}  // namespace

SlendernessReport wall_slenderness(const ColumnGrid& g, const Material& m, f64 h) {
  SlendernessReport rep;
  const f64 kcoef = 7.837 * m.E / (12.0 * m.rho * 9.81);
  std::vector<f64> ratio, heights;
  const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
  for (i32 y = 1; y + 1 < g.ny; ++y)
    for (i32 x = 1; x + 1 < g.nx; ++x) {
      const i32 c = g.column(x, y);
      for (const Run* r = g.begin(c); r != g.end(c); ++r) {
        if (r->kind != RunKind::Structural) continue;
        // vertical members only: runs standing on an anchored run (walls, pillars)
        const bool on_anchor = (r != g.begin(c)) && (r - 1)->kind == RunKind::Anchored && (r - 1)->z1 == r->z0;
        const i32 H = r->z1 - r->z0;
        if (!on_anchor || H < 8) continue;
        // braced if a horizontal neighbour holds a slab-like run near the top
        const i32 ztop = r->z1 - 1;
        bool braced = false;
        for (int d = 0; d < 4 && !braced; ++d) {
          const Run* nr = run_at(g, g.column(x + dx[d], y + dy[d]), ztop);
          if (nr && nr->z0 > r->z0 + H / 2) braced = true;
        }
        if (braced) {
          ++rep.braced_members;
          continue;
        }
        // thickness at mid-height: the shortest horizontal structural extent through (x, y)
        const i32 zm = (r->z0 + r->z1) / 2;
        i32 ext[2] = {1, 1};
        for (int axis = 0; axis < 2; ++axis)
          for (int sgn = -1; sgn <= 1; sgn += 2) {
            i32 xx = x, yy = y;
            for (int step = 0; step < 64; ++step) {
              xx += axis == 0 ? sgn : 0;
              yy += axis == 1 ? sgn : 0;
              if (xx < 0 || yy < 0 || xx >= g.nx || yy >= g.ny) break;
              if (!run_at(g, g.column(xx, yy), zm)) break;
              ++ext[axis];
            }
          }
        const f64 t = std::min(ext[0], ext[1]) * h;
        const f64 Hm = H * h;
        ++rep.free_members;
        // (H / H_cr)^3 with H_cr^3 = kcoef t^2: plain arithmetic, the same bits on every platform
        // (the cap sets the simulated compliance)
        ratio.push_back(Hm * Hm * Hm / (kcoef * t * t));
        heights.push_back(Hm);
      }
    }
  if (ratio.empty()) return rep;
  rep.height_p50 = percentile(heights, 0.5);
  rep.height_max = percentile(heights, 1.0);
  auto cap = [](f64 r) { return r > 0.0 ? std::floor(10.0 * 0.3 / r) / 10.0 : 1e9; };  // (0.1 steps)
  rep.max_compliance_p99 = cap(percentile(ratio, 0.99));
  rep.max_compliance_p999 = cap(percentile(ratio, 0.999));
  rep.max_compliance_all = cap(percentile(ratio, 1.0));
  return rep;
}

}  // namespace svx::doom
