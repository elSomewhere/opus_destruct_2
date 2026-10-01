// svx_city — a lot: one building plot of a block (voxel_city city/lots.js's lot records, as the
// cell plan completes them: city/cellPlan.js's lotRec).
//
// The record only, for now: the fields the building archetypes read (buildings/archetypes.hpp:
// planBuildingEnvelope and the envelopes; buildings/sample.hpp), the wings (buildings/wings.hpp:
// block, frontages, whole) and the site grading (city/grading.hpp: underground, underHighway).
// The port of city/lots.js and city/cellPlan.js adds the planners that make lots and the rest of
// the record's fields (cell, alley, arch, cuts, poly, edge, levelAt, wharf, site ...).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "buildings/frame.hpp"
#include "core/rect.hpp"

namespace svx::city {

// A street a lot fronts (lots.js frontagesOf { side, cls }; cellPlan.js fitLots adds `slant` on
// a side cut back by a slanted street, which then fronts that street): its world side, the
// street's class, and the slanted street's block cut (its id: blockPoly's cut id, a road id or
// null).
struct LotFrontage {
  char side = 'N';
  std::string cls{};       // ("": null)
  bool slant = false;      // (JS: the key is set)
  std::optional<std::string> slant_id{};  // (the cut's id; none: null)
};

struct Lot {
  std::string id{};        // "<block id>/l<k>" (a civic lot's "<block id>/c")
  Rect rect{};             // its world rect (a turned lot: the bounds of its cut polygon)
  char front = 'S';        // the world side of its main street frontage (N, E, S, W)
  std::string district{};  // the id of its block's district
  bool corner = false;     // fronts two streets or more
  bool micro = false;      // a microdistrict's slab or tower plot (lots.js microLots)
  // A turned lot (the angled world): the turn of its frame, with its U x V (frame.hpp lot_frame_of).
  std::optional<Turn> turn{};
  // (the cell plan's lot record) the urbanization and downtown-ness at the lot, and its ground
  // level (voxels: the street's in front of its middle)
  double u = 0, core = 0, ground_z = 0;
  std::string building{};  // the id of the building standing on it ("": null)
  // ---- what the wings (buildings/wings) and the site grading (city/grading) read; the port of
  // lots.js and cellPlan.js sets them
  std::string block{};                  // its block's id
  std::vector<LotFrontage> frontages{};  // the streets it fronts (lots.js frontagesOf)
  // `whole`: lots.js's `whole: true` on a lot taking its block whole (a downtown block, a single
  // lot), or the lot as planned before cellPlan.js fitLots trimmed it to its block's slanted
  // edges (a rect: whole_rect). JS reads `lot.whole ?? lot.rect` as a rect: `true` has no x0.
  bool whole = false;
  std::optional<Rect> whole_rect{};
  bool underground = false;    // a site's lot under the surface (sites/mountainBase.js)
  bool under_highway = false;  // a highway's corridor crosses it (cellPlan.js underHighway)
};

}  // namespace svx::city
