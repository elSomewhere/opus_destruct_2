// svx_city — underground/sewers.hpp (voxel_city underground/sewers.js).
#include "underground/sewers.hpp"

#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include "city/cellNetwork.hpp"
#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "nature/rivers.hpp"
#include "network/road.hpp"
#include "voxel/chunk.hpp"
#include "voxel/materials.hpp"
#include "world/World.hpp"
#include "world/caches.hpp"
#include "world/fields.hpp"

namespace svx::city {

namespace {

constexpr double DEPTH = 40;      // walkway 5 m below the street surface
constexpr double IN_W = 13;       // walkway outer edge (inner half width)
constexpr double CH_W = 4;        // channel half width
constexpr double SHELL = 3;
constexpr double VAULT = 22;      // crown above the walkway at the centre line
constexpr double CHAMBER = 22;    // junction chamber inner half size
constexpr double CHAMBER_H = 26;
constexpr double HALL = 64;       // hall inner half size
constexpr double HALL_H = 30;     // hall ceiling above the ledge (1.25 m below the street)
constexpr double LEDGE = 10;      // walkway ring at run level along the hall walls
constexpr double BASIN = 24;      // the basin floor lies 3 m below the ledge
constexpr double MAX_RUN = 640;   // chambers at least every 80 m
constexpr double MIN_U = 0.3;
constexpr double SHAFT = 4;       // ladder shaft clear half size (8 x 8 voxels)

// CLASSES
bool sewer_class(const std::string& cls) { return cls == "arterial" || cls == "collector" || cls == "local"; }

// (a node's key, `${x},${y}`: the exact position, -0 as 0)
using NodeKey = std::pair<double, double>;
NodeKey node_key(double x, double y) { return {x + 0.0, y + 0.0}; }

bool has_arm(const std::string& arms, char d) { return arms.find(d) != std::string::npos; }
void add_arm(std::string& arms, char d) {
  if (!has_arm(arms, d)) arms.push_back(d);
}

// overlap3: do two boxes (with z) overlap?
template <class A, class B>
bool overlap3(const A& a, const B& b) {
  return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1 && a.z0 <= b.z1 && b.z0 <= a.z1;
}

// A node while its cell is planned (JS mutates the node objects as it goes).
struct NodeW {
  SewerNode n;
  bool deleted = false;
};
// A run while its cell is planned: with its two nodes.
struct RunW {
  SewerRun r;
  NodeW* na = nullptr;
  NodeW* nb = nullptr;
};

// chamberBox: the volume of a node's chamber (hall size unknown here: the larger one when flagged).
Box3 chamber_box(const SewerNode& n) {
  const double e = (n.hall ? HALL : CHAMBER) + SHELL + 2;
  return {n.x - e, n.y - e, n.z - (n.hall ? BASIN : 0) - 6, n.x + e, n.y + e, n.zr};
}

// runBox: the volume of a run (axis, fixed, z0, z1) between l0 and l1.
Box3 run_box(int axis, double fixed, double rz0, double rz1, double l0, double l1) {
  const double w = IN_W + SHELL + 2;
  const double z0 = js::min(rz0, rz1) - 6;
  const double z1 = js::max(rz0, rz1) + VAULT + SHELL + 2;
  return axis == 0 ? Box3{fixed - w, l0, z0, fixed + w, l1, z1} : Box3{l0, fixed - w, z0, l1, fixed + w, z1};
}

// junctionsNear: intersections of perpendicular roads within 64 voxels of node n (excluding n).
std::vector<Point2> junctions_near(const std::vector<const Road*>& roads, const SewerNode& n) {
  const double R = 64;
  std::vector<const Road*> near;
  for (const Road* r : roads) {
    const RoadPt& a = r->pts[0];
    const RoadPt& b = r->pts[1];
    const double x0 = js::min(a.x, b.x);
    const double x1 = js::max(a.x, b.x);
    const double y0 = js::min(a.y, b.y);
    const double y1 = js::max(a.y, b.y);
    if (n.x >= x0 - R && n.x <= x1 + R && n.y >= y0 - R && n.y <= y1 + R) near.push_back(r);
  }
  std::vector<Point2> out;
  for (const Road* v : near) {
    if (v->pts[0].x != v->pts[1].x) continue;
    const double x = v->pts[0].x;
    for (const Road* hz : near) {
      if (hz->pts[0].y != hz->pts[1].y) continue;
      const double y = hz->pts[0].y;
      if (x == n.x && y == n.y) continue;
      if (std::fabs(x - n.x) >= R || std::fabs(y - n.y) >= R) continue;
      const double vy0 = js::min(v->pts[0].y, v->pts[1].y);
      const double vy1 = js::max(v->pts[0].y, v->pts[1].y);
      const double hx0 = js::min(hz->pts[0].x, hz->pts[1].x);
      const double hx1 = js::max(hz->pts[0].x, hz->pts[1].x);
      if (y >= vy0 && y <= vy1 && x >= hx0 && x <= hx1) out.push_back({x, y});
    }
  }
  return out;
}

// ---- the per-column profile writer

double run_z(const SewerRun& r, double l) {
  const double len = r.l1 - r.l0;
  return js::round(r.z0 + ((r.z1 - r.z0) * (l - r.l0)) / js::or_(len, 1));
}

void put(ChunkBuffer& chunk, int col, double z, uint16_t m, bool only_solid = false) {
  const IdxRange k = chunk.range_z(z, z);
  if (k.lo > k.hi) return;
  const int idx = col + k.lo * kP2;
  if (only_solid && !is_solid(chunk.data[idx])) return;
  chunk.data[idx] = m;
}

void span(ChunkBuffer& chunk, int col, double z0, double z1, uint16_t m, bool only_solid = false) {
  const IdxRange k = chunk.range_z(z0, z1);
  uint16_t* d = chunk.data.data();
  for (int kk = k.lo; kk <= k.hi; ++kk) {
    const int idx = col + kk * kP2;
    if (only_solid && !is_solid(d[idx])) continue;
    d[idx] = m;
  }
}

void wall_column(ChunkBuffer& chunk, int col, double z0, double z1) {
  const IdxRange k = chunk.range_z(z0, z1);
  uint16_t* d = chunk.data.data();
  for (int kk = k.lo; kk <= k.hi; ++kk) {
    const int idx = col + kk * kP2;
    if (!is_solid(d[idx])) continue;
    const double z = chunk.wz(kk);
    d[idx] = z - z0 < 6 ? MAT::SEWER_BRICK_DARK : MAT::SEWER_BRICK;
  }
}

void run_column(ChunkBuffer& chunk, int col, double ac, const SewerRun& r, double l) {
  const double zb = run_z(r, l);
  const double vault = zb + VAULT - js::round(5 * js::pow(ac / IN_W, 2));
  span(chunk, col, zb - 5, zb - 3, MAT::SEWER_BRICK, true);
  if (ac <= CH_W) {
    put(chunk, col, zb - 2, MAT::SLUDGE);
    put(chunk, col, zb - 1, MAT::SEWER_WATER);
    span(chunk, col, zb, vault, 0);
  } else {
    span(chunk, col, zb - 2, zb - 1, MAT::SEWER_BRICK_DARK);
    put(chunk, col, zb, ac == CH_W + 1 ? MAT::SEWER_CURB : MAT::SEWER_FLOOR);
    span(chunk, col, zb + 1, vault, 0);
  }
  span(chunk, col, vault + 1, vault + SHELL, MAT::SEWER_BRICK, true);
  const double la = mod(l, 96);
  if (ac <= 1 && la < 3) put(chunk, col, vault, MAT::LAMP_CAGE);
  if (ac == IN_W) put(chunk, col, vault, MAT::PIPE);
  if (ac == IN_W && la >= 40 && la < 42) span(chunk, col, zb + 9, vault - 1, MAT::PIPE);
}

// inArm: channels run from the centre along every incident arm.
bool in_arm(const SewerNode& n, double dx, double dy, double cw) {
  return (std::fabs(dx) <= cw && ((dy < 0 && has_arm(n.arms, 'N')) || (dy > 0 && has_arm(n.arms, 'S')) || std::fabs(dy) <= cw)) ||
         (std::fabs(dy) <= cw && ((dx < 0 && has_arm(n.arms, 'W')) || (dx > 0 && has_arm(n.arms, 'E')) || std::fabs(dx) <= cw));
}

// Overflow hall: the runs arrive on a ledge ring along the walls; sewage pours over the ledge into
// a pillared basin 3 m lower, reached by four stairs that descend along the ledge.
void hall_column(ChunkBuffer& chunk, int col, double dx, double dy, const SewerNode& n) {
  const double zb = n.z;
  const double top = zb + n.H;
  const double ax = std::fabs(dx);
  const double ay = std::fabs(dy);
  const double m = js::max(ax, ay);
  const double inner = n.R - LEDGE;  // last basin ring
  const double zf = zb - BASIN;
  span(chunk, col, top, top + SHELL - 1, MAT::SEWER_BRICK, true);
  if (m > inner) {
    // ledge
    span(chunk, col, zb - 5, zb - 3, MAT::SEWER_BRICK, true);
    if (in_arm(n, dx, dy, CH_W + 1)) {
      put(chunk, col, zb - 2, MAT::SLUDGE);
      put(chunk, col, zb - 1, MAT::SEWER_WATER);
      span(chunk, col, zb, top - 1, 0);
    } else {
      span(chunk, col, zb - 2, zb - 1, MAT::SEWER_BRICK_DARK);
      put(chunk, col, zb, m == inner + 1 ? MAT::SEWER_CURB : MAT::SEWER_FLOOR);
      span(chunk, col, zb + 1, top - 1, 0);
      // railing along the drop, except where a basin stair starts
      const bool stair_top = ax == inner + 1 && ay >= 7 && ay < 18;
      if (m == inner + 1 && !stair_top) {
        put(chunk, col, zb + 7, MAT::RAILING);
        if ((js::to_int32(dx + dy) & 3) == 0) span(chunk, col, zb + 1, zb + 6, MAT::RAILING);
      }
    }
    if (m == n.R && std::fmod(ax + ay, 24) == 0) span(chunk, col, zb + 1, top - 1, MAT::PIPE);
    return;
  }
  // basin
  span(chunk, col, zf - 5, zf - 3, MAT::SEWER_BRICK, true);
  double floor = zf;
  if (ax > inner - 8 && ay >= 7) floor = js::max(zf, zb - js::sar(ay - 6, 1));  // stairs along the x walls
  const bool wet = in_arm(n, dx, dy, CH_W + 2);
  if (wet && floor == zf) {
    put(chunk, col, zf - 2, MAT::SLUDGE);
    put(chunk, col, zf - 1, MAT::SEWER_WATER);
    span(chunk, col, zf, top - 1, 0);
  } else {
    span(chunk, col, zf - 2, floor - 1, floor > zf ? MAT::STAIR_CONCRETE : MAT::SEWER_BRICK_DARK);
    const bool dark = (js::to_int32(static_cast<double>(js::sar(dx, 3)) + static_cast<double>(js::sar(dy, 3))) & 1) != 0;
    put(chunk, col, floor, floor > zf ? MAT::STAIR_CONCRETE : dark ? MAT::CONCRETE_DARK : MAT::SEWER_FLOOR);
    span(chunk, col, floor + 1, top - 1, 0);
  }
  // sewage pours over the ledge where an arm arrives
  if (m == inner && in_arm(n, dx, dy, CH_W + 1)) span(chunk, col, zf, zb - 1, MAT::SEWER_WATER);
  const bool px = (ax >= 20 && ax <= 23) || (ax >= 40 && ax <= 43);
  const bool py = (ay >= 20 && ay <= 23) || (ay >= 40 && ay <= 43);
  if (px && py) {
    span(chunk, col, zf + 1, top - 1, MAT::CONCRETE);
    span(chunk, col, zf + 1, zf + 4, MAT::SEWER_BRICK_DARK);
    if ((ax == 20 || ax == 40) && (ay == 21 || ay == 41)) put(chunk, col, zb + 4, MAT::LAMP_CAGE);
    return;
  }
  if (std::fmod(ax, 20) == 10 && std::fmod(ay, 20) == 10) put(chunk, col, top - 1, MAT::LAMP_CAGE);
}

void chamber_column(ChunkBuffer& chunk, int col, double x, double y, const SewerNode& n) {
  if (n.hall) {
    hall_column(chunk, col, x - n.x, y - n.y, n);
    return;
  }
  const double dx = x - n.x;
  const double dy = y - n.y;
  const double zb = n.z;
  const double top = zb + n.H;
  span(chunk, col, zb - 5, zb - 3, MAT::SEWER_BRICK, true);
  if (in_arm(n, dx, dy, CH_W)) {
    put(chunk, col, zb - 2, MAT::SLUDGE);
    put(chunk, col, zb - 1, MAT::SEWER_WATER);
    span(chunk, col, zb, top - 1, 0);
  } else {
    span(chunk, col, zb - 2, zb - 1, MAT::SEWER_BRICK_DARK);
    const bool edge = std::fabs(dx) == CH_W + 1 || std::fabs(dy) == CH_W + 1;
    put(chunk, col, zb, edge ? MAT::SEWER_CURB : MAT::SEWER_FLOOR);
    span(chunk, col, zb + 1, top - 1, 0);
  }
  span(chunk, col, top, top + SHELL - 1, MAT::SEWER_BRICK, true);
  const double ax = std::fabs(dx);
  const double ay = std::fabs(dy);
  // a caged lamp in the middle of the ceiling, pipes with valves on the walls
  if (ax <= 1 && ay <= 1) put(chunk, col, top - 1, MAT::LAMP_CAGE);
  if (ax == n.R && std::fmod(ay, 12) == 6) {
    span(chunk, col, zb + 1, top - 1, MAT::PIPE);
    put(chunk, col, zb + 10, MAT::VALVE_RED);
  }
}

void cone_column(ChunkBuffer& chunk, int col, double x, double y, double sx, double sy, double zs) {
  constexpr double kCones[4][2] = {{-8, -8}, {7, -8}, {-8, 7}, {7, 7}};
  for (const auto& o : kCones) {
    const double u = x - (sx + o[0]);
    const double v = y - (sy + o[1]);
    if (u < 0 || u > 1 || v < 0 || v > 1) continue;
    put(chunk, col, zs + 1, MAT::CONE_ORANGE);
    put(chunk, col, zs + 2, MAT::CONE_WHITE);
    if (u == 0 && v == 0) put(chunk, col, zs + 3, MAT::CONE_ORANGE);
  }
  // the lifted cover lies next to the hole
  const double u = x - (sx + 8);
  const double v = y - sy;
  if (u >= 1 && u <= 6 && v >= -3 && v <= 2) put(chunk, col, zs + 1, MAT::MANHOLE);
}

// Ladder shaft from a chamber up to a manhole in the street. Runs after the chamber profile of the
// same column only when the column is outside the chamber interior; inside, chamber_column has
// carved the room and this adds the ladder and the shaft above the ceiling.
void shaft_column(ChunkBuffer& chunk, const SewerColumns* tile, int col, double x, double y, const SewerNode& n, double sx, double sy) {
  const double dx = x - sx;
  const double dy = y - sy;
  const bool clear = dx >= -SHAFT && dx < SHAFT && dy >= -SHAFT && dy < SHAFT;
  const double zs = tile && tile->z ? static_cast<double>(tile->z[col]) : n.zr;
  const double ceil_z = n.z + n.H;
  if (!clear) {
    const bool ring = dx >= -SHAFT - 2 && dx <= SHAFT + 1 && dy >= -SHAFT - 2 && dy <= SHAFT + 1;
    if (ring) {
      // concrete ring and an iron rim at the street surface
      span(chunk, col, ceil_z, zs - 1, MAT::CONCRETE, true);
      if (dx >= -SHAFT - 1 && dx <= SHAFT && dy >= -SHAFT - 1 && dy <= SHAFT) put(chunk, col, zs, MAT::MANHOLE_RIM);
    } else if (n.open) {
      cone_column(chunk, col, x, y, sx, sy, zs);
    }
    return;
  }
  span(chunk, col, ceil_z - 1, zs - 1, 0);
  if (n.open)
    put(chunk, col, zs, 0);
  else
    put(chunk, col, zs, (js::to_int32(dx + dy) & 1) != 0 ? MAT::MANHOLE : MAT::MANHOLE_RIM);
  // ladder on the outer wall of the shaft, from the chamber floor to the street
  const double lx = n.qx > 0 ? SHAFT - 1 : -SHAFT;
  if (dx == lx && dy >= -SHAFT + 1 && dy <= SHAFT - 2) {
    const bool rail = dy == -SHAFT + 1 || dy == SHAFT - 2;
    for (double z = n.z + 1; z < zs; z += 1)
      if (rail || (js::to_int32(z - n.z) & 1) == 0) put(chunk, col, z, MAT::LADDER);
  }
}

// Per-column profile writer. For each column the dominant piece is chosen: chamber interior > run
// interior > chamber wall > run wall, so tunnels open cleanly into chambers.
void rasterize_columns(ChunkBuffer& chunk, const SewerColumns* tile, const std::vector<const SewerRun*>& runs,
                       const std::vector<const SewerNode*>& nodes) {
  for (int j = 0; j < kP; ++j) {
    const double y = chunk.wy(j);
    for (int i = 0; i < kP; ++i) {
      const double x = chunk.wx(i);
      const int col = i + j * kP;
      const SewerNode* n_in = nullptr;
      const SewerNode* n_wall = nullptr;
      double d_in = js::kInf;
      for (const SewerNode* n : nodes) {
        const double m = js::max(std::fabs(x - n->x), std::fabs(y - n->y));
        if (m <= n->R && m < d_in) {
          n_in = n;
          d_in = m;
        } else if (m > n->R && m <= n->R + SHELL) {
          n_wall = n;
        }
      }
      if (n_in) {
        chamber_column(chunk, col, x, y, *n_in);
      } else {
        const SewerRun* best = nullptr;
        double bc = js::kInf;
        for (const SewerRun* r : runs) {
          const double l = r->axis == 0 ? y : x;
          if (l < r->l0 || l > r->l1) continue;
          const double c = std::fabs((r->axis == 0 ? x : y) - r->fixed);
          if (c < bc) {
            bc = c;
            best = r;
          }
        }
        if (best && bc <= IN_W) {
          run_column(chunk, col, bc, *best, best->axis == 0 ? y : x);
        } else if (n_wall) {
          wall_column(chunk, col, n_wall->z - (n_wall->hall ? BASIN : 0) - 3, n_wall->z + n_wall->H + SHELL - 1);
        } else if (best && bc <= IN_W + SHELL) {
          const double zb = run_z(*best, best->axis == 0 ? y : x);
          wall_column(chunk, col, zb - 3, zb + VAULT - 5 + SHELL);
        }
      }
      // ladder shafts (and the cones around open manholes) go on top
      for (const SewerNode* n : nodes) {
        if (!n->shaft) continue;
        const double sx = n->x + n->qx * 12;
        const double sy = n->y + n->qy * 12;
        const double reach = n->open ? 15 : SHAFT + 2;
        if (std::fabs(x - sx) <= reach && std::fabs(y - sy) <= reach) shaft_column(chunk, tile, col, x, y, *n, sx, sy);
      }
    }
  }
}

const std::shared_ptr<const SewerPlan>& empty_plan() {
  static const std::shared_ptr<const SewerPlan> kEmpty = std::make_shared<const SewerPlan>();
  return kEmpty;
}

}  // namespace

std::string SewerNode::key() const { return js::cat(x, ",", y); }

Sewers::Sewers(const World& w, StreetLevel sl, size_t cache_capacity) : world(&w), street_level(std::move(sl)), plans_(cache_capacity) {}

bool Sewers::eligible(const Road& road, double i, double j) const {
  if (!sewer_class(road.cls) || road.pts.size() != 2) return false;
  const RoadPt& a = road.pts[0];
  const RoadPt& b = road.pts[1];
  if (a.x != b.x && a.y != b.y) return false;
  if (!road.arterial_edge.empty()) {
    const int axis = road.arterial_edge == "W" ? 0 : 1;
    const double line = axis == 0 ? i : j;
    if (world->subway && world->subway->line_exists(axis, line)) return false;
  }
  return world->fields->urban((a.x + b.x) / 2, (a.y + b.y) / 2).u >= MIN_U;
}

std::shared_ptr<const SewerPlan> Sewers::cell_plan(double i, double j) const {
  return plans_.get(cell_key(i, j), [&] { return plan_cell(i, j); });
}

std::shared_ptr<const SewerPlan> Sewers::plan_cell(double i, double j) const {
  const World& w = *world;
  if (!street_level) SVX_FAIL("sewers: no street level (World::street_level, network/roadLevel)");
  const std::shared_ptr<const CellNet> net = w.cell_net(i, j);
  std::vector<const Road*> mine;
  for (const std::shared_ptr<const Road>& r : net->roads)
    if (eligible(*r, i, j)) mine.push_back(r.get());
  if (mine.empty()) return empty_plan();
  // the eligible roads of the 3 x 3 neighbourhood (their networks held while planning)
  std::vector<std::shared_ptr<const CellNet>> held;
  std::vector<const Road*> others;
  for (double dj = -1; dj <= 1; dj += 1)
    for (double di = -1; di <= 1; di += 1) {
      std::shared_ptr<const CellNet> nb = w.cell_net(i + di, j + dj);
      for (const std::shared_ptr<const Road>& r : nb->roads)
        if (eligible(*r, i + di, j + dj)) others.push_back(r.get());
      held.push_back(std::move(nb));
    }

  // the nodes, in the order made (JS: a Map by `${x},${y}`)
  std::vector<std::unique_ptr<NodeW>> nodes;
  std::map<NodeKey, NodeW*> index;
  auto node = [&](double x, double y) -> NodeW* {
    const NodeKey key = node_key(x, y);
    const auto it = index.find(key);
    if (it != index.end()) return it->second;
    const double zr = js::round(street_level(x, y));
    auto nw = std::make_unique<NodeW>();
    nw->n.x = x;
    nw->n.y = y;
    nw->n.z = zr - DEPTH;
    nw->n.zr = zr;
    NodeW* p = nw.get();
    nodes.push_back(std::move(nw));
    index.emplace(key, p);
    return p;
  };
  std::vector<RunW> runs;
  for (const Road* r : mine) {
    const RoadPt& a = r->pts[0];
    const RoadPt& b = r->pts[1];
    const int axis = a.x == b.x ? 0 : 1;  // 0: runs north-south (along y)
    const double fixed = axis == 0 ? a.x : a.y;
    const double l0 = axis == 0 ? js::min(a.y, b.y) : js::min(a.x, b.x);
    const double l1 = axis == 0 ? js::max(a.y, b.y) : js::max(a.x, b.x);
    // (a Set: the first of equal values kept, in insertion order)
    std::vector<double> cuts;
    auto add_cut = [&](double v) {
      for (const double c : cuts)
        if (c == v) return;
      cuts.push_back(v);
    };
    add_cut(l0);
    add_cut(l1);
    for (const Road* o : others) {
      if (same_road(*o, *r)) continue;
      const RoadPt& oa = o->pts[0];
      const RoadPt& ob = o->pts[1];
      const int oaxis = oa.x == ob.x ? 0 : 1;
      if (oaxis == axis) continue;
      const double ofixed = oaxis == 0 ? oa.x : oa.y;
      const double ol0 = oaxis == 0 ? js::min(oa.y, ob.y) : js::min(oa.x, ob.x);
      const double ol1 = oaxis == 0 ? js::max(oa.y, ob.y) : js::max(oa.x, ob.x);
      if (ofixed >= l0 && ofixed <= l1 && fixed >= ol0 && fixed <= ol1) add_cut(ofixed);
    }
    std::vector<double> sorted = cuts;
    js::sort(sorted, [](double p, double q) { return p - q; });
    std::vector<double> ls{sorted[0]};
    for (size_t k = 1; k < sorted.size(); ++k) {
      const double gap = sorted[k] - sorted[k - 1];
      const double n = std::ceil(gap / MAX_RUN);
      for (double m = 1; m < n; m += 1) ls.push_back(js::round(sorted[k - 1] + (gap * m) / n));
      ls.push_back(sorted[k]);
    }
    auto at = [&](double l) { return axis == 0 ? node(fixed, l) : node(l, fixed); };
    for (size_t k = 0; k + 1 < ls.size(); ++k) {
      NodeW* na = at(ls[k]);
      NodeW* nb = at(ls[k + 1]);
      RunW rw;
      rw.r.axis = axis;
      rw.r.fixed = fixed;
      rw.r.l0 = ls[k];
      rw.r.l1 = ls[k + 1];
      rw.r.z0 = na->n.z;
      rw.r.z1 = nb->n.z;
      rw.r.cls = r->cls;
      rw.na = na;
      rw.nb = nb;
      runs.push_back(std::move(rw));
    }
  }

  // overflow hall where the cell's two collectors cross
  const Road* vcol = nullptr;
  const Road* hcol = nullptr;
  for (const std::shared_ptr<const Road>& r : net->roads) {
    if (!(r->cls == "collector" && eligible(*r, i, j))) continue;
    if (!vcol && r->pts[0].x == r->pts[1].x) vcol = r.get();
    if (!hcol && r->pts[0].y == r->pts[1].y) hcol = r.get();
  }
  NodeW* hall_node = nullptr;
  if (vcol && hcol) {
    const auto it = index.find(node_key(vcol->pts[0].x, hcol->pts[0].y));
    if (it != index.end()) hall_node = it->second;
  }
  if (hall_node) hall_node->n.hall = true;

  // keep clear of subway stations: blocked chambers and the runs that reach them or cross station
  // volumes are dropped (pure functions of position, so every cell drops the same pieces) ...
  // ... and of river channels (a pure test as well)
  const Rivers* rivers = w.rivers.get();
  auto wet = [&](const Box3& b) { return rivers && rivers->hits_rect({b.x0, b.y0, b.x1, b.y1}, 4); };
  for (const std::unique_ptr<NodeW>& n : nodes) n->n.blocked = hits_subway(chamber_box(n->n)) || wet(chamber_box(n->n));
  std::vector<RunW> kept;
  for (const RunW& r : runs) {
    if (r.na->n.blocked || r.nb->n.blocked) continue;
    const Box3 rb = run_box(r.r.axis, r.r.fixed, r.r.z0, r.r.z1, r.r.l0, r.r.l1);
    if (hits_subway(rb) || wet(rb)) continue;
    kept.push_back(r);
  }
  for (const RunW& r : kept) {
    add_arm(r.na->n.arms, r.r.axis == 0 ? 'S' : 'E');
    add_arm(r.nb->n.arms, r.r.axis == 0 ? 'N' : 'W');
  }
  // arms of runs owned by neighbours that end on one of our nodes
  auto arm_clear = [&](const SewerNode& n, char dir) {
    const double e = CHAMBER + 48;
    const int axis = dir == 'N' || dir == 'S' ? 0 : 1;
    const double sgn = dir == 'S' || dir == 'E' ? 1 : -1;
    const double l = axis == 0 ? n.y : n.x;
    return !hits_subway(run_box(axis, axis == 0 ? n.x : n.y, n.z, n.z, js::min(l, l + sgn * e), js::max(l, l + sgn * e)));
  };
  for (const Road* o : others) {
    const RoadPt& oa = o->pts[0];
    const RoadPt& ob = o->pts[1];
    for (const std::unique_ptr<NodeW>& np : nodes) {
      SewerNode& n = np->n;
      if (n.blocked) continue;
      auto add = [&](char dir) {
        if (!has_arm(n.arms, dir) && arm_clear(n, dir)) add_arm(n.arms, dir);
      };
      if (oa.x == ob.x && oa.x == n.x && n.y >= js::min(oa.y, ob.y) && n.y <= js::max(oa.y, ob.y)) {
        if (n.y > js::min(oa.y, ob.y)) add('N');
        if (n.y < js::max(oa.y, ob.y)) add('S');
      } else if (oa.y == ob.y && oa.y == n.y && n.x >= js::min(oa.x, ob.x) && n.x <= js::max(oa.x, ob.x)) {
        if (n.x > js::min(oa.x, ob.x)) add('W');
        if (n.x < js::max(oa.x, ob.x)) add('E');
      }
    }
  }
  for (const std::unique_ptr<NodeW>& n : nodes)
    if (n->n.blocked || n->n.arms.empty()) n->deleted = true;
  if (hall_node && hall_node->deleted) hall_node->n.hall = false;

  auto plan = std::make_shared<SewerPlan>();
  if (hall_node && hall_node->n.hall) {
    // the stair from the hall's ledge up to the sidewalk of the north-south collector, rising
    // away from the crossing (hallStair; boxes use the subway's conventions: mode 2 only
    // replaces solid ground)
    SewerNode& n = hall_node->n;
    const uint32_t h = hash32(w.seed, n.x, n.y, 4244);
    const double side = (h & 1u) != 0 ? 1 : -1;
    const double dir = (h & 2u) != 0 ? 1 : -1;
    // on the hall's ledge, which lies under the collector's sidewalk
    const double c0 = HALL - LEDGE + 1;
    const double c1 = HALL - 1;
    const double l_start = 36;
    std::vector<UndergroundBox>& boxes = plan->boxes;
    auto W = [&](double a0, double a1, double b0, double b1, double z0, double z1, uint16_t m, int mode = 0) {
      const double ca = side * a0;
      const double cb = side * a1;
      const double la = dir * b0;
      const double lb = dir * b1;
      boxes.push_back({n.x + js::min(ca, cb), n.x + js::max(ca, cb), n.y + js::min(la, lb), n.y + js::max(la, lb), z0, z1, m, mode});
    };
    const double z_low = n.z;
    // the stair tops out on the sidewalk surface next to its far end
    const double probe_l = l_start + 2 * (n.zr + 1 - z_low);
    const double z_high = js::round(street_level(n.x + side * (c0 + c1) * 0.5, n.y + dir * probe_l)) + 1;
    const double R = z_high - z_low;
    const double HEAD = 20;
    for (double k = 1; k <= R; k += 1) {
      const double l = l_start + 2 * (k - 1);
      const bool open = z_low + k + HEAD > z_high - 1;
      W(c0 - 2, c1 + 2, l, l + 1, z_low + k - 3, js::min(z_high - 1, z_low + k + HEAD + 2), MAT::SEWER_BRICK, 2);
      W(c0, c1, l, l + 1, z_low + 1, z_low + k, MAT::STAIR_CONCRETE);
      W(c0, c1, l, l + 1, z_low + k + 1, z_low + k + HEAD, 0);
      W(c0, c0, l, l + 1, z_low + k + 7, z_low + k + 7, MAT::RAILING);
      if (open) {
        W(c0 - 2, c1 + 2, l, l + 1, z_high + 1, z_high + HEAD + 4, 0);
        for (const double cc : {c0 - 1, c1 + 1}) {
          W(cc, cc, l, l + 1, z_high, z_high, MAT::CONCRETE_DARK);
          W(cc, cc, l, l + 1, z_high + 8, z_high + 8, MAT::POLE_METAL);
          if ((js::to_int32(l) & 3) == 0) W(cc, cc, l, l, z_high + 1, z_high + 7, MAT::POLE_METAL);
        }
      }
    }
    const double l_end = l_start + 2 * R;
    W(c0, c1, l_end, l_end + 3, z_high - 1, z_high, MAT::SIDEWALK);
    const double lb = l_start + 2 * js::max(0.0, R - HEAD - 1);
    W(c0 - 1, c1 + 1, lb - 1, lb - 1, z_high + 8, z_high + 8, MAT::POLE_METAL);
    for (double c = c0; c <= c1; c += 3) W(c, c, lb - 1, lb - 1, z_high + 1, z_high + 7, MAT::POLE_METAL);
    // service sign and a caged lamp at the top
    W(c1 + 2, c1 + 2, l_end, l_end, z_high + 1, z_high + 20, MAT::POLE_METAL);
    W(c1 + 1, c1 + 3, l_end, l_end, z_high + 15, z_high + 19, MAT::HAZARD_YELLOW);
    W(c1 + 2, c1 + 2, l_end - 1, l_end + 1, z_high + 21, z_high + 22, MAT::LAMP_CAGE);
    const Point2 pa{n.x + side * c0, n.y + dir * (l_start + 2 * (R - HEAD - 1))};
    const Point2 pb{n.x + side * c1, n.y + dir * (l_end + 3)};
    SewerOpening o;
    o.x0 = js::min(pa.x, pb.x) - 4;
    o.y0 = js::min(pa.y, pb.y) - 4;
    o.x1 = js::max(pa.x, pb.x) + 4;
    o.y1 = js::max(pa.y, pb.y) + 4;
    o.top = {n.x + side * js::round((c0 + c1) / 2), n.y + dir * (l_end + 1), z_high + 1};
    plan->openings.push_back(o);
    n.stair_top = o.top;
  }

  for (const std::unique_ptr<NodeW>& np : nodes) {
    if (np->deleted) continue;
    SewerNode& n = np->n;
    const uint32_t h = hash32(w.seed, n.x, n.y, 4242);
    n.qx = (h & 1u) != 0 ? 1 : -1;
    n.qy = (h & 2u) != 0 ? 1 : -1;
    // staggered junctions closer than two chambers share one ladder shaft: the junction with the
    // smaller key keeps it. Junctions are intersections of eligible roads, which every cell that
    // knows this node can enumerate identically from its 3 x 3 neighbourhood.
    bool before = false;
    if (!n.hall)
      for (const Point2& p : junctions_near(others, n))
        if (p.x < n.x || (p.x == n.x && p.y < n.y)) {
          before = true;
          break;
        }
    n.shaft = !n.hall && !before;
    n.open = n.shaft && hash_float(w.seed, n.x, n.y, 4243) < 0.35;
    n.R = n.hall ? HALL : CHAMBER;
    n.H = n.hall ? HALL_H : CHAMBER_H;
    plan->nodes.push_back(n);
  }
  for (const RunW& r : kept) plan->runs.push_back(r.r);
  std::optional<Box3>& bb = plan->bb;
  auto grow = [&](double x0, double y0, double z0, double x1, double y1, double z1) {
    if (bb)
      bb = Box3{js::min(bb->x0, x0), js::min(bb->y0, y0), js::min(bb->z0, z0), js::max(bb->x1, x1), js::max(bb->y1, y1), js::max(bb->z1, z1)};
    else
      bb = Box3{x0, y0, z0, x1, y1, z1};
  };
  const double W = IN_W + SHELL;
  for (const SewerRun& r : plan->runs) {
    const double zl = js::min(r.z0, r.z1) - 6;
    const double zh = js::max(r.z0, r.z1) + DEPTH + 4;
    if (r.axis == 0)
      grow(r.fixed - W, r.l0, zl, r.fixed + W, r.l1, zh);
    else
      grow(r.l0, r.fixed - W, zl, r.l1, r.fixed + W, zh);
  }
  for (const SewerNode& n : plan->nodes) {
    const double e = n.R + SHELL + 12;
    grow(n.x - e, n.y - e, n.z - (n.hall ? BASIN : 0) - 6, n.x + e, n.y + e, n.zr + 6);
  }
  for (const UndergroundBox& q : plan->boxes) grow(q.x0, q.y0, q.z0, q.x1, q.y1, q.z1);
  return plan;
}

bool Sewers::hits_subway(const Box3& b) const {
  const Subway* sw = world->subway.get();
  if (!sw) return false;
  for (const std::shared_ptr<const Station>& s : sw->stations_near({b.x0, b.y0, b.x1, b.y1})) {
    if (!overlap3(s->bb, b)) continue;
    for (const UndergroundBox& q : s->boxes)
      if (overlap3(q, b)) return true;
  }
  return false;
}

std::vector<std::shared_ptr<const SewerPlan>> Sewers::near(const Rect& rect) const {
  const double pad = HALL + 160;
  std::vector<std::shared_ptr<const SewerPlan>> out;
  for (const CellIJ& c : world->cells_overlapping({rect.x0 - pad, rect.y0 - pad, rect.x1 + pad, rect.y1 + pad})) {
    std::shared_ptr<const SewerPlan> p = cell_plan(c.i, c.j);
    if (p->bb && p->bb->x0 <= rect.x1 && p->bb->x1 >= rect.x0 && p->bb->y0 <= rect.y1 && p->bb->y1 >= rect.y0) out.push_back(std::move(p));
  }
  return out;
}

bool Sewers::blocks_surface(double x, double y) const {
  for (const std::shared_ptr<const SewerPlan>& p : near({x, y, x, y})) {
    for (const SewerOpening& o : p->openings)
      if (x >= o.x0 && x <= o.x1 && y >= o.y0 && y <= o.y1) return true;
    for (const SewerNode& n : p->nodes) {
      if (!n.open) continue;
      const double sx = n.x + n.qx * 12;
      const double sy = n.y + n.qy * 12;
      if (std::fabs(x - sx) < 20 && std::fabs(y - sy) < 20) return true;
    }
  }
  return false;
}

SewerMap Sewers::map_data(const Rect& rect) const {
  SewerMap out;
  // (JS's seen keys `${r.axis},${r.fixed},${r.l0}`: -0 as 0)
  std::set<std::tuple<int, double, double>> seen;
  for (const std::shared_ptr<const SewerPlan>& p : near(rect)) {
    for (const SewerRun& r : p->runs) {
      if (!seen.insert({r.axis, r.fixed + 0.0, r.l0 + 0.0}).second) continue;
      if (r.axis == 0)
        out.lines.push_back({{{r.fixed, r.l0}, {r.fixed, r.l1}}});
      else
        out.lines.push_back({{{r.l0, r.fixed}, {r.l1, r.fixed}}});
    }
    for (const SewerNode& n : p->nodes)
      if (n.hall) out.halls.push_back({n.x, n.y, n.stair_top.value_or(XYZ{})});
  }
  return out;
}

std::shared_ptr<const SewerNode> Sewers::nearest_hall(double x, double y, double reach) const {
  std::shared_ptr<const SewerNode> best;
  double bd = js::kInf;
  for (const std::shared_ptr<const SewerPlan>& p : near({x - reach, y - reach, x + reach, y + reach})) {
    for (const SewerNode& n : p->nodes) {
      if (!n.hall) continue;
      const double d = js::hypot(n.x - x, n.y - y);
      if (d < bd) {
        bd = d;
        best = std::shared_ptr<const SewerNode>(p, &n);
      }
    }
  }
  return best;
}

bool sewer_z_range(const World& world, const Rect& rect, int lod, double* z0, double* z1) {
  if (lod > 1 || !world.sewers) return false;
  double lo = js::kInf;
  double hi = -js::kInf;
  auto hit = [&](double x0, double y0, double x1, double y1) { return x0 <= rect.x1 && x1 >= rect.x0 && y0 <= rect.y1 && y1 >= rect.y0; };
  const double W = IN_W + SHELL;
  for (const std::shared_ptr<const SewerPlan>& p : world.sewers->near(rect)) {
    for (const SewerRun& r : p->runs) {
      const bool ok = r.axis == 0 ? hit(r.fixed - W, r.l0, r.fixed + W, r.l1) : hit(r.l0, r.fixed - W, r.l1, r.fixed + W);
      if (!ok) continue;
      lo = js::min(lo, js::min(r.z0, r.z1) - 6);
      hi = js::max(hi, js::max(r.z0, r.z1) + VAULT + SHELL + 1);
    }
    for (const SewerNode& n : p->nodes) {
      const double e = n.R + SHELL + (n.open ? 16 : 8);
      if (!hit(n.x - e, n.y - e, n.x + e, n.y + e)) continue;
      lo = js::min(lo, n.z - (n.hall ? BASIN : 0) - 6);
      hi = js::max(hi, n.zr + 4);
    }
    for (const UndergroundBox& q : p->boxes) {
      if (!hit(q.x0, q.y0, q.x1, q.y1)) continue;
      lo = js::min(lo, q.z0);
      hi = js::max(hi, q.z1);
    }
  }
  if (lo == js::kInf) return false;
  *z0 = lo;
  *z1 = hi;
  return true;
}

void sewer_rasterize(const World& world, ChunkBuffer& chunk, const SewerColumns* tile) {
  if (chunk.lod > 1 || !world.sewers) return;
  const Box3 box = chunk.world_box();
  const std::vector<std::shared_ptr<const SewerPlan>> plans = world.sewers->near({box.x0, box.y0, box.x1, box.y1});
  if (plans.empty()) return;
  std::vector<const SewerRun*> runs;
  std::vector<const SewerNode*> nodes;
  std::set<NodeKey> seen;
  for (const std::shared_ptr<const SewerPlan>& p : plans) {
    if (p->bb->z0 > box.z1 || p->bb->z1 < box.z0) continue;
    for (const SewerRun& r : p->runs) {
      const double lo = r.axis == 0 ? box.y0 : box.x0;
      const double hi = r.axis == 0 ? box.y1 : box.x1;
      const double c0 = r.axis == 0 ? box.x0 : box.y0;
      const double c1 = r.axis == 0 ? box.x1 : box.y1;
      if (r.l1 < lo || r.l0 > hi || r.fixed + IN_W + SHELL < c0 || r.fixed - IN_W - SHELL > c1) continue;
      runs.push_back(&r);
    }
    for (const SewerNode& n : p->nodes) {
      const NodeKey key = node_key(n.x, n.y);
      if (seen.count(key)) continue;
      const double e = n.R + SHELL + 12;
      if (n.x + e < box.x0 || n.x - e > box.x1 || n.y + e < box.y0 || n.y - e > box.y1) continue;
      seen.insert(key);
      nodes.push_back(&n);
    }
  }
  if (!runs.empty() || !nodes.empty()) rasterize_columns(chunk, tile, runs, nodes);
  for (const std::shared_ptr<const SewerPlan>& p : plans) {
    for (const UndergroundBox& q : p->boxes) {
      if (q.x1 < box.x0 || q.x0 > box.x1 || q.y1 < box.y0 || q.y0 > box.y1 || q.z1 < box.z0 || q.z0 > box.z1) continue;
      chunk.fill_box(q.x0, q.y0, q.z0, q.x1, q.y1, q.z1, q.m, q.mode);
    }
  }
}

}  // namespace svx::city
