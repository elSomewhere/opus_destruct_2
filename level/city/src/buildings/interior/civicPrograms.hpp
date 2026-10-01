// svx_city — room programs of the civic buildings, venues and big shops (voxel_city
// buildings/interior/civicPrograms.js): the planners are civic.hpp's, the envelopes
// buildings/civic.hpp's. A program lists which rooms go where; the planner fits them to the
// footprint. The tables are immutable once made (on first use, from any thread).
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "buildings/interior/civic.hpp"

namespace svx::city {

// HALL_PROGRAMS (supermarket, concertHall, musicClub, cinema, marketHall, houseOfCulture),
// CORRIDOR_PROGRAMS (hospital, polyclinic, policeStation, fireStation, museum, artGallery,
// library, townHall, hotel) and KIOSK_PROGRAMS (petrolStation), each in the reference's order.
const std::vector<std::pair<std::string, HallProgram>>& hall_programs();
const std::vector<std::pair<std::string, CorridorProgram>>& corridor_programs();
const std::vector<std::pair<std::string, KioskProgram>>& kiosk_programs();

// CIVIC_PLANNERS[id]: plans a civic envelope with its program's planner (and the department
// store's); false when the id has none.
bool has_civic_planner(const std::string& id);
bool run_civic_planner(const std::string& id, const Envelope& env, Rng& rng, PlanBuilder& pb);

}  // namespace svx::city
