#include "svx/anim/voxel/mesh.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace svx::anim {

namespace {

// Face directions: the normal, and tangents u, v with u x v = normal (CCW quads).
struct Face {
  i32 n[3], u[3], v[3];
};
constexpr Face kFaces[6] = {
    {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},
    {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
};

// Math.round: the nearest integer, halves up.
inline f64 js_round(f64 x) {
  const f64 r = std::ceil(x);
  return r - 0.5 <= x ? r : r - 1.0;
}

inline void put_u32(u8* p, u32 v) {
  p[0] = static_cast<u8>(v);
  p[1] = static_cast<u8>(v >> 8);
  p[2] = static_cast<u8>(v >> 16);
  p[3] = static_cast<u8>(v >> 24);
}

struct Builder {
  CharacterMesh m;
  explicit Builder(i32 cap) {
    m.vertices.reserve(size_t(cap) * kCharVertexStride);
    m.indices.reserve(size_t(std::ceil(cap * 1.5)));
  }
  u32 vertex(f64 x, f64 y, f64 z, i32 nx, i32 ny, i32 nz, f64 ao, u32 packed) {
    const size_t b = m.vertices.size();
    m.vertices.resize(b + kCharVertexStride);
    u8* p = m.vertices.data() + b;
    put_u32(p, std::bit_cast<u32>(static_cast<f32>(x)));
    put_u32(p + 4, std::bit_cast<u32>(static_cast<f32>(y)));
    put_u32(p + 8, std::bit_cast<u32>(static_cast<f32>(z)));
    p[12] = static_cast<u8>(static_cast<i8>(nx * 127));
    p[13] = static_cast<u8>(static_cast<i8>(ny * 127));
    p[14] = static_cast<u8>(static_cast<i8>(nz * 127));
    p[15] = static_cast<u8>(static_cast<i8>(js_round((ao * 2.0 - 1.0) * 127.0)));
    put_u32(p + 16, packed);
    return static_cast<u32>(m.vertex_count++);
  }
  void tri(u32 a, u32 b, u32 c) {
    m.indices.push_back(a);
    m.indices.push_back(b);
    m.indices.push_back(c);
    m.index_count += 3;
  }
};

// A fingerprint of a part's shape and cells (FNV-1a, 64 bits).
u64 fingerprint(const VoxelPart& p) {
  u64 h = 0xcbf29ce484222325ull;
  auto mix = [&h](u64 v) {
    h ^= v;
    h *= 0x100000001b3ull;
  };
  mix(static_cast<u32>(p.bone));
  for (int a = 0; a < 3; ++a) {
    mix(static_cast<u32>(p.origin[size_t(a)]));
    mix(static_cast<u32>(p.dims[size_t(a)]));
  }
  mix(static_cast<u32>(p.count));
  for (const u8 c : p.cells) mix(c);
  for (const u8 c : p.shade) mix(c);
  for (const u8 c : p.stain) mix(c);
  return h;
}

}  // namespace

CharacterMesh mesh_part(const VoxelPart& part, f64 voxel_size) { return mesh_part(part, voxel_size, part.bone); }

CharacterMesh mesh_part(const VoxelPart& part, f64 voxel_size, i32 bone) {
  const i32 nx = part.dims[0], ny = part.dims[1], nz = part.dims[2];
  Builder b(std::max(64, part.count * 6));
  const std::vector<u8>& cells = part.cells;
  auto solid = [&](i32 x, i32 y, i32 z) -> i32 {
    return x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz ? 0 : cells[size_t(x + nx * (y + ny * z))] != 0 ? 1 : 0;
  };
  const f64 s = voxel_size;
  const i32 ox = part.origin[0], oy = part.origin[1], oz = part.origin[2];
  f64 ao[4] = {0, 0, 0, 0};
  for (i32 z = 0; z < nz; ++z)
    for (i32 y = 0; y < ny; ++y)
      for (i32 x = 0; x < nx; ++x) {
        const size_t n = size_t(x + nx * (y + ny * z));
        if (cells[n] == 0) continue;
        const u32 packed = pack_vertex_word(bone, part.shown_cell(n) - 1, part.shown_shade(n));
        for (const Face& f : kFaces) {
          const i32 lx = x + f.n[0], ly = y + f.n[1], lz = z + f.n[2];
          if (solid(lx, ly, lz)) continue;
          // corner (du, dv) in CCW order: (0,0), (1,0), (1,1), (0,1)
          for (int q = 0; q < 4; ++q) {
            const i32 du = q == 1 || q == 2 ? 1 : -1;
            const i32 dv = q >= 2 ? 1 : -1;
            const i32 s1 = solid(lx + f.u[0] * du, ly + f.u[1] * du, lz + f.u[2] * du);
            const i32 s2 = solid(lx + f.v[0] * dv, ly + f.v[1] * dv, lz + f.v[2] * dv);
            const i32 cr = solid(lx + f.u[0] * du + f.v[0] * dv, ly + f.u[1] * du + f.v[1] * dv, lz + f.u[2] * du + f.v[2] * dv);
            ao[q] = s1 && s2 ? 0.0 : (3 - s1 - s2 - cr) / 3.0;
          }
          // the face's base corner: the cell's min corner, moved to the far side along +n
          const i32 bx = x + (f.n[0] > 0 ? 1 : 0), by = y + (f.n[1] > 0 ? 1 : 0), bz = z + (f.n[2] > 0 ? 1 : 0);
          u32 v[4];
          for (int q = 0; q < 4; ++q) {
            const i32 du = q == 1 || q == 2 ? 1 : 0;
            const i32 dv = q >= 2 ? 1 : 0;
            const f64 px = (ox + bx + f.u[0] * du + f.v[0] * dv) * s;
            const f64 py = (oy + by + f.u[1] * du + f.v[1] * dv) * s;
            const f64 pz = (oz + bz + f.u[2] * du + f.v[2] * dv) * s;
            v[q] = b.vertex(px, py, pz, f.n[0], f.n[1], f.n[2], ao[q], packed);
          }
          // split along the brighter diagonal (no AO anisotropy artefacts)
          if (ao[0] + ao[2] >= ao[1] + ao[3]) {
            b.tri(v[0], v[1], v[2]);
            b.tri(v[0], v[2], v[3]);
          } else {
            b.tri(v[1], v[2], v[3]);
            b.tri(v[1], v[3], v[0]);
          }
        }
      }
  return std::move(b.m);
}

CharacterMesh merge_meshes(const std::vector<const CharacterMesh*>& meshes) {
  CharacterMesh out;
  i32 nv = 0, ni = 0;
  for (const CharacterMesh* m : meshes) {
    nv += m->vertex_count;
    ni += m->index_count;
  }
  out.vertices.resize(size_t(nv) * kCharVertexStride);
  out.indices.resize(size_t(ni));
  i32 vo = 0, io = 0;
  for (const CharacterMesh* m : meshes) {
    std::copy_n(m->vertices.begin(), size_t(m->vertex_count) * kCharVertexStride, out.vertices.begin() + size_t(vo) * kCharVertexStride);
    for (i32 k = 0; k < m->index_count; ++k) out.indices[size_t(io + k)] = m->indices[size_t(k)] + static_cast<u32>(vo);
    vo += m->vertex_count;
    io += m->index_count;
  }
  out.vertex_count = nv;
  out.index_count = ni;
  return out;
}

CharacterMesh ModelMesher::mesh(const VoxelModel& model) { return mesh(model, model.parts); }

CharacterMesh ModelMesher::mesh(const VoxelModel& model, std::span<const VoxelPart> parts) {
  std::vector<const CharacterMesh*> meshes;
  meshes.reserve(parts.size());
  for (const VoxelPart& p : parts) {
    if (p.count == 0) continue;
    const u64 fp = fingerprint(p);
    auto it = cache_.find(&p);
    if (it == cache_.end() || it->second.version != p.version || it->second.fingerprint != fp) {
      Entry e{p.version, fp, mesh_part(p, model.voxel_size)};
      it = cache_.insert_or_assign(&p, std::move(e)).first;
    }
    meshes.push_back(&it->second.mesh);
  }
  return merge_meshes(meshes);
}

}  // namespace svx::anim
