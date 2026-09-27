#include "svx/mesh/mesher.hpp"

#include <algorithm>
#include <cmath>

namespace svx {

namespace {

constexpr int B = kChunk + 2;  // chunk with a one-voxel border

struct Local {
  Vox v[B * B * B];
  Vox at(int x, int y, int z) const { return v[((x + 1) * B + (y + 1)) * B + (z + 1)]; }
};

inline i8 snorm(f64 v) { return static_cast<i8>(std::lround(std::clamp(v, -1.0, 1.0) * 127.0)); }

struct FaceKey {
  u16 tex;
  u8 light, debug;
  u8 ao;  // 4 corners x 2 bits
  bool operator==(const FaceKey& o) const {
    return tex == o.tex && light == o.light && debug == o.debug && ao == o.ao;
  }
};

}  // namespace

ChunkMesh mesh_chunk(const VoxelGrid& g, const IVec3& cc, const MeshOptions& opt, bool displaced) {
  ChunkMesh out;
  out.chunk = cc;
  const IVec3 base{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
  static thread_local Local loc;
  const Chunk* ch = g.chunk(cc);
  if (!ch || (ch->uniform && !vox_solid(ch->value))) return out;
  // copy the chunk and its one-voxel border straight from the chunk arrays (27 chunk lookups
  // instead of one hash lookup per voxel)
  for (int dx = -1; dx <= 1; ++dx)
    for (int dy = -1; dy <= 1; ++dy)
      for (int dz = -1; dz <= 1; ++dz) {
        const Chunk* nc = g.chunk({cc[0] + dx, cc[1] + dy, cc[2] + dz});
        const int x0 = dx < 0 ? -1 : (dx > 0 ? kChunk : 0), x1 = dx < 0 ? 0 : (dx > 0 ? kChunk + 1 : kChunk);
        const int y0 = dy < 0 ? -1 : (dy > 0 ? kChunk : 0), y1 = dy < 0 ? 0 : (dy > 0 ? kChunk + 1 : kChunk);
        const int z0 = dz < 0 ? -1 : (dz > 0 ? kChunk : 0), z1 = dz < 0 ? 0 : (dz > 0 ? kChunk + 1 : kChunk);
        for (int x = x0; x < x1; ++x)
          for (int y = y0; y < y1; ++y) {
            Vox* dst = &loc.v[((x + 1) * B + (y + 1)) * B + (z0 + 1)];
            if (!nc) {
              for (int z = z0; z < z1; ++z) *dst++ = kAir;
            } else if (nc->uniform) {
              for (int z = z0; z < z1; ++z) *dst++ = nc->value;
            } else {
              const int lx = x & (kChunk - 1), ly = y & (kChunk - 1);
              const Vox* src = &nc->v[(lx * kChunk + ly) * kChunk];
              for (int z = z0; z < z1; ++z) *dst++ = src[z & (kChunk - 1)];
            }
          }
      }
  const f64 h = g.h;
  const f64 tpm = opt.texels_per_metre;
  auto solid = [&](int x, int y, int z) { return vox_solid(loc.at(x, y, z)); };
  // corner displacement (average over the solid voxels sharing the corner)
  auto corner_disp = [&](int cx, int cy, int cz, f32 d[3]) {
    // corner at voxel-space (cx - 0.5, cy - 0.5, cz - 0.5) relative to the chunk base
    d[0] = d[1] = d[2] = 0.0f;
    int cnt = 0;
    for (int dx = -1; dx <= 0; ++dx)
      for (int dy = -1; dy <= 0; ++dy)
        for (int dz = -1; dz <= 0; ++dz) {
          const int x = cx + dx, y = cy + dy, z = cz + dz;
          if (x < -1 || y < -1 || z < -1 || x > kChunk || y > kChunk || z > kChunk) continue;
          if (!solid(x, y, z)) continue;
          ++cnt;
          f32 v[3];
          if (opt.displacement && opt.displacement({base[0] + x, base[1] + y, base[2] + z}, v))
            for (int q = 0; q < 3; ++q) d[q] += v[q];
        }
    if (cnt > 0)
      for (int q = 0; q < 3; ++q) d[q] /= f32(cnt);
  };
  for (int face = 0; face < 6; ++face) {
    const int a = face >> 1;
    const int s = (face & 1) ? 1 : -1;
    const int t1 = (a + 1) % 3, t2 = (a + 2) % 3;
    // per slice mask of faces
    std::vector<FaceKey> mask(kChunk * kChunk);
    std::vector<u8> has(kChunk * kChunk);
    for (int d = 0; d < kChunk; ++d) {
      std::fill(has.begin(), has.end(), 0);
      for (int i = 0; i < kChunk; ++i)
        for (int j = 0; j < kChunk; ++j) {
          int p[3];
          p[a] = d;
          p[t1] = i;
          p[t2] = j;
          if (!solid(p[0], p[1], p[2])) continue;
          int q[3] = {p[0], p[1], p[2]};
          q[a] += s;
          if (solid(q[0], q[1], q[2])) continue;
          const IVec3 wp{base[0] + p[0], base[1] + p[1], base[2] + p[2]};
          FaceKey k;
          k.tex = opt.texture ? opt.texture(wp, face) : static_cast<u16>(0xFF00 + static_cast<u16>(vox_mat(loc.at(p[0], p[1], p[2]))));
          k.light = opt.light ? opt.light(wp, face) : 255;
          k.debug = opt.debug ? opt.debug(wp) : 0;
          // AO: the air layer in front of the face (q), neighbours along t1 / t2
          u8 ao = 0;
          for (int c = 0; c < 4; ++c) {
            const int du = (c == 1 || c == 2) ? 1 : -1;
            const int dv = (c >= 2) ? 1 : -1;
            int s1[3] = {q[0], q[1], q[2]}, s2[3] = {q[0], q[1], q[2]}, sc[3] = {q[0], q[1], q[2]};
            s1[t1] += du;
            s2[t2] += dv;
            sc[t1] += du;
            sc[t2] += dv;
            const int o1 = solid(s1[0], s1[1], s1[2]), o2 = solid(s2[0], s2[1], s2[2]), oc = solid(sc[0], sc[1], sc[2]);
            const int occ = (o1 && o2) ? 3 : (o1 + o2 + oc);
            ao |= static_cast<u8>((3 - occ) << (2 * c));
          }
          k.ao = ao;
          mask[i * kChunk + j] = k;
          has[i * kChunk + j] = 1;
        }
      // emit quads
      for (int i = 0; i < kChunk; ++i)
        for (int j = 0; j < kChunk; ++j) {
          if (!has[i * kChunk + j]) continue;
          const FaceKey k = mask[i * kChunk + j];
          int w = 1, hh = 1;
          if (!displaced) {
            while (j + hh < kChunk && has[i * kChunk + j + hh] && mask[i * kChunk + j + hh] == k) ++hh;
            bool grow = true;
            while (grow && i + w < kChunk) {
              for (int jj = j; jj < j + hh; ++jj)
                if (!has[(i + w) * kChunk + jj] || !(mask[(i + w) * kChunk + jj] == k)) {
                  grow = false;
                  break;
                }
              if (grow) ++w;
            }
          }
          for (int ii = i; ii < i + w; ++ii)
            for (int jj = j; jj < j + hh; ++jj) has[ii * kChunk + jj] = 0;
          // corners in (t1, t2) of the merged rectangle; the face plane at d + s/2
          const int u0 = i, u1 = i + w, v0 = j, v1 = j + hh;
          const int cu[4] = {u0, u1, u1, u0}, cv[4] = {v0, v0, v1, v1};
          const u32 first = static_cast<u32>(out.vertices.size());
          f64 aoc[4];
          for (int c = 0; c < 4; ++c) aoc[c] = f64((k.ao >> (2 * c)) & 3) / 3.0;
          for (int c = 0; c < 4; ++c) {
            // corner index in voxel units relative to the chunk base, measured at the
            // voxel's -0.5 boundary: plane coordinate along a
            int cidx[3];
            cidx[a] = d + (s > 0 ? 1 : 0);
            cidx[t1] = cu[c];
            cidx[t2] = cv[c];
            MeshVertex vx{};
            for (int q = 0; q < 3; ++q) vx.pos[q] = static_cast<f32>(h * (base[q] + cidx[q] - 0.5));
            if (displaced) {
              f32 dd[3];
              corner_disp(cidx[0], cidx[1], cidx[2], dd);
              for (int q = 0; q < 3; ++q) vx.pos[q] += dd[q];
            }
            vx.normal[0] = vx.normal[1] = vx.normal[2] = 0;
            vx.normal[a] = static_cast<i8>(s * 127);
            // corner AO: merged rectangles have equal AO patterns at every face, so the
            // rectangle corner takes the matching corner's value
            vx.normal[3] = snorm(2.0 * aoc[c] - 1.0);
            // texel coordinates: horizontal along the face, v downward on walls
            const f64 wx = h * (base[0] + cidx[0] - 0.5), wy = h * (base[1] + cidx[1] - 0.5), wz = h * (base[2] + cidx[2] - 0.5);
            if (a == 2) {
              vx.uv[0] = static_cast<f32>(wx * tpm);
              vx.uv[1] = static_cast<f32>(-wy * tpm);
            } else if (a == 0) {
              vx.uv[0] = static_cast<f32>((s > 0 ? wy : -wy) * tpm);
              vx.uv[1] = static_cast<f32>(-wz * tpm);
            } else {
              vx.uv[0] = static_cast<f32>((s > 0 ? -wx : wx) * tpm);
              vx.uv[1] = static_cast<f32>(-wz * tpm);
            }
            vx.texture = k.tex;
            vx.light = k.light;
            vx.debug = k.debug;
            out.vertices.push_back(vx);
          }
          // CCW seen from outside: corners (0,0),(1,0),(1,1),(0,1) in (t1, t2) are CCW
          // around +a; reverse for -a. Flip the diagonal to follow the AO gradient.
          const bool flip = aoc[0] + aoc[2] < aoc[1] + aoc[3];
          u32 idx[6];
          if (!flip) {
            const u32 t[6] = {0, 1, 2, 0, 2, 3};
            std::copy(t, t + 6, idx);
          } else {
            const u32 t[6] = {1, 2, 3, 1, 3, 0};
            std::copy(t, t + 6, idx);
          }
          if (s < 0) {
            std::swap(idx[1], idx[2]);
            std::swap(idx[4], idx[5]);
          }
          for (u32 t : idx) out.indices.push_back(first + t);
        }
    }
  }
  return out;
}

}  // namespace svx
