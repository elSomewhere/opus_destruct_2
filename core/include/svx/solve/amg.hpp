// structvox — 6-DOF block-sparse matrices and an aggregation multigrid for fragment graphs.
//
// Nodes are rigid fragments with 6 DOFs (u, theta about the fragment's centre). Level l+1 nodes
// are greedy aggregates of strongly coupled level-l nodes; the prolongation maps an aggregate's
// rigid motion (U, Theta about its centroid X) to each member: u = U + Theta x (x_i - X),
// th = Theta (the near-nullspace of elasticity). Coarse operators are scaled Galerkin products.
// Smoother: symmetric block Gauss-Seidel (6x6 blocks) or block-Jacobi Chebyshev; coarsest level:
// dense Cholesky. Used as the preconditioner of CG.
#pragma once

#include <vector>

#include "svx/base/vec.hpp"

namespace svx {

// Block-sparse (6x6, row-major blocks) symmetric matrix in CSR-of-blocks form. Each row holds
// its diagonal block (first) and its off-diagonal blocks.
struct Bsr6 {
  i32 n = 0;
  std::vector<i32> rowptr, col;
  std::vector<f64> val;  // 36 per block
  void apply(const f64* x, f64* y) const;  // (rows in parallel: deterministic)
  i64 blocks() const { return static_cast<i64>(col.size()); }
};

// Rows per parallel chunk and Gauss-Seidel partition (fixed: results never depend on the thread
// count).
constexpr i64 kRowGrain = 512;

// Assembles a Bsr6 from (row, col, 6x6 block) contributions (both triangles given).
class Bsr6Builder {
 public:
  explicit Bsr6Builder(i32 n) : n_(n), rows_(static_cast<size_t>(n)) {}
  f64* block(i32 r, i32 c);  // zero-initialized on first use
  Bsr6 finish();

 private:
  i32 n_;
  std::vector<std::vector<std::pair<i32, i32>>> rows_;  // (col, slot)
  std::vector<f64> store_;
};

struct AmgOptions {
  i32 coarse_max = 48;       // dense solve at or below this many nodes
  f64 strength = 0.1;        // aggregate across couplings with ||A_ij|| >= s sqrt(||A_ii|| ||A_jj||)
  bool smoothed = true;      // smoothed aggregation: P = (I - w D^-1 A) P_rigid
  f64 coarse_scale = 0.6;    // scaling of unsmoothed Galerkin coarse operators (smoothed: none)
  int max_levels = 16;
  int gamma = 1;             // 1 V-cycle, 2 W-cycle
  bool sgs = true;           // symmetric block Gauss-Seidel (else block-Jacobi Chebyshev)
  int sweeps = 1;            // smoother sweeps (SGS: forward + backward each)
  int cheb_degree = 3;
};

class Amg {
 public:
  // A: SPD (all rows free). pos: 3 per node (m). Returns false if a diagonal block is singular.
  bool build(const Bsr6& A, const std::vector<V3>& pos, const AmgOptions& opt = {});
  void apply(const f64* r, f64* z) const;  // z = M^{-1} r (one cycle from zero)
  const Bsr6& matrix() const { return lv_.front().A; }
  i32 levels() const { return static_cast<i32>(lv_.size()); }
  std::vector<i32> level_sizes() const;
  std::vector<i64> level_blocks() const;  // matrix blocks per level, then prolongation blocks per level
  bool built() const { return !lv_.empty(); }
  i64 work_per_apply() const { return work_; }  // block operations per cycle (for budgets)

 private:
  struct Level {
    Bsr6 A;
    std::vector<f64> Dinv;       // 36 per node
    std::vector<V3> pos;
    std::vector<i32> parent;     // node -> next-level node
    std::vector<V3> off;         // pos - parent pos
    // prolongation to this level from the next (rows: this level's nodes, 6x6 blocks)
    std::vector<i32> Prow, Pcol;
    std::vector<f64> Pval;
    f64 lmax = 0.0;
    mutable std::vector<f64> x, b, r, t, xc, xold;
    // restriction: the transpose of the prolongation by coarse node (fine node, P block index)
    std::vector<i32> Rrow;
    std::vector<std::pair<i32, i32>> Rent;
    // single-precision copies for apply(): 6x6 blocks column-major with rows padded to 8 (48
    // floats), vectors padded to 8 floats per node (SIMD, deterministic: no fused operations)
    std::vector<f32> Af, Df, Pf, Rf;  // A, Dinv, P (by fine row), P^T (by coarse row, Rent order)
    mutable std::vector<f32> xf, bf, rf, of, cf;
  };
  bool finalize(Level& L);
  f64 estimate_lmax(const Level& L) const;
  bool coarsen(size_t l);
  void smooth(const Level& L, f64* x, const f64* b, bool zero) const;
  void cycle(size_t l, const f64* b, f64* x) const;
  void make_fast();
  void cycle_f(size_t l, const f32* b, f32* x) const;
  void smooth_f(const Level& L, f32* x, const f32* b, bool fresh, bool backward) const;
  void factor_coarsest();
  void solve_coarsest(const f64* b, f64* x) const;

  AmgOptions opt_;
  std::vector<Level> lv_;
  std::vector<f64> chol_;
  i32 chol_n_ = 0;
  i64 work_ = 0;
};

struct PcgResult {
  int iters = 0;
  f64 rel_res = 0.0;
  bool converged = false;
};

// Solves A x = b (x warm-started if use_x0) to ||r|| <= rtol ||b|| (or ||r|| <= atol).
PcgResult pcg(const Bsr6& A, const Amg* M, const f64* b, f64* x, f64 rtol, int maxit, bool use_x0, f64 atol = 0.0);

namespace blk6 {
bool inv6(const f64* A, f64* Ainv);
inline void mv6(const f64* __restrict M, const f64* __restrict x, f64* __restrict y) {
  const f64 x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3], x4 = x[4], x5 = x[5];
  for (int r = 0; r < 6; ++r) {
    const f64* m = M + r * 6;
    y[r] = m[0] * x0 + m[1] * x1 + m[2] * x2 + m[3] * x3 + m[4] * x4 + m[5] * x5;
  }
}
inline void mv6_add(const f64* __restrict M, const f64* __restrict x, f64* __restrict y) {
  const f64 x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3], x4 = x[4], x5 = x[5];
  for (int r = 0; r < 6; ++r) {
    const f64* m = M + r * 6;
    y[r] += m[0] * x0 + m[1] * x1 + m[2] * x2 + m[3] * x3 + m[4] * x4 + m[5] * x5;
  }
}
inline void mv6_sub(const f64* __restrict M, const f64* __restrict x, f64* __restrict y) {
  const f64 x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3], x4 = x[4], x5 = x[5];
  for (int r = 0; r < 6; ++r) {
    const f64* m = M + r * 6;
    y[r] -= m[0] * x0 + m[1] * x1 + m[2] * x2 + m[3] * x3 + m[4] * x4 + m[5] * x5;
  }
}
// y = M^T x
inline void mtv6(const f64* __restrict M, const f64* __restrict x, f64* __restrict y) {
  for (int c = 0; c < 6; ++c) y[c] = 0.0;
  for (int r = 0; r < 6; ++r) {
    const f64 xr = x[r];
    const f64* m = M + r * 6;
    for (int c = 0; c < 6; ++c) y[c] += m[c] * xr;
  }
}
f64 fro6(const f64* M);
// P = [[I, -[r]x], [0, I]]: maps a rigid motion (U, Theta) about X to a point at X + r.
void rigid_block(const V3& r, f64* P);
// C = A^T B A (6x6), accumulated: C += s A^T B D
void atbd_add(const f64* A, const f64* B, const f64* D, f64 s, f64* C);
}  // namespace blk6

}  // namespace svx
