// structvox mesh — water surfaces (mesher.hpp).
#include <array>

#include "svx/mesh/mesher.hpp"

namespace svx {

namespace {

// A quad on the plane of axis a (at coordinate w, metres), facing s (+1 / -1), over the
// rectangle [u0, u1] x [v0, v1] of the axes t1 = a + 1, t2 = a + 2 (mod 3).
void quad(ChunkMesh& out, int a, int s, f64 w, f64 u0, f64 u1, f64 v0, f64 v1) {
  const int t1 = (a + 1) % 3, t2 = (a + 2) % 3;
  const u32 first = static_cast<u32>(out.vertices.size());
  const f64 cu[4] = {u0, u1, u1, u0}, cv[4] = {v0, v0, v1, v1};
  for (int c = 0; c < 4; ++c) {
    f64 p[3];
    p[a] = w;
    p[t1] = cu[c];
    p[t2] = cv[c];
    MeshVertex vx{};
    for (int q = 0; q < 3; ++q) vx.pos[q] = static_cast<f32>(p[q]);
    vx.normal[a] = static_cast<i8>(s * 127);
    vx.normal[3] = 127;
    vx.uv[0] = static_cast<f32>(p[0]);
    vx.uv[1] = static_cast<f32>(p[1]);
    vx.texture = kWaterTexture;
    vx.light = 255;
    out.vertices.push_back(vx);
  }
  u32 idx[6] = {0, 1, 2, 0, 2, 3};
  if (s < 0) {
    std::swap(idx[1], idx[2]);
    std::swap(idx[4], idx[5]);
  }
  for (u32 t : idx) out.indices.push_back(first + t);
}

}  // namespace

ChunkMesh mesh_water(const VoxelGrid& g, int L, const IVec3& cc) {
  ChunkMesh out;
  out.chunk = cc;
  const Chunk* c = g.chunk(cc);
  if (!c || L < 0 || L >= kMaxLayers || c->layer[size_t(L)].empty()) return out;
  const LayerValues& a = c->layer[size_t(L)];
  const f64 h = g.h;
  const IVec3 base{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
  // (inside the chunk: its arrays; outside: the grid)
  auto inside = [&](const IVec3& p) {
    return p[0] >= base[0] && p[0] < base[0] + kChunk && p[1] >= base[1] && p[1] < base[1] + kChunk && p[2] >= base[2] && p[2] < base[2] + kChunk;
  };
  auto water = [&](const IVec3& p) -> u8 { return inside(p) ? a[size_t(chunk_index(p))] : g.layer(L, p); };
  auto solid = [&](const IVec3& p) {
    if (!inside(p)) return vox_solid(g.get(p));
    return vox_solid(c->uniform ? c->value : c->v[size_t(chunk_index(p))]);
  };
  for (i32 z = 0; z < kChunk; ++z) {
    // tops, greedy: equal levels in rectangles
    std::array<u8, kChunk * kChunk> top{};
    for (i32 x = 0; x < kChunk; ++x)
      for (i32 y = 0; y < kChunk; ++y) {
        const u8 v = a[size_t((x * kChunk + y) * kChunk + z)];
        if (!v) continue;
        const IVec3 p{base[0] + x, base[1] + y, base[2] + z};
        const IVec3 up{p[0], p[1], p[2] + 1};
        if (!water(up) && !solid(up)) top[size_t(x * kChunk + y)] = v;
        // sides: where it stands higher than its neighbour (air or lower water)
        const f64 bottom = h * (p[2] - 0.5), level = bottom + h * (water(up) ? 1.0 : v / 255.0);
        constexpr int kSides[4][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
        for (const auto& d : kSides) {
          const IVec3 q{p[0] + d[0], p[1] + d[1], p[2]};
          if (solid(q)) continue;
          const u8 wq = water(q);
          const IVec3 qu{q[0], q[1], q[2] + 1};
          const f64 lq = bottom + h * (wq && water(qu) ? 1.0 : wq / 255.0);
          if (lq >= level - 1e-6) continue;
          const int ax = d[0] ? 0 : 1, sg = d[0] + d[1];
          const f64 w = h * (p[ax] + 0.5 * sg);
          const int other = ax == 0 ? 1 : 0;
          const f64 o0 = h * (p[other] - 0.5), o1 = h * (p[other] + 0.5);
          // (the plane's axes: t1 = ax + 1, t2 = ax + 2)
          if (ax == 0) quad(out, 0, sg, w, o0, o1, lq, level);  // t1 = y, t2 = z
          else quad(out, 1, sg, w, lq, level, o0, o1);           // t1 = z, t2 = x
        }
        // bottom: a falling sheet over air
        const IVec3 dn{p[0], p[1], p[2] - 1};
        if (!solid(dn) && !water(dn)) quad(out, 2, -1, bottom, h * (p[0] - 0.5), h * (p[0] + 0.5), h * (p[1] - 0.5), h * (p[1] + 0.5));
      }
    for (i32 x = 0; x < kChunk; ++x)
      for (i32 y = 0; y < kChunk;) {
        const u8 v = top[size_t(x * kChunk + y)];
        if (!v) {
          ++y;
          continue;
        }
        i32 y1 = y + 1;
        while (y1 < kChunk && top[size_t(x * kChunk + y1)] == v) ++y1;
        i32 x1 = x + 1;
        for (; x1 < kChunk; ++x1) {
          bool row = true;
          for (i32 yy = y; yy < y1 && row; ++yy) row = top[size_t(x1 * kChunk + yy)] == v;
          if (!row) break;
        }
        for (i32 xx = x; xx < x1; ++xx)
          for (i32 yy = y; yy < y1; ++yy) top[size_t(xx * kChunk + yy)] = 0;
        const f64 zt = h * (base[2] + z - 0.5) + h * v / 255.0;
        quad(out, 2, 1, zt, h * (base[0] + x - 0.5), h * (base[0] + x1 - 0.5), h * (base[1] + y - 0.5), h * (base[1] + y1 - 0.5));
        y = y1;
      }
  }
  return out;
}

}  // namespace svx
