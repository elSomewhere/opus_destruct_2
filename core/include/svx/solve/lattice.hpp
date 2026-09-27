// structvox — fine RBSM lattice (compact representation) and its matrix-free operator.
//
// Every cell carries 6 DOFs [ux, uy, uz, thx, thy, thz] (small-displacement statics).
// Anchored cells are Dirichlet (their DOFs are identically zero and never updated).
// Partial supports (e.g. pinned: translations fixed, rotations free) use a per-cell DOF
// mask; support springs act on the cell centre in world DOFs.
// Bonds are implicit between face neighbours; bond models are deduplicated in a small
// table (bonds between identical cell types share one model).
#pragma once

#include <array>
#include <span>
#include <vector>

#include "svx/base/types.hpp"
#include "svx/mech/archetype.hpp"
#include "svx/mech/bond.hpp"
#include "svx/mech/law.hpp"

namespace svx {

struct CellIn {
  std::array<i32, 3> p;                       // integer lattice coordinates
  MaterialId mat = MaterialId::Concrete;
  bool anchored = false;                      // all six DOFs fixed
  std::array<f64, 3> eff = {1.0, 1.0, 1.0};   // effDims (fractions of h)
  u8 fixmask = 0;                             // partial Dirichlet: bit q fixes DOF q
  Vec6 spring = {0, 0, 0, 0, 0, 0};           // support springs (world DOFs, cell centre)
  i16 arch = -1;                              // archetype id (overrides mat/eff), -1 = none
  u8 strength = 0;                            // design strength class (capacity multiplier, bonds
                                              // take the weaker cell's class); stiffness unchanged
  // plate class (PlateParams): 1 + plate normal axis, 0 none, kPlateAuto: classified from the
  // input cells. (A window of a larger world classifies from the world: its own cells end at
  // its boundary, and its pinned rim is no support there.)
  u8 plate = 0xFF;
};
inline constexpr u8 kPlateAuto = 0xFF;

// Capacity multiplier of a design strength class: 1, 1.5, 2, 3, 4, 6, 8, 12, 16, ... 1024 (the
// class also scales the fracture energy by its square: LawParams::toughness).
inline constexpr int kStrengthClasses = 21;
f64 strength_multiplier(u8 cls);
u8 strength_class_for(f64 multiplier);  // smallest class whose multiplier >= the argument

constexpr u8 kAllDofs = 0x3F;

// Unilateral contact of cracked bonds (plan §B7): a ruptured bond between two live cells can
// stay a contact instead of a gap (see Lattice::crack_bond).
struct ContactParams {
  bool enabled = false;
  f64 friction = 0.6;     // Coulomb: |V| <= mu |N|, |T| <= mu |N| a / 3
  f64 separation = 0.25;  // a crack separates for good beyond this gap or face-edge opening
                          // (fraction of h), or after sliding h / 2
};

struct Lattice {
  f64 h = 0.125;
  i32 n = 0;
  std::vector<std::array<i32, 3>> p;
  std::vector<MaterialId> mat;
  std::vector<u8> anchored;               // 1 iff fixmask == kAllDofs
  std::vector<u8> fixmask;                // per-cell Dirichlet DOF mask
  std::vector<Vec6> spring;               // support springs; empty if none in the lattice
  std::vector<std::array<f64, 3>> eff;
  std::array<std::vector<i32>, 3> nbr;    // +axis neighbour index or -1 (no bond)
  std::array<std::vector<i32>, 3> nbrm;   // -axis neighbour index or -1
  std::array<std::vector<u16>, 3> bid;    // model id of the (+axis) bond of each cell
  std::vector<BondModel> models;
  std::vector<f64> mass;
  std::vector<std::array<f64, 3>> inertia;  // principal local inertia (box)
  std::vector<i16> arch;                  // archetype id per cell (-1 = none)
  std::vector<u8> strength;               // design strength class per cell
  std::vector<u8> plate;                  // 1 + plate normal axis (0: not in a plate); may be empty
  std::vector<u8> dead;                   // removed cells (inert; never supports)
  std::array<std::vector<f32>, 3> dmg;    // committed bond damage (empty = undamaged)
  std::array<std::vector<i32>, 3> ext_id; // optional external bond ids (rupture ordering)
  f64 kscale = 1.0;                       // global stiffness multiplier (1/S_p compliance)
  SectionFixes fixes{};
  LawParams law{};
  // Cracked bonds (unilateral contacts): kept in nbr (the pieces still touch); instead of the
  // material law they carry per-component contact secants of the current state (cscale):
  // normal stiffness while the crack is closed, shear / torsion capped by Coulomb friction,
  // bending by rocking (|M| <= |N| a / 2), the floor on every component while it is open.
  // sweep_law reports a separation candidate once the gap exceeds contact.separation h; only
  // then is the bond broken for good.
  ContactParams contact{};
  std::array<std::vector<u8>, 3> cracked;   // empty = none
  std::array<std::vector<Vec6>, 3> cscale;  // contact secants of cracked bonds
  bool is_cracked(int axis, i32 i) const { return !cracked[axis].empty() && cracked[axis][i] != 0; }
  void crack_bond(i32 i, int axis);
  // Optional linearization frames (corotational tangent): per (+axis) bond slot, kFrameSize
  // values = bond rotation R_mid (3x3 row-major; columns = local axes in world) and the
  // current lever arms r_i, r_j (world). Empty = rest configuration. When present,
  // apply_stiffness / stiffness_diag_blocks / the multigrid use the corotated tangent
  // K_t = sum B^T k B (material part; no geometric stiffness).
  // Optional prescribed displacement of anchored cells (6 per cell; empty = all zero): used
  // when evaluating internal forces, bond jumps and the law, never by the (increment)
  // operators, whose anchored increments are zero. Windows pin their rim at the cached
  // baseline this way, so totals stay exact while the increments are local.
  std::vector<f64> u_fixed;
  static constexpr int kFrameSize = 15;
  std::array<std::vector<f64>, 3> frame;
  bool framed() const { return !frame[0].empty(); }

  const BondModel& bond(int axis, i32 i) const { return models[bid[axis][i]]; }
  // Effective stiffness multiplier of the (+axis) bond of cell i: kscale * s(d) (a cracked
  // bond: its torsion secant; bond_scale6 / bond_k give every component).
  f64 bond_scale(int axis, i32 i) const {
    if (is_cracked(axis, i)) return kscale * cscale[axis][i][3];
    return dmg[axis].empty() ? kscale : kscale * secant_scale(models[bid[axis][i]], dmg[axis][i]);
  }
  Vec6 bond_scale6(int axis, i32 i) const {
    if (is_cracked(axis, i)) {
      Vec6 s = cscale[axis][i];
      for (auto& v : s) v *= kscale;
      return s;
    }
    const f64 s = bond_scale(axis, i);
    return {s, s, s, s, s, s};
  }
  Vec6 bond_k(int axis, i32 i) const {
    Vec6 k = models[bid[axis][i]].k;
    const Vec6 s = bond_scale6(axis, i);
    for (int q = 0; q < 6; ++q) k[q] *= s[q];
    return k;
  }
  // A support is any live cell with a Dirichlet DOF or a support spring.
  bool support(i32 i) const;
  void enable_damage();
  // Remove a cell: all its bonds break, it becomes inert (no DOFs, not a support).
  void remove_cell(i32 i);
  i32 num_alive() const;
  bool partial(i32 i) const { return fixmask[i] != 0 && fixmask[i] != kAllDofs; }
  // Copies u_i with Dirichlet DOFs zeroed into out (returns out), or returns u_i directly.
  const f64* masked(const f64* u, i32 i, f64* out) const;
  i32 num_free() const;
  i64 num_bonds() const;
  // Rupture the bond between cell i and its +axis neighbour.
  void break_bond(i32 i, int axis);
};

// Plate action (plan Phase 7 "Poisson and plate capacity factors"): cells of thin, wide
// members (slabs, walls: at most max_thickness cells through, at least min_width cells in both
// in-plane directions, same material, not anchored) form plates. Their in-plane bonds get the
// plate stiffness 1 / (1 - nu^2) (Poisson coupling a beam lattice lacks) and a capacity factor
// for two-way load redistribution (yield-line reserve of a slab over its first-cracking load).
struct PlateParams {
  bool enabled = false;
  i32 max_thickness = 4;
  i32 min_width = 8;
  f64 poisson = 0.2;
  f64 capacity = 1.4;
};

struct LatticeOptions {
  f64 h = 0.125;
  SectionFixes fixes{};
  LawParams law{};
  ContactParams contact{};
  PlateParams plate{};
};

// Builds the lattice; face bonds are created between all face-adjacent input cells
// except anchored-anchored pairs (irrelevant to the solve).
Lattice build_lattice(std::span<const CellIn> cells, const LatticeOptions& opt);

// Sub-lattice of `cells` (live cells of L) plus their anchored neighbours as anchored ghosts:
// same bond models, damage, compliance and law; only bonds between included cells are kept.
struct SubLattice {
  Lattice F;
  std::vector<i32> to_world;  // F cell -> L cell
};
SubLattice extract_sublattice(const Lattice& L, std::span<const i32> cells);

// y = K u (parallel, deterministic). u and y have 6*n entries; anchored rows of y are
// zero and anchored entries of u are ignored (treated as zero).
void apply_stiffness(const Lattice& L, const f64* u, f64* y);

// Gravity load f (6*n): -m g on uz of free cells, zero elsewhere.
void gravity_load(const Lattice& L, f64 g, f64* f);

// 6x6 diagonal blocks of K (row-major, 36 per cell). Anchored cells get identity.
void stiffness_diag_blocks(const Lattice& L, f64* D);

// Bond-side kinematic matrices: jump g (svx order) = Bi * u_i + Bj * u_j for a bond
// along `axis` with half-pitch hh = h/2. Row-major 6x6.
void bond_side_matrices(int axis, f64 hh, f64 Bi[36], f64 Bj[36]);

// Linearized bond-side matrices about a frame (Lattice::frame layout: R_mid, r_i, r_j).
void bond_side_matrices_framed(int axis, const f64* frame, f64 Bi[36], f64 Bj[36]);

// Generalized bond forces F = k .* g for every bond along `axis`, written at the lower
// cell's slot (F has 6*n entries; slots without a bond are zeroed).
void bond_forces(const Lattice& L, int axis, const f64* u, f64* F);

// Force vector of one bond (i -> j = nbr[axis][i]) for displacement u (svx order).
Vec6 bond_force(const Lattice& L, int axis, i32 i, const f64* u);

// Event residual after removing cells: r = f - K_post u0 = sum over bonds between a removed
// cell and a surviving cell of that bond's contribution to (K u0) at the surviving cell.
// `removed` has one flag per cell; r (6*n) is overwritten (zero at removed cells).
void released_bond_residual(const Lattice& L, const std::vector<u8>& removed, const f64* u0, f64* r);

}  // namespace svx
