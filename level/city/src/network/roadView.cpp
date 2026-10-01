// svx_city — network/roadView.hpp (voxel_city network/roadView.js), and World.js's roadView.
#include "network/roadView.hpp"

#include <unordered_map>

#include "city/cellNetwork.hpp"
#include "core/js.hpp"
#include "network/roadClasses.hpp"
#include "world/World.hpp"
#include "world/caches.hpp"

namespace svx::city {

namespace {

// x ?? d for a number JS may leave undefined (NaN here)
double or_undef(double x, double d) { return js::is_undefined(x) ? d : x; }

bool crosswalk_rule(const RoadSeg& s, const RoadSeg& c) {
  if (s.rank < 3 || c.rank < 2) return false;
  if (*s.cls == "rural" || *c.cls == "rural" || *s.cls == "alley") return false;
  if (*c.cls == "pedestrian") return true;
  return c.rank >= js::min(s.rank, 4.0);
}

// Segment s's junction with c, at s's arc ts and c's tc (where their centre lines cross, or a
// road's end lies).
RoadJunction junction(const RoadSeg& s, const RoadSeg& c, double ts, double tc, double eps) {
  // does c continue on both sides of s (crossing) or end at it (T)?
  const bool c_ends_here = (tc < eps && c.first) || (tc > c.len - eps && c.last);
  const bool s_ends_here = (ts < eps && s.first) || (ts > s.len - eps && s.last);
  RoadJunction j;
  j.s = js::max(0.0, js::min(s.len, ts));
  j.hc = c.hc;
  j.hr = c.hr;
  j.cls = c.cls;
  j.rank = c.rank;
  j.c_ends = c_ends_here;
  j.s_ends = s_ends_here;
  j.cw = crosswalk_rule(s, c);
  j.signal = s.rank >= 4 && c.rank >= 4;
  // the crossing segment and where it is crossed (road levels meet there)
  j.other = &c;
  j.t_other = js::max(0.0, js::min(c.len, tc));
  return j;
}

// Does a road end on segment e (its first or last) lie in segment o's carriageway? { t: that
// end's arc on e, to: where it lies along o }.
struct EndIn {
  bool found = false;
  double t = 0, to = 0;
};
EndIn end_in(const RoadSeg& e, const RoadSeg& o, double eps) {
  double ts[2];
  int n = 0;
  if (e.first) ts[n++] = 0;
  if (e.last) ts[n++] = e.len;
  for (int q = 0; q < n; ++q) {
    const double t = ts[q];
    const double px = e.ax + e.dx * t;
    const double py = e.ay + e.dy * t;
    const double to = (px - o.ax) * o.dx + (py - o.ay) * o.dy;
    if (to >= -eps && to <= o.len + eps && js::abs((px - o.ax) * o.dy - (py - o.ay) * o.dx) <= o.hc) return {true, t, js::max(0.0, js::min(o.len, to))};
  }
  return {};
}

}  // namespace

std::vector<RoadSeg> build_segments(const RoadList& roads) {
  std::vector<RoadSeg> segs;
  for (const RoadPtr& road : roads) {
    double acc = 0;
    const std::vector<RoadPt>& pts = road->pts;
    for (size_t k = 0; k + 1 < pts.size(); ++k) {
      const RoadPt& a = pts[k];
      const RoadPt& b = pts[k + 1];
      const double len = js::hypot(b.x - a.x, b.y - a.y);
      if (len < 1e-6) continue;
      const double pad = road->hr + road->corner + 2;
      RoadSeg s;
      s.road = road;
      s.idx = static_cast<double>(k);
      s.ax = a.x;
      s.ay = a.y;
      s.bx = b.x;
      s.by = b.y;
      // (az, bz: a.z, b.z - undefined, no road's points have one)
      s.len = len;
      s.dx = (b.x - a.x) / len;
      s.dy = (b.y - a.y) / len;
      s.s0 = acc;
      s.hc = road->hc;
      s.hr = road->hr;
      s.sidewalk = or_undef(road->sidewalk, road->hr - road->hc);
      s.parking = or_undef(road->parking, 0);
      s.median = or_undef(road->median, 0);
      s.lanes = or_undef(road->lanes, 2);
      s.lane = or_undef(road->lane, 26);
      s.shoulder = or_undef(road->shoulder, 0);
      s.cls = &road->cls;
      s.rank = or_undef(class_rank(road->cls), 0);
      s.first = k == 0;
      s.last = k + 2 == pts.size();
      s.bbox = {js::min(a.x, b.x) - pad, js::min(a.y, b.y) - pad, js::max(a.x, b.x) + pad, js::max(a.y, b.y) + pad};
      segs.push_back(std::move(s));
      acc += len;
    }
  }
  return segs;
}

void annotate_junctions(std::vector<RoadSeg>& segs, const SpatialGrid<const RoadSeg*>& grid) {
  std::vector<const RoadSeg*> tmp;
  const double eps = 3;
  // (a road's end lying in the other's carriageway, the centre lines crossing short of it or past
  // it, as a side road winding into a winding street does: decided once every plain junction is
  // known)
  struct Loose {
    RoadSeg* s;
    const RoadSeg* c;
    double ts, tc;
    int n_ends;
    const RoadSeg* e[2];
    double t[2];
  };
  std::vector<Loose> loose;
  for (RoadSeg& s : segs) {
    tmp.clear();
    grid.query(s.bbox, tmp);
    for (const RoadSeg* cp : tmp) {
      const RoadSeg& c = *cp;
      if (&c == &s || c.road.get() == s.road.get()) continue;
      const double cross = s.dx * c.dy - s.dy * c.dx;
      if (js::abs(cross) < 0.5) continue;  // near-parallel
      // intersect infinite lines
      const double qx = c.ax - s.ax;
      const double qy = c.ay - s.ay;
      const double ts = (qx * c.dy - qy * c.dx) / cross;
      const double tc = (qx * s.dy - qy * s.dx) / cross;
      if (ts >= -eps && ts <= s.len + eps && tc >= -eps && tc <= c.len + eps) {
        s.jn.push_back(junction(s, c, ts, tc, eps));
        continue;
      }
      // (else a road's end in the other's carriageway: the junction where that end lies, on it
      // and across)
      const EndIn se = end_in(s, c, eps);
      const EndIn ce = end_in(c, s, eps);
      if (se.found) {
        if (ce.found)
          loose.push_back({&s, &c, se.t, se.to, 2, {&s, &c}, {se.t, ce.t}});
        else
          loose.push_back({&s, &c, se.t, se.to, 1, {&s, nullptr}, {se.t, 0}});
      } else if (ce.found) {
        loose.push_back({&s, &c, ce.to, ce.t, 1, {&c, nullptr}, {ce.t, 0}});
      }
    }
  }
  // (such an end is a junction where it meets no other road: a dead end standing in the street,
  // else a ledge or a wall; an end at a junction already has that junction's level)
  std::unordered_map<const Road*, std::vector<const RoadSeg*>> by_road;
  for (const RoadSeg& s : segs) by_road[s.road.get()].push_back(&s);
  auto free_end = [&](const RoadSeg& e, double t) {
    for (const RoadSeg* x : by_road[e.road.get()])
      for (const RoadJunction& j : x->jn)
        if (js::abs(x->s0 + j.s - (e.s0 + t)) <= e.hr + 8) return false;
    return true;
  };
  // (two roads that cross or meet elsewhere already have their junction)
  auto met = [&](const Road* a, const Road* b) {
    for (const RoadSeg* x : by_road[a])
      for (const RoadJunction& j : x->jn)
        if (j.other->road.get() == b) return true;
    return false;
  };
  std::vector<const Loose*> joined;
  for (const Loose& q : loose) {
    if (met(q.s->road.get(), q.c->road.get())) continue;
    bool every = true;
    for (int k = 0; k < q.n_ends && every; ++k) every = free_end(*q.e[k], q.t[k]);
    if (every) joined.push_back(&q);
  }
  for (const Loose* q : joined) q->s->jn.push_back(junction(*q->s, *q->c, q->ts, q->tc, eps));
  for (RoadSeg& s : segs) js::sort(s.jn, [](const RoadJunction& a, const RoadJunction& b) { return a.s - b.s; });
}

RoadView::RoadView(RoadList roads_in) : roads(std::move(roads_in)) {
  segs = build_segments(roads);
  for (const RoadSeg& s : segs) grid_.insert(&s, s.bbox);
  annotate_junctions(segs, grid_);
  // every junction along each road (road arc position `at`), shared by its segments (roadLevel.js)
  std::unordered_map<const Road*, size_t> index;
  for (const RoadSeg& s : segs)
    if (index.emplace(s.road.get(), index.size()).second) by_road_.emplace_back();
  for (const RoadSeg& s : segs) {
    RoadJunctions& list = by_road_[index[s.road.get()]];
    for (const RoadJunction& j : s.jn) list.list.push_back({&j, &s, s.s0 + j.s});
  }
  for (RoadSeg& s : segs) s.rj = &by_road_[index[s.road.get()]];
  max_reach = 0;
  for (const RoadSeg& s : segs) max_reach = js::max(max_reach, s.hr + s.road->corner + 2);
}

void RoadView::near(const Rect& rect, std::vector<const RoadSeg*>& out) const { grid_.query(rect, out); }

std::vector<const RoadSeg*> RoadView::near(const Rect& rect) const {
  std::vector<const RoadSeg*> out;
  grid_.query(rect, out);
  return out;
}

// World.js roadView: the roads of cell (i, j) and its eight neighbours, with junction annotations
// (the cell networks' roads, or those a test serves: World::cell_roads).
std::shared_ptr<const RoadView> World::road_view(double i, double j) const {
  return caches().road_views.get(cell_key(i, j), [&]() -> std::shared_ptr<const RoadView> {
    RoadList roads;
    for (double dj = -1; dj <= 1; dj += 1)
      for (double di = -1; di <= 1; di += 1) {
        if (cell_roads) {
          const RoadList cell = cell_roads(i + di, j + dj);
          roads.insert(roads.end(), cell.begin(), cell.end());
        } else {
          const std::shared_ptr<const CellNet> net = cell_net(i + di, j + dj);
          roads.insert(roads.end(), net->roads.begin(), net->roads.end());
        }
      }
    return std::make_shared<const RoadView>(std::move(roads));
  });
}

}  // namespace svx::city
