#include "svx/mesh/mesher.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

#include "svx/base/parallel.hpp"

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
  // Solidity as bit columns along z (bit z + 1 for z in [-1, 32]): a direction's visible faces -
  // solid, with air in front - are found with word operations, and only they are looked at.
  static thread_local u64 occ[B * B];
  for (int c = 0; c < B * B; ++c) {
    const Vox* colv = &loc.v[size_t(c) * B];
    u64 bits = 0;
    for (int z = 0; z < B; ++z) bits |= u64(vox_solid(colv[z]) ? 1 : 0) << z;
    occ[c] = bits;
  }
  auto column = [&](int x, int y) { return occ[(x + 1) * B + (y + 1)]; };
  constexpr u64 kInner = ((u64{1} << kChunk) - 1) << 1;  // (z in [0, 32))
  static thread_local u64 vis[kChunk * kChunk];            // (x * 32 + y: the direction's faces along z)
  for (int face = 0; face < 6; ++face) {
    const int a = face >> 1;
    const int s = (face & 1) ? 1 : -1;
    const int t1 = (a + 1) % 3, t2 = (a + 2) % 3;
    int per_slice[kChunk] = {};
    for (int x = 0; x < kChunk; ++x)
      for (int y = 0; y < kChunk; ++y) {
        const u64 c = column(x, y);
        const u64 front = a == 0 ? column(x + s, y) : a == 1 ? column(x, y + s) : (s > 0 ? c >> 1 : c << 1);
        const u64 f = c & ~front & kInner;
        vis[x * kChunk + y] = f;
        if (!f) continue;
        if (a == 0) {
          per_slice[x] += std::popcount(f);
        } else if (a == 1) {
          per_slice[y] += std::popcount(f);
        } else {
          for (u64 b = f; b; b &= b - 1) ++per_slice[std::countr_zero(b) - 1];
        }
      }
    // per slice mask of faces
    std::vector<FaceKey> mask(kChunk * kChunk);
    std::vector<u8> has(kChunk * kChunk);
    for (int d = 0; d < kChunk; ++d) {
      if (per_slice[d] == 0) continue;
      std::fill(has.begin(), has.end(), 0);
      // (a face at (i, j) of the slice: its voxel p, the air in front of it q)
      auto look = [&](int i, int j) {
        int p[3];
        p[a] = d;
        p[t1] = i;
        p[t2] = j;
        int q[3] = {p[0], p[1], p[2]};
        q[a] += s;
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
          const int occ3 = (o1 && o2) ? 3 : (o1 + o2 + oc);
          ao |= static_cast<u8>((3 - occ3) << (2 * c));
        }
        k.ao = ao;
        mask[i * kChunk + j] = k;
        has[i * kChunk + j] = 1;
      };
      // (t1, t2: y, z for x faces; z, x for y faces; x, y for z faces)
      if (a == 0) {
        for (int y = 0; y < kChunk; ++y)
          for (u64 b = vis[d * kChunk + y]; b; b &= b - 1) look(y, std::countr_zero(b) - 1);
      } else if (a == 1) {
        for (int x = 0; x < kChunk; ++x)
          for (u64 b = vis[x * kChunk + d]; b; b &= b - 1) look(std::countr_zero(b) - 1, x);
      } else {
        const u64 bit = u64{1} << (d + 1);
        for (int x = 0; x < kChunk; ++x)
          for (int y = 0; y < kChunk; ++y)
            if (vis[x * kChunk + y] & bit) look(x, y);
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

namespace svx {

ChunkMesh mesh_shape(const BodyShape& S, f64 h, const MeshOptions& opt) {
  VoxelGrid piece;
  piece.h = h;
  std::vector<u64> keys;
  for (i32 i = 0; i < static_cast<i32>(S.vox.size()); ++i) {
    if (!vox_solid(S.vox[size_t(i)])) continue;
    const IVec3 p = S.voxel(i);
    piece.set(p, S.vox[size_t(i)]);
    keys.push_back(key3(p[0] >> kChunkBits, p[1] >> kChunkBits, p[2] >> kChunkBits));
  }
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  // (its chunks at once when the providers allow: a car crumpling is meshed again every tick)
  std::vector<ChunkMesh> parts(keys.size());
  auto part = [&](i64 a, i64 b) {
    for (i64 k = a; k < b; ++k) parts[size_t(k)] = mesh_chunk(piece, unkey3(keys[size_t(k)]), opt, false);
  };
  if (opt.concurrent && keys.size() > 1)
    parallel_for(static_cast<i64>(keys.size()), 1, part);
  else
    part(0, static_cast<i64>(keys.size()));
  ChunkMesh out;
  for (const ChunkMesh& m : parts) {
    const u32 base = static_cast<u32>(out.vertices.size());
    out.vertices.insert(out.vertices.end(), m.vertices.begin(), m.vertices.end());
    for (u32 i : m.indices) out.indices.push_back(base + i);
  }
  return out;
}

ChunkMesh mesh_coarse(const std::vector<Vox>& occ, const IVec3& nn, const IVec3& lo, i32 f, f64 h) {
  ChunkMesh m;
  const i32 n[3] = {nn[0], nn[1], nn[2]};
  if (n[0] <= 0 || n[1] <= 0 || n[2] <= 0 || occ.size() != size_t(n[0]) * size_t(n[1]) * size_t(n[2])) return m;
  auto at = [&](i32 x, i32 y, i32 z) -> Vox {
    if (z >= n[2]) return kAir;
    if (x < 0 || y < 0 || z < 0 || x >= n[0] || y >= n[1]) return make_vox(MaterialId::Rock, true);
    return occ[(size_t(x) * n[1] + y) * n[2] + z];
  };
  std::vector<i32> mask;
  for (int face = 0; face < 6; ++face) {
    const int a = face >> 1, s = (face & 1) ? 1 : -1;
    const int b = (a + 1) % 3, c = (a + 2) % 3;
    const i32 nb = n[b], nc = n[c];
    mask.assign(size_t(nb) * nc, 0);
    for (i32 k = 0; k < n[a]; ++k) {
      for (i32 u = 0; u < nb; ++u)
        for (i32 w = 0; w < nc; ++w) {
          i32 p[3];
          p[a] = k;
          p[b] = u;
          p[c] = w;
          const Vox v = at(p[0], p[1], p[2]);
          p[a] += s;
          mask[size_t(u) * nc + w] = vox_solid(v) && !vox_solid(at(p[0], p[1], p[2])) ? 1 + static_cast<i32>(vox_mat(v)) : 0;
        }
      for (i32 u = 0; u < nb; ++u)
        for (i32 w = 0; w < nc;) {
          const i32 id = mask[size_t(u) * nc + w];
          if (!id) {
            ++w;
            continue;
          }
          i32 ww = 1;
          while (w + ww < nc && mask[size_t(u) * nc + w + ww] == id) ++ww;
          i32 hh = 1;
          for (bool grow = true; grow && u + hh < nb;) {
            for (i32 q = 0; q < ww; ++q)
              if (mask[size_t(u + hh) * nc + w + q] != id) {
                grow = false;
                break;
              }
            if (grow) ++hh;
          }
          for (i32 du = 0; du < hh; ++du)
            for (i32 q = 0; q < ww; ++q) mask[size_t(u + du) * nc + w + q] = 0;
          f64 base[3];
          base[a] = h * (lo[a] + (k + (s > 0 ? 1 : 0)) * f - 0.5);
          base[b] = h * (lo[b] + u * f - 0.5);
          base[c] = h * (lo[c] + w * f - 0.5);
          const f64 eb = h * hh * f, ec = h * ww * f;
          const u32 v0 = static_cast<u32>(m.vertices.size());
          const f64 cr[4][2] = {{0, 0}, {eb, 0}, {eb, ec}, {0, ec}};
          for (const auto& q : cr) {
            MeshVertex mv{};
            f64 pp[3] = {base[0], base[1], base[2]};
            pp[b] += q[0];
            pp[c] += q[1];
            for (int kq = 0; kq < 3; ++kq) mv.pos[kq] = static_cast<f32>(pp[kq]);
            mv.normal[a] = static_cast<i8>(127 * s);
            mv.normal[3] = 127;
            mv.uv[0] = static_cast<f32>(pp[b] * 32.0);
            mv.uv[1] = static_cast<f32>(pp[c] * 32.0);
            mv.texture = static_cast<u16>(0xFF00 + (id - 1));
            mv.light = 255;
            m.vertices.push_back(mv);
          }
          if (s > 0) {
            for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(v0 + i);
          } else {
            for (u32 i : {0u, 2u, 1u, 0u, 3u, 2u}) m.indices.push_back(v0 + i);
          }
          w += ww;
        }
    }
  }
  return m;
}

}  // namespace svx
