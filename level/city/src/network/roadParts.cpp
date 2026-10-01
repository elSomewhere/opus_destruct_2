// svx_city — network/roadParts.hpp (voxel_city network/roadParts.js).
#include "network/roadParts.hpp"

#include "core/js.hpp"
#include "core/placement.hpp"
#include "network/roadLevel.hpp"
#include "network/roadSurface.hpp"
#include "voxel/chunk.hpp"
#include "voxel/materials.hpp"
#include "world/World.hpp"

namespace svx::city {

namespace {

// Grade samples along a segment (voxels), and how far one may stray from its table grade.
constexpr double kSample = 8;
constexpr double kTol = 0.0015;
// Shortest run worth a lattice (voxels): 16 m.
constexpr double kMinRun = 128;
// Room above the slab (curbs, markings), cells.
constexpr double kAbove = 3;

// The signed pitch index whose grade is within TOL of g (0 when none).
int pitch_of(double g) {
  const std::vector<Yaw>& P = pitches();
  for (size_t k = 1; k < P.size(); ++k) {
    const double q = P[k].s / P[k].c;
    if (js::abs(js::abs(g) - q) <= kTol) return g > 0 ? static_cast<int>(k) : -static_cast<int>(k);
  }
  return 0;
}

}  // namespace

std::vector<PitchedRun> pitched_runs(const World& world, const RoadSeg& seg) {
  const int yaw = nearest_yaw(seg.dx, seg.dy);
  const Yaw& Y = yaws()[static_cast<size_t>(yaw)];
  if (js::abs(Y.c / Y.r - seg.dx) > 1e-9 || js::abs(Y.s / Y.r - seg.dy) > 1e-9) return {};
  std::vector<PitchedRun> out;
  const int n = static_cast<int>(std::floor(seg.len / kSample));
  std::vector<double> z;
  for (int k = 0; k <= n; ++k) z.push_back(segment_level(world, seg, k * kSample));
  int k0 = -1;
  int p0 = 0;
  auto close = [&](int k1) {
    if (k0 < 0 || !p0) return;
    const double a0 = k0 * kSample;
    const double a1 = k1 * kSample;
    if (a1 - a0 < kMinRun) return;
    // (on the line through its ends, a quarter voxel at most)
    const double g = (z[static_cast<size_t>(k1)] - z[static_cast<size_t>(k0)]) / (a1 - a0);
    for (int k = k0; k <= k1; ++k)
      if (js::abs(z[static_cast<size_t>(k)] - (z[static_cast<size_t>(k0)] + g * (k - k0) * kSample)) > 0.25) return;
    // (on dry land: a bridge or a causeway, its centre line over the water, keeps its deck)
    for (int k = k0; k <= k1; ++k)
      if (world.is_wet(seg.ax + seg.dx * k * kSample, seg.ay + seg.dy * k * kSample, 0)) return;
    out.push_back({a0, a1, p0, yaw});
  };
  for (int k = 0; k < n; ++k) {
    const int p = pitch_of((z[static_cast<size_t>(k + 1)] - z[static_cast<size_t>(k)]) / kSample);
    if (p && p == p0 && k0 >= 0) continue;
    close(k);
    k0 = p ? k : -1;
    p0 = p;
  }
  close(n);
  return out;
}

std::vector<Part> road_run_parts(const World& world, const Road& road, const std::vector<const RoadSeg*>& segs, const PartCell& cell) {
  std::vector<Part> out;
  for (const RoadSeg* sp : segs) {
    const RoadSeg& seg = *sp;
    for (const PitchedRun& run : pitched_runs(world, seg)) {
      const Yaw& P = pitches()[static_cast<size_t>(run.pitch < 0 ? -run.pitch : run.pitch)];
      const double cos = P.c / P.r;
      // the lattice: u along the street (up the slope), v across it, w up from the slab's foot
      const double W = 2 * std::ceil(seg.hr);
      const double x0 = seg.ax + seg.dx * run.a0;
      const double y0 = seg.ay + seg.dy * run.a0;
      // (the surface over the centre line at the run's start: the world grid's road top there)
      const double s0z = segment_level(world, seg, run.a0) + 1;
      PlacementOpts po;
      po.yaw = run.yaw;
      po.pitch = run.pitch;
      const Placement probe(po);
      const std::array<double, 9>& m = probe.m;
      const double d = probe.d;
      const double u = 0, v = W / 2, w = kRoadSlab;
      const double ox = (m[0] * u + m[1] * v + m[2] * w) / d;
      const double oy = (m[3] * u + m[4] * v + m[5] * w) / d;
      const double oz = (m[6] * u + m[7] * v + m[8] * w) / d;
      PlacementOpts lo;
      lo.origin = {js::round(x0 - ox), js::round(y0 - oy), js::round(s0z - oz)};
      lo.yaw = run.yaw;
      lo.pitch = run.pitch;
      lo.anchored = true;
      const Placement placement(lo);
      // pieces: the fewest equal ones within reach (8 m at least)
      const double L = (run.a1 - run.a0) / cos;
      auto piece = [&](double n, double k) {
        const double u0 = std::floor((L * k) / n);
        const double u1 = std::floor((L * (k + 1)) / n) - 1;
        // (the run's lattice, a placement of its own: its priority is the piece's)
        PlacementOpts po2;
        po2.origin = placement.origin;
        po2.yaw = run.yaw;
        po2.pitch = run.pitch;
        po2.anchored = true;
        const Placement pl(po2);
        LocalBox extent;
        extent.u0 = u0;
        extent.v0 = 0;
        extent.w0 = 0;
        extent.u1 = u1;
        extent.v1 = W - 1;
        extent.w1 = kRoadSlab + kAbove;
        Part part = make_part(cell, 0, js::cat(road.id, "/r", seg.idx, ".", run.a0, ".", k), "road", pl, extent, true);
        part.road = road.id;
        part.seg = seg.idx;
        part.s0 = seg.s0 + run.a0 + u0 * cos;
        part.s1 = seg.s0 + run.a0 + (u1 + 1) * cos;
        part.grade = (js::sign(run.pitch) * P.s) / P.c;
        part.hr = seg.hr;
        return part;
      };
      std::vector<Part> best;
      double best_r = 0;
      bool have = false;
      const double n_max = js::max(1.0, std::floor(L / 64));
      for (double n = 1; n <= n_max; n += 1) {
        std::vector<Part> ps;
        for (double k = 0; k < n; k += 1) ps.push_back(piece(n, k));
        double r = -js::kInf;
        for (const Part& p : ps) r = js::max(r, p.reach);
        if (!have || r < best_r) {
          best = std::move(ps);
          best_r = r;
          have = true;
        }
        if (r <= 4) break;
      }
      for (Part& p : best) out.push_back(std::move(p));
    }
  }
  return out;
}

void rasterize_road_part(const World& world, const Part& part, ChunkBuffer& chunk) {
  const LocalBox& e = part.extent;
  const Placement& pl = part.placement;
  const IdxRange ri = chunk.range_x(e.u0, e.u1);
  const IdxRange rj = chunk.range_y(e.v0, e.v1);
  // (every cell a voxel of the slab lies in, at any LOD)
  const int k1 = chunk.range_z(e.w0, e.w1 + chunk.s - 1).hi;
  const int k0 = chunk.range_z(e.w0 - chunk.s + 1, e.w1).lo;
  if (ri.lo > ri.hi || rj.lo > rj.hi || k0 > k1) return;
  RoadSample rs = make_road_sample();
  const std::array<double, 9>& m = pl.m;
  const double d = pl.d;
  std::vector<const RoadSeg*> cands;
  for (int j = rj.lo; j <= rj.hi; ++j) {
    const double v = chunk.wy(j) + 0.5;
    for (int i = ri.lo; i <= ri.hi; ++i) {
      const double u = chunk.wx(i) + 0.5;
      // the column's centre on the surface, in the world
      const double x = pl.origin.x + (m[0] * u + m[1] * v + m[2] * kRoadSlab) / d;
      const double y = pl.origin.y + (m[3] * u + m[4] * v + m[5] * kRoadSlab) / d;
      const CellIJ c = world.cell_at(x, y);
      const std::shared_ptr<const RoadView> view = world.road_view(c.i, c.j);
      cands.clear();
      view->near(Rect{x - 2, y - 2, x + 2, y + 2}, cands);
      sample_road_surface(cands, x, y, rs, world.seed);
      if (rs.kind == RoadKind::NONE) continue;
      const double top = kRoadSlab - 1 + rs.dz;
      const uint16_t sub = rs.kind == RoadKind::CARRIAGE ? static_cast<uint16_t>(MAT::GRAVEL) : static_cast<uint16_t>(MAT::CONCRETE);
      // (a coarse cell holds the slab where any of its voxels does: a thin slab survives every LOD)
      const double lo = chunk.half;
      for (int k = k0; k <= k1; ++k) {
        const double w = chunk.wz(k) - lo;
        if (w > top) break;
        chunk.data[static_cast<size_t>(i + j * kP + k * kP2)] = w + chunk.s > top ? rs.mat : sub;
      }
    }
  }
}

double slab_foot(const Part& part, double x, double y) {
  const std::array<double, 9>& m = part.placement.m;
  const double d = part.placement.d;
  const XYZ& origin = part.placement.origin;
  // (u, v) of the plane's point over (x, y): the 2 x 2 system of the placement's x, y rows at w = 0
  const double a = m[0];
  const double b = m[1];
  const double c = m[3];
  const double e = m[4];
  const double det = a * e - b * c;
  const double px = (x - origin.x) * d;
  const double py = (y - origin.y) * d;
  const double u = (e * px - b * py) / det;
  const double v = (a * py - c * px) / det;
  return origin.z + (m[6] * u + m[7] * v) / d;
}

}  // namespace svx::city
