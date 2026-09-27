// structvox — telescoping composite ("event bubble") of one structure (plan §B4/B5).
//
// Partition: free cells within R0 (cells) of the event centres stay fine (level 0); farther
// out, 2^l blocks whose cells all lie beyond R0 g^(l-1) become rigid aggregates (connected
// components of the block through intact bonds, as the multigrid hierarchy); the rest of the
// structure ends at level Lmax. Cells with partial supports always stay fine.
//
// Operator: Galerkin projection of the fine bonds onto the aggregates' rigid motions, with
// every bond scaled by 2 / (n_i + n_j) where n is the fibre length of its endpoint node along
// the bond axis (exact RBSM at the coarse pitch for prismatic members) and a St-Venant torsion
// correction per coarse face (rigid aggregates otherwise restrain warping; research/mlexp
// hierarchy.py). The mass is the exact Galerkin rigid-body mass about each node's mass
// centroid (block diagonal). The system is SPD without phantom supports.
//
// State: 6 DOFs per node (translation of the node reference point, rotation vector). Fine
// displacement of an aggregate member: u = U + Theta x (x_c - X), theta = Theta.
#pragma once

#include <array>
#include <span>
#include <vector>

#include "svx/solve/lattice.hpp"
#include "svx/solve/multigrid.hpp"

namespace svx {

struct CompositeOptions {
  f64 R0 = 16.0;        // fine radius (cells)
  f64 grading = 2.0;    // g: level l starts at R0 g^(l-1)
  int max_level = 4;    // Lmax
  bool scaling = true;  // fibre-length scaling of the bonds
  bool torsion = true;  // St-Venant torsion correction of coarse faces
  bool fine_bonds = true;  // assemble fine-fine bonds into A (false: the caller evaluates them,
                           // e.g. nonlinearly on a sub-lattice; their pattern entries remain)
  // Assemble node blocks from per-face spring moments (exact; O(1) per bond) instead of a 6x6
  // Galerkin product per bond (the reference, kept for tests).
  bool moment_assembly = true;
  std::vector<i32> force_fine;  // cells that must stay fine (event cells' neighbours)
};

struct Composite {
  i32 n = 0;                                // nodes
  i32 n_fine = 0;                           // level-0 nodes
  std::vector<i32> node_of;                 // per lattice cell (-1: anchored / dead)
  std::vector<u8> level;                    // per node
  std::vector<i32> cell;                    // per node: its lattice cell (level 0), else -1
  std::vector<i32> mptr, members;           // CSR: member cells per node
  std::vector<f64> X;                       // 3 per node: reference point (mass centroid, m)
  std::vector<f64> off;                     // 3 per lattice cell: x_c - X_node (m)
  std::vector<u8> fix;                      // per node Dirichlet mask (fine nodes only)
  std::array<std::vector<f32>, 3> scale;    // per (+axis) bond slot: fibre scaling (1 fine-fine)
  Bsr6 A;                                   // stiffness (fixed DOFs: identity rows / cols)
  std::vector<f64> M;                       // 36 per node: Galerkin mass
  std::vector<std::array<i32, 3>> blk;      // per node: cell-unit block (multigrid coarsening)
  i64 crossing_bonds = 0;                   // bonds assembled between different nodes
  // build time per phase (ms): levels, aggregates, members, fibres, pattern, Galerkin blocks,
  // torsion correction, mass
  std::array<f64, 8> ms{};
};

// Partition + assemble. centers: event centres in world metres.
Composite build_composite(const Lattice& L, std::span<const std::array<f64, 3>> centers, const CompositeOptions& opt);

// Fine displacement field (6 n, zero on anchored / dead cells) of composite state xc.
void composite_prolong(const Lattice& L, const Composite& C, const f64* xc, f64* u);
// Composite forces P^T f of fine generalized forces f (6 n).
void composite_restrict(const Lattice& L, const Composite& C, const f64* f, f64* fc);
// y = A x + mass_shift M x (fixed DOFs pass through).
void composite_apply(const Composite& C, const f64* x, f64* y, f64 mass_shift = 0.0);

// Adds the stiffness of every bond of sub-lattice S (current frames and damage of S.F) into A
// (a copy of C.A with the same pattern). S.F's live free cells must be fine nodes of C.
void add_sublattice_bonds(const Composite& C, const SubLattice& S, Bsr6& A);

// Multigrid preconditioner over the assembled composite (level 0 = the composite itself).
void build_composite_mg(const Composite& C, Multigrid& mg, const MGOptions& opt);

}  // namespace svx
