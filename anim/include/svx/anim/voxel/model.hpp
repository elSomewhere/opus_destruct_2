// svx_anim — voxel character models: voxels bound to the bones of a skeleton.
//
// A model lives on a lattice of pitch `voxel_size` in rest model space: voxel (i, j, k) is the
// cube centred at ((i + 1/2) s, (j + 1/2) s, (k + 1/2) s), so the ground (z = 0) and the sagittal
// plane (x = 0) are voxel boundaries. Each bone that carries voxels has one part: a dense box of
// cells, each empty or holding a palette slot and a shade.
//
// Colours are not in the model: voxels hold a palette slot (skin, hair, top, ...) and a shade (a
// per-voxel brightness), and a character supplies a palette - one model serves many colour
// variants (Doom's colour translation).
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "svx/anim/skeleton.hpp"

namespace svx::anim {

// Palette slots (16).
namespace Slot {
enum : u8 {
  Skin = 0,
  Hair = 1,
  Top = 2,
  Top2 = 3,
  Bottom = 4,
  Bottom2 = 5,
  Shoes = 6,
  Gear = 7,
  GearDark = 8,
  Metal = 9,
  Furniture = 10,
  Detail = 11,
  Accent = 12,
  Flesh = 13,
  Bone = 14,
  Blood = 15,
};
}  // namespace Slot
constexpr int kSlotCount = 16;
extern const std::array<const char*, kSlotCount> kSlotName;  // ("top variation")

// Linear RGB per slot.
using Palette = std::array<std::array<f32, 3>, kSlotCount>;

// What a cell is made of, for damage (damage/anatomy.hpp: its resistance): a property of the
// material, not of its look. A model maps its slots to tissues (VoxelModel::tissue); a cell may
// override its slot's (VoxelPart::tissue).
enum class Tissue : u8 { Soft, Flesh, Bone, Metal };
constexpr int kTissueCount = 4;
std::array<Tissue, kSlotCount> default_tissues();  // Flesh, Bone and Metal slots as such; the rest soft

struct VoxelPart {
  i32 bone = 0;
  std::array<i32, 3> origin{0, 0, 0};  // lattice coordinates of cell (0, 0, 0)
  std::array<i32, 3> dims{0, 0, 0};
  std::vector<u8> cells;  // per cell (x fastest, then y, then z): 0 empty, else slot + 1
  std::vector<u8> shade;  // per cell brightness, 128 = 1.0
  std::vector<u8> stain;  // per cell blood on it (0: none, else the shade it is drawn with); empty: none
  std::vector<u8> tissue;  // per cell tissue override (0: its slot's, else Tissue + 1); empty: none
  i32 count = 0;          // solid cells now ...
  i32 initial_count = 0;  // ... and when built (damage bookkeeping)
  u32 version = 0;        // bumped on every change (meshes cache by it)
  i32 index(i32 x, i32 y, i32 z) const { return x + dims[0] * (y + dims[1] * z); }
  // The slot and shade cell n is drawn with: blood over what is there.
  u8 shown_cell(size_t n) const { return cells[n] && !stain.empty() && stain[n] ? u8(Slot::Blood + 1) : cells[n]; }
  u8 shown_shade(size_t n) const { return !stain.empty() && stain[n] ? stain[n] : shade.empty() ? u8(128) : shade[n]; }
  // (copies cell n's look and make from another part's cell m)
  void copy_cell(size_t n, const VoxelPart& from, size_t m);
  void clear_cell(size_t n);
  // slot + 1 of the cell at lattice (i, j, k); 0: empty, or outside the part
  u8 cell(i32 i, i32 j, i32 k) const {
    const i32 x = i - origin[0], y = j - origin[1], z = k - origin[2];
    if (x < 0 || y < 0 || z < 0 || x >= dims[0] || y >= dims[1] || z >= dims[2]) return 0;
    return cells[size_t(index(x, y, z))];
  }
};

class VoxelModel {
 public:
  VoxelModel(SkeletonPtr sk, f64 voxel_size, std::vector<VoxelPart> parts, std::string name = "model");
  SkeletonPtr skeleton;
  f64 voxel_size = 0.0;
  std::vector<VoxelPart> parts;
  std::string name;
  std::array<Tissue, kSlotCount> tissue = default_tissues();  // what each slot is made of
  std::vector<i16> part_of_bone;  // part index per bone (-1: the bone carries no voxels)
  Tissue tissue_at(const VoxelPart& p, size_t n) const {
    if (!p.tissue.empty() && p.tissue[n]) return Tissue(p.tissue[n] - 1);
    return p.cells[n] ? tissue[size_t(p.cells[n] - 1) % kSlotCount] : Tissue::Soft;
  }
  std::shared_ptr<VoxelModel> clone() const;  // (damage goes to a character's own copy)
  i32 voxel_count() const;
  V3 cell_centre(i32 i, i32 j, i32 k) const { return V3{(i + 0.5) * voxel_size, (j + 0.5) * voxel_size, (k + 0.5) * voxel_size}; }
};

using ModelPtr = std::shared_ptr<VoxelModel>;

// The rest-space box of a part's cells (min, max corners).
void part_bounds(const VoxelPart& p, f64 s, V3* lo, V3* hi);
// A part that keeps only its occupied box (after sculpting or damage).
VoxelPart shrink_part(const VoxelPart& p);

}  // namespace svx::anim
