#include "svx/doom/world.hpp"

#include <algorithm>
#include <cmath>

#include "svx/base/dmath.hpp"

namespace svx::doom {

namespace {

inline bool segments_cross(f64 ax, f64 ay, f64 bx, f64 by, f64 cx, f64 cy, f64 dx, f64 dy) {
  auto orient = [](f64 px, f64 py, f64 qx, f64 qy, f64 rx, f64 ry) {
    return (qx - px) * (ry - py) - (qy - py) * (rx - px);
  };
  const f64 o1 = orient(ax, ay, bx, by, cx, cy), o2 = orient(ax, ay, bx, by, dx, dy);
  const f64 o3 = orient(cx, cy, dx, dy, ax, ay), o4 = orient(cx, cy, dx, dy, bx, by);
  return ((o1 > 0) != (o2 > 0)) && ((o3 > 0) != (o4 > 0));
}

inline f64 seg_dist2(f64 px, f64 py, f64 ax, f64 ay, f64 bx, f64 by) {
  const f64 vx = bx - ax, vy = by - ay;
  const f64 l2 = vx * vx + vy * vy;
  f64 t = l2 > 0 ? ((px - ax) * vx + (py - ay) * vy) / l2 : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  const f64 qx = ax + t * vx - px, qy = ay + t * vy - py;
  return qx * qx + qy * qy;
}

std::string pic(const std::array<char, 9>& a) { return std::string(a.data()); }

}  // namespace

u8 DoomWorld::face_light(const IVec3& p, int face) const {
  const int a = face >> 1, s = (face & 1) ? 1 : -1;
  IVec3 q = p;
  q[a] += s;
  if (q[0] < 0 || q[1] < 0 || q[0] >= nx || q[1] >= ny) return 160;
  const i32 sec = sector[size_t(q[1]) * nx + q[0]];
  if (sec < 0) {
    const i32 own = (p[0] >= 0 && p[1] >= 0 && p[0] < nx && p[1] < ny) ? sector[size_t(p[1]) * nx + p[0]] : -1;
    return own >= 0 ? light[own] : 160;
  }
  return light[sec];
}

i32 DoomWorld::face_linedef(const IVec3& p, int face) const {
  const int a = face >> 1, s = (face & 1) ? 1 : -1;
  if (a == 2) return -1;
  IVec3 q = p;
  q[a] += s;
  if (p[0] < 0 || p[1] < 0 || p[0] >= nx || p[1] >= ny || q[0] < 0 || q[1] < 0 || q[0] >= nx || q[1] >= ny) return -1;
  const i32 qs = sector[size_t(q[1]) * nx + q[0]];
  if (qs < 0) return -1;
  const i32 upv = vstats.units_per_voxel;
  const f64 qx = vstats.origin_x + (q[0] + 0.5) * upv, qy = vstats.origin_y + (q[1] + 0.5) * upv;
  const f64 px = vstats.origin_x + (p[0] + 0.5) * upv, py = vstats.origin_y + (p[1] + 0.5) * upv;
  i32 best = -1;
  f64 bd = INFINITY;
  for (i32 li : sector_lines[qs]) {
    const Linedef& L = map.linedefs[li];
    const Vertex& A = map.vertices[L.v1];
    const Vertex& B = map.vertices[L.v2];
    if (segments_cross(qx, qy, px, py, A.x, A.y, B.x, B.y)) return li;
    const f64 d = seg_dist2(0.5 * (qx + px), 0.5 * (qy + py), A.x, A.y, B.x, B.y);
    if (d < bd) {
      bd = d;
      best = li;
    }
  }
  return best;
}

u16 DoomWorld::face_texture(const IVec3& p, int face) const {
  const int a = face >> 1, s = (face & 1) ? 1 : -1;
  const Vox v = (live ? *live : grid).get(p);
  const u16 fallback = static_cast<u16>(0xFF00 + static_cast<u16>(vox_mat(v)));
  if (p[0] < 0 || p[1] < 0 || p[0] >= nx || p[1] >= ny) return fallback;
  const i32 own = sector[size_t(p[1]) * nx + p[0]];
  if (a == 2) {
    // floors (top faces) / ceilings (bottom faces) of a sector column
    if (own < 0) return fallback;
    const u16 t = s > 0 ? floor_tex[own] : ceil_tex[own];
    return t == 0xFFFF ? fallback : t;
  }
  IVec3 q = p;
  q[a] += s;
  if (q[0] < 0 || q[1] < 0 || q[0] >= nx || q[1] >= ny) return fallback;
  const i32 qs = sector[size_t(q[1]) * nx + q[0]];
  if (qs < 0) return fallback;
  // the linedef between column q (sector qs, air side) and column p
  const u64 key = (u64(u32(q[1] * nx + q[0])) << 3) | u64(face);
  auto it = wall_cache.find(key);
  if (it == wall_cache.end()) {
    std::array<u16, 3> tex{0xFFFF, 0xFFFF, 0xFFFF};
    const i32 upv = vstats.units_per_voxel;
    const f64 qx = vstats.origin_x + (q[0] + 0.5) * upv, qy = vstats.origin_y + (q[1] + 0.5) * upv;
    const f64 px = vstats.origin_x + (p[0] + 0.5) * upv, py = vstats.origin_y + (p[1] + 0.5) * upv;
    i32 best = -1;
    f64 bd = INFINITY;
    for (i32 li : sector_lines[qs]) {
      const Linedef& L = map.linedefs[li];
      const Vertex& A = map.vertices[L.v1];
      const Vertex& B = map.vertices[L.v2];
      if (segments_cross(qx, qy, px, py, A.x, A.y, B.x, B.y)) {
        best = li;
        break;
      }
      const f64 d = seg_dist2(0.5 * (qx + px), 0.5 * (qy + py), A.x, A.y, B.x, B.y);
      if (d < bd) {
        bd = d;
        best = li;
      }
    }
    if (best >= 0) {
      const Linedef& L = map.linedefs[best];
      // the sidedef facing sector qs
      i32 sd = -1;
      for (int side = 0; side < 2; ++side) {
        const u16 x = L.side[side];
        if (x != 0xFFFF && x < map.sidedefs.size() && map.sidedefs[x].sector == qs) sd = x;
      }
      if (sd < 0) sd = L.side[0] != 0xFFFF ? L.side[0] : -1;
      if (sd >= 0 && sd < static_cast<i32>(map.sidedefs.size())) {
        const Sidedef& S = map.sidedefs[sd];
        auto id = [&](const std::array<char, 9>& n) -> u16 {
          const i32 t = textures.find_wall(pic(n));
          return t < 0 ? 0xFFFF : static_cast<u16>(t);
        };
        tex = {id(S.top), id(S.mid), id(S.bottom)};
        // a one-sided wall only has a middle texture; use it everywhere
        if (tex[0] == 0xFFFF) tex[0] = tex[1];
        if (tex[2] == 0xFFFF) tex[2] = tex[1];
        if (tex[1] == 0xFFFF) tex[1] = tex[2] != 0xFFFF ? tex[2] : tex[0];
      }
    }
    it = wall_cache.emplace(key, tex).first;
  }
  // Which part of the wall: a solid voxel of another sector's column is either below that
  // sector's floor (a step: the lower texture) or above its ceiling (the upper texture);
  // void (shell) columns show the middle texture.
  const i32 upv = vstats.units_per_voxel;
  const f64 zmap = (p[2] + 0.5) * upv;
  u16 t = it->second[1];
  if (own >= 0) {
    const Sector& P = map.sectors[own];
    if (zmap < P.floor) t = it->second[2];
    else if (zmap >= P.ceiling) t = it->second[0];
  }
  return t == 0xFFFF ? fallback : t;
}

bool build_doom_world(const Wad& wad, const std::string& map_name, const VoxelizeOptions& vo, f64 h, bool with_textures,
                      DoomWorld* out, std::string* err) {
  DoomWorld& W = *out;
  if (!wad.read_map(map_name, &W.map, err)) return false;
  ColumnGrid cg;
  if (!voxelize(W.map, vo, &cg, &W.vstats, err, &W.sector, &W.movers)) return false;
  W.slenderness = wall_slenderness(cg, material(MaterialId::Concrete), h);
  W.nx = cg.nx;
  W.ny = cg.ny;
  VoxelGrid& g = W.grid;
  g.h = h;
  for (i32 j = 0; j < cg.ny; ++j)
    for (i32 i = 0; i < cg.nx; ++i) {
      const i32 c = cg.column(i, j);
      for (const Run* r = cg.begin(c); r != cg.end(c); ++r)
        g.fill_column(i, j, r->z0, r->z1, make_vox(r->mat, r->kind == RunKind::Anchored));
    }
  g.compact();
  g.lo = {0, 0, cg.zmin};
  g.hi = {cg.nx, cg.ny, cg.zmax};
  // textures and per-sector lookups
  const size_t ns = W.map.sectors.size();
  W.floor_tex.assign(ns, 0xFFFF);
  W.ceil_tex.assign(ns, 0xFFFF);
  W.light.assign(ns, 160);
  W.sector_lines.assign(ns, {});
  for (size_t li = 0; li < W.map.linedefs.size(); ++li) {
    const Linedef& L = W.map.linedefs[li];
    for (int side = 0; side < 2; ++side) {
      const u16 sd = L.side[side];
      if (sd == 0xFFFF || sd >= W.map.sidedefs.size()) continue;
      const u16 s = W.map.sidedefs[sd].sector;
      if (s < ns) W.sector_lines[s].push_back(static_cast<i32>(li));
    }
  }
  if (with_textures) {
    TextureOptions to;
    to.only = referenced_textures(W.map);
    TextureStats ts;
    std::string terr;
    if (!load_textures(wad, to, &W.textures, &ts, &terr)) W.textures = TextureSet{};
  }
  for (size_t s = 0; s < ns; ++s) {
    const Sector& S = W.map.sectors[s];
    const i32 f = W.textures.find_flat(pic(S.floor_pic));
    const i32 c = W.textures.find_flat(pic(S.ceil_pic));
    if (f >= 0) W.floor_tex[s] = static_cast<u16>(f);
    if (c >= 0) W.ceil_tex[s] = static_cast<u16>(c);
    W.light[s] = static_cast<u8>(std::clamp<int>(S.light, 0, 255));
  }
  // player 1 start (thing type 1), else the first sector column
  const i32 upv = W.vstats.units_per_voxel;
  bool found = false;
  for (const Thing& t : W.map.things)
    if (t.type == 1) {
      const f64 i = f64(t.x - W.vstats.origin_x) / upv - 0.5;
      const f64 j = f64(t.y - W.vstats.origin_y) / upv - 0.5;
      const i32 ci = std::clamp(static_cast<i32>(std::lround(i)), 0, W.nx - 1);
      const i32 cj = std::clamp(static_cast<i32>(std::lround(j)), 0, W.ny - 1);
      const i32 s = W.sector[size_t(cj) * W.nx + ci];
      const f64 floor_map = s >= 0 ? W.map.sectors[s].floor : 0.0;
      W.spawn_pos = {i * h, j * h, (std::round(floor_map / upv) - 0.5) * h + 0.02};
      const f64 ang = t.angle * M_PI / 180.0;
      W.spawn_dir = {dm::cos(ang), dm::sin(ang), 0.0};
      found = true;
      break;
    }
  if (!found) {
    for (i32 c = 0; c < W.nx * W.ny && !found; ++c)
      if (W.sector[c] >= 0) {
        const f64 floor_map = W.map.sectors[W.sector[c]].floor;
        W.spawn_pos = {(c % W.nx) * h, (c / W.nx) * h, (std::round(floor_map / upv) - 0.5) * h + 0.02};
        found = true;
      }
  }
  return true;
}

}  // namespace svx::doom
