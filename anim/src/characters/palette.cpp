#include "svx/anim/characters/palette.hpp"

namespace svx::anim {

namespace {

f64 srgb_to_linear(f64 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

std::array<f32, 3> rgb32(u32 h) {
  const Rgb c = hex(h);
  return {static_cast<f32>(c[0]), static_cast<f32>(c[1]), static_cast<f32>(c[2])};
}

}  // namespace

Rgb hex(u32 h) {
  return {srgb_to_linear(((h >> 16) & 255) / 255.0), srgb_to_linear(((h >> 8) & 255) / 255.0), srgb_to_linear((h & 255) / 255.0)};
}

Palette make_palette(const PaletteSpec& p) {
  Palette out{};
  out[Slot::Skin] = rgb32(p.skin);
  out[Slot::Hair] = rgb32(p.hair);
  out[Slot::Top] = rgb32(p.top);
  out[Slot::Top2] = rgb32(p.top2);
  out[Slot::Bottom] = rgb32(p.bottom);
  out[Slot::Bottom2] = rgb32(p.bottom2);
  out[Slot::Shoes] = rgb32(p.shoes);
  out[Slot::Gear] = rgb32(p.gear);
  out[Slot::GearDark] = rgb32(p.gear_dark);
  out[Slot::Metal] = rgb32(p.metal);
  out[Slot::Furniture] = rgb32(p.furniture);
  out[Slot::Detail] = rgb32(p.detail);
  out[Slot::Accent] = rgb32(p.accent);
  out[Slot::Flesh] = rgb32(p.flesh);
  out[Slot::Bone] = rgb32(p.bone);
  out[Slot::Blood] = rgb32(p.blood);
  return out;
}

}  // namespace svx::anim
