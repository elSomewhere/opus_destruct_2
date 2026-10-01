// structvox game — the appearance table (svx/game/appearance.hpp).
#include "svx/game/appearance.hpp"

#include "svx/base/dmath.hpp"
#include "svx/material/material.hpp"

namespace svx {

int AppearanceTable::set(int material, int look, const Appearance& a) {
  if (material < 0 || material >= kMaxMaterials || look < 0 || look > 255) return -1;
  if (material >= static_cast<int>(index_.size())) index_.resize(size_t(material) + 1, std::array<u16, 256>{});
  u16& slot = index_[size_t(material)][size_t(look)];
  if (slot) {
    entries_[size_t(slot) - 1] = a;
    return slot - 1;
  }
  if (static_cast<int>(entries_.size()) >= kMaxAppearances) return -1;
  entries_.push_back(a);
  slot = static_cast<u16>(entries_.size());
  return slot - 1;
}

std::vector<f32> AppearanceTable::packed() const {
  std::vector<f32> out;
  out.reserve(entries_.size() * kAppearanceFloats);
  for (const Appearance& a : entries_) {
    out.insert(out.end(), {a.rgb[0], a.rgb[1], a.rgb[2], a.opacity, a.emissive, a.noise, a.gloss, a.glow ? 1.0f : 0.0f});
  }
  return out;
}

f32 srgb_to_linear(int c) {
  const f64 s = static_cast<f64>(c < 0 ? 0 : c > 255 ? 255 : c) / 255.0;
  return static_cast<f32>(s <= 0.04045 ? s / 12.92 : dm::pow((s + 0.055) / 1.055, 2.4));
}

}  // namespace svx
