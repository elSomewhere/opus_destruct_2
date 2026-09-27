// structvox — rigid-aggregation multigrid preconditioner for the RBSM lattice.
//
// Level l+1 nodes are the connected components of level-l nodes inside each 2x2x2
// block (Dick et al. component splitting). The prolongation maps a coarse node's rigid
// motion (U, Theta about its centroid X) to each child: u = U + Theta x (x_c - X), th = Theta.
// Coarse operators are Galerkin products scaled by `coarse_scale` per level (Braess-style
// correction of the over-stiff unsmoothed aggregation; research/mlexp spike: 44-51 PCG
// iterations flat from 44k to 236k DOF). Smoother: block-Jacobi(6x6) Chebyshev.
// The finest level is matrix-free (svx::apply_stiffness).
#pragma once

#include <array>
#include <functional>
#include <vector>

#include "svx/solve/lattice.hpp"

namespace svx {

struct MGOptions {
  i32 coarse_max = 200;     // stop coarsening at <= this many nodes (dense Cholesky) ...
  i32 coarse_min = 24;      // ... scaled down to max(coarse_min, fine nodes / coarse_ratio) so
  i32 coarse_ratio = 256;   // small systems never pay an O(N^3) coarse factor (1200 DOF ~ 0.6 Gflop)
  f64 coarse_scale = 0.5;   // per-level multiplier of Galerkin coarse operators
  int cheb_degree = 3;      // coarse levels
  int cheb_degree_fine = 3; // finest level
  f64 cheb_ratio = 0.1;     // lambda_min = ratio * lambda_max
  // lambda_max(D^-1 A) of assembled levels: Lanczos steps (block-Jacobi CG; its tridiagonal's
  // largest eigenvalue converges from below far faster than power iteration); 0 = 20 power
  // iterations (the former estimate)
  int eig_iters = 10;
  int max_levels = 24;
  int cycle_gamma = 2;      // 1 = V-cycle, 2 = W-cycle. W is the default: unsmoothed
                            // aggregation needs the extra coarse work on thin-slab Doom
                            // geometry (MAP01 3.1M voxels: V 0.3 vs W 2e-6 residual @60 its)
  f64 strength = 0.0;       // aggregate coarse nodes only across couplings with
                            // ||A_ij|| >= strength * sqrt(||A_ii|| ||A_jj||)
  // Optional diagonal shift for dynamics: A = K + mass_shift * M (M = lumped mass/inertia).
  f64 mass_shift = 0.0;
  // Coarsening of assembled levels: false = components of 2x2x2 blocks (lattice-aligned
  // levels), true = greedy neighbourhood aggregation (any graph, e.g. an event composite whose
  // nodes have mixed sizes; ~5-15x reduction per level).
  bool greedy = false;
  // Preconditioner matrices in single precision (f64 accumulation; the Krylov operator and
  // residuals stay double). Halves the memory traffic of the smoother, which dominates.
  bool f32_levels = false;
  // Column-major SIMD copies of the level matrices for the smoother (default on).
  bool simd_levels = true;
  // Smoother of assembled levels: false = block-Jacobi Chebyshev (parallel), true = symmetric
  // block Gauss-Seidel (forward pre-sweep, backward post-sweep; sequential, ~3x cheaper per
  // cycle and at least as strong — the choice for single-threaded bubbles).
  bool sgs = false;
};

// Block-sparse (6x6) matrix in CSR-of-blocks form.
struct Bsr6 {
  i32 n = 0;
  std::vector<i32> rowptr, col;
  mutable std::vector<f64> val;  // 36 per block, row-major
  std::vector<f32> valf;  // optional f32 copy (preconditioner applies: half the memory traffic)
  std::vector<f64> valc;  // optional column-major copy (SIMD applies)
  std::vector<f32> valcf;  // optional column-major f32 copy (SIMD, f64 accumulation)
  void apply(const f64* x, f64* y) const;
  void apply_f32(const f64* x, f64* y) const;  // uses valf (f64 accumulation)
  Bsr6 without_copies() const { return Bsr6{n, rowptr, col, val, {}, {}, {}}; }  // val only
  void apply_simd(const f64* x, f64* y) const;  // uses valc (column-major, 2-wide vectors)
  void apply_simd_f32(const f64* x, f64* y) const;  // uses valcf (widened to 2-wide f64)
  void make_simd();                             // builds valc from val
  void make_simd_f32();                         // builds valcf from val
  i64 nnz_blocks() const { return static_cast<i64>(col.size()); }
};

class Multigrid {
 public:
  void build(const Lattice& L, const MGOptions& opt);
  // Assembled fine level (e.g. an event composite): A with identity rows / columns on the
  // Dirichlet DOFs, block-diagonal mass M (36 per node, may be empty; scaled by
  // opt.mass_shift), node positions (3 per node, m), cell-unit blocks for the 2x2x2
  // coarsening and per-node Dirichlet masks.
  void build_assembled(const Bsr6& A, const std::vector<f64>& M, const std::vector<f64>& pos,
                       const std::vector<std::array<i32, 3>>& blk, const std::vector<u8>& fix, const MGOptions& opt);
  // z = M^{-1} r for fine-level vectors (6 * L.n entries).
  void apply(const f64* r, f64* z) const;
  // Fine operator including the optional mass shift.
  void apply_fine_operator(const f64* x, f64* y) const;
  std::vector<i32> level_sizes() const;
  std::vector<i64> level_blocks() const;  // stored 6x6 blocks per level (assembled levels)
  const Bsr6* level_matrix(size_t l) const;  // an assembled level's matrix (diagnostics)
  const MGOptions& options() const { return opt_; }

 private:
  struct Level {
    i32 n = 0;
    Bsr6 A;                          // stiffness part (scaled Galerkin)
    std::vector<f64> M;              // mass part: exact Galerkin, block diagonal (36/node), may be empty
    std::vector<f64> Dinv;           // 36 per node
    f64 lmax = 0.0, lmin = 0.0;
    std::vector<i32> parent;         // node -> next-level node (-1: none)
    std::vector<f64> off;            // 3 per node: X_node - X_parent (m)
    std::vector<std::array<i32, 3>> blk;
    std::vector<f64> pos;            // 3 per node
    std::vector<f64> w;              // aggregated fine-cell count
    mutable std::vector<f64> x, b, r, t, d;
    mutable std::vector<f64> vres, vrhs, vsol;  // cycle buffers (residual here; rhs / solution as a coarse level)
    mutable std::vector<f64> vres2, vtmp;       // extra gamma-cycle corrections at this level
  };

  void apply_level(const Level& lv, const f64* x, f64* y) const;  // y = (A + M) x
  // Chebyshev smoothing of A x = b; x_zero: x is known to be zero on entry (pre-smoothing), so
  // the initial residual is b without an apply. (Degree k costs k - 1 applies, plus one for the
  // initial residual of a non-zero x: the residual after the last update is never needed.)
  void smooth_fine(f64* x, const f64* b, bool x_zero) const;
  void mask_fine(f64* z) const;
  void smooth_coarse(const Level& lv, f64* x, const f64* b, bool x_zero) const;
  void sgs_sweep(const Level& lv, f64* x, const f64* b, bool forward) const;
  void vcycle_coarse(size_t l, const f64* b, f64* x) const;
  void vcycle_coarse_add(size_t l, const f64* b, f64* x) const;
  void restrict_to(const std::vector<i32>& parent, const std::vector<f64>& off, i32 n_fine, const f64* r,
                   i32 n_coarse, f64* rc) const;
  void prolong_add(const std::vector<i32>& parent, const std::vector<f64>& off, i32 n_fine, const f64* xc,
                   f64* x) const;
  void build_level1(const Lattice& L);
  bool coarsen(size_t l);  // builds lv_[l+1] from lv_[l]; false if coarsening stalls
  void finalize_level(Level& lv);
  f64 lanczos_lmax(const Level& lv, std::vector<f64>& r, std::vector<f64>& p, std::vector<f64>& z, int k) const;
  void factor_coarsest();
  void fold_mass();

  const Lattice* L_ = nullptr;
  bool assembled_ = false;
  std::vector<u8> fix0_;  // assembled mode: per-node Dirichlet masks
  MGOptions opt_;
  std::vector<f64> D0inv_;
  f64 l0max_ = 0.0, l0min_ = 0.0;
  std::vector<i32> parent0_;
  std::vector<f64> off0_;
  std::vector<f64> shift0_;  // 6 per cell: mass_shift * (m,m,m,I,I,I)
  std::vector<Level> lv_;
  std::vector<f64> chol_;
  i32 chol_n_ = 0;
  mutable std::vector<f64> fr_, ft_, fd_, fq_, fres_;
};

struct PcgStats {
  int iters = 0;
  f64 rel_res = 0.0;
  bool converged = false;
};

// Solves A u = f with A = the multigrid's fine operator, preconditioned by the V-cycle.
// If use_x0 is false, u is zero-initialized. Relative residual ||r||/||f|| < rtol.
PcgStats pcg_solve(const Multigrid& mg, i32 n_cells, const f64* f, f64* u, f64 rtol, int maxit, bool use_x0);

// Same with an explicit SPD operator y = A x (e.g. a dynamic operator whose mass shift differs
// from the one the preconditioner was built with).
using LinOp = std::function<void(const f64* x, f64* y)>;
// Converged when ||r|| <= rtol ||f|| or ||r|| <= atol (absolute, force units).
PcgStats pcg_solve_op(const LinOp& A, const Multigrid& mg, i32 n_cells, const f64* f, f64* u, f64 rtol, int maxit,
                      bool use_x0, f64 atol = 0.0);
// Same with an explicit preconditioner z = M^-1 r (SPD).
PcgStats pcg_solve_op(const LinOp& A, const LinOp& precond, i32 n_cells, const f64* f, f64* u, f64 rtol, int maxit,
                      bool use_x0, f64 atol = 0.0);

}  // namespace svx
