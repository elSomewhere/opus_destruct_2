#include "svx/doom/voxelize.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <deque>

#include "svx/doom/specials.hpp"

namespace svx::doom {

namespace {

i32 floor_div(i32 a, i32 b) {
  i32 q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

i32 round_div(i32 a, i32 b) { return floor_div(a + b / 2, b); }

struct Crossing {
  f64 x;
  i32 sector;  // sector entered when moving in +x, or -1 for void
  i32 line;
};

// Separable square (Chebyshev) max / min filters on an nx*ny grid.
void max_filter(std::vector<i32>& a, i32 nx, i32 ny, i32 r) {
  std::vector<i32> t(a.size());
  for (i32 y = 0; y < ny; ++y)
    for (i32 x = 0; x < nx; ++x) {
      i32 m = INT_MIN;
      for (i32 k = std::max(0, x - r); k <= std::min(nx - 1, x + r); ++k) m = std::max(m, a[y * nx + k]);
      t[y * nx + x] = m;
    }
  for (i32 y = 0; y < ny; ++y)
    for (i32 x = 0; x < nx; ++x) {
      i32 m = INT_MIN;
      for (i32 k = std::max(0, y - r); k <= std::min(ny - 1, y + r); ++k) m = std::max(m, t[k * nx + x]);
      a[y * nx + x] = m;
    }
}

void min_filter(std::vector<i32>& a, i32 nx, i32 ny, i32 r) {
  for (auto& v : a) v = -v;
  max_filter(a, nx, ny, r);
  for (auto& v : a) v = -v;
}

}  // namespace

bool voxelize(const Map& map, const VoxelizeOptions& opt, ColumnGrid* out, VoxelizeStats* stats, std::string* err,
              std::vector<i32>* sector_map, std::vector<MoverInfo>* movers) {
  if (map.vertices.empty() || map.linedefs.empty() || map.sectors.empty()) {
    if (err) *err = "empty map";
    return false;
  }
  const i32 upv = opt.units_per_voxel;
  const i32 T = opt.shell_voxels;
  i32 minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN;
  for (const Vertex& v : map.vertices) {
    minx = std::min(minx, v.x);
    miny = std::min(miny, v.y);
    maxx = std::max(maxx, v.x);
    maxy = std::max(maxy, v.y);
  }
  const i32 margin = T + 2;
  const i32 ox = (floor_div(minx, upv) - margin) * upv;
  const i32 oy = (floor_div(miny, upv) - margin) * upv;
  const i32 nx = floor_div(maxx - ox, upv) + 1 + margin;
  const i32 ny = floor_div(maxy - oy, upv) + 1 + margin;
  const size_t ncol = size_t(nx) * ny;

  // ---- 2D sector map via scanline crossings --------------------------------------
  std::vector<i32> sec(ncol, -1);
  std::vector<Crossing> xs;
  for (i32 j = 0; j < ny; ++j) {
    const f64 yc = oy + (j + 0.5) * upv;
    xs.clear();
    for (size_t li = 0; li < map.linedefs.size(); ++li) {
      const Linedef& L = map.linedefs[li];
      if (L.v1 >= map.vertices.size() || L.v2 >= map.vertices.size()) continue;
      const Vertex& a = map.vertices[L.v1];
      const Vertex& b = map.vertices[L.v2];
      const f64 y1 = a.y, y2 = b.y;
      const bool cross = (y1 <= yc && yc < y2) || (y2 <= yc && yc < y1);
      if (!cross) continue;
      const f64 xc = a.x + (yc - y1) * (f64(b.x) - a.x) / (y2 - y1);
      // Moving +x we enter the right (front) side iff dy > 0.
      const int side = (y2 - y1) > 0 ? 0 : 1;
      i32 s = -1;
      const u16 sd = L.side[side];
      if (sd != 0xFFFF && sd < map.sidedefs.size()) {
        const u16 sn = map.sidedefs[sd].sector;
        if (sn < map.sectors.size()) s = sn;
      }
      xs.push_back({xc, s, static_cast<i32>(li)});
    }
    std::sort(xs.begin(), xs.end(), [](const Crossing& p, const Crossing& q) {
      return p.x != q.x ? p.x < q.x : p.line < q.line;
    });
    size_t k = 0;
    i32 cur = -1;
    for (i32 i = 0; i < nx; ++i) {
      const f64 xc = ox + (i + 0.5) * upv;
      while (k < xs.size() && xs[k].x < xc) cur = xs[k++].sector;
      sec[size_t(j) * nx + i] = cur;
    }
  }

  // ---- thicken diagonal-only void contacts (6-connectivity of thin walls) ----------
  i32 fixes = 0;
  if (opt.fix_diagonals) {
    for (int pass = 0; pass < 8; ++pass) {
      i32 changed = 0;
      for (i32 j = 0; j + 1 < ny; ++j)
        for (i32 i = 0; i + 1 < nx; ++i) {
          const size_t a = size_t(j) * nx + i, b = a + 1, c = a + nx, d = c + 1;
          const bool va = sec[a] < 0, vb = sec[b] < 0, vc = sec[c] < 0, vd = sec[d] < 0;
          if (va && vd && !vb && !vc) {
            sec[b] = -1;
            ++changed;
          } else if (vb && vc && !va && !vd) {
            sec[a] = -1;
            ++changed;
          }
        }
      fixes += changed;
      if (!changed) break;
    }
  }

  // ---- movers: sector planes moved by line specials (vanilla rules, doom/specials) ------
  // Each plane gathers the moves of the lines acting on it, with targets from the map's
  // heights. Its envelope (start and every target) is voxelized most open - a floor at its
  // lowest, a ceiling at its highest - and the engine fills the rest.
  const size_t nsec = map.sectors.size();
  auto side_sector = [&](const Linedef& L, int side) -> i32 {
    const u16 sd = L.side[side];
    if (sd == 0xFFFF || sd >= map.sidedefs.size()) return -1;
    const u16 sn = map.sidedefs[sd].sector;
    return sn < nsec ? static_cast<i32>(sn) : -1;
  };
  std::vector<std::vector<i32>> neighbours(nsec), lines_of(nsec);
  for (size_t li = 0; li < map.linedefs.size(); ++li) {
    const Linedef& L = map.linedefs[li];
    const i32 a = side_sector(L, 0), b = side_sector(L, 1);
    if (a >= 0) lines_of[size_t(a)].push_back(static_cast<i32>(li));
    if (b >= 0 && b != a) lines_of[size_t(b)].push_back(static_cast<i32>(li));
    if (a < 0 || b < 0 || a == b) continue;
    neighbours[size_t(a)].push_back(b);
    neighbours[size_t(b)].push_back(a);
  }
  auto F = [&](i32 s) { return static_cast<i32>(map.sectors[size_t(s)].floor); };
  auto C = [&](i32 s) { return static_cast<i32>(map.sectors[size_t(s)].ceiling); };
  constexpr i32 kNone = INT_MIN;
  auto target_of = [&](i32 s, PlaneTarget t) -> i32 {
    const std::vector<i32>& nb = neighbours[size_t(s)];
    switch (t) {
      case PlaneTarget::DoorOpen: {  // (a door already open higher stays there)
        if (nb.empty()) return kNone;
        i32 v = INT_MAX;
        for (i32 n : nb) v = std::min(v, C(n));
        return std::max(v - 4, C(s));
      }
      case PlaneTarget::OwnFloor: return F(s);
      case PlaneTarget::OwnFloorPlus8: return F(s) + 8;
      case PlaneTarget::HighestCeiling: {
        if (nb.empty()) return kNone;
        i32 v = INT_MIN;
        for (i32 n : nb) v = std::max(v, C(n));
        return v;
      }
      case PlaneTarget::LowestFloor: {
        i32 v = F(s);
        for (i32 n : nb) v = std::min(v, F(n));
        return v;
      }
      case PlaneTarget::HighestFloor:
      case PlaneTarget::TurboFloor:
      case PlaneTarget::HighestFloorOrOwn: {
        if (nb.empty()) return t == PlaneTarget::HighestFloorOrOwn ? F(s) : kNone;
        i32 v = INT_MIN;
        for (i32 n : nb) v = std::max(v, F(n));
        if (t == PlaneTarget::TurboFloor && v != F(s)) v += 8;
        if (t == PlaneTarget::HighestFloorOrOwn) v = std::max(v, F(s));
        return v;
      }
      case PlaneTarget::LowestCeiling:
      case PlaneTarget::LowestCeilingMinus8: {
        i32 v = C(s);
        for (i32 n : nb) v = std::min(v, C(n));
        return t == PlaneTarget::LowestCeiling ? v : v - 8;
      }
      case PlaneTarget::NextFloor: {
        i32 v = INT_MAX;
        for (i32 n : nb)
          if (F(n) > F(s)) v = std::min(v, F(n));
        return v == INT_MAX ? F(s) : v;
      }
      case PlaneTarget::Plus24: return F(s) + 24;
      case PlaneTarget::Plus32: return F(s) + 32;
      case PlaneTarget::Plus512: return F(s) + 512;
      default: return kNone;  // Start; stairs and the donut are built below
    }
  };
  struct Plan {
    std::vector<PlaneMove> moves;  // heights in map units until converted (back: kNone = start)
    bool door = false, lift = false, manual = false;
    i32 order = INT_MAX;  // first touch (manual doors first)
  };
  std::vector<Plan> fplan(nsec), cplan(nsec);
  i32 touches = 0;
  auto add = [&](i32 s, const SpecialAction& a, u16 special, u16 tag, i32 target, i32 back) {
    if (target == kNone) return;
    Plan& p = a.ceiling ? cplan[size_t(s)] : fplan[size_t(s)];
    for (const PlaneMove& m : p.moves)
      if (m.special == special && m.tag == tag) return;
    p.moves.push_back({special, tag, target, back});
    p.door = p.door || a.door;
    p.lift = p.lift || a.lift;
    if (p.order == INT_MAX) p.order = touches++;
  };
  // a staircase: the tagged sector rises one step, then through two-sided lines it fronts, the
  // back sectors with the same floor flat, each one step higher (vanilla EV_BuildStairs)
  auto stairs = [&](i32 s0, const SpecialAction& a, u16 special, u16 tag) {
    const i32 size = a.target == PlaneTarget::Stairs8 ? 8 : 16;
    const auto& flat = map.sectors[size_t(s0)].floor_pic;
    std::vector<char> in_chain(nsec, 0);
    in_chain[size_t(s0)] = 1;
    i32 sec = s0, height = F(s0) + size;
    add(s0, a, special, tag, height, kNone);
    for (bool ok = true; ok;) {
      ok = false;
      for (i32 li : lines_of[size_t(sec)]) {
        const Linedef& L = map.linedefs[size_t(li)];
        if (!(L.flags & 4) || side_sector(L, 0) != sec) continue;  // two-sided, fronting it
        const i32 t = side_sector(L, 1);
        if (t < 0 || map.sectors[size_t(t)].floor_pic != flat) continue;
        height += size;
        if (in_chain[size_t(t)]) continue;
        in_chain[size_t(t)] = 1;
        sec = t;
        add(t, a, special, tag, height, kNone);
        ok = true;
        break;
      }
    }
  };
  // the donut: the tagged hole and the ring around it go to the floor outside the ring
  auto donut = [&](i32 s1, const SpecialAction& a, u16 special, u16 tag) {
    if (lines_of[size_t(s1)].empty()) return;
    const Linedef& L0 = map.linedefs[size_t(lines_of[size_t(s1)][0])];
    if (!(L0.flags & 4)) return;
    const i32 s2 = side_sector(L0, 0) == s1 ? side_sector(L0, 1) : side_sector(L0, 0);
    if (s2 < 0) return;
    for (i32 li : lines_of[size_t(s2)]) {
      const i32 s3 = side_sector(map.linedefs[size_t(li)], 1);
      if (s3 < 0 || s3 == s1) continue;
      add(s2, a, special, tag, F(s3), kNone);
      add(s1, a, special, tag, F(s3), kNone);
      break;
    }
  };
  if (opt.movers)
    for (int pass = 0; pass < 2; ++pass) {  // manual doors first: their move is moves[0]
      for (size_t li = 0; li < map.linedefs.size(); ++li) {
        const Linedef& L = map.linedefs[li];
        SpecialInfo si;
        if (!special_info(L.special, &si)) continue;
        const bool manual = si.trigger == LineTrigger::Manual;
        if (manual != (pass == 0)) continue;
        std::vector<i32> secs;
        if (manual) {
          const i32 b = side_sector(L, 1);
          if (b >= 0) secs.push_back(b);
        } else if (L.tag != 0) {
          for (size_t sn = 0; sn < nsec; ++sn)
            if (map.sectors[sn].tag == L.tag) secs.push_back(static_cast<i32>(sn));
        }
        const u16 tag = manual ? 0 : L.tag;
        for (int k = 0; k < si.n; ++k) {
          const SpecialAction& a = si.a[k];
          for (i32 sn : secs) {
            if (a.target == PlaneTarget::Stairs8 || a.target == PlaneTarget::Stairs16) stairs(sn, a, L.special, tag);
            else if (a.target == PlaneTarget::Donut) donut(sn, a, L.special, tag);
            else if (a.kind == MoveKind::Stop) add(sn, a, L.special, tag, a.ceiling ? C(sn) : F(sn), kNone);
            else {
              const i32 back = a.back == PlaneTarget::Start ? kNone : target_of(sn, a.back);
              if (a.back != PlaneTarget::Start && back == kNone) continue;
              add(sn, a, L.special, tag, target_of(sn, a.target), back);
            }
          }
        }
      }
      if (pass == 0)
        for (Plan& p : cplan) p.manual = !p.moves.empty();
    }
  // envelopes: floors stay under the sector's ceiling, ceilings above the floor's envelope
  std::vector<i32> ceil_override(nsec, INT_MIN), floor_override(nsec, INT_MAX), floor_hi(nsec, INT_MIN);
  std::vector<std::pair<i32, MoverInfo>> planes;  // (order, plane)
  for (int pass = 0; pass < 2; ++pass)
    for (size_t sn = 0; sn < nsec; ++sn) {
      const i32 s = static_cast<i32>(sn);
      const bool ceiling = pass == 1;
      Plan& p = ceiling ? cplan[sn] : fplan[sn];
      if (p.moves.empty()) continue;
      const i32 start = ceiling ? C(s) : F(s);
      const i32 floor_top = floor_hi[sn] != INT_MIN ? floor_hi[sn] : F(s);
      i32 lo = start, hi = start;
      for (PlaneMove& m : p.moves) {
        if (ceiling) {
          m.target = std::max(m.target, floor_top);
          if (m.back != kNone) m.back = std::max(m.back, floor_top);
        } else {
          m.target = std::min(m.target, C(s));
          if (m.back != kNone) m.back = std::min(m.back, C(s));
        }
        lo = std::min(lo, m.target);
        hi = std::max(hi, m.target);
        if (m.back != kNone) {
          lo = std::min(lo, m.back);
          hi = std::max(hi, m.back);
        }
      }
      MoverInfo mi;
      mi.sector = s;
      mi.z0 = round_div(lo, upv);
      mi.z1 = round_div(hi, upv);
      if (mi.z1 <= mi.z0) continue;
      auto rows = [&](i32 height) { return ceiling ? mi.z1 - round_div(height, upv) : round_div(height, upv) - mi.z0; };
      mi.kind = ceiling ? (p.door ? MoverInfo::Kind::Door : MoverInfo::Kind::Ceiling)
                        : (p.lift ? MoverInfo::Kind::Lift : MoverInfo::Kind::Floor);
      mi.rows = rows(start);
      mi.special = p.moves[0].special;
      mi.tag = map.sectors[sn].tag;
      mi.manual = ceiling && p.manual;
      for (const PlaneMove& m : p.moves)
        mi.moves.push_back({m.special, m.tag, rows(m.target), m.back == kNone ? -1 : rows(m.back)});
      if (ceiling) {
        ceil_override[sn] = hi;
      } else {
        floor_override[sn] = lo;
        floor_hi[sn] = hi;
      }
      planes.emplace_back(p.order, std::move(mi));
    }
  std::stable_sort(planes.begin(), planes.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
  std::vector<MoverInfo> mv;
  for (auto& pl : planes) mv.push_back(std::move(pl.second));

  // ---- per-column heights (voxel units) -------------------------------------------
  const i32 BIG = 1 << 28;
  std::vector<i32> zf(ncol, BIG), zc(ncol, -BIG), sky(ncol, 0);
  i32 zlow = BIG, zhigh = -BIG;
  for (size_t c = 0; c < ncol; ++c) {
    const i32 s = sec[c];
    if (s < 0) continue;
    const Sector& S = map.sectors[s];
    zf[c] = round_div(floor_override[s] != INT_MAX ? floor_override[s] : S.floor, upv);
    zc[c] = std::max(zf[c] + 1, round_div(ceil_override[s] != INT_MIN ? ceil_override[s] : S.ceiling, upv));
    sky[c] = S.sky() ? 1 : 0;
  }
  // Ceiling slab top, extended where a neighbour's ceiling is higher (upper walls).
  std::vector<i32> ctop(ncol, -BIG);
  for (i32 j = 0; j < ny; ++j)
    for (i32 i = 0; i < nx; ++i) {
      const size_t c = size_t(j) * nx + i;
      if (sec[c] < 0) continue;
      i32 top = sky[c] ? zc[c] : zc[c] + opt.ceil_slab;
      const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
      if (!sky[c]) {
        for (int d = 0; d < 4; ++d) {
          const i32 ii = i + di[d], jj = j + dj[d];
          if (ii < 0 || jj < 0 || ii >= nx || jj >= ny) continue;
          const size_t n = size_t(jj) * nx + ii;
          if (sec[n] < 0 || zc[n] <= zc[c]) continue;
          top = std::max(top, sky[n] ? zc[n] : zc[n] + opt.ceil_slab);
        }
      }
      ctop[c] = top;
      zlow = std::min(zlow, zf[c] - opt.floor_slab);
      zhigh = std::max(zhigh, top);
    }
  if (zlow == BIG) {
    if (err) *err = "no sector columns";
    return false;
  }
  const i32 zmin = zlow - 1;
  const i32 zmax = zhigh + 1;

  // ---- shell: Chebyshev distance of void columns to playable space ----------------
  std::vector<i32> dist(ncol, INT_MAX);
  std::deque<size_t> q;
  for (size_t c = 0; c < ncol; ++c)
    if (sec[c] >= 0) {
      dist[c] = 0;
      q.push_back(c);
    }
  while (!q.empty()) {
    const size_t c = q.front();
    q.pop_front();
    const i32 i = static_cast<i32>(c % nx), j = static_cast<i32>(c / nx);
    if (dist[c] >= T) continue;
    for (int dj = -1; dj <= 1; ++dj)
      for (int di = -1; di <= 1; ++di) {
        if (!di && !dj) continue;
        const i32 ii = i + di, jj = j + dj;
        if (ii < 0 || jj < 0 || ii >= nx || jj >= ny) continue;
        const size_t n = size_t(jj) * nx + ii;
        if (dist[n] != INT_MAX) continue;
        dist[n] = dist[c] + 1;
        q.push_back(n);
      }
  }
  // Wall top / anchor level of shell columns from nearby playable columns.
  std::vector<i32> wall_top(ncol, INT_MIN), anchor_lvl(ncol, INT_MAX);
  for (size_t c = 0; c < ncol; ++c)
    if (sec[c] >= 0) {
      wall_top[c] = ctop[c];
      anchor_lvl[c] = zf[c] - opt.floor_slab;
    }
  max_filter(wall_top, nx, ny, T);
  min_filter(anchor_lvl, nx, ny, T);

  // ---- emit runs -----------------------------------------------------------------------
  ColumnGrid g;
  g.nx = nx;
  g.ny = ny;
  g.zmin = zmin;
  g.zmax = zmax;
  g.col_start.assign(ncol + 1, 0);
  VoxelizeStats st;
  const MaterialId mat = opt.material;
  auto push = [&](i32 z0, i32 z1, RunKind k) {
    if (z1 <= z0) return;
    g.runs.push_back({z0, z1, k, k == RunKind::Anchored ? MaterialId::Rock : mat});
  };
  for (size_t c = 0; c < ncol; ++c) {
    g.col_start[c] = static_cast<u32>(g.runs.size());
    if (sec[c] >= 0) {
      ++st.sector_columns;
      const i32 fs = zf[c] - opt.floor_slab;
      push(zmin, fs, RunKind::Anchored);
      push(fs, zf[c], RunKind::Structural);
      if (!sky[c]) push(zc[c], ctop[c], RunKind::Structural);
    } else if (dist[c] <= T) {
      ++st.shell_columns;
      const i32 za = std::max(zmin, anchor_lvl[c]);
      const i32 zt = std::min(zmax, wall_top[c]);
      // Shell columns stop at the wall top: air above the roofline in both void modes.
      push(zmin, za, RunKind::Anchored);
      push(za, zt, RunKind::Structural);
    } else {
      ++st.rock_columns;
      if (opt.void_mode == VoidMode::Rock) push(zmin, zmax, RunKind::Anchored);
    }
  }
  g.col_start[ncol] = static_cast<u32>(g.runs.size());
  st.nx = nx;
  st.ny = ny;
  st.nz = zmax - zmin;
  st.diagonal_fixes = fixes;
  st.structural_voxels = g.count(RunKind::Structural);
  st.anchored_voxels = g.count(RunKind::Anchored);
  st.map_min_x = minx;
  st.map_min_y = miny;
  st.map_max_x = maxx;
  st.map_max_y = maxy;
  st.origin_x = ox;
  st.origin_y = oy;
  st.units_per_voxel = upv;
  if (sector_map) *sector_map = sec;
  if (movers) *movers = std::move(mv);
  *out = std::move(g);
  if (stats) *stats = st;
  return true;
}

}  // namespace svx::doom
