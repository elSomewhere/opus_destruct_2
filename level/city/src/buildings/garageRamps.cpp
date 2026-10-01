// svx_city — voxel_city buildings/garageRamps.js.
#include "buildings/garageRamps.hpp"

#include <cmath>

#include "buildings/frame.hpp"
#include "core/js.hpp"
#include "core/obb.hpp"
#include "core/placement.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

namespace {

// Slab under a ramp's surface and the kerbs along its edges over it (cells).
constexpr double kSlab = 3;
constexpr double kKerb = 7;

// Material of a ramp's cell (u, v, w) of a W-cell-wide ramp: its surface, the slab under it, its
// kerbs.
int ramp_material(double W, double u, double v, double w) {
  if (v == 0 || v == W - 1) return w == kKerb - 1 ? MAT::HAZARD_YELLOW : MAT::CONCRETE_LIGHT;
  if (w >= 0) return 0;
  if (w < -1) return MAT::CONCRETE;
  return (v == 3 || v == W - 4) && (js::sar(u, 3) & 1) == 0 ? MAT::LINE_YELLOW : MAT::FLOOR_CONCRETE;
}

}  // namespace

Part ramp_part(const PartCell& cell, const Envelope& env, const Rect& q, double f, int pitch) {
  const Frame& F = envelope_frame(env);
  const XY o = local_point_to_world(F.placement, q.x1 + 1, q.y0);
  const XY d = F.dir_to_world(0, 1);
  PlacementOpts po;
  po.origin = {o[0], o[1], floor_z(env, f) + 2};
  po.yaw = nearest_yaw(d[0], d[1]);
  po.pitch = pitch;
  const Placement placement(po);
  const Yaw& T = pitches()[static_cast<size_t>(pitch)];
  // (as long as its run covers the plan's ramp: a hair into the upper deck, which owns it)
  const double Lu = std::ceil(((q.y1 - q.y0 + 1) * T.r) / T.c);
  const double W = q.x1 - q.x0 + 1;
  Part part = make_part(cell, 0, js::cat(env.id, "/ramp", f), "ramp", placement, LocalBox{0, 0, -kSlab, Lu - 1, W - 1, kKerb - 1});
  part.env = env.id;
  part.ramp = PartRamp{f, W, Lu};
  return part;
}

void rasterize_ramp_part(const World& world, const Part& part, ChunkBuffer& chunk) {
  (void)world;
  const double W = part.ramp->W;
  const double Lu = part.ramp->Lu;
  const double s = chunk.s;
  auto lo = [&](double c) { return c - chunk.half; };
  const IdxRange ri = chunk.range_x(0, Lu - 1);
  const IdxRange rj = chunk.range_y(0, W - 1);
  const IdxRange rk = chunk.range_z(-kSlab, kKerb - 1);
  for (int k = rk.lo; k <= rk.hi; ++k)
    for (int j = rj.lo; j <= rj.hi; ++j)
      for (int i = ri.lo; i <= ri.hi; ++i) {
        int m;
        if (s == 1) {
          m = ramp_material(W, chunk.wx(i), chunk.wy(j), chunk.wz(k));
        } else {
          // (the cell's lowest w within the slab or the kerbs, at its u and its edge v if it holds one)
          const double v0 = js::max(0.0, lo(chunk.wy(j)));
          const double v1 = js::min(W - 1, lo(chunk.wy(j)) + s - 1);
          const double w0 = js::max(-kSlab, lo(chunk.wz(k)));
          const double w1 = js::min(kKerb - 1, lo(chunk.wz(k)) + s - 1);
          if (v0 > v1 || w0 > w1) continue;
          const double v = v0 == 0 || v1 == W - 1 ? (v0 == 0 ? 0 : W - 1) : v0;
          m = ramp_material(W, js::max(0.0, lo(chunk.wx(i))), v, w0 < 0 ? js::max(w0, -1.0) : w0);
        }
        if (m) chunk.data[static_cast<size_t>(ChunkBuffer::index(i, j, k))] = static_cast<uint16_t>(m);
      }
}

}  // namespace svx::city
