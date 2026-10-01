// svx_city — site grading (voxel_city city/grading.js): how planned ground meets the terrain and
// the streets.
//
// An urban block's ground is one smooth surface stretched between the levels of the sidewalks
// round it (cellPlan's block surface; the natural ground in a village or on a farm), so gardens,
// yards, courtyards, parks and plazas meet every street and every neighbour flush.
//
// A building stands on a level pad at the level of its entrance (lot.groundZ: the sidewalk in front
// of it, the courtyard at a slab's middle): its footprint and an apron of APRON round it, plus the
// forecourt of a civic building. Away from a pad the ground eases back to the block surface over a
// band that widens with the height difference (EASE: at most ~1 : 1.5), whatever it is (a garden,
// a courtyard, a park, the leftover ground of the block), so on a hillside a house sits on a
// terrace with its garden sloping away and neighbours meet without cliffs. Where pads crowd each
// other their pulls are averaged; where a slope has no room, the rest is a retaining wall (compose
// caps it).
//
// Lots that need one level surface (work yards, car parks, petrol forecourts, schoolyards, wharves)
// stay a single terrace at the entrance level and act as one big pad for the ground round them.
//
// SiteGrading::at is the one answer for graded ground in a cell: the ground tiles (compose) and
// everything placed on it (yard trees, hedges, fences, sheds, park furniture; dressing) ask here.
// Immutable once made: any thread may ask.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "buildings/archetypes.hpp"
#include "buildings/frame.hpp"
#include "city/lots.hpp"
#include "core/geom2d.hpp"
#include "core/math.hpp"
#include "core/rect.hpp"

namespace svx::city {

// APRON: the level apron round a building's footprint (voxels: vx(1.5)).
constexpr double kApron = 12;

// levelLot(lot, env): is the lot kept as one level terrace (a yard, a car park, a school, a wharf
// house, a petrol forecourt)? (env null: no.)
bool level_lot(const Lot& lot, const Envelope* env);

// The pads of a cell's buildings and the level lots, looked up by position.
class SiteGrading {
 public:
  // lots: the cell plan's lots (SiteGrading keeps pointers to the level ones: JS keeps the lot
  // objects, and `at` compares a lot by identity - the lots must stay where they are);
  // building_by_id: its envelopes by id (null: none).
  SiteGrading(const std::vector<Lot>& lots, const std::function<const Envelope*(const std::string& id)>& building_by_id);

  // Graded ground (voxels, the top of the ground) at world column (x, y) of block `block_id` (only
  // its own buildings pull; a street lies between it and the others): `base` is the block surface
  // there (or the natural ground), `lot` the lot the column is on, if any.
  double at(double x, double y, double base, const std::string& block_id, const Lot* lot = nullptr) const;

 private:
  // A pad's rect: a world rect, or (a turned building) a canonical rect of its frame, measured in its
  // own axes.
  struct PadRect {
    Rect r{};
    bool turned = false;
  };
  struct Pad {
    double level = 0;
    std::vector<PadRect> rects;
    std::optional<Frame> frame;  // (a turned building's)
    double apron = 0;
    std::string block;
  };
  SpatialGrid<uint32_t> grid_{128};
  std::vector<Pad> pads_;
  std::unordered_set<const Lot*> levels_;
};

}  // namespace svx::city
