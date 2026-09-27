// structvox mesh — voxel meshing for renderers (docs/API.md vertex format, 28 bytes): world
// chunks, rigid pieces and coarse far tiles. Optional: the physics core never meshes.
//
// Voxel p occupies the cube h (p - 1/2) .. h (p + 1/2) (its centre is the lattice cell
// centre h p). Faces between solid and air are emitted with per-vertex ambient occlusion.
// Static chunks are greedy-merged (faces with equal texture, light, debug and AO corners);
// chunks with displacement emit one quad per face and move every corner by the average
// displacement of the solid voxels sharing it (continuous, crack-free surfaces).
#pragma once

#include <functional>
#include <vector>

#include "svx/phys/rigid.hpp"
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

// A rigid piece's voxels in its shape frame (metres: shape voxel p at h p; place it at
// x + R (s - com)). The providers see shape-frame voxel coordinates.
ChunkMesh mesh_shape(const BodyShape& s, f64 h, const MeshOptions& opt);

// The water of a chunk (a voxel layer of fill levels, 255 full: svx_env's "water"): its
// surfaces towards air, at the water's height in each voxel (tops greedy-merged; sides where it
// stands higher than its neighbour's; bottoms of falling sheets). Vertices have texture 0xFFFE
// (water), light 255, AO 1; uv are world x, y (metres).
constexpr u16 kWaterTexture = 0xFFFE;
ChunkMesh mesh_water(const VoxelGrid& g, int layer, const IVec3& chunk);

// A coarse block (a far render tier): n cells of factor^3 voxels each from voxel lo, cell
// (x, y, z) at cells[(x * n1 + y) * n2 + z]; outside the block is solid at the sides and below,
// air above. Greedy quads coloured by material (no AO, no textures).
ChunkMesh mesh_coarse(const std::vector<Vox>& cells, const IVec3& n, const IVec3& lo, i32 factor, f64 h);

}  // namespace svx
