// svx_anim — voxel damage of character models: rays against posed models (bullets), carving
// voxels out (wounds, gore), and severing parts that lost their connection to the joint they hang
// from (limbs shot off, gibs).
//
// Posed models are given by their skin matrices (WorldPose::write_skin: 16 floats per bone,
// column-major, rigid, mapping rest model-space points to world). Rays are taken into each part's
// rest space with the inverse of its bone's matrix and walked through the part's cells (3D DDA).
// Carving and severing work in rest space, on the model's own parts: damage a character's clone
// of a shared model (VoxelModel::clone), never the shared one.
#pragma once

#include <optional>
#include <span>
#include <vector>

#include "svx/anim/voxel/model.hpp"

namespace svx::anim {

struct CharacterHit {
  i32 part = -1;                     // index into model.parts
  i32 bone = -1;                     // the part's bone
  std::array<i32, 3> cell{0, 0, 0};  // lattice coordinates of the voxel hit
  V3 point;                          // world hit point (on the voxel face)
  V3 normal;                         // world face normal
  V3 rest_point;                     // the hit point in rest model space
  f64 distance = 0.0;                // along the ray
  u8 slot = 0;                       // palette slot of the voxel hit
};

struct RemovedVoxel {
  i32 bone = 0;
  V3 rest;  // the removed voxel's centre in rest model space
  u8 slot = 0;
  u8 shade = 0;
};

// The nearest voxel hit along the ray (unit `dir`) over all parts of the posed model (`skin`: 16
// floats per bone), if any within max_dist. Each part is culled by its bounding sphere, then by a
// slab test in its rest space, then walked cell by cell.
std::optional<CharacterHit> raycast_model(const VoxelModel& model, std::span<const f32> skin, const V3& origin, const V3& dir, f64 max_dist);

// Removes every solid cell within `radius` (m) of `center_rest` (rest model space) from the given
// parts (null: every part in range - joint balls are copied into several parts, so a wound at a
// joint opens all of them). Bumps each changed part's version and updates its count; the removed
// voxels are appended to `out` if given (a voxel shared by several parts is reported once).
// Deterministic: parts in order, cells z-major.
void carve_model(VoxelModel& model, const V3& center_rest, f64 radius, const std::vector<i32>* parts, std::vector<RemovedVoxel>* out);
inline std::vector<RemovedVoxel> carve_model(VoxelModel& model, const V3& center_rest, f64 radius, const std::vector<i32>* parts = nullptr) {
  std::vector<RemovedVoxel> out;
  carve_model(model, center_rest, radius, parts, &out);
  return out;
}

// The remaining fraction of a part's voxels (count / initial_count), 0..1.
f64 part_integrity(const VoxelPart& part);

// Severing: the cells of part `part` no longer 6-connected to its anchor (the cells within
// `anchor_radius` of its bone's rest head, where it hangs from its parent) are split off, removed
// from the part and returned as new parts (same bone, shrunk to their bounds, version 0), one per
// connected component, largest first. With no anchor cell left the whole part comes off as one
// piece (the limb is cut at the joint). The root and the bones directly under it (the pelvis: the
// body itself) never sever. Empty if nothing came off.
std::vector<VoxelPart> sever_disconnected(VoxelModel& model, i32 part, f64 anchor_radius);

// Removes the part of `bone` (and, with include_children, the parts of every bone below it) from
// the model and returns copies of them (a limb cut off with what hangs from it). Emptied parts keep
// their place in model.parts with count 0 and a bumped version.
std::vector<VoxelPart> detach_subtree(VoxelModel& model, i32 bone, bool include_children);

// The damage a model took, to keep with a body (a corpse the world archives comes back as it
// was): the cells of `whole` that `damaged` - a damaged copy of it (carving and severing clear
// cells in place: the same parts, the same boxes) - no longer has, as runs per changed part.
// Empty if none are gone.
std::vector<u8> encode_damage(const VoxelModel& whole, const VoxelModel& damaged);
// Clears the cells a damage record says are gone (from a copy of the model it was taken from),
// bumping the changed parts' versions. False if the record does not fit the model's parts (the
// model is then left as it was).
bool apply_damage(VoxelModel& model, std::span<const u8> record);

}  // namespace svx::anim
