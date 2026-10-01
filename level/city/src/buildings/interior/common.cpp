// svx_city — voxel_city buildings/interior/common.js.
#include "buildings/interior/common.hpp"

#include "svx/base/types.hpp"

namespace svx::city {

std::shared_ptr<Room> add_stair_room(FloorGrid& grid, const Stair& stair) {
  RoomProps props;
  props.stair = stair.id;
  props.paint = "PAINT_GRAY";
  props.floor_mat = "STAIR_CONCRETE";
  return grid.add_room("stair", {stair.rect}, props);
}

namespace {

std::shared_ptr<Door> try_stair_door(FloorGrid& grid, const Room& stair_room, const Stair& stair, const Room* circ, const DoorOpts& opts,
                                     const Rect& land, const std::vector<WallRun>& runs, double width) {
  const WallRun* best = nullptr;
  double best_t = 0;
  for (const WallRun& run : runs) {
    // the run's cells along the wall that are adjacent to the landing
    double lo, hi;
    if (run.orient == 'v') {
      lo = js::max(run.t0, land.y0);
      hi = js::min(run.t1, land.y1);
    } else {
      lo = js::max(run.t0, land.x0);
      hi = js::min(run.t1, land.x1);
    }
    // wall must actually border the landing (fixed coordinate adjacent to landing rect)
    const bool adj = run.orient == 'v' ? run.fixed == land.x1 + 1 || run.fixed + run.thick - 1 == land.x0 - 1
                                       : run.fixed == land.y1 + 1 || run.fixed + run.thick - 1 == land.y0 - 1;
    if (!adj) continue;
    if (hi - lo + 1 < width) continue;
    best = &run;
    best_t = js::round((lo + hi - width + 1) / 2);
    break;
  }
  if (!best) return nullptr;
  const WallRun& run = *best;
  const double t = best_t;
  auto d = std::make_shared<Door>();
  if (run.orient == 'v') {
    d->u0 = run.fixed, d->u1 = run.fixed + run.thick - 1, d->v0 = t, d->v1 = t + width - 1, d->orient = 'v';
  } else {
    d->u0 = t, d->u1 = t + width - 1, d->v0 = run.fixed, d->v1 = run.fixed + run.thick - 1, d->orient = 'h';
  }
  d->a = stair_room.id;
  d->b = circ ? circ->id : -1;
  d->kind = opts.kind ? *opts.kind : "stair";
  d->side_a = run.side_a;
  d->width = width;
  d->leaf = opts.leaf ? *opts.leaf : (stair.open ? "none" : "metal");
  d->id = static_cast<double>(grid.doors.size());
  for (double v = d->v0; v <= d->v1; v += 1)
    for (double u = d->u0; u <= d->u1; u += 1) grid.set(u, v, 3);
  grid.doors.push_back(d);
  return d;
}

}  // namespace

std::shared_ptr<Door> stair_door(FloorGrid& grid, const Room& stair_room, const Stair& stair, const Room* circ, const DoorOpts& opts) {
  const Rect land = near_landing(stair);
  const std::vector<WallRun> runs = grid.wall_runs(stair_room, circ);
  const double want = opts.width.value_or(8);
  for (double width = want; width >= 6; width -= 1) {
    auto d = try_stair_door(grid, stair_room, stair, circ, opts, land, runs, width);
    if (d) return d;
  }
  return nullptr;
}

std::vector<std::array<double, 2>> split_length(double a0, double a1, double target, double min_w, Rng& rng, double jitter) {
  const double len = a1 - a0 + 1;
  double n = js::max(1.0, js::round((len + 1) / (target + 1)));
  while (n > 1 && (len - (n - 1)) / n < min_w) n -= 1;
  std::vector<std::array<double, 2>> pieces;
  double a = a0;
  const double avail = len - (n - 1);
  double used = 0;
  for (double k = 0; k < n; k += 1) {
    const double remaining = n - k;
    double w;
    if (remaining == 1)
      w = avail - used;
    else {
      const double ideal = (avail - used) / remaining;
      w = js::round(ideal * (1 + (rng.next() - 0.5) * jitter));
      w = js::max(min_w, js::min(w, avail - used - min_w * (remaining - 1)));
      w = js::or_(js::round(w / 4) * 4, min_w);
    }
    pieces.push_back({a, a + w - 1});
    a += w + 1;
    used += w;
  }
  if (pieces.empty()) SVX_FAIL("common: splitLength of no pieces (a NaN length)");
  pieces.back()[1] = a1;
  return pieces;
}

std::vector<char> facade_sides_of(const FloorGrid& grid, const Rect& r) {
  std::vector<char> out;
  auto probe = [&](double u, double v) { return grid.get(u, v) == FloorGrid::EXT; };
  auto add = [&](char s) {
    for (char c : out)
      if (c == s) return;
    out.push_back(s);
  };
  if (probe(js::round((r.x0 + r.x1) / 2), r.y0 - 1)) add('N');
  if (probe(js::round((r.x0 + r.x1) / 2), r.y1 + 1)) add('S');
  if (probe(r.x0 - 1, js::round((r.y0 + r.y1) / 2))) add('W');
  if (probe(r.x1 + 1, js::round((r.y0 + r.y1) / 2))) add('E');
  return out;
}

bool rects_overlap_any(const Rect& r, const std::vector<Rect>& list) {
  for (const Rect& x : list)
    if (r_overlaps(x, r)) return true;
  return false;
}

std::vector<std::array<double, 2>> free_intervals(double a0, double a1, const std::vector<std::array<double, 2>>& blocked) {
  std::vector<std::array<double, 2>> sorted = blocked;
  js::sort(sorted, [](const std::array<double, 2>& x, const std::array<double, 2>& y) { return x[0] - y[0]; });
  std::vector<std::array<double, 2>> out;
  double a = a0;
  for (const auto& b : sorted) {
    const double b0 = b[0], b1 = b[1];
    if (b1 < a) continue;
    if (b0 - 2 >= a) out.push_back({a, b0 - 2});
    a = js::max(a, b1 + 2);
  }
  if (a <= a1) out.push_back({a, a1});
  return out;
}

}  // namespace svx::city
