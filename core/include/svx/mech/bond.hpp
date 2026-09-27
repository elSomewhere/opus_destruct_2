// structvox — face-bond section model and calibration (RBSM lattice).
//
// A bond joins two face-adjacent cells along lattice axis a. Its local frame is
//   n = e_a, t1 = e_{(a+1)%3}, t2 = e_{(a+2)%3}.
// Generalized jumps / forces use the component order used throughout structvox:
//   [0] N  (normal, along n)          [3] T  (torsion, rotation about n)
//   [1] V1 (shear along t1)           [4] M1 (bending, rotation about t1)
//   [2] V2 (shear along t2)           [5] M2 (bending, rotation about t2)
// NOTE: the prototype orders components [N, V1, V2, M1, M2, T]; convert with
// proto_to_svx_order() when comparing against fixtures.
//
// Calibration is a port of the prototype's continuum_to_voxel.js
// (calibrateInterfaceArchetypeFromContinuum): half-cell series stiffness, minimum-capacity
// strength, onset = cap/k * kappa_onset, break = max(onset * kappa_break/kappa_onset,
// 2 G_f A_min / cap). Two deliberate fixes (plan §A1/§B1), both switchable:
//   * bending section properties are assigned to the correct local axes for every lattice
//     axis (the prototype swaps them on Y edges);
//   * torsion uses the St-Venant constant of the rectangular section, not the polar moment.
#pragma once

#include <array>

#include "svx/base/types.hpp"
#include "svx/mech/material.hpp"

namespace svx {

using Vec6 = std::array<f64, 6>;

enum class Profile : u8 { Solid = 0, Monolithic, WeakJoint, Brace, Count };

struct ProfileParams {
  const char* name;
  f64 axial, shear, bend, torsion;  // stiffness continuity scales
  f64 strength;                     // capacity (and fracture energy) continuity scale
  f64 residual;                     // residual stiffness ratio r after full damage
  f64 kappa_on, kappa_br;           // onset / break reserve factors (prototype)
};

const ProfileParams& profile_params(Profile p);
Profile profile_from_name(const char* name, bool* ok = nullptr);

struct SectionFixes {
  bool fix_y_axes = true;      // correct bending-axis assignment on Y edges
  bool st_venant_j = true;     // St-Venant torsion constant instead of polar moment
};

// Constitutive calibration mode (plan §B1).
struct LawParams {
  // Reference: the prototype law bit-for-bit (onset at kappa_on x capacity).
  // Game: fracture-energy consistent with the cell size, onset scaled by `fragility`
  // (onset = cap/k * fragility * kappa_on_game), and a ductile reserve via the break ratio.
  bool game = false;
  f64 fragility = 1.0;       // game mode: capacity multiplier (<1 = weaker)
  f64 kappa_on_game = 1.0;   // game mode: onset reserve factor
  // Game mode: minimum softening ductility break/onset. When a cell is too large for its
  // fracture energy (2 w / P < ratio * P / k, a snap-back) the peak is lowered to
  // P = sqrt(2 w k / ratio), so the dissipated work stays exactly w = G_f A at any h.
  f64 min_ductility = 1.5;
  // Game mode: fracture-energy multiplier. A design strength class (capacity x m) sets it to m^2,
  // so that the whole softening curve scales by m - onset and break alike, the fracture-energy
  // cap on the peak included (with capacity alone, strengthening past that cap did nothing).
  f64 toughness = 1.0;
};

// Cross-section of one cell perpendicular to lattice axis a.
struct Section {
  f64 A, As;        // area, shear area (5/6 A)
  f64 I1, I2;       // second moments for bending about t1 and about t2
  f64 J;            // torsion constant
  f64 c1, c2;       // extreme-fibre distances for bending about t1 / t2
  f64 rt;           // torsion radius
};

// effDims are fractions of the lattice pitch h (full voxel = {1,1,1}).
Section cell_section(int axis, const std::array<f64, 3>& effDims, f64 h, const SectionFixes& fixes);

// Saint-Venant torsion constant of a solid a x b rectangle.
f64 st_venant_rect(f64 a, f64 b);

struct BondCapacity {
  f64 Nt, Nc, V, T, M1, M2;
};

// Onset / break thresholds on the elastic jump (m or rad), svx order with the axial
// component split into opening (tension) and closing (compression).
struct BondThresholds {
  f64 open, comp;   // axial
  Vec6 comp6;       // [unused(0), V1, V2, T, M1, M2]
};

struct BondModel {
  Vec6 k;                 // stiffness per component
  BondCapacity cap;       // nominal capacities
  BondThresholds onset;   // damage onset
  BondThresholds brk;     // rupture (progress = 1)
  f64 residual = 0.08;    // secant floor r
  f64 area_min = 0.0;     // interface area (min of the two sections)
  Profile profile = Profile::Solid;
};

// Bond between cell A and cell B across lattice axis `axis`.
BondModel make_bond(int axis, MaterialId matA, const std::array<f64, 3>& effA, MaterialId matB,
                    const std::array<f64, 3>& effB, f64 h, Profile profile, const SectionFixes& fixes,
                    const LawParams& law = LawParams{});

// Default profile choice for a material pair when archetype kinds are unknown
// (different materials -> WEAK_JOINT, same -> SOLID).
Profile default_profile(MaterialId a, MaterialId b);

// Nominal utilization of a bond force vector F (svx order) against its capacity:
// max over components, tension/compression distinguished on N.
f64 bond_utilization(const Vec6& F, const BondCapacity& cap);

// Convert a 6-vector from prototype order [N, V1, V2, M1, M2, T] to svx order and back.
inline Vec6 proto_to_svx_order(const Vec6& p) { return {p[0], p[1], p[2], p[5], p[3], p[4]}; }
inline Vec6 svx_to_proto_order(const Vec6& s) { return {s[0], s[1], s[2], s[4], s[5], s[3]}; }

}  // namespace svx
