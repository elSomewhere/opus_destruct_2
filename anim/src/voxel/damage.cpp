#include "svx/anim/voxel/damage.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace svx::anim {

namespace {

// The squared distance from point c to the rest-space box of a part.
f64 box_dist2(const VoxelPart& p, f64 s, const V3& c) {
  f64 d2 = 0.0;
  for (int a = 0; a < 3; ++a) {
    const f64 lo = p.origin[size_t(a)] * s;
    const f64 hi = (p.origin[size_t(a)] + p.dims[size_t(a)]) * s;
    const f64 v = c[a];
    const f64 e = v < lo ? lo - v : v > hi ? v - hi : 0.0;
    d2 += e * e;
  }
  return d2;
}

// The lattice cells [a, b] of [lo, hi] within [first, last] (clamped in f64 first); false if none.
bool clamp_range(f64 lo, f64 hi, i32 first, i32 last, i32& a, i32& b) {
  const f64 fa = std::max<f64>(lo, first), fb = std::min<f64>(hi, last);
  if (!(fa <= fb)) return false;
  a = static_cast<i32>(fa);
  b = static_cast<i32>(fb);
  return true;
}

// A new part of src's box holding only the listed cells (shrunk, version 0).
VoxelPart piece_of(const VoxelPart& src, const std::vector<i32>& list) {
  const size_t n = src.cells.size();
  VoxelPart full;
  full.bone = src.bone;
  full.origin = src.origin;
  full.dims = src.dims;
  full.cells.assign(n, 0);
  full.shade.assign(n, 0);
  full.count = full.initial_count = static_cast<i32>(list.size());
  for (const i32 idx : list) full.copy_cell(size_t(idx), src, size_t(idx));
  VoxelPart out = shrink_part(full);
  out.initial_count = out.count;
  out.version = 0;
  return out;
}

}  // namespace

std::optional<CharacterHit> raycast_model(const VoxelModel& model, std::span<const f32> skin, const V3& origin, const V3& dir, f64 max_dist) {
  const f64 inf = std::numeric_limits<f64>::infinity();
  const f64 s = model.voxel_size;
  const Skeleton& sk = *model.skeleton;
  f64 best = max_dist;
  i32 hit_part = -1;
  i32 hx = 0, hy = 0, hz = 0;  // the cell (local indices)
  i32 h_axis = 0, h_sign = 0;
  for (size_t pi = 0; pi < model.parts.size(); ++pi) {
    const VoxelPart& p = model.parts[pi];
    if (p.count == 0) continue;
    if (p.bone < 0 || size_t(p.bone) * 16 + 16 > skin.size() || p.bone >= sk.count) continue;
    const f32* m = skin.data() + size_t(p.bone) * 16;
    const i32 nx = p.dims[0], ny = p.dims[1], nz = p.dims[2];
    // the rest-space box
    const f64 bx0 = p.origin[0] * s, by0 = p.origin[1] * s, bz0 = p.origin[2] * s;
    const f64 bx1 = bx0 + nx * s, by1 = by0 + ny * s, bz1 = bz0 + nz * s;
    // its bounding sphere in the world
    const f64 cx = (bx0 + bx1) / 2.0, cy = (by0 + by1) / 2.0, cz = (bz0 + bz1) / 2.0;
    const f64 wx = m[0] * cx + m[4] * cy + m[8] * cz + m[12];
    const f64 wy = m[1] * cx + m[5] * cy + m[9] * cz + m[13];
    const f64 wz = m[2] * cx + m[6] * cy + m[10] * cz + m[14];
    const f64 r = 0.5 * std::sqrt((bx1 - bx0) * (bx1 - bx0) + (by1 - by0) * (by1 - by0) + (bz1 - bz0) * (bz1 - bz0));
    const f64 ex = wx - origin.x, ey = wy - origin.y, ez = wz - origin.z;
    const f64 tc = ex * dir.x + ey * dir.y + ez * dir.z;
    if (tc < -r || tc - r > best) continue;
    const f64 perp2 = ex * ex + ey * ey + ez * ez - tc * tc;
    if (perp2 > r * r) continue;
    // the ray in rest space: x_rest = R^T (x_world - T)
    const f64 tx = origin.x - m[12], ty = origin.y - m[13], tz = origin.z - m[14];
    const f64 ro[3] = {m[0] * tx + m[1] * ty + m[2] * tz, m[4] * tx + m[5] * ty + m[6] * tz, m[8] * tx + m[9] * ty + m[10] * tz};
    const f64 rd[3] = {m[0] * dir.x + m[1] * dir.y + m[2] * dir.z, m[4] * dir.x + m[5] * dir.y + m[6] * dir.z,
                       m[8] * dir.x + m[9] * dir.y + m[10] * dir.z};
    // slab test
    f64 t0 = 0.0, t1 = best;
    i32 entry_axis = -1;
    const f64 lo[3] = {bx0, by0, bz0}, hi[3] = {bx1, by1, bz1};
    bool miss = false;
    for (int a = 0; a < 3; ++a) {
      const f64 d = rd[a];
      const f64 oa = ro[a];
      if (std::abs(d) < 1e-12) {
        if (oa < lo[a] || oa > hi[a]) {
          miss = true;
          break;
        }
        continue;
      }
      f64 ta = (lo[a] - oa) / d, tb = (hi[a] - oa) / d;
      if (ta > tb) std::swap(ta, tb);
      if (ta > t0) {
        t0 = ta;
        entry_axis = a;
      }
      if (tb < t1) t1 = tb;
      if (t0 > t1) {
        miss = true;
        break;
      }
    }
    if (miss) continue;
    // DDA in local cell units
    const f64 px = (ro[0] + rd[0] * t0) / s - p.origin[0];
    const f64 py = (ro[1] + rd[1] * t0) / s - p.origin[1];
    const f64 pz = (ro[2] + rd[2] * t0) / s - p.origin[2];
    i32 ix = static_cast<i32>(std::min<f64>(nx - 1, std::max(0.0, std::floor(px))));
    i32 iy = static_cast<i32>(std::min<f64>(ny - 1, std::max(0.0, std::floor(py))));
    i32 iz = static_cast<i32>(std::min<f64>(nz - 1, std::max(0.0, std::floor(pz))));
    const i32 sx = rd[0] > 0.0 ? 1 : -1, sy = rd[1] > 0.0 ? 1 : -1, sz = rd[2] > 0.0 ? 1 : -1;
    const f64 dtx = rd[0] != 0.0 ? s / std::abs(rd[0]) : inf;
    const f64 dty = rd[1] != 0.0 ? s / std::abs(rd[1]) : inf;
    const f64 dtz = rd[2] != 0.0 ? s / std::abs(rd[2]) : inf;
    // the parameter of the next cell boundary on each axis
    f64 ntx = rd[0] != 0.0 ? t0 + ((sx > 0 ? ix + 1 - px : px - ix) * s) / std::abs(rd[0]) : inf;
    f64 nty = rd[1] != 0.0 ? t0 + ((sy > 0 ? iy + 1 - py : py - iy) * s) / std::abs(rd[1]) : inf;
    f64 ntz = rd[2] != 0.0 ? t0 + ((sz > 0 ? iz + 1 - pz : pz - iz) * s) / std::abs(rd[2]) : inf;
    f64 t = t0;
    i32 axis = entry_axis;
    const std::vector<u8>& cells = p.cells;
    for (;;) {
      if (cells[size_t(ix + nx * (iy + ny * iz))] != 0) {
        // (t <= t1 <= best: nearer than any earlier part's hit)
        best = t;
        hit_part = static_cast<i32>(pi);
        hx = ix;
        hy = iy;
        hz = iz;
        if (axis < 0) {
          // the ray starts inside a solid cell: the face it looks at
          const f64 ax = std::abs(rd[0]), ay = std::abs(rd[1]), az = std::abs(rd[2]);
          axis = ax >= ay && ax >= az ? 0 : ay >= az ? 1 : 2;
        }
        h_axis = axis;
        h_sign = -(axis == 0 ? sx : axis == 1 ? sy : sz);
        break;
      }
      if (ntx < nty && ntx < ntz) {
        t = ntx;
        ix += sx;
        ntx += dtx;
        axis = 0;
        if (ix < 0 || ix >= nx) break;
      } else if (nty < ntz) {
        t = nty;
        iy += sy;
        nty += dty;
        axis = 1;
        if (iy < 0 || iy >= ny) break;
      } else {
        t = ntz;
        iz += sz;
        ntz += dtz;
        axis = 2;
        if (iz < 0 || iz >= nz) break;
      }
      if (t > t1) break;
    }
  }
  if (hit_part < 0) return std::nullopt;
  const VoxelPart& p = model.parts[size_t(hit_part)];
  const f32* m = skin.data() + size_t(p.bone) * 16;
  // the rest-space ray of the winning part, again
  const f64 tx = origin.x - m[12], ty = origin.y - m[13], tz = origin.z - m[14];
  const f64 orx = m[0] * tx + m[1] * ty + m[2] * tz;
  const f64 ory = m[4] * tx + m[5] * ty + m[6] * tz;
  const f64 orz = m[8] * tx + m[9] * ty + m[10] * tz;
  const f64 drx = m[0] * dir.x + m[1] * dir.y + m[2] * dir.z;
  const f64 dry = m[4] * dir.x + m[5] * dir.y + m[6] * dir.z;
  const f64 drz = m[8] * dir.x + m[9] * dir.y + m[10] * dir.z;
  // the world normal: R times the rest face normal (axis h_axis, sign h_sign)
  const f32* col = m + h_axis * 4;
  CharacterHit hit;
  hit.part = hit_part;
  hit.bone = p.bone;
  hit.cell = {p.origin[0] + hx, p.origin[1] + hy, p.origin[2] + hz};
  hit.point = V3{origin.x + dir.x * best, origin.y + dir.y * best, origin.z + dir.z * best};
  hit.normal = V3{col[0] * static_cast<f64>(h_sign), col[1] * static_cast<f64>(h_sign), col[2] * static_cast<f64>(h_sign)};
  hit.rest_point = V3{orx + drx * best, ory + dry * best, orz + drz * best};
  hit.distance = best;
  hit.slot = static_cast<u8>(p.cells[size_t(p.index(hx, hy, hz))] - 1);
  return hit;
}

void carve_model(VoxelModel& model, const V3& c, f64 radius, const std::vector<i32>* parts, std::vector<RemovedVoxel>* out) {
  const f64 s = model.voxel_size;
  const f64 r2 = radius * radius;
  const f64 i0 = std::floor((c.x - radius) / s), i1 = std::floor((c.x + radius) / s);
  const f64 j0 = std::floor((c.y - radius) / s), j1 = std::floor((c.y + radius) / s);
  const f64 k0 = std::floor((c.z - radius) / s), k1 = std::floor((c.z + radius) / s);
  for (const f64 v : {i0, i1, j0, j1, k0, k1})
    if (std::isnan(v)) return;  // (no cell in range)
  std::vector<i32> all;
  if (!parts) {
    for (size_t i = 0; i < model.parts.size(); ++i) all.push_back(static_cast<i32>(i));
    parts = &all;
  }
  std::unordered_set<u64> seen;
  for (const i32 pi : *parts) {
    if (pi < 0 || size_t(pi) >= model.parts.size()) continue;
    VoxelPart& p = model.parts[size_t(pi)];
    if (p.count == 0 || box_dist2(p, s, c) > r2) continue;
    const i32 nx = p.dims[0], ny = p.dims[1], nz = p.dims[2];
    const i32 ox = p.origin[0], oy = p.origin[1], oz = p.origin[2];
    i32 ia, ib, ja, jb, ka, kb;
    if (!clamp_range(i0, i1, ox, ox + nx - 1, ia, ib) || !clamp_range(j0, j1, oy, oy + ny - 1, ja, jb) || !clamp_range(k0, k1, oz, oz + nz - 1, ka, kb)) continue;
    i32 removed = 0;
    for (i32 k = ka; k <= kb; ++k) {
      const f64 dz = (k + 0.5) * s - c.z;
      for (i32 j = ja; j <= jb; ++j) {
        const f64 dy = (j + 0.5) * s - c.y;
        for (i32 i = ia; i <= ib; ++i) {
          const f64 dx = (i + 0.5) * s - c.x;
          if (dx * dx + dy * dy + dz * dz > r2) continue;
          const size_t idx = size_t(i - ox + nx * (j - oy + ny * (k - oz)));
          const u8 cell = p.cells[idx];
          if (cell == 0) continue;
          const u8 shade = p.shade.empty() ? u8(128) : p.shade[idx];
          p.clear_cell(idx);
          ++removed;
          // the lattice key (exact for |i|, |j|, |k| < 2^15)
          const u64 key = (u64(i + 32768) * 65536u + u64(j + 32768)) * 65536u + u64(k + 32768);
          if (seen.insert(key).second && out)
            out->push_back(RemovedVoxel{p.bone, V3{(i + 0.5) * s, (j + 0.5) * s, (k + 0.5) * s}, static_cast<u8>(cell - 1), shade});
        }
      }
    }
    if (removed > 0) {
      p.count -= removed;
      ++p.version;
    }
  }
}

f64 part_integrity(const VoxelPart& part) {
  return part.initial_count > 0 ? std::max(0.0, std::min(1.0, static_cast<f64>(part.count) / static_cast<f64>(part.initial_count))) : 0.0;
}

std::vector<VoxelPart> sever_disconnected(VoxelModel& model, i32 part, f64 anchor_radius) {
  if (part < 0 || size_t(part) >= model.parts.size()) return {};
  VoxelPart& p = model.parts[size_t(part)];
  if (p.count == 0) return {};
  const Skeleton& sk = *model.skeleton;
  if (p.bone < 0 || p.bone >= sk.count) return {};
  const i32 par = sk.parents[size_t(p.bone)];
  if (par < 0 || sk.parents[size_t(par)] < 0) return {};
  const f64 s = model.voxel_size;
  const i32 nx = p.dims[0], ny = p.dims[1], nz = p.dims[2];
  const i32 ox = p.origin[0], oy = p.origin[1], oz = p.origin[2];
  const V3 head = sk.rest_head[size_t(p.bone)];
  const f64 ar2 = anchor_radius * anchor_radius;
  const i32 n = static_cast<i32>(p.cells.size());
  std::vector<u8> mark(size_t(n), 0);  // 1: reached from the anchor
  std::vector<i32> queue(static_cast<size_t>(n));
  i32 qh = 0, qt = 0;
  for (i32 k = 0; k < nz; ++k)
    for (i32 j = 0; j < ny; ++j)
      for (i32 i = 0; i < nx; ++i) {
        const i32 idx = i + nx * (j + ny * k);
        if (p.cells[size_t(idx)] == 0) continue;
        const f64 dx = (ox + i + 0.5) * s - head.x, dy = (oy + j + 0.5) * s - head.y, dz = (oz + k + 0.5) * s - head.z;
        if (dx * dx + dy * dy + dz * dz <= ar2) {
          mark[size_t(idx)] = 1;
          queue[size_t(qt++)] = idx;
        }
      }
  if (qt == 0) {
    // cut at the joint: everything comes off
    std::vector<i32> all;
    for (i32 idx = 0; idx < n; ++idx)
      if (p.cells[size_t(idx)] != 0) all.push_back(idx);
    std::vector<VoxelPart> out;
    out.push_back(piece_of(p, all));
    std::fill(p.cells.begin(), p.cells.end(), u8(0));
    p.count = 0;
    ++p.version;
    return out;
  }
  auto visit = [&](i32 m, u8 label) {
    if (mark[size_t(m)] != 0 || p.cells[size_t(m)] == 0) return;
    mark[size_t(m)] = label;
    queue[size_t(qt++)] = m;
  };
  auto flood = [&](u8 label) {
    while (qh < qt) {
      const i32 idx = queue[size_t(qh++)];
      const i32 i = idx % nx, j = (idx / nx) % ny, k = idx / (nx * ny);
      if (i > 0) visit(idx - 1, label);
      if (i < nx - 1) visit(idx + 1, label);
      if (j > 0) visit(idx - nx, label);
      if (j < ny - 1) visit(idx + nx, label);
      if (k > 0) visit(idx - nx * ny, label);
      if (k < nz - 1) visit(idx + nx * ny, label);
    }
  };
  flood(1);
  // the rest: connected components, each a piece
  std::vector<std::vector<i32>> pieces;
  for (i32 idx = 0; idx < n; ++idx) {
    if (p.cells[size_t(idx)] == 0 || mark[size_t(idx)] != 0) continue;
    qh = 0;
    qt = 0;
    mark[size_t(idx)] = 2;
    queue[size_t(qt++)] = idx;
    flood(2);
    pieces.emplace_back(queue.begin(), queue.begin() + qt);
  }
  if (pieces.empty()) return {};
  std::stable_sort(pieces.begin(), pieces.end(), [](const std::vector<i32>& a, const std::vector<i32>& b) { return a.size() > b.size(); });
  std::vector<VoxelPart> out;
  out.reserve(pieces.size());
  for (const std::vector<i32>& cells : pieces) out.push_back(piece_of(p, cells));
  i32 removed = 0;
  for (const std::vector<i32>& cells : pieces) {
    for (const i32 idx : cells) p.clear_cell(size_t(idx));
    removed += static_cast<i32>(cells.size());
  }
  p.count -= removed;
  ++p.version;
  return out;
}

std::vector<VoxelPart> detach_subtree(VoxelModel& model, i32 bone, bool include_children) {
  const Skeleton& sk = *model.skeleton;
  std::vector<VoxelPart> out;
  for (i32 b = 0; b < sk.count; ++b) {
    if (b != bone && !(include_children && sk.is_below(b, bone))) continue;
    const i32 pi = model.part_of_bone[size_t(b)];
    if (pi < 0 || size_t(pi) >= model.parts.size()) continue;
    VoxelPart& p = model.parts[size_t(pi)];
    if (p.count == 0) continue;
    std::vector<i32> all;
    for (i32 idx = 0; idx < static_cast<i32>(p.cells.size()); ++idx)
      if (p.cells[size_t(idx)] != 0) all.push_back(idx);
    out.push_back(piece_of(p, all));
    for (const i32 idx : all) p.clear_cell(size_t(idx));
    p.count = 0;
    ++p.version;
  }
  return out;
}

// ---- damage records ------------------------------------------------------------------------------
//
// "D", version 1, the model's part count (u16), the changed parts (u16), then per changed part: its
// index (u16), its cell count (u32; the record fits the model), the runs of cells gone (varint),
// and each run as the cells skipped since the last one and its length (varints; cells x fastest).

namespace {

void put_varint(std::vector<u8>& out, u64 v) {
  while (v >= 0x80) {
    out.push_back(static_cast<u8>(v | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<u8>(v));
}

template <typename T>
void put_raw(std::vector<u8>& out, T v) {
  for (size_t i = 0; i < sizeof(T); ++i) out.push_back(static_cast<u8>(static_cast<u64>(v) >> (8 * i)));
}

struct Reader {
  std::span<const u8> in;
  size_t at = 0;
  bool ok = true;
  u64 varint() {
    u64 v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (at >= in.size()) break;
      const u8 b = in[at++];
      v |= static_cast<u64>(b & 0x7f) << shift;
      if ((b & 0x80) == 0) return v;
    }
    ok = false;
    return 0;
  }
  u64 raw(size_t n) {
    if (at + n > in.size()) {
      ok = false;
      return 0;
    }
    u64 v = 0;
    for (size_t i = 0; i < n; ++i) v |= static_cast<u64>(in[at + i]) << (8 * i);
    at += n;
    return v;
  }
};

constexpr u8 kDamageMagic = 'D';
constexpr u8 kDamageVersion = 1;

}  // namespace

std::vector<u8> encode_damage(const VoxelModel& whole, const VoxelModel& damaged) {
  std::vector<u8> out;
  if (whole.parts.size() != damaged.parts.size() || whole.parts.size() > 0xffff) return out;
  std::vector<u8> body;
  u32 changed = 0;
  for (size_t pi = 0; pi < whole.parts.size(); ++pi) {
    const VoxelPart& a = whole.parts[pi];
    const VoxelPart& b = damaged.parts[pi];
    if (a.cells.size() != b.cells.size() || a.dims != b.dims || a.origin != b.origin) return {};
    if (a.count == b.count && a.cells == b.cells) continue;
    std::vector<std::pair<u64, u64>> runs;  // (start, length)
    const size_t n = a.cells.size();
    for (size_t i = 0; i < n;) {
      if (a.cells[i] == 0 || b.cells[i] != 0) {
        ++i;
        continue;
      }
      const size_t s = i;
      while (i < n && a.cells[i] != 0 && b.cells[i] == 0) ++i;
      runs.emplace_back(s, i - s);
    }
    if (runs.empty()) continue;
    ++changed;
    put_raw<u16>(body, static_cast<u16>(pi));
    put_raw<u32>(body, static_cast<u32>(n));
    put_varint(body, runs.size());
    u64 last = 0;
    for (const auto& [s, l] : runs) {
      put_varint(body, s - last);
      put_varint(body, l);
      last = s + l;
    }
  }
  if (changed == 0) return out;
  out.push_back(kDamageMagic);
  out.push_back(kDamageVersion);
  put_raw<u16>(out, static_cast<u16>(whole.parts.size()));
  put_raw<u16>(out, static_cast<u16>(changed));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

bool apply_damage(VoxelModel& model, std::span<const u8> record) {
  if (record.empty()) return true;
  Reader r{record};
  if (r.raw(1) != kDamageMagic || r.raw(1) != kDamageVersion || !r.ok) return false;
  if (r.raw(2) != model.parts.size() || !r.ok) return false;
  const u64 changed = r.raw(2);
  // (read it all first: a record that does not fit changes nothing)
  struct Run {
    u32 part;
    u64 start, length;
  };
  std::vector<Run> runs;
  for (u64 c = 0; c < changed && r.ok; ++c) {
    const u64 pi = r.raw(2);
    const u64 n = r.raw(4);
    if (!r.ok || pi >= model.parts.size() || n != model.parts[size_t(pi)].cells.size()) return false;
    const u64 count = r.varint();
    u64 at = 0;
    for (u64 k = 0; k < count && r.ok; ++k) {
      const u64 skip = r.varint(), len = r.varint();
      if (!r.ok || skip > n - at || len > n - at - skip) return false;
      runs.push_back(Run{static_cast<u32>(pi), at + skip, len});
      at += skip + len;
    }
  }
  if (!r.ok || r.at != record.size()) return false;
  std::vector<u8> touched(model.parts.size(), 0);
  for (const Run& run : runs) {
    VoxelPart& p = model.parts[run.part];
    for (u64 i = run.start; i < run.start + run.length; ++i) {
      if (p.cells[size_t(i)] == 0) continue;
      p.clear_cell(size_t(i));
      --p.count;
      touched[run.part] = 1;
    }
  }
  for (size_t pi = 0; pi < model.parts.size(); ++pi)
    if (touched[pi]) ++model.parts[pi].version;
  return true;
}

}  // namespace svx::anim
