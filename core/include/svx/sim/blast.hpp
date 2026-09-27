// structvox — gameplay blast: prefracture + radial impulse (port of the prototype's
// _applyBlastMutation, core.js:6132-6300, and applyPendingImpulses,
// oriented_interface_core_base.js:1205).
//
// With r the blast radius and positions the current cell centres:
//   - cells with |x - c| <= r are removed (removeCore);
//   - bonds whose midpoint is within r + pad are fractured (pad = 0.4 cells);
//   - bonds whose midpoint is within the damage radius dR = max(r + 0.75 cells, 2 r) get
//     d = max(d, peak * t), t = 1 - (dist - r) / (dR - r) (peak 0.85);
//   - a radial impulse acts on every cell within broad = 2.5 dR + h: load = scale * strength *
//     m * t^1.5 (t = 1 - dist / broad) along the radial direction, plus a torque of
//     0.15 * load * h about normalize(dir x z); strength = max(1, r) with r in cells.
// The prototype runs at 1 m cells; the cell-denominated constants keep the same rule at any h.
#pragma once

#include <array>
#include <vector>

#include "svx/solve/lattice.hpp"

namespace svx {

struct BlastParams {
  std::array<f64, 3> center{0, 0, 0};  // world (m); cell centre = h * p (+ u)
  f64 radius = 0.5;                    // m
  f64 damage_radius_scale = 2.0;
  f64 damage_radius_pad = 0.75;        // cells
  f64 fracture_padding = 0.4;          // cells
  f64 damage_peak = 0.85;
  bool remove_core = true;
  bool prefracture = true;             // false: impulse only (cohesive_load_only)
  f64 impulse_scale = 8.0;
  f64 impulse_strength = -1.0;         // < 0: max(1, r / h)
  f64 falloff_power = 1.5;
  f64 angular_strength = 0.15;
  f64 impulse_radius_scale = 2.5;
};

struct BlastResult {
  std::vector<i32> removed;    // cells, ascending index
  std::vector<i64> fractured;  // bond keys, lattice order
  std::vector<i64> damaged;    // bond keys whose damage increased
  std::vector<i32> seeds;      // survivors next to a change (connectivity seeds), sorted unique
  bool supports_removed = false;
  f64 damage_radius = 0.0;
};

// Cell centre (world, m) for state u (nullptr: rest).
std::array<f64, 3> cell_position(const Lattice& L, const f64* u, i32 i);

// Applies the prefracture to the lattice (removals, broken bonds, committed damage in L.dmg).
BlastResult apply_blast(Lattice& L, const f64* u, const BlastParams& bp);

// Adds the radial impulse loads (6 n generalized forces, applied for one step) into f.
void add_blast_impulse(const Lattice& L, const f64* u, const BlastParams& bp, f64* f);

}  // namespace svx
