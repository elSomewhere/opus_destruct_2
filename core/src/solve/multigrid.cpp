#include "svx/solve/multigrid.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "svx/base/parallel.hpp"
#include "svx/base/work.hpp"
#include "svx/solve/block6.hpp"

namespace svx {

using namespace blk6;

namespace {

constexpr i64 kGrain = 2048;  // rows per parallel chunk (fixed => deterministic)
constexpr i64 kVecGrain = i64(1) << 16;  // cheap per-element vector ops: parallel only for large systems

// ---------------------------------------------------------------------------
// Small dense helpers (6x6, row-major)
// ---------------------------------------------------------------------------

// Deterministic pseudo-random vector for eigenvalue estimates.
void det_random(f64* v, size_t m, u64 seed) {
  u64 s = seed * 6364136223846793005ULL + 1442695040888963407ULL;
  for (size_t i = 0; i < m; ++i) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    v[i] = (static_cast<f64>((s >> 11) & ((u64(1) << 52) - 1)) / static_cast<f64>(u64(1) << 52)) - 0.5;
  }
}

f64 pdot(const f64* a, const f64* b, size_t m) {
  return parallel_sum(static_cast<i64>(m), kVecGrain, [&](i64 lo, i64 hi) {
    f64 s = 0.0;
    for (i64 i = lo; i < hi; ++i) s += a[i] * b[i];
    return s;
  });
}

// Exact symmetrization of a 6x6 block (inverses of symmetric blocks are symmetric up to
// rounding; the smoother reads them column-major and must stay a symmetric operator).
inline void sym6(f64* B) {
  for (int r = 0; r < 6; ++r)
    for (int c = r + 1; c < 6; ++c) {
      const f64 v = 0.5 * (B[r * 6 + c] + B[c * 6 + r]);
      B[r * 6 + c] = B[c * 6 + r] = v;
    }
}

// y = Dinv x (block diagonal, 36 per node). The inverse blocks are symmetric (inverses of SPD
// diagonal blocks), so the row-major storage is also column-major: 2-wide column updates.
void block_diag_apply(const std::vector<f64>& Dinv, i32 n, const f64* x, f64* y) {
  typedef f64 v2 __attribute__((ext_vector_type(2)));
  parallel_for(n, 4 * kGrain, [&](i64 b, i64 e) {
    for (i64 i = b; i < e; ++i) {
      const f64* __restrict B = &Dinv[36 * size_t(i)];
      const f64* __restrict xi = x + 6 * size_t(i);
      v2 a0 = {0, 0}, a1 = {0, 0}, a2 = {0, 0};
      for (int c = 0; c < 6; ++c) {
        const v2 xs = {xi[c], xi[c]};
        v2 c0, c1, c2;
        __builtin_memcpy(&c0, B + 6 * c, 16);
        __builtin_memcpy(&c1, B + 6 * c + 2, 16);
        __builtin_memcpy(&c2, B + 6 * c + 4, 16);
        a0 += c0 * xs;
        a1 += c1 * xs;
        a2 += c2 * xs;
      }
      f64* __restrict yi = y + 6 * size_t(i);
      __builtin_memcpy(yi, &a0, 16);
      __builtin_memcpy(yi + 2, &a1, 16);
      __builtin_memcpy(yi + 4, &a2, 16);
    }
  });
}

}  // namespace

// ---------------------------------------------------------------------------
// Bsr6
// ---------------------------------------------------------------------------

namespace {

// y[r] = sum_k B_k x[col_k] for rows [b, e): fully unrolled 6x6 (the compiler vectorizes the
// independent row sums); no per-row indirection.
void bsr6_rows(const Bsr6& A, const f64* __restrict x, f64* __restrict y, i64 b, i64 e) {
  const i32* __restrict rp = A.rowptr.data();
  const i32* __restrict cl = A.col.data();
  const f64* __restrict vv = A.val.data();
  for (i64 r = b; r < e; ++r) {
    f64 a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0;
    for (i32 k = rp[r]; k < rp[r + 1]; ++k) {
      const f64* __restrict B = vv + 36 * size_t(k);
      const f64* __restrict xc = x + 6 * size_t(cl[k]);
      const f64 x0 = xc[0], x1 = xc[1], x2 = xc[2], x3 = xc[3], x4 = xc[4], x5 = xc[5];
      a0 += B[0] * x0 + B[1] * x1 + B[2] * x2 + B[3] * x3 + B[4] * x4 + B[5] * x5;
      a1 += B[6] * x0 + B[7] * x1 + B[8] * x2 + B[9] * x3 + B[10] * x4 + B[11] * x5;
      a2 += B[12] * x0 + B[13] * x1 + B[14] * x2 + B[15] * x3 + B[16] * x4 + B[17] * x5;
      a3 += B[18] * x0 + B[19] * x1 + B[20] * x2 + B[21] * x3 + B[22] * x4 + B[23] * x5;
      a4 += B[24] * x0 + B[25] * x1 + B[26] * x2 + B[27] * x3 + B[28] * x4 + B[29] * x5;
      a5 += B[30] * x0 + B[31] * x1 + B[32] * x2 + B[33] * x3 + B[34] * x4 + B[35] * x5;
    }
    f64* __restrict yr = y + 6 * size_t(r);
    yr[0] = a0;
    yr[1] = a1;
    yr[2] = a2;
    yr[3] = a3;
    yr[4] = a4;
    yr[5] = a5;
  }
}

}  // namespace

namespace {

void bsr6_rows_f32(const Bsr6& A, const f64* __restrict x, f64* __restrict y, i64 b, i64 e) {
  const i32* __restrict rp = A.rowptr.data();
  const i32* __restrict cl = A.col.data();
  const f32* __restrict vv = A.valf.data();
  for (i64 r = b; r < e; ++r) {
    f64 a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0;
    for (i32 k = rp[r]; k < rp[r + 1]; ++k) {
      const f32* __restrict B = vv + 36 * size_t(k);
      const f64* __restrict xc = x + 6 * size_t(cl[k]);
      const f64 x0 = xc[0], x1 = xc[1], x2 = xc[2], x3 = xc[3], x4 = xc[4], x5 = xc[5];
      a0 += B[0] * x0 + B[1] * x1 + B[2] * x2 + B[3] * x3 + B[4] * x4 + B[5] * x5;
      a1 += B[6] * x0 + B[7] * x1 + B[8] * x2 + B[9] * x3 + B[10] * x4 + B[11] * x5;
      a2 += B[12] * x0 + B[13] * x1 + B[14] * x2 + B[15] * x3 + B[16] * x4 + B[17] * x5;
      a3 += B[18] * x0 + B[19] * x1 + B[20] * x2 + B[21] * x3 + B[22] * x4 + B[23] * x5;
      a4 += B[24] * x0 + B[25] * x1 + B[26] * x2 + B[27] * x3 + B[28] * x4 + B[29] * x5;
      a5 += B[30] * x0 + B[31] * x1 + B[32] * x2 + B[33] * x3 + B[34] * x4 + B[35] * x5;
    }
    f64* __restrict yr = y + 6 * size_t(r);
    yr[0] = a0;
    yr[1] = a1;
    yr[2] = a2;
    yr[3] = a3;
    yr[4] = a4;
    yr[5] = a5;
  }
}

}  // namespace

namespace {

typedef f64 v2d __attribute__((ext_vector_type(2)));

inline v2d load2(const f64* p) {
  v2d v;
  __builtin_memcpy(&v, p, sizeof(v));
  return v;
}
inline void store2(f64* p, v2d v) { __builtin_memcpy(p, &v, sizeof(v)); }

// column-major blocks: y_r += sum_c B[c][r] x_c. Six 2-wide accumulators per row (even and odd
// columns apart) halve the dependent add chains, which bound the kernel before memory does.
void bsr6_rows_simd(const Bsr6& A, const f64* __restrict x, f64* __restrict y, i64 b, i64 e) {
  const i32* __restrict rp = A.rowptr.data();
  const i32* __restrict cl = A.col.data();
  const f64* __restrict vv = A.valc.data();
  for (i64 r = b; r < e; ++r) {
    v2d a0 = {0.0, 0.0}, a1 = {0.0, 0.0}, a2 = {0.0, 0.0};
    v2d b0 = {0.0, 0.0}, b1 = {0.0, 0.0}, b2 = {0.0, 0.0};
    for (i32 k = rp[r]; k < rp[r + 1]; ++k) {
      const f64* __restrict B = vv + 36 * size_t(k);
      const f64* __restrict xc = x + 6 * size_t(cl[k]);
      for (int c = 0; c < 6; c += 2) {
        const v2d xe = {xc[c], xc[c]}, xo = {xc[c + 1], xc[c + 1]};
        a0 += load2(B + 6 * c) * xe;
        a1 += load2(B + 6 * c + 2) * xe;
        a2 += load2(B + 6 * c + 4) * xe;
        b0 += load2(B + 6 * c + 6) * xo;
        b1 += load2(B + 6 * c + 8) * xo;
        b2 += load2(B + 6 * c + 10) * xo;
      }
    }
    f64* __restrict yr = y + 6 * size_t(r);
    store2(yr, a0 + b0);
    store2(yr + 2, a1 + b1);
    store2(yr + 4, a2 + b2);
  }
}

typedef f32 v2f __attribute__((ext_vector_type(2)));

inline v2d load2f(const f32* p) {
  v2f v;
  __builtin_memcpy(&v, p, sizeof(v));
  return __builtin_convertvector(v, v2d);
}

void bsr6_rows_simd_f32(const Bsr6& A, const f64* __restrict x, f64* __restrict y, i64 b, i64 e) {
  const i32* __restrict rp = A.rowptr.data();
  const i32* __restrict cl = A.col.data();
  const f32* __restrict vv = A.valcf.data();
  for (i64 r = b; r < e; ++r) {
    v2d a0 = {0.0, 0.0}, a1 = {0.0, 0.0}, a2 = {0.0, 0.0};
    v2d b0 = {0.0, 0.0}, b1 = {0.0, 0.0}, b2 = {0.0, 0.0};
    for (i32 k = rp[r]; k < rp[r + 1]; ++k) {
      const f32* __restrict B = vv + 36 * size_t(k);
      const f64* __restrict xc = x + 6 * size_t(cl[k]);
      for (int c = 0; c < 6; c += 2) {
        const v2d xe = {xc[c], xc[c]}, xo = {xc[c + 1], xc[c + 1]};
        a0 += load2f(B + 6 * c) * xe;
        a1 += load2f(B + 6 * c + 2) * xe;
        a2 += load2f(B + 6 * c + 4) * xe;
        b0 += load2f(B + 6 * c + 6) * xo;
        b1 += load2f(B + 6 * c + 8) * xo;
        b2 += load2f(B + 6 * c + 10) * xo;
      }
    }
    f64* __restrict yr = y + 6 * size_t(r);
    store2(yr, a0 + b0);
    store2(yr + 2, a1 + b1);
    store2(yr + 4, a2 + b2);
  }
}

}  // namespace

void Bsr6::make_simd_f32() {
  valcf.resize(val.size());
  for (size_t k = 0; k < col.size(); ++k)
    for (int r = 0; r < 6; ++r)
      for (int c = 0; c < 6; ++c) valcf[36 * k + c * 6 + r] = static_cast<f32>(val[36 * k + r * 6 + c]);
}

void Bsr6::apply_simd_f32(const f64* x, f64* y) const {
  constexpr i64 kRows = 4096;
  if (n <= kRows) {
    bsr6_rows_simd_f32(*this, x, y, 0, n);
    return;
  }
  parallel_for(n, kRows, [&](i64 b, i64 e) { bsr6_rows_simd_f32(*this, x, y, b, e); });
}

void Bsr6::make_simd() {
  valc.resize(val.size());
  for (size_t k = 0; k < col.size(); ++k)
    for (int r = 0; r < 6; ++r)
      for (int c = 0; c < 6; ++c) valc[36 * k + c * 6 + r] = val[36 * k + r * 6 + c];
}

void Bsr6::apply_simd(const f64* x, f64* y) const {
  constexpr i64 kRows = 4096;
  if (n <= kRows) {
    bsr6_rows_simd(*this, x, y, 0, n);
    return;
  }
  parallel_for(n, kRows, [&](i64 b, i64 e) { bsr6_rows_simd(*this, x, y, b, e); });
}

void Bsr6::apply_f32(const f64* x, f64* y) const {
  constexpr i64 kRows = 4096;
  if (n <= kRows) {
    bsr6_rows_f32(*this, x, y, 0, n);
    return;
  }
  parallel_for(n, kRows, [&](i64 b, i64 e) { bsr6_rows_f32(*this, x, y, b, e); });
}

void Bsr6::apply(const f64* x, f64* y) const {
  // ~36 multiply-adds per block: dispatch to the pool only for large matrices
  constexpr i64 kRows = 4096;
  if (n <= kRows) {
    bsr6_rows(*this, x, y, 0, n);
    return;
  }
  parallel_for(n, kRows, [&](i64 b, i64 e) { bsr6_rows(*this, x, y, b, e); });
}

// ---------------------------------------------------------------------------
// Multigrid
// ---------------------------------------------------------------------------

void Multigrid::apply_fine_operator(const f64* x, f64* y) const {
  if (assembled_) {
    // double precision (the level may also hold an f32 copy)
    if (!lv_[0].A.valc.empty()) lv_[0].A.apply_simd(x, y);
    else lv_[0].A.apply(x, y);
    return;
  }
  apply_stiffness(*L_, x, y);
  if (opt_.mass_shift != 0.0) {
    const i64 m = 6 * i64(L_->n);
    parallel_for(m, kVecGrain, [&](i64 b, i64 e) {
      for (i64 i = b; i < e; ++i) y[i] += shift0_[i] * x[i];
    });
  }
}

void Multigrid::build_assembled(const Bsr6& A, const std::vector<f64>& M, const std::vector<f64>& pos,
                                const std::vector<std::array<i32, 3>>& blk, const std::vector<u8>& fix,
                                const MGOptions& opt) {
  add_work(i64(A.n) * kWorkMgBuild);  // (deterministic work accounting: counted as it starts)
  L_ = nullptr;
  assembled_ = true;
  opt_ = opt;
  lv_.clear();
  chol_.clear();
  fix0_ = fix;
  Level l0;
  l0.n = A.n;
  l0.A = A;
  if (!M.empty() && opt.mass_shift != 0.0) {
    l0.M = M;
    for (auto& v : l0.M) v *= opt.mass_shift;
    // keep the identity rows of Dirichlet DOFs clean
    for (i32 r = 0; r < l0.n; ++r) {
      const u8 m = fix[r];
      if (!m) continue;
      f64* B = &l0.M[36 * size_t(r)];
      for (int q = 0; q < 6; ++q)
        if ((m >> q) & 1)
          for (int c = 0; c < 6; ++c) B[q * 6 + c] = B[c * 6 + q] = 0.0;
    }
  }
  l0.pos = pos;
  l0.blk = blk;
  l0.w.assign(l0.n, 1.0);
  // (SVX_MG_PROFILE: time per build phase, experiments)
  static const bool prof = std::getenv("SVX_MG_PROFILE") != nullptr;
  using PClock = std::chrono::steady_clock;
  auto pt = PClock::now();
  f64 t_fin = 0.0, t_coarsen = 0.0, t_chol = 0.0, t_fold = 0.0;
  auto lap = [&](f64& acc) {
    if (!prof) return;
    const auto now = PClock::now();
    acc += std::chrono::duration<f64, std::milli>(now - pt).count();
    pt = now;
  };
  lap(t_fin);
  t_fin = 0.0;
  finalize_level(l0);
  lap(t_fin);
  lv_.push_back(std::move(l0));
  const i32 cmax = std::max(opt_.coarse_min, std::min(opt_.coarse_max, A.n / std::max(1, opt_.coarse_ratio)));
  while (lv_.back().n > cmax && static_cast<int>(lv_.size()) < opt_.max_levels) {
    if (!coarsen(lv_.size() - 1)) break;
    lap(t_coarsen);
    finalize_level(lv_.back());
    lap(t_fin);
  }
  factor_coarsest();
  lap(t_chol);
  fold_mass();
  lap(t_fold);
  if (prof) {
    std::printf("      [mg build] finalize (Dinv + power its) %.2f ms, coarsen %.2f ms, coarsest factor %.2f ms, fold/simd %.2f ms; levels",
                t_fin, t_coarsen, t_chol, t_fold);
    for (const Level& lv : lv_) std::printf(" %d/%zu", lv.n, lv.A.col.size());
    std::printf("\n");
  }
  for (Level& lv : lv_) {
    lv.vres.assign(6 * size_t(lv.n), 0.0);
    lv.vrhs.assign(6 * size_t(lv.n), 0.0);
    lv.vsol.assign(6 * size_t(lv.n), 0.0);
  }
}

// After the hierarchy is built the per-level mass is only ever used together with A: fold it
// into A's diagonal blocks so applies are one pass (the dense coarsest factor already has it).
void Multigrid::fold_mass() {
  for (Level& lv : lv_) {
    if (lv.M.empty()) continue;
    for (i32 r = 0; r < lv.n; ++r) {
      f64* D = block_at(lv.A, r, r);
      for (int q = 0; q < 36; ++q) D[q] += lv.M[36 * size_t(r) + q];
    }
    std::vector<f64>().swap(lv.M);
  }
  if (opt_.f32_levels && opt_.simd_levels) {
    // smoother / cycle applies in f32 storage; the assembled fine level also keeps its f64
    // SIMD copy for apply_fine_operator (the Krylov operator)
    for (Level& lv : lv_) lv.A.make_simd_f32();
    if (assembled_ && !lv_.empty()) lv_.front().A.make_simd();
  } else if (opt_.simd_levels) {
    for (Level& lv : lv_) lv.A.make_simd();
  } else if (opt_.f32_levels) {
    for (Level& lv : lv_) {
      lv.A.valf.assign(lv.A.val.begin(), lv.A.val.end());
      // the assembled fine level keeps f64 for apply_fine_operator; coarser levels drop it
      if (!(assembled_ && &lv == &lv_.front())) std::vector<f64>().swap(lv.A.val);
    }
  }
}

void Multigrid::build(const Lattice& L, const MGOptions& opt) {
  add_work(i64(L.n) * kWorkMgBuild);  // (deterministic work accounting: counted as it starts)
  L_ = &L;
  assembled_ = false;
  opt_ = opt;
  const i32 n = L.n;
  const size_t m = 6 * size_t(n);
  fr_.assign(m, 0.0);
  ft_.assign(m, 0.0);
  fd_.assign(m, 0.0);
  fq_.assign(m, 0.0);

  shift0_.assign(m, 0.0);
  if (opt_.mass_shift != 0.0) {
    for (i32 i = 0; i < n; ++i) {
      if (L.anchored[i]) continue;
      f64* s = &shift0_[6 * size_t(i)];
      s[0] = s[1] = s[2] = opt_.mass_shift * L.mass[i];
      s[3] = opt_.mass_shift * L.inertia[i][0];
      s[4] = opt_.mass_shift * L.inertia[i][1];
      s[5] = opt_.mass_shift * L.inertia[i][2];
      for (int q = 0; q < 6; ++q)
        if ((L.fixmask[i] >> q) & 1) s[q] = 0.0;
    }
  }

  // Fine diagonal blocks + inverse.
  {
    std::vector<f64> D(36 * size_t(n));
    stiffness_diag_blocks(L, D.data());
    D0inv_.assign(36 * size_t(n), 0.0);
    parallel_for(n, kGrain, [&](i64 b, i64 e) {
      for (i64 i = b; i < e; ++i) {
        if (L.anchored[i]) continue;
        f64* Dc = &D[36 * size_t(i)];
        for (int k = 0; k < 6; ++k) Dc[k * 6 + k] += shift0_[6 * size_t(i) + k];
        if (!inv6(Dc, &D0inv_[36 * size_t(i)])) std::fill(&D0inv_[36 * size_t(i)], &D0inv_[36 * size_t(i)] + 36, 0.0);
        sym6(&D0inv_[36 * size_t(i)]);
        const u8 m = L.fixmask[i];
        if (m)
          for (int q = 0; q < 6; ++q)
            if ((m >> q) & 1)
              for (int c = 0; c < 6; ++c) D0inv_[36 * size_t(i) + q * 6 + c] = D0inv_[36 * size_t(i) + c * 6 + q] = 0.0;
      }
    });
  }
  // lambda_max(D^-1 A) by power iteration.
  {
    std::vector<f64> v(m), w(m), z(m);
    det_random(v.data(), m, 12345);
    for (i32 i = 0; i < n; ++i)
      for (int q = 0; q < 6; ++q)
        if ((L.fixmask[i] >> q) & 1) v[6 * size_t(i) + q] = 0.0;
    f64 lam = 1.0;
    for (int it = 0; it < 20; ++it) {
      apply_fine_operator(v.data(), w.data());
      block_diag_apply(D0inv_, n, w.data(), z.data());
      const f64 nz = std::sqrt(pdot(z.data(), z.data(), m));
      const f64 nv = std::sqrt(pdot(v.data(), v.data(), m));
      if (nz == 0.0 || nv == 0.0) break;
      lam = nz / nv;
      for (size_t k = 0; k < m; ++k) v[k] = z[k] / nz;
    }
    l0max_ = 1.1 * lam;
    l0min_ = opt_.cheb_ratio * l0max_;
  }

  lv_.clear();
  build_level1(L);
  if (lv_.empty()) return;
  size_t l = 0;
  const i32 cmax = std::max(opt_.coarse_min, std::min(opt_.coarse_max, L.num_free() / std::max(1, opt_.coarse_ratio)));
  while (lv_[l].n > cmax && static_cast<int>(lv_.size()) < opt_.max_levels) {
    if (!coarsen(l)) break;
    ++l;
  }
  for (size_t k = 0; k + 1 < lv_.size(); ++k) finalize_level(lv_[k]);
  factor_coarsest();
  fold_mass();
  for (Level& lv : lv_) {
    lv.vres.assign(6 * size_t(lv.n), 0.0);
    lv.vrhs.assign(6 * size_t(lv.n), 0.0);
    lv.vsol.assign(6 * size_t(lv.n), 0.0);
  }
}

void Multigrid::build_level1(const Lattice& L) {
  const i32 n = L.n;
  const f64 h = L.h;
  // Aggregate free cells: components (via bonds) within each 2x2x2 block.
  UnionFind uf(n);
  for (int a = 0; a < 3; ++a)
    for (i32 i = 0; i < n; ++i) {
      const i32 j = L.nbr[a][i];
      if (j < 0 || L.anchored[i] || L.anchored[j]) continue;
      const auto& pi = L.p[i];
      const auto& pj = L.p[j];
      if ((pi[0] >> 1) == (pj[0] >> 1) && (pi[1] >> 1) == (pj[1] >> 1) && (pi[2] >> 1) == (pj[2] >> 1))
        uf.unite(i, j);
    }
  parent0_.assign(n, -1);
  std::vector<i32> root_to_node(n, -1);
  Level lv;
  i32 nc = 0;
  for (i32 i = 0; i < n; ++i) {
    if (L.anchored[i]) continue;
    const i32 r = uf.find(i);
    if (root_to_node[r] < 0) {
      root_to_node[r] = nc++;
      lv.blk.push_back({L.p[i][0] >> 1, L.p[i][1] >> 1, L.p[i][2] >> 1});
    }
    parent0_[i] = root_to_node[r];
  }
  if (nc == 0) return;
  lv.n = nc;
  lv.pos.assign(3 * size_t(nc), 0.0);
  lv.w.assign(nc, 0.0);
  for (i32 i = 0; i < n; ++i) {
    const i32 p = parent0_[i];
    if (p < 0) continue;
    for (int d = 0; d < 3; ++d) lv.pos[3 * size_t(p) + d] += L.p[i][d] * h;
    lv.w[p] += 1.0;
  }
  for (i32 p = 0; p < nc; ++p)
    for (int d = 0; d < 3; ++d) lv.pos[3 * size_t(p) + d] /= lv.w[p];
  off0_.assign(3 * size_t(n), 0.0);
  for (i32 i = 0; i < n; ++i) {
    const i32 p = parent0_[i];
    if (p < 0) continue;
    for (int d = 0; d < 3; ++d) off0_[3 * size_t(i) + d] = L.p[i][d] * h - lv.pos[3 * size_t(p) + d];
  }

  // Pattern pass.
  BsrBuilder bb(nc);
  for (i32 p = 0; p < nc; ++p) bb.touch(p, p);
  for (int a = 0; a < 3; ++a)
    for (i32 i = 0; i < n; ++i) {
      const i32 j = L.nbr[a][i];
      if (j < 0) continue;
      const i32 I = parent0_[i], J = parent0_[j];
      if (I >= 0 && J >= 0 && I != J) {
        bb.touch(I, J);
        bb.touch(J, I);
      }
    }
  lv.A = bb.finish_pattern();

  // Accumulation pass (Galerkin directly from bonds; fine operator is matrix-free).
  const f64 hh = 0.5 * h;
  const f64 s = opt_.coarse_scale;
  f64 B[2][36], P[36], G[2][36];
  for (int a = 0; a < 3; ++a) {
    bond_side_matrices(a, hh, B[0], B[1]);
    for (i32 i = 0; i < n; ++i) {
      const i32 j = L.nbr[a][i];
      if (j < 0) continue;
      const i32 I = parent0_[i], J = parent0_[j];
      if (I >= 0 && I == J) continue;  // internal to a rigid aggregate: zero energy
      if (L.framed()) bond_side_matrices_framed(a, &L.frame[a][Lattice::kFrameSize * size_t(i)], B[0], B[1]);
      Vec6 k = L.bond_k(a, i);
      for (auto& kv : k) kv *= s;
      if (I >= 0) {
        rigid_block(&off0_[3 * size_t(i)], P);
        mask_rows(L.fixmask[i], P);
        mm6(B[0], P, G[0]);
        add_gtkg(G[0], k.data(), G[0], block_at(lv.A, I, I));
      }
      if (J >= 0) {
        rigid_block(&off0_[3 * size_t(j)], P);
        mask_rows(L.fixmask[j], P);
        mm6(B[1], P, G[1]);
        add_gtkg(G[1], k.data(), G[1], block_at(lv.A, J, J));
      }
      if (I >= 0 && J >= 0) {
        add_gtkg(G[0], k.data(), G[1], block_at(lv.A, I, J));
        add_gtkg(G[1], k.data(), G[0], block_at(lv.A, J, I));
      }
    }
  }
  // Support springs (Galerkin onto the aggregate, not scaled).
  if (!L.spring.empty()) {
    for (i32 i = 0; i < n; ++i) {
      const i32 I = parent0_[i];
      if (I < 0) continue;
      f64 Sd[36] = {};
      bool any = false;
      for (int q = 0; q < 6; ++q)
        if (!((L.fixmask[i] >> q) & 1) && L.spring[i][q] != 0.0) {
          Sd[q * 6 + q] = L.spring[i][q];
          any = true;
        }
      if (!any) continue;
      rigid_block(&off0_[3 * size_t(i)], P);
      add_ptaq(P, Sd, P, 1.0, block_at(lv.A, I, I));
    }
  }
  // Mass shift: exact Galerkin rigid-body mass (block diagonal), never scaled.
  if (opt_.mass_shift != 0.0) {
    lv.M.assign(36 * size_t(nc), 0.0);
    for (i32 i = 0; i < n; ++i) {
      const i32 I = parent0_[i];
      if (I < 0) continue;
      rigid_block(&off0_[3 * size_t(i)], P);
      mask_rows(L.fixmask[i], P);
      f64 Md[36] = {};
      for (int k = 0; k < 6; ++k) Md[k * 6 + k] = shift0_[6 * size_t(i) + k];
      add_ptaq(P, Md, P, 1.0, &lv.M[36 * size_t(I)]);
    }
  }
  lv_.push_back(std::move(lv));
}

bool Multigrid::coarsen(size_t l) {
  Level& f = lv_[l];
  const i32 n = f.n;
  // diagonal norms for the strength criterion
  std::vector<f64> dn(n, 0.0);
  for (i32 r = 0; r < n; ++r)
    for (i32 k = f.A.rowptr[r]; k < f.A.rowptr[r + 1]; ++k)
      if (f.A.col[k] == r) dn[r] = fro6(&f.A.val[36 * size_t(k)]);
  auto strong = [&](i32 r, i32 k) {
    const i32 c = f.A.col[k];
    if (c == r) return false;
    const f64 nrm = fro6(&f.A.val[36 * size_t(k)]);
    if (nrm <= 0.0) return false;
    return !(opt_.strength > 0.0 && nrm < opt_.strength * std::sqrt(dn[r] * dn[c]));
  };
  Level c;
  f.parent.assign(n, -1);
  i32 nc = 0;
  if (opt_.greedy && assembled_) {
    // plain aggregation: seed aggregates at nodes whose neighbourhood is still free, then
    // attach the leftovers to their strongest aggregated neighbour (deterministic order)
    std::vector<i32> agg(n, -1);
    for (i32 r = 0; r < n; ++r) {
      if (agg[r] >= 0) continue;
      bool free_nbhd = true;
      for (i32 k = f.A.rowptr[r]; k < f.A.rowptr[r + 1] && free_nbhd; ++k)
        if (strong(r, k) && agg[f.A.col[k]] >= 0) free_nbhd = false;
      if (!free_nbhd) continue;
      agg[r] = nc;
      for (i32 k = f.A.rowptr[r]; k < f.A.rowptr[r + 1]; ++k)
        if (strong(r, k)) agg[f.A.col[k]] = nc;
      ++nc;
    }
    for (i32 r = 0; r < n; ++r) {
      if (agg[r] >= 0) continue;
      i32 best = -1;
      f64 bn = 0.0;
      for (i32 k = f.A.rowptr[r]; k < f.A.rowptr[r + 1]; ++k) {
        const i32 cc = f.A.col[k];
        if (!strong(r, k) || agg[cc] < 0) continue;
        const f64 nrm = fro6(&f.A.val[36 * size_t(k)]);
        if (nrm > bn) {
          bn = nrm;
          best = agg[cc];
        }
      }
      agg[r] = best >= 0 ? best : nc++;
    }
    // renumber in first-occurrence order
    std::vector<i32> ren(nc, -1);
    i32 m = 0;
    for (i32 r = 0; r < n; ++r) {
      if (ren[agg[r]] < 0) {
        ren[agg[r]] = m++;
        c.blk.push_back({f.blk[r][0] >> 1, f.blk[r][1] >> 1, f.blk[r][2] >> 1});
      }
      f.parent[r] = ren[agg[r]];
    }
    nc = m;
  } else {
    UnionFind uf(n);
    for (i32 r = 0; r < n; ++r)
      for (i32 k = f.A.rowptr[r]; k < f.A.rowptr[r + 1]; ++k) {
        const i32 cc = f.A.col[k];
        if (!strong(r, k)) continue;
        const auto& br = f.blk[r];
        const auto& bc = f.blk[cc];
        if ((br[0] >> 1) != (bc[0] >> 1) || (br[1] >> 1) != (bc[1] >> 1) || (br[2] >> 1) != (bc[2] >> 1)) continue;
        uf.unite(r, cc);
      }
    std::vector<i32> root_to_node(n, -1);
    for (i32 r = 0; r < n; ++r) {
      const i32 root = uf.find(r);
      if (root_to_node[root] < 0) {
        root_to_node[root] = nc++;
        c.blk.push_back({f.blk[r][0] >> 1, f.blk[r][1] >> 1, f.blk[r][2] >> 1});
      }
      f.parent[r] = root_to_node[root];
    }
  }
  if (nc >= n || nc > static_cast<i32>(0.95 * n)) {
    f.parent.clear();
    return false;
  }
  c.n = nc;
  c.pos.assign(3 * size_t(nc), 0.0);
  c.w.assign(nc, 0.0);
  for (i32 r = 0; r < n; ++r) {
    const i32 p = f.parent[r];
    for (int d = 0; d < 3; ++d) c.pos[3 * size_t(p) + d] += f.w[r] * f.pos[3 * size_t(r) + d];
    c.w[p] += f.w[r];
  }
  for (i32 p = 0; p < nc; ++p)
    for (int d = 0; d < 3; ++d) c.pos[3 * size_t(p) + d] /= c.w[p];
  f.off.assign(3 * size_t(n), 0.0);
  for (i32 r = 0; r < n; ++r)
    for (int d = 0; d < 3; ++d) f.off[3 * size_t(r) + d] = f.pos[3 * size_t(r) + d] - c.pos[3 * size_t(f.parent[r]) + d];

  // Galerkin: A_c[pI][pJ] += s * P_I^T A_IJ P_J (whole operator scaled; with a mass shift
  // the caller should use coarse_scale = 1).
  BsrBuilder bb(nc);
  for (i32 r = 0; r < n; ++r)
    for (i32 k = f.A.rowptr[r]; k < f.A.rowptr[r + 1]; ++k) bb.touch(f.parent[r], f.parent[f.A.col[k]]);
  c.A = bb.finish_pattern();
  const f64 s = opt_.coarse_scale;
  for (i32 r = 0; r < n; ++r) {
    const f64* oI = &f.off[3 * size_t(r)];
    for (i32 k = f.A.rowptr[r]; k < f.A.rowptr[r + 1]; ++k) {
      const i32 col = f.A.col[k];
      add_rigid_ptaq(oI, &f.A.val[36 * size_t(k)], &f.off[3 * size_t(col)], s, block_at(c.A, f.parent[r], f.parent[col]));
    }
  }
  if (!f.M.empty()) {
    c.M.assign(36 * size_t(nc), 0.0);
    for (i32 r = 0; r < n; ++r) {
      const f64* oI = &f.off[3 * size_t(r)];
      add_rigid_ptaq(oI, &f.M[36 * size_t(r)], oI, 1.0, &c.M[36 * size_t(f.parent[r])]);
    }
  }
  lv_.push_back(std::move(c));
  return true;
}

void Multigrid::finalize_level(Level& lv) {
  const i32 n = lv.n;
  lv.Dinv.assign(36 * size_t(n), 0.0);
  parallel_for(n, kGrain, [&](i64 b, i64 e) {
    for (i64 r = b; r < e; ++r)
      for (i32 k = lv.A.rowptr[r]; k < lv.A.rowptr[r + 1]; ++k)
        if (lv.A.col[k] == r) {
          f64 D[36];
          for (int q = 0; q < 36; ++q) D[q] = lv.A.val[36 * size_t(k) + q] + (lv.M.empty() ? 0.0 : lv.M[36 * size_t(r) + q]);
          if (!inv6(D, &lv.Dinv[36 * size_t(r)])) std::fill(&lv.Dinv[36 * size_t(r)], &lv.Dinv[36 * size_t(r)] + 36, 0.0);
          sym6(&lv.Dinv[36 * size_t(r)]);
          break;
        }
  });
  const size_t m = 6 * size_t(n);
  lv.x.assign(m, 0.0);
  lv.b.assign(m, 0.0);
  lv.r.assign(m, 0.0);
  lv.t.assign(m, 0.0);
  lv.d.assign(m, 0.0);
  std::vector<f64> v(m), w(m), z(m);
  det_random(v.data(), m, 777 + n);
  f64 lam = 1.0;
  if (opt_.eig_iters > 0) {
    lam = lanczos_lmax(lv, v, w, z, opt_.eig_iters);
  } else {
    for (int it = 0; it < 20; ++it) {
      apply_level(lv, v.data(), w.data());
      block_diag_apply(lv.Dinv, n, w.data(), z.data());
      const f64 nz = std::sqrt(pdot(z.data(), z.data(), m));
      const f64 nv = std::sqrt(pdot(v.data(), v.data(), m));
      if (nz == 0.0 || nv == 0.0) break;
      lam = nz / nv;
      for (size_t k = 0; k < m; ++k) v[k] = z[k] / nz;
    }
  }
  lv.lmax = 1.1 * lam;
  lv.lmin = opt_.cheb_ratio * lv.lmax;
}

// Largest eigenvalue of D^-1 A by Lanczos: k steps of block-Jacobi preconditioned CG on A x = b
// (b = r, the random start), whose coefficients give the Lanczos tridiagonal T; its largest
// eigenvalue (Sturm bisection) approaches lambda_max from below. r, p, z are scratch.
f64 Multigrid::lanczos_lmax(const Level& lv, std::vector<f64>& r, std::vector<f64>& p, std::vector<f64>& z,
                            int k) const {
  const i32 n = lv.n;
  const size_t m = 6 * size_t(n);
  std::vector<f64> Ap(m), alpha, beta;
  block_diag_apply(lv.Dinv, n, r.data(), z.data());
  f64 rz = pdot(r.data(), z.data(), m);
  if (!(rz > 0.0)) return 1.0;
  p = z;
  for (int it = 0; it < k; ++it) {
    apply_level(lv, p.data(), Ap.data());
    const f64 pAp = pdot(p.data(), Ap.data(), m);
    if (!(pAp > 0.0)) break;
    const f64 a = rz / pAp;
    for (size_t q = 0; q < m; ++q) r[q] -= a * Ap[q];
    block_diag_apply(lv.Dinv, n, r.data(), z.data());
    const f64 rz_new = pdot(r.data(), z.data(), m);
    alpha.push_back(a);
    if (!(rz_new > 0.0)) break;
    const f64 b = rz_new / rz;
    beta.push_back(b);
    for (size_t q = 0; q < m; ++q) p[q] = z[q] + b * p[q];
    rz = rz_new;
  }
  const int K = static_cast<int>(alpha.size());
  if (K == 0) return 1.0;
  // T: diag d_j = 1/a_j + b_{j-1}/a_{j-1}, off-diagonal e_j = sqrt(b_j)/a_j
  std::vector<f64> d(static_cast<size_t>(K)), e(static_cast<size_t>(K), 0.0);
  for (int j = 0; j < K; ++j) {
    d[size_t(j)] = 1.0 / alpha[size_t(j)] + (j > 0 ? beta[size_t(j - 1)] / alpha[size_t(j - 1)] : 0.0);
    if (j + 1 < K) e[size_t(j)] = std::sqrt(beta[size_t(j)]) / alpha[size_t(j)];
  }
  // Gershgorin bounds, then bisection on the Sturm count of eigenvalues above x
  f64 lo = d[0], hi = d[0];
  for (int j = 0; j < K; ++j) {
    const f64 rad = std::abs(e[size_t(j)]) + (j > 0 ? std::abs(e[size_t(j - 1)]) : 0.0);
    lo = std::min(lo, d[size_t(j)] - rad);
    hi = std::max(hi, d[size_t(j)] + rad);
  }
  auto below = [&](f64 x) {  // eigenvalues of T below x
    int cnt = 0;
    f64 q = d[0] - x;
    if (q < 0.0) ++cnt;
    for (int j = 1; j < K; ++j) {
      if (q == 0.0) q = 1e-300;
      q = d[size_t(j)] - x - e[size_t(j - 1)] * e[size_t(j - 1)] / q;
      if (q < 0.0) ++cnt;
    }
    return cnt;
  };
  for (int it = 0; it < 64; ++it) {
    const f64 mid = 0.5 * (lo + hi);
    if (below(mid) >= K) hi = mid;  // all eigenvalues below mid
    else lo = mid;
  }
  return hi;
}

void Multigrid::factor_coarsest() {
  Level& lv = lv_.back();
  const i32 N = 6 * lv.n;
  // A free-cell component always coarsens to a handful of nodes; a large coarsest level
  // means the caller passed several disconnected components. Keep the dense factor bounded.
  SVX_ASSERT(lv.n <= 2000 && "coarsest multigrid level too large: solve components separately");
  chol_n_ = N;
  chol_.assign(size_t(N) * N, 0.0);
  for (i32 r = 0; r < lv.n; ++r)
    for (i32 k = lv.A.rowptr[r]; k < lv.A.rowptr[r + 1]; ++k) {
      const i32 c = lv.A.col[k];
      const f64* B = &lv.A.val[36 * size_t(k)];
      for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j) chol_[size_t(6 * r + i) * N + (6 * c + j)] += B[i * 6 + j];
    }
  if (!lv.M.empty())
    for (i32 r = 0; r < lv.n; ++r)
      for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j) chol_[size_t(6 * r + i) * N + (6 * r + j)] += lv.M[36 * size_t(r) + i * 6 + j];
  f64 dmax = 0.0;
  for (i32 i = 0; i < N; ++i) dmax = std::max(dmax, chol_[size_t(i) * N + i]);
  for (i32 i = 0; i < N; ++i)
    for (i32 j = 0; j < i; ++j) {
      const f64 v = 0.5 * (chol_[size_t(i) * N + j] + chol_[size_t(j) * N + i]);
      chol_[size_t(i) * N + j] = v;
      chol_[size_t(j) * N + i] = v;
    }
  for (i32 j = 0; j < N; ++j) {
    f64 d = chol_[size_t(j) * N + j];
    for (i32 k = 0; k < j; ++k) d -= chol_[size_t(j) * N + k] * chol_[size_t(j) * N + k];
    if (d <= 1e-14 * dmax) d = 1e-14 * dmax + 1e-300;  // guard (should not trigger)
    const f64 ljj = std::sqrt(d);
    chol_[size_t(j) * N + j] = ljj;
    for (i32 i = j + 1; i < N; ++i) {
      f64 s = chol_[size_t(i) * N + j];
      for (i32 k = 0; k < j; ++k) s -= chol_[size_t(i) * N + k] * chol_[size_t(j) * N + k];
      chol_[size_t(i) * N + j] = s / ljj;
    }
  }
  lv.x.assign(size_t(N), 0.0);
  lv.b.assign(size_t(N), 0.0);
}

void Multigrid::apply_level(const Level& lv, const f64* x, f64* y) const {
  if (!lv.A.valcf.empty()) lv.A.apply_simd_f32(x, y);
  else if (!lv.A.valc.empty()) lv.A.apply_simd(x, y);
  else if (!lv.A.valf.empty()) lv.A.apply_f32(x, y);
  else lv.A.apply(x, y);
  if (lv.M.empty()) return;
  parallel_for(lv.n, kGrain, [&](i64 b, i64 e) {
    for (i64 r = b; r < e; ++r) {
      f64 t[6];
      mv6(&lv.M[36 * size_t(r)], x + 6 * size_t(r), t);
      for (int q = 0; q < 6; ++q) y[6 * size_t(r) + q] += t[q];
    }
  });
}

void Multigrid::smooth_fine(f64* x, const f64* b, bool x_zero) const {
  const i32 n = L_->n;
  const i64 m = 6 * i64(n);
  const f64 theta = 0.5 * (l0max_ + l0min_);
  const f64 delta = 0.5 * (l0max_ - l0min_);
  const f64 sigma = theta / delta;
  f64 rho = 1.0 / sigma;
  if (x_zero) {
    block_diag_apply(D0inv_, n, b, fr_.data());
  } else {
    apply_fine_operator(x, ft_.data());
    parallel_for(m, kVecGrain, [&](i64 lo, i64 hi) {
      for (i64 k = lo; k < hi; ++k) fq_[k] = b[k] - ft_[k];
    });
    block_diag_apply(D0inv_, n, fq_.data(), fr_.data());
  }
  parallel_for(m, kVecGrain, [&](i64 lo, i64 hi) {
    for (i64 k = lo; k < hi; ++k) fd_[k] = fr_[k] / theta;
  });
  for (int it = 0; it < opt_.cheb_degree_fine; ++it) {
    parallel_for(m, kVecGrain, [&](i64 lo, i64 hi) {
      for (i64 k = lo; k < hi; ++k) x[k] += fd_[k];
    });
    if (it + 1 == opt_.cheb_degree_fine) break;
    apply_fine_operator(fd_.data(), ft_.data());
    block_diag_apply(D0inv_, n, ft_.data(), fq_.data());
    const f64 rho_new = 1.0 / (2.0 * sigma - rho);
    const f64 c1 = rho_new * rho, c2 = 2.0 * rho_new / delta;
    parallel_for(m, kVecGrain, [&](i64 lo, i64 hi) {
      for (i64 k = lo; k < hi; ++k) {
        fr_[k] -= fq_[k];
        fd_[k] = c1 * fd_[k] + c2 * fr_[k];
      }
    });
    rho = rho_new;
  }
}

void Multigrid::smooth_coarse(const Level& lv, f64* x, const f64* b, bool x_zero) const {
  const i32 n = lv.n;
  const i64 m = 6 * i64(n);
  const f64 theta = 0.5 * (lv.lmax + lv.lmin);
  const f64 delta = 0.5 * (lv.lmax - lv.lmin);
  const f64 sigma = theta / delta;
  f64 rho = 1.0 / sigma;
  if (x_zero) {
    block_diag_apply(lv.Dinv, n, b, lv.r.data());
  } else {
    apply_level(lv, x, lv.t.data());
    for (i64 k = 0; k < m; ++k) lv.t[k] = b[k] - lv.t[k];
    block_diag_apply(lv.Dinv, n, lv.t.data(), lv.r.data());
  }
  for (i64 k = 0; k < m; ++k) lv.d[k] = lv.r[k] / theta;
  for (int it = 0; it < opt_.cheb_degree; ++it) {
    for (i64 k = 0; k < m; ++k) x[k] += lv.d[k];
    if (it + 1 == opt_.cheb_degree) break;
    apply_level(lv, lv.d.data(), lv.t.data());
    block_diag_apply(lv.Dinv, n, lv.t.data(), lv.b.data());
    const f64 rho_new = 1.0 / (2.0 * sigma - rho);
    const f64 c1 = rho_new * rho, c2 = 2.0 * rho_new / delta;
    for (i64 k = 0; k < m; ++k) {
      lv.r[k] -= lv.b[k];
      lv.d[k] = c1 * lv.d[k] + c2 * lv.r[k];
    }
    rho = rho_new;
  }
}

void Multigrid::sgs_sweep(const Level& lv, f64* x, const f64* b, bool forward) const {
  const i32* rp = lv.A.rowptr.data();
  const i32* cl = lv.A.col.data();
  if (lv.A.val.empty()) {  // f32-only level: widen once (SGS is not the f32 path's smoother)
    lv.A.val.assign(lv.A.valf.begin(), lv.A.valf.end());
  }
  const f64* vv = lv.A.val.data();
  const i32 n = lv.n;
  for (i32 t = 0; t < n; ++t) {
    const i32 r = forward ? t : n - 1 - t;
    f64 a[6] = {b[6 * size_t(r)], b[6 * size_t(r) + 1], b[6 * size_t(r) + 2],
                b[6 * size_t(r) + 3], b[6 * size_t(r) + 4], b[6 * size_t(r) + 5]};
    for (i32 k = rp[r]; k < rp[r + 1]; ++k) {
      const f64* B = vv + 36 * size_t(k);
      const f64* xc = x + 6 * size_t(cl[k]);
      for (int i = 0; i < 6; ++i)
        a[i] -= B[i * 6 + 0] * xc[0] + B[i * 6 + 1] * xc[1] + B[i * 6 + 2] * xc[2] + B[i * 6 + 3] * xc[3] +
                B[i * 6 + 4] * xc[4] + B[i * 6 + 5] * xc[5];
    }
    f64 d[6];
    mv6(&lv.Dinv[36 * size_t(r)], a, d);
    for (int i = 0; i < 6; ++i) x[6 * size_t(r) + i] += d[i];
  }
}

void Multigrid::restrict_to(const std::vector<i32>& parent, const std::vector<f64>& off, i32 n_fine, const f64* r,
                            i32 n_coarse, f64* rc) const {
  std::fill(rc, rc + 6 * size_t(n_coarse), 0.0);
  for (i32 c = 0; c < n_fine; ++c) {
    const i32 p = parent[c];
    if (p < 0) continue;
    const f64* f = r + 6 * size_t(c);
    const f64* o = &off[3 * size_t(c)];
    f64* R = rc + 6 * size_t(p);
    R[0] += f[0];
    R[1] += f[1];
    R[2] += f[2];
    // moment about parent: m + o x f
    R[3] += f[3] + o[1] * f[2] - o[2] * f[1];
    R[4] += f[4] + o[2] * f[0] - o[0] * f[2];
    R[5] += f[5] + o[0] * f[1] - o[1] * f[0];
  }
}

void Multigrid::prolong_add(const std::vector<i32>& parent, const std::vector<f64>& off, i32 n_fine, const f64* xc,
                            f64* x) const {
  parallel_for(n_fine, kGrain, [&](i64 b, i64 e) {
    for (i64 c = b; c < e; ++c) {
      const i32 p = parent[c];
      if (p < 0) continue;
      const f64* U = xc + 6 * size_t(p);
      const f64* o = &off[3 * size_t(c)];
      f64* u = x + 6 * size_t(c);
      u[0] += U[0] + U[4] * o[2] - U[5] * o[1];
      u[1] += U[1] + U[5] * o[0] - U[3] * o[2];
      u[2] += U[2] + U[3] * o[1] - U[4] * o[0];
      u[3] += U[3];
      u[4] += U[4];
      u[5] += U[5];
    }
  });
}

void Multigrid::vcycle_coarse(size_t l, const f64* b, f64* x) const {
  const Level& lv = lv_[l];
  const size_t m = 6 * size_t(lv.n);
  if (l + 1 == lv_.size()) {
    const i32 N = chol_n_;
    std::memcpy(x, b, sizeof(f64) * m);
    for (i32 i = 0; i < N; ++i) {
      f64 s = x[i];
      for (i32 k = 0; k < i; ++k) s -= chol_[size_t(i) * N + k] * x[k];
      x[i] = s / chol_[size_t(i) * N + i];
    }
    for (i32 i = N - 1; i >= 0; --i) {
      f64 s = x[i];
      for (i32 k = i + 1; k < N; ++k) s -= chol_[size_t(k) * N + i] * x[k];
      x[i] = s / chol_[size_t(i) * N + i];
    }
    return;
  }
  std::fill(x, x + m, 0.0);
  const Level& cl = lv_[l + 1];
  f64* res = lv.vres.data();
  f64* rc = cl.vrhs.data();
  f64* xc = cl.vsol.data();
  // Standard gamma-cycle: one pre-smoothing, gamma recursive coarse corrections, one
  // post-smoothing (the finest level is smoothed exactly once per cycle).
  const int gamma = (l + 2 == lv_.size()) ? 1 : std::max(1, opt_.cycle_gamma);
  if (opt_.sgs) sgs_sweep(lv, x, b, true);
  else smooth_coarse(lv, x, b, true);
  apply_level(lv, x, res);
  for (size_t k = 0; k < m; ++k) res[k] = b[k] - res[k];
  restrict_to(lv.parent, lv.off, lv.n, res, cl.n, rc);
  vcycle_coarse(l + 1, rc, xc);
  if (gamma > 1) {
    const size_t mc = 6 * size_t(cl.n);
    std::vector<f64>& rr = cl.vres2;
    std::vector<f64>& dd = cl.vtmp;
    rr.resize(mc);
    dd.resize(mc);
    for (int g = 1; g < gamma; ++g) {
      apply_level(cl, xc, rr.data());
      for (size_t k = 0; k < mc; ++k) rr[k] = rc[k] - rr[k];
      // the recursive call uses cl.vrhs / cl.vsol of the next level only
      vcycle_coarse_add(l + 1, rr.data(), dd.data());
      for (size_t k = 0; k < mc; ++k) xc[k] += dd[k];
    }
  }
  prolong_add(lv.parent, lv.off, lv.n, xc, x);
  if (opt_.sgs) sgs_sweep(lv, x, b, false);
  else smooth_coarse(lv, x, b, false);
}

void Multigrid::vcycle_coarse_add(size_t l, const f64* b, f64* x) const { vcycle_coarse(l, b, x); }

void Multigrid::apply(const f64* r, f64* z) const {
  if (assembled_) {
    vcycle_coarse(0, r, z);
    mask_fine(z);
    return;
  }
  const i32 n = L_->n;
  const size_t m = 6 * size_t(n);
  std::fill(z, z + m, 0.0);
  if (lv_.empty()) {
    block_diag_apply(D0inv_, n, r, z);
    return;
  }
  smooth_fine(z, r, true);
  fres_.resize(m);
  f64* res = fres_.data();
  apply_fine_operator(z, res);
  parallel_for(i64(m), kVecGrain, [&](i64 lo, i64 hi) {
    for (i64 k = lo; k < hi; ++k) res[k] = r[k] - res[k];
  });
  const Level& l1 = lv_[0];
  restrict_to(parent0_, off0_, n, res, l1.n, l1.vrhs.data());
  vcycle_coarse(0, l1.vrhs.data(), l1.vsol.data());
  prolong_add(parent0_, off0_, n, l1.vsol.data(), z);
  mask_fine(z);
  smooth_fine(z, r, false);
  mask_fine(z);
}

void Multigrid::mask_fine(f64* z) const {
  if (assembled_) {
    for (size_t i = 0; i < fix0_.size(); ++i) {
      const u8 m = fix0_[i];
      if (!m) continue;
      for (int q = 0; q < 6; ++q)
        if ((m >> q) & 1) z[6 * i + q] = 0.0;
    }
    return;
  }
  const Lattice& L = *L_;
  for (i32 i = 0; i < L.n; ++i) {
    const u8 m = L.fixmask[i];
    if (!m) continue;
    for (int q = 0; q < 6; ++q)
      if ((m >> q) & 1) z[6 * size_t(i) + q] = 0.0;
  }
}

const Bsr6* Multigrid::level_matrix(size_t l) const { return l < lv_.size() ? &lv_[l].A : nullptr; }

std::vector<i64> Multigrid::level_blocks() const {
  std::vector<i64> b;
  for (const Level& lv : lv_) b.push_back(lv.A.rowptr.empty() ? 0 : i64(lv.A.rowptr.back()));
  return b;
}

std::vector<i32> Multigrid::level_sizes() const {
  std::vector<i32> s;
  if (!assembled_) s.push_back(L_ ? L_->num_free() : 0);
  for (const Level& lv : lv_) s.push_back(lv.n);
  return s;
}

// ---------------------------------------------------------------------------
// PCG
// ---------------------------------------------------------------------------

PcgStats pcg_solve(const Multigrid& mg, i32 n_cells, const f64* f, f64* u, f64 rtol, int maxit, bool use_x0) {
  return pcg_solve_op([&](const f64* x, f64* y) { mg.apply_fine_operator(x, y); }, mg, n_cells, f, u, rtol, maxit,
                      use_x0);
}

PcgStats pcg_solve_op(const LinOp& A, const Multigrid& mg, i32 n_cells, const f64* f, f64* u, f64 rtol, int maxit,
                      bool use_x0, f64 atol) {
  return pcg_solve_op(A, [&mg](const f64* x, f64* y) { mg.apply(x, y); }, n_cells, f, u, rtol, maxit, use_x0, atol);
}

PcgStats pcg_solve_op(const LinOp& A, const LinOp& precond, i32 n_cells, const f64* f, f64* u, f64 rtol, int maxit,
                      bool use_x0, f64 atol) {
  const size_t m = 6 * size_t(n_cells);
  const i64 im = static_cast<i64>(m);
  std::vector<f64> r(m), z(m), p(m), Ap(m);
  if (!use_x0) std::fill(u, u + m, 0.0);
  A(u, Ap.data());
  parallel_for(im, kVecGrain, [&](i64 lo, i64 hi) {
    for (i64 k = lo; k < hi; ++k) r[k] = f[k] - Ap[k];
  });
  const f64 fn = std::sqrt(pdot(f, f, m));
  PcgStats st;
  if (fn == 0.0) {
    std::fill(u, u + m, 0.0);
    st.converged = true;
    return st;
  }
  f64 rn = std::sqrt(pdot(r.data(), r.data(), m));
  st.rel_res = rn / fn;
  if (st.rel_res < rtol || rn <= atol) {
    st.converged = true;
    return st;
  }
  precond(r.data(), z.data());
  p = z;
  f64 rz = pdot(r.data(), z.data(), m);
  for (int it = 1; it <= maxit; ++it) {
    add_work(n_cells);  // (deterministic work accounting: svx/base/work.hpp)
    A(p.data(), Ap.data());
    const f64 pAp = pdot(p.data(), Ap.data(), m);
    if (!(pAp > 0.0)) {  // negative curvature / breakdown
      st.iters = it;
      return st;
    }
    const f64 alpha = rz / pAp;
    parallel_for(im, kVecGrain, [&](i64 lo, i64 hi) {
      for (i64 k = lo; k < hi; ++k) {
        u[k] += alpha * p[k];
        r[k] -= alpha * Ap[k];
      }
    });
    rn = std::sqrt(pdot(r.data(), r.data(), m));
    st.iters = it;
    st.rel_res = rn / fn;
    if (st.rel_res < rtol || rn <= atol) {
      st.converged = true;
      return st;
    }
    precond(r.data(), z.data());
    const f64 rz_new = pdot(r.data(), z.data(), m);
    const f64 beta = rz_new / rz;
    rz = rz_new;
    parallel_for(im, kVecGrain, [&](i64 lo, i64 hi) {
      for (i64 k = lo; k < hi; ++k) p[k] = z[k] + beta * p[k];
    });
  }
  return st;
}

}  // namespace svx
