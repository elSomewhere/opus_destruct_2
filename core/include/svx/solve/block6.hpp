// structvox — dense 6x6 block helpers and a block-sparse (BSR6) builder shared by the
// multigrid and the composite (bubble) assembly.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "svx/solve/multigrid.hpp"

namespace svx::blk6 {

// Inverse of a 6x6 matrix via Gauss-Jordan with partial pivoting. Returns false if singular.
inline bool inv6(const f64* A, f64* Ainv) {
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
    if (std::abs(M[piv][c]) <= 1e-14 * scale) return false;
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

inline void mv6(const f64* __restrict M, const f64* __restrict x, f64* __restrict y) {
  const f64 x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3], x4 = x[4], x5 = x[5];
  for (int r = 0; r < 6; ++r) {
    const f64* m = M + r * 6;
    y[r] = m[0] * x0 + m[1] * x1 + m[2] * x2 + m[3] * x3 + m[4] * x4 + m[5] * x5;
  }
}

inline f64 fro6(const f64* M) {
  f64 s = 0.0;
  for (int i = 0; i < 36; ++i) s += M[i] * M[i];
  return std::sqrt(s);
}

// P = [[I, -[r]x], [0, I]]: maps parent rigid motion (U, Theta) to child (u, th).
inline void rigid_block(const f64* r, f64* P) {
  std::memset(P, 0, sizeof(f64) * 36);
  for (int i = 0; i < 6; ++i) P[i * 6 + i] = 1.0;
  P[0 * 6 + 4] = r[2];
  P[0 * 6 + 5] = -r[1];
  P[1 * 6 + 3] = -r[2];
  P[1 * 6 + 5] = r[0];
  P[2 * 6 + 3] = r[1];
  P[2 * 6 + 4] = -r[0];
}

// Zero the rows of a 6x6 prolongation block that map onto Dirichlet DOFs.
inline void mask_rows(u8 m, f64* P) {
  if (!m) return;
  for (int q = 0; q < 6; ++q)
    if ((m >> q) & 1)
      for (int c = 0; c < 6; ++c) P[q * 6 + c] = 0.0;
}

inline void mm6(const f64* A, const f64* B, f64* C) {
  for (int r = 0; r < 6; ++r)
    for (int c = 0; c < 6; ++c) {
      f64 s = 0.0;
      for (int k = 0; k < 6; ++k) s += A[r * 6 + k] * B[k * 6 + c];
      C[r * 6 + c] = s;
    }
}

// out += s * P^T * A * Q
inline void add_ptaq(const f64* P, const f64* A, const f64* Q, f64 s, f64* out) {
  f64 AQ[36];
  mm6(A, Q, AQ);
  for (int r = 0; r < 6; ++r)
    for (int c = 0; c < 6; ++c) {
      f64 acc = 0.0;
      for (int k = 0; k < 6; ++k) acc += P[k * 6 + r] * AQ[k * 6 + c];
      out[r * 6 + c] += s * acc;
    }
}

// out += s * P(oI)^T A P(oJ) for rigid prolongations P(o) = rigid_block(o) = [[I, R], [0, I]],
// R = -[o]x: the same product as add_ptaq on rigid blocks, from the 3x3 sub-blocks
//   [[A11, A11 RJ + A12], [RI^T A11 + A21, RI^T (A11 RJ + A12) + A21 RJ + A22]]
inline void add_rigid_ptaq(const f64* oI, const f64* A, const f64* oJ, f64 s, f64* out) {
  // R = -[o]x = [[0, o2, -o1], [-o2, 0, o0], [o1, -o0, 0]]
  const f64 RI[9] = {0.0, oI[2], -oI[1], -oI[2], 0.0, oI[0], oI[1], -oI[0], 0.0};
  const f64 RJ[9] = {0.0, oJ[2], -oJ[1], -oJ[2], 0.0, oJ[0], oJ[1], -oJ[0], 0.0};
  auto a = [&](int r, int c) { return A[r * 6 + c]; };
  f64 B12[9], B21[9];
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) {
      f64 x = a(r, 3 + c), y = a(3 + r, c);
      for (int q = 0; q < 3; ++q) {
        x += a(r, q) * RJ[q * 3 + c];  // (A11 RJ)
        y += RI[q * 3 + r] * a(q, c);  // (RI^T A11)
      }
      B12[r * 3 + c] = x;
      B21[r * 3 + c] = y;
    }
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) {
      f64 x = a(3 + r, 3 + c);
      for (int q = 0; q < 3; ++q) x += RI[q * 3 + r] * B12[q * 3 + c] + a(3 + r, q) * RJ[q * 3 + c];
      out[r * 6 + c] += s * a(r, c);
      out[r * 6 + 3 + c] += s * B12[r * 3 + c];
      out[(3 + r) * 6 + c] += s * B21[r * 3 + c];
      out[(3 + r) * 6 + 3 + c] += s * x;
    }
}

// out += G1^T diag(k) G2
inline void add_gtkg(const f64* G1, const f64* k, const f64* G2, f64* out) {
  for (int r = 0; r < 6; ++r)
    for (int c = 0; c < 6; ++c) {
      f64 acc = 0.0;
      for (int q = 0; q < 6; ++q) acc += G1[q * 6 + r] * k[q] * G2[q * 6 + c];
      out[r * 6 + c] += acc;
    }
}

struct UnionFind {
  std::vector<i32> p;
  explicit UnionFind(i32 n) : p(n) {
    for (i32 i = 0; i < n; ++i) p[i] = i;
  }
  i32 find(i32 x) {
    while (p[x] != x) {
      p[x] = p[p[x]];
      x = p[x];
    }
    return x;
  }
  void unite(i32 a, i32 b) {
    a = find(a);
    b = find(b);
    if (a == b) return;
    if (a < b)
      p[b] = a;
    else
      p[a] = b;
  }
};

// Two-pass BSR builder: register (row, col) pairs, then accumulate blocks.
struct BsrBuilder {
  i32 n;
  std::vector<u64> keys;
  explicit BsrBuilder(i32 n_) : n(n_) {}
  void touch(i32 r, i32 c) { keys.push_back((u64(u32(r)) << 32) | u32(c)); }
  Bsr6 finish_pattern() {
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    Bsr6 A;
    A.n = n;
    A.rowptr.assign(n + 1, 0);
    A.col.resize(keys.size());
    for (size_t k = 0; k < keys.size(); ++k) {
      const i32 r = static_cast<i32>(keys[k] >> 32);
      A.col[k] = static_cast<i32>(keys[k] & 0xffffffffu);
      A.rowptr[r + 1]++;
    }
    for (i32 r = 0; r < n; ++r) A.rowptr[r + 1] += A.rowptr[r];
    A.val.assign(36 * keys.size(), 0.0);
    std::vector<u64>().swap(keys);
    return A;
  }
};

inline f64* block_at(Bsr6& A, i32 r, i32 c) {
  const i32* b = A.col.data() + A.rowptr[r];
  const i32* e = A.col.data() + A.rowptr[r + 1];
  const i32* it = std::lower_bound(b, e, c);
  SVX_ASSERT(it != e && *it == c);
  return &A.val[36 * size_t(it - A.col.data())];
}

}  // namespace svx::blk6
