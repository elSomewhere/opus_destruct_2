#include "svx/stress/stress.hpp"

#include "svx/base/diag.hpp"
#include "svx/base/parallel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace svx {

BondStrength bond_strength(const SBond& b, f64 fragility) {
  const Material& A = material(b.ma);
  const Material& B = material(b.mb);
  const f64 s = b.strength / std::max(1e-6, fragility);
  return {s * std::min(A.ft, B.ft), s * std::min(A.fb, B.fb), s * std::min(A.fc, B.fc), s * std::min(A.cohesion, B.cohesion),
          std::min(A.friction, B.friction)};
}

f64 bond_utilization(const SBond& b, const BondLoad& L, f64 fragility, FailMode* mode) {
  const BondStrength S = bond_strength(b, fragility);
  const f64 A = std::max(b.area, 1e-12);
  const f64 sN = L.N / A;
  const f64 sb = (b.s2 > 0 ? std::abs(L.M1) * b.c2 / b.s2 : 0.0) + (b.s1 > 0 ? std::abs(L.M2) * b.c1 / b.s1 : 0.0);
  f64 phi_t;
  if (sN >= 0.0) {
    phi_t = sN / S.ft + sb / S.fb;
  } else {
    phi_t = std::max(0.0, sb + sN) / S.fb;  // compression pre-stresses the section
  }
  const f64 phi_c = std::max(0.0, sb - sN) / S.fc;
  const f64 J = std::max(b.s1 + b.s2, 1e-18);
  const f64 tau = 1.5 * std::sqrt(L.V1 * L.V1 + L.V2 * L.V2) / A + std::abs(L.T) * b.rmax / J;
  const f64 cap = S.coh + S.mu * std::max(0.0, -sN);
  const f64 phi_s = tau / cap;
  f64 phi = phi_t;
  FailMode m = FailMode::Tension;
  if (phi_c > phi) {
    phi = phi_c;
    m = FailMode::Crush;
  }
  if (phi_s > phi) {
    phi = phi_s;
    m = FailMode::Shear;
  }
  if (mode) *mode = phi > 0 ? m : FailMode::None;
  return phi;
}

void StressProblem::bond_matrices(const SBond& b, f64 D[36], f64 Ba[36], f64 Bb[36]) const {
  const Material& A = material(b.ma);
  const Material& B = material(b.mb);
  const f64 la = std::max(b.la, 1e-4), lb = std::max(b.lb, 1e-4);
  const f64 invE = la / A.E + lb / B.E;
  const f64 invG = la / A.G + lb / B.G;
  // local stiffness: (n, t1, t2) translations, (n, t1, t2) rotations
  const f64 kt[3] = {b.area / invE, (5.0 / 6.0) * b.area / invG, (5.0 / 6.0) * b.area / invG};
  const f64 kr[3] = {0.8 * (b.s1 + b.s2) / invG, b.s2 / invE, b.s1 / invE};
  const V3 Q[3] = {b.n, b.t1, b.t2};
  std::fill(D, D + 36, 0.0);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) {
      f64 st = 0.0, sr = 0.0;
      for (int k = 0; k < 3; ++k) {
        st += Q[k][r] * kt[k] * Q[k][c];
        sr += Q[k][r] * kr[k] * Q[k][c];
      }
      D[r * 6 + c] = st;
      D[(3 + r) * 6 + 3 + c] = sr;
    }
  blk6::rigid_block(b.p - nodes[size_t(b.a)].c, Ba);
  if (b.b >= 0) blk6::rigid_block(b.p - nodes[size_t(b.b)].c, Bb);
}

bool StressProblem::build_matrix() {
  const i32 n = static_cast<i32>(nodes.size());
  dof_.assign(size_t(n), -1);
  nfree_ = 0;
  for (i32 i = 0; i < n; ++i)
    if (!nodes[size_t(i)].fixed) dof_[size_t(i)] = nfree_++;
  Bsr6Builder B(nfree_);
  for (i32 i = 0; i < n; ++i)
    if (dof_[size_t(i)] >= 0) B.block(dof_[size_t(i)], dof_[size_t(i)]);
  f64 Dm[36], Ba[36], Bb[36];
  for (SBond& b : bonds) {
    b.in_k = false;
    // (bonds of retired nodes stay out of K; their topology is the caller's: a retired chip
    // keeps its inner bonds intact)
    if (b.broken || nodes[size_t(b.a)].gone || (b.b >= 0 && nodes[size_t(b.b)].gone)) continue;
    b.in_k = true;
    bond_matrices(b, Dm, Ba, Bb);
    const i32 ia = dof_[size_t(b.a)];
    const i32 ib = b.b >= 0 ? dof_[size_t(b.b)] : -1;
    if (ia >= 0) blk6::atbd_add(Ba, Dm, Ba, 1.0, B.block(ia, ia));
    if (ib >= 0) blk6::atbd_add(Bb, Dm, Bb, 1.0, B.block(ib, ib));
    if (ia >= 0 && ib >= 0) {
      blk6::atbd_add(Ba, Dm, Bb, -1.0, B.block(ia, ib));
      blk6::atbd_add(Bb, Dm, Ba, -1.0, B.block(ib, ia));
    }
  }
  K_ = B.finish();
  // retired rows (and free nodes left without any bond) become scaled identities
  f64 tr = 0.0;
  i32 cnt = 0;
  for (i32 i = 0; i < nfree_; ++i) {
    const f64* d = &K_.val[36 * size_t(K_.rowptr[size_t(i)])];
    const f64 t = d[0] + d[7] + d[14];
    if (t > 0) {
      tr += t / 3.0;
      ++cnt;
    }
  }
  id_scale_ = cnt ? tr / cnt : 1.0;
  for (i32 i = 0; i < n; ++i) {
    const i32 d = dof_[size_t(i)];
    if (d < 0) continue;
    f64* D = &K_.val[36 * size_t(K_.rowptr[size_t(d)])];
    const bool empty = D[0] == 0.0 && D[7] == 0.0 && D[14] == 0.0;
    if (nodes[size_t(i)].gone || empty)
      for (int q = 0; q < 6; ++q) D[q * 7] = id_scale_;
  }
  return true;
}

bool StressProblem::assemble(const StressOptions& opt) {
  opt_ = opt;
  assembled_ = false;
  run_.active = false;
  if (!build_matrix()) return false;
  std::vector<V3> pos(static_cast<size_t>(nfree_));
  for (size_t i = 0; i < nodes.size(); ++i)
    if (dof_[i] >= 0) pos[size_t(dof_[i])] = nodes[i].c;
  const AmgOptions ao = opt_.amg;
  jacobi_only_ = nfree_ < opt_.amg_min_nodes;
  if (jacobi_only_) {
    amg_ = Amg{};
    pc_n_ = 0;
    jinv_.assign(36 * size_t(nfree_), 0.0);
    for (i32 i = 0; i < nfree_; ++i) {
      const f64* D = &K_.val[36 * size_t(K_.rowptr[size_t(i)])];
      if (!blk6::inv6(D, &jinv_[36 * size_t(i)]))
        for (int q = 0; q < 6; ++q) jinv_[36 * size_t(i) + size_t(q * 7)] = 1.0 / id_scale_;
    }
    assembled_ = true;
    return true;
  }
  if (!amg_.build(K_, pos, ao)) return false;
  if (diag("SVX_AMG_INFO")) {
    std::printf("  amg levels:");
    for (i32 k : amg_.level_sizes()) std::printf(" %d", k);
    std::printf(" | blocks:");
    for (i64 k : amg_.level_blocks()) std::printf(" %lld", static_cast<long long>(k));
    std::printf(" | work/apply %lld\n", static_cast<long long>(amg_.work_per_apply()));
  }
  pc_n_ = nfree_;
  jinv_.clear();
  amg2_ = Amg{};
  assembled_ = true;
  return true;
}

bool StressProblem::reassemble() {
  if (jacobi_only_ || !amg_.built() || pc_n_ == 0) return assemble(opt_);
  run_.active = false;
  if (!build_matrix()) return false;
  if (nfree_ < pc_n_) return assemble(opt_);  // (nodes were removed: indices moved)
  jinv_.assign(36 * size_t(nfree_ - pc_n_), 0.0);
  for (i32 i = pc_n_; i < nfree_; ++i) {
    const f64* D = &K_.val[36 * size_t(K_.rowptr[size_t(i)])];
    if (!blk6::inv6(D, &jinv_[36 * size_t(i - pc_n_)]))
      for (int q = 0; q < 6; ++q) jinv_[36 * size_t(i - pc_n_) + size_t(q * 7)] = 1.0 / id_scale_;
  }
  // the appended block of K gets its own multigrid (additive with the old one)
  amg2_ = Amg{};
  const i32 na = nfree_ - pc_n_;
  if (na >= 24) {
    Bsr6Builder B(na);
    std::vector<V3> pos(static_cast<size_t>(na));
    for (size_t i = 0; i < nodes.size(); ++i)
      if (dof_[i] >= pc_n_) pos[size_t(dof_[i] - pc_n_)] = nodes[i].c;
    for (i32 r = pc_n_; r < nfree_; ++r)
      for (i32 k = K_.rowptr[size_t(r)]; k < K_.rowptr[size_t(r) + 1]; ++k) {
        const i32 c = K_.col[size_t(k)];
        if (c < pc_n_) continue;
        f64* dst = B.block(r - pc_n_, c - pc_n_);
        for (int q = 0; q < 36; ++q) dst[q] += K_.val[36 * size_t(k) + size_t(q)];
      }
    const Bsr6 Ksub = B.finish();
    AmgOptions ao = opt_.amg;
    if (!amg2_.build(Ksub, pos, ao)) amg2_ = Amg{};
  }
  assembled_ = true;
  return true;
}

void StressProblem::extend_warm_start(std::vector<f64>& u, i32 first_new) const {
  const i32 n = static_cast<i32>(nodes.size());
  if (first_new >= n || u.size() != 6 * size_t(n)) return;
  std::vector<f64> acc(6 * size_t(n), 0.0);
  std::vector<f64> w(size_t(n), 0.0);
  std::vector<u8> known(size_t(n), 0);
  for (i32 i = 0; i < first_new; ++i) known[size_t(i)] = nodes[size_t(i)].gone ? 0 : 1;
  for (int pass = 0; pass < 4; ++pass) {
    std::fill(acc.begin(), acc.end(), 0.0);
    std::fill(w.begin(), w.end(), 0.0);
    for (const SBond& b : bonds) {
      if (b.broken || b.b < 0) continue;
      const i32 a = b.a, c = b.b;
      if (known[size_t(a)] && !known[size_t(c)] && c >= first_new) {
        // rigid transfer of a's motion to c's centre: u_c = u_a + th_a x (x_c - x_a)
        const V3 r = nodes[size_t(c)].c - nodes[size_t(a)].c;
        const V3 th{u[6 * size_t(a) + 3], u[6 * size_t(a) + 4], u[6 * size_t(a) + 5]};
        const V3 t = cross(th, r);
        for (int q = 0; q < 3; ++q) acc[6 * size_t(c) + size_t(q)] += u[6 * size_t(a) + size_t(q)] + t[q];
        for (int q = 3; q < 6; ++q) acc[6 * size_t(c) + size_t(q)] += u[6 * size_t(a) + size_t(q)];
        w[size_t(c)] += 1.0;
      } else if (known[size_t(c)] && !known[size_t(a)] && a >= first_new) {
        const V3 r = nodes[size_t(a)].c - nodes[size_t(c)].c;
        const V3 th{u[6 * size_t(c) + 3], u[6 * size_t(c) + 4], u[6 * size_t(c) + 5]};
        const V3 t = cross(th, r);
        for (int q = 0; q < 3; ++q) acc[6 * size_t(a) + size_t(q)] += u[6 * size_t(c) + size_t(q)] + t[q];
        for (int q = 3; q < 6; ++q) acc[6 * size_t(a) + size_t(q)] += u[6 * size_t(c) + size_t(q)];
        w[size_t(a)] += 1.0;
      }
    }
    bool any = false;
    for (i32 i = first_new; i < n; ++i) {
      if (known[size_t(i)] || w[size_t(i)] == 0.0) continue;
      for (int q = 0; q < 6; ++q) u[6 * size_t(i) + size_t(q)] = acc[6 * size_t(i) + size_t(q)] / w[size_t(i)];
      known[size_t(i)] = 1;
      any = true;
    }
    if (!any) break;
  }
}

f64* StressProblem::block(i32 r, i32 c) {
  for (i32 k = K_.rowptr[size_t(r)]; k < K_.rowptr[size_t(r) + 1]; ++k)
    if (K_.col[size_t(k)] == c) return &K_.val[36 * size_t(k)];
  return nullptr;
}

void StressProblem::remove_bond(i32 bi) {
  SBond& b = bonds[size_t(bi)];
  b.broken = true;
  if (!assembled_ || !b.in_k) return;  // (never subtracted twice)
  b.in_k = false;
  f64 Dm[36], Ba[36], Bb[36];
  bond_matrices(b, Dm, Ba, Bb);
  const i32 ia = dof_[size_t(b.a)];
  const i32 ib = b.b >= 0 ? dof_[size_t(b.b)] : -1;
  if (ia >= 0) blk6::atbd_add(Ba, Dm, Ba, -1.0, block(ia, ia));
  if (ib >= 0) blk6::atbd_add(Bb, Dm, Bb, -1.0, block(ib, ib));
  if (ia >= 0 && ib >= 0) {
    if (f64* k = block(ia, ib)) blk6::atbd_add(Ba, Dm, Bb, 1.0, k);
    if (f64* k = block(ib, ia)) blk6::atbd_add(Bb, Dm, Ba, 1.0, k);
  }
  if (jacobi_only_)
    for (i32 d : {ia, ib}) {
      if (d < 0) continue;
      const f64* D = &K_.val[36 * size_t(K_.rowptr[size_t(d)])];
      if (!blk6::inv6(D, &jinv_[36 * size_t(d)]))
        for (int q = 0; q < 36; ++q) jinv_[36 * size_t(d) + size_t(q)] = (q % 7 == 0) ? 1.0 / id_scale_ : 0.0;
    }
  run_.active = false;
}

void StressProblem::retire_nodes(const std::vector<i32>& list) {
  if (list.empty()) return;
  // the bonds of the nodes retired now leave K and break (bonds between nodes retired before
  // are left as they are: a caller may have restored their topology)
  std::vector<u8> now(nodes.size(), 0);
  for (i32 i : list) {
    now[size_t(i)] = nodes[size_t(i)].gone ? 0 : 1;
    nodes[size_t(i)].gone = true;
  }
  for (i32 b = 0; b < static_cast<i32>(bonds.size()); ++b) {
    const SBond& B = bonds[size_t(b)];
    if (now[size_t(B.a)] || (B.b >= 0 && now[size_t(B.b)])) remove_bond(b);
  }
  if (assembled_)
    for (i32 i : list) {
      const i32 d = dof_[size_t(i)];
      if (d < 0) continue;
      f64* D = block(d, d);
      for (int q = 0; q < 36; ++q) D[q] = (q % 7 == 0) ? id_scale_ : 0.0;
      if (jacobi_only_)
        for (int q = 0; q < 36; ++q) jinv_[36 * size_t(d) + size_t(q)] = (q % 7 == 0) ? 1.0 / id_scale_ : 0.0;
    }
  run_.active = false;
}

void StressProblem::precondition(const f64* r, f64* z) const {
  if (pc_n_ == nfree_ && !jacobi_only_) {
    amg_.apply(r, z);
    return;
  }
  if (pc_n_ > 0) amg_.apply(r, z);  // (the first pc_n_ nodes)
  if (amg2_.built() && !jacobi_only_) {
    amg2_.apply(r + 6 * size_t(pc_n_), z + 6 * size_t(pc_n_));
    return;
  }
  for (i32 i = pc_n_; i < nfree_; ++i)
    blk6::mv6(&jinv_[36 * size_t(i - pc_n_)], r + 6 * size_t(i), z + 6 * size_t(i));
}

PcgResult StressProblem::solve(const std::vector<f64>& f, std::vector<f64>& u, f64 rtol, int maxit, bool warm) {
  if (!assembled_ && !assemble(opt_)) {
    PcgResult failed;  // (no answer: never mistaken for an accurate one)
    failed.rel_res = INFINITY;
    failed.breakdown = true;
    return failed;
  }
  begin(f, warm ? u : std::vector<f64>{});
  const PcgResult r = iterate(maxit < 0 ? 100000 : maxit, rtol);
  current(u);
  run_.active = false;
  return r;
}

void StressProblem::begin(const std::vector<f64>& f, const std::vector<f64>& u0) {
  run_.active = false;
  if (!assembled_ && !assemble(opt_)) return;
  const size_t n = nodes.size();
  const size_t m = 6 * size_t(nfree_);
  run_.x.assign(m, 0.0);
  std::vector<f64> fb(m, 0.0);
  const bool warm = u0.size() == 6 * n;
  for (size_t i = 0; i < n; ++i) {
    const i32 d = dof_[i];
    if (d < 0) continue;
    for (int q = 0; q < 6; ++q) {
      fb[6 * size_t(d) + q] = f[6 * i + q];
      if (warm) run_.x[6 * size_t(d) + q] = u0[6 * i + q];
    }
  }
  run_.r.assign(m, 0.0);
  run_.z.assign(m, 0.0);
  run_.p.assign(m, 0.0);
  run_.q.assign(m, 0.0);
  K_.apply(run_.x.data(), run_.q.data());
  f64 bn = 0.0;
  for (size_t k = 0; k < m; ++k) {
    run_.r[k] = fb[k] - run_.q[k];
    bn += fb[k] * fb[k];
  }
  run_.bn = std::sqrt(bn);
  if (!(bn > 0.0)) {
    // no load (the answer is u = 0, whatever the warm start), or a load that is not finite
    // (reported as a breakdown by iterate())
    std::fill(run_.x.begin(), run_.x.end(), 0.0);
    std::fill(run_.r.begin(), run_.r.end(), 0.0);
  }
  precondition(run_.r.data(), run_.z.data());
  run_.p = run_.z;
  run_.rz = 0.0;
  for (size_t k = 0; k < m; ++k) run_.rz += run_.r[k] * run_.z[k];
  run_.iters = 0;
  run_.active = true;
}

PcgResult StressProblem::iterate(int maxit, f64 rtol) {
  PcgResult res;
  if (!run_.active) return res;
  const i64 m = static_cast<i64>(run_.x.size());
  constexpr i64 G = 8 * kRowGrain;
  auto dot = [&](const std::vector<f64>& a, const std::vector<f64>& b) {
    return parallel_sum(m, G, [&](i64 k0, i64 k1) {
      f64 s = 0.0;
      for (i64 k = k0; k < k1; ++k) s += a[size_t(k)] * b[size_t(k)];
      return s;
    });
  };
  f64 rn = std::sqrt(dot(run_.r, run_.r));
  const f64 target = rtol * run_.bn;
  if (!std::isfinite(run_.bn) || !std::isfinite(rn)) {
    res.rel_res = INFINITY;  // (non-finite loads: no answer)
    res.breakdown = true;
    run_.active = false;
    return res;
  }
  res.rel_res = run_.bn > 0 ? rn / run_.bn : 0.0;
  if (rn <= target || !(run_.bn > 0) || m == 0) {
    res.converged = true;
    return res;
  }
  for (int it = 0; it < maxit; ++it) {
    K_.apply(run_.p.data(), run_.q.data());
    const f64 pq = dot(run_.p, run_.q);
    ++res.iters;
    ++run_.iters;
    if (!(pq > 0)) {
      res.converged = false;  // (breakdown: not positive definite along p: no answer)
      res.breakdown = true;
      break;
    }
    const f64 alpha = run_.rz / pq;
    parallel_for(m, G, [&](i64 k0, i64 k1) {
      for (i64 k = k0; k < k1; ++k) {
        run_.x[size_t(k)] += alpha * run_.p[size_t(k)];
        run_.r[size_t(k)] -= alpha * run_.q[size_t(k)];
      }
    });
    rn = std::sqrt(dot(run_.r, run_.r));
    res.rel_res = rn / run_.bn;
    if (rn <= target) {
      res.converged = true;
      break;
    }
    precondition(run_.r.data(), run_.z.data());
    const f64 rz1 = dot(run_.r, run_.z);
    if (!(rz1 > 0)) {
      res.breakdown = true;  // (the preconditioner is not positive definite along r)
      break;
    }
    const f64 beta = rz1 / run_.rz;
    run_.rz = rz1;
    parallel_for(m, G, [&](i64 k0, i64 k1) {
      for (i64 k = k0; k < k1; ++k) run_.p[size_t(k)] = run_.z[size_t(k)] + beta * run_.p[size_t(k)];
    });
  }
  return res;
}

void StressProblem::current(std::vector<f64>& u) const {
  const size_t n = nodes.size();
  u.assign(6 * n, 0.0);
  if (run_.x.size() != 6 * size_t(nfree_)) return;
  for (size_t i = 0; i < n; ++i) {
    const i32 d = dof_[i];
    if (d < 0) continue;
    for (int q = 0; q < 6; ++q) u[6 * i + q] = run_.x[6 * size_t(d) + q];
  }
}

BondLoad StressProblem::bond_load(i32 bi, const std::vector<f64>& u) const {
  const SBond& b = bonds[size_t(bi)];
  const Material& A = material(b.ma);
  const Material& B = material(b.mb);
  f64 Ba[36], Bb[36];
  blk6::rigid_block(b.p - nodes[size_t(b.a)].c, Ba);
  f64 xa[6] = {0, 0, 0, 0, 0, 0}, xb[6] = {0, 0, 0, 0, 0, 0};
  if (!nodes[size_t(b.a)].fixed) blk6::mv6(Ba, &u[6 * size_t(b.a)], xa);
  if (b.b >= 0) {
    blk6::rigid_block(b.p - nodes[size_t(b.b)].c, Bb);
    if (!nodes[size_t(b.b)].fixed) blk6::mv6(Bb, &u[6 * size_t(b.b)], xb);
  }
  const V3 dt{xb[0] - xa[0], xb[1] - xa[1], xb[2] - xa[2]};
  const V3 dr{xb[3] - xa[3], xb[4] - xa[4], xb[5] - xa[5]};
  const f64 la = std::max(b.la, 1e-4), lb = std::max(b.lb, 1e-4);
  const f64 invE = la / A.E + lb / B.E;
  const f64 invG = la / A.G + lb / B.G;
  BondLoad L;
  L.N = b.area / invE * dot(dt, b.n);
  L.V1 = (5.0 / 6.0) * b.area / invG * dot(dt, b.t1);
  L.V2 = (5.0 / 6.0) * b.area / invG * dot(dt, b.t2);
  L.T = 0.8 * (b.s1 + b.s2) / invG * dot(dr, b.n);
  L.M1 = b.s2 / invE * dot(dr, b.t1);
  L.M2 = b.s1 / invE * dot(dr, b.t2);
  return L;
}

void StressProblem::add_force(std::vector<f64>& f, i32 node, const V3& c, const V3& F, const V3& at) {
  f64* fi = &f[6 * size_t(node)];
  fi[0] += F.x;
  fi[1] += F.y;
  fi[2] += F.z;
  const V3 M = cross(at - c, F);
  fi[3] += M.x;
  fi[4] += M.y;
  fi[5] += M.z;
}

}  // namespace svx
