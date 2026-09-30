// svx_anim — meshing voxel models into the character vertex format (renderer-neutral, documented
// here so any renderer can consume it).
//
// Character vertex: 20 bytes, little-endian, interleaved:
//   0  float32x3  position in rest model space (m)
//   12 snorm8x4   normal xyz (axis-aligned in rest space), w = ambient occlusion (-1..1 = 0..1)
//   16 uint32     bone (bits 0-7) | palette slot (bits 8-11) | shade (bits 12-19, 128 = 1.0)
// Indices are uint32, counter-clockwise seen from outside.
//
// A vertex is drawn at skin[bone] * position (WorldPose::write_skin): rigid skinning, so voxels
// stay cubes. Every part is meshed on its own (faces between parts are kept), so parts can rotate
// apart at their joints without opening holes into the body.
#pragma once

#include <span>
#include <unordered_map>
#include <vector>

#include "svx/anim/voxel/model.hpp"

namespace svx::anim {

constexpr i32 kCharVertexStride = 20;

struct CharacterMesh {
  std::vector<u8> vertices;  // vertex_count character vertices (kCharVertexStride bytes each)
  i32 vertex_count = 0;
  std::vector<u32> indices;
  i32 index_count = 0;
};

// The per-vertex word of the character vertex.
inline u32 pack_vertex_word(i32 bone, i32 slot, i32 shade) {
  return (static_cast<u32>(bone) & 0xffu) | ((static_cast<u32>(slot) & 0xfu) << 8) | ((static_cast<u32>(shade) & 0xffu) << 12);
}

// The mesh of one part (culled faces with per-vertex AO). `bone` overrides the part's bone in the
// vertices (gibs re-bind parts).
CharacterMesh mesh_part(const VoxelPart& part, f64 voxel_size);
CharacterMesh mesh_part(const VoxelPart& part, f64 voxel_size, i32 bone);
// Meshes concatenated (indices rebased).
CharacterMesh merge_meshes(const std::vector<const CharacterMesh*>& meshes);

// Meshes a model, reusing the meshes of parts that did not change since the last call: a model's
// damage re-meshes only the parts hit. Keep one mesher per model (or per set of models); parts are
// known by their address, checked against their version and content, so a part freed and another
// made at its address is meshed anew.
class ModelMesher {
 public:
  CharacterMesh mesh(const VoxelModel& model);
  CharacterMesh mesh(const VoxelModel& model, std::span<const VoxelPart> parts);
  void clear() { cache_.clear(); }

 private:
  struct Entry {
    u32 version = 0;
    u64 fingerprint = 0;
    CharacterMesh mesh;
  };
  std::unordered_map<const VoxelPart*, Entry> cache_;
};

}  // namespace svx::anim
