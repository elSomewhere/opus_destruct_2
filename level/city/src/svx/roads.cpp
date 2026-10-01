// svx_city — the city's streets as structvox's road network (svx/city/roads.hpp, svx/roads.hpp;
// voxel_city svx/roads.js).
//
// The streets as structvox's RoadNetwork (game/include/svx/game/roads.hpp: the lanes its traffic
// drives, the walkways its people walk, the kerbside places cars park), after the conventions of
// structvox's own drive city (procgen/src/drive_city.cpp):
//
//   - a lane is a straight run one way: a road is cut at its junctions into links, a link at its
//     bends into pieces, and a lane stops at the kerb line of the road it meets; lane k counts from
//     the centre line (or the median) outwards;
//   - a junction is every road meeting at one place, as each of them sees it (roads cut at a
//     cell's border meet their continuations through the roads they cross), and junctions too
//     close for a lane between them (a jog, a side road just past a crossing) are one;
//   - next: straight on keeps the lane (the nearest there is), a right turn leaves from the kerb
//     lane to the kerb lane, a left one from the inner lane to the inner lane (any lane, where
//     there is no straight on); never a U-turn, but at a dead end;
//   - green: a signalled junction gives each heading of its roads a phase of 17 s (green 14 s), u
//     offset per junction; a crossing is open the first 4 s of the next phase; elsewhere always;
//   - walks: along each side of a link on the middle of its sidewalk, from corner to corner (a
//     corner is where two walking lines cross, worked out alike from either road, so walks meet
//     exactly), between corners a junction puts on one side, over the road at its crosswalks; down
//     the middle of alleys and single-track lanes, and over a street they cross;
//   - parking: in a road's parking strips, one place every 6 m from 9 m past the kerb of the road
//     a link starts at, heading with the traffic on its side.
//
// Streets with a lane each way or more carry traffic; the highways' lanes are svx/highwayLanes.cpp,
// linked to the streets where their ramps land. Ids are stable 52-bit integers from structural
// keys; queries return records in id order.
#include "svx/roads.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <initializer_list>
#include <unordered_set>

#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "network/highways.hpp"
#include "network/roadLevel.hpp"
#include "svx/highwayLanes.hpp"
#include "svx/ids.hpp"
#include "world/World.hpp"

namespace svx::city {

using roads::Corner;
using roads::Info;
using roads::InfoPtr;
using roads::Junction;
using roads::Meet;
using roads::Node;
using roads::RampRef;
using roads::WalkRec;

namespace {

constexpr double H = kVoxelSize;
// A signal's phase (s): green, then all red.
constexpr double kPhase = 17;
constexpr double kGreen = 14;
// A crossing opens this long (s) at the start of the phase after its road's.
constexpr double kWalk = 4;
// Roads of one junction within this of each other's heading (cos 22.5°) move in one phase.
constexpr double kOnePhase = 0.924;
// The first parking place starts this far past the kerb a link starts at, one every PITCH (voxels).
constexpr double kParkStart = 72;
constexpr double kParkPitch = 48;
constexpr double kParkLen = 40;
// Walks whose ends lie this close meet (voxels): a junction on a bend of a road may give its two
// roads corners a hair apart.
constexpr double kSnap = 3;
// The shortest link that has lanes (voxels, kerb to kerb): closer junctions are one.
constexpr double kMinLink = 8;
// How far the roads of one junction may meet from its first road's node (voxels).
constexpr double kJunctionReach = 96;
// Most two roads' levels may differ (voxels) where one ends in the other's carriageway for them to meet.
constexpr double kLooseStep = 2;
// Most a lane's straight line may stray from its road's profile (voxels) before its piece is cut in two.
constexpr double kProfileSlack = 1;
// Straight on: within this much of the heading (cos 30°: Math.cos(Math.PI / 6)).
double straight() {
  static const double s = js::cos(3.141592653589793 / 6);
  return s;
}

// Speed limits by class (m/s).
double speed_of(const std::string& cls) {
  if (cls == "arterial") return 16.7;
  if (cls == "collector") return 13.9;
  if (cls == "local" || cls == "village") return 11.1;
  if (cls == "rural") return 19.4;
  return 11.1;
}

// City point (voxels) -> export metres.
inline double m(double q) { return H * (q - 0.5); }
uint64_t to_id(double id) { return static_cast<uint64_t>(id); }

using Vec2 = RoadNetwork::Vec2;
using Pieces = std::vector<std::array<double, 2>>;

// The box (city voxels) of a query in metres.
Rect vox_box(const Vec2& lo, const Vec2& hi) { return {lo[0] / H + 0.5, lo[1] / H + 0.5, hi[0] / H + 0.5, hi[1] / H + 0.5}; }

bool in_box(const std::array<double, 3>& p, const std::array<double, 3>& q, const Vec2& lo, const Vec2& hi) {
  return js::max(p[0], q[0]) >= lo[0] && js::min(p[0], q[0]) <= hi[0] && js::max(p[1], q[1]) >= lo[1] && js::min(p[1], q[1]) <= hi[1];
}

template <class T>
void sort_by_id(std::vector<T>& v) {
  js::sort(v, [](const T& p, const T& q) { return static_cast<double>(p.id) - static_cast<double>(q.id); });
}
void sort_turns(std::vector<RoadTurn>& v) {
  js::sort(v, [](const RoadTurn& p, const RoadTurn& q) { return static_cast<double>(p.first) - static_cast<double>(q.first); });
}

// Point and unit direction of a road at arc position s.
struct Along {
  double x = 0, y = 0, dx = 0, dy = 0;
  const RoadSeg* seg = nullptr;
};
Along along(const Info& info, double s) {
  const RoadSeg* seg = info.segs[0];
  for (const RoadSeg* q : info.segs)
    if (q->s0 <= s) seg = q;
  const double t = js::max(0.0, js::min(seg->len, s - seg->s0));
  return {seg->ax + seg->dx * t, seg->ay + seg->dy * t, seg->dx, seg->dy, seg};
}

// The level (voxels, continuous) of a road's own profile at arc position s.
double level_along(const World& world, const Info& info, double s) {
  const Along q = along(info, s);
  return segment_level(world, *q.seg, js::max(0.0, js::min(q.seg->len, s - q.seg->s0)));
}

// Top voxel of the carriageway at a city point (the nearest road's: for a point off the lanes, a corner).
double road_top(const World& world, const Info& info, double x, double y) {
  const std::optional<RoadLevel> r = road_level_at(world, *info.view, x, y, 8);
  return r ? js::round(r->z) : 0;
}

std::array<double, 2> heading(const RoadLane& l) {
  const double dx = l.b[0] - l.a[0];
  const double dy = l.b[1] - l.a[1];
  const double n = js::or_(js::hypot(dx, dy), 1);
  return {dx / n, dy / n};
}

// Where people walk along a road: on the middle of its sidewalks (w off its centre line, each
// side); down the middle of an alley or a single-track lane, which has none (and no traffic);
// nowhere on a road with neither (a rural road: w is its kerb, where a walk meeting it stops).
struct Side {
  double w = 0, width = 0;
  int n = 0;  // (sides: n of them)
  std::array<int, 2> sides{};
};
Side side_of(const Road& road) {
  if (road.hr > road.hc) return {(road.hc + road.hr) / 2, road.hr - road.hc, 2, {1, -1}};
  if (road.lanes <= 1) return {0, 2 * road.hc, 1, {0, 0}};
  return {road.hc, 0, 0, {0, 0}};
}

// The corner where road A's walking line on side sa meets road B's on side sb (0: its middle): the
// crossing of the two lines, from the two segments' own base points and directions, with the
// roads in id order (so either road finds the very same point).
Point2 corner(const Road& A, const RoadSeg& sg_a, double sa, const Road& B, const RoadSeg& sg_b, double sb) {
  if (js::compare(A.id, B.id) > 0) return corner(B, sg_b, sb, A, sg_a, sa);
  const double oa = side_of(A).w * sa;
  const double ob = side_of(B).w * sb;
  const double ax = sg_a.ax + sg_a.dy * oa;
  const double ay = sg_a.ay - sg_a.dx * oa;
  const double bx = sg_b.ax + sg_b.dy * ob;
  const double by = sg_b.ay - sg_b.dx * ob;
  const double den = sg_a.dx * sg_b.dy - sg_a.dy * sg_b.dx;
  const double t = ((bx - ax) * sg_b.dy - (by - ay) * sg_b.dx) / den;
  return {ax + sg_a.dx * t, ay + sg_a.dy * t};
}

// Which side of a road (at point p) the road of meet q lies on, when it ends there: +1 right, -1
// left, 0 both (it crosses).
double side_of_ending(const Along& p, const Meet& q) {
  if (!q.ends) return 0;
  // (its body runs away from the junction: along it if it starts here, against it if it ends here)
  const double away = q.t < q.seg->len / 2 ? 1 : -1;
  const double vx = q.seg->dx * away;
  const double vy = q.seg->dy * away;
  return js::or_(js::sign(vx * p.dy - vy * p.dx), 1);
}

// Road `other`'s own segment where a road meets it (meet q): its structure's, by index.
const RoadSeg* seg_of(const Info& other, const Meet& q) {
  for (const RoadSeg* sg : other.segs)
    if (sg->idx == q.seg->idx) return sg;
  return other.segs[0];
}

// Is a signal { cycle, offset, from, to } open at time t (none: always)?
bool open_at(const std::optional<RoadSignal>& sig, double t) {
  if (!sig) return true;
  const double u = std::fmod(std::fmod(t + sig->offset, sig->cycle) + sig->cycle, sig->cycle);
  return u >= sig->from && u < sig->to;
}

// -?\d+ at s[p...]: its number, p past it.
bool parse_int(const std::string& s, size_t& p, double* out) {
  size_t q = p;
  if (q < s.size() && s[q] == '-') ++q;
  const size_t d = q;
  while (q < s.size() && std::isdigit(static_cast<unsigned char>(s[q]))) ++q;
  if (q == d) return false;
  *out = js::parse_number(std::string_view(s).substr(p, q - p));
  p = q;
  return true;
}

// The highway ramps landing on an arterial (network/highways ramps): [at (arc along the road), ramp].
std::vector<std::pair<double, RampRef>> ramp_landings(const World& world, const Road& road, const std::vector<const RoadSeg*>& segs) {
  std::vector<std::pair<double, RampRef>> out;
  const HighwayNetwork* hw = world.highways.get();
  if (!hw || road.cls != "arterial") return out;
  Rect box = segs[0]->bbox;
  for (size_t k = 1; k < segs.size(); ++k) {
    const Rect& b = segs[k]->bbox;
    box = {js::min(box.x0, b.x0), js::min(box.y0, b.y0), js::max(box.x1, b.x1), js::max(box.y1, b.y1)};
  }
  for (const HighwayEdgePtr& e : hw->edges_near(box)) {
    const std::vector<HighwayRamp>& ramps = hw->ramps(*e);
    for (size_t index = 0; index < ramps.size(); ++index) {
      const HighwayRamp& r = ramps[index];
      if (r.arterial != road.id) continue;
      // (its arc: the nearest point of the road's centre line)
      bool found = false;
      double best_d = 0, best_at = 0;
      for (const RoadSeg* s : segs) {
        const double t = js::max(0.0, js::min(s->len, (r.x - s->ax) * s->dx + (r.y - s->ay) * s->dy));
        const double d = js::hypot(r.x - (s->ax + s->dx * t), r.y - (s->ay + s->dy * t));
        if (!found || d < best_d) {
          found = true;
          best_d = d;
          best_at = s->s0 + t;
        }
      }
      if (found) out.push_back({best_at, RampRef{e->id, static_cast<double>(index), r.side < 0 ? r.s_deck < r.s_ground : r.s_deck > r.s_ground}});
    }
  }
  return out;
}

// Where a road's end lies in another road's carriageway at its level, but the road network has no
// junction of the two (network/roadView makes one only where that end meets no other road: an end
// at a corner keeps the corner's level in the voxels): the junctions it makes for traffic and
// walks, on road `road` (its segments `segs` of its cell's view), as [at, meet]. Both roads work
// out the same pair: this road's ends in the others' carriageways, and the others' ends in this one's.
std::vector<std::pair<double, Meet>> loose_ends(const World& world, const Road& road, const RoadView& view, const std::vector<const RoadSeg*>& segs) {
  std::vector<std::pair<double, Meet>> out;
  auto ends = [](const RoadSeg& s, double* ts) {
    int n = 0;
    if (s.first) ts[n++] = 0;
    if (s.last) ts[n++] = s.len;
    return n;
  };
  // (the end of segment e at t lies in segment c's carriageway, at its level, crossing it, with no
  // junction of the two)
  auto lies = [&](const RoadSeg& e, double t, const RoadSeg& c) -> std::optional<double> {
    if (e.road.get() == c.road.get() || js::abs(e.dx * c.dy - e.dy * c.dx) < 0.5) return std::nullopt;
    for (const RoadJunction& j : e.jn)
      if (j.other->road.get() == c.road.get()) return std::nullopt;
    for (const RoadJunction& j : c.jn)
      if (j.other->road.get() == e.road.get()) return std::nullopt;
    const double px = e.ax + e.dx * t;
    const double py = e.ay + e.dy * t;
    const double tc = (px - c.ax) * c.dx + (py - c.ay) * c.dy;
    if (tc < -3 || tc > c.len + 3 || js::abs((px - c.ax) * c.dy - (py - c.ay) * c.dx) > c.hc) return std::nullopt;
    const double at = js::max(0.0, js::min(c.len, tc));
    if (js::abs(segment_level(world, e, t) - segment_level(world, c, at)) > kLooseStep) return std::nullopt;
    return at;
  };
  std::vector<const RoadSeg*> near;
  for (const RoadSeg* e : segs) {
    double ts[2];
    const int nt = ends(*e, ts);
    for (int q = 0; q < nt; ++q) {
      const double t = ts[q];
      const double x = e->ax + e->dx * t;
      const double y = e->ay + e->dy * t;
      near.clear();
      view.near(Rect{x - 1, y - 1, x + 1, y + 1}, near);
      for (const RoadSeg* c : near) {
        const std::optional<double> tc = lies(*e, t, *c);
        if (!tc) continue;
        const bool c_ends = (*tc < 3 && c->first) || (*tc > c->len - 3 && c->last);
        out.push_back({e->s0 + t, Meet{c->road, e, c, *tc, c->hc, c->hr, c_ends, e->rank >= 4 && c->rank >= 4, false}});
      }
    }
  }
  for (const RoadSeg* c : segs) {
    near.clear();
    view.near(c->bbox, near);
    for (const RoadSeg* e : near) {
      if (same_road(*e->road, road)) continue;
      double ts[2];
      const int nt = ends(*e, ts);
      for (int q = 0; q < nt; ++q) {
        const double t = ts[q];
        const std::optional<double> tc = lies(*e, t, *c);
        if (tc) out.push_back({c->s0 + *tc, Meet{e->road, c, e, t, e->hc, e->hr, true, e->rank >= 4 && c->rank >= 4, false}});
      }
    }
  }
  return out;
}

}  // namespace

bool parse_cell_id(const std::string& id, double* i, double* j) {
  if (id.empty() || id[0] != 'C') return false;
  size_t p = 1;
  if (!parse_int(id, p, i)) return false;
  if (p >= id.size() || id[p] != '_') return false;
  ++p;
  return parse_int(id, p, j);
}

bool signal_open(const std::optional<RoadSignal>& signal, double t) { return open_at(signal, t); }

const char* walk_kind_name(WalkKind kind) {
  switch (kind) {
    case WalkKind::Sidewalk:
      return "sidewalk";
    case WalkKind::Corner:
      return "corner";
    case WalkKind::Crossing:
      return "crossing";
    case WalkKind::Middle:
      return "middle";
  }
  return "";
}

// ---- the network

struct RoadNetwork::Impl {
  Impl(std::shared_ptr<const World> w, const RoadNetworkOptions& opt)
      : world_ptr(std::move(w)), world(*world_ptr), infos(opt.roads), lane_index(opt.remembered), walk_index(opt.remembered) {
    // the highways' lanes, linked to the streets where their ramps land
    if (world.highways)
      hl = std::make_unique<HighwayLanes>(
          world,
          [this](const std::string& edge_id, double ri) -> std::vector<RoadLane> {
            double axis = 0, a = 0, b = 0;
            if (!parse_edge_id(edge_id, &axis, &a, &b)) return {};
            const HighwayEdgePtr e = world.highways->edge(static_cast<int>(axis), a, b);
            if (!e) return {};
            const std::vector<HighwayRamp>& ramps = world.highways->ramps(*e);
            if (!(ri >= 0 && ri < static_cast<double>(ramps.size()))) return {};
            const RoadPtr road = road_by_id(ramps[static_cast<size_t>(ri)].arterial);
            if (!road) return {};
            const InfoPtr info = road_info(road);
            for (size_t k = 0; k < info->nodes.size(); ++k)
              for (const RampRef& x : info->nodes[k].ramps)
                if (x.edge == edge_id && x.index == ri) return leaving(info, static_cast<int>(k));
            return {};
          },
          opt.edges, opt.remembered);
  }

  std::shared_ptr<const World> world_ptr;
  const World& world;
  mutable MemoCache<std::string, Info> infos;
  // (the lanes handed out, and the road each is of: JS laneIndex, laneInfo; the walks: walkIndex,
  // walkEnds)
  mutable Remembered<RoadPtr> lane_index;
  mutable Remembered<RoadPtr> walk_index;
  std::unique_ptr<HighwayLanes> hl;

  // ---- a road's structure

  // A road's structure (cached by id): its segments in its own cell's view, its nodes along it
  // (junctions, merged where roads meet at one point, and its dead ends) and its links between them.
  InfoPtr road_info(const RoadPtr& road) const {
    return infos.get(road->id, [&] { return build_info(road); });
  }

  std::shared_ptr<const Info> build_info(const RoadPtr& road) const {
    auto info = std::make_shared<Info>();
    info->road = road;
    double ci = 0, cj = 0;
    if (!parse_cell_id(road->cell, &ci, &cj)) ci = cj = 0;
    info->view = world.road_view(ci, cj);
    const RoadView& view = *info->view;
    for (const RoadSeg& s : view.segs)
      if (s.road->id == road->id) info->segs.push_back(&s);
    js::sort(info->segs, [](const RoadSeg* p, const RoadSeg* q) { return p->idx - q->idx; });
    // (no segment of its own: a degenerate road, nothing to drive or walk)
    if (info->segs.empty()) return info;
    const std::vector<const RoadSeg*>& segs = info->segs;
    const double L = segs.back()->s0 + segs.back()->len;
    struct Draft {
      double at = 0, trim = 0;
      std::vector<Meet> meets;
      std::vector<RampRef> ramps;
    };
    std::vector<Draft> nodes;
    auto node_near = [&](double at) -> Draft& {
      for (Draft& r : nodes)
        if (js::abs(r.at - at) < 4) return r;
      nodes.push_back(Draft{at, 0, {}, {}});
      return nodes.back();
    };
    for (const RoadSeg* s : segs)
      for (const RoadJunction& j : s->jn)
        // (own: this road's segment the junction was found on, seg: the other road's; both roads see the same pair)
        node_near(s->s0 + j.s).meets.push_back(Meet{j.other->road, s, j.other, j.t_other, j.hc, j.hr, j.c_ends, j.signal, j.cw});
    for (auto& [at, q] : loose_ends(world, *road, view, segs)) node_near(at).meets.push_back(std::move(q));
    // (where a highway's ramp lands on it: a node of its own, so lanes end and leave there)
    for (auto& [at, ramp] : ramp_landings(world, *road, segs)) node_near(at).ramps.push_back(std::move(ramp));
    js::sort(nodes, [](const Draft& p, const Draft& q) { return p.at - q.at; });
    // (a road end with no junction there: a dead end)
    if (nodes.empty() || nodes[0].at > 4) nodes.insert(nodes.begin(), Draft{0, 0, {}, {}});
    if (nodes.back().at < L - 4) nodes.push_back(Draft{L, 0, {}, {}});
    // (the kerb line of the widest road met, along this one)
    for (Draft& n : nodes) {
      double t = 0;
      for (const Meet& q : n.meets) t = js::max(t, q.hc);
      n.trim = t;
    }
    // (junctions too close for a lane between their kerbs are one: a jog, a side road just past a
    // crossing, a dead end at a junction)
    for (size_t k = 0; k + 1 < nodes.size();) {
      const Draft& a = nodes[k];
      const Draft& b = nodes[k + 1];
      if (b.at - b.trim - (a.at + a.trim) >= kMinLink) {
        k += 1;
        continue;
      }
      const double lo = js::min(a.at - a.trim, b.at - b.trim);
      const double hi = js::max(a.at + a.trim, b.at + b.trim);
      Draft merged{(lo + hi) / 2, (hi - lo) / 2, a.meets, a.ramps};
      merged.meets.insert(merged.meets.end(), b.meets.begin(), b.meets.end());
      merged.ramps.insert(merged.ramps.end(), b.ramps.begin(), b.ramps.end());
      nodes.erase(nodes.begin() + static_cast<std::ptrdiff_t>(k), nodes.begin() + static_cast<std::ptrdiff_t>(k) + 2);
      nodes.insert(nodes.begin() + static_cast<std::ptrdiff_t>(k), std::move(merged));
    }
    info->L = L;
    info->nodes.resize(nodes.size());
    for (size_t k = 0; k < nodes.size(); ++k) {
      Node& n = info->nodes[k];
      n.at = nodes[k].at;
      n.trim = nodes[k].trim;
      n.meets = std::move(nodes[k].meets);
      n.ramps = std::move(nodes[k].ramps);
    }
    info->links.resize(nodes.size() - 1);
    for (size_t k = 0; k + 1 < nodes.size(); ++k) {
      info->links[k].k = static_cast<int>(k);
      info->links[k].from = static_cast<int>(k);
      info->links[k].to = static_cast<int>(k) + 1;
    }
    info->lanes = road->lanes >= 2 ? std::floor(road->lanes / 2) : 0;
    return info;
  }

  // A street by its id (C<i>_<j>/...), from its cell's road view, or none.
  RoadPtr road_by_id(const std::string& id) const {
    double i = 0, j = 0;
    if (!parse_cell_id(id, &i, &j)) return nullptr;
    const std::shared_ptr<const RoadView> view = world.road_view(i, j);
    for (const RoadSeg& s : view->segs)
      if (s.road->id == id) return s.road;
    return nullptr;
  }

  // The roads whose right-of-way reaches into a box of city voxels (every cell's view round it), by id.
  std::vector<RoadPtr> roads_in(const Rect& box) const {
    std::vector<RoadPtr> seen;
    std::unordered_set<std::string> ids;
    const CellIJ c0 = world.cell_at(box.x0, box.y0);
    const CellIJ c1 = world.cell_at(box.x1, box.y1);
    std::vector<const RoadSeg*> near;
    for (double j = c0.j; j <= c1.j; j += 1)
      for (double i = c0.i; i <= c1.i; i += 1) {
        const std::shared_ptr<const RoadView> view = world.road_view(i, j);
        near.clear();
        view->near(box, near);
        for (const RoadSeg* s : near)
          if (ids.insert(s->road->id).second) seen.push_back(s->road);
      }
    js::sort(seen, [](const RoadPtr& p, const RoadPtr& q) { return static_cast<double>(js::compare(p->id, q->id)); });
    return seen;
  }

  // ---- junctions

  // Road `other`'s node where it meets road `id` nearest city point (x, y) (within 8 m), or -1.
  static int node_of(const Info& other, const std::string& id, double x, double y) {
    int best = -1;
    double best_d = js::kInf;
    for (size_t k = 0; k < other.nodes.size(); ++k) {
      const Node& n = other.nodes[k];
      if (std::none_of(n.meets.begin(), n.meets.end(), [&](const Meet& q) { return q.road->id == id; })) continue;
      const Along p = along(other, n.at);
      const double d = js::hypot(p.x - x, p.y - y);
      if (d < best_d) {
        best = static_cast<int>(k);
        best_d = d;
      }
    }
    return best_d < 64 ? best : -1;
  }

  // The junction node `node` of road `info` is at, as every road there sees it: the roads meeting
  // there, and the roads they meet there, closed over (a road cut at a cell's border meets its
  // continuation through the roads they both cross), each with its node. Its first road (by id)
  // gives it its place, its axis (the phases) and its signal's offset, so every road there works
  // out the same signal. Kept on the node (a pure function of the node).
  const Junction& junction_of(const Info& info, int node) const {
    const Node& start = info.nodes[static_cast<size_t>(node)];
    return start.junction.get([&] {
      const Along p0 = along(info, start.at);
      struct Member {
        std::string id;
        const Info* info;
        int node;
      };
      std::vector<Member> members{{info.road->id, &info, node}};
      std::vector<InfoPtr> held;
      auto has = [&](const std::string& id) {
        return std::any_of(members.begin(), members.end(), [&](const Member& g) { return g.id == id; });
      };
      std::deque<size_t> queue{0};
      while (!queue.empty()) {
        const Info& I = *members[queue.front()].info;
        const Node& N = I.nodes[static_cast<size_t>(members[queue.front()].node)];
        queue.pop_front();
        const Along p = along(I, N.at);
        for (const Meet& q : N.meets) {
          if (has(q.road->id)) continue;
          InfoPtr other = road_info(q.road);
          const int on = node_of(*other, I.road->id, p.x, p.y);
          if (on < 0) continue;
          const Along po = along(*other, other->nodes[static_cast<size_t>(on)].at);
          if (js::hypot(po.x - p0.x, po.y - p0.y) > kJunctionReach) continue;
          members.push_back({q.road->id, other.get(), on});
          held.push_back(std::move(other));
          queue.push_back(members.size() - 1);
        }
      }
      Junction J;
      for (const Member& g : members) J.ids.push_back(g.id);
      js::sort_strings(J.ids);
      auto member = [&](const std::string& id) -> const Member& {
        return *std::find_if(members.begin(), members.end(), [&](const Member& g) { return g.id == id; });
      };
      const Member& first = member(J.ids[0]);
      const Along pf = along(*first.info, first.info->nodes[static_cast<size_t>(first.node)].at);
      // (the phases: the roads by heading, each joining the first phase whose first road runs
      // within 22.5° of it)
      std::vector<Along> heads;
      for (const std::string& id : J.ids) {
        const Member& g = member(id);
        const Along d = along(*g.info, g.info->nodes[static_cast<size_t>(g.node)].at);
        int i = -1;
        for (size_t k = 0; k < heads.size(); ++k)
          if (js::abs(heads[k].dx * d.dx + heads[k].dy * d.dy) >= kOnePhase) {
            i = static_cast<int>(k);
            break;
          }
        if (i < 0) {
          heads.push_back(d);
          i = static_cast<int>(heads.size()) - 1;
        }
        J.phase.push_back(i);
        J.members.push_back({g.info->road, g.node});
      }
      J.phases = static_cast<double>(heads.size());
      J.signal = std::any_of(members.begin(), members.end(), [](const Member& g) {
        const Node& n = g.info->nodes[static_cast<size_t>(g.node)];
        return std::any_of(n.meets.begin(), n.meets.end(), [](const Meet& q) { return q.signal; });
      });
      J.offset = (hash32(world.seed, js::round(pf.x), js::round(pf.y), 0x516) / 4294967296.0) * kPhase * J.phases;
      return J;
    });
  }

  // The signal of road `info` at its node `node`, or none where the junction has none: one phase
  // per heading of the junction's roads (roads within 22.5° of each other move together), 17 s
  // each, green the first 14 s (3 s all red after), in the order of their first roads by id, u
  // offset per junction; a crossing over the road is open the first 4 s of the next phase.
  std::optional<RoadSignal> signal_of(const Info& info, int node, bool crossing) const {
    const Junction& j = junction_of(info, node);
    if (!j.signal || j.phases < 2) return std::nullopt;
    const double cycle = kPhase * j.phases;
    const double i = j.phase_of(info.road->id);
    if (crossing) {
      const double from = kPhase * std::fmod(i + 1, j.phases);
      return RoadSignal{cycle, j.offset, from, from + kWalk};
    }
    return RoadSignal{cycle, j.offset, kPhase * i, kPhase * i + kGreen};
  }

  // ---- lanes

  // The pieces of a link one way: the arc intervals between its trimmed ends and the road's bends.
  const Pieces& pieces(const Info& info, const roads::Link& link) const {
    return link.pieces.get([&] {
      const Node& from = info.nodes[static_cast<size_t>(link.from)];
      const Node& to = info.nodes[static_cast<size_t>(link.to)];
      const double s0 = from.at + from.trim;
      const double s1 = to.at - to.trim;
      Pieces out;
      if (s1 - s0 >= kMinLink) {
        std::vector<double> cuts{s0};
        for (const RoadSeg* q : info.segs)
          if (q->s0 > s0 + 4 && q->s0 < s1 - 4) cuts.push_back(q->s0);
        cuts.push_back(s1);
        // (and where the road's profile bends away from the straight line: a landing, a change of grade)
        auto refine = [&](auto&& self, double a, double b, int depth) -> void {
          const double za = level_along(world, info, a);
          const double zb = level_along(world, info, b);
          double worst = 0;
          for (int k = 1; k < 8; ++k) {
            const double t = k / 8.0;
            worst = js::max(worst, js::abs(level_along(world, info, a + (b - a) * t) - (za + (zb - za) * t)));
          }
          if (worst > kProfileSlack && depth < 5 && b - a > 32) {
            self(self, a, (a + b) / 2, depth + 1);
            self(self, (a + b) / 2, b, depth + 1);
          } else {
            out.push_back({a, b});
          }
        };
        for (size_t k = 0; k + 1 < cuts.size(); ++k) refine(refine, cuts[k], cuts[k + 1], 0);
      }
      return out;
    });
  }

  // The lane of a road's link li, piece pi (in its direction's order), direction dir, lane k.
  RoadLane lane_record(const Info& info, int li, int pi, int dir, int k) const {
    const roads::Link& link = info.links[static_cast<size_t>(li)];
    const Pieces& ps = pieces(info, link);
    const std::array<double, 2>& pr = dir == 0 ? ps[static_cast<size_t>(pi)] : ps[ps.size() - 1 - static_cast<size_t>(pi)];
    const double sa = dir == 0 ? pr[0] : pr[1];
    const double sb = dir == 0 ? pr[1] : pr[0];
    const Road& road = *info.road;
    const double off = road.median / 2 + (k + 0.5) * road.lane;
    auto end = [&](double s) {
      const Along q = along(info, s);
      // (heading with the traffic, right of it in structvox's frame; on its own road's surface)
      const double hx = dir == 0 ? q.dx : -q.dx;
      const double hy = dir == 0 ? q.dy : -q.dy;
      const double x = q.x + hy * off;
      const double y = q.y - hx * off;
      return std::array<double, 3>{m(x), m(y), H * (js::round(level_along(world, info, s)) + 0.5)};
    };
    RoadLane l;
    l.id = to_id(id_of(road.id, "lane", li, pi, dir, k));
    l.a = end(sa);
    l.b = end(sb);
    l.width = road.lane * H;
    l.speed = speed_of(road.cls);
    l.key.kind = RoadLaneKey::Kind::Street;
    l.key.road = road.id;
    l.key.link = li;
    l.key.piece = pi;
    l.key.dir = dir;
    l.key.k = k;
    l.key.last = static_cast<size_t>(pi) + 1 == ps.size();
    return l;
  }

  // Every lane of a road.
  const std::vector<RoadLane>& road_lanes(const Info& info) const {
    return info.lane_list.get([&] {
      std::vector<RoadLane> out;
      if (js::truthy(info.lanes))
        for (size_t li = 0; li < info.links.size(); ++li) {
          const size_t n = pieces(info, info.links[li]).size();
          for (size_t pi = 0; pi < n; ++pi)
            for (int dir = 0; dir < 2; ++dir)
              for (int k = 0; k < info.lanes; ++k) out.push_back(lane_record(info, static_cast<int>(li), static_cast<int>(pi), dir, k));
        }
      return out;
    });
  }

  // A road's lanes, handed out (remembered: lane(id) knows them now).
  const std::vector<RoadLane>& index(const InfoPtr& info) const {
    const std::vector<RoadLane>& list = road_lanes(*info);
    if (!list.empty()) lane_index.add(info->road->id, info->road, list);
    return list;
  }

  // A lane handed out, with its road's structure.
  struct Hit {
    InfoPtr info;
    const RoadLane* lane = nullptr;
  };
  std::optional<Hit> lookup(uint64_t id) const {
    const auto owner = lane_index.find(id);
    if (!owner) return std::nullopt;
    InfoPtr info = road_info(owner->second);
    for (const RoadLane& l : road_lanes(*info))
      if (l.id == id) return Hit{std::move(info), &l};
    return std::nullopt;
  }

  // The node a lane runs into (its link's end node for its direction).
  static int end_node(const Info& info, const RoadLane& l) {
    const roads::Link& link = info.links[static_cast<size_t>(l.key.link)];
    return l.key.dir == 0 ? link.to : link.from;
  }

  // The lanes leaving a node of a road away from it (the first piece of the links on either side).
  std::vector<RoadLane> leaving(const InfoPtr& info, int node) const {
    int li = -1;
    int lb = -1;
    for (size_t k = 0; k < info->links.size(); ++k)
      if (info->links[k].from == node) {
        li = static_cast<int>(k);
        break;
      }
    for (size_t k = 0; k < info->links.size(); ++k)
      if (info->links[k].to == node) {
        lb = static_cast<int>(k);
        break;
      }
    const std::vector<RoadLane>& all = index(info);
    std::vector<RoadLane> out;
    if (li >= 0)
      for (const RoadLane& l : all)
        if (l.key.link == li && l.key.dir == 0 && l.key.piece == 0) out.push_back(l);
    if (lb >= 0)
      for (const RoadLane& l : all)
        if (l.key.link == lb && l.key.dir == 1 && l.key.piece == 0) out.push_back(l);
    return out;
  }

  std::vector<RoadLane> lanes_in(const Vec2& lo, const Vec2& hi) const {
    std::vector<RoadLane> out;
    for (const RoadPtr& road : roads_in(vox_box(lo, hi))) {
      const InfoPtr info = road_info(road);
      for (const RoadLane& l : index(info))
        if (in_box(l.a, l.b, lo, hi)) out.push_back(l);
    }
    if (hl) hl->lanes_in(vox_box(lo, hi), [&](const RoadLane& l) { return in_box(l.a, l.b, lo, hi); }, out);
    sort_by_id(out);
    return out;
  }

  std::optional<RoadLane> lane(uint64_t id) const {
    if (const std::optional<Hit> h = lookup(id)) return *h->lane;
    if (hl) return hl->lane(id);
    return std::nullopt;
  }

  std::vector<RoadTurn> next(uint64_t id) const {
    if (hl && hl->has(id)) return hl->next(id);
    const std::optional<Hit> h = lookup(id);
    if (!h) return {};
    const InfoPtr& info = h->info;
    const RoadLane& l = *h->lane;
    const int node = end_node(*info, l);
    const roads::Link& link = info->links[static_cast<size_t>(l.key.link)];
    const std::vector<RoadLane>& all = index(info);
    // (along its link: the next piece, the same lane)
    if (!l.key.last) {
      for (const RoadLane& r : all)
        if (r.key.link == l.key.link && r.key.dir == l.key.dir && r.key.k == l.key.k && r.key.piece == l.key.piece + 1) return {{r.id, 0}};
      return {};
    }
    const double n = info->lanes;
    // the ways on: every road of the junction (this one on beyond it among them), each way it leaves
    const std::array<double, 2> hd = heading(l);
    const double hx = hd[0];
    const double hy = hd[1];
    struct Way {
      std::vector<RoadLane> list;
      int turn;
    };
    std::vector<Way> ways;
    for (const Junction::Member& g : junction_of(*info, node).members) {
      const InfoPtr gi = road_info(g.road);
      const bool own = gi->road->id == info->road->id;
      std::vector<std::pair<std::array<int, 2>, std::vector<RoadLane>>> by_dir;
      for (RoadLane& r : leaving(gi, g.node)) {
        if (own && r.key.link == l.key.link) continue;
        const std::array<int, 2> key{r.key.link, r.key.dir};
        auto it = std::find_if(by_dir.begin(), by_dir.end(), [&](const auto& e) { return e.first == key; });
        if (it == by_dir.end()) {
          by_dir.push_back({key, {}});
          it = by_dir.end() - 1;
        }
        it->second.push_back(std::move(r));
      }
      for (auto& [key, list] : by_dir) {
        js::sort(list, [](const RoadLane& a, const RoadLane& b) { return static_cast<double>(a.key.k - b.key.k); });
        const std::array<double, 2> o = heading(list[0]);
        const double dot = hx * o[0] + hy * o[1];
        if (dot < -0.95) continue;
        const double cross = hx * o[1] - hy * o[0];
        ways.push_back({std::move(list), dot > straight() ? 0 : cross < 0 ? 1 : -1});
      }
    }
    // (an on-ramp of a highway starting here)
    for (const RampRef& q : info->nodes[static_cast<size_t>(node)].ramps) {
      if (q.off || !hl) continue;
      const std::optional<RoadLane> first = hl->ramp_lanes(q.edge, q.index).first;
      if (!first) continue;
      const std::array<double, 2> o = heading(*first);
      const double dot = hx * o[0] + hy * o[1];
      if (dot < -0.95) continue;
      ways.push_back({{*first}, dot > straight() ? 0 : hx * o[1] - hy * o[0] < 0 ? 1 : -1});
    }
    // (straight on keeps its lane; right from the kerb lane, left from the inner one; with no
    // straight on, any lane turns)
    const bool straight_on = std::any_of(ways.begin(), ways.end(), [](const Way& w) { return w.turn == 0; });
    std::vector<RoadTurn> cands;
    for (const Way& w : ways) {
      if (w.turn == 0)
        cands.push_back({w.list[std::min(static_cast<size_t>(l.key.k), w.list.size() - 1)].id, 0});
      else if (w.turn == 1 && (l.key.k == n - 1 || n == 1 || !straight_on))
        cands.push_back({w.list.back().id, 1});
      else if (w.turn == -1 && (l.key.k == 0 || n == 1 || !straight_on))
        cands.push_back({w.list[0].id, -1});
    }
    if (cands.empty()) {
      // (a dead end, for traffic too where it meets only alleys or footways: back the way it came,
      // from its inner lane)
      for (const RoadLane& r : all)
        if (r.key.link == link.k && r.key.dir != l.key.dir && r.key.k == 0 && r.key.piece == 0) {
          cands.push_back({r.id, -1});
          break;
        }
    }
    sort_turns(cands);
    return cands;
  }

  std::optional<RoadSignal> signal(uint64_t id) const {
    if (hl && hl->has(id)) return hl->signal(id);
    const std::optional<Hit> h = lookup(id);
    if (!h || !h->lane->key.last) return std::nullopt;
    return signal_of(*h->info, end_node(*h->info, *h->lane), false);
  }

  std::vector<RoadParking> parking_in(const Vec2& lo, const Vec2& hi) const {
    std::vector<RoadParking> out;
    for (const RoadPtr& road : roads_in(vox_box(lo, hi))) {
      if (!js::truthy(road->parking) || road->lanes < 2) continue;
      const InfoPtr info = road_info(road);
      const double off = road->hc - road->shoulder - road->parking / 2;
      for (size_t li = 0; li < info->links.size(); ++li) {
        const roads::Link& link = info->links[li];
        const Node& from = info->nodes[static_cast<size_t>(link.from)];
        const Node& to = info->nodes[static_cast<size_t>(link.to)];
        const double s0 = from.at + from.trim;
        const double s1 = to.at - to.trim;
        for (int dir = 0; dir < 2; ++dir) {
          double n = 0;
          for (double s = kParkStart; s + kParkLen <= s1 - s0 - kParkStart; s += kParkPitch, n += 1) {
            const double at = dir == 0 ? s0 + s + kParkLen / 2 : s1 - s - kParkLen / 2;
            const Along q = along(*info, at);
            const double hx = dir == 0 ? q.dx : -q.dx;
            const double hy = dir == 0 ? q.dy : -q.dy;
            const double x = q.x + hy * off;
            const double y = q.y - hx * off;
            const std::array<double, 3> pos{m(x), m(y), H * (js::round(level_along(world, *info, at)) + 0.5)};
            if (pos[0] < lo[0] || pos[0] > hi[0] || pos[1] < lo[1] || pos[1] > hi[1]) continue;
            out.push_back(RoadParking{to_id(id_of(road->id, "park", li, dir, n)), pos, {hx, hy}});
          }
        }
      }
    }
    sort_by_id(out);
    return out;
  }

  // ---- walkways: sidewalks along each side of a link from corner to corner, the stretches between
  // corners a junction puts on one side, crossings over a road at its crosswalks; walks down the
  // middle of alleys and single-track lanes, and over a street they run across. A walk's end meets
  // every walk of the junction's roads ending within SNAP of it (a pure function of the junction:
  // the same whatever was asked before).

  // The corners on a road's walking line on side sgn at a node, in order along the road: where the
  // walking lines of the roads met there on that side cross it (a road with sidewalks: both of
  // them; an alley: its middle; a rural road: its kerbs), else (a dead end, or a road ending on the
  // other side) the line at the node itself. Each with its meet (s: arc offset from the node).
  const std::vector<Corner>& corners_at(const Info& info, int node, int sgn) const {
    const Node& N = info.nodes[static_cast<size_t>(node)];
    return N.corners[sgn + 1].get([&] {
      const Along p = along(info, N.at);
      std::vector<Corner> out;
      for (size_t qi = 0; qi < N.meets.size(); ++qi) {
        const Meet& q = N.meets[qi];
        const double on = side_of_ending(p, q);
        if (sgn != 0 && on != 0 && on != sgn) continue;
        const InfoPtr other = road_info(q.road);
        const RoadSeg* so = seg_of(*other, q);
        // (an alley's middle, else both sidewalks: [0] or [1, -1])
        const bool middle = side_of(*other->road).w == 0;
        for (int fi = 0; fi < (middle ? 1 : 2); ++fi) {
          const double f = middle ? 0 : fi == 0 ? 1 : -1;
          const Point2 c = corner(*info.road, *q.own, sgn, *other->road, *so, f);
          out.push_back(Corner{c.x, c.y, (c.x - p.x) * p.dx + (c.y - p.y) * p.dy, static_cast<int>(qi), other->road, other->lanes});
        }
      }
      if (out.empty()) {
        const double o = side_of(*info.road).w * sgn;
        out.push_back(Corner{p.x + p.dy * o, p.y - p.dx * o, 0, -1, nullptr, 0});
      }
      js::sort(out, [](const Corner& a, const Corner& b) {
        double d = a.s - b.s;
        if (js::truthy(d)) return d;
        d = a.x - b.x;
        if (js::truthy(d)) return d;
        return a.y - b.y;
      });
      return out;
    });
  }

  // Where a road's walk on side sgn ends at a node on its arm `arm` (+1: the link runs on from the
  // node, -1: back): the corner nearest the link.
  const Corner& walk_end(const Info& info, int node, int arm, int sgn) const {
    const std::vector<Corner>& cs = corners_at(info, node, sgn);
    return arm > 0 ? cs.back() : cs.front();
  }

  std::vector<WalkRec> build_walks(const Info& info) const {
    const Road& road = *info.road;
    std::vector<WalkRec> out;
    const Side sd = side_of(road);
    const double width = sd.width;
    // (on the sidewalk, a kerb above the carriageway; on the carriageway of an alley)
    const double lift = sd.n == 2 ? 1.5 : 0.5;
    auto z = [&](const Corner& c) { return H * (road_top(world, info, c.x, c.y) + lift); };
    auto add = [&](RoadWalk w, const Corner& pa, const Corner& pb, std::initializer_list<int> nodes) {
      WalkRec rec{std::move(w), {pa.x, pa.y}, {pb.x, pb.y}, {}};
      for (const int n : nodes)
        for (const Junction::Member& g : junction_of(info, n).members)
          if (std::none_of(rec.roads.begin(), rec.roads.end(), [&](const RoadPtr& r) { return r->id == g.road->id; })) rec.roads.push_back(g.road);
      out.push_back(std::move(rec));
    };
    auto walk = [&](double id, WalkKind kind, const Corner& pa, const Corner& pb) {
      RoadWalk w;
      w.id = to_id(id);
      w.a = {m(pa.x), m(pa.y), z(pa)};
      w.b = {m(pb.x), m(pb.y), z(pb)};
      w.inset = {0, 0, 0};
      w.width = width * H;
      w.crossing = kind == WalkKind::Crossing;
      w.kind = kind;
      return w;
    };
    for (size_t li = 0; li < info.links.size(); ++li) {
      const roads::Link& link = info.links[li];
      const Along mid = along(info, (info.nodes[static_cast<size_t>(link.from)].at + info.nodes[static_cast<size_t>(link.to)].at) / 2);
      for (int si = 0; si < sd.n; ++si) {
        const int sgn = sd.sides[static_cast<size_t>(si)];
        const Corner& pa = walk_end(info, link.from, 1, sgn);
        const Corner& pb = walk_end(info, link.to, -1, sgn);
        if ((pb.x - pa.x) * mid.dx + (pb.y - pa.y) * mid.dy < 8) continue;
        // (towards the buildings: a fifth of the sidewalk)
        const double ins = 0.2 * width * H * sgn;
        RoadWalk w = walk(id_of(road.id, "walk", li, sgn), sgn ? WalkKind::Sidewalk : WalkKind::Middle, pa, pb);
        w.inset = {mid.dy * ins, -mid.dx * ins, 0};
        add(std::move(w), pa, pb, {link.from, link.to});
      }
      // a crosswalk over this road on each of the link's arms at a junction that marks one (between
      // its two corners there)
      if (sd.n == 2) {
        const struct {
          int node, arm;
          const char* tag;
        } arms[2] = {{link.from, 1, "a"}, {link.to, -1, "b"}};
        for (const auto& [node, arm, tag] : arms) {
          const Node& N = info.nodes[static_cast<size_t>(node)];
          if (std::none_of(N.meets.begin(), N.meets.end(), [](const Meet& q) { return q.cw; })) continue;
          const Corner& ca = walk_end(info, node, arm, 1);
          const Corner& cb = walk_end(info, node, arm, -1);
          if (ca.q < 0 || cb.q < 0) continue;
          // (over a road at a signalled junction: open while its traffic is held)
          const std::optional<RoadSignal> signal = signal_of(info, node, true);
          RoadWalk w = walk(id_of(road.id, "cross", li, tag), WalkKind::Crossing, ca, cb);
          w.width = 3;
          w.signal = signal;
          add(std::move(w), ca, cb, {node});
        }
      }
    }
    // at each junction: on along a side between the corners of two roads met there (a side road's
    // mouth just past a crossing); an alley over a street it runs across (between that street's two
    // sidewalks)
    for (size_t ni = 0; ni < info.nodes.size(); ++ni) {
      const bool through = ni > 0 && ni + 1 < info.nodes.size();
      for (int si = 0; si < sd.n; ++si) {
        const int sgn = sd.sides[static_cast<size_t>(si)];
        const std::vector<Corner>& cs = corners_at(info, static_cast<int>(ni), sgn);
        for (size_t k = 0; k + 1 < cs.size(); ++k) {
          const Corner& c0 = cs[k];
          const Corner& c1 = cs[k + 1];
          if (c0.q < 0 || c1.q < 0 || c1.s - c0.s <= kSnap) continue;
          if (c0.q != c1.q) {
            add(walk(id_of(road.id, "jw", ni, sgn, k), sgn ? WalkKind::Corner : WalkKind::Middle, c0, c1), c0, c1, {static_cast<int>(ni)});
          } else if (sgn == 0 && through && js::truthy(c0.other_lanes)) {
            std::optional<RoadSignal> signal;
            for (const Junction::Member& g : junction_of(info, static_cast<int>(ni)).members)
              if (g.road->id == c0.other->id) {
                signal = signal_of(*road_info(g.road), g.node, true);
                break;
              }
            RoadWalk w = walk(id_of(road.id, "cross", ni, k), WalkKind::Crossing, c0, c1);
            w.signal = signal;
            add(std::move(w), c0, c1, {static_cast<int>(ni)});
          }
        }
      }
    }
    return out;
  }

  // A road's walks, handed out (remembered: walk(id) knows them now).
  const std::vector<WalkRec>& walks_of(const InfoPtr& info) const {
    const std::vector<WalkRec>& list = info->walk_list.get([&] { return build_walks(*info); });
    if (!list.empty()) walk_index.add(info->road->id, info->road, list);
    return list;
  }

  // A walk handed out, with its road's structure.
  struct WalkHit {
    InfoPtr info;
    const WalkRec* walk = nullptr;
  };
  std::optional<WalkHit> walk_rec(uint64_t id) const {
    const auto owner = walk_index.find(id);
    if (!owner) return std::nullopt;
    InfoPtr info = road_info(owner->second);
    for (const WalkRec& w : walks_of(info))
      if (w.walk.id == id) return WalkHit{std::move(info), &w};
    return std::nullopt;
  }

  std::vector<RoadWalk> walks_in(const Vec2& lo, const Vec2& hi) const {
    std::vector<RoadWalk> out;
    for (const RoadPtr& road : roads_in(vox_box(lo, hi))) {
      // (held: the cache may drop it meanwhile)
      const InfoPtr info = road_info(road);
      for (const WalkRec& w : walks_of(info))
        if (in_box(w.walk.a, w.walk.b, lo, hi)) out.push_back(w.walk);
    }
    sort_by_id(out);
    return out;
  }

  std::vector<RoadTurn> walk_next(uint64_t id, int end) const {
    const std::optional<WalkHit> e = walk_rec(id);
    if (!e) return {};
    const Point2 p = end == 0 ? e->walk->pa : e->walk->pb;
    std::vector<RoadTurn> out;
    for (const RoadPtr& road : e->walk->roads) {
      const InfoPtr info = road_info(road);
      for (const WalkRec& w : walks_of(info)) {
        if (w.walk.id == id) continue;
        if (js::hypot(w.pa.x - p.x, w.pa.y - p.y) <= kSnap) out.push_back({w.walk.id, 0});
        if (js::hypot(w.pb.x - p.x, w.pb.y - p.y) <= kSnap) out.push_back({w.walk.id, 1});
      }
    }
    js::sort(out, [](const RoadTurn& a, const RoadTurn& b) {
      const double d = static_cast<double>(a.first) - static_cast<double>(b.first);
      return js::truthy(d) ? d : static_cast<double>(a.second - b.second);
    });
    return out;
  }
};

RoadNetwork::RoadNetwork(std::shared_ptr<const World> world, const RoadNetworkOptions& options)
    : impl_(std::make_unique<Impl>(std::move(world), options)) {}
RoadNetwork::~RoadNetwork() = default;

std::vector<RoadLane> RoadNetwork::lanes_in(const Vec2& lo, const Vec2& hi) const { return impl_->lanes_in(lo, hi); }
std::optional<RoadLane> RoadNetwork::lane(uint64_t id) const { return impl_->lane(id); }
std::vector<RoadTurn> RoadNetwork::next(uint64_t id) const { return impl_->next(id); }
std::optional<RoadSignal> RoadNetwork::signal(uint64_t id) const { return impl_->signal(id); }
bool RoadNetwork::green(uint64_t id, double t) const { return open_at(impl_->signal(id), t); }
std::vector<RoadParking> RoadNetwork::parking_in(const Vec2& lo, const Vec2& hi) const { return impl_->parking_in(lo, hi); }
std::vector<RoadWalk> RoadNetwork::walks_in(const Vec2& lo, const Vec2& hi) const { return impl_->walks_in(lo, hi); }
bool RoadNetwork::walk_open(uint64_t id, double t) const {
  const auto w = impl_->walk_rec(id);
  return open_at(w ? w->walk->walk.signal : std::nullopt, t);
}
std::optional<RoadWalk> RoadNetwork::walk(uint64_t id) const {
  const auto w = impl_->walk_rec(id);
  if (!w) return std::nullopt;
  return w->walk->walk;
}
std::vector<RoadTurn> RoadNetwork::walk_next(uint64_t id, int end) const { return impl_->walk_next(id, end); }
const World& RoadNetwork::world() const { return impl_->world; }

}  // namespace svx::city
