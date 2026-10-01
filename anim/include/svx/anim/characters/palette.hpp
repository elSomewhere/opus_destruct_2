// svx_anim — character palettes: one linear-RGB colour per slot (voxel/model.hpp Slot). Colours
// are written as sRGB hex (0xRRGGBB) and converted here.
#pragma once

#include <array>

#include "svx/anim/voxel/model.hpp"

namespace svx::anim {

using Rgb = std::array<f64, 3>;

// The linear RGB of an sRGB hex colour (0xRRGGBB).
Rgb hex(u32 h);

inline constexpr std::array<u32, 8> kSkinTones = {0xf1c8a6, 0xe0ac86, 0xc68a62, 0xa66b45, 0x7d4a2d, 0x5a3420, 0xd9a07a, 0xefbf9a};
inline constexpr std::array<u32, 8> kHairColours = {0x1f1712, 0x3a2618, 0x5c3b22, 0x8a6036, 0xb8894a, 0x2b2b2b, 0x6e6e6e, 0x9c3c1c};

struct PaletteSpec {
  u32 skin = 0, hair = 0, top = 0, top2 = 0, bottom = 0, bottom2 = 0, shoes = 0, gear = 0, gear_dark = 0, metal = 0, furniture = 0,
      detail = 0, accent = 0;
  u32 flesh = 0x8c1c1c, bone = 0xe0d6c0, blood = 0x5c0808;
};

Palette make_palette(const PaletteSpec& p);

// Soldier camouflage schemes.
struct CamoScheme {
  u32 top, top2, bottom, bottom2, gear, gear_dark, shoes;
};
inline constexpr std::array<CamoScheme, 4> kCamoSchemes = {{
    {0x5b6440, 0x3a4529, 0x5b6440, 0x6e5a3c, 0x4d5534, 0x262a22, 0x2d241c},  // woodland
    {0xb5a07a, 0x8c7552, 0xb5a07a, 0x6e5c40, 0x9b8660, 0x4a3f2e, 0x7a6344},  // desert
    {0x6d7074, 0x3f4246, 0x6d7074, 0x55585c, 0x2f3236, 0x1d1f21, 0x1c1c1e},  // urban
    {0x2c2e2a, 0x3d4234, 0x4a4f3a, 0x33372a, 0x3a3d33, 0x191a17, 0x1e1b17},  // mercenary black / olive
}};

inline constexpr std::array<u32, 16> kClothColours = {0xc0392b, 0x2e6fb0, 0x2f8f5b, 0xe0b43a, 0x8e44ad, 0xd35400, 0x34495e, 0xecf0f1,
                                                      0x7f8c8d, 0x16a085, 0xb03060, 0x1b2a49, 0x6b4f2a, 0xa9cce3, 0xf5e6c8, 0x222222};
inline constexpr std::array<u32, 8> kTrouserColours = {0x2b3a55, 0x3d4f73, 0x1f2328, 0x6b5b45, 0x8a7a5c, 0x4a4a4a, 0x55402b, 0x23324d};
inline constexpr std::array<u32, 6> kShoeColours = {0x1c1c1c, 0x3b2a1e, 0xe8e8e8, 0x7a1f1f, 0x2d3e50, 0x5a4632};

}  // namespace svx::anim
