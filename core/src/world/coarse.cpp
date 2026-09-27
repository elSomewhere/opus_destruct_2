#include "svx/world/coarse.hpp"

namespace svx {

size_t ChunkSummary::bytes() const {
  size_t b = sizeof(*this) + anchored.capacity() + voxels.capacity() * sizeof(u32);
  for (const auto& f : face) b += f.capacity() * sizeof(u16);
  return b;
}

ChunkSummary summarize_chunk(const VoxelGrid& g, const IVec3& cc) {
  ChunkSummary S;
  const Chunk* c = g.chunk(cc);
  if (!c || (c->uniform && !vox_solid(c->value))) return S;
  constexpr int N = kChunk, NN = kChunk * kChunk;
  auto vox = [&](int i) { return c->uniform ? c->value : c->v[size_t(i)]; };
  // bit a of brk(i): the bond from voxel i to its +a neighbour is broken (cracked still bonds)
  auto brk = [&](int i, int a) { return !c->broken.empty() && ((c->broken[size_t(i)] >> a) & 1); };
  const int step[3] = {NN, N, 1};  // index (x * 32 + y) * 32 + z
  auto coord = [&](int i, int a) { return a == 0 ? i / NN : a == 1 ? (i / N) % N : i % N; };
  // components of the non-anchored voxels over the chunk's intact bonds
  std::vector<i32> comp(kChunkVox, -1);
  std::vector<i32> queue;
  for (int s = 0; s < kChunkVox; ++s) {
    const Vox vs = vox(s);
    if (!vox_solid(vs) || vox_anchored(vs) || comp[size_t(s)] >= 0) continue;
    const i32 id = S.components();
    S.anchored.push_back(0);
    S.voxels.push_back(0);
    comp[size_t(s)] = id;
    queue.assign(1, s);
    for (size_t head = 0; head < queue.size(); ++head) {
      const int i = queue[head];
      ++S.voxels[size_t(id)];
      for (int a = 0; a < 3; ++a) {
        const int x = coord(i, a);
        for (int dir = -1; dir <= 1; dir += 2) {
          if ((dir < 0 && x == 0) || (dir > 0 && x == N - 1)) continue;
          const int j = i + dir * step[a];
          const Vox vj = vox(j);
          if (!vox_solid(vj) || brk(dir > 0 ? i : j, a)) continue;
          if (vox_anchored(vj)) {
            S.anchored[size_t(id)] = 1;
          } else if (comp[size_t(j)] < 0) {
            comp[size_t(j)] = id;
            queue.push_back(j);
          }
        }
      }
    }
  }
  // face labels
  for (int a = 0; a < 3; ++a) {
    const int b = (a + 1) % 3, d = (a + 2) % 3;
    for (int side = 0; side < 2; ++side) {
      std::vector<u16> lab(size_t(NN), 0);
      bool any = false;
      for (int u = 0; u < N; ++u)
        for (int v = 0; v < N; ++v) {
          int l[3];
          l[a] = side ? N - 1 : 0;
          l[b] = u;
          l[d] = v;
          const int i = l[0] * NN + l[1] * N + l[2];
          const Vox vi = vox(i);
          if (!vox_solid(vi) || (side == 1 && brk(i, a))) continue;
          lab[size_t(u * N + v)] = vox_anchored(vi) ? ChunkSummary::kAnchor : static_cast<u16>(1 + comp[size_t(i)]);
          any = true;
        }
      if (any) S.face[size_t(2 * a + side)] = std::move(lab);
    }
  }
  return S;
}

}  // namespace svx
