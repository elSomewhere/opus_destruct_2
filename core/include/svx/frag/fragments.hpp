// structvox — fragments: the pre-scored rubble pieces of the world (docs/V2_DESIGN.md §1).
//
// The free (non-anchored) voxels of a chunk are partitioned into fragments by a deterministic
// function of the chunk's content: every voxel joins the nearest seed of a jittered lattice
// (spacing and seam noise per material), and each (seed, material) cell splits into its
// 6-connected components over unbroken faces. Tiny components merge into their best-connected
// neighbour. Fragments never cross chunk borders, so a chunk is re-fragmented on its own
// (after a carve, on streaming in), and the ids of untouched fragments are stable.
#pragma once

#include <array>
#include <vector>

#include "svx/base/vec.hpp"
#include "svx/world/grid.hpp"

namespace svx {

struct FragInfo {
  i32 count = 0;                   // voxels (0: removed whole, e.g. detached)
  i32 first = 0;                   // chunk-local index of its first voxel (scan order): identity
  MaterialId mat = MaterialId::Concrete;
  f64 mass = 0.0;                  // kg
  V3 com;                          // centre of mass (world metres; voxel p's centre is h p)
  M3 inertia;                      // about com, world axes
  std::array<i8, 3> lo{0, 0, 0};   // chunk-local bounding box, inclusive
  std::array<i8, 3> hi{0, 0, 0};
};

struct FragChunk {
  u32 vox_version = 0;             // the grid chunk's vox_version it was built from
  std::vector<u16> id;             // kChunkVox entries (index (x*32+y)*32+z): 0 = none, else fragment + 1
  std::vector<FragInfo> frags;
  std::vector<u16> vox;            // chunk-local voxel indices, grouped by fragment ...
  std::vector<i32> vox_start;      // ... fragment f: vox[vox_start[f] .. vox_start[f + 1])
  bool empty() const { return frags.empty(); }
  i32 at(int local_index) const { return id.empty() ? -1 : static_cast<i32>(id[local_index]) - 1; }
};

struct FragParams {
  i32 min_voxels = 4;              // smaller components merge into a neighbour (if they have one)
  f64 jitter_lo = 0.25, jitter_span = 0.5;  // seed position within its lattice cell
  f64 noise_scale = 0.5;           // seam noise (x the material's)
  u64 salt = 0x5EEDF4A6ull;        // seam pattern
};

// Fragments of chunk cc of the grid (uses its voxels, broken faces and h).
FragChunk fragment_chunk(const VoxelGrid& g, const IVec3& cc, const FragParams& p = {});

// Mass properties of a voxel set: mass, centre of mass and inertia about it (world axes), from
// voxel centres h p and each voxel's own cube inertia.
struct MassProps {
  f64 mass = 0.0;
  V3 com;
  M3 inertia;
};
void accumulate_voxel(f64 m, const V3& centre, f64 h, f64* sums /* 10: m, mx,my,mz, mxx,myy,mzz,mxy,mxz,myz */);
MassProps finish_mass(const f64* sums, f64 h);

}  // namespace svx
