// structvox game — the "paint" layer's palette: a voxel's colour over its material's. The game's
// content (its cars, the city's facades and road markings) paints with it; the front end has the
// colours.
#pragma once

#include <array>

#include "svx/base/types.hpp"

namespace svx {

// The "paint" layer's values: a voxel's colour over its material's (0: its material's). Cars'
// paints, facades' plasters and the road's markings; the front end has their colours.
enum class Paint : u8 {
  None = 0,
  // car paints
  White, Silver, Black, Red, Blue, Green, Yellow, Orange, TaxiYellow, NavyBlue, Maroon, Beige, Graphite, Teal,
  // trim: bumpers, grilles, tail lamps, indicators
  Trim, TailRed, Amber,
  // facades
  Plaster, Cream, Terracotta, Sand, Slate, Ochre, Mint,
  // the road
  LineWhite, LineYellow, Kerb,
  Count
};

// A painted voxel's texture id for the renderer: kPaintTexture + its paint (palette slot 31 +
// paint: after the materials').
inline constexpr u16 kPaintTexture = 0xFF00 + 31;

// The paints a car is sprayed in (at random; an awning too).
inline constexpr std::array<Paint, 14> kCarPaints = {Paint::White,  Paint::Silver,     Paint::Black,    Paint::Red,    Paint::Blue,
                                                     Paint::Green,  Paint::Yellow,     Paint::Orange,   Paint::NavyBlue, Paint::Maroon,
                                                     Paint::Beige,  Paint::Graphite,   Paint::Teal,     Paint::TaxiYellow};

}  // namespace svx
