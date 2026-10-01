// svx_city — network/highways.hpp (voxel_city network/highways.js).
#include "network/highways.hpp"

#include <utility>

#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "network/arterials.hpp"
#include "network/roadLevel.hpp"
#include "network/roadSurface.hpp"
#include "network/roadView.hpp"
#include "terrain/terrain.hpp"
#include "voxel/chunk.hpp"
#include "voxel/materials.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"

namespace svx::city {

namespace {

constexpr double kPi = 3.141592653589793;
const double kSample = vx(8);
// Steepest ramp grade, and the share of a ramp's length easing into and out of it at each end.
constexpr double kRampGrade = 0.07;
constexpr double kRampEase = 0.15;
// Longest ramp (voxels).
const double kRampMax = vx(320);
// Headroom a ramp leaves over a carriageway under it, and over a sidewalk (voxels).
constexpr double kRampClear = 36;
constexpr double kWalkClear = 20;
// Pruning passes for chance edges: spurs up to this many edges long vanish.
constexpr int kSpurPasses = 2;
// The four lattice edges at node (a, b): [axis, a, b].
std::array<std::array<double, 3>, 4> incident(double a, double b) { return {{{0, a, b}, {0, a - 1, b}, {1, a, b}, {1, a, b - 1}}}; }
constexpr double kNodeRing[8][2] = {{2400, 0}, {-2400, 0}, {0, 2400}, {0, -2400}, {1700, 1700}, {-1700, 1700}, {1700, -1700}, {-1700, -1700}};

// Rasterizer constants
constexpr double kDeckT = 10;         // slab thickness (voxels)
constexpr double kTunnelCover = 56;   // 7 m of ground over the deck: bore a tunnel
constexpr double kEmbank = 44;        // decks lower than 5.5 m over the ground sit on earth
constexpr double kTunnelH = 52;

// The cache key of an edge (or of its existence after k passes): lattice indices well within
// 2^29 in magnitude.
uint64_t edge_key(int axis, double a, double b, int k = 0) {
  const uint64_t ua = static_cast<uint32_t>(static_cast<int32_t>(a)) & 0x3FFFFFFFu;
  const uint64_t ub = static_cast<uint32_t>(static_cast<int32_t>(b)) & 0x3FFFFFFFu;
  return (ua << 34) | (ub << 4) | (static_cast<uint64_t>(k) << 1) | static_cast<uint64_t>(axis);
}

// The arterial street at a crossing point: { id, hc } or none.
struct ArterialHere {
  std::string id;
  double hc = 0;
};
std::optional<ArterialHere> arterial_at(const World& w, double x, double y) {
  const CellIJ c = w.cell_at(x, y);
  const std::shared_ptr<const RoadView> view = w.road_view(c.i, c.j);
  RoadSample rs = make_road_sample();
  sample_road_surface(view->near(Rect{x - 2, y - 2, x + 2, y + 2}), x + 0.5, y + 0.5, rs, w.seed);
  const RoadSeg* seg = rs.seg;
  if (!seg || rs.kind == RoadKind::NONE || seg->road->cls != "arterial") return std::nullopt;
  return ArterialHere{seg->road->id, seg->hc};
}

// A crossing of the deck with an arterial lattice line.
struct Crossing {
  double s = 0, x = 0, y = 0;
  int axis = 0;
  double line = 0;
};

// The arc where a ramp's centre line (offset `off` from the deck) meets the arterial crossing at
// c (an axis-aligned lattice line, half-width hc): as it enters the arterial (before the crossing)
// or leaves it (after), or none.
std::optional<double> kerb_arc(const HighwayEdge& e, const Crossing& c, double off, double hc, bool before) {
  auto inside = [&](double s) {
    const HighwayPoint p = offset_at(e, s, off);
    return js::abs((c.axis == 0 ? p.x : p.y) - c.line) <= hc;
  };
  const double reach = vx(120);
  std::optional<double> first;
  std::optional<double> last;
  for (double s = js::max(0.0, c.s - reach); s <= js::min(e.total, c.s + reach); s += 2)
    if (inside(s)) {
      if (!first) first = s;
      last = s;
    }
  if (!first) return std::nullopt;
  return before ? first : last;
}

// A ramp's steepest grade (every 4 voxels along it).
double ramp_steepest(const HighwayEdge& e, const HighwayRamp& r) {
  const double n = js::max(2.0, std::ceil(js::abs(r.s_deck - r.s_ground) / 4));
  double worst = 0;
  double prev = ramp_z(e, r, r.s_ground);
  for (double k = 1; k <= n; k += 1) {
    const double s = r.s_ground + ((r.s_deck - r.s_ground) * k) / n;
    const double z = ramp_z(e, r, s);
    worst = js::max(worst, js::abs(z - prev) / (js::abs(r.s_deck - r.s_ground) / n));
    prev = z;
  }
  return worst;
}

// Distance from a segment to a rect (0 if they meet).
double seg_rect_dist(const HighwaySeg& s, const Rect& r) {
  auto inside = [&](double x, double y) { return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1; };
  if (inside(s.ax, s.ay) || inside(s.bx, s.by)) return 0;
  double best = js::kInf;
  const double corners[4][2] = {{r.x0, r.y0}, {r.x1, r.y0}, {r.x0, r.y1}, {r.x1, r.y1}};
  for (const auto& c : corners) {
    const double x = c[0], y = c[1];
    const double t = js::max(0.0, js::min(s.len, (x - s.ax) * s.dx + (y - s.ay) * s.dy));
    best = js::min(best, js::hypot(x - (s.ax + s.dx * t), y - (s.ay + s.dy * t)));
  }
  const double ends[2][2] = {{s.ax, s.ay}, {s.bx, s.by}};
  for (const auto& p : ends) {
    const double x = p[0], y = p[1];
    const double cx = js::max(r.x0, js::min(r.x1, x));
    const double cy = js::max(r.y0, js::min(r.y1, y));
    best = js::min(best, js::hypot(x - cx, y - cy));
  }
  // crossing test: segment intersects rect edges
  const double steps = std::ceil(s.len / 16);
  for (double k = 0; k <= steps; k += 1) {
    const double t = (k / steps) * s.len;
    if (inside(s.ax + s.dx * t, s.ay + s.dy * t)) return 0;
  }
  return best;
}

}  // namespace

// ---------------------------------------------------------------- the deck's geometry

HighwayPoint point_at(const HighwayEdge& e, double s) {
  const std::vector<double>& L = e.lengths;
  int lo = 0;
  int hi = static_cast<int>(L.size()) - 1;
  while (hi - lo > 1) {
    const int mid = (lo + hi) >> 1;
    if (L[static_cast<size_t>(mid)] <= s)
      lo = mid;
    else
      hi = mid;
  }
  const PPoint& a = e.pts[static_cast<size_t>(lo)];
  const PPoint& b = e.pts[static_cast<size_t>(hi)];
  const double t = (s - L[static_cast<size_t>(lo)]) / js::or_(L[static_cast<size_t>(hi)] - L[static_cast<size_t>(lo)], 1);
  const double sl = js::or_(L[static_cast<size_t>(hi)] - L[static_cast<size_t>(lo)], 1);
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, (b.x - a.x) / sl, (b.y - a.y) / sl};
}

HighwayPoint offset_at(const HighwayEdge& e, double s, double off) {
  const HighwayPoint p = point_at(e, s);
  return {p.x - p.ty * off, p.y + p.tx * off, p.z, 0, 0};
}

double ramp_z(const HighwayEdge& e, const HighwayRamp& r, double s) {
  const double t = js::max(0.0, js::min(1.0, (s - r.s_ground) / (r.s_deck - r.s_ground)));
  const double k = kRampEase;
  const double m = 1 / (1 - k);
  const double f = t < k ? (m * t * t) / (2 * k) : t <= 1 - k ? m * (t - k / 2) : 1 - (m * (1 - t) * (1 - t)) / (2 * k);
  return point_at(e, js::max(0.0, js::min(e.total, s))).z - r.drop * (1 - f);
}

// ---------------------------------------------------------------- the network

HighwayNetwork::HighwayNetwork(const World& w) : world(w) {
  const Value& c = w.config["highways"];
  cfg.node_spacing = c["nodeSpacing"].to_number();
  cfg.jitter = c["jitter"].to_number();
  cfg.edge_chance = c["edgeChance"].to_number();
  cfg.min_urbanization = c["minUrbanization"].to_number();
  cfg.deck_height = c["deckHeight"].to_number();
  cfg.lanes_per_side = c["lanesPerSide"].to_number();
  cfg.lane_width = c["laneWidth"].to_number();
  cfg.shoulder = c["shoulder"].to_number();
  cfg.pier_spacing = c["pierSpacing"].to_number();
  cfg.corridor_margin = c["corridorMargin"].to_number();
  spacing = vx(cfg.node_spacing);
  hw = js::round(vx(2 * cfg.lanes_per_side * cfg.lane_width + 2 * cfg.shoulder + 0.6) / 2) + 3;
  clear = vx(cfg.deck_height);
  wrap = wrap_of(w.config);
  n = wrap.count(cfg.node_spacing);
  plateau = js::round(2.5 * hw);
  ramp_in = hw + 1;
  ramp_mid = ramp_in + vx(3.5);
  settlement_cell_ = w.config["world"]["settlementCell"].to_number();
}

HighwayNode HighwayNetwork::node(double a, double b) const {
  const double s = world.seed;
  const double j = cfg.jitter * spacing;
  const double ca = canon(a);
  const double cb = canon(b);
  const double x = js::round(ca * spacing + (hash_float(s, ca, cb, 501) - 0.5) * 2 * j) + Wrap::lap(a, n) * wrap.size_v;
  const double y = js::round(cb * spacing + (hash_float(s, ca, cb, 502) - 0.5) * 2 * j) + Wrap::lap(b, n) * wrap.size_v;
  const TerrainSample ts = world.terrain->sample(x, y);
  // in rough country a junction takes the local valley level (it may lie inside a mountain:
  // tunnels meet there)
  double h = ts.h;
  if (ts.u < 0.3)
    for (const auto& d : kNodeRing) h = js::min(h, world.terrain->sample(x + d[0], y + d[1]).h);
  const double z = js::round(h + clearance_at(ts.u));
  return {a, b, x, y, z};
}

std::vector<double> HighwayNetwork::profile(const std::vector<PPoint>& pts, double z_start, double z_end, double flat_start, double flat_end) const {
  const World& w = world;
  const int n_pts = static_cast<int>(pts.size());
  // (the 5% limit over the samples' own spacing: the shortest chord, a little under SAMPLE on a bend)
  double step = kSample;
  for (int i = 1; i < n_pts; ++i) step = js::min(step, js::hypot(pts[static_cast<size_t>(i)].x - pts[static_cast<size_t>(i - 1)].x, pts[static_cast<size_t>(i)].y - pts[static_cast<size_t>(i - 1)].y));
  const double g = step * 0.05;
  std::vector<double> target(static_cast<size_t>(n_pts), 0.0);
  std::vector<double> hard(static_cast<size_t>(n_pts), -js::kInf);
  RoadSample rs = make_road_sample();
  std::vector<const RoadSeg*> cands;
  for (int i = 0; i < n_pts; ++i) {
    const PPoint& p = pts[static_cast<size_t>(i)];
    const TerrainSample ts = w.terrain->sample(p.x, p.y);
    target[static_cast<size_t>(i)] = ts.h + clearance_at(ts.u);
    if (ts.u > 0.3) hard[static_cast<size_t>(i)] = target[static_cast<size_t>(i)];
    // crossing roads and rivers need an overpass / bridge
    const CellIJ c = w.cell_at(p.x, p.y);
    const std::shared_ptr<const RoadView> view = w.road_view(c.i, c.j);
    cands.clear();
    view->near(Rect{p.x - 40, p.y - 40, p.x + 40, p.y + 40}, cands);
    sample_road_surface(cands, p.x + 0.5, p.y + 0.5, rs, w.seed);
    if (rs.kind != RoadKind::NONE || rs.sdf_r < 40) hard[static_cast<size_t>(i)] = js::max(hard[static_cast<size_t>(i)], ts.h + js::round(clear * 0.8));
    if (w.is_wet(p.x, p.y, 12)) hard[static_cast<size_t>(i)] = js::max(hard[static_cast<size_t>(i)], ts.h + 40);
  }
  // spread hard constraints over +-3 samples (clear the whole crossing)
  std::vector<double> lo(static_cast<size_t>(n_pts), 0.0);
  for (int i = 0; i < n_pts; ++i) {
    double m = -js::kInf;
    for (int j = std::max(0, i - 3); j <= std::min(n_pts - 1, i + 3); ++j) m = js::max(m, hard[static_cast<size_t>(j)]);
    lo[static_cast<size_t>(i)] = m;
  }
  // soft target smoothed, then the highest grade-limited profile that stays at or below it:
  // ridges are tunnelled, valleys are never bridged high above the ground (only hard constraints
  // lift the road)
  std::vector<double> z(static_cast<size_t>(n_pts), 0.0);
  for (int i = 0; i < n_pts; ++i) {
    double s = 0;
    double c = 0;
    double mn = js::kInf;
    for (int j = std::max(0, i - 6); j <= std::min(n_pts - 1, i + 6); ++j) {
      s += target[static_cast<size_t>(j)];
      c += 1;
      mn = js::min(mn, target[static_cast<size_t>(j)]);
    }
    z[static_cast<size_t>(i)] = js::min(s / c, mn + 24);
  }
  for (int i = 1; i < n_pts; ++i) z[static_cast<size_t>(i)] = js::min(z[static_cast<size_t>(i)], z[static_cast<size_t>(i - 1)] + g);
  for (int i = n_pts - 2; i >= 0; --i) z[static_cast<size_t>(i)] = js::min(z[static_cast<size_t>(i)], z[static_cast<size_t>(i + 1)] + g);
  // hard bounds win; propagate the raise at the grade limit
  for (int i = 0; i < n_pts; ++i) z[static_cast<size_t>(i)] = js::max(z[static_cast<size_t>(i)], lo[static_cast<size_t>(i)]);
  for (int i = 1; i < n_pts; ++i) z[static_cast<size_t>(i)] = js::max(z[static_cast<size_t>(i)], z[static_cast<size_t>(i - 1)] - g);
  for (int i = n_pts - 2; i >= 0; --i) z[static_cast<size_t>(i)] = js::max(z[static_cast<size_t>(i)], z[static_cast<size_t>(i + 1)] - g);
  // round the grade breaks
  std::vector<double> out(static_cast<size_t>(n_pts), 0.0);
  for (int i = 0; i < n_pts; ++i) {
    double s = 0;
    double c = 0;
    for (int j = std::max(0, i - 3); j <= std::min(n_pts - 1, i + 3); ++j) {
      s += z[static_cast<size_t>(j)];
      c += 1;
    }
    out[static_cast<size_t>(i)] = js::max(s / c, lo[static_cast<size_t>(i)]);
  }
  // meet the node heights (shared by every edge at a node) within the grade limit: clamp into the
  // cone reachable from both ends, then sweep
  out[0] = z_start;
  out[static_cast<size_t>(n_pts - 1)] = z_end;
  for (int i = 1; i < n_pts - 1; ++i) {
    const double a = js::max(z_start - g * i, z_end - g * (n_pts - 1 - i));
    const double b = js::min(z_start + g * i, z_end + g * (n_pts - 1 - i));
    out[static_cast<size_t>(i)] = js::min(js::max(out[static_cast<size_t>(i)], a), b);
  }
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 1; i < n_pts - 1; ++i)
      out[static_cast<size_t>(i)] = js::min(js::max(out[static_cast<size_t>(i)], out[static_cast<size_t>(i - 1)] - g), out[static_cast<size_t>(i - 1)] + g);
    for (int i = n_pts - 2; i > 0; --i)
      out[static_cast<size_t>(i)] = js::min(js::max(out[static_cast<size_t>(i)], out[static_cast<size_t>(i + 1)] - g), out[static_cast<size_t>(i + 1)] + g);
  }
  // junction plateaus: level at the node's height, then back to the profile within the grade limit
  const double fs = js::min(n_pts - 1.0, std::ceil(flat_start / step));
  const double fe = js::min(n_pts - 1.0, std::ceil(flat_end / step));
  if (js::truthy(fs) || js::truthy(fe)) {
    for (double i = 0; i <= fs; i += 1) out[static_cast<size_t>(i)] = z_start;
    for (double i = n_pts - 1 - fe; i < n_pts; i += 1) out[static_cast<size_t>(i)] = z_end;
    for (double i = fs + 1; i < n_pts - 1 - fe; i += 1)
      out[static_cast<size_t>(i)] = js::min(js::max(out[static_cast<size_t>(i)], out[static_cast<size_t>(i - 1)] - g), out[static_cast<size_t>(i - 1)] + g);
    for (double i = n_pts - 2 - fe; i > fs; i -= 1)
      out[static_cast<size_t>(i)] = js::min(js::max(out[static_cast<size_t>(i)], out[static_cast<size_t>(i + 1)] - g), out[static_cast<size_t>(i + 1)] + g);
  }
  return out;
}

bool HighwayNetwork::base_edge(int axis, double a, double b, int k) const {
  return *memo_.get(edge_key(axis, a, b, k), [&]() -> bool {
    if (!grade_ok(axis, a, b)) return false;
    if (on_intercity_route(axis, a, b)) return true;
    if (!chance_edge(axis, a, b)) return false;
    if (k == 0) return true;
    const double a1 = axis == 0 ? a + 1 : a;
    const double b1 = axis == 0 ? b : b + 1;
    auto company = [&](double na, double nb) {
      for (const auto& e : incident(na, nb)) {
        if (e[0] == axis && e[1] == a && e[2] == b) continue;
        if (base_edge(static_cast<int>(e[0]), e[1], e[2], k - 1)) return true;
      }
      return false;
    };
    return company(a, b) && company(a1, b1);
  });
}

bool HighwayNetwork::chance_edge(int axis, double a, double b) const {
  if (hash_float(world.seed, canon(a) * 2 + axis, canon(b), 503) >= cfg.edge_chance) return false;
  const Point2 n0 = lattice_xy(a, b);
  const Point2 n1 = axis == 0 ? lattice_xy(a + 1, b) : lattice_xy(a, b + 1);
  const MacroFields& f = *world.fields;
  return js::min(f.urban(n0.x, n0.y).u, f.urban(n1.x, n1.y).u) >= cfg.min_urbanization;
}

std::vector<std::array<double, 3>> HighwayNetwork::edges_at(double a, double b) const {
  std::vector<std::array<double, 3>> out;
  for (const auto& e : incident(a, b))
    if (edge_exists(static_cast<int>(e[0]), e[1], e[2])) out.push_back(e);
  return out;
}

double HighwayNetwork::node_z(double a, double b) const {
  const HighwayNode nd = node(a, b);
  if (edges_at(a, b).size() != 1) return nd.z;
  return js::round(world.street_level(nd.x, nd.y));
}

bool HighwayNetwork::grade_ok(int axis, double a, double b) const {
  const HighwayNode n0 = node(a, b);
  const HighwayNode n1 = axis == 0 ? node(a + 1, b) : node(a, b + 1);
  return js::abs(n1.z - n0.z) <= 0.05 * js::hypot(n1.x - n0.x, n1.y - n0.y) * 0.9;
}

bool HighwayNetwork::on_intercity_route(int axis, double a, double b) const {
  const MacroFields& f = *world.fields;
  const double cell_v = settlement_cell_ * 8;
  const double x = a * spacing;
  const double y = b * spacing;
  const double ci = js::round(x / cell_v);
  const double cj = js::round(y / cell_v);
  const double R = std::ceil((spacing * 2) / cell_v) + 1;
  for (double j = cj - R; j <= cj + R; j += 1) {
    for (double i = ci - R; i <= ci + R; i += 1) {
      const Settlement* s = f.settlement(i, j);
      if (!s) continue;
      const Settlement* ts[2] = {f.settlement(i + 1, j), f.settlement(i, j + 1)};
      for (const Settlement* t : ts) {
        if (!t) continue;
        if (route_has(*s, *t, axis, a, b)) return true;
      }
    }
  }
  return false;
}

bool HighwayNetwork::route_has(const Settlement& s, const Settlement& t, int axis, double a, double b) const {
  const double a0 = js::round(s.x / spacing);
  const double b0 = js::round(s.y / spacing);
  const double a1 = js::round(t.x / spacing);
  const double b1 = js::round(t.y / spacing);
  // L-shaped: along a first (at row b0), then along b (at column a1), or the reverse
  const bool x_first = hash_float(world.seed, canon(a0) * 31 + canon(a1), canon(b0) * 17 + canon(b1), 507) < 0.5;
  const double row_b = x_first ? b0 : b1;
  const double col_a = x_first ? a1 : a0;
  if (axis == 0) return b == row_b && a >= js::min(a0, a1) && a < js::max(a0, a1);
  return a == col_a && b >= js::min(b0, b1) && b < js::max(b0, b1);
}

HighwayEdgePtr HighwayNetwork::edge(int axis, double a, double b) const {
  return edges_.get(edge_key(axis, a, b), [&]() -> std::shared_ptr<const HighwayEdge> {
    return edge_exists(axis, a, b) ? build_edge(axis, a, b) : nullptr;
  });
}

std::shared_ptr<const HighwayEdge> HighwayNetwork::build_edge(int axis, double a, double b) const {
  const World& w = world;
  const HighwayNode n0 = node(a, b);
  const HighwayNode n1 = axis == 0 ? node(a + 1, b) : node(a, b + 1);
  const double dx = n1.x - n0.x;
  const double dy = n1.y - n0.y;
  const double len = js::hypot(dx, dy);
  const double px = -dy / len;
  const double py = dx / len;
  std::vector<PPoint> ctrl;
  ctrl.push_back({n0.x, n0.y});
  const int bends = 3;
  for (int k = 1; k <= bends; ++k) {
    const double t = static_cast<double>(k) / (bends + 1);
    const double off = (hash_float(w.seed, canon(a) * 2 + axis, canon(b), 510 + k) - 0.5) * 2 * 0.2 * len;
    ctrl.push_back({n0.x + dx * t + px * off, n0.y + dy * t + py * off});
  }
  ctrl.push_back({n1.x, n1.y});
  std::vector<PPoint> spline = catmull_rom(ctrl, 24);
  // resample uniformly
  const std::vector<double> L = polyline_lengths(spline);
  const double total = L.back();
  const double n = js::max(2.0, js::round(total / kSample));
  std::vector<PPoint> pts;
  size_t k = 0;
  for (double i = 0; i <= n; i += 1) {
    const double s = (i / n) * total;
    while (k + 2 < L.size() && L[k + 1] < s) k += 1;
    const double t = (s - L[k]) / js::or_(L[k + 1] - L[k], 1);
    pts.push_back({spline[k].x + (spline[k + 1].x - spline[k].x) * t, spline[k].y + (spline[k + 1].y - spline[k].y) * t});
  }
  // deck profile (see profile()): elevated through towns and over roads and rivers, near grade in
  // open country, never steeper than 5%; level over a junction plateau at a node where other than
  // two edges meet
  const double a1 = axis == 0 ? a + 1 : a;
  const double b1 = axis == 0 ? b : b + 1;
  const double deg0 = static_cast<double>(edges_at(a, b).size());
  const double deg1 = static_cast<double>(edges_at(a1, b1).size());
  const double z0 = node_z(a, b);
  const double z1 = node_z(a1, b1);
  const std::vector<double> prof = profile(pts, z0, z1, deg0 != 2 ? plateau : 0, deg1 != 2 ? plateau : 0);
  // (unrounded: each column rounds its own level, so the grade limit holds between samples)
  for (size_t i = 0; i < pts.size(); ++i) pts[i].z = prof[i];
  auto e = std::make_shared<HighwayEdge>();
  e->lengths = polyline_lengths(pts);
  // segment index
  for (size_t i = 0; i + 1 < pts.size(); ++i) {
    const PPoint& p = pts[i];
    const PPoint& q = pts[i + 1];
    const double sl = js::or_(js::hypot(q.x - p.x, q.y - p.y), 1);
    HighwaySeg seg;
    seg.ax = p.x;
    seg.ay = p.y;
    seg.az = p.z;
    seg.bx = q.x;
    seg.by = q.y;
    seg.bz = q.z;
    seg.len = sl;
    seg.dx = (q.x - p.x) / sl;
    seg.dy = (q.y - p.y) / sl;
    seg.s0 = e->lengths[i];
    const double pad = hw + 4 + 64;  // deck + ramps
    seg.bb = {js::min(p.x, q.x) - pad, js::min(p.y, q.y) - pad, js::max(p.x, q.x) + pad, js::max(p.y, q.y) + pad};
    e->segs.push_back(seg);
  }
  for (const HighwaySeg& s : e->segs) e->grid.insert(&s, s.bb);
  bool have_bb = false;
  for (const HighwaySeg& s : e->segs) {
    if (have_bb)
      e->bb = {js::min(e->bb.x0, s.bb.x0), js::min(e->bb.y0, s.bb.y0), js::max(e->bb.x1, s.bb.x1), js::max(e->bb.y1, s.bb.y1)};
    else
      e->bb = s.bb;
    have_bb = true;
  }
  // the junctions at its ends (other than two edges meeting): level, no barriers within reach
  if (deg0 != 2) e->junctions.push_back({n0.x, n0.y, z0, plateau, deg0, {a, b}});
  if (deg1 != 2) e->junctions.push_back({n1.x, n1.y, z1, plateau, deg1, {a1, b1}});
  e->id = js::cat("H", axis, "_", a, "_", b);
  e->axis = axis;
  e->a = a;
  e->b = b;
  e->nodes = {{{a, b, deg0}, {a1, b1, deg1}}};
  e->pts = std::move(pts);
  e->total = e->lengths.back();
  return e;
}

std::vector<HighwayEdgePtr> HighwayNetwork::edges_near(const Rect& rect) const {
  const double sp = spacing;
  const double a0 = std::floor(rect.x0 / sp) - 1;
  const double a1 = std::floor(rect.x1 / sp) + 1;
  const double b0 = std::floor(rect.y0 / sp) - 1;
  const double b1 = std::floor(rect.y1 / sp) + 1;
  std::vector<HighwayEdgePtr> out;
  for (double b = b0; b <= b1; b += 1)
    for (double a = a0; a <= a1; a += 1)
      for (const int axis : {0, 1}) {
        HighwayEdgePtr e = edge(axis, a, b);
        if (!e) continue;
        if (e->bb.x1 < rect.x0 || e->bb.x0 > rect.x1 || e->bb.y1 < rect.y0 || e->bb.y0 > rect.y1) continue;
        out.push_back(std::move(e));
      }
  return out;
}

std::vector<HighwayCorridor> HighwayNetwork::corridors_near(const Rect& rect) const {
  const double margin = vx(cfg.corridor_margin);
  std::vector<HighwayCorridor> out;
  for (HighwayEdgePtr& e : edges_near(rect)) out.push_back({std::move(e), this, margin});
  return out;
}

bool HighwayCorridor::hits_rect(const Rect& r) const {
  const HighwayNetwork& hn = *net;
  const HighwayEdge& e = *edge;
  const double extra = vx(8);
  const double reach = hn.hw + margin + extra;
  const Rect q{r.x0 - reach, r.y0 - reach, r.x1 + reach, r.y1 + reach};
  const std::vector<HighwayRamp>& ramps = hn.ramps(e);
  for (const HighwaySeg* sp : e.grid.query(q)) {
    const HighwaySeg& s = *sp;
    const double d = seg_rect_dist(s, r);
    if (d < hn.hw + margin) return true;
    if (d < reach && !ramps.empty()) {
      const double cx = (r.x0 + r.x1) / 2;
      const double cy = (r.y0 + r.y1) / 2;
      const double t = js::max(0.0, js::min(s.len, (cx - s.ax) * s.dx + (cy - s.ay) * s.dy));
      const double spos = s.s0 + t;
      const double half = js::max(r.x1 - r.x0, r.y1 - r.y0) / 2;
      for (const HighwayRamp& rp : ramps)
        if (spos >= js::min(rp.s_deck, rp.s_ground) - half - vx(20) && spos <= js::max(rp.s_deck, rp.s_ground) + half + vx(20)) return true;
    }
  }
  return false;
}

std::optional<HighwayNearest> HighwayNetwork::nearest(double x, double y, const std::vector<HighwayEdgePtr>& edges, double max_d) const {
  std::optional<HighwayNearest> best;
  std::vector<const HighwaySeg*> segs;
  for (const HighwayEdgePtr& e : edges) {
    segs.clear();
    e->grid.query_point(x, y, segs);
    for (const HighwaySeg* sp : segs) {
      const HighwaySeg& s = *sp;
      const double vx0 = x - s.ax;
      const double vy0 = y - s.ay;
      const double t = vx0 * s.dx + vy0 * s.dy;
      const double tc = t < 0 ? 0 : t > s.len ? s.len : t;
      const double ex = x - (s.ax + s.dx * tc);
      const double ey = y - (s.ay + s.dy * tc);
      const double dist = js::hypot(ex, ey);
      if (dist > max_d || (best && dist >= js::abs(best->d))) continue;
      const double side = s.dx * vy0 - s.dy * vx0;
      best = HighwayNearest{side >= 0 ? dist : -dist, s.s0 + tc, s.az + (s.bz - s.az) * (tc / s.len), e.get(), &s};
    }
  }
  return best;
}

const std::vector<HighwayRamp>& HighwayNetwork::ramps(const HighwayEdge& e) const {
  return e.ramps.get([&] {
    const World& w = world;
    const ArterialGrid& A = *w.arterials;
    std::vector<HighwayRamp> out;
    std::vector<Crossing> crossings;
    for (size_t k = 0; k + 1 < e.pts.size(); ++k) {
      const PPoint& a = e.pts[k];
      const PPoint& b = e.pts[k + 1];
      for (const int axis : {0, 1}) {
        const double lo = js::min(axis == 0 ? a.x : a.y, axis == 0 ? b.x : b.y);
        const double hi = js::max(axis == 0 ? a.x : a.y, axis == 0 ? b.x : b.y);
        const double i0 = A.index_at(axis, lo);
        for (double i = i0; i <= i0 + 1; i += 1) {
          const double L = A.line(axis, i);
          if (L < lo || L > hi || hi == lo) continue;
          const double t = (L - (axis == 0 ? a.x : a.y)) / ((axis == 0 ? b.x : b.y) - (axis == 0 ? a.x : a.y));
          const double cx = a.x + (b.x - a.x) * t;
          const double cy = a.y + (b.y - a.y) * t;
          if (w.fields->urban(cx, cy).u < 0.3) continue;
          crossings.push_back({e.lengths[k] + t * (e.lengths[k + 1] - e.lengths[k]), cx, cy, axis, L});
        }
      }
    }
    js::sort(crossings, [](const Crossing& p, const Crossing& q) { return p.s - q.s; });
    const double mid = ramp_mid;
    // (no ramp onto a junction plateau, nor past the edge's ends)
    auto plateau_at = [&](size_t k) {
      for (const HighwayJunction& j : e.junctions)
        if (j.node[0] == e.nodes[k][0] && j.node[1] == e.nodes[k][1]) return true;
      return false;
    };
    const double s0 = plateau_at(0) ? plateau + vx(40) : vx(60);
    const double s1 = e.total - (plateau_at(1) ? plateau + vx(40) : vx(60));
    double last = -js::kInf;
    for (const Crossing& c : crossings) {
      if (c.s - last < 2 * kRampMax + vx(200)) continue;
      if (hash_float(world.seed, wrap.vi(c.x), wrap.vi(c.y), 777) > 0.7) continue;
      // the arterial there (a lattice line without its street has no interchange)
      const std::optional<ArterialHere> art = arterial_at(w, c.x, c.y);
      if (!art) continue;
      std::vector<HighwayRamp> found;
      for (const double side : {-1.0, 1.0}) {
        for (const bool before : {true, false}) {
          // (landing a little inside the arterial's kerb: on it, not on a side street meeting it there)
          const std::optional<double> s_ground = kerb_arc(e, c, side * mid, art->hc - 6, before);
          if (!s_ground) continue;
          const HighwayPoint land = offset_at(e, *s_ground, side * mid);
          const double z_ground = js::round(w.street_level(land.x, land.y));
          // the drop under the deck at its landing, closed over its length as the grade allows (the
          // deck's own grade included)
          const double drop = point_at(e, *s_ground).z - z_ground;
          std::optional<HighwayRamp> r;
          for (double len = js::max(vx(40), (js::abs(drop) / kRampGrade) * (1 / (1 - kRampEase))); len <= kRampMax; len *= 1.1) {
            const double s_deck = before ? *s_ground - len : *s_ground + len;
            HighwayRamp q;
            q.side = side;
            q.s_deck = s_deck;
            q.s_ground = *s_ground;
            q.drop = drop;
            q.z_deck = js::round(point_at(e, js::max(0.0, js::min(e.total, s_deck))).z);
            q.z_ground = z_ground;
            q.cross = c.s;
            q.x = land.x;
            q.y = land.y;
            q.arterial = art->id;
            if (s_deck >= s0 && s_deck <= s1 && ramp_steepest(e, q) <= kRampGrade) {
              r = std::move(q);
              break;
            }
          }
          if (!r) continue;
          if (ramp_blocked(e, *r)) continue;
          found.push_back(std::move(*r));
        }
      }
      if (found.empty()) continue;
      last = c.s;
      for (HighwayRamp& r : found) out.push_back(std::move(r));
    }
    return out;
  });
}

bool HighwayNetwork::ramp_blocked(const HighwayEdge& e, const HighwayRamp& r) const {
  const World& w = world;
  const double mid = r.side * ramp_mid;
  const double s0 = js::min(r.s_deck, r.s_ground);
  const double s1 = js::max(r.s_deck, r.s_ground);
  RoadSample rs = make_road_sample();
  std::vector<const RoadSeg*> cands;
  for (double s = s0; s <= s1; s += 8) {
    const HighwayPoint p = offset_at(e, s, mid);
    const double z = ramp_z(e, r, s);
    const CellIJ c = w.cell_at(p.x, p.y);
    const std::shared_ptr<const RoadView> view = w.road_view(c.i, c.j);
    cands.clear();
    view->near(Rect{p.x - 2, p.y - 2, p.x + 2, p.y + 2}, cands);
    sample_road_surface(cands, p.x + 0.5, p.y + 0.5, rs, w.seed);
    if (rs.kind == RoadKind::NONE || !rs.seg || rs.seg->road->id == r.arterial) continue;
    const double level = segment_level(w, *rs.seg, js::max(0.0, js::min(rs.seg->len, rs.along)));
    const bool carriage = rs.kind == RoadKind::CARRIAGE || rs.kind == RoadKind::MEDIAN;
    if (z - level < (carriage ? kRampClear : kWalkClear)) return true;
  }
  return false;
}

std::optional<HighwayRampAt> HighwayNetwork::ramp_at(const HighwayEdge& e, double s_pos, double d) const {
  const double ramp_out = ramp_in + vx(7);
  const double ad = js::abs(d);
  if (ad < ramp_in - 2 || ad > ramp_out + 1) return std::nullopt;
  for (const HighwayRamp& r : ramps(e)) {
    if (js::sign(d) != r.side) continue;
    // (from its deck end to its ground end, and an apron on into the arterial)
    const double dir = js::sign(r.s_ground - r.s_deck);
    const double a = (s_pos - r.s_deck) * dir;
    const double span = js::abs(r.s_ground - r.s_deck);
    if (a < -vx(2) || a > span + vx(2.25)) continue;
    const double t_raw = (s_pos - r.s_ground) / (r.s_deck - r.s_ground);
    return HighwayRampAt{js::round(ramp_z(e, r, s_pos)), &r, t_raw, ad > ramp_out - 3, ad < ramp_in + 1};
  }
  return std::nullopt;
}

bool HighwayNetwork::covers(double x, double y, double margin) const {
  const double outer = ramp_in + vx(7);
  const std::optional<HighwayNearest> nr = nearest(x, y, edges_near(Rect{x - 1, y - 1, x + 1, y + 1}), outer + margin);
  if (!nr) return false;
  const double ad = js::abs(nr->d);
  if (ad <= hw + margin) return true;
  // (a ramp's band, widened by the margin across; along it to its ends and aprons)
  return ramp_at(*nr->edge, nr->s, js::sign(nr->d) * js::max(ramp_in, js::min(outer, ad))).has_value();
}

std::optional<double> HighwayNetwork::underside(double x, double y) const {
  const std::vector<HighwayEdgePtr> edges = edges_near(Rect{x - 1, y - 1, x + 1, y + 1});
  const std::optional<HighwayNearest> nr = nearest(x + 0.5, y + 0.5, edges, ramp_in + vx(7) + 1);
  if (!nr) return std::nullopt;
  const double ad = js::abs(nr->d);
  if (ad <= hw) return js::round(nr->z) - kDeckT - 1;
  const std::optional<HighwayRampAt> rp = ramp_at(*nr->edge, nr->s, nr->d);
  if (!rp) return std::nullopt;
  const double gz = world.terrain->sample(x, y).h;
  return rp->z - gz < 10 ? -js::kInf : rp->z - 9;
}

bool HighwayNetwork::on_road(double x, double y) const {
  const World& w = world;
  const CellIJ c = w.cell_at(x, y);
  const std::shared_ptr<const RoadView> view = w.road_view(c.i, c.j);
  RoadSample rs = make_road_sample();
  sample_road_surface(view->near(Rect{x - 12, y - 12, x + 12, y + 12}), x + 0.5, y + 0.5, rs, w.seed);
  return rs.kind == RoadKind::CARRIAGE || rs.kind == RoadKind::MEDIAN || (rs.kind != RoadKind::NONE && rs.sdf_c < 10);
}

const std::vector<HighwayPier>& HighwayNetwork::piers(const HighwayEdge& e) const {
  return e.piers.get([&] {
    const double sp = vx(cfg.pier_spacing);
    const double H = hw;
    std::vector<HighwayPier> out;
    auto on_plateau = [&](double s) {
      for (const HighwayJunction& j : e.junctions)
        if (j.degree >= 3 && js::hypot(point_at(e, s).x - j.x, point_at(e, s).y - j.y) < j.r) return true;
      return false;
    };
    for (double s = sp / 2; s < e.total - sp / 4; s += sp) {
      if (on_plateau(s)) continue;
      std::optional<HighwayPier> pier;
      for (const double off : {0.0, 4.0, -4.0, 8.0, -8.0, 12.0, -12.0}) {
        const double ss = s + vx(off);
        if (ss < 0 || ss > e.total) continue;
        const HighwayPoint p = point_at(e, ss);
        if (on_road(p.x, p.y)) continue;
        pier = HighwayPier{ss, p.x, p.y, p.tx, p.ty, p.z, {0}, H - 8};
        break;
      }
      if (!pier) {
        // a street under the deck all along here: a portal astride it
        const HighwayPoint p = point_at(e, s);
        std::vector<double> cols;
        for (const double side : {-1.0, 1.0}) {
          double col = side * (H - 6);
          for (double q = 8; q <= H + 24; q += 4)
            if (!on_road(p.x - p.ty * q * side, p.y + p.tx * q * side)) {
              col = q * side;
              break;
            }
          cols.push_back(col);
        }
        double cap = H - 8;
        for (const double d : cols) cap = js::max(cap, js::abs(d) + 8);
        pier = HighwayPier{s, p.x, p.y, p.tx, p.ty, p.z, cols, cap};
      }
      out.push_back(std::move(*pier));
    }
    // a junction plateau: a column at its node, and a ring between the arms (under the deck, off
    // the streets)
    for (const HighwayJunction& j : e.junctions) {
      if (j.degree < 3) continue;
      const std::vector<HighwayEdgePtr> edges = edges_near(Rect{j.x - j.r, j.y - j.r, j.x + j.r, j.y + j.r});
      std::vector<std::array<double, 2>> spots{{j.x, j.y}};
      for (int k = 0; k < 12; ++k)
        for (const double f : {0.45, 0.85}) spots.push_back({j.x + js::cos((k * kPi) / 6) * j.r * f, j.y + js::sin((k * kPi) / 6) * j.r * f});
      for (const auto& spot : spots) {
        const double x = spot[0], y = spot[1];
        const std::optional<HighwayNearest> nr = nearest(x, y, edges, H);
        if (!nr || js::abs(nr->d) > H - 10 || on_road(x, y)) continue;
        out.push_back(HighwayPier{nr->s, x, y, 1, 0, j.z, {0}, 12});
      }
    }
    return out;
  });
}

const std::vector<double>& HighwayNetwork::ramp_piers(const HighwayEdge& e, const HighwayRamp& r) const {
  return r.piers.get([&] {
    std::vector<double> out;
    const double s0 = js::min(r.s_deck, r.s_ground);
    const double s1 = js::max(r.s_deck, r.s_ground);
    const double off = r.side * ramp_mid;
    for (double s = s0 + vx(8); s < s1; s += vx(24))
      for (const double d : {0.0, 2.0, -2.0, 4.0, -4.0, 6.0, -6.0, 8.0, -8.0}) {
        const double ss = s + vx(d);
        if (ss < s0 || ss > s1) continue;
        const HighwayPoint p = offset_at(e, ss, off);
        if (on_road(p.x, p.y)) continue;
        out.push_back(ss);
        break;
      }
    return out;
  });
}

std::vector<HighwayNetwork::MapEdge> HighwayNetwork::map_data(const Rect& rect) const {
  std::vector<MapEdge> out;
  for (const HighwayEdgePtr& e : edges_near(rect)) {
    MapEdge m;
    m.id = e->id;
    for (const PPoint& p : e->pts) m.pts.push_back({p.x, p.y});
    m.width = hw * 2;
    out.push_back(std::move(m));
  }
  return out;
}

// ---------------------------------------------------------------- rasterizer (highwaySource)

namespace {

bool wet_at(const World& world, double x, double y) { return world.is_wet(x, y, 4); }

void set_voxel(ChunkBuffer& chunk, int i, int j, int k, uint16_t m) { chunk.data[static_cast<size_t>(i + j * kP + k * kP2)] = m; }

// Tunnel bore: carve the clearance, line the walls and vault, light the ceiling.
void tunnel_column(ChunkBuffer& chunk, int i, int j, double ad, double H, double z_top, double s) {
  const IdxRange rk = chunk.range_z(z_top - kDeckT, z_top + kTunnelH + 3);
  const bool light_row = ad < 2 && mod(std::floor(s), 64) < 10;
  for (int k = rk.lo; k <= rk.hi; ++k) {
    const double z = chunk.wz(k);
    uint16_t m;
    if (z <= z_top - 1)
      m = MAT::HW_CONCRETE;
    else if (z == z_top)
      m = ad > H - 3 ? MAT::HW_CONCRETE : ad < 2 ? MAT::LINE_YELLOW : MAT::ASPHALT;
    else if (z > z_top + kTunnelH)
      m = MAT::HW_CONCRETE;  // vault
    else if (ad > H - 2)
      m = z <= z_top + 7 ? MAT::HW_BARRIER : MAT::TUNNEL_TILE;  // lined walls
    else if (z == z_top + kTunnelH && light_row)
      m = MAT::LIGHT_STRIP;
    else
      m = 0;
    set_voxel(chunk, i, j, k, m);
  }
}

// Beside the carriageway: retaining walls along cuttings (and at tunnel portals), grassy slopes
// down from embankments.
void earthworks(ChunkBuffer& chunk, int i, int j, double x, double y, double out, double z_top, double gz, const World& world) {
  if (gz > z_top + 1) {
    // cutting: a retaining wall two voxels thick, the ground beyond untouched
    if (out > 2) return;
    const double top = js::min(gz, z_top + kTunnelCover + 8);
    const IdxRange rk = chunk.range_z(z_top - 2, top);
    for (int k = rk.lo; k <= rk.hi; ++k) set_voxel(chunk, i, j, k, chunk.wz(k) == top ? MAT::PARAPET_CAP : MAT::CONCRETE_LIGHT);
    return;
  }
  const double h = z_top - kDeckT - gz;
  if (h <= 0 || z_top - gz >= kEmbank + kDeckT || wet_at(world, x, y)) return;
  // embankment slope 1:1.5 from the deck edge down to the ground
  const double zs = js::round(z_top - 2 - out / 1.5);
  if (zs <= gz) return;
  const IdxRange rk = chunk.range_z(gz + 1, zs);
  for (int k = rk.lo; k <= rk.hi; ++k) set_voxel(chunk, i, j, k, chunk.wz(k) > zs - chunk.s ? MAT::GRASS : MAT::DIRT);
}

uint16_t deck_marking(double ad, double s, double lanes_per_side, double lane) {
  const double inner = 3;  // median barrier half-width + gap
  if (ad < inner) return MAT::ASPHALT;
  const double x = ad - inner;
  const double edge_out = lanes_per_side * lane;
  if (x >= edge_out && x < edge_out + 1) return MAT::LINE_WHITE;  // outer edge line
  if (x < 1) return MAT::LINE_YELLOW;                             // inner edge line
  for (double k = 1; k < lanes_per_side; k += 1) {
    const double lx = k * lane;
    if (x >= lx && x < lx + 1 && mod(std::floor(s), 96) < 32) return MAT::LINE_WHITE;
  }
  return MAT::ASPHALT;
}

// Fill an oriented box (along tangent t, across n) by point sampling.
void stamp_oriented(ChunkBuffer& chunk, double cx, double cy, double tx, double ty, double nx, double ny, double a0, double a1, double b0, double b1, double z0,
                    double z1, uint16_t m) {
  const double r = js::max(js::abs(a0), js::abs(a1), js::abs(b0), js::abs(b1)) + 2;
  const IdxRange ri = chunk.range_x(cx - r, cx + r);
  const IdxRange rj = chunk.range_y(cy - r, cy + r);
  const IdxRange rk = chunk.range_z(z0, z1);
  if (ri.lo > ri.hi || rj.lo > rj.hi || rk.lo > rk.hi) return;
  for (int j = rj.lo; j <= rj.hi; ++j) {
    const double y = chunk.wy(j) + 0.5 - cy;
    for (int i = ri.lo; i <= ri.hi; ++i) {
      const double x = chunk.wx(i) + 0.5 - cx;
      const double a = x * tx + y * ty;
      const double b = x * nx + y * ny;
      if (a < a0 || a > a1 || b < b0 || b > b1) continue;
      for (int k = rk.lo; k <= rk.hi; ++k) set_voxel(chunk, i, j, k, m);
    }
  }
}

// One column of a ramp at its own level: a slab (an embankment near the ground), barriers and edge
// lines; through ground above it a cutting, a retaining wall along its outer edge. `gz` the
// ground's top there.
void draw_ramp(ChunkBuffer& chunk, int i, int j, const HighwayRampAt& rp, double gz) {
  const double z_top = rp.z;
  const bool cut = gz > z_top;
  const bool low = z_top - gz < 10;
  const double z0 = low ? js::min(gz, z_top) - 2 : z_top - 8;
  const double z1 = cut ? js::max(gz, z_top + 8) : z_top + 8;
  const IdxRange rk = chunk.range_z(z0, z1);
  for (int k = rk.lo; k <= rk.hi; ++k) {
    const double z = chunk.wz(k);
    uint16_t m = 0;
    const bool surf = z > z_top - chunk.s && z <= z_top;
    if (z < z_top && !surf)
      m = low ? MAT::GRAVEL : MAT::HW_CONCRETE;
    else if (surf)
      m = rp.outer || rp.inner ? MAT::LINE_WHITE : MAT::ASPHALT;
    else if (cut)
      m = rp.outer && z <= gz ? static_cast<uint16_t>(MAT::CONCRETE_LIGHT) : static_cast<uint16_t>(0);
    else if (rp.outer && rp.t > 0.05 && z <= z_top + 7 && z_top - gz > 3)
      m = MAT::HW_BARRIER;
    else if (rp.inner && rp.t > 0.05 && rp.t < 1 - kRampEase && z <= z_top + 7 && z_top - gz > 3)
      m = MAT::HW_BARRIER;
    if (m || z > z_top) set_voxel(chunk, i, j, k, m);
  }
}

}  // namespace

bool highway_z_range(const World& world, const Rect& rect, double* z0, double* z1) {
  const HighwayNetwork& hn = *world.highways;
  double lo = js::kInf;
  double hi = -js::kInf;
  for (const HighwayEdgePtr& e : hn.edges_near(rect)) {
    for (const HighwaySeg* s : e->grid.query(rect)) {
      lo = js::min(lo, s->az - 200, s->bz - 200);
      hi = js::max(hi, s->az + kTunnelH + 6, s->bz + kTunnelH + 6);
    }
  }
  if (lo == js::kInf) return false;
  *z0 = js::max(lo, -200.0);
  *z1 = hi;
  return true;
}

void rasterize_highways(const World& world, ChunkBuffer& chunk, const int32_t* tile_z) {
  const HighwayNetwork& hn = *world.highways;
  const Box3 box = chunk.world_box();
  const std::vector<HighwayEdgePtr> edges = hn.edges_near(Rect{box.x0, box.y0, box.x1, box.y1});
  if (edges.empty()) return;
  const double H = hn.hw;
  const bool coarse = chunk.lod >= 2;
  const double lanes_per_side = hn.cfg.lanes_per_side;
  const double lane = vx(hn.cfg.lane_width);
  for (int j = 0; j < kP; ++j) {
    const double y = chunk.wy(j);
    for (int i = 0; i < kP; ++i) {
      const double x = chunk.wx(i);
      const std::optional<HighwayNearest> nr = hn.nearest(x + 0.5, y + 0.5, edges, H + 4 + 60);
      if (!nr) continue;
      const double ad = js::abs(nr->d);
      const int col = i + j * kP;
      const double gz = tile_z ? static_cast<double>(tile_z[col]) : js::round(world.terrain->sample(x, y).h);
      if (ad > H) {
        const std::optional<HighwayRampAt> rp = hn.ramp_at(*nr->edge, nr->s, nr->d);
        if (rp)
          draw_ramp(chunk, i, j, *rp, gz);
        else
          earthworks(chunk, i, j, x, y, ad - H, js::round(nr->z), gz, world);
        continue;
      }
      const double z_top = js::round(nr->z);
      // through a hill: open cutting, or a tunnel under a mountain (bores are invisible from afar:
      // coarse LODs leave the mountain whole)
      if (gz > z_top + kTunnelCover) {
        if (chunk.lod <= 2) tunnel_column(chunk, i, j, ad, H, z_top, nr->s);
        continue;
      }
      if (gz > z_top) {
        const IdxRange rk = chunk.range_z(z_top + 1, gz);
        for (int k = rk.lo; k <= rk.hi; ++k) set_voxel(chunk, i, j, k, 0);
      } else if (z_top - kDeckT - gz < kEmbank && z_top - kDeckT > gz && !wet_at(world, x, y)) {
        // low deck: an earth embankment instead of air under the slab
        const IdxRange rk = chunk.range_z(gz + 1, z_top - kDeckT - 1);
        for (int k = rk.lo; k <= rk.hi; ++k) set_voxel(chunk, i, j, k, MAT::GRAVEL);
      }
      // a junction plateau (a crossing of highways, a terminus): one level deck, no barriers across
      // it, a railing round it (where the edge of this arm is no other arm's carriageway)
      const HighwayJunction* jn = nullptr;
      for (const HighwayEdgePtr& e : edges) {
        for (const HighwayJunction& q : e->junctions)
          if (js::hypot(x + 0.5 - q.x, y + 0.5 - q.y) < q.r) {
            jn = &q;
            break;
          }
        if (jn) break;
      }
      const bool junction = jn != nullptr;
      bool rim = false;
      if (jn && jn->degree >= 3 && ad > H - 3) {
        std::vector<HighwayEdgePtr> others;
        for (const HighwayEdgePtr& e : edges)
          if (e.get() != nr->edge) others.push_back(e);
        const std::optional<HighwayNearest> other = hn.nearest(x + 0.5, y + 0.5, others, H);
        rim = !other || js::abs(other->d) > H - 3;
      }
      // barrier opening where a ramp merges at deck level
      bool merge = junction;
      if (ad > H - 3) {
        const std::optional<HighwayRampAt> rp = hn.ramp_at(*nr->edge, nr->s, js::sign(nr->d) * (H + 5));
        if (rp && rp->t > 1 - kRampEase && js::abs(rp->z - z_top) <= 1) merge = true;
      }
      const IdxRange rk = chunk.range_z(z_top - kDeckT, z_top + 8);
      for (int k = rk.lo; k <= rk.hi; ++k) {
        const double z = chunk.wz(k);
        uint16_t m = 0;
        // surface = the top representative voxel at this LOD (thin layers must survive point sampling)
        const bool surf = z > z_top - chunk.s && z <= z_top;
        if (z < z_top && !surf)
          m = z == z_top - kDeckT ? MAT::CONCRETE_DARK : MAT::HW_CONCRETE;
        else if (surf) {
          if (ad > H - 3 && !junction)
            m = MAT::HW_CONCRETE;
          else
            m = coarse || junction ? static_cast<uint16_t>(MAT::ASPHALT) : deck_marking(ad, nr->s, lanes_per_side, lane);
        } else if (ad > H - 3 && ad <= H)
          m = z <= z_top + 7 && (!merge || rim) ? static_cast<uint16_t>(MAT::HW_BARRIER) : static_cast<uint16_t>(0);
        else if (ad < 2 && z <= z_top + 7 && !junction)
          m = MAT::HW_BARRIER;
        if (m || (merge && !rim && z > z_top)) set_voxel(chunk, i, j, k, m);
      }
    }
  }
  // piers
  for (const HighwayEdgePtr& e : edges) {
    for (const HighwayPier& p : hn.piers(*e)) {
      const double reach = p.cap + 16;
      if (p.x < box.x0 - reach || p.x > box.x1 + reach || p.y < box.y0 - reach || p.y > box.y1 + reach) continue;
      const double z_top = js::round(p.z);
      const double nx = -p.ty;
      const double ny = p.tx;
      // columns: 2.0 m across the deck, 1.25 m along, down into the ground
      for (const double d : p.cols) {
        const double cx = p.x + nx * d;
        const double cy = p.y + ny * d;
        const double gz = js::round(world.terrain->sample(cx, cy).h) - 4;
        stamp_oriented(chunk, cx, cy, p.tx, p.ty, nx, ny, -5, 5, -8, 8, gz, z_top - kDeckT - 1, MAT::HW_CONCRETE);
      }
      // the cap: a hammerhead under the deck (a portal's beam astride the street)
      double lo = -p.cap;
      double hi = p.cap;
      for (const double d : p.cols) {
        lo = js::min(lo, d - 8);
        hi = js::max(hi, d + 8);
      }
      stamp_oriented(chunk, p.x, p.y, p.tx, p.ty, nx, ny, -6, 6, lo, hi, z_top - kDeckT - 7, z_top - kDeckT - 1, MAT::HW_CONCRETE);
    }
    // ramp piers where the ramp is well above ground
    for (const HighwayRamp& r : hn.ramps(*e)) {
      for (const double sp : hn.ramp_piers(*e, r)) {
        const HighwayPoint p = point_at(*e, sp);
        const double off = r.side * hn.ramp_mid;
        const double px = p.x - p.ty * off;
        const double py = p.y + p.tx * off;
        if (px < box.x0 - 40 || px > box.x1 + 40 || py < box.y0 - 40 || py > box.y1 + 40) continue;
        const std::optional<HighwayRampAt> rp = hn.ramp_at(*e, sp, off);
        if (!rp) continue;
        const double gz = js::round(world.terrain->sample(px, py).h);
        if (rp->z - gz < 20) continue;
        stamp_oriented(chunk, px, py, p.tx, p.ty, -p.ty, p.tx, -4, 4, -5, 5, gz - 3, rp->z - 9, MAT::HW_CONCRETE);
      }
    }
  }
}

}  // namespace svx::city
