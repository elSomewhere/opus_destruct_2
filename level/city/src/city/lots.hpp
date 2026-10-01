// svx_city — a lot: one building plot of a block (voxel_city city/lots.js's lot records, as the
// cell plan completes them: city/cellPlan.js's lotRec).
//
// The record only, for now: the fields the building archetypes read (buildings/archetypes.hpp:
// planBuildingEnvelope and the envelopes; buildings/sample.hpp). The port of city/lots.js and
// city/cellPlan.js adds the planners that make lots and the rest of the record's fields (block,
// cell, frontages, alley, whole, arch, cuts, poly, edge, levelAt, underHighway, wharf, site ...).
#pragma once

#include <optional>
#include <string>

#include "buildings/frame.hpp"
#include "core/rect.hpp"

namespace svx::city {

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
};

}  // namespace svx::city
