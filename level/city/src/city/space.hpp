// svx_city — the open space record (voxel_city: the objects city/cellPlan.js's planCell makes for
// the blocks it leaves open - parks, plazas, squares, gardens, car parks, sports grounds,
// courtyards, cemeteries, allotments, garage yards, wasteland, quays, tank farms, container yards,
// riverside parks - with what city/landscape.js, city/parks.js, city/industry.js, city/dressing.js
// and voxel/compose.js read of it).
//
// A space is complete when its cell plan is (planCell sets a cemetery's `chapel` before it hands
// the plan out); nothing changes it after that but its lazy fields, each a pure function of the
// space (the reference caches them by the space object: WeakMaps in parks.js, landscape.js and
// industry.js).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "buildings/frame.hpp"
#include "city/industry.hpp"
#include "city/parks.hpp"
#include "core/cache.hpp"
#include "core/rect.hpp"

namespace svx::city {

// What the cell plan caches on a block (city/cellPlan: the block's graded surface, JS levelAt).
struct BlockSurface;

// Every field is JS's of the same name in snake_case; industry.js's (id, kind, rect, quay and its
// layout) are IndustrySpace's.
//   id      `${block.id}/o`
//   kind    plaza, quay, parking, sports, courtyard, square, garden, cemetery, allotments,
//           garages, wasteland, tankFarm, containerYard, riverside, park (any other: a park)
//   rect    the block's property rect (inclusive voxels)
//   quay    a port district's yard where the water reaches in (industry.hpp port_quay)
struct OpenSpace : IndustrySpace {
  std::string block;     // its block's id
  std::string district;  // its block's district (a courtyard of "projects" is bleaker)
  double ground_z = 0;   // the block's level at its centre (voxels)
  std::shared_ptr<const BlockSurface> level_at;  // its block's graded surface (null: none)
  bool under_highway = false;                    // a highway's corridor crosses it
  // The side on the block's main street ('\0': undefined - a superblock's courtyard; the landscape
  // reads `front ?? "S"`).
  char front = 0;
  std::optional<Rect> chapel = std::nullopt;  // a cemetery's chapel lot (JS: undefined otherwise)

  // parks.js's layout (park_layout) and landscape.js's frame of the space (spaceFrame: its rect
  // facing front ?? "S"), each made once.
  Lazy<ParkLayout> park{};
  Lazy<Frame> frame{};
};

// The ground of an open space at a column (landscape.js spaceSurface's `out`): its material, a
// height offset (voxels: a pond's or a basin's depth below the ground, a rim above it) and
// whether it is water.
struct SpaceSample {
  uint16_t mat = 0;
  double dz = 0;
  bool water = false;
};

}  // namespace svx::city
