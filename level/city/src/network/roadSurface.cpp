// svx_city — network/roadSurface.hpp (voxel_city network/roadSurface.js).
#include "network/roadSurface.hpp"

#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

namespace {

// JS keeps the per-road minima in Float64Arrays of 64 (module scratch): a 65th road's are never
// stored and read back as undefined (NaN here: every comparison false, the same sums). The port
// keeps them per call, so threads share nothing.
constexpr int kPerRoad = 64;

struct NearestJunction {
  const RoadJunction* j = nullptr;
  double rel = js::kInf;
};
NearestJunction nearest_junction(const RoadSeg& s, double along) {
  NearestJunction out;
  for (const RoadJunction& j : s.jn) {
    const double rel = along - j.s;
    if (js::abs(rel) < js::abs(out.rel)) {
      out.rel = rel;
      out.j = &j;
    }
  }
  return out;
}

// Cobbles (setts of ~25 cm) in three shades; wrapping worlds hash canonical positions.
uint16_t cobble_at(double px, double py, double period) {
  const double hx = js::truthy(period) ? js::sar(std::fmod(std::fmod(std::floor(px), period) + period, period), 1) : js::sar(std::floor(px), 1);
  const double hy = js::truthy(period) ? js::sar(std::fmod(std::fmod(std::floor(py), period) + period, period), 1) : js::sar(std::floor(py), 1);
  const uint32_t k = hash32(hx, hy, 0xc0b) & 15;
  return k < 3 ? MAT::COBBLE_DARK : k < 5 ? MAT::COBBLE_LIGHT : MAT::COBBLE;
}

void classify_carriage(const RoadSeg& s, double along, double side, double px, double py, RoadSample& out, double seed) {
  out.kind = RoadKind::CARRIAGE;
  out.dz = 0;
  const double d = js::abs(side);
  // old-town streets and lanes: cobbles with a gutter of dark setts, no markings
  if (s.road->paving == "cobble") {
    out.mat = d > s.hc - 2 && s.hc > 12 ? static_cast<uint16_t>(MAT::COBBLE_DARK) : cobble_at(px, py, out.period);
    return;
  }
  const double a = s.s0 + along;
  const NearestJunction nj = nearest_junction(s, along);
  const RoadJunction* j = nj.j;
  const double rel = nj.rel;
  const double arel = js::abs(rel);
  const bool in_junction = j && arel < j->hc + 2;
  uint16_t mat = MAT::ASPHALT;

  // wear / patches for texture variety (coarse hashed cells)
  const double hx = std::floor(px / 24);
  const double hy = std::floor(py / 24);
  const double hn = out.period / 24;
  const uint32_t hcell = js::truthy(hn) ? hash32(std::fmod(std::fmod(hx, hn) + hn, hn), std::fmod(std::fmod(hy, hn) + hn, hn), seed, 77) : hash32(hx, hy, seed, 77);
  if ((hcell & 31) == 0)
    mat = MAT::ASPHALT_PATCH;
  else if ((hcell & 31) < 4)
    mat = MAT::ASPHALT_WORN;

  if (*s.cls == "alley") {
    out.mat = (hcell & 7) == 0 ? MAT::ASPHALT_WORN : MAT::CONCRETE_DARK;
    return;
  }

  // crosswalks & stop lines
  if (j && j->cw && !in_junction) {
    const double c0 = j->hc + 6;
    const double c1 = j->hc + 30;
    if (arel >= c0 && arel < c1) {
      const int32_t stripe = js::to_int32(std::floor(side + 2048)) & 7;
      out.mat = stripe < 4 ? static_cast<uint16_t>(MAT::LINE_WHITE) : mat;
      return;
    }
    if (arel >= c1 + 4 && arel < c1 + 7 && d < s.hc - s.parking && (rel < 0 ? side < 0 : side > 0) && s.lanes > 1) {
      out.mat = MAT::LINE_WHITE;
      return;
    }
    // lane arrows pointing at the junction, in each approach lane
    const double t = arel - (c1 + 16);
    if (t >= 0 && t < 22 && (*s.cls == "arterial" || *s.cls == "collector") && (rel < 0 ? side < 0 : side > 0)) {
      const double half = s.median / 2;
      const double per_side = js::max(1.0, s.lanes / 2);
      for (double k = 0; k < per_side; k += 1) {
        const double lat = js::abs(d - (half + s.lane * k + s.lane / 2));
        if (t < 7 ? lat <= t * 0.7 : lat < 1) {
          out.mat = MAT::LINE_WHITE;
          return;
        }
      }
    }
  }

  // raised median (arterials); painted (hatched) median near junctions
  if (s.median > 0 && d < s.median / 2) {
    const bool clear = j ? arel < j->hr + 10 : false;
    if (!clear) {
      out.kind = RoadKind::MEDIAN;
      out.dz = 1;
      out.mat = d >= s.median / 2 - 1 ? MAT::CURB : MAT::GRASS_LAWN;
      return;
    }
    if (!in_junction) {
      const bool edge = d >= s.median / 2 - 1;
      const bool hatch = mod(std::floor(a + side * (side < 0 ? -1 : 1)), 12) < 2;
      out.mat = edge || hatch ? static_cast<uint16_t>(MAT::LINE_YELLOW) : mat;
      return;
    }
  }

  if (in_junction) {
    out.mat = mat;
    return;
  }

  if (*s.cls == "rural") {
    if (d >= s.hc - s.shoulder) {
      out.kind = RoadKind::SHOULDER;
      out.mat = MAT::GRAVEL;
      return;
    }
    if (d >= s.hc - s.shoulder - 1) {
      out.mat = MAT::LINE_WHITE;
      return;
    }
    if (d < 1 && mod(std::floor(a), 96) < 32) {
      out.mat = MAT::LINE_YELLOW;
      return;
    }
    out.mat = mat;
    return;
  }

  const double inner = s.hc - s.parking;
  // parking lane boundary + stall ticks
  if (s.parking > 0 && d >= inner - 1) {
    if (d < inner) {
      out.mat = MAT::LINE_WHITE;
      return;
    }
    const double pa = mod(std::floor(a), 48);
    if (pa == 0 && d < s.hc - 2) {
      out.mat = MAT::LINE_WHITE;
      return;
    }
    // occasional manhole / drain grate near curb
    if (d >= s.hc - 3 && mod(std::floor(a), 160) < 4) {
      out.mat = MAT::DRAIN_GRATE;
      return;
    }
    out.mat = mat;
    return;
  }

  if (*s.cls == "arterial") {
    const double half = s.median / 2;
    if (half > 0 && d >= half && d < half + 1) {
      out.mat = MAT::LINE_YELLOW;
      return;
    }
    if (d >= s.hc - 2 && d < s.hc - 1) {
      out.mat = MAT::LINE_WHITE;
      return;
    }
    const double per_side = js::max(1.0, s.lanes / 2);
    for (double k = 1; k < per_side; k += 1) {
      const double lx = half + s.lane * k;
      if (d >= lx - 0.5 && d < lx + 0.5 && mod(std::floor(a), 96) < 24) {
        out.mat = MAT::LINE_WHITE;
        return;
      }
    }
  } else if (*s.cls == "collector") {
    if (d >= 0.5 && d < 1.5) {
      out.mat = MAT::LINE_YELLOW;
      return;
    }
  } else if (*s.cls == "local" || *s.cls == "village") {
    if (side >= 0 && side < 1 && mod(std::floor(a), 72) < 24) {
      out.mat = MAT::LINE_YELLOW;
      return;
    }
  }

  // manholes in lane centers
  const double ma = mod(std::floor(a), 240);
  const double lane_center = s.median / 2 + s.lane / 2;
  if (ma < 6 && js::abs(d - lane_center) < 3) {
    out.mat = MAT::MANHOLE;
    return;
  }
  out.mat = mat;
}

void classify_sidewalk(const RoadSeg& s, double along, double sdf_c, RoadSample& out) {
  out.kind = RoadKind::SIDEWALK;
  out.dz = 1;
  const double q = sdf_c;  // distance from curb face
  out.q = q;
  const double a = s.s0 + along;
  if (s.road->paving == "cobble") {
    // flagstone pavement of an old town
    const double pa = mod(std::floor(a), 8);
    out.mat = pa == 0 || std::fmod(std::floor(q), 7) == 0 ? MAT::SIDEWALK_JOINT : MAT::FLAGSTONE;
    return;
  }
  const std::string_view strip = s.road->strip.empty() ? std::string_view("pits") : std::string_view(s.road->strip);
  const NearestJunction nj = nearest_junction(s, along);
  const bool near_corner = nj.j && js::abs(nj.rel) < nj.j->hr + 4;
  if (s.sidewalk >= 24 && q >= 1 && q < 11 && !near_corner) {
    if (strip == "grass") {
      out.kind = RoadKind::STRIP;
      out.mat = MAT::GRASS_LAWN;
      return;
    }
    if (strip == "pits") {
      const double pa = mod(std::floor(a), 72);
      if (pa >= 30 && pa < 40) {
        out.kind = RoadKind::STRIP;
        out.mat = MAT::TREE_PIT;
        return;
      }
    }
  }
  const double qa = std::floor(q);
  const double pa = mod(std::floor(a), 12);
  out.mat = pa == 0 || std::fmod(qa, 12) == 0 ? MAT::SIDEWALK_JOINT : MAT::SIDEWALK;
}

}  // namespace

RoadSample& sample_road_surface(const std::vector<const RoadSeg*>& cands, double px, double py, RoadSample& out, double seed, double reach) {
  out.kind = RoadKind::NONE;
  out.mat = 0;
  out.dz = 0;
  out.seg = nullptr;
  out.sdf_r = js::kInf;
  out.sdf_c = js::kInf;
  if (cands.empty()) return out;

  // per road (JS: module scratch perRoadRef / perRoadD / perRoadR / perRoadCorner)
  std::vector<const Road*> ref;
  double per_d[kPerRoad], per_r[kPerRoad], per_corner[kPerRoad];
  auto read = [](const double* a, size_t r) { return r < kPerRoad ? a[r] : js::kNaN; };
  int n_roads = 0;
  const RoadSeg* best = nullptr;
  double best_d = js::kInf;
  double best_along = 0;
  double best_side = 0;
  bool pedestrian = false;

  for (const RoadSeg* sp : cands) {
    const RoadSeg& s = *sp;
    const double vx = px - s.ax;
    const double vy = py - s.ay;
    const double t = vx * s.dx + vy * s.dy;
    const double side = s.dx * vy - s.dy * vx;
    const double tc = t < 0 ? 0 : t > s.len ? s.len : t;
    const double ex = px - (s.ax + s.dx * tc);
    const double ey = py - (s.ay + s.dy * tc);
    const double dist = std::sqrt(ex * ex + ey * ey);
    if (dist > s.hr + js::max(s.road->corner, reach) + 1) continue;
    const double dc = s.hc > 0 ? dist - s.hc : js::kInf;
    const double dr = dist - s.hr;
    // aggregate per road with plain min
    int slot = -1;
    for (int r = 0; r < n_roads; ++r) {
      if (ref[static_cast<size_t>(r)] == s.road.get()) {
        slot = r;
        break;
      }
    }
    if (slot < 0) {
      slot = n_roads++;
      ref.push_back(s.road.get());
      if (slot < kPerRoad) {
        per_d[slot] = dc;
        per_r[slot] = dr;
        per_corner[slot] = s.road->corner;
      }
    } else if (slot < kPerRoad) {
      if (dc < per_d[slot]) per_d[slot] = dc;
      if (dr < per_r[slot]) per_r[slot] = dr;
    }
    const double score = s.hc > 0 ? dc : dr + 1000;
    if (score < best_d) {
      best_d = score;
      best = &s;
      best_along = t;
      best_side = side;
    }
    if (*s.cls == "pedestrian" && dr < 0) pedestrian = true;
  }
  if (!n_roads) return out;

  double sdf_c = js::kInf;
  double sdf_r = js::kInf;
  // (the curb fillets fold road by road, each with its own corner radius, so the fold's order
  // matters: the angled world's roads, which carry the cell that owns them, fold in id order, the
  // same for any candidate list, so neighbouring tiles agree on every column)
  // (and a fillet takes the smaller corner of the roads it joins: a lane without a sidewalk never
  // gets a big street's fillet bulging into a lot)
  std::vector<int> order;
  const bool ordered = n_roads > 1 && ref[0]->home.has_value();
  if (ordered) {
    // slots 0..n-1 by the id of their road (insertion sort: a handful of roads)
    order.resize(static_cast<size_t>(n_roads));
    for (int r = 0; r < n_roads; ++r) order[static_cast<size_t>(r)] = r;
    auto by_id = [&](int a, int b) { return js::compare(ref[static_cast<size_t>(a)]->id, ref[static_cast<size_t>(b)]->id); };
    for (int i = 1; i < n_roads; ++i) {
      const int v = order[static_cast<size_t>(i)];
      int j = i - 1;
      while (j >= 0 && by_id(order[static_cast<size_t>(j)], v) > 0) {
        order[static_cast<size_t>(j + 1)] = order[static_cast<size_t>(j)];
        j -= 1;
      }
      order[static_cast<size_t>(j + 1)] = v;
    }
  }
  double k_min = js::kInf;
  for (int q = 0; q < n_roads; ++q) {
    const size_t r = static_cast<size_t>(ordered ? order[static_cast<size_t>(q)] : q);
    const double d = read(per_d, r);
    if (d != js::kInf) {
      if (sdf_c == js::kInf)
        sdf_c = d;
      else
        sdf_c = smin_circular(sdf_c, d, js::min(ordered ? js::min(k_min, read(per_corner, r)) : read(per_corner, r), 64.0));
      k_min = js::min(k_min, read(per_corner, r));
    }
    if (read(per_r, r) < sdf_r) sdf_r = read(per_r, r);
  }
  out.sdf_c = sdf_c;
  out.sdf_r = sdf_r;
  out.seg = best;
  out.along = best_along;
  out.side = best_side;

  if (sdf_c < 0) {
    classify_carriage(*best, best_along, best_side, px, py, out, seed);
    return out;
  }
  if (sdf_r >= 0) return out;
  if (pedestrian && *best->cls == "pedestrian") {
    out.kind = RoadKind::PLAZA;
    out.dz = 1;
    const double a = std::floor(best->s0 + best_along);
    const double l = std::floor(best_side + 4096);
    if (best->road->paving == "cobble")
      out.mat = cobble_at(px, py, out.period);
    else
      out.mat = (js::to_int32(a) & 15) == 0 || (js::to_int32(l) & 15) == 0 ? MAT::PLAZA_STONE_DARK : MAT::PLAZA_STONE;
    return out;
  }
  if (sdf_c < 1 && best->sidewalk > 0) {
    out.kind = RoadKind::CURB;
    out.mat = MAT::CURB;
    out.dz = 1;
    // lowered curb with tactile paving where a crosswalk lands
    const NearestJunction nj = nearest_junction(*best, best_along);
    if (nj.j && nj.j->cw && js::abs(nj.rel) >= nj.j->hc + 6 && js::abs(nj.rel) < nj.j->hc + 30) {
      out.mat = MAT::TACTILE;
      out.dz = 0;
    }
    return out;
  }
  if (best->sidewalk <= 0 && best->hc > 0 && sdf_c < best->hr - best->hc + 1) {
    // alley / rural apron
    return out;
  }
  classify_sidewalk(*best, best_along, sdf_c, out);
  return out;
}

}  // namespace svx::city
