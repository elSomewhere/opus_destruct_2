#include "svx/sim/dynamics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

#include "svx/base/parallel.hpp"
#include "svx/sim/corot.hpp"

namespace svx {

namespace {

constexpr i64 kVecGrain = i64(1) << 16;  // cheap per-element vector ops: parallel only for large systems
constexpr i32 kMaxRayCells = 1024;

using Clock = std::chrono::steady_clock;
inline f64 ms_since(Clock::time_point t0) {
  return std::chrono::duration<f64, std::milli>(Clock::now() - t0).count();
}

inline u64 pack(i32 x, i32 y, i32 z) {
  constexpr i64 off = i64(1) << 20;
  return (u64(i64(x) + off) << 42) | (u64(i64(y) + off) << 21) | u64(i64(z) + off);
}

f64 norm2(const std::vector<f64>& v) {
  const i64 m = static_cast<i64>(v.size());
  return std::sqrt(parallel_sum(m, kVecGrain, [&](i64 b, i64 e) {
    f64 s = 0.0;
    for (i64 k = b; k < e; ++k) s += v[k] * v[k];
    return s;
  }));
}

}  // namespace

void Dynamics::init(Lattice& L, const DynamicsOptions& opt) {
  L_ = &L;
  opt_ = opt;
  L.kscale = 1.0 / std::max(opt.compliance, 1e-9);
  L.enable_damage();
  const size_t m = 6 * size_t(L.n);
  u_.assign(m, 0.0);
  v_.assign(m, 0.0);
  v_prev_.assign(m, 0.0);
  d_prev_.assign(m, 0.0);
  f_once_.assign(m, 0.0);
  timed_.clear();
  index_.clear();
  index_.reserve(size_t(L.n) * 2);
  for (i32 i = 0; i < L.n; ++i) index_[pack(L.p[i][0], L.p[i][1], L.p[i][2])] = i;
  rebuild_mass();
  mg_dirty_ = true;
  be_next_ = true;
  asleep_ = false;
  quiet_steps_ = 0;
  time_ = 0.0;
  steps_ = 0;
  ruptured_.clear();
  detached_cells_ = 0;
  islands_.clear();
  conn_ = ConnStats{};
}

i32 Dynamics::cell_at(i32 x, i32 y, i32 z) const {
  const auto it = index_.find(pack(x, y, z));
  return it == index_.end() ? -1 : it->second;
}

void Dynamics::rebuild_mass() {
  const Lattice& L = *L_;
  mdiag_.assign(6 * size_t(L.n), 0.0);
  for (i32 i = 0; i < L.n; ++i) {
    if (L.anchored[i] || L.dead[i]) continue;
    f64* d = &mdiag_[6 * size_t(i)];
    d[0] = d[1] = d[2] = L.mass[i];
    d[3] = L.inertia[i][0];
    d[4] = L.inertia[i][1];
    d[5] = L.inertia[i][2];
    const u8 fm = L.fixmask[i];
    if (fm)
      for (int q = 0; q < 6; ++q)
        if ((fm >> q) & 1) d[q] = 0.0;
  }
}

void Dynamics::build_mg() {
  MGOptions mo = opt_.mg;
  const f64 c = opt_.bdf2 ? 1.5 / opt_.dt : 1.0 / opt_.dt;
  mo.mass_shift = (c * c + opt_.rayleigh_alpha * c) / (1.0 + opt_.rayleigh_beta * c);
  mg_.build(*L_, mo);
  mg_dirty_ = false;
  mg_framed_ = L_->framed();
}

void Dynamics::refresh_operator(const std::vector<f64>& x, StepStats* st) {
  Lattice& L = *L_;
  if (opt_.corot) {
    const bool framed = update_frames(L, x.data(), opt_.frame_threshold);
    if (framed != mg_framed_) mg_dirty_ = true;
    if (framed && !mg_dirty_ && th_build_.size() == x.size()) {
      f64 d = 0.0;
      for (size_t k = 0; k < x.size(); k += 6)
        for (int q = 3; q < 6; ++q) d = std::max(d, std::abs(x[k + q] - th_build_[k + q]));
      if (d > opt_.frame_rebuild) mg_dirty_ = true;
    }
  }
  if (mg_dirty_) {
    const auto t0 = Clock::now();
    build_mg();
    th_build_ = x;
    if (st) {
      st->rebuilt = true;
      st->ms_mg += ms_since(t0);
    }
  }
}

void Dynamics::apply_mass(const f64* x, f64* y, f64 s) const {
  const i64 m = static_cast<i64>(mdiag_.size());
  parallel_for(m, kVecGrain, [&](i64 b, i64 e) {
    for (i64 k = b; k < e; ++k) y[k] += s * mdiag_[k] * x[k];
  });
}

void Dynamics::set_state(const std::vector<f64>& u) {
  const size_t m = 6 * size_t(L_->n);
  u_ = u;
  u_.resize(m, 0.0);
  std::fill(v_.begin(), v_.end(), 0.0);
  std::fill(v_prev_.begin(), v_prev_.end(), 0.0);
  std::fill(d_prev_.begin(), d_prev_.end(), 0.0);
  be_next_ = true;
}

EquilibriumStats Dynamics::settle_to_equilibrium() {
  StaticsOptions so = opt_.statics;
  so.corot = opt_.corot;
  so.g = opt_.g;
  so.damage = false;
  const std::vector<f64> f = gravity_vector(*L_, opt_.g);
  const DamageField dc = L_->dmg;
  const EquilibriumStats st = solve_equilibrium(*L_, u_, f, dc, so, nullptr);
  set_state(u_);
  return st;
}

void Dynamics::wake() {
  asleep_ = false;
  quiet_steps_ = 0;
}

void Dynamics::add_force(i32 cell, const f64* f6) {
  for (int q = 0; q < 6; ++q) f_once_[6 * size_t(cell) + q] += f6[q];
  wake();
}

void Dynamics::add_velocity(i32 cell, const f64* dv6) {
  if (L_->anchored[cell]) return;
  for (int q = 0; q < 6; ++q)
    if (!((L_->fixmask[cell] >> q) & 1)) v_[6 * size_t(cell) + q] += dv6[q];
  be_next_ = true;
  wake();
}

void Dynamics::external_forces(f64 t, std::vector<f64>& f) {
  const Lattice& L = *L_;
  f.assign(6 * size_t(L.n), 0.0);
  gravity_load(L, opt_.g, f.data());
  for (size_t k = 0; k < f.size(); ++k) f[k] += f_once_[k];
  for (const TimedLoad& tl : timed_) {
    if (t < tl.t0 || t > tl.t1 || L.dead[tl.cell] || L.anchored[tl.cell]) continue;
    const f64 s = tl.ramp ? std::max(0.0, 1.0 - (t - tl.t0) / std::max(tl.t1 - tl.t0, 1e-12)) : 1.0;
    for (int q = 0; q < 6; ++q) f[6 * size_t(tl.cell) + q] += s * tl.f[q];
  }
  for (i32 i = 0; i < L.n; ++i) {
    const u8 fm = L.fixmask[i];
    if (!fm) continue;
    for (int q = 0; q < 6; ++q)
      if ((fm >> q) & 1) f[6 * size_t(i) + q] = 0.0;
  }
}

f64 Dynamics::kinetic_energy() const {
  f64 ke = 0.0;
  for (size_t k = 0; k < v_.size(); ++k) ke += 0.5 * mdiag_[k] * v_[k] * v_[k];
  return ke;
}

std::vector<DetachedIsland> Dynamics::take_islands() {
  std::vector<DetachedIsland> out;
  out.swap(islands_);
  return out;
}

void Dynamics::detach(const CutSet& cut, const std::vector<std::array<f64, 6>>* pre_forces,
                      const std::vector<std::pair<i32, i32>>* broken, StepStats* st) {
  Lattice& L = *L_;
  const auto islands = detached_islands(L, cut, &conn_);
  if (islands.empty()) return;
  std::vector<i32> island_of(0);
  island_of.assign(L.n, -1);
  for (size_t k = 0; k < islands.size(); ++k)
    for (i32 c : islands[k]) island_of[c] = static_cast<i32>(k);
  std::vector<f64> fall(islands.size(), -1.0);
  for (size_t k = 0; k < islands.size(); ++k) {
    const auto& isl = islands[k];
    DetachedIsland ev;
    ev.cells = isl;
    ev.step = steps_;
    for (i32 c : isl) {
      const f64 mc = L.mass[c];
      ev.mass += mc;
      const auto x = cell_position(L, u_.data(), c);
      for (int q = 0; q < 3; ++q) {
        ev.com[q] += mc * x[q];
        ev.v[q] += mc * v_[6 * size_t(c) + q];
        ev.w[q] += mc * v_[6 * size_t(c) + 3 + q];
      }
    }
    if (ev.mass > 0.0)
      for (int q = 0; q < 3; ++q) {
        ev.com[q] /= ev.mass;
        ev.v[q] /= ev.mass;
        ev.w[q] /= ev.mass;
      }
    if (opt_.virtual_impact && opt_.g > 0.0) {
      // lowest island cell and mass per (x, y) column
      std::map<std::pair<i32, i32>, std::pair<i32, f64>> cols;
      for (i32 c : isl) {
        const auto key = std::make_pair(L.p[c][0], L.p[c][1]);
        auto it = cols.find(key);
        if (it == cols.end())
          cols.emplace(key, std::make_pair(L.p[c][2], L.mass[c]));
        else {
          it->second.first = std::min(it->second.first, L.p[c][2]);
          it->second.second += L.mass[c];
        }
      }
      const f64 vdown = std::max(0.0, -ev.v[2]);
      for (const auto& [xy, zm] : cols) {
        for (i32 z = zm.first - 1; z >= zm.first - kMaxRayCells; --z) {
          const i32 hit = cell_at(xy.first, xy.second, z);
          if (hit < 0 || L.dead[hit] || island_of[hit] >= 0) continue;
          const f64 H = (zm.first - z - 1) * L.h;
          const f64 tf = (-vdown + std::sqrt(vdown * vdown + 2.0 * opt_.g * H)) / opt_.g;
          const f64 vimp = vdown + opt_.g * tf;
          if (!L.anchored[hit]) {
            TimedLoad tl;
            tl.cell = hit;
            tl.f = {0, 0, -zm.second * (vimp / opt_.impact_duration + opt_.g), 0, 0, 0};
            tl.t0 = time_ + tf;
            tl.t1 = time_ + tf + opt_.impact_duration;
            tl.ramp = false;
            timed_.push_back(tl);
          }
          fall[k] = fall[k] < 0.0 ? tf : std::min(fall[k], tf);
          break;
        }
      }
    }
    ev.fall_time = fall[k];
    islands_.push_back(std::move(ev));
  }
  // The load an island exerted through the bonds broken in this step ramps out over its fall.
  if (pre_forces && broken) {
    for (size_t b = 0; b < broken->size(); ++b) {
      const auto [i, j] = (*broken)[b];
      const i32 ii = island_of[i], ij = island_of[j];
      if ((ii >= 0) == (ij >= 0)) continue;
      const i32 surv = ii >= 0 ? j : i;
      const i32 isl = ii >= 0 ? ii : ij;
      const f64* fs = surv == i ? &(*pre_forces)[2 * b][0] : &(*pre_forces)[2 * b + 1][0];
      TimedLoad tl;
      tl.cell = surv;
      for (int q = 0; q < 6; ++q) tl.f[q] = -fs[q];
      tl.t0 = time_;
      tl.t1 = time_ + std::max(opt_.ramp_min, fall[isl]);
      tl.ramp = true;
      timed_.push_back(tl);
    }
  }
  i32 removed = 0;
  for (const auto& isl : islands)
    for (i32 c : isl) {
      if (L.dead[c]) continue;
      L.remove_cell(c);
      for (int q = 0; q < 6; ++q) {
        const size_t o = 6 * size_t(c) + q;
        u_[o] = v_[o] = v_prev_[o] = d_prev_[o] = 0.0;
      }
      ++removed;
    }
  detached_cells_ += removed;
  if (st) {
    st->islands += static_cast<i32>(islands.size());
    st->detached_cells += removed;
  }
  rebuild_mass();
  mg_dirty_ = true;
  be_next_ = true;
}

i32 Dynamics::rupture_candidates(std::vector<RuptureCandidate>& cand, StepStats* st) {
  Lattice& L = *L_;
  sort_candidates(cand);
  const size_t nb = std::min(cand.size(), static_cast<size_t>(std::max(0, opt_.max_breaks)));
  if (st) st->carried += static_cast<i32>(cand.size() - nb);
  std::vector<std::array<f64, 6>> pre;
  std::vector<std::pair<i32, i32>> broken;
  CutSet cut;
  pre.reserve(2 * nb);
  for (size_t k = 0; k < nb; ++k) {
    const RuptureCandidate& c = cand[k];
    const i32 j = L.nbr[c.axis][c.cell];
    if (j < 0) continue;
    std::array<f64, 6> fi{}, fj{};
    bond_internal_forces(L, c.axis, c.cell, u_.data(), opt_.corot, fi.data(), fj.data());
    pre.push_back(fi);
    pre.push_back(fj);
    broken.emplace_back(c.cell, j);
    L.break_bond(c.cell, c.axis);
    ruptured_.push_back(c.key);
    cut.seeds.push_back(c.cell);
    cut.seeds.push_back(j);
  }
  const i32 n = static_cast<i32>(broken.size());
  if (st) st->ruptured += n;
  if (n > 0) {
    detach(cut, &pre, &broken, st);
    mg_dirty_ = true;
    be_next_ = true;
  }
  return n;
}

StepStats Dynamics::step() {
  StepStats st;
  const auto t_step = Clock::now();
  st = step_impl();
  st.ms_total = ms_since(t_step);
  return st;
}

StepStats Dynamics::step_impl() {
  StepStats st;
  const f64 dt = opt_.dt;
  if (asleep_) {
    time_ += dt;
    ++steps_;
    st.asleep = true;
    return st;
  }
  Lattice& L = *L_;
  const size_t m = 6 * size_t(L.n);
  const i64 im = static_cast<i64>(m);
  refresh_operator(u_, &st);
  const bool be = be_next_ || !opt_.bdf2;
  st.be = be;
  const f64 c = be ? 1.0 / dt : 1.5 / dt;
  const f64 al = opt_.rayleigh_alpha, bt = opt_.rayleigh_beta;
  const f64 cm = c * c + al * c;
  const f64 ck = 1.0 + bt * c;
  const f64 ms = cm / ck;

  std::vector<f64> e(m, 0.0), vhat(m);
  if (be) {
    vhat = v_;
  } else {
    for (size_t k = 0; k < m; ++k) {
      e[k] = -d_prev_[k] / 3.0;
      vhat[k] = (4.0 * v_[k] - v_prev_[k]) / 3.0;
    }
  }
  std::vector<f64> R0;
  external_forces(time_ + dt, R0);
  const f64 fscale = norm2(R0);
  apply_mass(vhat.data(), R0.data(), c);
  std::vector<f64> tmp(m);
  if (!be) {
    apply_mass(e.data(), R0.data(), -cm);
    if (bt != 0.0) {
      apply_stiffness(L, e.data(), tmp.data());
      for (size_t k = 0; k < m; ++k) R0[k] -= bt * c * tmp[k];
    }
  }
  auto op = [&](const f64* x, f64* y) {
    apply_stiffness(L, x, y);
    apply_mass(x, y, ms);
  };
  std::vector<f64> dx(m, 0.0), xt = u_, fint(m), R(m), delta(m);
  if (opt_.warm_start && !be) {
    dx = d_prev_;
    apply_increment(L, xt.data(), dx.data(), 1.0, opt_.corot);
  }
  f64 r0n = 0.0;
  const int iters = opt_.corot ? std::max(1, opt_.newton_iters) : 1;
  for (int it = 0; it <= iters; ++it) {
    internal_forces(L, xt.data(), opt_.corot, fint.data());
    for (size_t k = 0; k < m; ++k) R[k] = R0[k] - fint[k];
    apply_mass(dx.data(), R.data(), -cm);
    if (bt != 0.0) {
      apply_stiffness(L, dx.data(), tmp.data());
      for (size_t k = 0; k < m; ++k) R[k] -= bt * c * tmp[k];
    }
    const f64 rn = norm2(R);
    if (it == 0) r0n = rn;
    st.residual = r0n > 0.0 ? rn / r0n : 0.0;
    if (rn == 0.0 || (it > 0 && (rn <= opt_.newton_rtol * r0n || rn <= opt_.newton_atol * fscale)) || it == iters)
      break;
    if (it > 0) refresh_operator(xt, &st);
    for (size_t k = 0; k < m; ++k) R[k] /= ck;
    const auto t_solve = Clock::now();
    const PcgStats ps = pcg_solve_op(op, mg_, L.n, R.data(), delta.data(), opt_.lin_rtol, opt_.lin_maxit, false,
                                     opt_.lin_atol * fscale / ck);
    st.ms_solve += ms_since(t_solve);
    st.pcg += ps.iters;
    if (ps.iters >= opt_.mg_rebuild_iters) mg_dirty_ = true;
    for (size_t k = 0; k < m; ++k) dx[k] += delta[k];
    xt = u_;
    apply_increment(L, xt.data(), dx.data(), 1.0, opt_.corot);
    ++st.newton;
    if (!opt_.corot) break;
  }
  u_.swap(xt);
  parallel_for(im, kVecGrain, [&](i64 b, i64 en) {
    for (i64 k = b; k < en; ++k) {
      const f64 vn = mdiag_[k] > 0.0 ? c * (dx[k] + e[k]) : 0.0;
      v_prev_[k] = v_[k];
      v_[k] = vn;
      d_prev_[k] = dx[k];
    }
  });
  be_next_ = false;
  time_ += dt;
  ++steps_;
  std::fill(f_once_.begin(), f_once_.end(), 0.0);
  timed_.erase(std::remove_if(timed_.begin(), timed_.end(), [&](const TimedLoad& t) { return t.t1 < time_; }),
               timed_.end());

  if (opt_.damage) {
    const auto t_law = Clock::now();
    LawSweep sw = sweep_law(L, u_.data(), L.dmg, opt_.corot, L.kscale);
    st.max_damage = sw.max_damage;
    st.ms_law += ms_since(t_law);
    const auto t_topo = Clock::now();
    if (!sw.candidates.empty()) rupture_candidates(sw.candidates, &st);
    st.ms_topo += ms_since(t_topo);
  }

  f64 vmax = 0.0;
  for (i32 i = 0; i < L.n; ++i) {
    if (L.anchored[i]) continue;
    const f64* vi = &v_[6 * size_t(i)];
    vmax = std::max(vmax, std::sqrt(vi[0] * vi[0] + vi[1] * vi[1] + vi[2] * vi[2]));
  }
  st.max_speed = vmax;
  st.kinetic = kinetic_energy();
  const bool quiet = st.ruptured == 0 && st.detached_cells == 0 && timed_.empty() && vmax < opt_.sleep_velocity;
  quiet_steps_ = quiet ? quiet_steps_ + 1 : 0;
  if (quiet_steps_ >= opt_.sleep_steps) {
    quiet_steps_ = 0;
    bool stay_awake = false;
    const auto t_settle = Clock::now();
    if (opt_.settle) {
      StaticsOptions so = opt_.statics;
      so.corot = opt_.corot;
      so.g = opt_.g;
      so.damage = opt_.damage;
      const std::vector<f64> f = gravity_vector(L, opt_.g);
      const DamageField dc = L.dmg;
      std::vector<f64> us = u_;
      solve_equilibrium(L, us, f, dc, so, nullptr);  // trial damage at rest lands in L.dmg
      set_state(us);
      if (opt_.damage) {
        LawSweep sw = sweep_law(L, u_.data(), L.dmg, opt_.corot, L.kscale);
        if (!sw.candidates.empty() && rupture_candidates(sw.candidates, &st) > 0) stay_awake = true;
      }
      st.settled = true;
    }
    st.ms_settle += ms_since(t_settle);
    if (!stay_awake) {
      asleep_ = true;
      st.asleep = true;
    }
  }
  return st;
}

BlastResult Dynamics::blast(const BlastParams& bp) {
  Lattice& L = *L_;
  std::vector<f64> f(6 * size_t(L.n), 0.0);
  add_blast_impulse(L, u_.data(), bp, f.data());
  BlastResult res = apply_blast(L, u_.data(), bp);
  for (i32 c : res.removed)
    for (int q = 0; q < 6; ++q) {
      const size_t o = 6 * size_t(c) + q;
      u_[o] = v_[o] = v_prev_[o] = d_prev_[o] = 0.0;
      f[o] = 0.0;
    }
  for (size_t k = 0; k < f.size(); ++k) f_once_[k] += f[k];
  rebuild_mass();
  StepStats st;
  CutSet cut;
  cut.seeds = res.seeds;
  cut.supports_removed = res.supports_removed;
  detach(cut, nullptr, nullptr, &st);
  mg_dirty_ = true;
  be_next_ = true;
  wake();
  return res;
}

void Dynamics::carve(const std::vector<i32>& cells) {
  Lattice& L = *L_;
  const CutSet cut = removal_cut(L, cells);
  for (i32 c : cells) {
    if (c < 0 || c >= L.n || L.dead[c]) continue;
    L.remove_cell(c);
    for (int q = 0; q < 6; ++q) {
      const size_t o = 6 * size_t(c) + q;
      u_[o] = v_[o] = v_prev_[o] = d_prev_[o] = 0.0;
    }
  }
  rebuild_mass();
  StepStats st;
  detach(cut, nullptr, nullptr, &st);
  mg_dirty_ = true;
  be_next_ = true;
  wake();
}

}  // namespace svx
