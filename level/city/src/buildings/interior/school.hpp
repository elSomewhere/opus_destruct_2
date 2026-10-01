// svx_city — schools (voxel_city buildings/interior/school.js): a double-loaded corridor along the
// building with a stair core at each end. Ground floor: the entrance lobby on the street side with
// the office and restrooms beside it, a gym (two bays wide, with a door to the schoolyard), the
// cafeteria and its kitchen, classrooms. Upper floors (one shared grid): classrooms, a library, a
// teachers' room and restrooms.
#pragma once

#include <array>
#include <vector>

#include "buildings/archetypes.hpp"
#include "buildings/interior/plan.hpp"
#include "core/hash.hpp"

namespace svx::city {

// planSchool({env, rng, pb}).
void plan_school(const Envelope& env, Rng& rng, PlanBuilder& pb);

// endSafe(pieces, minEnd): merges end pieces that would not reach past the stair core to the
// corridor into their neighbours (school.js and civic.js each have one, the same).
std::vector<std::array<double, 2>> end_safe(const std::vector<std::array<double, 2>>& pieces, double min_end);

}  // namespace svx::city
