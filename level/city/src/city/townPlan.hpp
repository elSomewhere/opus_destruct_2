// svx_city — town plans (voxel_city city/townPlan.js): the landmarks that give a town its
// character, placed once per settlement from its centre, size, harbour and industrial side:
//
//   square      the market square (torg) near the centre, towards the harbour on an island
//   church      the main church on its own block near the centre, on the highest ground of a few
//               candidates (churches like hills)
//   cemetery    on the edge of the town, away from the works and the harbour
//   wharf       gabled warehouses along the harbour front (island towns)
//   park, school, sports
//   allotments  garden plots with huts on the fringe
//   garages     rows of lock-up garages by the bleak estates and the works
//   wasteland   a vacant lot of rubble and scrub by the works
//   civic       a town hall by the square, a museum, galleries, a concert hall (a house of culture
//               in Soviet and Karelian towns), cinemas, libraries, hotels and a department store
//               round the centre, police and fire stations, hospitals (polyclinics), supermarkets
//               further out, petrol stations on the main roads at the edge, a market hall in a
//               harbour town, music clubs (buildings/civic)
//
// A landmark is a point with a kind; the nearest block that suits it (by district and size, from
// the stage-1 cell networks, so there is no cycle with the cell plans) takes that use
// (landmark_use). Everything is a pure function of the settlement record, cached on it
// (Settlement::plan) - the anchors when the plan is made, the blocks they land on when first
// asked (the whole town at once, in order of importance).
#pragma once

#include <string>
#include <vector>

#include "core/cache.hpp"
#include "core/rect.hpp"

namespace svx::city {

class World;
struct Settlement;
struct Block;

// A landmark: its kind (a KINDS key: square, church, ..., or a civic archetype id), where it is
// (voxels, rounded) and its importance (0 first).
struct TownAnchor {
  std::string kind;
  double x = 0, y = 0, pri = 0;
};

struct TownPlan {
  // The landmarks in the order they were placed (only those on land). Villages get none.
  std::vector<TownAnchor> anchors;
  // The block (id) each anchor lands on, in anchor order ("": none - JS a.block null): the
  // nearest suitable block within reach that no more important landmark took. Resolved for the
  // whole town at once on first ask (JS resolveTown: plan.resolved, a.block).
  const std::vector<std::string>& blocks(const World& world) const;

 private:
  Lazy<std::vector<std::string>> blocks_;
};

// townPlan(world, s): the landmarks of settlement s (cached on it).
const TownPlan& town_plan(const World& world, const Settlement& s);

// LANDMARK_USE[kind]: the block program a landmark kind turns into (cellPlan's block use:
// "square", "church", ..., "civic:<id>"); "" for a kind it does not list.
std::string landmark_use_of(const std::string& kind);
// The landmark kinds, in LANDMARK_USE's key order (the plain kinds, then the civic buildings).
std::vector<std::string> landmark_kinds();

// landmarkUse(world, block): the landmark use of a block, or "" (null): the most important
// landmark of any town nearby that lands on it. cellPlan asks it with the block's property rect
// cleared of the streets (a copy of the block with another prop): pass that prop.
std::string landmark_use(const World& world, const std::string& block_id, const Rect& prop);
std::string landmark_use(const World& world, const Block& block);

}  // namespace svx::city
