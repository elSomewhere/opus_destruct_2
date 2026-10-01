// svx_city — the export's seams (PROCGEN_MERGE_PLAN.md §7.3, §8.2, §10.2: no JS twin): the faces
// of a chunk's voxels that do not bond, from the objects its writer marked (ChunkBuffer::obj). A
// loose object (svx/props.hpp) gets a seam on every face between its voxels and anything else - a
// floor, a wall, another object - so it is a free component resting where it stands; fixed
// objects and everything else bond as the structure they are.
#pragma once

#include <cstdint>
#include <vector>

#include "voxel/chunk.hpp"

namespace svx::city {

// The seams of the chunk's 32^3 voxels (ChunkSource::generate_seams' layout: structvox index
// (x * 32 + y) * 32 + z, bit a the face towards +axis a, the faces towards the next chunk's voxels
// included - the padding holds them). `solid(m)`: whether a city material is a solid voxel in the
// export (not air, liquid or left out). False (out untouched): no seam in the chunk, or no objects.
template <class Solid>
bool export_seams(const ChunkBuffer& c, Solid&& solid, std::vector<uint8_t>& out);

// Whether an object of the chunk (1 + its index in c.objects; 0: none) is loose.
bool object_loose(const ChunkBuffer& c, uint32_t object);

template <class Solid>
bool export_seams(const ChunkBuffer& c, Solid&& solid, std::vector<uint8_t>& out) {
  if (c.obj.empty() || c.objects.empty()) return false;
  std::vector<uint8_t> loose(c.objects.size() + 1, 0);
  bool any_loose = false;
  for (uint32_t k = 1; k <= c.objects.size(); ++k) any_loose = (loose[k] = object_loose(c, k) ? 1 : 0) || any_loose;
  if (!any_loose) return false;
  bool any = false;
  const uint16_t* d = c.data.data();
  const uint32_t* ob = c.obj.data();
  constexpr int kStep[3] = {1, kP, kP2};
  for (int k = 1; k <= kP - 2; ++k)
    for (int j = 1; j <= kP - 2; ++j)
      for (int i = 1; i <= kP - 2; ++i) {
        const int idx = i + j * kP + k * kP2;
        if (!d[idx] || !solid(d[idx])) continue;
        const uint32_t a = ob[idx];
        uint8_t bits = 0;
        for (int ax = 0; ax < 3; ++ax) {
          const int n = idx + kStep[ax];
          const uint32_t b = ob[n];
          if (a == b || !(loose[a] || loose[b]) || !d[n] || !solid(d[n])) continue;
          bits = static_cast<uint8_t>(bits | (1u << ax));
        }
        if (!bits) continue;
        if (!any) out.assign(size_t(kP - 2) * size_t(kP - 2) * size_t(kP - 2), 0);
        any = true;
        out[(size_t(i - 1) * size_t(kP - 2) + size_t(j - 1)) * size_t(kP - 2) + size_t(k - 1)] = bits;
      }
  return any;
}

}  // namespace svx::city
