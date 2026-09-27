// structvox — static equilibrium and closure (the quasi-static truth model, plan §B2).
//
// Equilibrium at full load is a non-accumulating secant fixed point: within a pass the bond
// damage is the trial value d = max(d_committed, D(p(u))) of the current iterate (never
// committed mid-pass), the operator is the secant stiffness K(d), and every iteration solves
// K(d) du = f - f_int(u; d) with multigrid-preconditioned CG. With linear kinematics that is
// exactly the Picard iteration u <- K(d(u))^-1 f; with corotational kinematics the same loop is
// a modified Newton iteration on the corotated residual. Under proportional softening the
// iteration increases monotonically to the first equilibrium on the load path, or runs the
// governing bonds to d = 1 when none exists (the structure fails).
//
// Closure (the oracle's solveToClosure protocol, cohesive_voxel_solver.js step()):
//   pass: equilibrium -> law sweep -> commit damage -> rupture candidates in oracle order
//   (score = max(d, margin, phi) desc, d desc, id asc; at most max_breaks per pass) ->
//   delete detached islands -> repeat until a quiet pass (converged, nothing ruptured or
//   deleted). Components without support are deleted in the first pass.
// Dynamic effects: a dynamic increase factor (DIF) amplifies the change of state caused by
// the previous pass's ruptures when the law is evaluated (u_eval = u_prev + DIF (u - u_prev)).
#pragma once

#include <vector>

#include "svx/solve/lattice.hpp"
#include "svx/solve/multigrid.hpp"
#include "svx/topo/connectivity.hpp"

namespace svx {

// Stable bond key: the external id when the lattice carries them, else 3 * lower cell + axis.
inline i64 bond_key(const Lattice& L, int axis, i32 i) {
  if (!L.ext_id[axis].empty() && L.ext_id[axis][i] >= 0) return L.ext_id[axis][i];
  return 3 * i64(i) + axis;
}

struct RuptureCandidate {
  i64 key = 0;
  i32 cell = 0;   // lower cell of the bond
  u8 axis = 0;
  f64 score = 0.0, damage = 0.0;
  // A cracked bond (contact) whose gap exceeds the separation limit: break it for good. Other
  // candidates rupture: with contacts enabled they crack (Lattice::crack_bond) when they fail
  // closed (shear, crushing); a bond failing open (tension) leaves a gap and breaks.
  bool separate = false;
  bool opening = false;   // the normal jump was opening when the bond failed
};

// Oracle ordering (score desc, damage desc, key asc).
void sort_candidates(std::vector<RuptureCandidate>& c);

// Committed damage of every bond slot (same layout as Lattice::dmg).
using DamageField = std::array<std::vector<f32>, 3>;

struct LawSweep {
  f64 max_damage = 0.0;
  f64 max_phi = 0.0;
  f64 max_change = 0.0;   // max |d_new - d_previous_trial| (fixed-point convergence)
  i64 damaged = 0;        // bonds with d > 0
  std::vector<RuptureCandidate> candidates;  // unsorted
};

// Evaluates the law on every bond for state u: writes the trial damage max(dc, D(p)) into
// L.dmg (L.enable_damage() must have been called) and returns the candidates. The law sees
// the jump scaled by `strain_scale` (the compliance normalization, = L.kscale by default).
// demand_scale multiplies the law's demand of intact bonds only (a bubble's near-field
// inflation, plan §B6); contacts keep their mechanics.
LawSweep sweep_law(Lattice& L, const f64* u, const DamageField& dc, bool corot, f64 strain_scale, f64 demand_scale = 1.0);

// Per-component contact secants of a cracked bond (svx order N, V1, V2, T, M1, M2; the floor
// kMinSecant everywhere when open): the jump (m / rad, unscaled) gives elastic contact forces
// (scaled like the law's strains); friction caps |V|, |T| at mu |N| (torsion arm a / 3),
// rocking caps |M| at |N| a / 2 (a: side of the contact face).
Vec6 contact_scales(const Lattice& L, int axis, i32 i, const Vec6& jump, f64 strain_scale);

struct StaticsOptions {
  f64 g = 9.81;              // gravity (m/s^2, acting along -z)
  bool corot = true;         // corotational kinematics (linear otherwise)
  int max_passes = 96;       // closure passes
  int max_iters = 200;       // secant / modified-Newton iterations per equilibrium
  f64 res_tol = 1e-9;        // ||f - f_int|| / ||f||
  f64 res_accept = 1e-7;     // also converged once below this and no longer improving (the
                             // floating-point floor of stiff structures: rel. strain ~1e-6)
  f64 damage_tol = 1e-7;     // max trial-damage change between iterations
  f64 lin_rtol = 1e-4;       // inexact linear solves (relative to the current residual)
  int lin_maxit = 400;
  int max_breaks = 512;      // ruptures per pass
  f64 dif = 1.0;             // dynamic increase factor for cascade passes (1 = static)
  bool event = false;        // the input state predates a sudden change (carve / blast):
                             // the first pass is amplified by the DIF as well
  bool damage = true;        // evaluate the law (false: elastic equilibrium only)
  int mg_rebuild_iters = 80; // rebuild the preconditioner when a solve needs more
  int candidate_grace = 24;  // stop iterating this many iterations after a bond reached
                             // rupture (the state beyond is a mechanism, not an equilibrium)
  f64 frame_threshold = 0.01;   // rad: corotated tangent once any cell rotates more than this
  f64 frame_rebuild = 0.05;     // rad: rebuild the preconditioner when rotations drift this much
  f64 max_translation_increment = 0.25;  // cells per iteration (corotational trust region)
  f64 max_rotation_increment = 0.2;      // rad per iteration
  bool verbose = false;                  // per-iteration trace on stdout
  int max_pcg_total = 0;                 // > 0: stop (unconverged) once the linear solves took this many
  // Anderson acceleration of the secant fixed point (plan §B2; Walker & Ni): the step is mixed
  // with the last `anderson` steps (least squares on their differences); the history restarts
  // when the residual grows. 0 = plain secant iteration. (Depth 3: 37% fewer iterations on the
  // oracle fixtures at identical parity; converges on marginal scenes the plain iteration
  // cannot - docs/STATUS.md.)
  int anderson = 3;
  MGOptions mg{};
};

struct EquilibriumStats {
  bool converged = false;
  int iters = 0;
  int anderson_restarts = 0;
  int pcg_iters = 0;
  int mg_builds = 0;
  f64 residual = 0.0;        // final relative residual
  f64 max_damage = 0.0;
  f64 max_phi = 0.0;
  // wall-clock profile (diagnostics only; never feeds a decision)
  f64 ms_law = 0.0, ms_forces = 0.0, ms_build = 0.0, ms_pcg = 0.0;
};

// Equilibrium of the loads f_ext (6 n) with committed damage dc; u holds the initial guess
// and returns the solution; L.dmg returns the trial damage at the solution. `mg` may be
// reused across calls (it is (re)built as needed; pass nullptr for a private one).
EquilibriumStats solve_equilibrium(Lattice& L, std::vector<f64>& u, const std::vector<f64>& f_ext,
                                   const DamageField& dc, const StaticsOptions& opt, Multigrid* mg = nullptr);

struct PassLog {
  int pass = 0;
  EquilibriumStats eq;
  i32 ruptured = 0;
  i32 deleted = 0;
  bool quiet = false;
};

struct ClosureResult {
  bool converged = false;             // reached a quiet pass
  int passes = 0;                     // passes up to (and including) the quiet pass
  bool all_equilibria_converged = true;
  std::vector<i64> ruptured;          // bond keys, in rupture order
  std::vector<i32> deleted;           // cells deleted as detached, in deletion order
  std::vector<PassLog> log;
  ConnStats conn;
};

// Gravity load vector for the lattice (free DOFs only).
std::vector<f64> gravity_vector(const Lattice& L, f64 g);

// Runs passes until closure. u: initial guess in / final state out (size 6 n).
ClosureResult solve_to_closure(Lattice& L, std::vector<f64>& u, const std::vector<f64>& f_ext,
                               const StaticsOptions& opt);

// Removes the cells of the given islands (sorted, deterministic) and zeroes their state.
void delete_islands(Lattice& L, const std::vector<std::vector<i32>>& islands, std::vector<f64>& u,
                    std::vector<i32>* deleted);

}  // namespace svx
