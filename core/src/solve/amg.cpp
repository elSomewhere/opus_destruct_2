#include "svx/solve/amg.hpp"

#include "svx/base/mem.hpp"
#include "svx/base/parallel.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace svx {

namespace {

// 4-lane single-precision vectors (NEON natively, SIMD128 in WASM); multiplies and adds stay
// separate (-ffp-contract=off): bit-identical everywhere. (GCC does not know clang's
// ext_vector_type - it would make f4 a plain float - but both know vector_size; a scalar is
// broadcast with splat4, which both compile to the same lanes.)
#if defined(__clang__)
typedef float f4 __attribute__((ext_vector_type(4)));
#else
typedef float f4 __attribute__((vector_size(16)));
#endif
static_assert(sizeof(f4) == 4 * sizeof(float), "f4: four lanes");
inline f4 splat4(float x) { return f4{x, x, x, x}; }
inline f4 ld4(const f32* p) {
  f4 v;
  __builtin_memcpy(&v, p, sizeof v);
  return v;
}
inline void st4(f32* p, f4 v) { __builtin_memcpy(p, &v, sizeof v); }
// (y0, y1) -= B x, B column-major padded (48 floats), x 8 floats (6 used)
inline void bsub(const f32* __restrict B, const f32* __restrict x, f4& y0, f4& y1) {
  for (int j = 0; j < 6; ++j) {
    const f4 xj = splat4(x[j]);
    y0 -= ld4(B + 8 * j) * xj;
    y1 -= ld4(B + 8 * j + 4) * xj;
  }
}
inline void badd(const f32* __restrict B, const f32* __restrict x, f4& y0, f4& y1) {
  for (int j = 0; j < 6; ++j) {
    const f4 xj = splat4(x[j]);
    y0 += ld4(B + 8 * j) * xj;
    y1 += ld4(B + 8 * j + 4) * xj;
  }
}
inline void to_f32_block(const f64* B, f32* out) {  // row-major f64 -> column-major padded f32
  for (int c = 0; c < 6; ++c) {
    for (int r = 0; r < 6; ++r) out[8 * c + r] = static_cast<f32>(B[r * 6 + c]);
    out[8 * c + 6] = out[8 * c + 7] = 0.0f;
  }
}

}  // namespace

namespace blk6 {

bool inv6(const f64* A, f64* Ainv) {
  f64 M[6][12];
  f64 scale = 0.0;
  for (int r = 0; r < 6; ++r) {
    for (int c = 0; c < 6; ++c) {
      M[r][c] = A[r * 6 + c];
      scale = std::max(scale, std::abs(M[r][c]));
    }
    for (int c = 0; c < 6; ++c) M[r][6 + c] = (r == c) ? 1.0 : 0.0;
  }
  if (scale == 0.0) return false;
  for (int c = 0; c < 6; ++c) {
    int piv = c;
    for (int r = c + 1; r < 6; ++r)
      if (std::abs(M[r][c]) > std::abs(M[piv][c])) piv = r;
    if (std::abs(M[piv][c]) <= 1e-15 * scale) return false;
    if (piv != c)
      for (int k = 0; k < 12; ++k) std::swap(M[c][k], M[piv][k]);
    const f64 inv = 1.0 / M[c][c];
    for (int k = 0; k < 12; ++k) M[c][k] *= inv;
    for (int r = 0; r < 6; ++r) {
      if (r == c) continue;
      const f64 f = M[r][c];
      if (f == 0.0) continue;
      for (int k = 0; k < 12; ++k) M[r][k] -= f * M[c][k];
    }
  }
  for (int r = 0; r < 6; ++r)
    for (int c = 0; c < 6; ++c) Ainv[r * 6 + c] = M[r][6 + c];
  return true;
}

f64 fro6(const f64* M) {
  f64 s = 0.0;
  for (int i = 0; i < 36; ++i) s += M[i] * M[i];
  return std::sqrt(s);
}

void rigid_block(const V3& r, f64* P) {
  std::memset(P, 0, sizeof(f64) * 36);
  for (int i = 0; i < 6; ++i) P[i * 6 + i] = 1.0;
  // u = U + Theta x r = U - [r]x Theta
  P[0 * 6 + 4] = r.z;
  P[0 * 6 + 5] = -r.y;
  P[1 * 6 + 3] = -r.z;
  P[1 * 6 + 5] = r.x;
  P[2 * 6 + 3] = r.y;
  P[2 * 6 + 4] = -r.x;
}

void atbd_add(const f64* A, const f64* B, const f64* D, f64 s, f64* C) {
  // T = B D, C += s A^T T
  f64 T[36];
  for (int r = 0; r < 6; ++r)
    for (int c = 0; c < 6; ++c) {
      f64 acc = 0.0;
      for (int k = 0; k < 6; ++k) acc += B[r * 6 + k] * D[k * 6 + c];
      T[r * 6 + c] = acc;
    }
  for (int r = 0; r < 6; ++r)
    for (int c = 0; c < 6; ++c) {
      f64 acc = 0.0;
      for (int k = 0; k < 6; ++k) acc += A[k * 6 + r] * T[k * 6 + c];
      C[r * 6 + c] += s * acc;
    }
}

}  // namespace blk6

void Bsr6::apply(const f64* x, f64* y) const {
  parallel_for(n, kRowGrain, [&](i64 r0, i64 r1) {
    for (i64 i = r0; i < r1; ++i) {
      f64* yi = y + 6 * size_t(i);
      for (int q = 0; q < 6; ++q) yi[q] = 0.0;
      for (i32 k = rowptr[size_t(i)]; k < rowptr[size_t(i) + 1]; ++k)
        blk6::mv6_add(&val[36 * size_t(k)], x + 6 * size_t(col[size_t(k)]), yi);
    }
  });
}

f64* Bsr6Builder::block(i32 r, i32 c) {
  auto& row = rows_[size_t(r)];
  for (const auto& e : row)
    if (e.first == c) return &store_[36 * size_t(e.second)];
  const i32 slot = static_cast<i32>(store_.size() / 36);
  store_.resize(store_.size() + 36, 0.0);
  row.push_back({c, slot});
  return &store_[36 * size_t(slot)];
}

Bsr6 Bsr6Builder::finish() {
  Bsr6 A;
  A.n = n_;
  A.rowptr.assign(size_t(n_) + 1, 0);
  size_t nnz = 0;
  for (i32 r = 0; r < n_; ++r) {
    auto& row = rows_[size_t(r)];
    // diagonal first, then ascending columns
    std::sort(row.begin(), row.end(), [r](const auto& a, const auto& b) {
      const bool da = a.first == r, db = b.first == r;
      if (da != db) return da;
      return a.first < b.first;
    });
    if (row.empty() || row.front().first != r) row.insert(row.begin(), {r, -1});
    nnz += row.size();
    A.rowptr[size_t(r) + 1] = static_cast<i32>(nnz);
  }
  A.col.resize(nnz);
  A.val.assign(36 * nnz, 0.0);
  size_t k = 0;
  for (i32 r = 0; r < n_; ++r)
    for (const auto& e : rows_[size_t(r)]) {
      A.col[k] = e.first;
      if (e.second >= 0) std::memcpy(&A.val[36 * k], &store_[36 * size_t(e.second)], sizeof(f64) * 36);
      ++k;
    }
  rows_.clear();
  store_.clear();
  return A;
}

// ---------------------------------------------------------------------------------------------

bool Amg::finalize(Level& L) {
  const i32 n = L.A.n;
  L.Dinv.assign(36 * size_t(n), 0.0);
  for (i32 i = 0; i < n; ++i) {
    const f64* D = &L.A.val[36 * size_t(L.A.rowptr[size_t(i)])];
    SVX_ASSERT(L.A.col[size_t(L.A.rowptr[size_t(i)])] == i);
    if (!blk6::inv6(D, &L.Dinv[36 * size_t(i)])) {
      // a node without stiffness (should not happen: every node has a bond or a pin)
      f64 Dr[36];
      std::memcpy(Dr, D, sizeof(Dr));
      f64 tr = 0.0;
      for (int q = 0; q < 6; ++q) tr += std::abs(Dr[q * 7]);
      const f64 eps = tr > 0 ? 1e-9 * tr : 1.0;
      for (int q = 0; q < 6; ++q) Dr[q * 7] += eps;
      if (!blk6::inv6(Dr, &L.Dinv[36 * size_t(i)])) return false;
    }
  }
  const size_t m = 6 * size_t(n);
  L.x.assign(m, 0.0);
  L.b.assign(m, 0.0);
  L.r.assign(m, 0.0);
  L.t.assign(m, 0.0);
  L.xold.assign(m, 0.0);
  build_work_ += 12 * static_cast<i64>(n);  // (the inverses)
  if (!opt_.sgs) L.lmax = estimate_lmax(L);
  return true;
}

f64 Amg::estimate_lmax(const Level& L) {
  // lambda_max(D^-1 A) by power iteration
  const i32 n = L.A.n;
  build_work_ += 15 * (L.A.blocks() + n);
  const size_t m = 6 * static_cast<size_t>(n);
  std::vector<f64> v(m), w(m);
  for (size_t k = 0; k < m; ++k) v[k] = 1.0 + 0.001 * static_cast<f64>(k % 7);
  f64 lam = 1.0;
  for (int it = 0; it < 15; ++it) {
    L.A.apply(v.data(), w.data());
    for (i32 i = 0; i < n; ++i) blk6::mv6(&L.Dinv[36 * size_t(i)], &w[6 * size_t(i)], &v[6 * size_t(i)]);
    f64 nv = 0.0;
    for (f64 q : v) nv += q * q;
    nv = std::sqrt(nv);
    if (!(nv > 0)) break;
    lam = nv;
    for (f64& q : v) q /= nv;
  }
  return 1.1 * lam;
}

bool Amg::build(const Bsr6& A, const std::vector<V3>& pos, const AmgOptions& opt) {
  opt_ = opt;
  lv_.clear();
  chol_.clear();
  chol_n_ = 0;
  work_ = 0;
  build_work_ = 0;
  if (A.n == 0) return true;
  lv_.emplace_back();
  lv_[0].A = A;
  lv_[0].pos = pos;
  if (!finalize(lv_[0])) {
    lv_.clear();
    return false;
  }
  while (static_cast<int>(lv_.size()) < opt_.max_levels && lv_.back().A.n > opt_.coarse_max) {
    // (a level grown dense, as on huge irregular structures, would make the next products
    // expensive and the cycle no cheaper: it becomes the coarsest - solved densely if it is small
    // enough, else coarsened once more by its aggregates' rigid motions alone, unsmoothed: cheap
    // products, and a coarsest level solved exactly instead of smoothed. AmgOptions::coarsen_dense)
    const Bsr6& L = lv_.back().A;
    const bool dense = lv_.size() > 1 && L.blocks() > 80 * static_cast<i64>(L.n);
    if (dense && !(opt_.coarsen_dense && L.n > 4 * opt_.coarse_max)) break;
    if (!coarsen(lv_.size() - 1, !dense && opt_.smoothed)) break;
    if (dense) break;
  }
  factor_coarsest();
  make_fast();
  for (size_t l = 0; l < lv_.size(); ++l) {
    // (the coarsest: a dense solve - its two triangles, of a block (36 flops) per pair of its
    // nodes - or 8 smoothing sweeps when coarsening stalled)
    const bool last = l + 1 == lv_.size();
    const i64 nd = opt_.dense_work_per_unknown ? chol_n_ : chol_n_ / 6;
    const i64 coarsest = chol_n_ > 0 ? std::max<i64>(lv_[l].A.blocks(), 2 * nd * nd) : 8 * 2 * lv_[l].A.blocks();
    const i64 per = last ? coarsest : lv_[l].A.blocks() * (4 * opt_.sweeps + 2);
    i64 g = 1;
    for (size_t k = 0; k < l; ++k) g *= opt_.gamma;
    work_ += per * g;
  }
  return true;
}

bool Amg::coarsen(size_t l, bool smoothed) {
  Level& F = lv_[l];
  const Bsr6& A = F.A;
  const i32 n = A.n;
  std::vector<f64> dn(static_cast<size_t>(n));
  for (i32 i = 0; i < n; ++i) dn[size_t(i)] = blk6::fro6(&A.val[36 * size_t(A.rowptr[size_t(i)])]);
  auto strong = [&](i32 i, i32 k) {
    const i32 j = A.col[size_t(k)];
    return j != i && blk6::fro6(&A.val[36 * size_t(k)]) >= opt_.strength * std::sqrt(dn[size_t(i)] * dn[size_t(j)]);
  };
  std::vector<i32> agg(size_t(n), -1);
  i32 na = 0;
  // pass 1: roots whose strong neighbourhood is free (rows without couplings - retired nodes -
  // are grouped in pass 3: as singleton roots they would stall the coarsening)
  auto isolated = [&](i32 i) { return A.rowptr[size_t(i) + 1] - A.rowptr[size_t(i)] <= 1; };
  for (i32 i = 0; i < n; ++i) {
    if (agg[size_t(i)] >= 0 || isolated(i)) continue;
    bool free = true;
    for (i32 k = A.rowptr[size_t(i)]; k < A.rowptr[size_t(i) + 1] && free; ++k)
      if (strong(i, k) && agg[size_t(A.col[size_t(k)])] >= 0) free = false;
    if (!free) continue;
    agg[size_t(i)] = na;
    for (i32 k = A.rowptr[size_t(i)]; k < A.rowptr[size_t(i) + 1]; ++k)
      if (strong(i, k)) agg[size_t(A.col[size_t(k)])] = na;
    ++na;
  }
  // pass 2: join the most strongly coupled aggregated neighbour
  std::vector<i32> agg1 = agg;
  for (i32 i = 0; i < n; ++i) {
    if (agg1[size_t(i)] >= 0) continue;
    f64 best = -1.0;
    i32 ba = -1;
    for (i32 k = A.rowptr[size_t(i)]; k < A.rowptr[size_t(i) + 1]; ++k) {
      const i32 j = A.col[size_t(k)];
      if (j == i || agg1[size_t(j)] < 0) continue;
      const f64 s = blk6::fro6(&A.val[36 * size_t(k)]);
      if (s > best) {
        best = s;
        ba = agg1[size_t(j)];
      }
    }
    agg[size_t(i)] = ba;
  }
  // pass 3: leftovers. Nodes without any coupling (retired rows) are grouped eight at a time
  // (they are decoupled: any grouping is exact for them), others become singletons.
  {
    i32 open = -1, fill = 0;
    for (i32 i = 0; i < n; ++i) {
      if (agg[size_t(i)] >= 0) continue;
      if (!isolated(i)) {
        agg[size_t(i)] = na++;
        continue;
      }
      if (open < 0 || fill >= 8) {
        open = na++;
        fill = 0;
      }
      agg[size_t(i)] = open;
      ++fill;
    }
  }
  if (na >= n || static_cast<f64>(na) > 0.85 * n) return false;

  Level C;
  C.pos.assign(size_t(na), V3{});
  std::vector<i32> cnt(size_t(na), 0);
  for (i32 i = 0; i < n; ++i) {
    C.pos[size_t(agg[size_t(i)])] += F.pos[size_t(i)];
    ++cnt[size_t(agg[size_t(i)])];
  }
  for (i32 a = 0; a < na; ++a) C.pos[size_t(a)] *= 1.0 / cnt[size_t(a)];
  F.parent = agg;
  F.off.resize(static_cast<size_t>(n));
  for (i32 i = 0; i < n; ++i) F.off[size_t(i)] = F.pos[size_t(i)] - C.pos[size_t(agg[size_t(i)])];
  // (rows of the products below are independent - any partition gives the same - in chunks of
  // about as many blocks: dense coarse levels' rows are many times their fine levels')
  const i64 grain = std::clamp<i64>(kRowGrain * 2 * n / std::max<i64>(1, A.blocks()), 8, kRowGrain / 2);
  // prolongation: rigid-body modes of each aggregate, smoothed by one damped Jacobi step
  {
    std::vector<std::vector<std::pair<i32, std::array<f64, 36>>>> rows(static_cast<size_t>(n));
    const f64 omega = smoothed ? (4.0 / 3.0) / std::max(1e-300, estimate_lmax(F)) : 0.0;
    parallel_for(n, grain, [&](i64 i0, i64 i1) {
    f64 Pt[36];
    // (a sparse accumulator: the row's entry of each coarse node, reset after the row)
    std::vector<i32> slot(static_cast<size_t>(na), -1);
    auto add = [&](std::vector<std::pair<i32, std::array<f64, 36>>>& row, i32 c, const f64* blk, f64 sc) {
      i32& at = slot[size_t(c)];
      if (at < 0) {
        at = static_cast<i32>(row.size());
        row.push_back({c, {}});
      }
      auto& b = row[size_t(at)].second;
      for (int q = 0; q < 36; ++q) b[size_t(q)] += sc * blk[q];
    };
    for (i64 i = i0; i < i1; ++i) {
      auto& row = rows[size_t(i)];
      blk6::rigid_block(F.off[size_t(i)], Pt);
      add(row, agg[size_t(i)], Pt, 1.0);
      const f64* Di = &F.Dinv[36 * size_t(i)];
      // (unsmoothed: the aggregate's rigid motion alone; the row's slots are reset all the same)
      for (i32 k = A.rowptr[size_t(i)]; k < (omega != 0.0 ? A.rowptr[size_t(i) + 1] : A.rowptr[size_t(i)]); ++k) {
        const i32 j = A.col[size_t(k)];
        f64 Pj[36], T[36], U[36];
        blk6::rigid_block(F.off[size_t(j)], Pj);
        // U = Dinv_i A_ij Pt_j
        for (int r = 0; r < 6; ++r)
          for (int c = 0; c < 6; ++c) {
            f64 acc = 0.0;
            for (int q = 0; q < 6; ++q) acc += A.val[36 * size_t(k) + size_t(r * 6 + q)] * Pj[q * 6 + c];
            T[r * 6 + c] = acc;
          }
        for (int r = 0; r < 6; ++r)
          for (int c = 0; c < 6; ++c) {
            f64 acc = 0.0;
            for (int q = 0; q < 6; ++q) acc += Di[r * 6 + q] * T[q * 6 + c];
            U[r * 6 + c] = acc;
          }
        add(row, agg[size_t(j)], U, -omega);
      }
      for (const auto& e : row) slot[size_t(e.first)] = -1;
    }
    });
    F.Prow.assign(static_cast<size_t>(n) + 1, 0);
    F.Pcol.clear();
    F.Pval.clear();
    for (i32 i = 0; i < n; ++i) {
      auto& row = rows[size_t(i)];
      std::sort(row.begin(), row.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
      for (const auto& e : row) {
        F.Pcol.push_back(e.first);
        F.Pval.insert(F.Pval.end(), e.second.begin(), e.second.end());
      }
      F.Prow[size_t(i) + 1] = static_cast<i32>(F.Pcol.size());
    }
  }
  // (the build's work: the smoothing's two products a block of A, A P's a block of A and of P
  // each, below)
  if (smoothed) build_work_ += 12 * A.blocks();
  for (i32 i = 0; i < n; ++i)
    for (i32 k = A.rowptr[size_t(i)]; k < A.rowptr[size_t(i) + 1]; ++k) {
      const i32 j = A.col[size_t(k)];
      build_work_ += 6 * static_cast<i64>(F.Prow[size_t(j) + 1] - F.Prow[size_t(j)]);
    }
  // restriction lists: P^T by coarse node
  F.Rrow.assign(static_cast<size_t>(na) + 1, 0);
  for (size_t e = 0; e < F.Pcol.size(); ++e) ++F.Rrow[size_t(F.Pcol[e]) + 1];
  for (i32 a = 0; a < na; ++a) F.Rrow[size_t(a) + 1] += F.Rrow[size_t(a)];
  F.Rent.assign(F.Pcol.size(), {0, 0});
  {
    std::vector<i32> fill(F.Rrow.begin(), F.Rrow.end() - 1);
    for (i32 i = 0; i < n; ++i)
      for (i32 e = F.Prow[size_t(i)]; e < F.Prow[size_t(i) + 1]; ++e) F.Rent[size_t(fill[size_t(F.Pcol[size_t(e)])]++)] = {i, e};
  }
  F.xc.assign(6 * static_cast<size_t>(na), 0.0);
  // Galerkin product A_c = P^T A P: (A P) by fine row, then P^T (A P) by coarse row (parallel,
  // each row in a fixed order: the same for any thread count)
  std::vector<std::vector<std::pair<i32, std::array<f64, 36>>>> apr(static_cast<size_t>(n));
  parallel_for(n, grain, [&](i64 i0, i64 i1) {
    std::vector<i32> slot(static_cast<size_t>(na), -1);
    for (i64 i = i0; i < i1; ++i) {
      auto& ap = apr[size_t(i)];
      for (i32 k = A.rowptr[size_t(i)]; k < A.rowptr[size_t(i) + 1]; ++k) {
        const i32 j = A.col[size_t(k)];
        const f64* Aij = &A.val[36 * size_t(k)];
        for (i32 e = F.Prow[size_t(j)]; e < F.Prow[size_t(j) + 1]; ++e) {
          const i32 J = F.Pcol[size_t(e)];
          const f64* Pj = &F.Pval[36 * size_t(e)];
          i32& at = slot[size_t(J)];
          if (at < 0) {
            at = static_cast<i32>(ap.size());
            ap.push_back({J, {}});
          }
          std::array<f64, 36>* dst = &ap[size_t(at)].second;
          for (int r = 0; r < 6; ++r)
            for (int c = 0; c < 6; ++c) {
              f64 acc = 0.0;
              for (int q = 0; q < 6; ++q) acc += Aij[r * 6 + q] * Pj[q * 6 + c];
              (*dst)[size_t(r * 6 + c)] += acc;
            }
        }
      }
      for (const auto& x : ap) slot[size_t(x.first)] = -1;
    }
  });
  for (i32 i = 0; i < n; ++i) build_work_ += 6 * static_cast<i64>(F.Prow[size_t(i) + 1] - F.Prow[size_t(i)]) * static_cast<i64>(apr[size_t(i)].size());
  std::vector<std::vector<std::pair<i32, std::array<f64, 36>>>> crow(static_cast<size_t>(na));
  parallel_for(na, std::max<i64>(4, grain / 2), [&](i64 a0, i64 a1) {
    std::vector<i32> slot(static_cast<size_t>(na), -1);
    for (i64 I = a0; I < a1; ++I) {
      auto& out = crow[size_t(I)];
      for (i32 r = F.Rrow[size_t(I)]; r < F.Rrow[size_t(I) + 1]; ++r) {
        const auto& [i, e] = F.Rent[size_t(r)];
        const f64* Pi = &F.Pval[36 * size_t(e)];
        for (const auto& x : apr[size_t(i)]) {
          i32& at = slot[size_t(x.first)];
          if (at < 0) {
            at = static_cast<i32>(out.size());
            out.push_back({x.first, {}});
          }
          std::array<f64, 36>* dst = &out[size_t(at)].second;
          for (int rr = 0; rr < 6; ++rr)
            for (int c = 0; c < 6; ++c) {
              f64 acc = 0.0;
              for (int q = 0; q < 6; ++q) acc += Pi[q * 6 + rr] * x.second[size_t(q * 6 + c)];
              (*dst)[size_t(rr * 6 + c)] += acc;
            }
        }
      }
      for (const auto& y : out) slot[size_t(y.first)] = -1;
    }
  });
  apr.clear();
  Bsr6Builder B(na);
  for (i32 I = 0; I < na; ++I)
    for (const auto& y : crow[size_t(I)]) {
      f64* dst = B.block(I, y.first);
      for (int q = 0; q < 36; ++q) dst[q] += y.second[size_t(q)];
    }
  crow.clear();
  C.A = B.finish();
  if (!smoothed && opt_.coarse_scale != 1.0) {
    // scale the coarse operator (unsmoothed aggregation is over-stiff); the diagonal too
    for (f64& v : C.A.val) v *= opt_.coarse_scale;
  }
  if (!finalize(C)) return false;
  lv_.push_back(std::move(C));
  return true;
}

void Amg::factor_coarsest() {
  const Level& L = lv_.back();
  if (L.A.n > 4 * opt_.coarse_max) {
    // coarsening stalled: no dense factor (O(n^3)); the coarsest level is smoothed instead
    chol_n_ = 0;
    chol_.clear();
    return;
  }
  const i32 m = 6 * L.A.n;
  chol_n_ = m;
  build_work_ += static_cast<i64>(m) * m * m / 108;  // (m^3 / 3 flops, 36 a block)
  chol_.assign(size_t(m) * size_t(m), 0.0);
  for (i32 i = 0; i < L.A.n; ++i)
    for (i32 k = L.A.rowptr[size_t(i)]; k < L.A.rowptr[size_t(i) + 1]; ++k) {
      const i32 j = L.A.col[size_t(k)];
      const f64* b = &L.A.val[36 * size_t(k)];
      for (int r = 0; r < 6; ++r)
        for (int c = 0; c < 6; ++c) chol_[size_t(6 * i + r) * size_t(m) + size_t(6 * j + c)] += b[r * 6 + c];
    }
  // symmetrize (roundoff) and factor in place: lower triangle L L^T
  f64 dmax = 0.0;
  for (i32 i = 0; i < m; ++i) dmax = std::max(dmax, std::abs(chol_[size_t(i) * size_t(m) + size_t(i)]));
  for (i32 j = 0; j < m; ++j) {
    f64 d = chol_[size_t(j) * size_t(m) + size_t(j)];
    for (i32 k = 0; k < j; ++k) d -= chol_[size_t(j) * size_t(m) + size_t(k)] * chol_[size_t(j) * size_t(m) + size_t(k)];
    if (!(d > 1e-12 * dmax)) d = std::max(1e-12 * dmax, 1e-300);
    const f64 s = std::sqrt(d);
    chol_[size_t(j) * size_t(m) + size_t(j)] = s;
    // (the rows below, each on its own: in parallel, the same for any thread count - chunks of
    // some 64k flops)
    parallel_for(m - j - 1, std::max<i64>(8, 65536 / std::max(1, j)), [&](i64 i0, i64 i1) {
      for (i64 ii = i0; ii < i1; ++ii) {
        const i32 i = j + 1 + static_cast<i32>(ii);
        f64 v = 0.5 * (chol_[size_t(i) * size_t(m) + size_t(j)] + chol_[size_t(j) * size_t(m) + size_t(i)]);
        for (i32 k = 0; k < j; ++k) v -= chol_[size_t(i) * size_t(m) + size_t(k)] * chol_[size_t(j) * size_t(m) + size_t(k)];
        chol_[size_t(i) * size_t(m) + size_t(j)] = v / s;
      }
    });
  }
  // (the factor's transpose into the upper triangle - A's, used by the factorization alone - so
  // the backward pass reads rows, not columns a row apart: the same products in the same order)
  for (i32 i = 0; i < m; ++i)
    for (i32 k = i + 1; k < m; ++k) chol_[size_t(i) * size_t(m) + size_t(k)] = chol_[size_t(k) * size_t(m) + size_t(i)];
}

void Amg::solve_coarsest(const f64* b, f64* x) const {
  const i32 m = chol_n_;
  for (i32 i = 0; i < m; ++i) {
    f64 v = b[i];
    for (i32 k = 0; k < i; ++k) v -= chol_[size_t(i) * size_t(m) + size_t(k)] * x[k];
    x[i] = v / chol_[size_t(i) * size_t(m) + size_t(i)];
  }
  for (i32 i = m - 1; i >= 0; --i) {
    f64 v = x[i];
    for (i32 k = i + 1; k < m; ++k) v -= chol_[size_t(i) * size_t(m) + size_t(k)] * x[k];
    x[i] = v / chol_[size_t(i) * size_t(m) + size_t(i)];
  }
}

void Amg::make_fast() {
  for (size_t l = 0; l < lv_.size(); ++l) {
    Level& L = lv_[l];
    const size_t nb = static_cast<size_t>(L.A.blocks());
    L.Af.assign(48 * nb, 0.0f);
    for (size_t k = 0; k < nb; ++k) to_f32_block(&L.A.val[36 * k], &L.Af[48 * k]);
    L.Df.assign(48 * size_t(L.A.n), 0.0f);
    for (i32 i = 0; i < L.A.n; ++i) to_f32_block(&L.Dinv[36 * size_t(i)], &L.Df[48 * size_t(i)]);
    L.Pf.assign(48 * L.Pcol.size(), 0.0f);
    for (size_t e = 0; e < L.Pcol.size(); ++e) to_f32_block(&L.Pval[36 * e], &L.Pf[48 * e]);
    L.Rf.assign(48 * L.Rent.size(), 0.0f);
    for (size_t r = 0; r < L.Rent.size(); ++r) {
      const f64* P = &L.Pval[36 * size_t(L.Rent[r].second)];
      f64 T[36];
      for (int a = 0; a < 6; ++a)
        for (int b = 0; b < 6; ++b) T[a * 6 + b] = P[b * 6 + a];
      to_f32_block(T, &L.Rf[48 * r]);
    }
    const size_t m = 8 * size_t(L.A.n);
    L.xf.assign(m, 0.0f);
    L.bf.assign(m, 0.0f);
    L.rf.assign(m, 0.0f);
    L.of.assign(m, 0.0f);
    L.cf.assign(m, 0.0f);
    // the cycle runs on the single-precision copies and the pattern: the double-precision
    // blocks and the build's work arrays go (level 0's matrix is the caller's K again)
    std::vector<f64>().swap(L.A.val);
    std::vector<f64>().swap(L.Dinv);
    std::vector<f64>().swap(L.Pval);
    std::vector<V3>().swap(L.pos);
    std::vector<V3>().swap(L.off);
    std::vector<i32>().swap(L.parent);
    for (std::vector<f64>* v : {&L.x, &L.b, &L.r, &L.t, &L.xc, &L.xold}) std::vector<f64>().swap(*v);
  }
}

void Amg::smooth_f(const Level& L, f32* x, const f32* b, bool fresh, bool backward) const {
  // hybrid Gauss-Seidel over fixed row blocks (the other blocks' values from before the sweep)
  const Bsr6& A = L.A;
  const i32 n = A.n;
  if (!fresh) std::copy(x, x + 8 * size_t(n), L.of.begin());
  const f32* xo = L.of.data();
  parallel_for(n, kRowGrain, [&](i64 r0, i64 r1) {
    alignas(16) f32 sv[8];
    for (i64 ii = 0; ii < r1 - r0; ++ii) {
      const i64 i = backward ? r1 - 1 - ii : r0 + ii;
      f4 s0 = ld4(b + 8 * i), s1 = ld4(b + 8 * i + 4);
      const i32 k1 = A.rowptr[size_t(i) + 1];
      for (i32 k = A.rowptr[size_t(i)] + 1; k < k1; ++k) {
        const i64 j = A.col[size_t(k)];
        if (j >= r0 && j < r1) {
          if (fresh && j > i) continue;  // (still zero)
          bsub(&L.Af[48 * size_t(k)], x + 8 * j, s0, s1);
        } else if (!fresh) {
          bsub(&L.Af[48 * size_t(k)], xo + 8 * j, s0, s1);
        }
      }
      st4(sv, s0);
      st4(sv + 4, s1);
      f4 y0 = splat4(0.0f), y1 = splat4(0.0f);
      badd(&L.Df[48 * size_t(i)], sv, y0, y1);
      st4(x + 8 * i, y0);
      st4(x + 8 * i + 4, y1);
    }
  });
}

void Amg::cycle_f(size_t l, const f32* b, f32* x) const {
  const Level& L = lv_[l];
  const i32 n = L.A.n;
  if (l + 1 == lv_.size() && chol_n_ == 0) {
    // (stalled coarsening) a few symmetric sweeps instead of a direct solve
    for (int sw = 0; sw < 4; ++sw) smooth_f(L, x, b, sw == 0, false);
    for (int sw = 0; sw < 4; ++sw) smooth_f(L, x, b, false, true);
    return;
  }
  if (l + 1 == lv_.size()) {
    std::vector<f64> bd(6 * size_t(n)), xd(6 * size_t(n));
    for (i32 i = 0; i < n; ++i)
      for (int q = 0; q < 6; ++q) bd[6 * size_t(i) + q] = b[8 * size_t(i) + q];
    solve_coarsest(bd.data(), xd.data());
    for (i32 i = 0; i < n; ++i) {
      for (int q = 0; q < 6; ++q) x[8 * size_t(i) + q] = static_cast<f32>(xd[6 * size_t(i) + q]);
      x[8 * size_t(i) + 6] = x[8 * size_t(i) + 7] = 0.0f;
    }
    return;
  }
  for (int sw = 0; sw < opt_.sweeps; ++sw) smooth_f(L, x, b, sw == 0, false);
  // residual
  const Bsr6& A = L.A;
  f32* r = L.rf.data();
  parallel_for(n, kRowGrain, [&](i64 r0, i64 r1) {
    for (i64 i = r0; i < r1; ++i) {
      f4 s0 = ld4(b + 8 * i), s1 = ld4(b + 8 * i + 4);
      for (i32 k = A.rowptr[size_t(i)]; k < A.rowptr[size_t(i) + 1]; ++k) bsub(&L.Af[48 * size_t(k)], x + 8 * size_t(A.col[size_t(k)]), s0, s1);
      st4(r + 8 * i, s0);
      st4(r + 8 * i + 4, s1);
    }
  });
  // restriction
  const Level& C = lv_[l + 1];
  f32* bc = C.bf.data();
  parallel_for(C.A.n, kRowGrain, [&](i64 a0, i64 a1) {
    for (i64 a = a0; a < a1; ++a) {
      f4 y0 = splat4(0.0f), y1 = splat4(0.0f);
      for (i32 e = L.Rrow[size_t(a)]; e < L.Rrow[size_t(a) + 1]; ++e) badd(&L.Rf[48 * size_t(e)], r + 8 * size_t(L.Rent[size_t(e)].first), y0, y1);
      st4(bc + 8 * a, y0);
      st4(bc + 8 * a + 4, y1);
    }
  });
  f32* xc = L.cf.data();  // (the coarse correction lives in this level's spare buffer, sized for C)
  cycle_f(l + 1, bc, xc);
  // prolongation
  parallel_for(n, kRowGrain, [&](i64 r0, i64 r1) {
    for (i64 i = r0; i < r1; ++i) {
      f4 y0 = ld4(x + 8 * i), y1 = ld4(x + 8 * i + 4);
      for (i32 e = L.Prow[size_t(i)]; e < L.Prow[size_t(i) + 1]; ++e) badd(&L.Pf[48 * size_t(e)], xc + 8 * size_t(L.Pcol[size_t(e)]), y0, y1);
      st4(x + 8 * i, y0);
      st4(x + 8 * i + 4, y1);
    }
  });
  for (int sw = 0; sw < opt_.sweeps; ++sw) smooth_f(L, x, b, false, true);
}

void Amg::apply(const f64* r, f64* z) const {
  if (lv_.empty()) return;
  const Level& L = lv_[0];
  const i32 n = L.A.n;
  f32* b = L.bf.data();
  for (i32 i = 0; i < n; ++i) {
    for (int q = 0; q < 6; ++q) b[8 * size_t(i) + q] = static_cast<f32>(r[6 * size_t(i) + q]);
    b[8 * size_t(i) + 6] = b[8 * size_t(i) + 7] = 0.0f;
  }
  f32* x = L.xf.data();
  cycle_f(0, b, x);
  for (i32 i = 0; i < n; ++i)
    for (int q = 0; q < 6; ++q) z[6 * size_t(i) + q] = x[8 * size_t(i) + q];
}

i64 Bsr6::memory_bytes(Bytes kind) const { return vec_bytes(rowptr, kind) + vec_bytes(col, kind) + vec_bytes(val, kind); }

i64 Amg::memory_bytes(Bytes kind) const {
  // (a level's own record - of pointer-sized containers - as a fixed size in what is used)
  i64 b = vec_bytes(chol_, kind) + (kind == Bytes::Held ? static_cast<i64>(lv_.capacity() * sizeof(Level)) : static_cast<i64>(lv_.size()) * 1024);
  for (const Level& L : lv_) {
    b += L.A.memory_bytes(kind) + vec_bytes(L.Dinv, kind) + vec_bytes(L.pos, kind) + vec_bytes(L.parent, kind) + vec_bytes(L.off, kind);
    b += vec_bytes(L.Prow, kind) + vec_bytes(L.Pcol, kind) + vec_bytes(L.Pval, kind) + vec_bytes(L.Rrow, kind) + vec_bytes(L.Rent, kind);
    b += vec_bytes(L.x, kind) + vec_bytes(L.b, kind) + vec_bytes(L.r, kind) + vec_bytes(L.t, kind) + vec_bytes(L.xc, kind) + vec_bytes(L.xold, kind);
    b += vec_bytes(L.Af, kind) + vec_bytes(L.Df, kind) + vec_bytes(L.Pf, kind) + vec_bytes(L.Rf, kind);
    b += vec_bytes(L.xf, kind) + vec_bytes(L.bf, kind) + vec_bytes(L.rf, kind) + vec_bytes(L.of, kind) + vec_bytes(L.cf, kind);
  }
  return b;
}

std::vector<i64> Amg::level_blocks() const {
  std::vector<i64> s;
  for (const auto& L : lv_) s.push_back(L.A.blocks());
  for (const auto& L : lv_) s.push_back(static_cast<i64>(L.Pcol.size()));
  return s;
}

std::vector<i32> Amg::level_sizes() const {
  std::vector<i32> s;
  for (const auto& L : lv_) s.push_back(L.A.n);
  return s;
}

// ---------------------------------------------------------------------------------------------

PcgResult pcg(const Bsr6& A, const Amg* M, const f64* b, f64* x, f64 rtol, int maxit, bool use_x0, f64 atol) {
  PcgResult res;
  const size_t m = 6 * size_t(A.n);
  if (m == 0) {
    res.converged = true;
    return res;
  }
  if (!use_x0) std::fill(x, x + m, 0.0);
  std::vector<f64> r(m), z(m), p(m), q(m);
  A.apply(x, q.data());
  f64 bn = 0.0, rn = 0.0;
  for (size_t k = 0; k < m; ++k) {
    r[k] = b[k] - q[k];
    bn += b[k] * b[k];
    rn += r[k] * r[k];
  }
  bn = std::sqrt(bn);
  rn = std::sqrt(rn);
  const f64 target = std::max(rtol * bn, atol);
  res.rel_res = bn > 0 ? rn / bn : 0.0;
  if (rn <= target || !(bn > 0)) {
    res.converged = true;
    return res;
  }
  auto precond = [&](const f64* in, f64* out) {
    if (M && M->built()) {
      M->apply(in, out);
    } else {
      std::copy(in, in + m, out);
    }
  };
  precond(r.data(), z.data());
  p = z;
  f64 rz = 0.0;
  for (size_t k = 0; k < m; ++k) rz += r[k] * z[k];
  for (int it = 0; it < maxit; ++it) {
    A.apply(p.data(), q.data());
    f64 pq = 0.0;
    for (size_t k = 0; k < m; ++k) pq += p[k] * q[k];
    res.iters = it + 1;
    if (!(pq > 0)) break;  // not SPD along p (should not happen)
    const f64 alpha = rz / pq;
    rn = 0.0;
    for (size_t k = 0; k < m; ++k) {
      x[k] += alpha * p[k];
      r[k] -= alpha * q[k];
      rn += r[k] * r[k];
    }
    rn = std::sqrt(rn);
    res.rel_res = rn / bn;
    if (rn <= target) {
      res.converged = true;
      break;
    }
    precond(r.data(), z.data());
    f64 rz1 = 0.0;
    for (size_t k = 0; k < m; ++k) rz1 += r[k] * z[k];
    const f64 beta = rz1 / rz;
    rz = rz1;
    for (size_t k = 0; k < m; ++k) p[k] = z[k] + beta * p[k];
  }
  return res;
}

}  // namespace svx
