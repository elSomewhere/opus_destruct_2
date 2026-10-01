// svx_city — voxel_city buildings/wings.js.
#include "buildings/wings.hpp"

#include <array>
#include <cmath>
#include <optional>
#include <vector>

#include "buildings/facade.hpp"
#include "buildings/frame.hpp"
#include "buildings/massing.hpp"
#include "city/blockPoly.hpp"
#include "city/cellNetwork.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "core/obb.hpp"
#include "network/roadLevel.hpp"
#include "network/roadSurface.hpp"
#include "network/roadView.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

namespace {

constexpr double kMargin = kEmbed + 2;
// Clearance from the kerb, the most a bay may project past a facade, and the least headroom under a
// wing (voxels).
const double kKerb = vx(1);
const double kOver = vx(1.2);
const double kHeadroom = vx(2.5);
// Roof slab over a wing's top floor, and its parapet above it (voxels).
constexpr double kRoofT = 2;
constexpr double kParapet = 3;
// Chamfer legs (the 20-21-29 triple, along the front first) and the depth (cells) of its slab: its
// facade and the stepped wall of the cut behind it.
constexpr double kChLegs[2][2] = {{20, 21}, {21, 20}};
constexpr double kChT = 4;
// Most floors of a chamfered building (the corner blocks of a street grid: their plans are checked
// with the cut).
constexpr double kChFloors = 8;

// Archetypes that may carry wings (flat facades of 2-10 floors, apartments and offices).
bool winged(const std::string& a) { return a == "walkup" || a == "midrise" || a == "office" || a == "rowhouse" || a == "townhouse"; }

// Table yaw index of the exact direction (c, s) (a triple's legs, signs as given): parallel and the
// same way, in integers; -1 for none.
int yaw_of(double c, double s) {
  const std::vector<Yaw>& Y = yaws();
  for (size_t i = 0; i < Y.size(); ++i)
    if (Y[i].c * s == Y[i].s * c && Y[i].c * c + Y[i].s * s > 0) return static_cast<int>(i);
  return -1;
}
// Relative yaws: the corner bays' (+ for the front-right corner, - for the front-left), the canted
// bays'.
const std::vector<int>& corner_r() {
  static const std::vector<int> v = {yaw_of(21, 20), yaw_of(20, 21)};
  return v;
}
const std::vector<int>& corner_l() {
  static const std::vector<int> v = {yaw_of(21, -20), yaw_of(20, -21)};
  return v;
}
const std::vector<int>& cant() {
  static const std::vector<int> v = {yaw_of(24, 7), yaw_of(24, -7), yaw_of(40, 9), yaw_of(40, -9)};
  return v;
}

inline uint16_t mat16(int m) { return m < 0 ? 0 : static_cast<uint16_t>(m); }
inline void put(ChunkBuffer& c, int i, int j, int k, int m) { c.data[static_cast<size_t>(ChunkBuffer::index(i, j, k))] = mat16(m); }

// floorZ(env, f): the floors' z, 0 .. floors, summed once (the same sums, in the same order, as
// floor_z).
class FloorZs {
 public:
  explicit FloorZs(const Envelope& env) : env_(env) {
    double acc = env.base_z;
    z_.push_back(acc);
    for (double k = 0; k < env.floors; k += 1) {
      acc += k < static_cast<double>(env.story_h.size()) ? env.story_h[static_cast<size_t>(k)] : js::kNaN;
      z_.push_back(acc);
    }
  }
  double operator()(double f) const {
    if (f >= 0 && f < static_cast<double>(z_.size()) && f == std::floor(f)) return z_[static_cast<size_t>(f)];
    return floor_z(env_, f);
  }

 private:
  const Envelope& env_;
  std::vector<double> z_;
};

double story_h(const Envelope& env, double f) {
  return f >= 0 && f < static_cast<double>(env.story_h.size()) ? env.story_h[static_cast<size_t>(f)] : js::kNaN;
}

// Is canonical point (u, v) at least m inside one rect of rects (continuous: a rect covers
// [x0, x1 + 1))? The rect, or null.
const Rect* rect_around(const std::vector<Rect>& rects, double u, double v, double m) {
  for (const Rect& r : rects)
    if (u >= r.x0 + m && u <= r.x1 + 1 - m && v >= r.y0 + m && v <= r.y1 + 1 - m) return &r;
  return nullptr;
}
bool around(const Rect& r, double u, double v, double m) { return u >= r.x0 + m && u <= r.x1 + 1 - m && v >= r.y0 + m && v <= r.y1 + 1 - m; }

// Canonical continuous point (u, v) of a building's frame F -> world point (a turned frame's
// canonical cells are its lattice's, offset).
XY canon_to_world(const Frame& F, double u, double v) { return local_point_to_world(F.placement, u + F.ou, v + F.ov); }
// World continuous point -> canonical point of a building's frame F.
XY world_to_canon(const Frame& F, double x, double y) {
  const XY uv = world_point_to_local(F.placement, x, y);
  return {uv[0] - F.ou, uv[1] - F.ov};
}

// Does a cell (u, v) of a building's canonical frame lie in one of rects?
bool in_rects(const std::vector<Rect>& rects, double u, double v) {
  for (const Rect& r : rects)
    if (u >= r.x0 && u <= r.x1 && v >= r.y0 && v <= r.y1) return true;
  return false;
}

bool overlaps(const Box3& a, const Box3& b) { return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1; }

// A wing as planned: its record, and its placement (JS keeps it on the record until planWings
// returns; its outline is canon_of).
struct Shape {
  Wing w;
  Placement placement;
};
// A point (u, v) of a shape's lattice in the building's canonical frame.
XY canon_of(const Frame& F, const Placement& p, double u, double v) {
  const XY xy = local_point_to_world(p, u, v);
  return world_to_canon(F, xy[0], xy[1]);
}

// shape()'s options: the wing's kind, its yaw relative to the building (rel) or in the world
// (wyaw), its outer face's centre Q (canonical, continuous), its width W, its floors and its
// deepest depth.
struct ShapeOpts {
  std::string kind;
  std::optional<int> rel, wyaw;
  double qu = 0, qv = 0;
  double W = 0, f0 = 0, f1 = 0;
  double max_d = vx(6);
};

// The wing a building would get of `kind`, its outer face centred on canonical point Q of the main
// frame F, at relative yaw `rel` (or world yaw `wyaw`), W cells wide: as deep as its back face needs
// to lie MARGIN inside the main footprint on every floor it spans (at most max_d); none when it
// cannot.
std::optional<Shape> shape(const Envelope& env, const Frame& F, const ShapeOpts& o) {
  // (on a building square to the grid a quarter turn of a table yaw is one; on a turned one, the
  // product of the two)
  int yaw = 0;
  int yaw2 = 0;
  if (o.wyaw) {
    yaw = *o.wyaw;
  } else if (F.placement.q >= 0) {
    yaw = (F.placement.yaw + *o.rel) % static_cast<int>(yaws().size());
  } else {
    yaw = F.placement.yaw;
    yaw2 = *o.rel;
  }
  const Yaw Y = yaw_vector(yaw, yaw2);
  // (the wing's u axis in the world, and the left end of its outer face)
  const XY q = canon_to_world(F, o.qu, o.qv);
  PlacementOpts po;
  po.origin = {js::round(q[0] - ((o.W / 2) * Y.c) / Y.r), js::round(q[1] - ((o.W / 2) * Y.s) / Y.r), 0};
  po.yaw = yaw;
  po.yaw2 = yaw2;
  const Placement placement(po);
  const double W = o.W;
  double D = 0;
  for (double d = 6; d <= o.max_d && !js::truthy(D); d += 1) {
    const XY a = canon_of(F, placement, 0, d);
    const XY b = canon_of(F, placement, W, d);
    bool ok = true;
    for (double f = o.f0; f <= o.f1 && ok; f += 1) {
      const Rect* r = rect_around(tier_rects(env, f), a[0], a[1], kMargin);
      ok = r && around(*r, b[0], b[1], kMargin);
    }
    if (ok) D = d;
  }
  if (!js::truthy(D)) return std::nullopt;
  // (its outer face out of the main mass on every floor: its middle or an end a cell clear of it)
  for (double f = o.f0; f <= o.f1; f += 1) {
    const std::vector<Rect>& rects = tier_rects(env, f);
    bool every = true;
    for (const double u : {0.0, W / 2, W}) {
      const XY c = canon_of(F, placement, u, 0);
      if (!rect_around(rects, c[0], c[1], -1)) {
        every = false;
        break;
      }
    }
    if (every) return std::nullopt;
  }
  Shape s;
  s.w.z0 = floor_z(env, o.f0);
  s.w.z1 = floor_z(env, o.f1 + 1) + kRoofT + kParapet - 1;
  s.w.bounds = placement.local_bounds_to_world_aabb(LocalBox{0, 0, s.w.z0, W - 1, D - 1, s.w.z1});
  double cx0 = js::kInf, cy0 = js::kInf, cx1 = -js::kInf, cy1 = -js::kInf;
  const double corners[4][2] = {{0, 0}, {W, 0}, {W, D}, {0, D}};
  for (const auto& p : corners) {
    const XY c = canon_of(F, placement, p[0], p[1]);
    cx0 = js::min(cx0, c[0]);
    cy0 = js::min(cy0, c[1]);
    cx1 = js::max(cx1, c[0]);
    cy1 = js::max(cy1, c[1]);
  }
  s.w.kind = o.kind;
  s.w.turn.yaw = placement.yaw;
  s.w.turn.yaw2 = placement.yaw2;
  s.w.turn.origin = {placement.origin.x, placement.origin.y};
  s.w.turn.ou = 0;
  s.w.turn.ov = 0;
  s.w.U = W;
  s.w.V = D;
  s.w.f0 = o.f0;
  s.w.f1 = o.f1;
  s.w.canon = {std::floor(cx0), std::floor(cy0), std::ceil(cx1) - 1, std::ceil(cy1) - 1};
  s.placement = placement;
  return s;
}

// The chamfer of a flat-roofed building's front corner `side` ('L', 'R'), legs `legs` times k
// (buildings/chamfer): the slab on the cut line (u along it, v in from it, its w the world's z
// from the ground floor to the roof's parapet), or none where some floor's footprint does not hold
// the corner with room to spare.
std::optional<Shape> chamfer_shape(const Envelope& env, const Frame& F, char side, const double* legs, double k) {
  const double a = legs[0] * k;
  const double b = legs[1] * k;
  const double L = 29 * k;
  const double spare = vx(3);
  for (double f = 0; f < env.floors; f += 1) {
    bool ok = false;
    for (const Rect& r : tier_rects(env, f))
      if (r.y0 == 0 && r.y1 >= b + spare && (side == 'L' ? r.x0 == 0 && r.x1 >= a + spare : r.x1 == env.U - 1 && r.x0 <= env.U - 1 - a - spare)) {
        ok = true;
        break;
      }
    if (!ok) return std::nullopt;
  }
  // (u from the side street's end of the cut to the front's on the left, from the front's to the
  // side street's on the right: v points in)
  const double ou = side == 'L' ? 0 : env.U - a;
  const double ov = side == 'L' ? b : 0;
  const double du = a;
  const double dv = side == 'L' ? -b : b;
  const XY o = canon_to_world(F, ou, ov);
  const XY dxy = F.dir_to_world(du, dv);
  const int yaw = yaw_of(dxy[0], dxy[1]);
  PlacementOpts po;
  po.origin = {o[0], o[1], 0};
  po.yaw = yaw;
  const Placement placement(po);
  Shape s;
  s.w.z0 = floor_z(env, 0);
  s.w.z1 = floor_z(env, env.floors) + kRoofT + 4;
  s.w.bounds = placement.local_bounds_to_world_aabb(LocalBox{0, 0, s.w.z0, L - 1, kChT - 1, s.w.z1});
  const XY cs[4] = {canon_of(F, placement, 0, 0), canon_of(F, placement, L, 0), canon_of(F, placement, L, kChT), canon_of(F, placement, 0, kChT)};
  s.w.kind = "chamfer";
  s.w.chamfer = Chamfer{side, a, b};
  s.w.turn.yaw = yaw;
  s.w.turn.yaw2 = 0;
  s.w.turn.origin = {o[0], o[1]};
  s.w.turn.ou = 0;
  s.w.turn.ov = 0;
  s.w.U = L;
  s.w.V = kChT;
  s.w.f0 = 0;
  s.w.f1 = env.floors - 1;
  s.w.canon = {std::floor(js::min(cs[0][0], cs[1][0], cs[2][0], cs[3][0])), std::floor(js::min(cs[0][1], cs[1][1], cs[2][1], cs[3][1])),
               std::ceil(js::max(cs[0][0], cs[1][0], cs[2][0], cs[3][0])) - 1, std::ceil(js::max(cs[0][1], cs[1][1], cs[2][1], cs[3][1])) - 1};
  s.placement = placement;
  return s;
}

// Material of a chamfer's slab cell (u along it, v in from its face) at world height z (s the
// LOD's cell size): the building's facade on its outer cells, floor by floor as long as the cut
// (the ground floor's shopfront too), wall behind; over the top floor the roof slab and its
// parapet, as the flat roof draws them on the exterior wall. -1: nothing (JS's null).
int chamfer_material(const Envelope& env, const Wing& w, const BuildingLook& look, const FloorZs& fz, double u, double v, double z, double s) {
  const double thick = js::max(2.0, s);
  const double z_roof = fz(env.floors);
  if (z >= z_roof) {
    const double zr = z - z_roof;
    if (zr < kRoofT) return look.style.wall;
    if (zr < kRoofT + 4) return look.style.wall;
    return zr == kRoofT + 4 ? MAT::PARAPET_CAP : -1;
  }
  double f = 0;
  while (f < env.floors - 1 && fz(f + 1) <= z) f += 1;
  const double zr = z - fz(f);
  if (v < thick) return facade_material(look, facade_cell(look, w.U, u, zr, story_h(env, f), f), f, zr);
  return zr < 2 ? static_cast<int>(MAT::CONCRETE) : static_cast<int>(look.style.wall);
}

// Is what a wing shows outside the main mass clear (its outer edges sampled every 4 cells, and a
// kerb's clearance out from them)? Never on a carriageway or a kerb; over a sidewalk or a plaza
// only of a street its cell `cell_id` owns; in_lot(x, y) where it stands on no street; and HEADROOM
// above the ground under it (the building's ground level or the sidewalk's, whichever is higher: a
// steep street rises along a facade).
template <class InLot>
bool clear_outside(const World& world, const RoadView& view, const Envelope& env, const Frame& F, const Shape& s, const std::string& cell_id, const InLot& in_lot) {
  const Wing& w = s.w;
  RoadSample rs = make_road_sample();
  std::vector<std::array<double, 4>> pts;
  for (double u = 0; u <= w.U; u += 4) pts.push_back({js::min(u, w.U), 0, 0, -1});
  for (double v = 0; v <= w.V; v += 4) {
    pts.push_back({0, js::min(v, w.V), -1, 0});
    pts.push_back({w.U, js::min(v, w.V), 1, 0});
  }
  std::vector<const RoadSeg*> cands;
  for (const auto& p : pts) {
    const double u = p[0], v = p[1], du = p[2], dv = p[3];
    const XY c = canon_of(F, s.placement, u, v);
    if (rect_around(tier_rects(env, w.f0), c[0], c[1], 0)) continue;
    const XY g = local_point_to_world(s.placement, u, v);
    const std::optional<RoadLevel> r = road_level_at(world, view, g[0], g[1], 16);
    if (w.z0 - js::max(env.ground_z, r ? std::ceil(r->z) + 1 : -js::kInf) < kHeadroom) return false;
    for (const double k : {0.0, kKerb}) {
      const XY xy = local_point_to_world(s.placement, u + du * k, v + dv * k);
      const double x = xy[0], y = xy[1];
      cands.clear();
      view.near(Rect{x - 2, y - 2, x + 2, y + 2}, cands);
      sample_road_surface(cands, x, y, rs, world.seed);
      if (rs.kind == RoadKind::CARRIAGE || rs.kind == RoadKind::CURB || rs.kind == RoadKind::MEDIAN || rs.kind == RoadKind::SHOULDER) return false;
      if (rs.kind != RoadKind::NONE) {
        if (rs.seg->road->cell != cell_id) return false;
      } else if (k == 0 && !in_lot(x, y)) {
        return false;
      }
    }
  }
  return true;
}

// Material of a wing's cell at local (u, v), world height z, on floor f (zr above its slab) or on
// its roof: its outer faces a facade of the building's look (the front its width long, the sides
// its depth), inside a floor slab under open rooms. -1 above its roof (JS's null).
int wing_material(const Envelope& env, const Wing& w, const BuildingLook& look, const FloorZs& fz, double u, double v, double z, double s, bool snowy) {
  const double thick = js::max(2.0, s);
  const double z_roof = fz(w.f1 + 1);
  const bool wall = u < thick || u >= w.U - thick || v < thick;
  if (z >= z_roof) {
    const double zz = z - z_roof;
    if (zz < kRoofT) return wall ? look.style.wall : snowy && zz > kRoofT - 1 - s ? static_cast<int>(MAT::SNOW) : look.style.roof;
    if (!wall || zz >= kRoofT + kParapet) return -1;
    return zz == kRoofT + kParapet - 1 ? static_cast<int>(MAT::PARAPET_CAP) : look.style.wall;
  }
  double f = w.f0;
  while (f < w.f1 && fz(f + 1) <= z) f += 1;
  const double zr = z - fz(f);
  if (wall) {
    double len, t;
    if (v < thick) {
      len = w.U;
      t = u;
    } else if (u < thick) {
      len = w.V;
      t = w.V - 1 - v;
    } else {
      len = w.V;
      t = v;
    }
    return facade_material(look, facade_cell(look, len, t, zr, story_h(env, f), f), f, zr);
  }
  if (zr == 0) return MAT::CONCRETE;
  if (zr == 1) return env.program.upper == "office" ? MAT::FLOOR_CARPET_GRAY : MAT::FLOOR_OAK;
  return 0;
}

// A chamfer in the world grid (grid mode, after its building): every cell of its slab, over the
// stepped wall of the cut (it owns what they share); at a coarse LOD a slab as deep as two of its
// cells, so the facade covers the cut's edge however the coarse columns fall.
void rasterize_chamfer(const World& world, const Envelope& env, const Wing& w, ChunkBuffer& chunk) {
  const Box3& b = w.bounds;
  const Box3 box = chunk.world_box();
  const Frame wf = turned_frame(w.turn, w.U, w.V);
  const BuildingLook& look = building_look(env, world.seed);
  const double s = chunk.s;
  const double depth = js::max(w.V, 2 * s);
  const IdxRange ri = chunk.range_x(js::max(b.x0 - 2 * s, box.x0), js::min(b.x1 + 2 * s, box.x1));
  const IdxRange rj = chunk.range_y(js::max(b.y0 - 2 * s, box.y0), js::min(b.y1 + 2 * s, box.y1));
  const IdxRange rk = chunk.range_z(w.z0, w.z1);
  const FloorZs fz(env);
  for (int j = rj.lo; j <= rj.hi; ++j) {
    const double y = chunk.wy(j);
    for (int i = ri.lo; i <= ri.hi; ++i) {
      const XY uv = wf.from_world(chunk.wx(i), y);
      const double u = uv[0], v = uv[1];
      if (u < 0 || u >= w.U || v < 0 || v >= depth) continue;
      for (int k = rk.lo; k <= rk.hi; ++k) {
        const int m = chamfer_material(env, w, look, fz, u, v, chunk.wz(k), s);
        if (m >= 0) put(chunk, i, j, k, m);
      }
    }
  }
}

// The cells a coarse cell of a part's lattice probes: its centre, or (s > 1) its centre and its
// corners.
std::vector<std::array<double, 2>> probes_of(const ChunkBuffer& chunk) {
  const double s = chunk.s;
  const double h = chunk.half;
  if (s == 1) return {{0, 0}};
  return {{0, 0}, {-h, -h}, {s - 1 - h, -h}, {-h, s - 1 - h}, {s - 1 - h, s - 1 - h}};
}

// A chamfer's slab in its own lattice (parts mode): a coarse cell is its where its centre or a
// corner lies in the slab.
void rasterize_chamfer_part(const World& world, const Envelope& env, const Wing& w, ChunkBuffer& chunk) {
  const BuildingLook& look = building_look(env, world.seed);
  const double s = chunk.s;
  const IdxRange ri = chunk.range_x(-s + 1, w.U - 1 + s - 1);
  const IdxRange rj = chunk.range_y(-s + 1, w.V - 1 + s - 1);
  const IdxRange rk = chunk.range_z(w.z0, w.z1);
  if (ri.empty() || rj.empty() || rk.empty()) return;
  const std::vector<std::array<double, 2>> probes = probes_of(chunk);
  const FloorZs fz(env);
  for (int j = rj.lo; j <= rj.hi; ++j)
    for (int i = ri.lo; i <= ri.hi; ++i) {
      double u = -1;
      double v = -1;
      for (const auto& p : probes) {
        const double pu = chunk.wx(i) + p[0];
        const double pv = chunk.wy(j) + p[1];
        if (pu < 0 || pu >= w.U || pv < 0 || pv >= w.V) continue;
        u = pu;
        v = pv;
        break;
      }
      if (u < 0) continue;
      for (int k = rk.lo; k <= rk.hi; ++k) {
        const int m = chamfer_material(env, w, look, fz, u, v, chunk.wz(k), s);
        if (m > 0) put(chunk, i, j, k, m);
      }
    }
}

}  // namespace

std::vector<Wing> plan_wings(const World& world, const Envelope& env, const Lot* lot, const Block* block, const RoadView& view, const std::string& cell_id, Rng& rng) {
  if (!winged(env.archetype) || !lot) return {};
  // floorsOf: 1 up to the top floor (one less under a pitched roof), or none
  const double fl1 = env.floors - 1 - (env.roof.type == "flat" ? 0 : 1);
  if (!(fl1 >= 1)) return {};
  const double f0 = 1;
  const double f1 = fl1;
  const Frame& F = envelope_frame(env);
  const BuildingLook& look = building_look(env, world.seed);
  // (where a wing may stand off every street: the lot, its slanted corner included; JS reads
  // `lot.whole ?? lot.rect`, and `whole: true` has no x0: no point is in it)
  const Rect* whole = lot->whole_rect ? &*lot->whole_rect : lot->whole ? nullptr : &lot->rect;
  static const std::vector<BlockCut> kNoCuts;
  const std::vector<BlockCut>& cuts = block ? block->cuts : kNoCuts;
  // (a voxel past a slanted property line is the street's sidewalk, whose owner is checked)
  auto in_lot = [&](double x, double y) {
    if (!whole) return false;
    if (!(x >= whole->x0 && x <= whole->x1 + 1 && y >= whole->y0 && y <= whole->y1 + 1)) return false;
    for (const BlockCut& k : cuts)
      if (!(k.nx * x + k.ny * y >= k.c - 1)) return false;
    return true;
  };
  // (a turned building fronts its slanted street)
  bool street_f = false, street_l = false, street_r = false;
  if (env.turn) {
    street_f = true;
  } else {
    for (const Frontage& fr : lot->frontages) {
      if (fr.cls.empty() || fr.cls == "alley") continue;
      const char cs = F.canon_side(fr.side);
      if (cs == 'F') street_f = true;
      if (cs == 'L') street_l = true;
      if (cs == 'R') street_r = true;
    }
  }
  std::vector<Shape> out;
  auto try_add = [&](std::optional<Shape> w) {
    if (!w || out.size() >= 2 || !clear_outside(world, view, env, F, *w, cell_id, in_lot)) return false;
    for (const Shape& o : out)
      if (overlaps(o.w.bounds, w->w.bounds)) return false;
    out.push_back(std::move(*w));
    return true;
  };
  // a corner bay where the front meets a side street, the footprint square to both
  std::vector<char> corners;
  if (street_f && street_l) corners.push_back('L');
  if (street_f && street_r) corners.push_back('R');
  if (!corners.empty() && rng.chance(0.55)) {
    const char side = rng.pick(corners);
    // a flat-roofed corner block square to the grid (8 floors at most) cuts the corner off instead
    // half of the time, where a chamfer fits
    bool cut = false;
    if (!env.turn && env.roof.type == "flat" && env.floors <= kChFloors && rng.chance(0.5)) {
      const double* legs = kChLegs[rng.pick_index(2)];
      for (double k = js::min(3.0, std::floor(js::min(env.U / 3, env.V / 2) / 21)); k >= 2 && !cut; k -= 1) cut = try_add(chamfer_shape(env, F, side, legs, k));
    }
    if (!cut) {
      const int rel = rng.pick(side == 'L' ? corner_l() : corner_r());
      const Yaw& Y = yaws()[static_cast<size_t>(rel)];
      // (the corner point, out along the bay's normal 0-2 cells)
      const double c0 = side == 'L' ? 0 : env.U;
      const double c1 = 0;
      const double p = rng.int_(0, 2);
      const double W = 2 * rng.int_(vx(1.25), vx(1.6));
      ShapeOpts o;
      o.kind = "corner";
      o.rel = rel;
      o.qu = c0 + (p * Y.s) / Y.r;
      o.qv = c1 - (p * Y.c) / Y.r;
      o.W = W;
      o.f0 = f0;
      o.f1 = f1;
      try_add(shape(env, F, o));
    }
  }
  // a wing to the slanted street a lot was cut back by
  const Frontage* sl = nullptr;
  for (const Frontage& fr : lot->frontages)
    if (fr.slanted && !fr.cls.empty() && fr.cls != "alley") {
      sl = &fr;
      break;
    }
  const BlockCut* cut = nullptr;
  if (sl)
    for (const BlockCut& k : cuts)
      if (k.id == sl->slant) {
        cut = &k;
        break;
      }
  if (cut && !env.turn && rng.chance(0.75)) {
    const int wyaw = nearest_yaw(cut->ny, -cut->nx);
    const Yaw& Y = yaws()[static_cast<size_t>(wyaw)];
    // the footprint corner nearest the street, on the street's line a little inside the lot
    bool has_apex = false;
    double ax = 0, ay = 0, ad = 0;
    for (const Rect& r : tier_rects(env, f0)) {
      const double cs[4][2] = {{r.x0, r.y0}, {r.x1 + 1, r.y0}, {r.x1 + 1, r.y1 + 1}, {r.x0, r.y1 + 1}};
      for (const auto& p : cs) {
        const XY xy = canon_to_world(F, p[0], p[1]);
        const double d = cut->nx * xy[0] + cut->ny * xy[1] - cut->c;
        if (!has_apex || d < ad) {
          has_apex = true;
          ax = xy[0];
          ay = xy[1];
          ad = d;
        }
      }
    }
    // (its outer face square to the street: the table yaw is the street's own)
    const bool square = std::fabs(Y.c / Y.r - cut->ny) < 1e-6 && std::fabs(Y.s / Y.r + cut->nx) < 1e-6;
    if (square && has_apex && ad >= 0) {
      // (its outer face on the line c + 2, or through the corner where that stands closer, centred
      // on the corner or slid along the street into the wedge between it and a facade)
      const double g = js::max(0.0, ad - 2);
      const double fx = ax - cut->nx * g;
      const double fy = ay - cut->ny * g;
      bool done = false;
      for (double W = vx(10); W >= vx(4) && !done; W -= vx(1))
        for (const double t : {0.0, W / 2, -W / 2}) {
          const XY q = world_to_canon(F, fx + (t * Y.c) / Y.r, fy + (t * Y.s) / Y.r);
          ShapeOpts o;
          o.kind = "wing";
          o.wyaw = wyaw;
          o.qu = q[0];
          o.qv = q[1];
          o.W = W;
          o.f0 = f0;
          o.f1 = f1;
          o.max_d = vx(14);
          if (try_add(shape(env, F, o))) {
            done = true;
            break;
          }
        }
    }
  }
  // a canted bay over one window bay of the front
  if (street_f && rng.chance(0.3)) {
    const double bay = look.bay;
    const double nb = std::floor(env.U / bay);
    if (nb >= 4) {
      const double margin = std::floor((env.U - nb * bay) / 2);
      const double k = rng.int_(1, nb - 2);
      const int rel = rng.pick(cant());
      const Yaw& Y = yaws()[static_cast<size_t>(rel)];
      const double W = bay + 4;
      const double sn = std::fabs(Y.s / Y.r);
      const double p0 = 2 + (W / 2) * sn;
      if (p0 + (W / 2) * sn <= kOver) {
        ShapeOpts o;
        o.kind = "bay";
        o.rel = rel;
        o.qu = margin + (k + 0.5) * bay + (p0 * Y.s) / Y.r;
        o.qv = -(p0 * Y.c) / Y.r;
        o.W = W;
        o.f0 = f0;
        o.f1 = f1;
        try_add(shape(env, F, o));
      }
    }
  }
  std::vector<Wing> wings;
  wings.reserve(out.size());
  for (Shape& s : out) wings.push_back(std::move(s.w));
  return wings;
}

Placement wing_placement(const Wing& w) {
  PlacementOpts o;
  o.origin = {w.turn.origin.x, w.turn.origin.y, 0};
  o.yaw = w.turn.yaw;
  o.yaw2 = w.turn.yaw2;
  return Placement(o);
}

void rasterize_wing(const World& world, const Envelope& env, const Wing& w, ChunkBuffer& chunk) {
  const Box3& b = w.bounds;
  const Box3 box = chunk.world_box();
  if (b.x1 < box.x0 || b.x0 > box.x1 || b.y1 < box.y0 || b.y0 > box.y1 || w.z1 < box.z0 || w.z0 > box.z1) return;
  if (w.kind == "chamfer") {
    rasterize_chamfer(world, env, w, chunk);
    return;
  }
  // (wingFrame: its box's cells (u, v) in the world)
  const Frame wf = turned_frame(w.turn, w.U, w.V);
  const Frame& F = envelope_frame(env);
  const BuildingLook& look = building_look(env, world.seed);
  const double snow = roof_snow_cover(world, env);
  const IdxRange ri = chunk.range_x(js::max(b.x0, box.x0), js::min(b.x1, box.x1));
  const IdxRange rj = chunk.range_y(js::max(b.y0, box.y0), js::min(b.y1, box.y1));
  const IdxRange rk = chunk.range_z(w.z0, w.z1);
  const FloorZs fz(env);
  const double z_roof = fz(w.f1 + 1);
  for (int j = rj.lo; j <= rj.hi; ++j) {
    const double y = chunk.wy(j);
    for (int i = ri.lo; i <= ri.hi; ++i) {
      const double x = chunk.wx(i);
      const XY uv = wf.from_world(x, y);
      const double u = uv[0], v = uv[1];
      if (u < 0 || u >= w.U || v < 0 || v >= w.V) continue;
      const XY m = F.from_world(x, y);
      const bool snowy = js::truthy(snow) && snow_at(world.seed, snow, x, y);
      for (int k = rk.lo; k <= rk.hi; ++k) {
        const double z = chunk.wz(k);
        // (the main mass owns its footprint, floor by floor; its top floor's under the wing's roof)
        double f = w.f0;
        while (f < w.f1 && fz(f + 1) <= z) f += 1;
        if (in_rects(tier_rects(env, z >= z_roof ? js::min(w.f1 + 1, env.floors - 1) : f), m[0], m[1]) || (z >= z_roof && in_rects(tier_rects(env, w.f1), m[0], m[1])))
          continue;
        const int mat = wing_material(env, w, look, fz, u, v, z, chunk.s, snowy);
        if (mat >= 0) put(chunk, i, j, k, mat);
      }
    }
  }
}

void rasterize_wing_part(const World& world, const Envelope& env, const Wing& w, ChunkBuffer& chunk) {
  if (w.kind == "chamfer") {
    rasterize_chamfer_part(world, env, w, chunk);
    return;
  }
  const Frame& F = envelope_frame(env);
  const Placement pl = wing_placement(w);
  const BuildingLook& look = building_look(env, world.seed);
  const double snow = roof_snow_cover(world, env);
  const IdxRange ri = chunk.range_x(-chunk.s + 1, w.U - 1 + chunk.s - 1);
  const IdxRange rj = chunk.range_y(-chunk.s + 1, w.V - 1 + chunk.s - 1);
  const IdxRange rk = chunk.range_z(w.z0, w.z1);
  if (ri.empty() || rj.empty() || rk.empty()) return;
  const FloorZs fz(env);
  const double z_roof = fz(w.f1 + 1);
  // (a coarse cell is the wing's where any of its corners or its centre shows: a bay survives
  // every LOD)
  const double s = chunk.s;
  const std::vector<std::array<double, 2>> probes = probes_of(chunk);
  const std::vector<Rect>& base = tier_rects(env, w.f0);
  for (int j = rj.lo; j <= rj.hi; ++j) {
    for (int i = ri.lo; i <= ri.hi; ++i) {
      double u = -1;
      double v = -1;
      for (const auto& p : probes) {
        const double pu = chunk.wx(i) + p[0];
        const double pv = chunk.wy(j) + p[1];
        if (pu < 0 || pu >= w.U || pv < 0 || pv >= w.V) continue;
        const XY pxy = local_point_to_world(pl, pu + 0.5, pv + 0.5);
        const XY q = world_to_canon(F, pxy[0], pxy[1]);
        if (s > 1 && rect_around(base, q[0], q[1], 0)) continue;
        u = pu;
        v = pv;
        break;
      }
      if (u < 0) continue;
      const XY xy = local_point_to_world(pl, u + 0.5, v + 0.5);
      const XY m = world_to_canon(F, xy[0], xy[1]);
      const bool snowy = js::truthy(snow) && snow_at(world.seed, snow, std::floor(xy[0]), std::floor(xy[1]));
      for (int k = rk.lo; k <= rk.hi; ++k) {
        const double z = chunk.wz(k);
        double f = w.f0;
        while (f < w.f1 && fz(f + 1) <= z) f += 1;
        const std::vector<Rect>& rects = tier_rects(env, z >= z_roof ? js::min(w.f1 + 1, env.floors - 1) : f);
        int mat;
        if (rect_around(rects, m[0], m[1], 0))
          mat = rect_around(rects, m[0], m[1], kEmbed) ? -1 : static_cast<int>(look.style.wall);
        else
          mat = wing_material(env, w, look, fz, u, v, z, chunk.s, snowy);
        if (mat > 0) put(chunk, i, j, k, mat);
      }
    }
  }
}

}  // namespace svx::city
