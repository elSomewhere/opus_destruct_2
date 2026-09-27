// structvox — self-weight buckling margin of a voxelized map's walls (plan §B3, §B10 step 5).
//
// Softening the physical stiffness by S_p lowers every Euler load by S_p while self-weight stays
// the same. Each free-standing vertical member (a structural run standing on anchored rock with
// no slab bracing its top) is treated as a self-weight cantilever strip of height H and
// thickness t (the shortest horizontal extent at mid-height):
//     H_cr = (7.837 E t^2 / (12 rho g))^(1/3),   ratio(S_p) = S_p (H / H_cr)^3
// The margin S_p · P / P_cr <= 0.3 then caps S_p at 0.3 / (H / H_cr)^3. The strip model is
// conservative (walls are plates with braced edges). v1 drops geometric stiffness, so nothing
// buckles in the simulation: the cap keeps tall slender walls from looking implausibly soft.
#pragma once

#include "svx/material/material.hpp"
#include "svx/game/columns.hpp"

namespace svx::doom {

struct SlendernessReport {
  i64 free_members = 0, braced_members = 0;
  f64 height_p50 = 0.0, height_max = 0.0;  // m, free members
  // largest S_p keeping the ratio <= 0.3 for 99% / 99.9% / all free members (1e9: no free member)
  f64 max_compliance_p99 = 1e9, max_compliance_p999 = 1e9, max_compliance_all = 1e9;
};

SlendernessReport wall_slenderness(const ColumnGrid& g, const Material& m, f64 h);

}  // namespace svx::doom
