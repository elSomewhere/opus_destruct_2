// structvox — implicit dynamics of the fine lattice (the dynamic truth model, plan §B2/B3/B7).
//
// Linearly implicit BDF2 (backward Euler on the first step and after every rupture or
// removal) with Rayleigh damping C = alpha M + beta K folded into the operator:
//   [(c^2 + alpha c) M + (1 + beta c) K] dx = f - f_int(x_n) - c M (c e - v^) - c (alpha M + beta K) e
// with c = 3 / (2 dt), e = -(x_n - x_{n-1}) / 3, v^ = (4 v_n - v_{n-1}) / 3 (BDF2) or
// c = 1 / dt, e = 0, v^ = v_n (BE). With corotational kinematics the step is a few
// modified-Newton iterations on the corotated residual with the same operator. The system is
// solved by PCG preconditioned with the lattice multigrid (built with the BDF2 mass shift).
//
// Damage is frozen within a step: the operator uses the committed damage, the law is then
// evaluated at the new state and committed (damage accumulates across steps), and at most
// max_breaks rupture candidates break per step in oracle order (the rest carry over).
// Detached islands are removed and reported (v1: they vanish); a ray cast below each island
// column schedules a virtual impact load (m v_impact / T + m g over T) on the landing cell
// at the predicted landing time, and the load the island exerted through the ruptured bonds
// ramps out over its fall time.
//
// Look knobs (plan §B3): compliance S_p divides the stiffness (the law sees jump / S_p, so
// failure is S_p-invariant while motion is sqrt(S_p) slower and S_p larger); render
// amplification is a front-end concern.
#pragma once

#include <array>
#include <unordered_map>
#include <vector>

#include "svx/sim/blast.hpp"
#include "svx/sim/statics.hpp"

namespace svx {

struct DynamicsOptions {
  f64 dt = 1.0 / 60.0;
  f64 g = 9.81;
  bool bdf2 = true;
  bool corot = true;
  // Linearly implicit steps: the residual form carries any unbalanced force into the next
  // step, so extra Newton iterations are only needed while rotations change the tangent.
  int newton_iters = 3;         // max Newton iterations per step (corot)
  f64 newton_rtol = 0.1;        // stop when ||R|| <= rtol ||R_0|| ...
  f64 newton_atol = 1e-2;       // ... or ||R|| <= atol ||f_ext|| (the rest carries over)
  f64 lin_rtol = 1e-4;          // PCG: relative to the step residual ...
  f64 lin_atol = 1e-6;          // ... or absolute, relative to ||f_ext||
  bool warm_start = true;       // BDF2 steps start from the previous increment
  f64 frame_threshold = 0.01;   // rad: corotated tangent once any cell rotates more than this
  f64 frame_rebuild = 0.05;     // rad: rebuild the preconditioner when rotations drift this much
  int lin_maxit = 300;
  f64 rayleigh_alpha = 0.0;     // 1/s
  f64 rayleigh_beta = 0.0;      // s
  f64 compliance = 1.0;         // S_p
  int max_breaks = 512;
  bool damage = true;
  bool virtual_impact = true;
  f64 impact_duration = 0.05;   // s
  f64 ramp_min = 0.05;          // s
  f64 sleep_velocity = 1e-2;    // m/s (max cell speed) below which the structure may settle
  int sleep_steps = 8;          // consecutive quiet steps before the settle projection
  bool settle = true;           // run the static settle projection before sleeping
  int mg_rebuild_iters = 60;
  MGOptions mg{};
  StaticsOptions statics{};     // for the settle projection
};

struct DetachedIsland {
  std::vector<i32> cells;
  f64 mass = 0.0;
  std::array<f64, 3> com{0, 0, 0}, v{0, 0, 0}, w{0, 0, 0};
  i64 step = 0;
  f64 fall_time = -1.0;         // to the first landing (-1: nothing below)
};

struct StepStats {
  int newton = 0;
  int pcg = 0;
  f64 residual = 0.0;           // final relative Newton residual
  i32 ruptured = 0;
  i32 carried = 0;              // candidates left for later steps
  i32 detached_cells = 0;
  i32 islands = 0;
  f64 kinetic = 0.0;
  f64 max_speed = 0.0;
  f64 max_damage = 0.0;
  bool be = false;
  bool rebuilt = false;
  bool refined = false;  // (bubbles) nominate / decide grew the fine region
  bool settled = false;
  bool asleep = false;
  // wall-clock diagnostics (ms; not part of the deterministic state)
  f64 ms_total = 0.0, ms_solve = 0.0, ms_mg = 0.0, ms_law = 0.0, ms_topo = 0.0, ms_settle = 0.0;
};

class Dynamics {
 public:
  // The lattice is owned by the caller and mutated (damage, ruptures, removals).
  void init(Lattice& L, const DynamicsOptions& opt);
  // State at rest (e.g. a static equilibrium). Resets the integrator history.
  void set_state(const std::vector<f64>& u);
  // Static equilibrium of gravity with the committed damage (no rupture) as the rest state.
  EquilibriumStats settle_to_equilibrium();
  StepStats step();

  void wake();
  bool asleep() const { return asleep_; }
  // One-step external generalized force (6 per cell).
  void add_force(i32 cell, const f64* f6);
  // Velocity change applied now (6 per cell).
  void add_velocity(i32 cell, const f64* dv6);
  // Blast prefracture + radial impulse at the current state.
  BlastResult blast(const BlastParams& bp);
  // Removes cells (carve) and detaches what they held.
  void carve(const std::vector<i32>& cells);

  const std::vector<f64>& u() const { return u_; }
  const std::vector<f64>& v() const { return v_; }
  f64 time() const { return time_; }
  i64 steps() const { return steps_; }
  const std::vector<i64>& ruptured() const { return ruptured_; }
  i64 detached_cells() const { return detached_cells_; }
  std::vector<DetachedIsland> take_islands();
  const ConnStats& conn_stats() const { return conn_; }
  f64 kinetic_energy() const;
  i32 cell_at(i32 x, i32 y, i32 z) const;  // -1 if none (dead cells included)

 private:
  struct TimedLoad {
    i32 cell;
    std::array<f64, 6> f;
    f64 t0, t1;
    bool ramp;  // linear ramp from f at t0 to 0 at t1 (else constant on [t0, t1])
  };

  void rebuild_mass();
  void build_mg();
  void refresh_operator(const std::vector<f64>& x, StepStats* st);  // frames + preconditioner
  StepStats step_impl();
  void external_forces(f64 t, std::vector<f64>& f);
  void apply_mass(const f64* x, f64* y, f64 s) const;  // y += s M x
  void detach(const CutSet& cut, const std::vector<std::array<f64, 6>>* pre_forces,
              const std::vector<std::pair<i32, i32>>* broken, StepStats* st);
  i32 rupture_candidates(std::vector<RuptureCandidate>& cand, StepStats* st);

  Lattice* L_ = nullptr;
  DynamicsOptions opt_;
  std::vector<f64> u_, v_, v_prev_, d_prev_;
  std::vector<f64> mdiag_;
  std::vector<f64> f_once_;
  std::vector<TimedLoad> timed_;
  Multigrid mg_;
  bool mg_dirty_ = true;
  bool mg_framed_ = false;
  std::vector<f64> th_build_;  // state at the last preconditioner build (rotation drift)
  bool be_next_ = true;
  bool asleep_ = false;
  int quiet_steps_ = 0;
  f64 time_ = 0.0;
  i64 steps_ = 0;
  std::vector<i64> ruptured_;
  i64 detached_cells_ = 0;
  std::vector<DetachedIsland> islands_;
  ConnStats conn_;
  std::unordered_map<u64, i32> index_;
};

}  // namespace svx
