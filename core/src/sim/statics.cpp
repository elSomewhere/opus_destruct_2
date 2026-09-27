#include "svx/sim/statics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "svx/base/parallel.hpp"
#include "svx/base/work.hpp"
#include "svx/sim/corot.hpp"

namespace svx {

namespace {

using Clock = std::chrono::steady_clock;
inline f64 ms_since(Clock::time_point t0) { return std::chrono::duration<f64, std::milli>(Clock::now() - t0).count(); }

constexpr i64 kGrain = 2048;
constexpr i64 kVecGrain = i64(1) << 16;  // cheap per-element vector ops: parallel only for large systems

f64 dot(const std::vector<f64>& a, const std::vector<f64>& b) {
  const i64 m = static_cast<i64>(a.size());
  return parallel_sum(m, kVecGrain, [&](i64 lo, i64 hi) {
    f64 s = 0.0;
    for (i64 k = lo; k < hi; ++k) s += a[k] * b[k];
    return s;
  });
}

// Anderson mixing coefficients: min || f - sum_i g_i dF_i || (normal equations, regularized).
std::vector<f64> anderson_gamma(const std::vector<std::vector<f64>>& dF, const std::vector<f64>& f) {
  const size_t m = dF.size();
  std::vector<f64> A(m * m), b(m), g(m, 0.0);
  f64 tr = 0.0;
  for (size_t i = 0; i < m; ++i) {
    for (size_t j = 0; j <= i; ++j) A[i * m + j] = A[j * m + i] = dot(dF[i], dF[j]);
    b[i] = dot(dF[i], f);
    tr += A[i * m + i];
  }
  for (size_t i = 0; i < m; ++i) A[i * m + i] += 1e-10 * tr + 1e-300;
  // Gaussian elimination with partial pivoting (m <= 8)
  std::vector<size_t> piv(m);
  for (size_t i = 0; i < m; ++i) piv[i] = i;
  for (size_t c = 0; c < m; ++c) {
    size_t p = c;
    for (size_t r = c + 1; r < m; ++r)
      if (std::abs(A[r * m + c]) > std::abs(A[p * m + c])) p = r;
    if (p != c) {
      for (size_t q = 0; q < m; ++q) std::swap(A[c * m + q], A[p * m + q]);
      std::swap(b[c], b[p]);
    }
    const f64 d = A[c * m + c];
    if (d == 0.0) return std::vector<f64>(m, 0.0);
    for (size_t r = c + 1; r < m; ++r) {
      const f64 l = A[r * m + c] / d;
      for (size_t q = c; q < m; ++q) A[r * m + q] -= l * A[c * m + q];
      b[r] -= l * b[c];
    }
  }
  for (size_t c = m; c-- > 0;) {
    f64 s = b[c];
    for (size_t q = c + 1; q < m; ++q) s -= A[c * m + q] * g[q];
    g[c] = s / A[c * m + c];
  }
  return g;
}

f64 norm2(const std::vector<f64>& v) {
  const i64 m = static_cast<i64>(v.size());
  return std::sqrt(parallel_sum(m, kVecGrain, [&](i64 b, i64 e) {
    f64 s = 0.0;
    for (i64 k = b; k < e; ++k) s += v[k] * v[k];
    return s;
  }));
}

void mask_fixed(const Lattice& L, std::vector<f64>& v) {
  for (i32 i = 0; i < L.n; ++i) {
    const u8 m = L.fixmask[i];
    if (!m) continue;
    for (int q = 0; q < 6; ++q)
      if ((m >> q) & 1) v[6 * size_t(i) + q] = 0.0;
  }
}

// Trust region for corotational modified Newton (the oracle's maxTranslationIncrement /
// maxRotationIncrement, in cells and radians).
f64 increment_scale(const Lattice& L, const std::vector<f64>& du, f64 max_t, f64 max_r) {
  f64 mt = 0.0, mr = 0.0;
  for (i32 i = 0; i < L.n; ++i) {
    if (L.anchored[i]) continue;
    const f64* d = &du[6 * size_t(i)];
    mt = std::max(mt, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    mr = std::max(mr, std::sqrt(d[3] * d[3] + d[4] * d[4] + d[5] * d[5]));
  }
  f64 s = 1.0;
  if (mt > max_t) s = std::min(s, max_t / mt);
  if (mr > max_r) s = std::min(s, max_r / mr);
  return s;
}

}  // namespace

void sort_candidates(std::vector<RuptureCandidate>& c) {
  std::sort(c.begin(), c.end(), [](const RuptureCandidate& a, const RuptureCandidate& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.damage != b.damage) return a.damage > b.damage;
    return a.key < b.key;
  });
}

Vec6 contact_scales(const Lattice& L, int a, i32 i, const Vec6& jump, f64 strain_scale) {
  const f64 lo = kMinSecant;
  if (!(jump[0] < 0.0)) return {lo, lo, lo, lo, lo, lo};  // open (N > 0 is opening)
  // closed: elastic contact forces of the (scaled) jump; normal stiffness kept, shear /
  // torsion capped by Coulomb friction, bending by rocking about the face edge
  const BondModel& m = L.bond(a, i);
  const f64 N = std::abs(m.k[0] * jump[0]) * strain_scale;
  // sqrt of a sum, not std::hypot: hypot is not correctly rounded, so platform libms differ
  auto norm2 = [](f64 x, f64 y) { return std::sqrt(x * x + y * y); };
  const f64 V = norm2(m.k[1] * jump[1], m.k[2] * jump[2]) * strain_scale;
  const f64 T = std::abs(m.k[3] * jump[3]) * strain_scale;
  const f64 M = norm2(m.k[4] * jump[4], m.k[5] * jump[5]) * strain_scale;
  const f64 side = m.area_min > 0.0 ? std::sqrt(m.area_min) : L.h;
  const f64 mu = L.contact.friction;
  auto cap = [lo](f64 demand, f64 limit) { return demand > limit ? std::max(lo, limit / demand) : 1.0; };
  const f64 sv = cap(V, mu * N), st = cap(T, mu * N * side / 3.0), sm = cap(M, 0.5 * N * side);
  return {1.0, sv, sv, st, sm, sm};
}

LawSweep sweep_law(Lattice& L, const f64* u, const DamageField& dc, bool corot, f64 strain_scale, f64 demand_scale) {
  add_work(i64(L.n) * kWorkLawSweep);  // (deterministic work accounting)
  const i64 nchunks = (i64(L.n) + kGrain - 1) / kGrain;
  struct Part {
    f64 max_damage = 0.0, max_phi = 0.0, max_change = 0.0;
    i64 damaged = 0;
    std::vector<RuptureCandidate> cand;
  };
  std::vector<Part> parts(static_cast<size_t>(nchunks));
  const bool game = L.law.game;
  parallel_for(L.n, kGrain, [&](i64 b, i64 e) {
    Part& P = parts[static_cast<size_t>(b / kGrain)];
    for (i64 cc = b; cc < e; ++cc) {
      const i32 i = static_cast<i32>(cc);
      for (int a = 0; a < 3; ++a) {
        const i32 j = L.nbr[a][i];
        if (j < 0) continue;
        Vec6 d = bond_jump(L, a, i, u, corot);
        // a non-finite state (a numerical breakdown upstream) decides nothing: no damage, no
        // rupture (NaN comparisons would otherwise rupture at random, and NaN bits differ
        // between architectures)
        if (!(std::isfinite(d[0]) && std::isfinite(d[1]) && std::isfinite(d[2]) && std::isfinite(d[3]) &&
              std::isfinite(d[4]) && std::isfinite(d[5])))
          continue;
        if (L.is_cracked(a, i)) {
          // unilateral contact: per-component secants of the current state (not monotone), a
          // separation candidate once the crack has opened for good
          const Vec6 sc = contact_scales(L, a, i, d, strain_scale);
          for (int q = 0; q < 6; ++q)
            P.max_change = std::max(P.max_change, std::abs(sc[q] - L.cscale[a][i][q]));
          L.cscale[a][i] = sc;
          // separated for good: the gap opened, the faces slid half a voxel apart, or the
          // piece rocked its face edge open (a sliding or tipping piece never opens the gap
          // at the face centre, and would otherwise drag on its neighbours indefinitely)
          const f64 slip = std::sqrt(d[1] * d[1] + d[2] * d[2]);
          const BondModel& cm = L.bond(a, i);
          const f64 side = cm.area_min > 0.0 ? std::sqrt(cm.area_min) : L.h;
          const f64 edge = std::sqrt(d[4] * d[4] + d[5] * d[5]) * 0.5 * side;
          const f64 sep = L.contact.separation * L.h;
          if (d[0] > sep || slip > 0.5 * L.h || edge > sep) {
            // a failure like a rupture for every consumer (triage, settle, verification): a
            // state that only stands with separated cracks is not an equilibrium
            P.max_damage = 1.0;
            RuptureCandidate rc;
            rc.key = bond_key(L, a, i);
            rc.cell = i;
            rc.axis = static_cast<u8>(a);
            rc.damage = 1.0;
            rc.score = 1.0 + std::max({d[0], slip, edge}) / L.h;
            rc.separate = true;
            P.cand.push_back(rc);
          }
          continue;
        }
        for (auto& v : d) v *= strain_scale;
        if (demand_scale != 1.0)
          for (auto& v : d) v *= demand_scale;
        const f64 d0 = dc[a].empty() ? 0.0 : f64(dc[a][i]);
        const LawEval ev = evaluate_law(L.bond(a, i), d, d0, game);
        const f32 dn = static_cast<f32>(ev.damage);
        P.max_change = std::max(P.max_change, std::abs(f64(dn) - f64(L.dmg[a][i])));
        L.dmg[a][i] = dn;
        P.max_damage = std::max(P.max_damage, ev.damage);
        P.max_phi = std::max(P.max_phi, ev.phi);
        if (ev.damage > 0.0) ++P.damaged;
        if (ev.candidate) {
          RuptureCandidate rc;
          rc.key = bond_key(L, a, i);
          rc.cell = i;
          rc.axis = static_cast<u8>(a);
          rc.damage = ev.damage;
          rc.score = std::max({ev.damage, ev.margin, ev.phi});
          rc.opening = d[0] > 0.0;
          P.cand.push_back(rc);
        }
      }
    }
  });
  LawSweep s;
  for (Part& P : parts) {
    s.max_damage = std::max(s.max_damage, P.max_damage);
    s.max_phi = std::max(s.max_phi, P.max_phi);
    s.max_change = std::max(s.max_change, P.max_change);
    s.damaged += P.damaged;
    s.candidates.insert(s.candidates.end(), P.cand.begin(), P.cand.end());
  }
  return s;
}

std::vector<f64> gravity_vector(const Lattice& L, f64 g) {
  std::vector<f64> f(6 * size_t(L.n), 0.0);
  gravity_load(L, g, f.data());
  return f;
}

namespace {

EquilibriumStats equilibrium_impl(Lattice& L, std::vector<f64>& u, const std::vector<f64>& f_ext, const DamageField& dc,
                                  const StaticsOptions& opt, Multigrid* mg_in);

}  // namespace

EquilibriumStats solve_equilibrium(Lattice& L, std::vector<f64>& u, const std::vector<f64>& f_ext,
                                   const DamageField& dc, const StaticsOptions& opt_in, Multigrid* mg_in) {
  static const char* anderson_env = std::getenv("SVX_ANDERSON");  // (experiments)
  StaticsOptions opt = opt_in;
  if (anderson_env) opt.anderson = std::atoi(anderson_env);
  const EquilibriumStats st = equilibrium_impl(L, u, f_ext, dc, opt, mg_in);
  if (L.framed()) clear_frames(L);
  return st;
}

namespace {

EquilibriumStats equilibrium_impl(Lattice& L, std::vector<f64>& u, const std::vector<f64>& f_ext, const DamageField& dc,
                                  const StaticsOptions& opt, Multigrid* mg_in) {
  EquilibriumStats st;
  const size_t m = 6 * size_t(L.n);
  u.resize(m, 0.0);
  L.enable_damage();
  std::vector<f64> f = f_ext;
  f.resize(m, 0.0);
  mask_fixed(L, f);
  const f64 fn = norm2(f);
  if (fn == 0.0) {
    std::fill(u.begin(), u.end(), 0.0);
    for (int a = 0; a < 3; ++a) L.dmg[a] = dc[a].empty() ? std::vector<f32>(L.n, 0.0f) : dc[a];
    st.converged = true;
    return st;
  }
  Multigrid local;
  Multigrid& mg = mg_in ? *mg_in : local;
  bool built = false;
  bool built_framed = false;
  std::vector<f64> fint(m), r(m), du(m), u_keep, th_build;
  const f64 max_t = opt.max_translation_increment * L.h, max_r = opt.max_rotation_increment;
  auto rotation_drift = [&]() {
    f64 d = 0.0;
    for (i32 i = 0; i < L.n; ++i)
      for (int q = 3; q < 6; ++q) d = std::max(d, std::abs(u[6 * size_t(i) + q] - th_build[6 * size_t(i) + q]));
    return d;
  };
  f64 prev_rel = INFINITY, best_rel = INFINITY;
  int with_candidates = 0;
  // Anderson: the last steps f_k (= s du_k) and iterates u_k
  std::vector<std::vector<f64>> hist_f, hist_u;
  f64 hist_rel = INFINITY;
  for (int it = 0; it <= opt.max_iters; ++it) {
    f64 change = 0.0;
    auto t0 = Clock::now();
    if (opt.damage) {
      const LawSweep sw = sweep_law(L, u.data(), dc, opt.corot, L.kscale);
      change = sw.max_change;
      st.max_damage = sw.max_damage;
      st.max_phi = sw.max_phi;
      with_candidates = sw.candidates.empty() ? 0 : with_candidates + 1;
    } else if (it == 0) {
      for (int a = 0; a < 3; ++a) L.dmg[a] = dc[a].empty() ? std::vector<f32>(L.n, 0.0f) : dc[a];
    }
    st.ms_law += ms_since(t0);
    t0 = Clock::now();
    internal_forces(L, u.data(), opt.corot, fint.data());
    parallel_for(static_cast<i64>(m), kVecGrain, [&](i64 b, i64 e) {
      for (i64 k = b; k < e; ++k) r[k] = f[k] - fint[k];
    });
    st.ms_forces += ms_since(t0);
    const f64 rel = norm2(r) / fn;
    if (!std::isfinite(rel)) {  // numerical breakdown: keep the last finite iterate
      if (!u_keep.empty()) u = u_keep;
      st.residual = prev_rel;
      return st;
    }
    st.residual = rel;
    st.iters = it;
    const bool stalled = rel <= opt.res_accept && rel > 0.5 * best_rel && it >= 2;
    best_rel = std::min(best_rel, rel);
    if ((rel <= opt.res_tol || stalled) && change <= opt.damage_tol) {
      st.converged = true;
      break;
    }
    if (it == opt.max_iters || with_candidates > opt.candidate_grace) break;
    if (opt.corot) {
      const bool framed = update_frames(L, u.data(), opt.frame_threshold);
      if (built && (framed != built_framed || (framed && rotation_drift() > opt.frame_rebuild))) built = false;
    }
    if (!built) {
      t0 = Clock::now();
      mg.build(L, opt.mg);
      built = true;
      built_framed = L.framed();
      th_build = u;
      ++st.mg_builds;
      st.ms_build += ms_since(t0);
    }
    t0 = Clock::now();
    // (the caller's budget bounds this solve too: a single solve may not overrun it)
    const int maxit = opt.max_pcg_total > 0 ? std::max(1, std::min(opt.lin_maxit, opt.max_pcg_total - st.pcg_iters))
                                            : opt.lin_maxit;
    const PcgStats ps = pcg_solve(mg, L.n, r.data(), du.data(), opt.lin_rtol, maxit, false);
    st.ms_pcg += ms_since(t0);
    st.pcg_iters += ps.iters;
    if (opt.verbose)
      std::printf("  eq it %3d rel %.3e dchange %.2e pcg %3d (res %.1e) framed %d maxrot %.4f\n", it, rel, change,
                  ps.iters, ps.rel_res, L.framed() ? 1 : 0, max_rotation(L, u.data()));
    if (ps.iters >= opt.mg_rebuild_iters) built = false;  // stale preconditioner
    u_keep = u;
    if (opt.max_pcg_total > 0 && st.pcg_iters >= opt.max_pcg_total) {  // (the caller's budget)
      const f64 s = opt.corot ? increment_scale(L, du, max_t, max_r) : 1.0;
      apply_increment(L, u.data(), du.data(), s, opt.corot);
      break;
    }
    prev_rel = rel;
    const f64 s = opt.corot ? increment_scale(L, du, max_t, max_r) : 1.0;
    if (opt.anderson > 0) {
      std::vector<f64> fk(du.size());
      for (size_t k = 0; k < du.size(); ++k) fk[k] = s * du[k];
      if (rel > hist_rel && !hist_f.empty()) {  // (the residual grew: restart the history)
        hist_f.clear();
        hist_u.clear();
        ++st.anderson_restarts;
      }
      hist_rel = rel;
      std::vector<f64> step = fk;
      if (!hist_f.empty()) {
        const size_t m = hist_f.size();
        std::vector<std::vector<f64>> dF(m), dX(m);
        for (size_t i = 0; i < m; ++i) {  // differences to the current step / iterate
          dF[i].resize(fk.size());
          dX[i].resize(fk.size());
          for (size_t k = 0; k < fk.size(); ++k) {
            dF[i][k] = fk[k] - hist_f[i][k];
            dX[i][k] = u[k] - hist_u[i][k];
          }
        }
        const std::vector<f64> gam = anderson_gamma(dF, fk);
        for (size_t i = 0; i < m; ++i)
          for (size_t k = 0; k < fk.size(); ++k) step[k] -= gam[i] * (dX[i][k] + dF[i][k]);
      }
      hist_f.insert(hist_f.begin(), std::move(fk));
      hist_u.insert(hist_u.begin(), u);
      if (static_cast<int>(hist_f.size()) > opt.anderson) {
        hist_f.pop_back();
        hist_u.pop_back();
      }
      const f64 sa = opt.corot ? increment_scale(L, step, max_t, max_r) : 1.0;
      apply_increment(L, u.data(), step.data(), sa, opt.corot);
      continue;
    }
    apply_increment(L, u.data(), du.data(), s, opt.corot);
  }
  return st;
}

}  // namespace

void delete_islands(Lattice& L, const std::vector<std::vector<i32>>& islands, std::vector<f64>& u,
                    std::vector<i32>* deleted) {
  for (const auto& isl : islands)
    for (i32 c : isl) {
      if (L.dead[c]) continue;
      L.remove_cell(c);
      if (!u.empty())
        for (int q = 0; q < 6; ++q) u[6 * size_t(c) + q] = 0.0;
      if (deleted) deleted->push_back(c);
    }
}

ClosureResult solve_to_closure(Lattice& L, std::vector<f64>& u, const std::vector<f64>& f_ext,
                               const StaticsOptions& opt) {
  ClosureResult res;
  L.enable_damage();
  u.resize(6 * size_t(L.n), 0.0);
  DamageField dc = L.dmg;
  Multigrid mg;
  std::vector<f64> u_prev, u_eval;
  // Components without any support never enter a solve; they are deleted in pass 1.
  i32 pending_deleted = 0;
  {
    const size_t before = res.deleted.size();
    delete_islands(L, unsupported_components(L), u, &res.deleted);
    pending_deleted = static_cast<i32>(res.deleted.size() - before);
  }
  bool cascade = opt.event;
  for (int pass = 1; pass <= opt.max_passes; ++pass) {
    PassLog pl;
    pl.pass = pass;
    pl.deleted = pending_deleted;
    pending_deleted = 0;
    const bool amplify = cascade && opt.dif != 1.0 && opt.damage;
    if (amplify) u_prev = u;
    pl.eq = solve_equilibrium(L, u, f_ext, dc, opt, &mg);
    res.all_equilibria_converged = res.all_equilibria_converged && pl.eq.converged;
    std::vector<i32> seeds;
    if (opt.damage) {
      LawSweep sw = sweep_law(L, u.data(), dc, opt.corot, L.kscale);
      if (amplify) {
        u_eval.resize(u.size());
        for (size_t k = 0; k < u.size(); ++k) u_eval[k] = u_prev[k] + opt.dif * (u[k] - u_prev[k]);
        const DamageField ds = L.dmg;
        sw = sweep_law(L, u_eval.data(), ds, opt.corot, L.kscale);
      }
      dc = L.dmg;  // commit
      sort_candidates(sw.candidates);
      const size_t nb = std::min(sw.candidates.size(), static_cast<size_t>(std::max(0, opt.max_breaks)));
      for (size_t k = 0; k < nb; ++k) {
        const RuptureCandidate& c = sw.candidates[k];
        const i32 j = L.nbr[c.axis][c.cell];
        if (j < 0) continue;
        L.break_bond(c.cell, c.axis);
        res.ruptured.push_back(c.key);
        seeds.push_back(c.cell);
        seeds.push_back(j);
        ++pl.ruptured;
      }
    }
    if (!seeds.empty()) {
      const size_t before = res.deleted.size();
      CutSet cut;
      cut.seeds = std::move(seeds);
      delete_islands(L, detached_islands(L, cut, &res.conn), u, &res.deleted);
      pl.deleted += static_cast<i32>(res.deleted.size() - before);
    }
    pl.quiet = pl.eq.converged && pl.ruptured == 0 && pl.deleted == 0;
    res.log.push_back(pl);
    res.passes = pass;
    if (pl.quiet) {
      res.converged = true;
      break;
    }
    cascade = pl.ruptured > 0 || pl.deleted > 0;
  }
  return res;
}

}  // namespace svx
