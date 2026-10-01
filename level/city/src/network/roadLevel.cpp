// svx_city — network/roadLevel.hpp (voxel_city network/roadLevel.js), and createWorld.js's
// streetLevel.
#include "network/roadLevel.hpp"

#include <array>
#include <string_view>
#include <utility>
#include <vector>

#include "core/js.hpp"
#include "core/placement.hpp"
#include "terrain/terrain.hpp"
#include "world/World.hpp"

namespace svx::city {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kStep = 64;            // 8 m
constexpr double kJunctionBlend = 96;   // 12 m
constexpr double kNodeR = 160;          // 20 m
constexpr double kSteepQ = 0.06;        // steps steeper than this are what a table grade takes over

struct ClassGrades {
  std::string_view cls;
  double grade, steep;
  int pitch_grade, pitch_steep;
};
// GRADE, STEEP, and the angled world's PITCH_GRADE, PITCH_STEEP (indices into the pitch table)
constexpr ClassGrades kGrades[] = {
    {"arterial", 0.08, 0.1, 1, 2}, {"collector", 0.1, 0.13, 2, 3},   {"local", 0.12, 0.16, 3, 5}, {"village", 0.13, 0.16, 4, 5}, {"alley", 0.14, 0.17, 4, 5},
    {"lane", 0.16, 0.18, 5, 6},    {"pedestrian", 0.12, 0.17, 3, 5}, {"rural", 0.12, 0.14, 3, 4}, {"path", 0.15, 0.18, 4, 5},
};
const ClassGrades* grades_of(std::string_view cls) {
  for (const ClassGrades& g : kGrades)
    if (g.cls == cls) return &g;
  return nullptr;
}

double pitch_grade(int i) { return pitches()[static_cast<size_t>(i)].s / pitches()[static_cast<size_t>(i)].c; }

// x !== false on a config flag
bool not_false(const Value& v) { return !(v.is_bool() && !v.truthy()); }

// Angled streets on (ANGLED_WORLD_PLAN.md S1)?
bool angled_levels(const Value& config) {
  const Value& a = config["world"]["angles"];
  return a["enabled"].truthy() && not_false(a["features"]["roads"]);
}
// Do road profiles keep to the pitch table (the angled world's pitched roads, S4)?
bool table_profiles(const Value& config) {
  const Value& a = config["world"]["angles"];
  return a["enabled"].truthy() && not_false(a["features"]["roads"]) && not_false(a["features"]["ramps"]);
}

// Comfortable and steepest grade of a road class: [g0, g1].
std::array<double, 2> grade_limits(const Value& config, std::string_view cls) {
  const ClassGrades* g = grades_of(cls);
  if (angled_levels(config)) {
    const double g0 = pitch_grade(g ? g->pitch_grade : 3);
    return {g0, js::max(g0, pitch_grade(g ? g->pitch_steep : 5))};
  }
  const double g0 = g ? g->grade : 0.12;
  return {g0, js::max(g0, g ? g->steep : 0.16)};
}

// Level of a road node: the ground averaged over a disc of NODE_R round it (a pure function of the
// node's position, so every road meeting there agrees).
double node_level(const World& world, double x, double y) {
  const Terrain& t = *world.terrain;
  double sum = t.sample(x, y).h * 2;
  double w = 2;
  for (int k = 0; k < 8; ++k) {
    const double a = (k / 8.0) * kPi * 2;
    for (const double r : {kNodeR / 2, kNodeR}) {
      sum += t.sample(x + js::cos(a) * r, y + js::sin(a) * r).h;
      w += 1;
    }
  }
  return sum / w;
}

// Level at arc s of a profile with knots of its own (ks increasing), held flat past its ends.
double knot_at(const std::vector<double>& ks, const std::vector<double>& kz, double s) {
  const int n = static_cast<int>(ks.size());
  if (s <= ks[0]) return kz[0];
  if (s >= ks[static_cast<size_t>(n - 1)]) return kz[static_cast<size_t>(n - 1)];
  int lo = 0;
  int hi = n - 1;
  while (hi - lo > 1) {
    const int mid = (lo + hi) >> 1;
    if (ks[static_cast<size_t>(mid)] <= s)
      lo = mid;
    else
      hi = mid;
  }
  const double span = ks[static_cast<size_t>(hi)] - ks[static_cast<size_t>(lo)];
  return span > 0 ? kz[static_cast<size_t>(lo)] + ((kz[static_cast<size_t>(hi)] - kz[static_cast<size_t>(lo)]) * (s - ks[static_cast<size_t>(lo)])) / span
                  : kz[static_cast<size_t>(hi)];
}

// Pitch a profile to the grade table (ANGLED_WORLD_PLAN.md §4.1, S4): every steep stretch (steps
// of one sign steeper than STEEP_Q) keeps its two end levels and becomes one run at the gentlest
// table grade that climbs it, with level landings of equal length either side; gentle stretches
// stay as fitted. The profile gets knots of its own.
void quantize_profile(RoadProfile& prof, const std::vector<double>& at) {
  const std::vector<float>& z = prof.z;
  const int n = prof.n;
  auto zd = [&](int i) { return static_cast<double>(z[static_cast<size_t>(i)]); };
  std::vector<double> ks{at[0]};
  std::vector<double> kz{zd(0)};
  auto grade = [&](int i) { return (zd(i) - zd(i - 1)) / js::or_(at[static_cast<size_t>(i)] - at[static_cast<size_t>(i - 1)], 1); };
  int i = 1;
  while (i < n) {
    const double g = grade(i);
    if (js::abs(g) < kSteepQ) {
      ks.push_back(at[static_cast<size_t>(i)]);
      kz.push_back(zd(i));
      i += 1;
      continue;
    }
    int i1 = i;
    while (i1 + 1 < n && js::abs(grade(i1 + 1)) >= kSteepQ && js::sign(grade(i1 + 1)) == js::sign(g)) i1 += 1;
    const double s0 = at[static_cast<size_t>(i - 1)];
    const double s1 = at[static_cast<size_t>(i1)];
    const double dz = zd(i1) - zd(i - 1);
    const double need = js::abs(dz) / (s1 - s0);
    double G = 0;
    for (size_t k = 1; k < pitches().size() && !js::truthy(G); ++k)
      if (pitches()[k].s / pitches()[k].c >= need) G = pitches()[k].s / pitches()[k].c;
    if (js::truthy(G)) {
      const double land = (s1 - s0 - js::abs(dz) / G) / 2;
      ks.push_back(s0 + land);
      ks.push_back(s1 - land);
      kz.push_back(zd(i - 1));
      kz.push_back(zd(i1));
    }
    ks.push_back(s1);
    kz.push_back(zd(i1));
    i = i1 + 1;
  }
  prof.knots = true;
  prof.ks = std::move(ks);
  prof.kz = std::move(kz);
}

// A road's profile between end levels z0 and z1 (none: the node levels).
RoadProfile fit_profile(const World& world, const Road& road, std::optional<double> z0, std::optional<double> z1) {
  const std::vector<PPoint>& pts = road.pts;
  std::vector<double> acc{0.0};
  for (size_t k = 1; k < pts.size(); ++k) acc.push_back(acc[k - 1] + js::hypot(pts[k].x - pts[k - 1].x, pts[k].y - pts[k - 1].y));
  const double L = acc.back();
  const int n = static_cast<int>(js::max(2.0, std::ceil(L / kStep) + 1));
  std::vector<double> at(static_cast<size_t>(n), 0.0);
  std::vector<double> raw(static_cast<size_t>(n), 0.0);
  const int last_seg = static_cast<int>(pts.size()) - 2;
  int k = 0;
  for (int i = 0; i < n; ++i) {
    const double s = i == n - 1 ? L : js::min(L, i * kStep);
    at[static_cast<size_t>(i)] = s;
    while (k < last_seg && acc[static_cast<size_t>(k + 1)] < s) k += 1;
    const double seg_l = js::or_(acc[static_cast<size_t>(k + 1)] - acc[static_cast<size_t>(k)], 1);
    const double t = js::max(0.0, js::min(1.0, (s - acc[static_cast<size_t>(k)]) / seg_l));
    const PPoint& p = pts[static_cast<size_t>(k)];
    const PPoint& q = pts[static_cast<size_t>(k + 1)];
    const double x = p.x + (q.x - p.x) * t;
    const double y = p.y + (q.y - p.y) * t;
    raw[static_cast<size_t>(i)] = world.terrain->sample(x, y).h;
  }
  // smooth out the small bumps (a ±16 m window), keep the node heights
  std::vector<double> T(static_cast<size_t>(n), 0.0);
  for (int i = 0; i < n; ++i) {
    double sum = 0;
    double cnt = 0;
    for (int q = std::max(0, i - 2); q <= std::min(n - 1, i + 2); ++q) {
      sum += raw[static_cast<size_t>(q)];
      cnt += 1;
    }
    T[static_cast<size_t>(i)] = sum / cnt;
  }
  // the end nodes: the ground round the node (the same for every road meeting there), or the
  // through road's level
  T[0] = z0 ? *z0 : node_level(world, pts.front().x, pts.front().y);
  T[static_cast<size_t>(n - 1)] = z1 ? *z1 : node_level(world, pts.back().x, pts.back().y);
  // the grade allowed on each step (g[i]: from sample i-1 to i): the comfortable grade, up to the
  // steepest where the ground is that steep
  const std::array<double, 2> lim = grade_limits(world.config, road.cls);
  const double g0 = lim[0];
  const double g1 = lim[1];
  std::vector<double> g(static_cast<size_t>(n), 0.0);
  auto slope = [&](int i) {
    const int a = std::max(0, i - 2);
    const int b = std::min(n - 1, i + 2);
    return b > a ? js::abs(T[static_cast<size_t>(b)] - T[static_cast<size_t>(a)]) / js::or_(at[static_cast<size_t>(b)] - at[static_cast<size_t>(a)], 1) : 0.0;
  };
  double reach = 0;
  for (int i = 1; i < n; ++i) {
    g[static_cast<size_t>(i)] = js::max(g0, js::min(g1, js::max(slope(i - 1), slope(i))));
    reach += g[static_cast<size_t>(i)] * (at[static_cast<size_t>(i)] - at[static_cast<size_t>(i - 1)]);
  }
  // the pinned ends must be reachable: a short road between very different nodes grades steeper
  const double need = js::abs(T[static_cast<size_t>(n - 1)] - T[0]) * 1.02;
  if (need > reach && reach > 0)
    for (int i = 1; i < n; ++i) g[static_cast<size_t>(i)] *= need / reach;
  auto d = [&](int i) { return g[static_cast<size_t>(i)] * (at[static_cast<size_t>(i)] - at[static_cast<size_t>(i - 1)]); };
  // all-cut (upper) and all-fill (lower) envelopes, then their mean
  std::vector<double> up = T;
  std::vector<double> lo = T;
  for (int i = 1; i < n; ++i) {
    up[static_cast<size_t>(i)] = js::min(up[static_cast<size_t>(i)], up[static_cast<size_t>(i - 1)] + d(i));
    lo[static_cast<size_t>(i)] = js::max(lo[static_cast<size_t>(i)], lo[static_cast<size_t>(i - 1)] - d(i));
  }
  for (int i = n - 2; i >= 0; --i) {
    up[static_cast<size_t>(i)] = js::min(up[static_cast<size_t>(i)], up[static_cast<size_t>(i + 1)] + d(i + 1));
    lo[static_cast<size_t>(i)] = js::max(lo[static_cast<size_t>(i)], lo[static_cast<size_t>(i + 1)] - d(i + 1));
  }
  std::vector<double> z(static_cast<size_t>(n), 0.0);
  for (int i = 0; i < n; ++i) z[static_cast<size_t>(i)] = (up[static_cast<size_t>(i)] + lo[static_cast<size_t>(i)]) / 2;
  // pin the ends: within the cones of reach from both end nodes
  std::vector<double> from0(static_cast<size_t>(n), 0.0);
  for (int i = 1; i < n; ++i) from0[static_cast<size_t>(i)] = from0[static_cast<size_t>(i - 1)] + d(i);
  const double total = from0[static_cast<size_t>(n - 1)];
  for (int i = 0; i < n; ++i) {
    const double a = from0[static_cast<size_t>(i)];
    const double b = total - a;
    const double hi = js::min(T[0] + a, T[static_cast<size_t>(n - 1)] + b);
    const double lw = js::max(T[0] - a, T[static_cast<size_t>(n - 1)] - b);
    z[static_cast<size_t>(i)] = js::max(lw, js::min(hi, z[static_cast<size_t>(i)]));
  }
  RoadProfile prof;
  prof.z.reserve(static_cast<size_t>(n));
  for (const double v : z) prof.z.push_back(js::f32(v));
  prof.L = L;
  prof.n = n;
  if (table_profiles(world.config)) quantize_profile(prof, at);
  return prof;
}

// The plain profile (between the node levels), cached on the road.
const RoadProfile& plain_profile(const World& world, const Road& road) {
  return road.prof0.get([&] { return fit_profile(world, road, std::nullopt, std::nullopt); });
}

// The road a node of `road` lies on without ending there (a T), with the arc position.
struct Through {
  RoadPtr road;
  double s = 0, rank = 0;
};
std::optional<Through> through_at(const World& world, const Road& road, const PPoint& p) {
  const CellIJ c = world.cell_at(p.x, p.y);
  const std::shared_ptr<const RoadView> view = world.road_view(c.i, c.j);
  std::optional<Through> best;
  for (const RoadSeg* sp : view->near(Rect{p.x - 8, p.y - 8, p.x + 8, p.y + 8})) {
    const RoadSeg& s = *sp;
    if (same_road(*s.road, road)) continue;
    const double t = js::max(0.0, js::min(s.len, (p.x - s.ax) * s.dx + (p.y - s.ay) * s.dy));
    const double d = js::hypot(p.x - (s.ax + s.dx * t), p.y - (s.ay + s.dy * t));
    if (d > 4) continue;
    const std::vector<PPoint>& q = s.road->pts;
    const bool at_end = js::hypot(p.x - q.front().x, p.y - q.front().y) < 6 || js::hypot(p.x - q.back().x, p.y - q.back().y) < 6;
    if (at_end) return std::nullopt;  // a corner or a crossing of road ends: the node level
    if (!best || s.rank > best->rank || (s.rank == best->rank && js::compare(s.road->id, best->road->id) < 0)) best = Through{s.road, s.s0 + t, s.rank};
  }
  return best;
}

// Level of a junction: the dominant road's profile where the two meet (cached on the annotation).
// At a T the road that goes on through keeps its level and the one ending there meets it; at a
// crossing or a corner the more important road does.
const RoadJunction::Level& junction_level(const World& world, const RoadSeg& seg, const RoadJunction& j) {
  return j.level.get([&] {
    const RoadSeg* o = j.other;
    const bool self_dominant =
        !o || (j.s_ends != j.c_ends ? j.c_ends : seg.rank > o->rank || (seg.rank == o->rank && js::compare(seg.road->id, o->road->id) <= 0));
    RoadJunction::Level l;
    l.dom = self_dominant;
    l.z = self_dominant ? profile_at(road_profile(world, *seg.road), seg.s0 + j.s) : profile_at(road_profile(world, *o->road), o->s0 + j.t_other);
    return l;
  });
}
double junction_z(const World& world, const RoadSeg& seg, const RoadJunction& j) { return junction_level(world, seg, j).z; }

// Half-length of a junction's level box along a road: the crossing road's right-of-way for the
// road that meets it, only its carriageway for the dominant road.
double box_half(const World& world, const RoadJunctionRef& e) {
  const RoadJunction& j = *e.j;
  return junction_level(world, *e.seg, j).dom ? js::max(8.0, j.hc) : j.hr;
}

// Blend length of a junction (voxels): it lengthens with the offset the road needs there.
double blend_of(const World& world, const RoadJunctionRef& e, const RoadProfile& prof) {
  return e.j->blend.get([&] {
    const double jz = junction_z(world, *e.seg, *e.j);
    const double h = box_half(world, e);
    const double d = js::max(js::abs(jz - profile_at(prof, e.at - h)), js::abs(jz - profile_at(prof, e.at + h)));
    return js::min(kJunctionBlend * 8, js::max(kJunctionBlend, (d * 1.5) / 0.035));
  });
}

// A road's segment as its owner cell's view has it (the angled world): that view (held) and the
// segment, or the asking segment itself where the owner's view has none.
struct OwnSeg {
  std::shared_ptr<const RoadView> view;
  const RoadSeg* seg = nullptr;
};
OwnSeg own_seg(const World& world, const RoadSeg& seg) {
  const std::array<double, 2>& home = *seg.road->home;
  std::shared_ptr<const RoadView> view = world.road_view(home[0], home[1]);
  const int k = seg.own.get([&] {
    for (size_t q = 0; q < view->segs.size(); ++q)
      if (same_road(*view->segs[q].road, *seg.road) && view->segs[q].idx == seg.idx) return static_cast<int>(q);
    return -1;
  });
  if (k < 0) return {nullptr, &seg};
  const RoadSeg* own = &view->segs[static_cast<size_t>(k)];
  return {std::move(view), own};
}

std::vector<RoadJunctionRef> sorted_by_arc(const std::vector<RoadJunctionRef>& rj) {
  std::vector<RoadJunctionRef> v = rj;
  js::sort(v, [](const RoadJunctionRef& p, const RoadJunctionRef& q) { return p.at - q.at; });
  return v;
}

// Road level near its junctions in the angled world, one stretch between two neighbouring
// junctions at a time (continuous however their boxes overlap): over p's box the road lies at p's
// level, over q's at q's, blending to its own profile between them; where the gap between the
// boxes is too short for their level difference it eases from one level to the other over the
// length the class's steepest grade needs (a smoothstep), or the boxes' overlap, whichever is
// longer, centred on the gap and never past the junctions' centres.
double junction_level_at(const World& world, const RoadSeg& seg, const RoadJunctions* rjs, const std::vector<RoadJunctionRef>& rj, const RoadProfile& prof,
                         double a, double base) {
  std::vector<RoadJunctionRef> local;
  const std::vector<RoadJunctionRef>* listp;
  if (rjs) {
    listp = &rjs->sorted.get([&] { return sorted_by_arc(rj); });
  } else {
    local = sorted_by_arc(rj);
    listp = &local;
  }
  const std::vector<RoadJunctionRef>& list = *listp;
  // (p the last junction at or before a, q the next)
  int lo = 0;
  int hi = static_cast<int>(list.size());
  while (lo < hi) {
    const int mid = (lo + hi) >> 1;
    if (list[static_cast<size_t>(mid)].at <= a)
      lo = mid + 1;
    else
      hi = mid;
  }
  const RoadJunctionRef* p = lo > 0 ? &list[static_cast<size_t>(lo - 1)] : nullptr;
  const RoadJunctionRef* q = lo < static_cast<int>(list.size()) ? &list[static_cast<size_t>(lo)] : nullptr;
  const double hp = p ? box_half(world, *p) : 0;
  const double hq = q ? box_half(world, *q) : 0;
  const double zp = p ? junction_z(world, *p->seg, *p->j) : 0;
  const double zq = q ? junction_z(world, *q->seg, *q->j) : 0;
  const double e0 = p ? p->at + hp : -js::kInf;
  const double e1 = q ? q->at - hq : js::kInf;
  if (p && q) {
    const double need = (1.5 * js::abs(zq - zp)) / grade_limits(world.config, *seg.cls)[1];
    if (e1 - e0 < need) {
      const double len = js::max(need, e0 - e1);
      const double c = js::max(p->at, js::min(q->at, (e0 + e1) / 2));
      const double l0 = js::max(p->at, c - len / 2);
      const double l1 = js::min(q->at, c + len / 2);
      if (a <= l0 || l1 <= l0) return zp;
      if (a >= l1) return zq;
      const double u = (a - l0) / (l1 - l0);
      return zp + (zq - zp) * u * u * (3 - 2 * u);
    }
  }
  if (a <= e0) return zp;
  if (a >= e1) return zq;
  double bl = p ? blend_of(world, *p, prof) : 0;
  double br = q ? blend_of(world, *q, prof) : 0;
  if (p && q && e1 - e0 < bl + br) {
    const double k = (e1 - e0) / (bl + br);
    bl *= k;
    br *= k;
  }
  double off = 0;
  if (p && bl > 0) {
    const double u = (a - e0) / bl;
    if (u < 1) off += (zp - profile_at(prof, e0)) * (1 - u * u * (3 - 2 * u));
  }
  if (q && br > 0) {
    const double u = (e1 - a) / br;
    if (u < 1) off += (zq - profile_at(prof, e1)) * (1 - u * u * (3 - 2 * u));
  }
  return base + off;
}

}  // namespace

const RoadProfile& road_profile(const World& world, const Road& road) {
  return road.prof.get([&] {
    // An end that meets a through road (a T) is pinned to that road's own level there (its
    // profile fitted between plain node levels: no chains, no cycles); other ends to the node level.
    auto end_level = [&](const PPoint& p) {
      const std::optional<Through> t = through_at(world, road, p);
      return t ? profile_at(plain_profile(world, *t->road), t->s) : node_level(world, p.x, p.y);
    };
    const double z0 = end_level(road.pts.front());
    const double z1 = end_level(road.pts.back());
    return fit_profile(world, road, z0, z1);
  });
}

double profile_at(const RoadProfile& prof, double s) {
  if (prof.knots) return knot_at(prof.ks, prof.kz, s);
  const double f = js::max(0.0, js::min(prof.n - 1.000001, s / kStep));
  if (f != f) return js::kNaN;  // (JS: z[NaN] is undefined)
  const double i = std::floor(f);
  const double t = f - i;
  return static_cast<double>(prof.z[static_cast<size_t>(i)]) * (1 - t) + static_cast<double>(prof.z[static_cast<size_t>(js::min(i + 1, prof.n - 1.0))]) * t;
}

double segment_level(const World& world, const RoadSeg& seg_in, double along) {
  // (the angled world: the road's own segment in its owner's view, which sees every junction along
  // it, so no view gives it another level)
  OwnSeg own{nullptr, &seg_in};
  if (seg_in.road->home) own = own_seg(world, seg_in);
  const RoadSeg& seg = *own.seg;
  const RoadProfile& prof = road_profile(world, *seg.road);
  const double a = seg.s0 + along;
  const double base = profile_at(prof, a);
  std::vector<RoadJunctionRef> local;
  if (!seg.rj)
    for (const RoadJunction& j : seg.jn) local.push_back({&j, &seg, seg.s0 + j.s});
  const std::vector<RoadJunctionRef>& rj = seg.rj ? seg.rj->list : local;
  if (rj.empty()) return base;
  // (the angled world: junction by junction, continuous however their boxes overlap)
  if (angled_levels(world.config)) return junction_level_at(world, seg, seg.rj, rj, prof, a, base);
  const RoadJunctionRef* in_box = nullptr;
  const RoadJunctionRef* in_box2 = nullptr;
  const RoadJunctionRef* left = nullptr;
  const RoadJunctionRef* right = nullptr;
  for (const RoadJunctionRef& e : rj) {
    const double hr = box_half(world, e);
    const double d = a - e.at;
    if (js::abs(d) <= hr) {
      if (!in_box || js::abs(d) < js::abs(a - in_box->at)) {
        in_box2 = in_box;
        in_box = &e;
      } else if (!in_box2 || js::abs(d) < js::abs(a - in_box2->at)) {
        in_box2 = &e;
      }
    } else if (d > 0) {
      if (!left || e.at + hr > left->at + box_half(world, *left)) left = &e;
    } else if (!right || e.at - hr < right->at - box_half(world, *right)) {
      right = &e;
    }
  }
  if (in_box && in_box2 && js::abs(in_box->at - in_box2->at) > 0.5) {
    // two junction boxes overlap (streets meeting a few metres apart): the level eases from one
    // junction's to the other's across the overlap
    const RoadJunctionRef* p = in_box->at < in_box2->at ? in_box : in_box2;
    const RoadJunctionRef* q = in_box->at < in_box2->at ? in_box2 : in_box;
    const double lo = q->at - box_half(world, *q);
    const double hi = p->at + box_half(world, *p);
    const double u = hi > lo ? js::max(0.0, js::min(1.0, (a - lo) / (hi - lo))) : 0.5;
    const double zp = junction_z(world, *p->seg, *p->j);
    return zp + (junction_z(world, *q->seg, *q->j) - zp) * u * u * (3 - 2 * u);
  }
  if (in_box) return junction_z(world, *in_box->seg, *in_box->j);
  double bl = left ? blend_of(world, *left, prof) : 0;
  double br = right ? blend_of(world, *right, prof) : 0;
  const double hl = left ? box_half(world, *left) : 0;
  const double hrr = right ? box_half(world, *right) : 0;
  if (left && right) {
    const double gap = right->at - hrr - (left->at + hl);
    if (gap < bl + br) {
      const double k = gap / (bl + br);
      bl *= k;
      br *= k;
    }
  }
  double off = 0;
  if (left && bl > 0) {
    const double edge = left->at + hl;
    const double u = (a - edge) / bl;
    if (u < 1) off += (junction_z(world, *left->seg, *left->j) - profile_at(prof, edge)) * (1 - u * u * (3 - 2 * u));
  }
  if (right && br > 0) {
    const double edge = right->at - hrr;
    const double u = (edge - a) / br;
    if (u < 1) off += (junction_z(world, *right->seg, *right->j) - profile_at(prof, edge)) * (1 - u * u * (3 - 2 * u));
  }
  return base + off;
}

std::optional<RoadLevel> road_level_at(const World& world, const RoadView& view, double x, double y, double reach) {
  std::optional<RoadLevel> best;
  double best_score = js::kInf;
  for (const RoadSeg* sp : view.near(Rect{x - reach, y - reach, x + reach, y + reach})) {
    const RoadSeg& s = *sp;
    const double vx = x - s.ax;
    const double vy = y - s.ay;
    const double t = js::max(0.0, js::min(s.len, vx * s.dx + vy * s.dy));
    const double d = js::hypot(x - (s.ax + s.dx * t), y - (s.ay + s.dy * t));
    if (d > s.hr + reach) continue;
    const double score = d - s.hc;
    if (score < best_score) {
      best_score = score;
      best = RoadLevel{0, &s, t, d, false};
    }
  }
  if (!best) return std::nullopt;
  best->z = segment_level(world, *best->seg, best->along);
  best->sidewalk = best->seg->sidewalk > 0;
  return best;
}

// createWorld.js streetLevel: the nearest road's graded level, else the terrain.
double World::street_level(double x, double y) const {
  const CellIJ c = cell_at(x, y);
  const std::shared_ptr<const RoadView> view = road_view(c.i, c.j);
  const std::optional<RoadLevel> r = road_level_at(*this, *view, x, y, 24);
  return r ? r->z : terrain->sample(x, y).h;
}

}  // namespace svx::city
