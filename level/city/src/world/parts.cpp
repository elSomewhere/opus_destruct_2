// svx_city — voxel_city world/parts.js.
#include "world/parts.hpp"

#include <cmath>

#include "svx/base/types.hpp"

namespace svx::city {

namespace {
// Slack (voxels) of the resident check's geometry, always in the cap's favour (a centre that far
// out still counts).
constexpr double kEps = 1e-6;
}  // namespace

double part_id(double ci, double cj, double k) {
  if (k < 0 || k > 255) SVX_FAIL("parts: index out of range");
  const int32_t a = js::to_int32(ci) & 2047;
  const int32_t b = js::to_int32(cj) & 2047;
  return static_cast<double>((a << 19) | (b << 8) | js::to_int32(k)) + 1;
}

double part_priority(std::string_view kind, double id, bool yields_to_grid) {
  // PRIORITY_CLASS: a building owns what it shares with its wings, both what they share with a
  // road piece or a garage's ramp
  const double cls = kind == "road" ? 1 : kind == "ramp" ? 1 : kind == "wing" ? 2 : kind == "building" ? 3 : 1;
  const double p = cls * 16777216.0 + std::fmod(id, 16777216.0);
  return yields_to_grid ? -(67108864.0 - p) : p;
}

ChunkXYZ home_chunk(double x, double y, double z, const Rect& cell_rect) {
  double cx = chunk_of(x);
  double cy = chunk_of(y);
  while (owner_corner(cx) < cell_rect.x0) cx += 1;
  while (owner_corner(cx) >= cell_rect.x1) cx -= 1;
  while (owner_corner(cy) < cell_rect.y0) cy += 1;
  while (owner_corner(cy) >= cell_rect.y1) cy -= 1;
  return {cx, cy, chunk_of(z)};
}

Part make_part(const PartCell& cell, double index, const std::string& key, const std::string& kind, const Placement& placement, const LocalBox& extent,
               bool anchored) {
  Part p;
  p.aabb = placement.local_bounds_to_world_aabb(extent);
  // home: the world voxel under the middle of the extent, at its base
  const std::array<double, 3> h = placement.to_world(std::floor((extent.u0 + extent.u1) / 2), std::floor((extent.v0 + extent.v1) / 2), extent.w0);
  p.home = home_chunk(h[0], h[1], h[2], cell.rect);
  p.id = part_id(cell.ci, cell.cj, index);
  p.cell = {cell.i, cell.j};
  p.key = key;
  p.kind = kind;
  p.placement = placement;
  p.extent = extent;
  p.base = {h[0], h[1], h[2]};
  p.reach = reach_of(p.aabb, p.home);
  p.anchored = anchored;
  p.priority = placement.priority;
  return p;
}

PartBudget::PartBudget(const Value& config, const Rect& cell_rect, PartLattice lattice_) : rect(cell_rect), lattice(std::move(lattice_)) {
  const Value& a = config["world"]["angles"];
  enabled = a["enabled"].truthy();
  max_per_chunk = a["maxPartsPerChunk"].num(1);
  // squares of at least cluster x partArea (m2), within the cell, holding cluster parts each
  cluster = js::max(1.0, a["partCluster"].num(4));
  const double side = std::sqrt(a["partArea"].num(3600) * cluster) * 8;
  nx = js::max(1.0, std::floor((cell_rect.x1 - cell_rect.x0) / side));
  ny = js::max(1.0, std::floor((cell_rect.y1 - cell_rect.y0) / side));
  // (8-bit part indices: at most 255 parts per cell)
  limit = js::min(255.0, nx * ny * cluster);
  used.assign(static_cast<size_t>(nx * ny), 0);
  max_resident = a["maxResident"].num(8);
  R = resident_d(config) / 2;
}

std::vector<std::array<double, 4>> PartBudget::cells_near(double x, double y) const {
  auto g = [](const Rect& c) { return std::array<double, 4>{c.x0 - kChunk / 2.0, c.y0 - kChunk / 2.0, c.x1 + kChunk / 2.0, c.y1 + kChunk / 2.0}; };
  std::vector<std::array<double, 4>> out = {g(rect)};
  if (!lattice) return out;
  const double r = 2 * R + kChunk;
  const double i1 = lattice.index_at(0, x + r);
  const double j0 = lattice.index_at(1, y - r);
  const double j1 = lattice.index_at(1, y + r);
  for (double i = lattice.index_at(0, x - r); i <= i1; i += 1)
    for (double j = j0; j <= j1; j += 1) {
      const Rect c = lattice.cell_rect(i, j);
      if (c.x0 == rect.x0 && c.y0 == rect.y0) continue;
      out.push_back(g(c));
    }
  return out;
}

bool PartBudget::resident(const ChunkXYZ& home) const {
  const double hx = home.cx * kChunk + kChunk / 2.0;
  const double hy = home.cy * kChunk + kChunk / 2.0;
  std::vector<Point2> pts = {{hx, hy}};
  for (const Point2& q : homes)
    if (js::pow(q.x - hx, 2) + js::pow(q.y - hy, 2) <= 4 * R * R) pts.push_back(q);
  const std::vector<std::array<double, 4>> cells = cells_near(hx, hy);
  // (a disc with every one of them and every cell round could still hold them all)
  if (static_cast<double>(pts.size()) <= std::floor(max_resident / static_cast<double>(cells.size()))) return true;
  std::vector<Point2> circles = pts;
  std::vector<double> vlines, hlines;
  for (const auto& c : cells) {
    vlines.push_back(c[0] - R);
    vlines.push_back(c[2] + R);
    hlines.push_back(c[1] - R);
    hlines.push_back(c[3] + R);
    circles.push_back({c[0], c[1]});
    circles.push_back({c[2], c[1]});
    circles.push_back({c[0], c[3]});
    circles.push_back({c[2], c[3]});
  }
  const double R2 = js::pow(R + kEps, 2);
  auto over = [&](double cx, double cy) {
    if (js::pow(cx - hx, 2) + js::pow(cy - hy, 2) > R2) return false;
    double n = 0;
    for (const Point2& p : pts)
      if (js::pow(p.x - cx, 2) + js::pow(p.y - cy, 2) <= R2) n += 1;
    double t = 0;
    for (const auto& c : cells) {
      const double dx = js::max(c[0] - cx, 0.0, cx - c[2]);
      const double dy = js::max(c[1] - cy, 0.0, cy - c[3]);
      if (dx * dx + dy * dy <= R2) t += 1;
    }
    return n > std::floor(max_resident / t);
  };
  for (const Point2& p : pts)
    if (over(p.x, p.y)) return false;
  // circles with circles (all of radius R)
  for (size_t a = 0; a < circles.size(); ++a)
    for (size_t b = a + 1; b < circles.size(); ++b) {
      const double ax = circles[a].x, ay = circles[a].y;
      const double bx = circles[b].x, by = circles[b].y;
      const double dx = bx - ax;
      const double dy = by - ay;
      const double d2 = dx * dx + dy * dy;
      if (d2 == 0 || d2 > 4 * R * R) continue;
      const double k = std::sqrt(js::max(0.0, R * R / d2 - 0.25));
      const double mx = ax + dx / 2;
      const double my = ay + dy / 2;
      if (over(mx - dy * k, my + dx * k) || over(mx + dy * k, my - dx * k)) return false;
    }
  // circles with the reaches' edges, and the edges with each other
  for (const Point2& c : circles) {
    for (const double x : vlines) {
      const double e = R * R - js::pow(x - c.x, 2);
      if (e >= 0 && (over(x, c.y - std::sqrt(e)) || over(x, c.y + std::sqrt(e)))) return false;
    }
    for (const double y : hlines) {
      const double e = R * R - js::pow(y - c.y, 2);
      if (e >= 0 && (over(c.x - std::sqrt(e), y) || over(c.x + std::sqrt(e), y))) return false;
    }
  }
  for (const double x : vlines)
    for (const double y : hlines)
      if (over(x, y)) return false;
  return true;
}

double PartBudget::square(double x, double y) const {
  const Rect& r = rect;
  const double i = js::min(nx - 1, js::max(0.0, std::floor(((x - r.x0) * nx) / (r.x1 - r.x0))));
  const double j = js::min(ny - 1, js::max(0.0, std::floor(((y - r.y0) * ny) / (r.y1 - r.y0))));
  return i + j * nx;
}

bool PartBudget::fits(double x, double y, const ChunkXYZ& home) const {
  if (!enabled || count >= limit) return false;
  const double sq = square(x, y);
  // (used[NaN] is undefined in JS: never full)
  if (sq == sq && used[static_cast<size_t>(sq)] >= cluster) return false;
  auto it = chunks_.find(js::cat(home.cx, ",", home.cy, ",", home.cz));
  if ((it == chunks_.end() ? 0 : it->second) >= max_per_chunk) return false;
  return resident(home);
}

double PartBudget::grant(double x, double y, const ChunkXYZ& home) {
  if (!fits(x, y, home)) return -1;
  const double sq = square(x, y);
  if (sq == sq) used[static_cast<size_t>(sq)] = static_cast<uint8_t>(used[static_cast<size_t>(sq)] + 1);
  chunks_[js::cat(home.cx, ",", home.cy, ",", home.cz)] += 1;
  homes.push_back({home.cx * kChunk + kChunk / 2.0, home.cy * kChunk + kChunk / 2.0});
  const double k = count;
  count += 1;
  return k;
}

}  // namespace svx::city
