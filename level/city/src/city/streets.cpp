// svx_city — city/streets.hpp (voxel_city city/streets.js).
#include "city/streets.hpp"

#include <array>
#include <optional>
#include <utility>
#include <vector>

#include "core/js.hpp"
#include "core/math.hpp"
#include "core/placement.hpp"

namespace svx::city {

namespace {

constexpr double kSnap = 8;  // 1 m

double snap8(double v) { return js::round(v / kSnap) * kSnap; }

std::string pick_class(const District& district, Rng& rng) {
  const DistrictStreets& st = district.streets;
  return rng.chance(st.pedestrian_chance) ? std::string("pedestrian") : st.local_class;
}

// (streets.jog ?? 0.3: no registered district sets jog)
constexpr double kJog = 0.3;

// gridStreets' split: positions a0 .. a0 + len in n parts, each jittered by up to 15% and snapped;
// again without a minimum width when a part came out narrower than minW.
std::vector<double> grid_split(Rng& rng, double a0, double len, int n, double min_w) {
  std::vector<double> out{a0};
  for (int k = 1; k < n; ++k) out.push_back(snap8(a0 + (len * k) / n + (rng.next() - 0.5) * 0.3 * (len / n)));
  out.push_back(a0 + len);
  for (size_t k = 1; k < out.size(); ++k)
    if (out[k] - out[k - 1] < min_w) return grid_split(rng, a0, len, n, 0);
  return out;
}

const District& local_or(StreetEmit& emit, const Rect& rect, const District& district) {
  // (emit.local && emit.local(rect)) || district
  const District* d = emit.local ? emit.local(rect) : nullptr;
  return d ? *d : district;
}

// ---- subdivide

struct Subdivide {
  StreetEmit& emit;
  Rng& rng;
  const District& district;
  double long_t, short_t;

  void recurse(const Rect& rect, const BlockSides& sides, int depth) {
    const double w = rect.x1 - rect.x0;
    const double h = rect.y1 - rect.y0;
    const bool long_is_x = w >= h;
    const double long_dim = long_is_x ? w : h;
    const double short_dim = long_is_x ? h : w;
    char axis = 0;
    if (short_dim > short_t * 1.45)
      axis = long_is_x ? 'y' : 'x';
    else if (long_dim > long_t * 1.3)
      axis = long_is_x ? 'x' : 'y';
    if (!axis || depth > 12) {
      emit.block(rect, sides, nullptr);
      return;
    }
    const double dim = axis == 'x' ? w : h;
    const double target = axis == (long_is_x ? 'x' : 'y') ? long_t : short_t;
    const double parts = js::max(2.0, js::round(dim / target));
    // cut near a multiple of the target so blocks stay near target size
    const double k = rng.int_(1, parts - 1);
    double at = (dim * k) / parts + (rng.next() - 0.5) * vx(10);
    at = snap8((axis == 'x' ? rect.x0 : rect.y0) + at);
    const std::string cls = pick_class(district, rng);
    if (axis == 'x') {
      const RoadSide road = emit.road(cls, at, rect.y0, at, rect.y1, nullptr);
      Rect a = rect;
      a.x1 = at;
      BlockSides sa = sides;
      sa.E = road;
      recurse(a, sa, depth + 1);
      Rect b = rect;
      b.x0 = at;
      BlockSides sb = sides;
      sb.W = road;
      recurse(b, sb, depth + 1);
    } else {
      const RoadSide road = emit.road(cls, rect.x0, at, rect.x1, at, nullptr);
      Rect a = rect;
      a.y1 = at;
      BlockSides sa = sides;
      sa.S = road;
      recurse(a, sa, depth + 1);
      Rect b = rect;
      b.y0 = at;
      BlockSides sb = sides;
      sb.N = road;
      recurse(b, sb, depth + 1);
    }
  }
};

// ---- organic (axis-aligned)

struct Organic {
  StreetEmit& emit;
  Rng& rng;
  const District& district;

  void recurse(const Rect& rect, const BlockSides& sides, int depth) {
    // adaptive: each part asks for the district at its own centre (emit.local), so blocks grow
    // small in the old core and large by the works
    const District& d = local_or(emit, rect, district);
    const DistrictStreets& st = d.streets;
    if (pattern_family(st.pattern) == "none") {
      emit.block(rect, sides, nullptr);
      return;
    }
    const std::array<double, 2>& long_r = st.block[0];
    const std::array<double, 2>& short_r = st.block[1];
    const double lane_chance = st.lane_chance.value_or(0);
    const std::string lane_cls = st.lane_class.empty() ? std::string("alley") : st.lane_class;
    const double min_part = vx(short_r[0] * 0.7);
    const double w = rect.x1 - rect.x0;
    const double h = rect.y1 - rect.y0;
    const bool long_is_x = w >= h;
    const double long_dim = long_is_x ? w : h;
    const double short_dim = long_is_x ? h : w;
    const double long_t = vx(rng.float_(long_r[0], long_r[1]));
    const double short_t = vx(rng.float_(short_r[0], short_r[1]));
    char axis = 0;
    if (long_dim > long_t * 1.2)
      axis = long_is_x ? 'x' : 'y';
    else if (short_dim > short_t * 1.55)
      axis = long_is_x ? 'y' : 'x';
    const double dim = axis == 'x' ? w : h;
    if (!axis || depth > 14 || dim < 2 * min_part) {
      emit.block(rect, sides, nullptr);
      return;
    }
    const double at0 = js::max(min_part, js::min(dim - min_part, dim * rng.float_(0.3, 0.7)));
    const double at = snap8((axis == 'x' ? rect.x0 : rect.y0) + at0);
    // a lane only between blocks that are small enough to be walked round
    const std::string cls = depth > 0 && dim < vx(long_r[1] * 2.2) && rng.chance(lane_chance) ? lane_cls : pick_class(d, rng);
    const std::string paving = st.paving;  // (?? null)
    if (axis == 'x') {
      const RoadSide road = emit.road(cls, at, rect.y0, at, rect.y1, &paving);
      Rect a = rect;
      a.x1 = at;
      BlockSides sa = sides;
      sa.E = road;
      recurse(a, sa, depth + 1);
      Rect b = rect;
      b.x0 = at;
      BlockSides sb = sides;
      sb.W = road;
      recurse(b, sb, depth + 1);
    } else {
      const RoadSide road = emit.road(cls, rect.x0, at, rect.x1, at, &paving);
      Rect a = rect;
      a.y1 = at;
      BlockSides sa = sides;
      sa.S = road;
      recurse(a, sa, depth + 1);
      Rect b = rect;
      b.y0 = at;
      BlockSides sb = sides;
      sb.N = road;
      recurse(b, sb, depth + 1);
    }
  }
};

// ---- organic (the angled world)

// Tilts of old-town cuts: the exact yaws of 8.8 to 14.3 degrees (core/placement).
constexpr int kTilts[4] = {1, 2, 3, 4};

// A block of the angled old town must fill at least this share of its bounding rect (no slivers,
// no fans).
constexpr double kFat = 0.6;

// The piece of the line through a with direction (dx, dy) inside a convex polygon: [p, q] or null.
std::optional<std::array<Point2, 2>> clip_line(const Poly& poly, const Point2& a, double dx, double dy) {
  double t0 = -js::kInf;
  double t1 = js::kInf;
  const size_t n = poly.size();
  // the polygon's winding decides which side of an edge is inside
  double wind = 0;
  for (size_t k = 0; k < n; ++k) wind = wind + poly[k].x * poly[(k + 1) % n].y - poly[(k + 1) % n].x * poly[k].y;
  const bool ccw = wind > 0;
  for (size_t k = 0; k < n; ++k) {
    const PolyPt& p = poly[k];
    const PolyPt& q = poly[(k + 1) % n];
    // inside: cross(q - p, x - p) has the winding's sign
    const double ex = q.x - p.x;
    const double ey = q.y - p.y;
    const double s0 = (ex * (a.y - p.y) - ey * (a.x - p.x)) * (ccw ? 1 : -1);
    const double ds = (ex * dy - ey * dx) * (ccw ? 1 : -1);
    if (ds == 0) {
      if (s0 < 0) return std::nullopt;
      continue;
    }
    const double t = -s0 / ds;
    if (ds > 0)
      t0 = js::max(t0, t);
    else
      t1 = js::min(t1, t);
  }
  if (!(t1 > t0)) return std::nullopt;
  return std::array<Point2, 2>{Point2{a.x + dx * t0, a.y + dy * t0}, Point2{a.x + dx * t1, a.y + dy * t1}};
}

// The side the cut's new edges follow until its road exists (JS: one object, cutSide, that the
// split polygons share and Object.assign fills with the road's side once it is made): no road id
// is this.
const RoadSide& cut_mark() {
  static const RoadSide s{std::nullopt, 0, std::string("\x01cut")};
  return s;
}
bool is_cut_mark(const RoadSide& s) { return s.id && *s.id == *cut_mark().id; }

struct OrganicAngled {
  StreetEmit& emit;
  Rng& rng;
  const District& district;
  double tilt;

  void emit_poly(const Poly& poly) {
    const PolyBlock b = poly_block(poly);
    emit.block(b.r, b.s, b.cuts.empty() ? nullptr : &poly);
  }

  struct Cut {
    std::array<Point2, 2> seg;
    Poly L, R;
  };

  void recurse(const Poly& poly, int depth) {
    const Rect bb = poly_bounds(poly);
    const Rect rect{std::floor(bb.x0), std::floor(bb.y0), std::ceil(bb.x1), std::ceil(bb.y1)};
    const District& d = local_or(emit, rect, district);
    const DistrictStreets& st = d.streets;
    if (pattern_family(st.pattern) == "none") {
      emit_poly(poly);
      return;
    }
    const std::array<double, 2>& long_r = st.block[0];
    const std::array<double, 2>& short_r = st.block[1];
    const double lane_chance = st.lane_chance.value_or(0);
    const std::string lane_cls = st.lane_class.empty() ? std::string("alley") : st.lane_class;
    const double min_part = vx(short_r[0] * 0.7);
    const double w = rect.x1 - rect.x0;
    const double h = rect.y1 - rect.y0;
    const bool long_is_x = w >= h;
    const double long_dim = long_is_x ? w : h;
    const double short_dim = long_is_x ? h : w;
    const double long_t = vx(rng.float_(long_r[0], long_r[1]));
    const double short_t = vx(rng.float_(short_r[0], short_r[1]));
    char axis = 0;
    if (long_dim > long_t * 1.2)
      axis = long_is_x ? 'x' : 'y';
    else if (short_dim > short_t * 1.55)
      axis = long_is_x ? 'y' : 'x';
    const double dim = axis == 'x' ? w : h;
    if (!axis || depth > 14 || dim < 2 * min_part) {
      emit_poly(poly);
      return;
    }
    const double at0 = js::max(min_part, js::min(dim - min_part, dim * rng.float_(0.3, 0.7)));
    const double at = snap8((axis == 'x' ? rect.x0 : rect.y0) + at0);
    const Point2 a = axis == 'x' ? Point2{at, (bb.y0 + bb.y1) / 2} : Point2{(bb.x0 + bb.x1) / 2, at};
    // the cut: across the axis, now and then tilted by an exact small angle (straight after all
    // where the tilt would leave a sliver or a fan)
    auto poor = [&](const std::optional<Poly>& q) {
      if (!q) return true;
      const Rect b = poly_bounds(*q);
      return js::min(b.x1 - b.x0, b.y1 - b.y0) < min_part || poly_area(*q) < js::max(min_part * min_part, kFat * (b.x1 - b.x0) * (b.y1 - b.y0));
    };
    auto try_cut = [&](double dx, double dy) -> std::optional<Cut> {
      const std::optional<std::array<Point2, 2>> seg = clip_line(poly, a, dx, dy);
      if (!seg) return std::nullopt;
      std::pair<std::optional<Poly>, std::optional<Poly>> lr = split_poly(poly, a, dx, dy, cut_mark());
      if (poor(lr.first) || poor(lr.second)) return std::nullopt;
      return Cut{*seg, std::move(*lr.first), std::move(*lr.second)};
    };
    std::optional<Cut> cut;
    // (a region with a slanted edge running the same way is cut parallel to it: tilted lanes run
    // side by side instead of closing into a fan)
    std::optional<std::array<double, 2>> par;
    for (size_t k = 0; k < poly.size() && !par; ++k) {
      const PolyPt& p = poly[k];
      const PolyPt& q = poly[(k + 1) % poly.size()];
      const double ex = q.x - p.x;
      const double ey = q.y - p.y;
      if (ex != 0 && ey != 0 && (axis == 'x' ? std::fabs(ey) > std::fabs(ex) : std::fabs(ex) > std::fabs(ey))) par = std::array<double, 2>{ex, ey};
    }
    if (rng.chance(tilt)) {
      const Yaw& t = yaws()[static_cast<size_t>(rng.pick(kTilts))];
      const double sg = rng.sign();
      cut = par ? try_cut((*par)[0], (*par)[1]) : axis == 'x' ? try_cut(-sg * t.s, t.c) : try_cut(t.c, sg * t.s);
    }
    if (!cut) cut = axis == 'x' ? try_cut(0, 1) : try_cut(1, 0);
    if (!cut) {
      emit_poly(poly);
      return;
    }
    // a lane only between blocks that are small enough to be walked round
    const std::string cls = depth > 0 && dim < vx(long_r[1] * 2.2) && rng.chance(lane_chance) ? lane_cls : pick_class(d, rng);
    const std::string paving = st.paving;  // (?? null)
    const RoadSide road = emit.road(cls, cut->seg[0].x, cut->seg[0].y, cut->seg[1].x, cut->seg[1].y, &paving);
    // (Object.assign(cutSide, side(road)): the new edges of both halves follow the road)
    for (Poly* half : {&cut->L, &cut->R})
      for (PolyPt& p : *half)
        if (is_cut_mark(p.side)) p.side = road;
    recurse(cut->L, depth + 1);
    recurse(cut->R, depth + 1);
  }
};

}  // namespace

// Grid streets: rows and columns of blocks. Block widths vary a little (+-15%), and now and then
// (streets.jog, 0.3) a north-south street shifts sideways by 8-25 m where it crosses a street, so
// the grid reads as grown rather than drawn. Neighbouring blocks may merge in pairs (mergeChance;
// only side by side where streets jog).
void grid_streets(const StreetSub& sub, const District& district, Rng& rng, StreetEmit& emit, const StreetOpts*) {
  const Rect& r = sub.rect;
  const double W = r.x1 - r.x0;
  const double H = r.y1 - r.y0;
  const std::array<double, 2>& long_r = district.streets.block[0];
  const std::array<double, 2>& short_r = district.streets.block[1];
  const double long_t = vx(rng.float_(long_r[0], long_r[1]));
  const double short_t = vx(rng.float_(short_r[0], short_r[1]));
  // orient long block side along the longer sub-cell dimension (mostly)
  const bool long_along_x = W >= H ? rng.chance(0.8) : rng.chance(0.2);
  const double tx = long_along_x ? long_t : short_t;
  const double ty = long_along_x ? short_t : long_t;
  const int nx = static_cast<int>(js::max(1.0, js::round(W / tx)));
  const int ny = static_cast<int>(js::max(1.0, js::round(H / ty)));
  const std::vector<double> ys = grid_split(rng, r.y0, H, ny, vx(short_r[0] * 0.6));
  // x positions per row: a line may jog at a row boundary
  const std::vector<double> base = grid_split(rng, r.x0, W, nx, vx(short_r[0] * 0.6));
  std::vector<std::vector<double>> xs_row{base};
  for (int b = 1; b < ny; ++b) {
    std::vector<double> row = xs_row[size_t(b - 1)];
    for (int a = 1; a < nx; ++a) {
      if (!rng.chance(kJog / js::max(1.0, ny - 1.0))) continue;
      const double lo = row[size_t(a - 1)] + vx(short_r[0] * 0.6);
      const double hi = row[size_t(a + 1)] - vx(short_r[0] * 0.6);
      const double m = vx(rng.float_(8, 25));
      const double sg = rng.chance(0.5) ? 1 : -1;
      const double x = snap8(row[size_t(a)] + m * sg);
      if (x > lo && x < hi) row[size_t(a)] = x;
    }
    xs_row.push_back(std::move(row));
  }
  bool jogged = false;
  for (const std::vector<double>& row : xs_row)
    for (size_t a = 0; a < row.size(); ++a) jogged = jogged || row[a] != base[a];

  // merges of neighbouring cells (pairs only, keeps rectangles)
  auto cell_id = [&](int a, int b) { return b * nx + a; };
  const size_t n = size_t(nx) * size_t(ny);
  std::vector<int> merged_with(n, -1);
  std::vector<char> removed_v(n, 0);  // vertical edge between (a, b) and (a + 1, b): key (a, b)
  std::vector<char> removed_h(n, 0);  // horizontal edge between (a, b) and (a, b + 1)
  const double mc = district.streets.merge_chance;
  for (int b = 0; b < ny; ++b) {
    for (int a = 0; a < nx; ++a) {
      if (merged_with[size_t(cell_id(a, b))] >= 0 || !rng.chance(mc)) continue;
      const bool horizontal = jogged || rng.chance(0.5);
      const int a2 = horizontal ? a + 1 : a;
      const int b2 = horizontal ? b : b + 1;
      if (a2 >= nx || b2 >= ny || merged_with[size_t(cell_id(a2, b2))] >= 0) continue;
      merged_with[size_t(cell_id(a, b))] = cell_id(a2, b2);
      merged_with[size_t(cell_id(a2, b2))] = cell_id(a, b);
      if (horizontal)
        removed_v[size_t(cell_id(a, b))] = 1;
      else
        removed_h[size_t(cell_id(a, b))] = 1;
    }
  }

  // vertical interior lines: maximal runs of kept edges at one x
  std::vector<std::optional<RoadSide>> v_roads(n);  // key (a, b) -> road
  for (int a = 0; a < nx - 1; ++a) {
    const std::string cls = pick_class(district, rng);
    int run_start = -1;
    for (int b = 0; b <= ny; ++b) {
      const bool kept = b < ny && !removed_v[size_t(cell_id(a, b))];
      // (past the last row x is null: never the run's x)
      if (run_start >= 0 && (!kept || b >= ny || xs_row[size_t(b)][size_t(a + 1)] != xs_row[size_t(run_start)][size_t(a + 1)])) {
        const double x = xs_row[size_t(run_start)][size_t(a + 1)];
        const RoadSide road = emit.road(cls, x, ys[size_t(run_start)], x, ys[size_t(b)], nullptr);
        for (int bb = run_start; bb < b; ++bb) v_roads[size_t(cell_id(a, bb))] = road;
        run_start = -1;
      }
      if (kept && run_start < 0) run_start = b;
    }
  }
  std::vector<std::optional<RoadSide>> h_roads(n);
  for (int b = 0; b < ny - 1; ++b) {
    const double y = ys[size_t(b + 1)];
    const std::string cls = pick_class(district, rng);
    int run_start = -1;
    for (int a = 0; a <= nx; ++a) {
      const bool kept = a < nx && !removed_h[size_t(cell_id(a, b))];
      if (kept && run_start < 0) run_start = a;
      if (!kept && run_start >= 0) {
        // (a jogged grid: the street spans the widest of the two rows it separates)
        const double x0 = js::min(xs_row[size_t(b)][size_t(run_start)], xs_row[size_t(b + 1)][size_t(run_start)]);
        const double x1 = js::max(xs_row[size_t(b)][size_t(a)], xs_row[size_t(b + 1)][size_t(a)]);
        const RoadSide road = emit.road(cls, x0, y, x1, y, nullptr);
        for (int aa = run_start; aa < a; ++aa) h_roads[size_t(cell_id(aa, b))] = road;
        run_start = -1;
      }
    }
  }

  auto road_at = [](const std::vector<std::optional<RoadSide>>& roads, size_t k) -> const RoadSide& {
    if (!roads[k]) SVX_FAIL("gridStreets: a block side without its street");
    return *roads[k];
  };
  std::vector<char> done(n, 0);
  for (int b = 0; b < ny; ++b) {
    for (int a = 0; a < nx; ++a) {
      const int id = cell_id(a, b);
      if (done[size_t(id)]) continue;
      done[size_t(id)] = 1;
      int a1 = a;
      int b1 = b;
      if (merged_with[size_t(id)] >= 0) {
        const int other = merged_with[size_t(id)];
        done[size_t(other)] = 1;
        a1 = std::max(a, other % nx);
        b1 = std::max(b, other / nx);
      }
      const std::vector<double>& xs = xs_row[size_t(b)];
      const Rect rect{xs[size_t(a)], ys[size_t(b)], xs[size_t(a1 + 1)], ys[size_t(b1 + 1)]};
      BlockSides sides;
      sides.W = a == 0 ? sub.sides.W : road_at(v_roads, size_t(cell_id(a - 1, b)));
      sides.E = a1 == nx - 1 ? sub.sides.E : road_at(v_roads, size_t(cell_id(a1, b)));
      sides.N = b == 0 ? sub.sides.N : road_at(h_roads, size_t(cell_id(a, b - 1)));
      sides.S = b1 == ny - 1 ? sub.sides.S : road_at(h_roads, size_t(cell_id(a, b1)));
      emit.block(rect, sides, nullptr);
    }
  }
}

void subdivide_streets(const StreetSub& sub, const District& district, Rng& rng, StreetEmit& emit, const StreetOpts*) {
  const std::array<double, 2>& long_r = district.streets.block[0];
  const std::array<double, 2>& short_r = district.streets.block[1];
  const double long_t = vx(rng.float_(long_r[0], long_r[1]));
  const double short_t = vx(rng.float_(short_r[0], short_r[1]));
  Subdivide s{emit, rng, district, long_t, short_t};
  s.recurse(sub.rect, sub.sides, 0);
}

// Old-town streets: a sub-cell split again and again at irregular places (anywhere between a third
// and two thirds of the block, never leaving a sliver), so blocks come in every size and
// proportion, streets end at T-junctions and jog where they cross. Now and then the cut is a
// narrow lane (streets.laneClass, a cobbled passage between the houses) instead of a street, and
// small blocks stay whole. In the angled world (opts.angled) the same on convex polygons, now and
// then (opts.tilt) a cut tilted off the axes by a small exact angle, so lanes meet at natural
// angles and blocks come out as irregular quadrilaterals; a block is its polygon (blockPoly).
void organic_streets(const StreetSub& sub, const District& district, Rng& rng, StreetEmit& emit, const StreetOpts* opts) {
  if (opts && opts->angled) {
    OrganicAngled o{emit, rng, district, opts->tilt};
    o.recurse(rect_poly(sub.rect, sub.sides), 0);
    return;
  }
  Organic o{emit, rng, district};
  o.recurse(sub.rect, sub.sides, 0);
}

void no_streets(const StreetSub& sub, const District&, Rng&, StreetEmit& emit, const StreetOpts*) { emit.block(sub.rect, sub.sides, nullptr); }

StreetPattern street_pattern(std::string_view name) {
  if (name == "grid") return grid_streets;
  if (name == "subdivide") return subdivide_streets;
  if (name == "organic") return organic_streets;
  if (name == "none") return no_streets;
  return nullptr;
}

std::string_view pattern_family(std::string_view pattern) {
  if (pattern == "grid" || pattern == "organic") return "fine";
  if (pattern == "subdivide") return "coarse";
  if (pattern == "none") return "none";
  return "";
}

}  // namespace svx::city
