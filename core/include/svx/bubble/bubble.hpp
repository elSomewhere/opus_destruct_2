// structvox — event bubble: implicit dynamics of the event delta on a telescoping composite
// (plan §B2 baseline + delta, §B5 bubbles, §B6 "fine level decides").
//
// The structure's exact fine baseline u0 is in equilibrium before the event; the event (a carve,
// a blast prefracture, an impact) releases the residual r at its neighbours. The bubble solves
// for the event delta x on the composite (fine within R0, rigid aggregates outward):
//   M_c x'' + C x' + K_coarse x + N(x) = P^T (r + f_event(t)),
//   N(x) = P_f^T [F_ff(u0 + x) - F_ff(u0)]
// where K_coarse holds every bond touching an aggregate (scaled Galerkin, linear) and F_ff the
// fine-fine bonds evaluated nonlinearly (corotational, damage) on a sub-lattice of the fine
// region. The total state is u0 + P x everywhere — the far field is never pinned, so there are
// no phantom supports. Time stepping is the linearly implicit BDF2 of svx::Dynamics with a
// lagged multigrid preconditioner over the assembled composite. Only fine bonds are evaluated
// by the law (the fine level decides; failures elsewhere are caught by the background
// verification of the whole structure once the bubble sleeps).
#pragma once

#include <array>
#include <span>
#include <unordered_map>
#include <vector>

#include "svx/bubble/composite.hpp"
#include "svx/sim/dynamics.hpp"

namespace svx {

struct BubbleOptions {
  CompositeOptions comp{};
  f64 dt = 1.0 / 60.0;
  bool bdf2 = true;
  bool corot = true;
  f64 rayleigh_alpha = 0.5;
  f64 rayleigh_beta = 0.0;
  int newton_iters = 3;
  f64 newton_rtol = 0.1;
  f64 newton_atol = 1e-2;   // relative to the load scale (gravity of the fine region + event)
  // The residual form carries any unbalanced force into the next step, so the linear solves
  // need not be exact: 0.1% relative, or 0.1% of the load scale absolute. (A looser solve while
  // the window moves — lin_atol 1e-2, with lin_atol_quiet in the last quiet_tight_steps before
  // sleep — halves the PCG iterations with unchanged near-field peaks; kept off as the
  // conservative choice: damage is monotone, so solver error near the thresholds could ratchet
  // up (the prototype's XPBD failure mode). Measured over 10-minute sessions it changes the
  // ruptures by -29% / +59% for two seeds: no systematic effect seen.)
  f64 lin_rtol = 1e-3;
  f64 lin_atol = 1e-3;
  f64 lin_atol_quiet = 1e-3;
  int quiet_tight_steps = 2;  // (of sleep_steps)
  int lin_maxit = 60;        // (= mg_rebuild_iters: a solve that needs more marks the
                             // preconditioner stale; the step carries its residual to the next)
  int max_breaks = 512;
  bool damage = true;
  f64 sleep_velocity = 1e-2;
  int sleep_steps = 8;
  int max_steps = 1800;     // hard cap (30 s at 60 Hz)
  f64 frame_threshold = 0.01;
  f64 frame_rebuild = 0.05;
  int mg_rebuild_iters = 60;
  // Corotated reuse of the assembled operator: the corotated tangent of a rigidly rotated piece
  // is T A T^T, T = diag(R, R) per node, so between rebuilds the Krylov operator, the
  // preconditioner and the projection basis act in each fine node's rotation since the build.
  // Rigid rotation then needs no rebuild; the rebuild test is the relative rotation across
  // intact (uncracked) fine bonds exceeding frame_rebuild. Besides the cost (MAP01 under 10
  // rockets/s: 54 -> 30 ms per step, 1885 -> 765 rebuilds), the rotated Jacobian keeps Newton
  // converged where the stale one did not: replayed sessions against a tightly converged
  // reference show the unrotated default's extra collapses to be numerical (one seed: 10k
  // spurious ruptures in 80 s; the rotated default and the reference: 7).
  bool rotate_operator = true;
  // Near-field demand inflation (plan §B6): the composite's coarse far field is stiffer than
  // the fine structure, so near-field peaks come out low (column removal: 0.925-0.943 of the
  // full-fine truth; rocket craters in rooms: 0.97-1.05). The law on the bubble's fine bonds
  // sees its demand scaled by this factor (contacts excepted). 1.06 puts the column-removal
  // peak ratio at 0.98-1.0 (gate [0.95, 1.15]), but under sustained fire it doubles the
  // destruction (MAP01, 10 rockets/s: 7.0k -> 14.9k ruptures): damage is monotone and the
  // full-fine verification only adds failures the bubbles missed, so a uniform factor near
  // thresholds ratchets. Off until calibrated against outcomes, not the elastic peak.
  f64 demand_inflation = 1.0;
  // PCG operator: the assembled composite tangent the preconditioner was built on (SIMD BSR,
  // frames / damage as of the last rebuild: a modified-Newton Jacobian; the residual stays
  // exact) instead of the matrix-free fine sub-lattice at the current frames.
  bool assembled_operator = true;
  // Successive right-hand sides (Fischer 1998): the Krylov operator is fixed between
  // preconditioner rebuilds, so the steps' solutions span a basis (A-orthonormal, up to this
  // many) whose A-projection is the best initial guess; PCG solves the remainder to the same
  // tolerance. 0 = off.
  int projection_basis = 8;  // (at most 32)
  // Ruptures without detachment rebuild the preconditioner once this many accumulated (the
  // residual stays exact: until then the Krylov operator is a slightly stale tangent, a
  // modified Newton step; a detachment re-partitions and rebuilds at once). 1 = every rupture.
  int rebuild_ruptures = 1;
  // trust region of a Newton increment: cells move at most this far (voxels / rad) per
  // iteration (a near-mechanism step must not jump the state)
  f64 max_translation_increment = 1.0;
  f64 max_rotation_increment = 0.5;
  // Nominate / decide (plan §B6.1-2): every nominate_every steps the crossing bonds of coarse
  // aggregates are checked at a conservative bound - the baseline plus the aggregate's rigid delta
  // amplified by its level's stress-concentration factor (inside an aggregate the peak exceeds the
  // rigid estimate by up to 2x at level 1, 4x at 2, 8x at 3+) - and aggregates with a bond over
  // nominate_phi become fine cells, with a ring of neighbours (the local oversampled solve: the
  // fine law decides there from the next step). At most max_refinements times, while the fine
  // region stays under max_fine_cells.
  bool nominate = true;
  int nominate_every = 2;
  f64 nominate_phi = 0.5;
  int nominate_persist = 2;  // checks in a row an aggregate must be flagged (not a passing wave)
  // Decide (plan §B6.2): the nominated aggregates and the composite nodes around them, the
  // ring's outer layer held at the composite's motion, are solved statically at the fine level
  // (bounded by decide_max_pcg); only aggregates whose bonds there demand decide_phi of onset
  // become fine, one found safe is decided again once its bound has grown by decide_regrow.
  // Off: the patch's boundary data is the composite's motion, stiff-biased exactly where an
  // event changes the load path - on the column-removal library it misses every failure the
  // truth has (refining what is nominated matches 16 of 18), and in rocket bubbles its solves
  // cost what the smaller refinement saves (docs/STATUS.md). The fine law decides after
  // refinement instead.
  bool decide = false;
  f64 decide_phi = 0.9;
  f64 decide_regrow = 0.1;
  int decide_max_pcg = 120;
  int max_refinements = 3;  // (per second of bubble time, and per event merged into it)
  i32 max_fine_cells = 12000;
  // Marginal bonds (plan §B6.2-3): a fine bond within interface_reach cells (one aggregate) of
  // the fine/coarse interface decides no failure - the stiff coarse field next to it biases its
  // demand. A step in which one there reaches the marginal band (phi >= 1 - marginal_eps) is
  // solved again from the state before it, with the fine region grown by the aggregates around
  // it, and decided then. At most max_grows times, while the fine region stays under
  // max_fine_cells; where it cannot grow, the bond is decided where it is. marginal_eps < 0: off.
  f64 marginal_eps = 0.05;
  int interface_reach = 2;
  int max_grows = 8;  // (per second of bubble time, and per event merged into it)
  // Coarsening (plan §B5): a refined region that stays quiet - no damaged bond, every bond under
  // coarsen_phi (hysteresis below nominate_phi), its cells slower than sleep_velocity - for
  // coarsen_steps steps goes back into aggregates (momentum-exact rigid projection; its internal
  // deformation energy is dropped).
  bool coarsen = true;
  f64 coarsen_phi = 0.3;
  int coarsen_steps = 30;
  // Pieces standing only on cracks (plan §B7: failed-closed bonds are unilateral contacts, still
  // connected for detachment) that move off them faster than this (m/s, mass-weighted) are
  // released as detached pieces - a structure tipping about the crushed side of its hinge falls
  // as a rigid body, not through the composite's linear far field. Checked every
  // contact_check_every steps while new cracks wait (0: never).
  f64 contact_release_speed = 0.5;
  int contact_check_every = 6;
  MGOptions mg{};
};

class Bubble {
 public:
  // L: world lattice after the event mutation (removed cells dead, broken bonds gone, new
  // damage committed). u0: fine baseline (6 L.n). r: event residual at the survivors
  // (6 L.n, e.g. released_bond_residual before the mutation). centers: event centres (m).
  // force_fine: cells forced into the fine region (typically the event's neighbours).
  void init(Lattice& L, const std::vector<f64>& u0, const std::vector<f64>& r, std::span<const std::array<f64, 3>> centers,
            const BubbleOptions& opt);
  // One-step external force on a world cell (6), e.g. a blast impulse or an impact.
  void add_force(i32 world_cell, const f64* f6);
  StepStats step();
  // The preconditioner for the next step, built ahead of it (a setup stage of its own: the
  // step then finds it ready; the result is the same as step() alone).
  void prepare();
  bool asleep() const { return asleep_; }
  // A numerical breakdown (a non-finite residual or increment) stopped the bubble: its state is
  // not an equilibrium to keep (finalize drops it).
  bool failed() const { return failed_; }
  // Settle projection on the composite: the static equilibrium of the current state (no
  // inertia), with the law evaluated on the fine bonds (trial damage left in the lattice).
  // Returns the largest trial damage; the state (total_displacement) becomes the static one.
  struct SettleResult {
    bool converged = false;
    int iters = 0, pcg = 0;
    f64 max_damage = 0.0;
    std::vector<i32> candidate_cells;  // lower cells of bonds at rupture
  };
  // max_pcg > 0 bounds the linear work (near a mechanism the static iteration never converges:
  // every solve would run to its iteration cap).
  SettleResult settle(int max_iters = 12, int max_pcg = 0);
  i64 steps() const { return steps_; }

  // Total fine displacement u0 + P x on every world cell (6 L.n; dead / anchored = 0).
  void total_displacement(std::vector<f64>& u) const;
  const Composite& composite() const { return C_; }
  i32 nodes() const { return C_.n; }
  // thread CPU time of the last init (ms): composite partition and operator, its SIMD layout,
  // the fine sub-lattice, the baseline forces, the residual's restriction
  struct InitProfile {
    f64 composite = 0.0, simd = 0.0, sublattice = 0.0, forces = 0.0, restrict_residual = 0.0;
  };
  const InitProfile& init_profile() const { return init_prof_; }
  i32 fine_cells() const { return C_.n_fine; }
  int refinements() const { return refinements_total_; }  // nominate / decide re-partitions so far
  int grows() const { return grows_total_; }          // marginal-bond growths of the fine region
  i64 held_bonds() const { return held_; }            // marginal bonds that had a step solved again
  int coarsenings() const { return coarsenings_; }   // refined regions returned to aggregates
  // Diagnostics: the delta state's linear and angular momentum (about the origin) and kinetic
  // energy, sum over the composite nodes of M_k v_k.
  std::array<f64, 7> momentum() const;
  // the largest relative momentum change and kinetic-energy change of a re-partition so far
  // (plan §B5 gate: momentum within 1e-9, no energy gain)
  f64 repartition_momentum_error() const { return rep_dp_; }
  f64 repartition_energy_gain() const { return rep_dke_; }
  std::vector<DetachedIsland> take_islands();
  const std::vector<i64>& ruptured() const { return ruptured_; }
  // (lattice cell, axis) of every bond ruptured since the last call
  std::vector<std::pair<i32, int>> take_broken();
  // Bonds that cracked (unilateral contacts, lattice contact.enabled) since the last call.
  std::vector<std::pair<i32, int>> take_cracked();
  // Remove cells decided detached elsewhere (e.g. by a world-level connectivity check):
  // their bonds' loads are released like a detachment and the operator is rebuilt.
  void remove_cells(const std::vector<i32>& cells);
  // Add a persistent event residual (6 L.n; e.g. a second event inside this bubble).
  void add_residual(const std::vector<f64>& r);
  // An event inside the running bubble (plan §B5 merging: a second rocket on the structure it
  // spans), as the world took it. The cells it touches become fine first (the state projected
  // as for any refinement; they count toward no nomination budget), then its bonds break, its
  // damage rises, its cells go (a crater: no island record; `islands`: pieces the world already
  // detached) - every changed bond fine at both ends, so it releases its total force through the
  // sub-lattice as a rupture does - and its one-step forces act. Wakes the bubble; its step
  // limit counts from here.
  struct EventMutation {
    std::vector<i32> fine;                           // cells to make fine (the event's neighbours)
    std::vector<i32> removed;                        // cells the event removed
    std::vector<i32> islands;                        // cells of pieces it detached
    std::vector<std::pair<i32, int>> broken;         // bonds (lower cell, axis) it broke
    std::vector<std::tuple<i32, int, f32>> damaged;  // bonds (lower cell, axis) and their damage
    std::vector<std::pair<i32, std::array<f64, 6>>> forces;  // one-step forces
    bool empty() const {
      return fine.empty() && removed.empty() && islands.empty() && broken.empty() && damaged.empty() && forces.empty();
    }
  };
  void apply_event(const EventMutation& ev);
  // The step size from the next step on (a heavy bubble keeps pace with the world by longer
  // steps, Engine: max_dt_multiplier): BDF2 restarts, the preconditioner is rebuilt (its mass
  // shift). One-step forces keep their impulse (scaled by the step at init / the step).
  void set_time_step(f64 dt);
  f64 time_step() const { return opt_.dt; }
  Lattice& lattice() { return *L_; }
  i64 detached_cells() const { return detached_; }
  const ConnStats& conn_stats() const { return conn_; }

 private:
  // composite operator after topology changes (same labels); cells flagged in `drop` (world
  // cells, 1 = coarsen) leave the fine region
  void rebuild_operator(const std::vector<u8>* drop = nullptr);
  // nominate / decide: cells of aggregates whose crossing bonds' bound exceeds nominate_phi (with
  // a ring of neighbours), and the re-partition that makes them fine
  std::vector<i32> nominate();
  void refine(const std::vector<i32>& cells);
  void coarsen_quiet();  // refined regions quiet long enough go back into aggregates
  // the fine sub-lattice again after a re-partition (its baseline forces kept as at init)
  void rebuild_sublattice();
  // one attempt at a step; false: stopped before the law for a re-solve, `grow` holds the
  // cells to make fine first (§B6.3)
  bool step_attempt(StepStats& st, std::vector<i32>& grow);
  void apply_force(i32 world_cell, const f64* f6);
  std::vector<std::pair<i32, std::array<f64, 6>>> once_;  // this step's one-step forces (world)
  int refinements_ = 0, coarsenings_ = 0, grows_ = 0;
  int refinements_total_ = 0, grows_total_ = 0;  // (refinements_, grows_: this period's, against
                                                  // max_refinements and max_grows)
  f64 period_start_ = 0.0;  // bubble time the period began (renewed each second and by an event)
  i64 held_ = 0;
  // per sub-lattice cell: within interface_reach of an aggregate (for comp_gen_ == near_gen_)
  std::vector<u8> near_;
  u64 comp_gen_ = 0, near_gen_ = ~u64(0);
  void mark_interface();
  // members of the aggregates within interface_reach of `cells` (world cells), whole aggregates
  // up to max_cells in all
  std::vector<i32> aggregates_near(const std::vector<i32>& cells, i64 max_cells) const;
  std::unordered_map<i32, int> flagged_;  // aggregate (first member cell) -> checks flagged in a row
  std::unordered_map<i32, f64> safe_;     // aggregate decided safe -> its bound then
  f64 rep_dp_ = 0.0, rep_dke_ = -1e300;
  void repartition_check(const std::array<f64, 7>& before);
  struct Refined {
    std::vector<i32> cells;
    int quiet = 0;
  };
  std::vector<Refined> refined_;
  void rebuild_preconditioner();
  void fine_state(const f64* x, std::vector<f64>& uF) const;  // u0_F + x_F on the sub-lattice
  void nonlinear_part(const f64* x, f64* out);                // N(x) in composite space
  void coarse_apply(const f64* x, f64* y) const;             // K_coarse x
  void tangent_apply(const f64* x, f64* y);                   // K_coarse x + P_f^T K_ff,t x_f
  void detach(const CutSet& cut, StepStats* st);
  // record: the pieces are reported (take_islands); a crater's cells or pieces the world already
  // detached are not
  void remove_islands(const std::vector<std::vector<i32>>& isl, StepStats* st, bool record = true);
  std::vector<i32> contact_seeds_;  // cells of cracks since the last contact check (still waiting)
  void release_contact_pieces(StepStats* st);
  f64 base_dt_ = 0.0;    // the step at init: one-step forces are impulses over it
  i32 event_fine_ = 0;   // fine cells events asked for (init's fine region, apply_event): the
                         // nomination and growth budgets (max_fine_cells) come on top
  i64 event_step_ = 0;   // the step of the last event (max_steps counts from it)

  Lattice* L_ = nullptr;
  BubbleOptions opt_;
  Composite C_;
  SubLattice S_;
  std::vector<i32> fnode_;          // sub-lattice cell -> composite node (-1 ghost / dead)
  std::vector<f64> u0_;             // world baseline (6 L.n)
  std::vector<f64> u0F_;            // baseline on the sub-lattice (6 F.n)
  std::vector<f64> fint0_;          // F_ff(u0) on the sub-lattice (6 F.n)
  std::vector<f64> r_;              // P^T r (6 C.n)
  std::vector<f64> x_, v_, v_prev_, d_prev_, f_once_;
  std::vector<f64> mass_diag_scale_;
  Multigrid mg_;
  bool mg_dirty_ = true;
  InitProfile init_prof_{};
  u8 dirty_why_ = 0;  // diagnostics: 1 init, 2 ruptures, 4 re-partition, 8 frames on/off, 16 rotation drift, 32 slow PCG,
                      // 64 step size
  int ruptures_since_build_ = 0;
  bool mg_framed_ = false;
  std::vector<f64> th_build_;
  // rotate_operator: per composite node, its rotation since the last build (3x3 row-major; only
  // for rot_nodes_, the rotated nodes; empty = identity)
  std::vector<f64> rot_;
  std::vector<i32> rot_nodes_;
  void update_rotation(const std::vector<f64>& uF);
  void refresh_operator(const f64* x, StepStats& st);
  f64 pending_ms_mg_ = 0.0;
  bool pending_rebuilt_ = false;
  void rotate_in(const f64* x, f64* t) const;   // t = T^T x
  void rotate_out(const f64* t, f64* y) const;  // y = T t
  bool be_next_ = true;
  bool asleep_ = false;
  bool failed_ = false;
  int quiet_ = 0;
  i64 steps_ = 0;
  f64 time_ = 0.0;
  f64 load_scale_ = 1.0;
  std::vector<i64> ruptured_;
  i64 detached_ = 0;
  std::vector<DetachedIsland> islands_;
  ConnStats conn_;
  std::vector<f64> scratch_uF_, scratch_fF_;
  // projection basis V (A-orthonormal) and W = A V, valid for the current operator
  std::vector<std::vector<f64>> proj_v_, proj_w_;
  std::vector<f64> proj_x0_, proj_r0_;
  size_t proj_next_ = 0;  // ring position once full
  void projection_reset() {
    proj_v_.clear();
    proj_w_.clear();
    proj_next_ = 0;
  }
  std::vector<std::pair<i32, int>> broken_;
  std::vector<std::pair<i32, int>> cracked_;
};

// Event residual of removing cells from L at baseline u0 (call BEFORE removing them):
// r = sum of the removed bonds' contributions to f_int(u0) at the surviving cells.
std::vector<f64> removal_residual(const Lattice& L, std::span<const i32> cells, const std::vector<f64>& u0, bool corot);

}  // namespace svx
