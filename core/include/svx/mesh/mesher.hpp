// structvox — chunk meshing for the front end (docs/API.md vertex format, 28 bytes).
//
// Voxel p occupies the cube h (p - 1/2) .. h (p + 1/2) (its centre is the lattice cell
// centre h p). Faces between solid and air are emitted with per-vertex ambient occlusion.
// Static chunks are greedy-merged (faces with equal texture, light, debug and AO corners);
// chunks with displacement emit one quad per face and move every corner by the average
// displacement of the solid voxels sharing it (continuous, crack-free surfaces).
#pragma once

#include <functional>
#include <vector>

#include "svx/world/grid.hpp"

namespace svx {

#pragma pack(push, 1)
struct MeshVertex {
  f32 pos[3];
  i8 normal[4];   // xyz, w = ambient occlusion (-127..127 -> 0..1)
  f32 uv[2];      // texels
  u16 texture;    // 0xFFFF = material colour, 0xFF00 + material id (untextured, by material)
  u8 light;
  u8 debug;
};
#pragma pack(pop)
static_assert(sizeof(MeshVertex) == 28, "vertex layout is part of the front-end contract");

struct ChunkMesh {
  IVec3 chunk{0, 0, 0};
  std::vector<MeshVertex> vertices;
  std::vector<u32> indices;
};

struct MeshOptions {
  f64 texels_per_metre = 32.0;       // Doom scale: 1 map unit = 1 texel
  // optional providers (return false if the voxel has none)
  std::function<bool(const IVec3&, f32 out[3])> displacement;  // metres, already amplified
  std::function<u8(const IVec3& p, int face)> light;          // default 255
  std::function<u16(const IVec3& p, int face)> texture;       // default 0xFF00 + material
  std::function<u8(const IVec3& p)> debug;                    // default 0
  // whether the providers may be called from several threads at once (chunks are then meshed
  // in parallel); false for providers with unsynchronized caches
  bool concurrent = true;
};

// Face order: 0 -x, 1 +x, 2 -y, 3 +y, 4 -z, 5 +z.
ChunkMesh mesh_chunk(const VoxelGrid& g, const IVec3& chunk, const MeshOptions& opt, bool displaced);

}  // namespace svx
