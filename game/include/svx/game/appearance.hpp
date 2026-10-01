// structvox game — the appearance table: what a voxel looks like by its material and its look
// (the "look" layer's value), for worlds whose looks are more than their materials' colours - the
// city's 454 looks of 27 physics classes, its plants (docs/API.md: texture ids). A source with a
// table (GameSource::appearances) has a regenerable "look" layer; the meshes carry texture id
// kAppearanceTexture + i for a face of appearance i, and the front end draws it from the table.
#pragma once

#include <array>
#include <vector>

#include "svx/base/types.hpp"

namespace svx {

struct Appearance {
  f32 rgb[3] = {0.3f, 0.3f, 0.3f};  // linear
  f32 opacity = 1.0f;               // below 1: drawn see-through (glazing)
  f32 emissive = 0.0f;              // 0..1: self-lit (lamps, neon, screens)
  f32 noise = 0.06f;                // per-voxel brightness variation (0..1)
  f32 gloss = 0.0f;                 // 0..1: the sky mirrored, the sun's highlight
  bool glow = false;                // window glass: a share of the panes lit at night
};

// Faces of appearance i carry texture id kAppearanceTexture + i (below the glow range 0xFE00).
inline constexpr u16 kAppearanceTexture = 0xC000;
inline constexpr int kMaxAppearances = 0xFE00 - 0xC000;

// (material id, look) -> appearance. Look 0 is a voxel without a look (its material's own colour)
// unless the table gives (material, 0) one.
class AppearanceTable {
 public:
  // Sets the appearance of (material, look): its index (an existing pair keeps its index; -1: the
  // table is full or the material out of range).
  int set(int material, int look, const Appearance& a);
  // The index of (material, look); -1: none.
  int find(int material, int look) const {
    if (material < 0 || material >= static_cast<int>(index_.size())) return -1;
    return static_cast<int>(index_[size_t(material)][size_t(look & 0xFF)]) - 1;
  }
  // The texture id of a face of (material, look): kAppearanceTexture + its appearance, else
  // `fallback` (the material's colour).
  u16 texture(int material, int look, u16 fallback) const {
    const int i = find(material, look);
    return i < 0 ? fallback : static_cast<u16>(kAppearanceTexture + i);
  }
  size_t size() const { return entries_.size(); }
  const Appearance& operator[](size_t i) const { return entries_[i]; }
  const std::vector<Appearance>& entries() const { return entries_; }
  // The table as the front end takes it: kAppearanceFloats per appearance (r, g, b, opacity,
  // emissive, noise, gloss, glow).
  std::vector<f32> packed() const;

 private:
  std::vector<Appearance> entries_;
  std::vector<std::array<u16, 256>> index_;  // per material: 1 + the appearance, 0 none
};

inline constexpr int kAppearanceFloats = 8;

// sRGB (0..255) to linear.
f32 srgb_to_linear(int c);

}  // namespace svx
