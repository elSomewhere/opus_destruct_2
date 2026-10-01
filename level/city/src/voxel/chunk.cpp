// svx_city — voxel_city voxel/chunk.js.
#include "voxel/chunk.hpp"

#include <algorithm>

namespace svx::city {

ChunkBuffer::ChunkBuffer(int lod_, double cx_, double cy_, double cz_)
    : lod(lod_),
      cx(cx_),
      cy(cy_),
      cz(cz_),
      s(static_cast<double>(1 << lod_)),
      half(static_cast<double>((1 << lod_) >> 1)),
      bx((cx_ * kChunk - 1) * s),
      by((cy_ * kChunk - 1) * s),
      bz((cz_ * kChunk - 1) * s),
      data(kP3, 0) {}

void ChunkBuffer::fill_box(double x0, double y0, double z0, double x1, double y1, double z1, uint16_t m, int mode) {
  const IdxRange ri = range_x(x0, x1);
  if (ri.lo > ri.hi) return;
  const IdxRange rj = range_y(y0, y1);
  if (rj.lo > rj.hi) return;
  const IdxRange rk = range_z(z0, z1);
  if (rk.lo > rk.hi) return;
  uint16_t* d = data.data();
  if (isolating && !iso.empty()) {
    uint16_t* is = iso.data();
    uint32_t* ob = obj.empty() ? nullptr : obj.data();
    for (int k = rk.lo; k <= rk.hi; ++k)
      for (int j = rj.lo; j <= rj.hi; ++j)
        for (int i = ri.lo, idx = index(ri.lo, j, k); i <= ri.hi; ++i, ++idx)
          if (mode == 0 || (mode == 1 && d[idx] == 0) || (mode == 2 && d[idx] != 0)) {
            d[idx] = m;
            is[idx] = m;
            if (ob) ob[idx] = object;
          }
    return;
  }
  const int n = ri.hi - ri.lo + 1;
  for (int k = rk.lo; k <= rk.hi; ++k)
    for (int j = rj.lo; j <= rj.hi; ++j) {
      uint16_t* row = d + index(ri.lo, j, k);
      if (mode == 0) {
        std::fill(row, row + n, m);
      } else if (mode == 1) {
        for (int i = 0; i < n; ++i)
          if (row[i] == 0) row[i] = m;
      } else {
        for (int i = 0; i < n; ++i)
          if (row[i] != 0) row[i] = m;
      }
    }
}

int ChunkBuffer::count_non_air() {
  int n = 0;
  for (const uint16_t v : data)
    if (v != 0) ++n;
  non_air = n;
  return n;
}

}  // namespace svx::city
