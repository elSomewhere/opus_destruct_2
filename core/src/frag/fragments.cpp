#include "svx/frag/fragments.hpp"

#include <cstdlib>

#include <algorithm>
#include <cmath>

namespace svx {

namespace {

inline u64 mix64(u64 x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}
inline u64 hash4(i64 a, i64 b, i64 c, u64 d) {
  u64 h = mix64(static_cast<u64>(a) ^ (d * 0x632BE59BD9B4E019ull));
  h = mix64(h ^ static_cast<u64>(b));
  h = mix64(h ^ static_cast<u64>(c));
  return h;
}
inline f64 unit(u64 h) { return static_cast<f64>(h >> 11) * (1.0 / 9007199254740992.0); }  // [0, 1)

inline i64 floor_div(f64 v) { return static_cast<i64>(std::floor(v)); }

struct UF {
  std::vector<i32> p;
  explicit UF(i32 n) : p(static_cast<size_t>(n)) {
    for (i32 i = 0; i < n; ++i) p[static_cast<size_t>(i)] = i;
  }
  i32 find(i32 x) {
    while (p[static_cast<size_t>(x)] != x) {
      p[static_cast<size_t>(x)] = p[static_cast<size_t>(p[static_cast<size_t>(x)])];
      x = p[static_cast<size_t>(x)];
    }
    return x;
  }
};

}  // namespace

void accumulate_voxel(f64 m, const V3& c, f64 h, f64* s) {
  s[0] += m;
  s[1] += m * c.x;
  s[2] += m * c.y;
  s[3] += m * c.z;
  s[4] += m * c.x * c.x;
  s[5] += m * c.y * c.y;
  s[6] += m * c.z * c.z;
  s[7] += m * c.x * c.y;
  s[8] += m * c.x * c.z;
  s[9] += m * c.y * c.z;
  (void)h;
}

MassProps finish_mass(const f64* s, f64 h) {
  MassProps mp;
  mp.mass = s[0];
  if (!(s[0] > 0)) return mp;
  const f64 im = 1.0 / s[0];
  mp.com = {s[1] * im, s[2] * im, s[3] * im};
  // second moments about the centre of mass
  const f64 cxx = s[4] - s[0] * mp.com.x * mp.com.x;
  const f64 cyy = s[5] - s[0] * mp.com.y * mp.com.y;
  const f64 czz = s[6] - s[0] * mp.com.z * mp.com.z;
  const f64 cxy = s[7] - s[0] * mp.com.x * mp.com.y;
  const f64 cxz = s[8] - s[0] * mp.com.x * mp.com.z;
  const f64 cyz = s[9] - s[0] * mp.com.y * mp.com.z;
  const f64 own = s[0] * h * h / 6.0;  // each voxel's own cube inertia
  M3& I = mp.inertia;
  I(0, 0) = cyy + czz + own;
  I(1, 1) = cxx + czz + own;
  I(2, 2) = cxx + cyy + own;
  I(0, 1) = I(1, 0) = -cxy;
  I(0, 2) = I(2, 0) = -cxz;
  I(1, 2) = I(2, 1) = -cyz;
  return mp;
}

FragChunk fragment_chunk(const VoxelGrid& g, const IVec3& cc, const FragParams& par) {
  FragChunk out;
  const Chunk* ch = g.chunk(cc);
  if (!ch) return out;
  out.vox_version = ch->vox_version;
  if (ch->free_count() == 0) return out;
  const IVec3 base{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
  auto vox_at = [&](int i) -> Vox { return ch->uniform ? ch->value : ch->v[static_cast<size_t>(i)]; };
  auto broken_at = [&](int i) -> u8 { return ch->broken.empty() ? u8(0) : ch->broken[static_cast<size_t>(i)]; };
  constexpr int S = kChunk;

  // 1. Voronoi label per free voxel: (material, seed cell) packed into a u64. A reinforcement
  // voxel takes the label its host material (the first free non-reinforcement neighbour's, in
  // the chunk) has at its place: bars belong to the fragments of the concrete around them.
  std::vector<u64> label(kChunkVox, 0);
  constexpr int kStride[3] = {S * S, S, 1};
  auto host_of = [&](int i, int x, int y, int z) -> MaterialId {
    const MaterialId own = vox_mat(vox_at(i));
    if (!material(own).reinforcement) return own;
    const int c[3] = {x, y, z};
    for (int a = 0; a < 3; ++a)
      for (int sg = -1; sg <= 1; sg += 2) {
        if (c[a] + sg < 0 || c[a] + sg >= S) continue;
        const Vox n = vox_at(i + sg * kStride[a]);
        if (vox_free(n) && !material(vox_mat(n)).reinforcement) return vox_mat(n);
      }
    return own;
  };
  for (int x = 0; x < S; ++x)
    for (int y = 0; y < S; ++y)
      for (int z = 0; z < S; ++z) {
        const int i = (x * S + y) * S + z;
        const Vox v = vox_at(i);
        if (!vox_free(v)) continue;
        const MaterialId mid = host_of(i, x, y, z);
        const Material& M = material(mid);
        const i64 gx = base[0] + x, gy = base[1] + y, gz = base[2] + z;
        f64 sp[3] = {M.frag_x, M.frag_y, M.frag_z};  // (the registry keeps them >= 1)
        if (par.scale != 1.0)
          for (f64& v : sp) v = std::max(1.0, v * par.scale);
        const f64 q[3] = {gx / sp[0], gy / sp[1], gz / sp[2]};
        const i64 c0[3] = {floor_div(q[0]), floor_div(q[1]), floor_div(q[2])};
        const u64 msalt = par.salt ^ (static_cast<u64>(mid) * 0x9E3779B97F4A7C15ull);
        f64 best = 1e300;
        u64 best_h = ~0ull;
        const f64 avg = (sp[0] + sp[1] + sp[2]) / 3.0;
        for (int dx = -1; dx <= 1; ++dx)
          for (int dy = -1; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz) {
              const i64 cx = c0[0] + dx, cy = c0[1] + dy, cz = c0[2] + dz;
              const u64 hs = hash4(cx, cy, cz, msalt);
              // seed inside the cell, kept away from its faces so cells stay compact
              const f64 jx = par.jitter_lo + par.jitter_span * unit(hs), jy = par.jitter_lo + par.jitter_span * unit(mix64(hs ^ 1)),
                        jz = par.jitter_lo + par.jitter_span * unit(mix64(hs ^ 2));
              const f64 ex = (static_cast<f64>(cx) + jx) * sp[0] - static_cast<f64>(gx);
              const f64 ey = (static_cast<f64>(cy) + jy) * sp[1] - static_cast<f64>(gy);
              const f64 ez = (static_cast<f64>(cz) + jz) * sp[2] - static_cast<f64>(gz);
              // jagged seams: a per-(voxel, seed) perturbation of the distance
              const f64 n = par.noise_scale * M.frag_noise * avg * avg * unit(hash4(gx * 3 + dx, gy * 3 + dy, gz * 3 + dz, hs));
              const f64 d = ex * ex + ey * ey + ez * ez + n;
              if (d < best || (d == best && hs < best_h)) {
                best = d;
                best_h = hs;
              }
            }
        label[static_cast<size_t>(i)] = best_h | 1ull;  // never 0
      }

  // 2. connected components of equal labels over unbroken faces (scan order)
  std::vector<i32> comp(kChunkVox, -1);
  std::vector<i32> stack;
  std::vector<i32> comp_count;
  i32 ncomp = 0;
  auto face_ok = [&](int i, int j, int axis, bool plus) -> bool {
    // the face between i and its neighbour j along +/-axis is unbroken
    return plus ? ((broken_at(i) >> axis) & 1) == 0 : ((broken_at(j) >> axis) & 1) == 0;
  };
  constexpr int stride[3] = {S * S, S, 1};
  for (int i = 0; i < kChunkVox; ++i) {
    if (label[static_cast<size_t>(i)] == 0 || comp[static_cast<size_t>(i)] >= 0) continue;
    const i32 c = ncomp++;
    comp_count.push_back(0);
    comp[static_cast<size_t>(i)] = c;
    stack.assign(1, i);
    while (!stack.empty()) {
      const int k = stack.back();
      stack.pop_back();
      ++comp_count[static_cast<size_t>(c)];
      const int kc[3] = {k / (S * S), (k / S) % S, k % S};
      for (int a = 0; a < 3; ++a)
        for (int s = -1; s <= 1; s += 2) {
          const int nc = kc[a] + s;
          if (nc < 0 || nc >= S) continue;
          const int j = k + s * stride[a];
          if (comp[static_cast<size_t>(j)] >= 0 || label[static_cast<size_t>(j)] != label[static_cast<size_t>(k)]) continue;
          if (!face_ok(k, j, a, s > 0)) continue;
          comp[static_cast<size_t>(j)] = c;
          stack.push_back(j);
        }
    }
  }
  if (ncomp == 0) return out;

  // 3. merge tiny components into the same-material neighbour they share the most faces with
  UF uf(ncomp);
  const i32 min_voxels =
      par.scale == 1.0 ? par.min_voxels : std::max<i32>(1, static_cast<i32>(std::lround(par.min_voxels * par.scale * par.scale * par.scale)));
  if (min_voxels > 1) {
    std::vector<std::vector<std::pair<i32, i32>>> nb(static_cast<size_t>(ncomp));  // (other comp, faces)
    for (int i = 0; i < kChunkVox; ++i) {
      const i32 ci = comp[static_cast<size_t>(i)];
      if (ci < 0) continue;
      const int kc[3] = {i / (S * S), (i / S) % S, i % S};
      for (int a = 0; a < 3; ++a) {
        if (kc[a] + 1 >= S) continue;
        const int j = i + stride[a];
        const i32 cj = comp[static_cast<size_t>(j)];
        if (cj < 0 || cj == ci) continue;
        if ((broken_at(i) >> a) & 1) continue;
        const MaterialId mi = vox_mat(vox_at(i)), mj = vox_mat(vox_at(j));
        if (mi != mj && !material(mi).reinforcement && !material(mj).reinforcement) continue;
        if (comp_count[static_cast<size_t>(ci)] < min_voxels) nb[static_cast<size_t>(ci)].push_back({cj, 1});
        if (comp_count[static_cast<size_t>(cj)] < min_voxels) nb[static_cast<size_t>(cj)].push_back({ci, 1});
      }
    }
    for (i32 c = 0; c < ncomp; ++c) {
      if (comp_count[static_cast<size_t>(c)] >= min_voxels) continue;
      auto& list = nb[static_cast<size_t>(c)];
      if (list.empty()) continue;
      std::sort(list.begin(), list.end());
      // count faces per neighbour
      i32 best = -1, best_faces = 0;
      for (size_t k = 0; k < list.size();) {
        size_t e = k;
        i32 faces = 0;
        while (e < list.size() && list[e].first == list[k].first) faces += list[e++].second;
        const i32 other = list[k].first;
        if (faces > best_faces || (faces == best_faces && other < best)) {
          best = other;
          best_faces = faces;
        }
        k = e;
      }
      if (best >= 0) {
        const i32 ra = uf.find(c), rb = uf.find(best);
        if (ra != rb) uf.p[static_cast<size_t>(std::max(ra, rb))] = std::min(ra, rb);
      }
    }
  }

  // 4. final ids in scan order of first voxel; mass properties
  std::vector<i32> fid(static_cast<size_t>(ncomp), -1);
  out.id.assign(kChunkVox, 0);
  const f64 h = g.h;
  const f64 vol = h * h * h;
  std::vector<std::array<f64, 10>> sums;
  for (int i = 0; i < kChunkVox; ++i) {
    const i32 c = comp[static_cast<size_t>(i)];
    if (c < 0) continue;
    const i32 r = uf.find(c);
    i32& f = fid[static_cast<size_t>(r)];
    if (f < 0) {
      f = static_cast<i32>(out.frags.size());
      FragInfo fi;
      fi.first = i;
      fi.mat = vox_mat(vox_at(i));
      const int kc[3] = {i / (S * S), (i / S) % S, i % S};
      for (int a = 0; a < 3; ++a) fi.lo[a] = fi.hi[a] = static_cast<i8>(kc[a]);
      out.frags.push_back(fi);
      sums.push_back({});
    }
    SVX_ASSERT(f < 0xFFFF);
    out.id[static_cast<size_t>(i)] = static_cast<u16>(f + 1);
    FragInfo& fi = out.frags[static_cast<size_t>(f)];
    ++fi.count;
    const int kc[3] = {i / (S * S), (i / S) % S, i % S};
    for (int a = 0; a < 3; ++a) {
      fi.lo[a] = std::min<i8>(fi.lo[a], static_cast<i8>(kc[a]));
      fi.hi[a] = std::max<i8>(fi.hi[a], static_cast<i8>(kc[a]));
    }
    const f64 m = material(vox_mat(vox_at(i))).rho * vol;
    // accumulate relative to the chunk origin (small numbers), shifted to world below
    accumulate_voxel(m, V3{h * kc[0], h * kc[1], h * kc[2]}, h, sums[static_cast<size_t>(f)].data());
  }
  // (a fragment's material: its first voxel's that is not reinforcement, if any)
  for (int i = 0; i < kChunkVox; ++i) {
    const u16 id = out.id[static_cast<size_t>(i)];
    if (!id) continue;
    FragInfo& fi = out.frags[static_cast<size_t>(id - 1)];
    if (!material(fi.mat).reinforcement) continue;
    const MaterialId m = vox_mat(vox_at(i));
    if (!material(m).reinforcement) fi.mat = m;
  }
  // voxel lists per fragment (counting sort by fragment, scan order within)
  out.vox_start.assign(out.frags.size() + 1, 0);
  for (size_t f = 0; f < out.frags.size(); ++f) out.vox_start[f + 1] = out.vox_start[f] + out.frags[f].count;
  out.vox.assign(static_cast<size_t>(out.vox_start.back()), 0);
  {
    std::vector<i32> fill(out.vox_start.begin(), out.vox_start.end() - 1);
    for (int i = 0; i < kChunkVox; ++i) {
      const u16 id = out.id[static_cast<size_t>(i)];
      if (id) out.vox[static_cast<size_t>(fill[id - 1]++)] = static_cast<u16>(i);
    }
  }
  const V3 origin{h * base[0], h * base[1], h * base[2]};
  for (size_t f = 0; f < out.frags.size(); ++f) {
    const MassProps mp = finish_mass(sums[f].data(), h);
    out.frags[f].mass = mp.mass;
    out.frags[f].com = mp.com + origin;
    out.frags[f].inertia = mp.inertia;
  }
  return out;
}

}  // namespace svx
