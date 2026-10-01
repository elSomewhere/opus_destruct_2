// svx_city — oriented parts (voxel_city world/parts.js, ANGLED_WORLD_PLAN.md §4.3): the unit of
// orientation.
//
// A part is one coherent object in a lattice of its own (a turned building, a pitched road
// segment, a garage's ramp, a wing): in structvox one oriented grid, here a record on the cell
// plan that made it (plan.parts). Everything axis-aligned stays in the world grid and is no part.
//
// The physics budget: every part is a resident grid there and the cost of resident grids is
// global, so a cell grants parts from a budget (PartBudget), in a fixed order, from its own list
// only (no neighbour ever asked):
//   - angles.partArea (m2): one part per that much of the cell on average; the cell is cut into
//     budget squares of angles.partCluster times that area, each holding that many parts;
//   - angles.maxResident: the parts in any disc of angles.residentRadius (structvox's 96 m load
//     radius), whichever cells they are of - a cell keeps, in every such disc, at most
//     floor(maxResident / t) of its parts, t the cells whose parts the disc could hold (from the
//     arterial lattice's lines: cells_near), checked where the count and t can be highest, the
//     vertices of the arrangement of the circles round the cell's parts and the edges of the
//     cells' reach (resident);
//   - angles.maxPartsPerChunk: parts at home in one chunk.
// A part is at home in one chunk, owned by its cell (the cell holding the chunk's last voxel), so
// the caps are exact without neighbours; it reaches at most kPartReach chunks from it
// horizontally (structvox's far tier gathers a tile's grids from home chunks that close).
#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/js.hpp"
#include "core/math.hpp"
#include "core/placement.hpp"
#include "core/rect.hpp"
#include "core/value.hpp"

namespace svx::city {

class World;

// PART_REACH: chunks a part may reach from its home chunk, horizontally.
constexpr int kPartReach = 4;

// residentD: the diameter (voxels) of the resident disc of a config (angles.residentRadius, m;
// structvox's 96 m load radius by default).
inline double resident_d(const Value& config) { return 2 * config["world"]["angles"]["residentRadius"].num(96) * 8; }

// partId: a stable part id (u32, never 0) - the canonical cell (11 bits per axis, so ids repeat
// only 2,000 cells apart, never resident together) and the part's index in the cell's grant
// order (8 bits; outside 0..255 a programming error).
double part_id(double ci, double cj, double k);

// partPriority: a part's structvox priority (GridDesc::priority: of two grids the higher owns
// their overlap; the world grid's is 0). Unique, from its id: a building's above any wing's, both
// above any road piece's or ramp's, all above the world grid; a part cast into what the world
// grid holds (yields_to_grid: a wing of a building square to the grid, a garage's ramp) yields to
// it: negative, the lower the class the lower. Fits a structvox i32.
double part_priority(std::string_view kind, double id, bool yields_to_grid = false);

// Chunk coordinate of a world voxel coordinate, and the cell owning a chunk: the one holding its
// last voxel (its max corner) - a chunk straddling an arterial line belongs to the cell east or
// south of it, the cell that owns the road on that line.
inline double chunk_of(double v) { return std::floor(v / kChunk); }
inline double owner_corner(double c) { return c * kChunk + kChunk - 1; }

struct ChunkXYZ {
  double cx = 0, cy = 0, cz = 0;
};
// The home chunk of a part whose base centre is world voxel (x, y, z), moved into a chunk owned by
// its cell (cell rect [x0, x1) x [y0, y1), as the arterial lines are).
ChunkXYZ home_chunk(double x, double y, double z, const Rect& cell_rect);
// Horizontal reach (chunks) of a world box (its x0, y0, x1, y1) from a home chunk.
inline double reach_of(const Rect& aabb, const ChunkXYZ& home) {
  return js::max(home.cx - chunk_of(aabb.x0), chunk_of(aabb.x1) - home.cx, home.cy - chunk_of(aabb.y0), chunk_of(aabb.y1) - home.cy);
}
inline double reach_of(const Box3& aabb, const ChunkXYZ& home) { return reach_of(Rect{aabb.x0, aabb.y0, aabb.x1, aabb.y1}, home); }

// The cell a part is made for: { i, j, ci, cj (canonical), rect }.
struct PartCell {
  double i = 0, j = 0, ci = 0, cj = 0;
  Rect rect;
};

struct PartRamp {
  double f = 0, W = 0, Lu = 0;
};

// A part record (makePart), and what the cell plan and the producers set on it afterwards: its id
// and priority once granted (the placement's priority with it), a building row's members, a wing's
// or ramp's envelope, a ramp's, a road piece's road, segment, arc range, grade and half width.
struct Part {
  double id = 0;
  std::array<double, 2> cell{};  // (the owner cell (i, j): its plan holds what the part is of)
  std::string key;
  std::string kind;  // "building", "road", "ramp", "wing"
  Placement placement;
  LocalBox extent;
  Box3 aabb;
  ChunkXYZ home;
  XYZ base;  // (the world voxel under the middle of its base: the budget square it takes)
  double reach = 0;
  bool anchored = false;
  double priority = 0;
  std::vector<std::string> members;  // (building)
  std::string env;                   // (wing, ramp)
  std::optional<PartRamp> ramp;      // (ramp)
  std::string road;                  // (road; "" none)
  double seg = js::kNaN, s0 = js::kNaN, s1 = js::kNaN, grade = js::kNaN, hr = js::kNaN;  // (road)
};

// makePart: placement with its local extent (inclusive cells); index its place in the cell's
// grant order (0 until granted).
Part make_part(const PartCell& cell, double index, const std::string& key, const std::string& kind, const Placement& placement, const LocalBox& extent,
               bool anchored = false);

// The arterial lattice a budget asks for the cells round it (network/arterials.js ArterialGrid:
// indexAt(axis, v), cellRect(i, j)); without one (empty functions) the cell is taken to be alone.
struct PartLattice {
  std::function<double(int axis, double v)> index_at;
  std::function<Rect(double i, double j)> cell_rect;
  explicit operator bool() const { return static_cast<bool>(index_at); }
};

// The parts budget of one cell. Candidates ask in a fixed order; grant() says whether a part at
// home in chunk `home` whose base centre is (x, y) fits, and books it if so. Off (angles
// disabled): none. (A cell plan makes one, asks it in order, and drops it.)
class PartBudget {
 public:
  PartBudget(const Value& config, const Rect& cell_rect, PartLattice lattice = {});

  bool enabled = false;
  Rect rect;
  double max_per_chunk = 1;
  double cluster = 4;
  double nx = 1, ny = 1;
  double limit = 0;
  std::vector<uint8_t> used;  // (a Uint8Array: parts per budget square)
  double count = 0;
  double max_resident = 8;
  double R = 0;
  PartLattice lattice;
  std::vector<Point2> homes;  // (the granted parts' home chunk centres)

  // Where each cell whose parts a disc of the resident radius centred near (x, y) could hold
  // keeps its homes: [x0, y0, x1, y1] (voxels), this cell's first.
  std::vector<std::array<double, 4>> cells_near(double x, double y) const;
  // Is the resident cap kept with a part at home in `home` added?
  bool resident(const ChunkXYZ& home) const;
  // The budget square of a point in the cell (NaN for a NaN point: JS's typed array ignores it).
  double square(double x, double y) const;
  // Would a part at home in `home` with base centre (x, y) fit?
  bool fits(double x, double y, const ChunkXYZ& home) const;
  // Book a part if it fits: its index in the cell's grant order, or -1.
  double grant(double x, double y, const ChunkXYZ& home);

 private:
  // parts at home per chunk (JS: a Map keyed by `${cx},${cy},${cz}`)
  std::map<std::string, double> chunks_;
};

// roadPieces: a road cut into the pieces an inclined road surface is made of - every straight
// segment of its centre line in as few equal pieces as keep each, right-of-way included, within
// kPartReach chunks of its home chunk (in a chunk its cell owns). Keys `road id/p<segment>.<piece>`
// are stable; home.cz is left to whoever knows the level. (JS caches them on the road: its port
// keeps them in a Lazy.) Pts: a sequence of points with x, y.
struct RoadPiece {
  std::string key;
  double seg = 0, k = 0, s0 = 0, s1 = 0;
  Point2 a, b;
  Rect aabb;
  ChunkXYZ home;
  double reach = 0;
};
template <class Pts>
std::vector<RoadPiece> road_pieces(const std::string& road_id, const Pts& pts, double hr, const Rect& cell_rect) {
  std::vector<RoadPiece> out;
  double acc = 0;
  for (size_t si = 0; si + 1 < pts.size(); ++si) {
    const auto& p = pts[si];
    const auto& q = pts[si + 1];
    const double len = std::sqrt(js::pow(q.x - p.x, 2) + js::pow(q.y - p.y, 2));
    if (len < 1e-6) continue;
    auto at = [&](double t) { return Point2{p.x + ((q.x - p.x) * t) / len, p.y + ((q.y - p.y) * t) / len}; };
    // (the piece is the right-of-way between its two end cross-sections)
    const double nx = (-(q.y - p.y) / len) * hr;
    const double ny = ((q.x - p.x) / len) * hr;
    auto piece = [&](double n, double k) {
      RoadPiece rp;
      rp.a = at((len * k) / n);
      rp.b = at((len * (k + 1)) / n);
      const Point2& a = rp.a;
      const Point2& b = rp.b;
      rp.aabb = {std::floor(js::min(a.x + nx, a.x - nx, b.x + nx, b.x - nx)), std::floor(js::min(a.y + ny, a.y - ny, b.y + ny, b.y - ny)),
                 std::ceil(js::max(a.x + nx, a.x - nx, b.x + nx, b.x - nx)), std::ceil(js::max(a.y + ny, a.y - ny, b.y + ny, b.y - ny))};
      rp.home = home_chunk((a.x + b.x) / 2, (a.y + b.y) / 2, 0, cell_rect);
      rp.key = js::cat(road_id, "/p", static_cast<double>(si), ".", k);
      rp.seg = static_cast<double>(si);
      rp.k = k;
      rp.s0 = acc + (len * k) / n;
      rp.s1 = acc + (len * (k + 1)) / n;
      rp.reach = reach_of(rp.aabb, rp.home);
      return rp;
    };
    // (shorter pieces help only while a piece's length, not the road's width, sets its reach:
    // pieces of 8 m at least; a piece that still reaches further keeps its true reach, and stays
    // in the world grid)
    const double n_max = js::max(1.0, std::floor(len / 64));
    double best = 1;
    double best_reach = js::kInf;
    for (double n = 1; n <= n_max; n += 1) {
      double r = 0;
      for (double k = 0; k < n; k += 1) r = js::max(r, piece(n, k).reach);
      if (r < best_reach) {
        best = n;
        best_reach = r;
      }
      if (r <= kPartReach) break;
    }
    for (double k = 0; k < best; k += 1) out.push_back(piece(best, k));
    acc += len;
  }
  return out;
}

// ---- left for the cell plan's port (they read world.cellPlan(i, j).parts): declared here,
// defined with CellPlan (city/cellPlan.cpp). Parts are a cell plan's, so a part outliving the
// call holds its plan (an aliasing shared_ptr).
//
// partsIn: parts whose world box overlaps a rect (voxels), from the cells that can reach it
// (world.cellsOverlapping of the rect grown by kPartReach * kChunk + kChunk), each once, in the
// cells' order.
std::vector<std::shared_ptr<const Part>> parts_in(const World& world, const Rect& rect);
// partsHomedIn: the parts at home in a chunk (structvox's ChunkSource::grids(chunk)): only the
// chunk's own cell (world.cellAt(owner_corner(cx), owner_corner(cy))) can have homed them there.
std::vector<std::shared_ptr<const Part>> parts_homed_in(const World& world, double cx, double cy, double cz);

}  // namespace svx::city
