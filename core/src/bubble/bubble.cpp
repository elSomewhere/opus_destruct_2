#include "svx/bubble/bubble.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "svx/base/parallel.hpp"
#include "svx/base/work.hpp"
#include "svx/sim/corot.hpp"
#include "svx/solve/block6.hpp"

namespace svx {

using namespace blk6;

namespace {

using Clock = std::chrono::steady_clock;
inline f64 ms_since(Clock::time_point t0) { return std::chrono::duration<f64, std::milli>(Clock::now() - t0).count(); }
// thread CPU time (ms): the setup profile on a loaded machine
inline f64 cpu_ms() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return 1e3 * static_cast<f64>(ts.tv_sec) + 1e-6 * static_cast<f64>(ts.tv_nsec);
#else
  return std::chrono::duration<f64, std::milli>(Clock::now().time_since_epoch()).count();
#endif
}

f64 norm2(const std::vector<f64>& v) {
  f64 s = 0.0;
  for (f64 x : v) s += x * x;
  return std::sqrt(s);
}

// y += s * M x (block-diagonal composite mass)
void mass_apply(const Composite& C, const f64* x, f64* y, f64 s) {
  for (i32 r = 0; r < C.n; ++r) {
    f64 t[6];
    mv6(&C.M[36 * size_t(r)], x + 6 * size_t(r), t);
    for (int q = 0; q < 6; ++q) y[6 * size_t(r) + q] += s * t[q];
  }
}

// Mass-weighted rigid projection of a fine field (6 per cell) onto the composite: exact linear
// and angular momentum when the field is a velocity (plan §B5 re-partition).
void project_rigid(const Lattice& L, const Composite& C, const f64* uf, f64* x) {
  for (i32 k = 0; k < C.n; ++k) {
    f64* X = x + 6 * size_t(k);
    for (int q = 0; q < 6; ++q) X[q] = 0.0;
    if (C.level[k] == 0) {
      const i32 c = C.cell[k];
      for (int q = 0; q < 6; ++q) X[q] = uf[6 * size_t(c) + q];
      continue;
    }
    // generalized momentum P^T M u, then solve with the node's Galerkin mass
    f64 g[6] = {0, 0, 0, 0, 0, 0};
    for (i32 e = C.mptr[k]; e < C.mptr[k + 1]; ++e) {
      const i32 c = C.members[e];
      const f64 m = L.mass[c];
      const f64* u = uf + 6 * size_t(c);
      const f64* o = &C.off[3 * size_t(c)];
      const f64 p[3] = {m * u[0], m * u[1], m * u[2]};
      g[0] += p[0];
      g[1] += p[1];
      g[2] += p[2];
      g[3] += L.inertia[c][0] * u[3] + o[1] * p[2] - o[2] * p[1];
      g[4] += L.inertia[c][1] * u[4] + o[2] * p[0] - o[0] * p[2];
      g[5] += L.inertia[c][2] * u[5] + o[0] * p[1] - o[1] * p[0];
    }
    f64 Minv[36];
    if (inv6(&C.M[36 * size_t(k)], Minv)) mv6(Minv, g, X);
  }
}

}  // namespace

std::vector<f64> removal_residual(const Lattice& L, std::span<const i32> cells, const std::vector<f64>& u0, bool corot) {
  std::vector<f64> r(6 * size_t(L.n), 0.0);
  std::vector<u8> removed(L.n, 0);
  for (i32 c : cells)
    if (c >= 0 && c < L.n) removed[c] = 1;
  f64 fi[6], fj[6];
  for (i32 c : cells) {
    if (c < 0 || c >= L.n || L.dead[c]) continue;
    for (int a = 0; a < 3; ++a) {
      // bonds (c -> +a) and (-a -> c)
      for (int side = 0; side < 2; ++side) {
        const i32 lo = side == 0 ? c : L.nbrm[a][c];
        if (lo < 0) continue;
        const i32 hi = L.nbr[a][lo];
        if (hi < 0) continue;
        const i32 other = side == 0 ? hi : lo;
        if (removed[other]) continue;  // bond between two removed cells
        bond_internal_forces(L, a, lo, u0.data(), corot, fi, fj);
        const f64* fo = other == lo ? fi : fj;
        if (L.anchored[other]) continue;
        // the survivor loses this bond's contribution to f_int: r = +contribution
        for (int q = 0; q < 6; ++q) r[6 * size_t(other) + q] += fo[q];
      }
    }
  }
  for (i32 i = 0; i < L.n; ++i) {
    const u8 m = L.fixmask[i];
    if (!m) continue;
    for (int q = 0; q < 6; ++q)
      if ((m >> q) & 1) r[6 * size_t(i) + q] = 0.0;
  }
  return r;
}

void Bubble::init(Lattice& L, const std::vector<f64>& u0, const std::vector<f64>& r,
                  std::span<const std::array<f64, 3>> centers, const BubbleOptions& opt) {
  L_ = &L;
  opt_ = opt;
  if (const char* e = std::getenv("SVX_PROJ")) opt_.projection_basis = std::atoi(e);  // (experiments)
  if (const char* e = std::getenv("SVX_REBUILD_RUPTURES")) opt_.rebuild_ruptures = std::atoi(e);
  if (const char* e = std::getenv("SVX_FRAME_REBUILD")) opt_.frame_rebuild = std::atof(e);
  if (const char* e = std::getenv("SVX_COROT_PC")) opt_.rotate_operator = std::atoi(e) != 0;
  if (const char* e = std::getenv("SVX_NEWTON_ITERS")) opt_.newton_iters = std::atoi(e);
  if (const char* e = std::getenv("SVX_INFLATION")) opt_.demand_inflation = std::atof(e);
  if (const char* e = std::getenv("SVX_NEWTON_RTOL")) opt_.newton_rtol = std::atof(e);
  if (const char* e = std::getenv("SVX_NEWTON_ATOL")) opt_.newton_atol = std::atof(e);
  if (const char* e = std::getenv("SVX_NOMINATE_PHI")) opt_.nominate_phi = std::atof(e);
  if (const char* e = std::getenv("SVX_NOMINATE")) opt_.nominate = std::atoi(e) != 0;
  if (const char* e = std::getenv("SVX_DECIDE")) opt_.decide = std::atoi(e) != 0;
  if (const char* e = std::getenv("SVX_DECIDE_PHI")) opt_.decide_phi = std::atof(e);
  if (const char* e = std::getenv("SVX_MAX_REFINEMENTS")) opt_.max_refinements = std::atoi(e);
  if (const char* e = std::getenv("SVX_MARGINAL_EPS")) opt_.marginal_eps = std::atof(e);
  if (const char* e = std::getenv("SVX_MAX_FINE")) opt_.max_fine_cells = std::atoi(e);
  opt_.projection_basis = std::clamp(opt_.projection_basis, 0, 32);
  L.enable_damage();
  u0_ = u0;
  u0_.resize(6 * size_t(L.n), 0.0);
  CompositeOptions co = opt.comp;
  co.fine_bonds = false;
  f64 t = cpu_ms();
  auto lap = [&](f64& slot) {
    const f64 now = cpu_ms();
    slot = now - t;
    t = now;
  };
  init_prof_ = InitProfile{};
  C_ = build_composite(L, centers, co);
  ++comp_gen_;
  lap(init_prof_.composite);
  C_.A.make_simd();  // the residual's linear couplings (every Newton iteration)
  lap(init_prof_.simd);
  projection_reset();
  std::vector<i32> fine;
  fine.reserve(C_.n_fine);
  for (i32 k = 0; k < C_.n; ++k)
    if (C_.level[k] == 0) fine.push_back(C_.cell[k]);
  S_ = extract_sublattice(L, fine);
  lap(init_prof_.sublattice);
  const Lattice& F = S_.F;
  fnode_.assign(F.n, -1);
  u0F_.assign(6 * size_t(F.n), 0.0);
  for (i32 k = 0; k < F.n; ++k) {
    const i32 c = S_.to_world[k];
    if (!F.anchored[k]) fnode_[k] = C_.node_of[c];
    for (int q = 0; q < 6; ++q) u0F_[6 * size_t(k) + q] = u0_[6 * size_t(c) + q];
  }
  fint0_.assign(6 * size_t(F.n), 0.0);
  internal_forces(S_.F, u0F_.data(), opt_.corot, fint0_.data());
  lap(init_prof_.forces);
  r_.assign(6 * size_t(C_.n), 0.0);
  composite_restrict(L, C_, r.data(), r_.data());
  lap(init_prof_.restrict_residual);
  const size_t m = 6 * size_t(C_.n);
  x_.assign(m, 0.0);
  v_.assign(m, 0.0);
  v_prev_.assign(m, 0.0);
  d_prev_.assign(m, 0.0);
  f_once_.assign(m, 0.0);
  // load scale: the event residual plus the weight carried by the fine region
  f64 w = 0.0;
  for (i32 k = 0; k < F.n; ++k)
    if (!F.anchored[k]) w += F.mass[k] * 9.81;
  load_scale_ = std::max(norm2(r_), w) + 1e-30;
  mg_dirty_ = true;
  dirty_why_ |= 1;
  be_next_ = true;
  asleep_ = false;
  quiet_ = 0;
  steps_ = 0;
  time_ = 0.0;
  ruptured_.clear();
  detached_ = 0;
  islands_.clear();
  conn_ = ConnStats{};
  refinements_ = 0;
  refinements_total_ = 0;
  coarsenings_ = 0;
  grows_ = 0;
  grows_total_ = 0;
  period_start_ = 0.0;
  held_ = 0;
  event_fine_ = C_.n_fine;
  event_step_ = 0;
  base_dt_ = opt_.dt;
  contact_seeds_.clear();
  refined_.clear();
  flagged_.clear();
  safe_.clear();
  rep_dp_ = 0.0;
  rep_dke_ = -1e300;
}

void Bubble::add_force(i32 world_cell, const f64* f6) {
  once_.push_back({world_cell, {f6[0], f6[1], f6[2], f6[3], f6[4], f6[5]}});
  apply_force(world_cell, f6);
}

void Bubble::apply_force(i32 world_cell, const f64* f6) {
  const i32 k = C_.node_of[world_cell];
  if (k < 0) return;
  const f64* o = &C_.off[3 * size_t(world_cell)];
  f64* R = &f_once_[6 * size_t(k)];
  R[0] += f6[0];
  R[1] += f6[1];
  R[2] += f6[2];
  R[3] += f6[3] + o[1] * f6[2] - o[2] * f6[1];
  R[4] += f6[4] + o[2] * f6[0] - o[0] * f6[2];
  R[5] += f6[5] + o[0] * f6[1] - o[1] * f6[0];
  const u8 fm = C_.fix[k];
  for (int q = 0; q < 6; ++q)
    if ((fm >> q) & 1) R[q] = 0.0;
  asleep_ = false;
  quiet_ = 0;
}

void Bubble::fine_state(const f64* x, std::vector<f64>& uF) const {
  const Lattice& F = S_.F;
  uF = u0F_;
  for (i32 k = 0; k < F.n; ++k) {
    const i32 node = fnode_[k];
    if (node < 0 || F.dead[k]) continue;
    for (int q = 0; q < 6; ++q) uF[6 * size_t(k) + q] += x[6 * size_t(node) + q];
  }
}

void Bubble::rotate_in(const f64* x, f64* t) const {
  std::memcpy(t, x, sizeof(f64) * 6 * size_t(C_.n));
  for (i32 k : rot_nodes_) {
    const f64* R = &rot_[9 * size_t(k)];
    for (int b = 0; b < 6; b += 3) {
      const f64* v = x + 6 * size_t(k) + b;
      f64* o = t + 6 * size_t(k) + b;
      for (int r = 0; r < 3; ++r) o[r] = R[r] * v[0] + R[3 + r] * v[1] + R[6 + r] * v[2];  // R^T v
    }
  }
}

void Bubble::rotate_out(const f64* t, f64* y) const {
  std::memcpy(y, t, sizeof(f64) * 6 * size_t(C_.n));
  for (i32 k : rot_nodes_) {
    const f64* R = &rot_[9 * size_t(k)];
    for (int b = 0; b < 6; b += 3) {
      const f64* v = t + 6 * size_t(k) + b;
      f64* o = y + 6 * size_t(k) + b;
      for (int r = 0; r < 3; ++r) o[r] = R[3 * r] * v[0] + R[3 * r + 1] * v[1] + R[3 * r + 2] * v[2];  // R v
    }
  }
}

namespace {

// Rotation matrix (row-major) of a rotation vector: the rotation quat_from_rotvec describes.
void rotvec_matrix(const f64* th, f64* R) {
  const Quat q = quat_from_rotvec(th);
  const f64 x = q[0], y = q[1], z = q[2], w = q[3];
  R[0] = 1 - 2 * (y * y + z * z);
  R[1] = 2 * (x * y - z * w);
  R[2] = 2 * (x * z + y * w);
  R[3] = 2 * (x * y + z * w);
  R[4] = 1 - 2 * (x * x + z * z);
  R[5] = 2 * (y * z - x * w);
  R[6] = 2 * (x * z - y * w);
  R[7] = 2 * (y * z + x * w);
  R[8] = 1 - 2 * (x * x + y * y);
}

// The largest scale <= 1 keeping every node's translation below max_t (m) and rotation below
// max_r (rad); 0 for a non-finite increment.
f64 trust_scale(const std::vector<f64>& d, f64 max_t, f64 max_r) {
  f64 mt = 0.0, mr = 0.0;
  for (size_t k = 0; k + 5 < d.size(); k += 6) {
    const f64 t = std::sqrt(d[k] * d[k] + d[k + 1] * d[k + 1] + d[k + 2] * d[k + 2]);
    const f64 r = std::sqrt(d[k + 3] * d[k + 3] + d[k + 4] * d[k + 4] + d[k + 5] * d[k + 5]);
    if (!std::isfinite(t) || !std::isfinite(r)) return 0.0;
    mt = std::max(mt, t);
    mr = std::max(mr, r);
  }
  f64 s = 1.0;
  if (mt > max_t) s = std::min(s, max_t / mt);
  if (mr > max_r) s = std::min(s, max_r / mr);
  return s;
}

}  // namespace

// Each fine node's rotation since the last build, T = R(now) R(build)^T; the projection basis
// follows it (v -> T' T^T v keeps V A'-orthonormal and W = A' V for A' = T' A T'^T).
void Bubble::update_rotation(const std::vector<f64>& uF) {
  const Lattice& F = S_.F;
  const size_t m = 6 * size_t(C_.n);
  std::vector<f64> rot(9 * size_t(C_.n), 0.0);
  std::vector<i32> nodes;
  for (i32 k = 0; k < F.n; ++k) {
    const i32 node = fnode_[k];
    if (node < 0 || F.dead[k]) continue;
    const f64* tn = &uF[6 * size_t(k) + 3];
    const f64* tb = &th_build_[6 * size_t(k) + 3];
    if (tn[0] == tb[0] && tn[1] == tb[1] && tn[2] == tb[2]) continue;
    f64 Rn[9], Rb[9];
    rotvec_matrix(tn, Rn);
    rotvec_matrix(tb, Rb);
    f64* T = &rot[9 * size_t(node)];
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) T[3 * r + c] = Rn[3 * r] * Rb[3 * c] + Rn[3 * r + 1] * Rb[3 * c + 1] + Rn[3 * r + 2] * Rb[3 * c + 2];
    nodes.push_back(node);
  }
  std::sort(nodes.begin(), nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  if (!proj_v_.empty()) {
    std::vector<f64> t(m);
    for (auto* basis : {&proj_v_, &proj_w_})
      for (std::vector<f64>& v : *basis) {
        rotate_in(v.data(), t.data());  // (the old rotation)
        v.swap(t);
      }
  }
  rot_.swap(rot);
  rot_nodes_.swap(nodes);
  if (!proj_v_.empty()) {
    std::vector<f64> t(m);
    for (auto* basis : {&proj_v_, &proj_w_})
      for (std::vector<f64>& v : *basis) {
        rotate_out(v.data(), t.data());  // (the new rotation)
        v.swap(t);
      }
  }
}

void Bubble::nonlinear_part(const f64* x, f64* out) {
  const Lattice& F = S_.F;
  fine_state(x, scratch_uF_);
  scratch_fF_.resize(6 * size_t(F.n));
  internal_forces(F, scratch_uF_.data(), opt_.corot, scratch_fF_.data());
  std::fill(out, out + 6 * size_t(C_.n), 0.0);
  for (i32 k = 0; k < F.n; ++k) {
    const i32 node = fnode_[k];
    if (node < 0 || F.dead[k]) continue;
    for (int q = 0; q < 6; ++q)
      out[6 * size_t(node) + q] += scratch_fF_[6 * size_t(k) + q] - fint0_[6 * size_t(k) + q];
  }
}

void Bubble::coarse_apply(const f64* x, f64* y) const {
  if (!C_.A.valc.empty()) C_.A.apply_simd(x, y);
  else C_.A.apply(x, y);
}

void Bubble::tangent_apply(const f64* x, f64* y) {
  const Lattice& F = S_.F;
  coarse_apply(x, y);
  std::vector<f64>& xf = scratch_uF_;
  std::vector<f64>& yf = scratch_fF_;
  xf.assign(6 * size_t(F.n), 0.0);
  yf.resize(6 * size_t(F.n));
  for (i32 k = 0; k < F.n; ++k) {
    const i32 node = fnode_[k];
    if (node < 0 || F.dead[k]) continue;
    for (int q = 0; q < 6; ++q) xf[6 * size_t(k) + q] = x[6 * size_t(node) + q];
  }
  apply_stiffness(F, xf.data(), yf.data());
  for (i32 k = 0; k < F.n; ++k) {
    const i32 node = fnode_[k];
    if (node < 0 || F.dead[k]) continue;
    for (int q = 0; q < 6; ++q) y[6 * size_t(node) + q] += yf[6 * size_t(k) + q];
  }
}

void Bubble::rebuild_preconditioner() {
  Bsr6 A = C_.A.without_copies();
  add_sublattice_bonds(C_, S_, A);
  MGOptions mo = opt_.mg;
  const f64 c = opt_.bdf2 ? 1.5 / opt_.dt : 1.0 / opt_.dt;
  mo.mass_shift = (c * c + opt_.rayleigh_alpha * c) / (1.0 + opt_.rayleigh_beta * c);
  mo.greedy = true;
  mo.cycle_gamma = 1;  // V-cycle: the Galerkin coarse levels of a composite are relatively dense
  mo.cheb_degree = 3;
  // experiment knobs (not part of the deterministic state; defaults = tuned values)
  if (const char* e = std::getenv("SVX_SGS")) mo.sgs = std::atoi(e) != 0;
  if (const char* e = std::getenv("SVX_CSCALE")) mo.coarse_scale = std::atof(e);
  if (const char* e = std::getenv("SVX_GAMMA")) mo.cycle_gamma = std::atoi(e);
  if (const char* e = std::getenv("SVX_CHEB")) mo.cheb_degree = std::atoi(e);
  if (const char* e = std::getenv("SVX_F32")) mo.f32_levels = std::atoi(e) != 0;
  if (const char* e = std::getenv("SVX_STRENGTH")) mo.strength = std::atof(e);
  if (const char* e = std::getenv("SVX_EIG_ITERS")) mo.eig_iters = std::atoi(e);
  if (const char* e = std::getenv("SVX_LINRTOL")) opt_.lin_rtol = std::atof(e);
  if (const char* e = std::getenv("SVX_LINATOL")) opt_.lin_atol = std::atof(e);
  mg_.build_assembled(A, C_.M, C_.X, C_.blk, C_.fix, mo);
  projection_reset();  // a new operator
  rot_.clear();        // (built at the current frames)
  rot_nodes_.clear();
  ruptures_since_build_ = 0;
  if (std::getenv("SVX_MG_INFO")) {
    const auto ns = mg_.level_sizes();
    const auto nb = mg_.level_blocks();
    std::printf("    [mg] levels:");
    for (size_t l = 0; l < ns.size(); ++l)
      std::printf(" %d nodes/%lld blocks (%.1f/row)", ns[l], static_cast<long long>(l < nb.size() ? nb[l] : 0),
                  ns[l] ? static_cast<f64>(l < nb.size() ? nb[l] : 0) / ns[l] : 0.0);
    std::printf("; fine cells %d of %d nodes\n", S_.F.n, C_.n);
    // raw throughput of the level-0 operator (the Krylov operator = the smoother's matrix)
    std::vector<f64> xa(6 * size_t(C_.n)), ya(6 * size_t(C_.n));
    for (size_t k = 0; k < xa.size(); ++k) xa[k] = 1e-3 * static_cast<f64>((k * 7919) % 1000);
    f64 ms = 1e30;
    for (int batch = 0; batch < 5; ++batch) {
      const auto t0 = Clock::now();
      constexpr int kReps = 40;
      for (int r = 0; r < kReps; ++r) mg_.apply_fine_operator(xa.data(), ya.data());
      ms = std::min(ms, ms_since(t0) / kReps);
    }
    std::printf("    [mg] level-0 apply: %.3f ms (%.1f ns/row, %.2f GB/s of blocks)\n", ms, 1e6 * ms / C_.n,
                ms > 0 ? 288.0 * static_cast<f64>(mg_.level_blocks()[0]) / (ms * 1e6) : 0.0);
  }
  mg_dirty_ = false;
  mg_framed_ = S_.F.framed();
}

void Bubble::rebuild_operator(const std::vector<u8>* drop) {
  // Re-partition with the same centres after aggregate members detached: project the delta
  // state (displacement, velocities, history) through the fine level onto the new composite.
  Lattice& L = *L_;
  const size_t nf = 6 * size_t(L.n);
  std::vector<f64> ux(nf), uv(nf), uvp(nf), udp(nf);
  composite_prolong(L, C_, x_.data(), ux.data());
  composite_prolong(L, C_, v_.data(), uv.data());
  composite_prolong(L, C_, v_prev_.data(), uvp.data());
  composite_prolong(L, C_, d_prev_.data(), udp.data());
  // keep the fine region: the old fine cells stay fine
  CompositeOptions co = opt_.comp;
  co.fine_bonds = false;
  for (i32 k = 0; k < C_.n; ++k)
    if (C_.level[k] == 0 && !L.dead[C_.cell[k]] && !(drop && (*drop)[size_t(C_.cell[k])])) co.force_fine.push_back(C_.cell[k]);
  std::vector<std::array<f64, 3>> centers;  // no new growth: centres at the fine cells only
  Composite Cn = build_composite(L, centers, co);
  // Old node loads -> new composite. Fine-node loads map exactly; an aggregate's load is
  // re-expressed at its old reference point on its first surviving member (a rigid resultant).
  std::vector<f64> rf(nf, 0.0);
  for (i32 k = 0; k < C_.n; ++k) {
    const f64* Rk = &r_[6 * size_t(k)];
    if (C_.level[k] == 0) {
      const i32 c = C_.cell[k];
      if (L.dead[c]) continue;
      for (int q = 0; q < 6; ++q) rf[6 * size_t(c) + q] += Rk[q];
      continue;
    }
    i32 host = -1;
    for (i32 e = C_.mptr[k]; e < C_.mptr[k + 1] && host < 0; ++e)
      if (!L.dead[C_.members[e]]) host = C_.members[e];
    if (host < 0) continue;
    // move the wrench from X_k to the host cell centre: m' = m - (x_host - X) x f
    const f64* o = &C_.off[3 * size_t(host)];
    f64* fh = &rf[6 * size_t(host)];
    fh[0] += Rk[0];
    fh[1] += Rk[1];
    fh[2] += Rk[2];
    fh[3] += Rk[3] - (o[1] * Rk[2] - o[2] * Rk[1]);
    fh[4] += Rk[4] - (o[2] * Rk[0] - o[0] * Rk[2]);
    fh[5] += Rk[5] - (o[0] * Rk[1] - o[1] * Rk[0]);
  }
  C_ = std::move(Cn);
  ++comp_gen_;
  C_.A.make_simd();
  projection_reset();
  const size_t m = 6 * size_t(C_.n);
  x_.assign(m, 0.0);
  v_.assign(m, 0.0);
  v_prev_.assign(m, 0.0);
  d_prev_.assign(m, 0.0);
  project_rigid(L, C_, ux.data(), x_.data());
  project_rigid(L, C_, uv.data(), v_.data());
  project_rigid(L, C_, uvp.data(), v_prev_.data());
  project_rigid(L, C_, udp.data(), d_prev_.data());
  r_.assign(m, 0.0);
  composite_restrict(L, C_, rf.data(), r_.data());
  f_once_.assign(m, 0.0);
  // the sub-lattice keeps its cells; remap its nodes
  const Lattice& F = S_.F;
  for (i32 k = 0; k < F.n; ++k) fnode_[k] = (F.anchored[k] || F.dead[k]) ? -1 : C_.node_of[S_.to_world[k]];
  mg_dirty_ = true;
  dirty_why_ |= 4;
  be_next_ = true;
}

std::vector<i32> Bubble::nominate() {
  const Lattice& L = *L_;
  // (the work: a pass over the cells, a law evaluation per crossing bond - counted below)
  add_work(i64(L.n) / 8);
  i64 evals = 0;
  std::vector<f64> ud(6 * size_t(L.n), 0.0);  // the delta at the world cells: rigid node motions
  composite_prolong(L, C_, x_.data(), ud.data());
  constexpr f64 kScf[4] = {1.0, 2.0, 4.0, 8.0};
  std::vector<u8> flag(size_t(C_.n), 0);
  std::vector<f64> bound(size_t(C_.n), 0.0);
  std::vector<f64> own(size_t(C_.n), 0.0);  // the composite's own estimate (no amplification)
  for (int a = 0; a < 3; ++a)
    for (i32 i = 0; i < L.n; ++i) {
      const i32 j = L.nbr[a][i];
      if (j < 0 || L.dead[i] || L.dead[j]) continue;
      const i32 ni = C_.node_of[i], nj = C_.node_of[j];
      if (ni == nj) continue;  // (inside an aggregate, or anchored on both sides)
      const int li = ni >= 0 ? C_.level[ni] : 0, lj = nj >= 0 ? C_.level[nj] : 0;
      const int lev = std::min(3, std::max(li, lj));
      if (lev == 0) continue;  // fine on both sides: the law decides already
      if ((li == 0 || flag[size_t(ni)]) && (lj == 0 || flag[size_t(nj)])) continue;
      ++evals;
      const Vec6 g0 = bond_jump(L, a, i, u0_.data(), opt_.corot);
      const Vec6 gd = bond_jump(L, a, i, ud.data(), false);
      Vec6 g;
      // (the coarser side's factor also where the other side is fine: with the finer side's, the
      // column-removal library lost a collapse at its threshold - docs/STATUS.md)
      for (int q = 0; q < 6; ++q) g[q] = (g0[q] + kScf[lev] * gd[q]) * L.kscale * opt_.demand_inflation;
      const f64 d = L.dmg[a].empty() ? 0.0 : f64(L.dmg[a][i]);  // (its committed damage: a coarsened
      const f64 phi = evaluate_law(L.bond(a, i), g, d, L.law.game).phi;  // crater rim comes back)
      if (phi < opt_.nominate_phi) continue;
      Vec6 g1;
      for (int q = 0; q < 6; ++q) g1[q] = (g0[q] + gd[q]) * L.kscale * opt_.demand_inflation;
      const f64 phi1 = evaluate_law(L.bond(a, i), g1, d, L.law.game).phi;
      if (li > 0) {
        flag[size_t(ni)] = 1;
        bound[size_t(ni)] = std::max(bound[size_t(ni)], phi);
        own[size_t(ni)] = std::max(own[size_t(ni)], phi1);
      }
      if (lj > 0) {
        flag[size_t(nj)] = 1;
        bound[size_t(nj)] = std::max(bound[size_t(nj)], phi);
        own[size_t(nj)] = std::max(own[size_t(nj)], phi1);
      }
    }
  add_work(evals * kWorkLawSweep);
  // (persistence: an aggregate flagged in nominate_persist checks in a row - a demand that stays,
  // not a passing wave - identified by its first member)
  std::unordered_map<i32, int> seen;
  for (i32 k = 0; k < C_.n; ++k) {
    if (!flag[size_t(k)] || C_.mptr[k] == C_.mptr[k + 1]) continue;
    const i32 id = C_.members[C_.mptr[k]];
    const auto it = flagged_.find(id);
    const int n = (it == flagged_.end() ? 0 : it->second) + 1;
    seen.emplace(id, n);
    if (n < opt_.nominate_persist) flag[size_t(k)] = 0;
    // decided safe before, and its bound has not grown by decide_regrow since: not again
    const auto sf = safe_.find(id);
    if (flag[size_t(k)] && sf != safe_.end() && bound[size_t(k)] < sf->second + opt_.decide_regrow) flag[size_t(k)] = 0;
  }
  flagged_.swap(seen);
  // Decide (plan §B6.2): the flagged aggregates and the composite nodes around them, the ring's
  // outer layer held at the composite's motion, solved statically at the fine level; only an
  // aggregate whose fine bonds there near onset becomes fine - else it is safe (until its bound
  // grows).
  if (opt_.decide) {
    std::vector<u8> inside(size_t(L.n), 0);  // 1: a flagged aggregate's, 2: the ring's
    std::vector<i32> patch;
    auto take = [&](i32 node, u8 tag) {
      for (i32 e = C_.mptr[node]; e < C_.mptr[node + 1]; ++e)
        if (!inside[size_t(C_.members[e])]) {
          inside[size_t(C_.members[e])] = tag;
          patch.push_back(C_.members[e]);
        }
    };
    for (i32 k = 0; k < C_.n; ++k)
      if (flag[size_t(k)]) take(k, 1);
    const size_t nset = patch.size();
    std::vector<u8> ring(size_t(C_.n), 0);
    for (size_t q = 0; q < nset; ++q)
      for (int a = 0; a < 3; ++a)
        for (const i32 nb : {L.nbr[a][patch[q]], L.nbrm[a][patch[q]]}) {
          if (nb < 0 || L.dead[nb]) continue;
          const i32 m = C_.node_of[nb];
          if (m < 0 || flag[size_t(m)] || ring[size_t(m)]) continue;
          ring[size_t(m)] = 1;
          take(m, 2);
        }
    if (nset > 0) {
      std::sort(patch.begin(), patch.end());
      SubLattice D = extract_sublattice(L, patch);
      Lattice& W = D.F;
      std::vector<f64> ut;
      total_displacement(ut);
      std::vector<f64> u(6 * size_t(W.n), 0.0);
      W.u_fixed.assign(6 * size_t(W.n), 0.0);
      i32 pinned = 0;
      for (i32 k = 0; k < W.n; ++k) {
        const i32 c = D.to_world[k];
        for (int q = 0; q < 6; ++q) u[6 * size_t(k) + q] = ut[6 * size_t(c) + q];
        if (W.anchored[k] || inside[size_t(c)] != 2) continue;
        bool edge = false;  // the ring's outer layer: next to a free cell outside the patch
        for (int a = 0; a < 3 && !edge; ++a)
          for (const i32 nb : {L.nbr[a][c], L.nbrm[a][c]})
            if (nb >= 0 && !L.dead[nb] && !L.anchored[nb] && !inside[size_t(nb)]) edge = true;
        if (!edge) continue;
        W.fixmask[k] = kAllDofs;
        W.anchored[k] = 1;
        for (int q = 0; q < 6; ++q) W.u_fixed[6 * size_t(k) + q] = ut[6 * size_t(c) + q];
        ++pinned;
      }
      StaticsOptions so;
      so.corot = opt_.corot;
      so.damage = false;
      so.res_tol = 1e-6;
      so.lin_rtol = 1e-3;
      so.max_pcg_total = opt_.decide_max_pcg;
      const DamageField dc = W.dmg;
      const EquilibriumStats es = solve_equilibrium(W, u, gravity_vector(W, 9.81), dc, so, nullptr);
      std::vector<f64> worst(size_t(C_.n), 0.0);  // per flagged aggregate: its bonds' largest demand
      for (int a = 0; a < 3; ++a)
        for (i32 k = 0; k < W.n; ++k) {
          const i32 j = W.nbr[a][k];
          if (j < 0 || W.dead[k] || W.dead[j]) continue;
          const i32 ci = D.to_world[k], cj = D.to_world[j];
          if (inside[size_t(ci)] != 1 && inside[size_t(cj)] != 1) continue;
          Vec6 g = bond_jump(W, a, k, u.data(), opt_.corot);
          for (auto& x : g) x *= W.kscale * opt_.demand_inflation;
          const f64 phi = evaluate_law(W.bond(a, k), g, dc[a].empty() ? 0.0 : f64(dc[a][k]), W.law.game).phi;
          for (const i32 c : {ci, cj})
            if (inside[size_t(c)] == 1) {
              const i32 node = C_.node_of[c];
              worst[size_t(node)] = std::max(worst[size_t(node)], phi);
            }
        }
      int nflag = 0, nfine = 0;
      f64 max_bound = 0.0, max_worst = 0.0;
      for (i32 k = 0; k < C_.n; ++k) {
        if (!flag[size_t(k)]) continue;
        ++nflag;
        max_bound = std::max(max_bound, bound[size_t(k)]);
        max_worst = std::max(max_worst, worst[size_t(k)]);
        if (worst[size_t(k)] >= opt_.decide_phi) {  // nears onset: fine from now on
          ++nfine;
          continue;
        }
        flag[size_t(k)] = 0;
        safe_[C_.members[C_.mptr[k]]] = bound[size_t(k)];
      }
      if (std::getenv("SVX_NOMINATE_TRACE"))
        std::printf("      [decide] step %lld: %d aggregates (bound <= %.2f), patch %d cells (%d pinned), %d pcg%s: "
                    "fine law <= %.2f, %d become fine\n",
                    static_cast<long long>(steps_), nflag, max_bound, W.n, pinned, es.pcg_iters,
                    es.converged ? "" : " (unconverged)", max_worst, nfine);
    }
  }
  // The fine region stays within max_fine_cells (above the cells events asked for): the
  // aggregates nearest onset first - by the composite's own estimate, which the level factors do
  // not skew (a coarse aggregate's factor 8 would put any loaded one ahead of a finer one at a
  // hinge), then by the bound - ties by their first member (deterministic).
  // An aggregate costs its members and the ring around them (the oversampling), each ring cell
  // with its 2^3 block (a partition cannot aggregate a block holding a fine cell). (A structure
  // failing as a whole - a tower tipping - nominates everything at once; its failures are where
  // the bound is largest.)
  std::vector<i32> order;
  for (i32 k = 0; k < C_.n; ++k)
    if (flag[size_t(k)]) order.push_back(k);
  std::sort(order.begin(), order.end(), [&](i32 a, i32 b) {
    if (own[size_t(a)] != own[size_t(b)]) return own[size_t(a)] > own[size_t(b)];
    if (bound[size_t(a)] != bound[size_t(b)]) return bound[size_t(a)] > bound[size_t(b)];
    return C_.members[C_.mptr[a]] < C_.members[C_.mptr[b]];
  });
  i64 room = i64(opt_.max_fine_cells) + event_fine_ - C_.n_fine;
  std::vector<u8> taken(size_t(L.n), 0);  // cells the refinement makes fine
  std::vector<i32> cells, mark;
  auto is_coarse = [&](i32 c) { return c >= 0 && !L.dead[c] && !L.anchored[c] && C_.node_of[c] >= 0 && C_.level[C_.node_of[c]] > 0; };
  auto take = [&](i32 c) {
    if (!taken[size_t(c)]) {
      taken[size_t(c)] = 1;
      mark.push_back(c);
    }
  };
  for (const i32 k : order) {
    mark.clear();
    for (i32 e = C_.mptr[k]; e < C_.mptr[k + 1]; ++e) take(C_.members[e]);
    const size_t own = mark.size();
    for (size_t q = 0; q < own; ++q)
      for (int a = 0; a < 3; ++a)
        for (const i32 nb : {L.nbr[a][mark[q]], L.nbrm[a][mark[q]]}) {
          if (!is_coarse(nb) || taken[size_t(nb)]) continue;
          // the ring cell and its block: the members of its aggregate in the same 2^3 block
          const i32 node = C_.node_of[nb];
          const std::array<i32, 3> b{L.p[nb][0] >> 1, L.p[nb][1] >> 1, L.p[nb][2] >> 1};
          for (i32 e = C_.mptr[node]; e < C_.mptr[node + 1]; ++e) {
            const i32 m = C_.members[e];
            if ((L.p[m][0] >> 1) == b[0] && (L.p[m][1] >> 1) == b[1] && (L.p[m][2] >> 1) == b[2]) take(m);
          }
        }
    if (static_cast<i64>(mark.size()) <= room) {
      room -= static_cast<i64>(mark.size());
      for (size_t q = 0; q < own; ++q) cells.push_back(mark[q]);
      for (size_t q = own; q < mark.size(); ++q) cells.push_back(mark[q]);
    } else {
      flag[size_t(k)] = 0;
      for (const i32 c : mark) taken[size_t(c)] = 0;
    }
  }
  if (std::getenv("SVX_NOMINATE_TRACE") && !cells.empty()) {
    int by_level[4] = {0, 0, 0, 0};
    for (i32 k = 0; k < C_.n; ++k)
      if (flag[size_t(k)]) ++by_level[std::min(3, int(C_.level[k]))];
    std::printf("      [nominate] step %lld: aggregates L1 %d L2 %d L3+ %d (of %zu flagged), %zu cells (fine %d of %d nodes)\n",
                static_cast<long long>(steps_), by_level[1], by_level[2], by_level[3], order.size(), cells.size(), C_.n_fine,
                C_.n);
  }
  std::sort(cells.begin(), cells.end());
  cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
  return cells;
}

void Bubble::mark_interface() {
  const Lattice& L = *L_;
  const Lattice& F = S_.F;
  near_.assign(size_t(F.n), 0);
  const int reach = std::max(1, opt_.interface_reach);
  std::vector<i32> front, next;
  for (i32 k = 0; k < F.n; ++k) {
    if (F.anchored[k] || F.dead[k]) continue;
    front.assign(1, S_.to_world[k]);
    bool nr = false;
    for (int d = 0; d < reach && !nr; ++d) {  // (through intact bonds, as the load goes)
      next.clear();
      for (const i32 x : front)
        for (int a = 0; a < 3 && !nr; ++a)
          for (const i32 y : {L.nbr[a][x], L.nbrm[a][x]}) {
            if (y < 0 || L.dead[y]) continue;
            const i32 nd = C_.node_of[y];
            if (nd >= 0 && C_.level[nd] > 0) {
              nr = true;
              break;
            }
            next.push_back(y);
          }
      front.swap(next);
    }
    near_[size_t(k)] = nr ? 1 : 0;
  }
}

std::vector<i32> Bubble::aggregates_near(const std::vector<i32>& cells, i64 max_cells) const {
  const Lattice& L = *L_;
  const int reach = std::max(1, opt_.interface_reach);
  std::vector<u8> taken(size_t(C_.n), 0);
  std::vector<i32> out, front, next;
  for (const i32 c : cells) {
    if (static_cast<i64>(out.size()) >= max_cells) break;  // (the bonds first in line get theirs)
    front.assign(1, c);
    for (int d = 0; d < reach; ++d) {
      next.clear();
      for (const i32 x : front)
        for (int a = 0; a < 3; ++a)
          for (const i32 y : {L.nbr[a][x], L.nbrm[a][x]}) {
            if (y < 0 || L.dead[y]) continue;
            const i32 nd = C_.node_of[y];
            if (nd >= 0 && C_.level[nd] > 0 && !taken[size_t(nd)] &&
                static_cast<i64>(out.size()) + (C_.mptr[nd + 1] - C_.mptr[nd]) <= max_cells) {
              taken[size_t(nd)] = 1;
              for (i32 e = C_.mptr[nd]; e < C_.mptr[nd + 1]; ++e) out.push_back(C_.members[e]);
            }
            next.push_back(y);
          }
      front.swap(next);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

void Bubble::refine(const std::vector<i32>& extra) {
  // The re-partition of rebuild_operator with more fine cells: the delta state is projected
  // through the fine level (the new fine cells start from their aggregates' rigid motions) and
  // the fine sub-lattice grows (rebuild_sublattice).
  for (i32 c : extra) opt_.comp.force_fine.push_back(c);
  std::sort(opt_.comp.force_fine.begin(), opt_.comp.force_fine.end());
  opt_.comp.force_fine.erase(std::unique(opt_.comp.force_fine.begin(), opt_.comp.force_fine.end()), opt_.comp.force_fine.end());
  const std::array<f64, 7> before = momentum();
  rebuild_operator();  // (keeps the old fine cells fine, adds force_fine: the new composite)
  rebuild_sublattice();
  repartition_check(before);
  refined_.push_back({extra, 0});
}

std::array<f64, 7> Bubble::momentum() const {
  std::array<f64, 7> out{};
  for (i32 k = 0; k < C_.n; ++k) {
    f64 g[6];
    mv6(&C_.M[36 * size_t(k)], &v_[6 * size_t(k)], g);  // (p, angular momentum about X_k)
    const f64* X = &C_.X[3 * size_t(k)];
    for (int q = 0; q < 3; ++q) out[size_t(q)] += g[q];
    out[3] += g[3] + X[1] * g[2] - X[2] * g[1];
    out[4] += g[4] + X[2] * g[0] - X[0] * g[2];
    out[5] += g[5] + X[0] * g[1] - X[1] * g[0];
    for (int q = 0; q < 6; ++q) out[6] += 0.5 * v_[6 * size_t(k) + q] * g[q];
  }
  return out;
}

void Bubble::repartition_check(const std::array<f64, 7>& b) {
  const std::array<f64, 7> a = momentum();
  f64 dp = 0.0, pn = 1e-300;
  for (int q = 0; q < 6; ++q) {
    dp = std::max(dp, std::abs(a[size_t(q)] - b[size_t(q)]));
    pn = std::max(pn, std::abs(b[size_t(q)]));
  }
  rep_dp_ = std::max(rep_dp_, dp / pn);
  rep_dke_ = std::max(rep_dke_, a[6] - b[6]);
}

void Bubble::coarsen_quiet() {
  const Lattice& L = *L_;
  std::vector<u8> drop;
  for (Refined& rg : refined_) {
    if (rg.quiet < 0) continue;  // (holds cracks: fine for good)
    bool quiet = true, damaged = false;
    f64 phi = 0.0;
    for (i32 c : rg.cells) {
      if (L.dead[c]) continue;
      const i32 node = C_.node_of[c];
      if (node >= 0) {
        const f64* v = &v_[6 * size_t(node)];
        if (std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) >= opt_.sleep_velocity) quiet = false;
      }
      for (int a = 0; a < 3; ++a) {  // (a contact needs the fine level; damage coarsens with its
        if (L.nbr[a][c] < 0) continue;  // released load, rebuild_sublattice)
        if (L.is_cracked(a, c)) damaged = true;
      }
    }
    if (damaged) {
      rg.quiet = -1;
      continue;
    }
    if (quiet) {  // the demand of its bonds at the current state
      std::vector<f64> ut;
      total_displacement(ut);
      for (i32 c : rg.cells)
        for (int a = 0; a < 3; ++a) {
          const i32 j = L.nbr[a][c];
          if (j < 0 || L.dead[c] || L.dead[j]) continue;
          Vec6 g = bond_jump(L, a, c, ut.data(), opt_.corot);
          for (auto& x : g) x *= L.kscale * opt_.demand_inflation;
          const f64 d = L.dmg[a].empty() ? 0.0 : f64(L.dmg[a][c]);
          phi = std::max(phi, evaluate_law(L.bond(a, c), g, d, L.law.game).phi);
        }
      quiet = phi < opt_.coarsen_phi;
    }
    rg.quiet = quiet ? rg.quiet + 1 : 0;
    if (rg.quiet < opt_.coarsen_steps) continue;
    if (drop.empty()) drop.assign(size_t(L.n), 0);
    for (i32 c : rg.cells) drop[size_t(c)] = 1;
    rg.cells.clear();
    ++coarsenings_;
  }
  if (drop.empty()) return;
  refined_.erase(std::remove_if(refined_.begin(), refined_.end(), [](const Refined& r) { return r.cells.empty() && r.quiet >= 0; }),
                 refined_.end());
  auto& ff = opt_.comp.force_fine;
  ff.erase(std::remove_if(ff.begin(), ff.end(), [&](i32 c) { return drop[size_t(c)] != 0; }), ff.end());
  const std::array<f64, 7> before = momentum();
  rebuild_operator(&drop);
  rebuild_sublattice();
  repartition_check(before);
  event_fine_ = std::min(event_fine_, C_.n_fine);  // (events' cells coarsen too)
}

void Bubble::rebuild_sublattice() {
  // The baseline forces keep the bonds as they were at init: an old fine bond's (the released
  // loads of its ruptures stay released), a bond joining the fine level now (intact since init:
  // a coarse bond never ruptures) at the baseline; a bond leaving it (an intact one) takes its
  // baseline force out. A cell leaving it (coarsening) takes the loads its ruptures and damage
  // released along into the composite's load: the aggregate's increments are elastic about the
  // baseline, and without them it would stand as if its bonds were whole.
  Lattice& L = *L_;
  const SubLattice Sold = std::move(S_);
  const std::vector<f64> u0Fold = std::move(u0F_), fint0old = std::move(fint0_);
  std::vector<i32> fine;
  fine.reserve(size_t(C_.n_fine));
  for (i32 k = 0; k < C_.n; ++k)
    if (C_.level[k] == 0) fine.push_back(C_.cell[k]);
  S_ = extract_sublattice(L, fine);
  ++comp_gen_;
  const Lattice& F = S_.F;
  std::unordered_map<i32, i32> was, is;
  was.reserve(Sold.to_world.size() * 2);
  for (size_t k = 0; k < Sold.to_world.size(); ++k) was.emplace(Sold.to_world[k], static_cast<i32>(k));
  is.reserve(size_t(F.n) * 2);
  for (i32 k = 0; k < F.n; ++k) is.emplace(S_.to_world[k], k);
  std::vector<f64> fw(fint0old);  // old baseline forces, less the bonds leaving the fine level
  const Lattice& Fo = Sold.F;
  for (int a = 0; a < 3; ++a)
    for (i32 k = 0; k < Fo.n; ++k) {
      const i32 j = Fo.nbr[a][k];
      if (j < 0) continue;
      const bool ki = is.count(Sold.to_world[k]) > 0, ji = is.count(Sold.to_world[j]) > 0;
      if (ki == ji) continue;
      f64 fi[6], fj[6];
      bond_internal_forces(Fo, a, k, u0Fold.data(), opt_.corot, fi, fj);
      for (int q = 0; q < 6; ++q) (ki ? fw[6 * size_t(k) + q] : fw[6 * size_t(j) + q]) -= ki ? fi[q] : fj[q];
    }
  std::vector<f64> released;  // (world cells leaving the fine level)
  for (i32 k = 0; k < Fo.n; ++k) {
    const i32 c = Sold.to_world[k];
    if (Fo.anchored[k] || Fo.dead[k] || is.count(c) || L.dead[c]) continue;
    f64 rk[6];
    for (int q = 0; q < 6; ++q) rk[q] = fint0old[6 * size_t(k) + q];  // its baseline forces at init ...
    for (int a = 0; a < 3; ++a)  // ... less its bonds' now
      for (int side = 0; side < 2; ++side) {
        const i32 lo = side == 0 ? k : Fo.nbrm[a][k];
        if (lo < 0 || Fo.nbr[a][lo] < 0) continue;
        f64 fi[6], fj[6];
        bond_internal_forces(Fo, a, lo, u0Fold.data(), opt_.corot, fi, fj);
        for (int q = 0; q < 6; ++q) rk[q] -= side == 0 ? fi[q] : fj[q];
      }
    if (released.empty()) released.assign(6 * size_t(L.n), 0.0);
    for (int q = 0; q < 6; ++q) released[6 * size_t(c) + q] += rk[q];
  }
  if (!released.empty()) {
    std::vector<f64> rc(6 * size_t(C_.n));
    composite_restrict(L, C_, released.data(), rc.data());
    for (size_t q = 0; q < rc.size(); ++q) r_[q] += rc[q];
  }
  fnode_.assign(size_t(F.n), -1);
  u0F_.assign(6 * size_t(F.n), 0.0);
  fint0_.assign(6 * size_t(F.n), 0.0);
  for (i32 k = 0; k < F.n; ++k) {
    const i32 c = S_.to_world[k];
    if (!F.anchored[k] && !F.dead[k]) fnode_[size_t(k)] = C_.node_of[c];
    for (int q = 0; q < 6; ++q) u0F_[6 * size_t(k) + q] = u0_[6 * size_t(c) + q];
    const auto it = was.find(c);
    if (it != was.end())
      for (int q = 0; q < 6; ++q) fint0_[6 * size_t(k) + q] = fw[6 * size_t(it->second) + q];
  }
  for (int a = 0; a < 3; ++a)  // bonds joining the fine level: their baseline forces
    for (i32 k = 0; k < F.n; ++k) {
      const i32 j = F.nbr[a][k];
      if (j < 0) continue;
      if (was.count(S_.to_world[k]) && was.count(S_.to_world[j])) continue;  // (old: counted)
      f64 fi[6], fj[6];
      bond_internal_forces(F, a, k, u0F_.data(), opt_.corot, fi, fj);
      for (int q = 0; q < 6; ++q) {
        fint0_[6 * size_t(k) + q] += fi[q];
        fint0_[6 * size_t(j) + q] += fj[q];
      }
    }
  th_build_.clear();  // (frames and rotations follow the new sub-lattice)
  rot_.clear();
  rot_nodes_.clear();
}

void Bubble::detach(const CutSet& cut, StepStats* st) {
  const auto isl = detached_islands(*L_, cut, &conn_);
  if (!isl.empty()) remove_islands(isl, st);
}

void Bubble::remove_cells(const std::vector<i32>& cells) {
  std::vector<i32> live;
  for (i32 c : cells)
    if (c >= 0 && c < L_->n && !L_->dead[c]) live.push_back(c);
  if (live.empty()) return;
  std::sort(live.begin(), live.end());
  remove_islands({live}, nullptr, false);  // (the caller reported them)
}

void Bubble::apply_event(const EventMutation& ev) {
  constexpr f32 kEventFineDamage = 0.7f;  // (a blast's crater rim; its outer damage zone stays coarse)
  Lattice& L = *L_;
  auto ok = [&](i32 c) { return c >= 0 && c < L.n && !L.dead[c]; };
  // 1. the cells it touches fine: its neighbours, both ends of every bond it breaks or damages,
  //    and the survivors bonded to the cells it removes
  std::vector<i32> want;
  auto add = [&](i32 c) {
    if (ok(c) && !L.anchored[c]) want.push_back(c);
  };
  for (i32 c : ev.fine) add(c);
  for (const auto& [c, a] : ev.broken)
    if (ok(c)) {
      add(c);
      add(L.nbr[a][c]);
    }
  for (const auto& [c, a, d] : ev.damaged)  // (lightly damaged ones may stay in aggregates, as
    if (ok(c) && d >= kEventFineDamage) {    // the far field's committed damage does)
      add(c);
      add(L.nbr[a][c]);
    }
  for (i32 c : ev.removed)
    if (ok(c))
      for (int a = 0; a < 3; ++a) {
        add(L.nbr[a][c]);
        add(L.nbrm[a][c]);
      }
  std::sort(want.begin(), want.end());
  want.erase(std::unique(want.begin(), want.end()), want.end());
  std::vector<i32> coarse;
  for (i32 c : want) {
    const i32 k = C_.node_of[c];
    if (k >= 0 && C_.level[k] != 0) coarse.push_back(c);
  }
  const i32 fine_before = C_.n_fine;
  if (!coarse.empty()) {
    refine(coarse);  // (a quiet one coarsens again, coarsen_quiet)
    event_fine_ += C_.n_fine - fine_before;
  }
  if (std::getenv("SVX_NOMINATE_TRACE"))
    std::printf("      [event] step %lld: fine %zu, removed %zu, islands %zu, broken %zu, damaged %zu, forces %zu: "
                "%zu cells asked, %zu coarse; fine %d -> %d of %d nodes\n",
                static_cast<long long>(steps_), ev.fine.size(), ev.removed.size(), ev.islands.size(), ev.broken.size(),
                ev.damaged.size(), ev.forces.size(), want.size(), coarse.size(), fine_before, C_.n_fine, C_.n);
  // 2. the mutation. Every changed bond is fine at both ends now: the sub-lattice's forces lose
  //    it (fint0_ keeps its baseline part: its total force is released)
  Lattice& F = S_.F;
  std::unordered_map<i32, i32> sub;
  sub.reserve(want.size() * 2 + 16);
  for (i32 k = 0; k < F.n; ++k)
    if (std::binary_search(want.begin(), want.end(), S_.to_world[k])) sub.emplace(S_.to_world[k], k);
  auto sub_of = [&](i32 c) {
    const auto it = sub.find(c);
    return it == sub.end() ? -1 : it->second;
  };
  bool changed = false;
  for (const auto& [c, a, d] : ev.damaged) {
    if (!ok(c) || L.nbr[a][c] < 0) continue;
    L.dmg[a][c] = std::max(L.dmg[a][c], d);
    const i32 k = sub_of(c);
    if (k >= 0 && F.nbr[a][k] >= 0 && !F.dmg[a].empty()) F.dmg[a][k] = std::max(F.dmg[a][k], d);
    changed = true;
  }
  CutSet cut;
  for (const auto& [c, a] : ev.broken) {
    if (!ok(c) || L.nbr[a][c] < 0) continue;
    const i32 j = L.nbr[a][c];
    const i32 k = sub_of(c);
    if (k >= 0 && F.nbr[a][k] >= 0) F.break_bond(k, a);
    L.break_bond(c, a);
    cut.seeds.push_back(c);
    cut.seeds.push_back(j);
    changed = true;
  }
  std::vector<i32> gone;
  for (i32 c : ev.islands)
    if (ok(c)) gone.push_back(c);
  for (i32 c : ev.removed)
    if (ok(c)) {
      gone.push_back(c);
      if (L.support(c)) cut.supports_removed = true;
      for (int a = 0; a < 3; ++a) {
        if (L.nbr[a][c] >= 0) cut.seeds.push_back(L.nbr[a][c]);
        if (L.nbrm[a][c] >= 0) cut.seeds.push_back(L.nbrm[a][c]);
      }
    }
  std::sort(gone.begin(), gone.end());
  gone.erase(std::unique(gone.begin(), gone.end()), gone.end());
  if (!gone.empty()) {
    remove_islands({gone}, nullptr, false);
    changed = true;
  }
  // pieces the event cut off inside the bubble that the world had not (its own breaks since)
  if (!cut.seeds.empty()) {
    std::vector<i32>& sd = cut.seeds;
    sd.erase(std::remove_if(sd.begin(), sd.end(), [&](i32 c) { return !ok(c) || L.anchored[c]; }), sd.end());
    std::sort(sd.begin(), sd.end());
    sd.erase(std::unique(sd.begin(), sd.end()), sd.end());
    if (!sd.empty()) detach(cut, nullptr);
  }
  // 3. its one-step forces
  for (const auto& [c, f] : ev.forces)
    if (ok(c) && !L.anchored[c]) add_force(c, f.data());
  if (changed) {
    mg_dirty_ = true;
    dirty_why_ |= 2;
  }
  be_next_ = true;
  asleep_ = false;
  quiet_ = 0;
  refinements_ = 0;  // (its own nominations and growths)
  grows_ = 0;
  period_start_ = time_;
  event_step_ = steps_;
}

void Bubble::add_residual(const std::vector<f64>& r) {
  std::vector<f64> rc(6 * size_t(C_.n));
  composite_restrict(*L_, C_, r.data(), rc.data());
  for (size_t k = 0; k < rc.size(); ++k) r_[k] += rc[k];
  asleep_ = false;
  quiet_ = 0;
}

std::vector<std::pair<i32, int>> Bubble::take_cracked() {
  std::vector<std::pair<i32, int>> out;
  out.swap(cracked_);
  return out;
}

std::vector<std::pair<i32, int>> Bubble::take_broken() {
  std::vector<std::pair<i32, int>> out;
  out.swap(broken_);
  return out;
}

void Bubble::remove_islands(const std::vector<std::vector<i32>>& isl, StepStats* st, bool record) {
  Lattice& L = *L_;
  Lattice& F = S_.F;
  const size_t nf = 6 * size_t(L.n);
  std::vector<f64> ux(nf), vf(nf);
  composite_prolong(L, C_, x_.data(), ux.data());
  composite_prolong(L, C_, v_.data(), vf.data());
  std::vector<u8> gone(L.n, 0);
  for (const auto& cells : isl)
    for (i32 c : cells) gone[c] = 1;
  auto is_fine = [&](i32 c) {
    const i32 k = C_.node_of[c];
    return k >= 0 && C_.level[k] == 0;
  };
  // Released loads: every broken bond between a detached cell and a survivor stops carrying
  // its total force. Fine-fine (and fine-anchored) bonds release through the sub-lattice
  // (their baseline part stays in fint0_); every other bond is released here.
  std::vector<f64> release(nf, 0.0);
  std::vector<f64> ut(nf, 0.0);
  f64 fi[6], fj[6];
  for (const auto& cells : isl)
    for (i32 d : cells)
      for (int a = 0; a < 3; ++a)
        for (int side = 0; side < 2; ++side) {
          const i32 lo = side == 0 ? d : L.nbrm[a][d];
          if (lo < 0) continue;
          const i32 hi = L.nbr[a][lo];
          if (hi < 0) continue;
          const i32 s = side == 0 ? hi : lo;
          if (gone[s] || L.anchored[s]) continue;
          if (is_fine(d) && is_fine(s)) continue;
          for (i32 c : {lo, hi})
            for (int q = 0; q < 6; ++q) ut[6 * size_t(c) + q] = u0_[6 * size_t(c) + q] + ux[6 * size_t(c) + q];
          bond_internal_forces(L, a, lo, ut.data(), opt_.corot, fi, fj);
          const f64* fs = s == lo ? fi : fj;
          for (int q = 0; q < 6; ++q) release[6 * size_t(s) + q] += fs[q];
        }
  std::map<i32, i32> sub_of;
  for (i32 k = 0; k < F.n; ++k) sub_of[S_.to_world[k]] = k;
  for (const auto& cells : isl) {
    DetachedIsland ev;
    ev.cells = cells;
    ev.step = steps_;
    for (i32 c : cells) {
      const f64 mc = L.mass[c];
      ev.mass += mc;
      for (int q = 0; q < 3; ++q) {
        ev.com[q] += mc * (L.h * L.p[c][q] + u0_[6 * size_t(c) + q] + ux[6 * size_t(c) + q]);
        ev.v[q] += mc * vf[6 * size_t(c) + q];
        ev.w[q] += mc * vf[6 * size_t(c) + 3 + q];
      }
    }
    if (ev.mass > 0.0)
      for (int q = 0; q < 3; ++q) {
        ev.com[q] /= ev.mass;
        ev.v[q] /= ev.mass;
        ev.w[q] /= ev.mass;
      }
    for (i32 c : cells) {
      const auto it = sub_of.find(c);
      if (it != sub_of.end() && !F.dead[it->second]) F.remove_cell(it->second);
      L.remove_cell(c);
    }
    if (!record) continue;
    detached_ += static_cast<i64>(cells.size());
    if (st) {
      st->islands += 1;
      st->detached_cells += static_cast<i32>(cells.size());
    }
    islands_.push_back(std::move(ev));
  }
  rebuild_operator();
  std::vector<f64> rc(6 * size_t(C_.n));
  composite_restrict(L, C_, release.data(), rc.data());
  for (size_t k = 0; k < rc.size(); ++k) r_[k] += rc[k];
  mg_dirty_ = true;
  be_next_ = true;
}

// The operator for state x: frames, the rebuild tests, the preconditioner (and its time in st).
void Bubble::refresh_operator(const f64* x, StepStats& st) {
  Lattice& F = S_.F;
  if (opt_.corot) {
    std::vector<f64> uF;
    fine_state(x, uF);
    const bool framed = update_frames(F, uF.data(), opt_.frame_threshold);
    if (framed != mg_framed_) {
      mg_dirty_ = true;
      dirty_why_ |= 8;
    }
    if (framed && !mg_dirty_ && th_build_.size() == uF.size()) {
      f64 d = 0.0;
      if (opt_.rotate_operator) {
        // rotation across intact bonds since the build (what a per-node rotation cannot follow)
        for (int a = 0; a < 3; ++a)
          for (i32 k = 0; k < F.n; ++k) {
            const i32 j = F.nbr[a][k];
            if (j < 0 || F.dead[k] || F.dead[j] || F.is_cracked(a, k)) continue;
            for (int q = 3; q < 6; ++q)
              d = std::max(d, std::abs((uF[6 * size_t(k) + q] - th_build_[6 * size_t(k) + q]) -
                                       (uF[6 * size_t(j) + q] - th_build_[6 * size_t(j) + q])));
          }
      } else {
        for (size_t k = 0; k < uF.size(); k += 6)
          for (int q = 3; q < 6; ++q) d = std::max(d, std::abs(uF[k + q] - th_build_[k + q]));
      }
      if (d > opt_.frame_rebuild) {
        mg_dirty_ = true;
        dirty_why_ |= 16;
      }
    }
    if (mg_dirty_) th_build_ = uF;
    else if (opt_.rotate_operator && framed && th_build_.size() == uF.size()) update_rotation(uF);
  }
  if (mg_dirty_) {
    if (std::getenv("SVX_REBUILD_TRACE")) std::printf("      [rebuild] why %u nodes %d\n", unsigned(dirty_why_), C_.n);
    dirty_why_ = 0;
    const auto t0 = Clock::now();
    rebuild_preconditioner();
    st.rebuilt = true;
    st.ms_mg += ms_since(t0);
  }
}

void Bubble::prepare() {
  if (asleep_) return;
  StepStats st;
  refresh_operator(x_.data(), st);
  pending_ms_mg_ += st.ms_mg;
  pending_rebuilt_ = pending_rebuilt_ || st.rebuilt;
}

StepStats Bubble::step() {
  StepStats st;
  st.ms_mg = pending_ms_mg_;  // (a prepare() for this step)
  st.rebuilt = pending_rebuilt_;
  pending_ms_mg_ = 0.0;
  pending_rebuilt_ = false;
  const auto t_step = Clock::now();
  if (asleep_) {
    st.asleep = true;
    once_.clear();
    return st;
  }
  // (§B6.3) a step whose marginal bonds lie by the fine/coarse interface is solved again from
  // the state before it, with the fine region grown around them (the decision at the peak of a
  // passing load, not a step later), at most max_grows times
  struct Pre {
    std::vector<f64> x, v, vp, dp;
    bool be;
    f64 time;
    i64 steps;
  };
  auto take = [&] { return Pre{x_, v_, v_prev_, d_prev_, be_next_, time_, steps_}; };
  Pre pre = take();
  std::vector<i32> grow;
  while (!step_attempt(st, grow)) {
    x_ = std::move(pre.x);
    v_ = std::move(pre.v);
    v_prev_ = std::move(pre.vp);
    d_prev_ = std::move(pre.dp);
    be_next_ = pre.be;
    time_ = pre.time;
    steps_ = pre.steps;
    refine(grow);  // (the state before the step, re-partitioned)
    for (const auto& [c, f] : once_) apply_force(c, f.data());
    ++grows_;
    ++grows_total_;
    st.refined = true;
    grow.clear();
    pre = take();
  }
  once_.clear();
  st.ms_total = ms_since(t_step);
  if (std::getenv("SVX_BUBBLE_HASH")) {  // (diagnostics: the state's bits)
    u64 hx = 1469598103934665603ull, hv = hx;
    for (size_t k = 0; k < x_.size(); ++k) {
      u64 b;
      std::memcpy(&b, &x_[k], 8);
      hx = (hx ^ b) * 1099511628211ull;
      std::memcpy(&b, &v_[k], 8);
      hv = (hv ^ b) * 1099511628211ull;
    }
    std::printf("      [bubble] step %lld: nodes %d x %016llx v %016llx pcg %d ruptured %d residual %.17g\n",
                static_cast<long long>(steps_), C_.n, static_cast<unsigned long long>(hx),
                static_cast<unsigned long long>(hv), st.pcg, st.ruptured, st.residual);
  }
  return st;
}

bool Bubble::step_attempt(StepStats& st, std::vector<i32>& grow) {
  add_work(i64(C_.n) * kWorkStepFixed);  // (deterministic work accounting)
  Lattice& L = *L_;
  Lattice& F = S_.F;
  const size_t m = 6 * size_t(C_.n);
  const f64 dt = opt_.dt;
  auto refresh = [&](const f64* x) { refresh_operator(x, st); };
  refresh(x_.data());
  const bool be = be_next_ || !opt_.bdf2;
  st.be = be;
  const f64 c = be ? 1.0 / dt : 1.5 / dt;
  const f64 al = opt_.rayleigh_alpha;
  const f64 cm = c * c + al * c;
  const f64 ck = 1.0 + opt_.rayleigh_beta * c;
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
  std::vector<f64> R0(m);
  const f64 once = base_dt_ > 0.0 ? base_dt_ / dt : 1.0;  // (one-step forces: impulses over base_dt_)
  for (size_t k = 0; k < m; ++k) R0[k] = r_[k] + once * f_once_[k];
  mass_apply(C_, vhat.data(), R0.data(), c);
  if (!be) mass_apply(C_, e.data(), R0.data(), -cm);
  std::vector<f64> dx(m, 0.0), xt(m), N(m), Kc(m), R(m), delta(m);
  if (!be) dx = d_prev_;
  std::vector<f64> rt, ry;  // (rotate_operator scratch)
  auto op = [&](const f64* xin, f64* y) {
    if (opt_.assembled_operator) {
      if (!rot_nodes_.empty()) {  // T (A + ms M) T^T
        rt.resize(m);
        ry.resize(m);
        rotate_in(xin, rt.data());
        mg_.apply_fine_operator(rt.data(), ry.data());
        rotate_out(ry.data(), y);
        return;
      }
      mg_.apply_fine_operator(xin, y);  // (A + ms M), as assembled for the preconditioner
      return;
    }
    tangent_apply(xin, y);
    for (i32 r = 0; r < C_.n; ++r) {
      f64 t[6];
      mv6(&C_.M[36 * size_t(r)], xin + 6 * size_t(r), t);
      for (int q = 0; q < 6; ++q) y[6 * size_t(r) + q] += ms * t[q];
    }
  };
  f64 r0n = 0.0;
  const int iters = opt_.corot ? std::max(1, opt_.newton_iters) : 1;
  for (int it = 0; it <= iters; ++it) {
    for (size_t k = 0; k < m; ++k) xt[k] = x_[k] + dx[k];
    // a step starting at the baseline (a bubble's first; x = 0): the internal force increment
    // and the coarse couplings are exactly zero (skipping them is bitwise the same)
    bool at_base = it == 0 && steps_ == 0;
    for (size_t k = 0; k < m && at_base; ++k) at_base = xt[k] == 0.0;
    if (at_base) {
      std::fill(N.begin(), N.end(), 0.0);
      std::fill(Kc.begin(), Kc.end(), 0.0);
    } else {
      nonlinear_part(xt.data(), N.data());
      coarse_apply(xt.data(), Kc.data());
    }
    if (std::getenv("SVX_NAN_CHECK")) {
      auto bad = [&](const std::vector<f64>& v) {
        for (f64 q : v)
          if (!std::isfinite(q)) return true;
        return false;
      };
      if (bad(xt) || bad(N) || bad(Kc) || bad(R0) || bad(dx))
        std::printf("      [nan] step %lld it %d: xt %d N %d Kc %d R0 %d dx %d (x_ %d)\n", static_cast<long long>(steps_) + 1, it,
                    bad(xt), bad(N), bad(Kc), bad(R0), bad(dx), bad(x_));
    }
    for (size_t k = 0; k < m; ++k) R[k] = R0[k] - Kc[k] - N[k];
    mass_apply(C_, dx.data(), R.data(), -cm);
    for (i32 k = 0; k < C_.n; ++k) {
      const u8 fm = C_.fix[k];
      if (fm)
        for (int q = 0; q < 6; ++q)
          if ((fm >> q) & 1) R[6 * size_t(k) + q] = 0.0;
    }
    const f64 rn = norm2(R);
    if (!std::isfinite(rn)) {  // a corrupt state: stop here, keep nothing of this step
      failed_ = true;
      asleep_ = true;
      st.asleep = true;
      std::fill(dx.begin(), dx.end(), 0.0);
      break;
    }
    if (it == 0) r0n = rn;
    st.residual = r0n > 0.0 ? rn / r0n : 0.0;
    if (rn == 0.0 || (it > 0 && (rn <= opt_.newton_rtol * r0n || rn <= opt_.newton_atol * load_scale_)) || it == iters)
      break;
    if (it > 0) refresh(xt.data());
    for (size_t k = 0; k < m; ++k) R[k] /= ck;
    const auto t_solve = Clock::now();
    // (the first steps take the same tolerance: a looser early solve lets the monotone law
    // ratchet on unconverged states; replayed against a converged reference one loose first
    // step turned 6 ruptures into 1,155)
    const f64 atol = quiet_ >= opt_.sleep_steps - opt_.quiet_tight_steps ? opt_.lin_atol_quiet : opt_.lin_atol;
    const f64 rtol = opt_.lin_rtol;
    PcgStats ps;
    const bool project = opt_.projection_basis > 0 && opt_.assembled_operator;
    std::vector<f64> pt, pz;
    const LinOp rotated_mg = [&](const f64* r, f64* z) {  // T MG T^T
      pt.resize(m);
      pz.resize(m);
      rotate_in(r, pt.data());
      mg_.apply(pt.data(), pz.data());
      rotate_out(pz.data(), z);
    };
    auto solve = [&](const f64* b, f64* x, f64 rtol, f64 at) {
      return rot_nodes_.empty() ? pcg_solve_op(op, mg_, C_.n, b, x, rtol, opt_.lin_maxit, false, at)
                                : pcg_solve_op(op, rotated_mg, C_.n, b, x, rtol, opt_.lin_maxit, false, at);
    };
    if (!project) {
      ps = solve(R.data(), delta.data(), rtol, atol * load_scale_ / ck);
    } else {
      // x0 = V V^T b (the A-projection onto the previous solutions), r0 = b - W V^T b; the
      // stopping rule stays the unprojected one: ||r|| <= max(rtol ||b||, atol)
      const f64 stop = std::max(rtol * norm2(R), atol * load_scale_ / ck);
      std::vector<f64>& x0 = proj_x0_;
      std::vector<f64>& r0 = proj_r0_;
      x0.assign(m, 0.0);
      r0 = R;
      const size_t nb = proj_v_.size();
      f64 a[32] = {};
      for (size_t i = 0; i < nb; ++i) {  // (each coefficient summed in index order)
        const f64* v = proj_v_[i].data();
        f64 ai = 0.0;
        for (size_t k = 0; k < m; ++k) ai += v[k] * R[k];
        a[i] = ai;
      }
      for (size_t i = 0; i < nb; ++i) {
        const f64* v = proj_v_[i].data();
        const f64* w = proj_w_[i].data();
        const f64 ai = a[i];
        for (size_t k = 0; k < m; ++k) {
          x0[k] += ai * v[k];
          r0[k] -= ai * w[k];
        }
      }
      ps = solve(r0.data(), delta.data(), 0.0, stop);
      // the new direction: A-orthogonalized against the basis, normalized (one operator apply)
      std::vector<f64> e = delta, Ae(m);
      op(e.data(), Ae.data());
      for (size_t i = 0; i < proj_v_.size(); ++i) {
        const f64* v = proj_v_[i].data();
        const f64* w = proj_w_[i].data();
        f64 c = 0.0;
        for (size_t k = 0; k < m; ++k) c += v[k] * Ae[k];
        for (size_t k = 0; k < m; ++k) {
          e[k] -= c * v[k];
          Ae[k] -= c * w[k];
        }
      }
      f64 eAe = 0.0;
      for (size_t k = 0; k < m; ++k) eAe += e[k] * Ae[k];
      if (eAe > 0.0 && std::isfinite(eAe)) {
        const f64 inv = 1.0 / std::sqrt(eAe);
        for (size_t k = 0; k < m; ++k) {
          e[k] *= inv;
          Ae[k] *= inv;
        }
        if (static_cast<int>(proj_v_.size()) < opt_.projection_basis) {
          proj_v_.push_back(std::move(e));
          proj_w_.push_back(std::move(Ae));
        } else {  // replace the oldest
          proj_v_[proj_next_] = std::move(e);
          proj_w_[proj_next_] = std::move(Ae);
          proj_next_ = (proj_next_ + 1) % proj_v_.size();
        }
      }
      for (size_t k = 0; k < m; ++k) delta[k] += x0[k];
    }
    st.ms_solve += ms_since(t_solve);
    st.pcg += ps.iters;
    if (std::getenv("SVX_NAN_CHECK")) {
      bool bd = false;
      for (f64 q : delta) bd = bd || !std::isfinite(q);
      f64 dn = 0.0;
      for (f64 q : delta) dn = std::max(dn, std::abs(q));
      std::printf("      [pcg] step %lld it %d: iters %d rel %.3e converged %d, max|delta| %.3e%s, rn %.3e\n",
                  static_cast<long long>(steps_) + 1, it, ps.iters, ps.rel_res, ps.converged ? 1 : 0, dn,
                  bd ? " NON-FINITE" : "", rn);
    }
    if (ps.iters >= opt_.mg_rebuild_iters) {
      mg_dirty_ = true;
      dirty_why_ |= 32;
    }
    const f64 ts = trust_scale(delta, opt_.max_translation_increment * L.h, opt_.max_rotation_increment);
    for (size_t k = 0; k < m; ++k) dx[k] += ts * delta[k];
    ++st.newton;
  }
  if (failed_) return true;
  for (size_t k = 0; k < m; ++k) {
    x_[k] += dx[k];
    const f64 vn = c * (dx[k] + e[k]);
    v_prev_[k] = v_[k];
    v_[k] = vn;
    d_prev_[k] = dx[k];
  }
  for (i32 k = 0; k < C_.n; ++k) {
    const u8 fm = C_.fix[k];
    if (fm)
      for (int q = 0; q < 6; ++q)
        if ((fm >> q) & 1) x_[6 * size_t(k) + q] = v_[6 * size_t(k) + q] = 0.0;
  }
  be_next_ = false;
  std::fill(f_once_.begin(), f_once_.end(), 0.0);
  time_ += dt;
  ++steps_;

  // law on the fine bonds (the fine level decides), mirrored into the world lattice
  if (opt_.damage) {
    const auto t_law = Clock::now();
    std::vector<f64> uF;
    fine_state(x_.data(), uF);
    // (§B6.3) marginal bonds by the fine/coarse interface decide nothing: the step is solved
    // again with the fine region grown around them (step())
    if (opt_.marginal_eps >= 0.0 && grows_ < opt_.max_grows && C_.n_fine < opt_.max_fine_cells + event_fine_ &&
        C_.n > C_.n_fine) {
      if (near_gen_ != comp_gen_) {
        mark_interface();
        near_gen_ = comp_gen_;
      }
      add_work(i64(F.n) * kWorkLawSweep);
      std::vector<i32> around;
      i64 held = 0;
      for (int a = 0; a < 3; ++a)
        for (i32 k = 0; k < F.n; ++k) {
          const i32 j = F.nbr[a][k];
          if (j < 0 || F.dead[k] || F.dead[j] || (!near_[size_t(k)] && !near_[size_t(j)]) || F.is_cracked(a, k)) continue;
          Vec6 g = bond_jump(F, a, k, uF.data(), opt_.corot);
          for (auto& v : g) v *= F.kscale * opt_.demand_inflation;
          const f64 d0 = F.dmg[a].empty() ? 0.0 : f64(F.dmg[a][k]);
          if (evaluate_law(F.bond(a, k), g, d0, F.law.game).phi < 1.0 - opt_.marginal_eps) continue;
          ++held;
          around.push_back(S_.to_world[k]);
          around.push_back(S_.to_world[j]);
        }
      if (held > 0) {
        // (uncapped: capping it by the nomination budget kept a tower's hinge coarse - it stood)
        grow = aggregates_near(around, i64(1) << 40);
        if (!grow.empty()) {
          if (std::getenv("SVX_NOMINATE_TRACE"))
            std::printf("      [marginal] step %lld: %lld bonds by the interface, %zu cells become fine: solved again\n",
                        static_cast<long long>(steps_), static_cast<long long>(held), grow.size());
          held_ += held;
          st.ms_law += ms_since(t_law);
          return false;
        }
      }
    }
    LawSweep sw = sweep_law(F, uF.data(), F.dmg, opt_.corot, F.kscale, opt_.demand_inflation);
    st.max_damage = sw.max_damage;
    for (int a = 0; a < 3; ++a)
      for (i32 k = 0; k < F.n; ++k)
        if (F.nbr[a][k] >= 0) L.dmg[a][S_.to_world[k]] = F.dmg[a][k];
    for (auto& cnd : sw.candidates) cnd.key = bond_key(L, cnd.axis, S_.to_world[cnd.cell]);
    st.ms_law += ms_since(t_law);
    if (!sw.candidates.empty()) {
      const auto t_topo = Clock::now();
      sort_candidates(sw.candidates);
      const size_t nb = std::min(sw.candidates.size(), static_cast<size_t>(std::max(0, opt_.max_breaks)));
      st.carried = static_cast<i32>(sw.candidates.size() - nb);
      CutSet cut;
      for (size_t k = 0; k < nb; ++k) {
        const RuptureCandidate& cd = sw.candidates[k];
        const i32 j = F.nbr[cd.axis][cd.cell];
        if (j < 0) continue;
        const i32 wi = S_.to_world[cd.cell], wj = S_.to_world[j];
        ruptured_.push_back(cd.key);
        ++st.ruptured;
        if (F.contact.enabled && !cd.separate && !cd.opening) {
          // failed closed (shear, crushing): a crack, not a gap — the faces still touch and
          // carry compression (unilateral contact); the pieces stay connected until the crack
          // opens for good. A bond failing open leaves a gap and breaks below.
          F.crack_bond(cd.cell, cd.axis);
          L.crack_bond(wi, cd.axis);
          cracked_.emplace_back(wi, static_cast<int>(cd.axis));
          contact_seeds_.push_back(wi);
          contact_seeds_.push_back(wj);
          continue;
        }
        F.break_bond(cd.cell, cd.axis);
        L.break_bond(wi, cd.axis);
        broken_.emplace_back(wi, static_cast<int>(cd.axis));
        cut.seeds.push_back(wi);
        cut.seeds.push_back(wj);
      }
      if (st.ruptured > 0) {
        // fint0_ keeps the ruptured bonds' baseline forces: their loss (or a crack's opening)
        // is the released load
        ruptures_since_build_ += st.ruptured;
        if (ruptures_since_build_ >= opt_.rebuild_ruptures) {
          mg_dirty_ = true;
          dirty_why_ |= 2;
        }
        be_next_ = true;
        if (!cut.seeds.empty()) detach(cut, &st);
      }
      st.ms_topo += ms_since(t_topo);
    }
  }
  // (§B6.3) the held bonds' region grows (their aggregates become fine) before they are decided
  // nominate / decide: aggregates whose bound nears onset become fine (the law decides there)
  // (a long-running bubble - a structure whose load path keeps changing as it gives way - gets
  // its refinements and growths anew every second of its time, whatever its step)
  if (time_ - period_start_ >= 1.0 - 1e-9) {
    period_start_ = time_;
    refinements_ = 0;
    grows_ = 0;
  }
  if (opt_.damage && opt_.nominate && opt_.nominate_every > 0 && steps_ % opt_.nominate_every == 0 &&
      refinements_ < opt_.max_refinements && C_.n_fine < opt_.max_fine_cells + event_fine_) {
    const auto t_nom = Clock::now();
    const std::vector<i32> cells = nominate();
    if (!cells.empty()) {
      refine(cells);
      ++refinements_;
      ++refinements_total_;
      st.refined = true;
    }
    st.ms_topo += ms_since(t_nom);
  }
  if (opt_.coarsen && !refined_.empty() && opt_.nominate_every > 0 && steps_ % opt_.nominate_every == 0) {
    const auto t_c = Clock::now();
    coarsen_quiet();
    st.ms_topo += ms_since(t_c);
  }
  if (!contact_seeds_.empty() && opt_.contact_check_every > 0 && steps_ % opt_.contact_check_every == 0) {
    const auto t_c = Clock::now();
    release_contact_pieces(&st);
    st.ms_topo += ms_since(t_c);
  }
  f64 vmax = 0.0;
  for (i32 k = 0; k < C_.n; ++k) {
    const f64* vk = &v_[6 * size_t(k)];
    vmax = std::max(vmax, std::sqrt(vk[0] * vk[0] + vk[1] * vk[1] + vk[2] * vk[2]));
  }
  st.max_speed = vmax;
  f64 ke = 0.0;
  for (i32 k = 0; k < C_.n; ++k) {
    f64 t[6];
    mv6(&C_.M[36 * size_t(k)], &v_[6 * size_t(k)], t);
    for (int q = 0; q < 6; ++q) ke += 0.5 * v_[6 * size_t(k) + q] * t[q];
  }
  st.kinetic = ke;
  const bool quiet = st.ruptured == 0 && st.detached_cells == 0 && vmax < opt_.sleep_velocity;
  quiet_ = quiet ? quiet_ + 1 : 0;
  if (quiet_ >= opt_.sleep_steps || steps_ - event_step_ >= opt_.max_steps) {
    asleep_ = true;
    st.asleep = true;
  }
  return true;
}

void Bubble::release_contact_pieces(StepStats* st) {
  Lattice& L = *L_;
  std::vector<i32>& sd = contact_seeds_;
  sd.erase(std::remove_if(sd.begin(), sd.end(), [&](i32 c) { return c < 0 || c >= L.n || L.dead[c] || L.anchored[c]; }),
           sd.end());
  std::sort(sd.begin(), sd.end());
  sd.erase(std::unique(sd.begin(), sd.end()), sd.end());
  if (sd.empty()) return;
  const i64 v0 = conn_.visited;
  const std::vector<std::vector<i32>> pieces = contact_held_pieces(L, sd, &conn_);
  add_work(conn_.visited - v0);  // (the searches' cells)
  std::vector<std::vector<i32>> go;
  std::vector<i32> keep;  // seeds of the pieces still resting on their cracks
  for (const auto& p : pieces) {
    f64 m = 0.0, mv[3] = {0.0, 0.0, 0.0};
    for (const i32 c : p) {
      const i32 k = C_.node_of[c];
      if (k < 0) continue;
      const f64* V = &v_[6 * size_t(k)];
      const f64* o = &C_.off[3 * size_t(c)];
      const f64 vc[3] = {V[0] + V[4] * o[2] - V[5] * o[1], V[1] + V[5] * o[0] - V[3] * o[2],
                         V[2] + V[3] * o[1] - V[4] * o[0]};  // (the node's motion at the cell)
      m += L.mass[c];
      for (int q = 0; q < 3; ++q) mv[q] += L.mass[c] * vc[q];
    }
    const f64 speed = m > 0.0 ? std::sqrt(mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2]) / m : 0.0;
    if (speed >= opt_.contact_release_speed) {
      go.push_back(p);
    } else {
      for (const i32 c : p)
        if (std::binary_search(sd.begin(), sd.end(), c)) keep.push_back(c);
    }
  }
  sd.swap(keep);
  if (!go.empty()) remove_islands(go, st, true);
}

void Bubble::set_time_step(f64 dt) {
  if (!(dt > 0.0) || dt == opt_.dt) return;
  opt_.dt = dt;
  be_next_ = true;  // (BDF2's history is at the old step)
  mg_dirty_ = true;  // (the mass shift)
  dirty_why_ |= 64;
}

Bubble::SettleResult Bubble::settle(int max_iters, int max_pcg) {
  SettleResult res;
  Lattice& F = S_.F;
  const size_t m = 6 * size_t(C_.n);
  // static composite preconditioner (no mass shift): coarse couplings + current fine tangent
  std::vector<f64> uF;
  fine_state(x_.data(), uF);
  if (opt_.corot) update_frames(F, uF.data(), opt_.frame_threshold);
  Multigrid mg;
  bool built = false;
  const DamageField dc = F.dmg;  // committed damage: the settle's damage is a trial
  std::vector<f64> N(m), Kc(m), R(m), dx(m);
  auto op = [&](const f64* xin, f64* y) {
    if (opt_.assembled_operator && built) mg.apply_fine_operator(xin, y);
    else tangent_apply(xin, y);
  };
  f64 r0 = 0.0;
  for (int it = 0; it <= max_iters; ++it) {
    // trial damage at the current state (non-accumulating from the committed values)
    fine_state(x_.data(), uF);
    const LawSweep sw = sweep_law(F, uF.data(), dc, opt_.corot, F.kscale, opt_.demand_inflation);
    res.max_damage = sw.max_damage;
    res.candidate_cells.clear();
    for (const RuptureCandidate& cd : sw.candidates) res.candidate_cells.push_back(S_.to_world[cd.cell]);
    nonlinear_part(x_.data(), N.data());
    coarse_apply(x_.data(), Kc.data());
    for (size_t k = 0; k < m; ++k) R[k] = r_[k] - Kc[k] - N[k];
    for (i32 k = 0; k < C_.n; ++k) {
      const u8 fm = C_.fix[k];
      if (fm)
        for (int q = 0; q < 6; ++q)
          if ((fm >> q) & 1) R[6 * size_t(k) + q] = 0.0;
    }
    const f64 rn = norm2(R);
    if (!std::isfinite(rn)) {  // a corrupt state: nothing to settle
      failed_ = true;
      break;
    }
    if (it == 0) r0 = std::max(rn, 1e-30);
    res.iters = it;
    if (rn <= 1e-3 * load_scale_ || (it > 0 && rn <= 1e-4 * r0)) {
      res.converged = true;
      break;
    }
    if (it == max_iters || (!sw.candidates.empty() && it >= 3)) break;
    if (opt_.corot && it > 0) update_frames(F, uF.data(), opt_.frame_threshold);
    if (!built) {  // only when a solve is actually needed (a sleeping bubble is usually at rest)
      Bsr6 A = C_.A.without_copies();
      add_sublattice_bonds(C_, S_, A);
      MGOptions mo = opt_.mg;
      mo.greedy = true;
      mo.cycle_gamma = 1;
      mg.build_assembled(A, {}, C_.X, C_.blk, C_.fix, mo);
      built = true;
    }
    const int maxit = max_pcg > 0 ? std::max(1, std::min(opt_.lin_maxit, max_pcg - res.pcg)) : opt_.lin_maxit;
    const PcgStats ps = pcg_solve_op(op, mg, C_.n, R.data(), dx.data(), 1e-3, maxit, false, 1e-4 * load_scale_);
    res.pcg += ps.iters;
    // trust region (near a mechanism the static operator is nearly singular: an unlimited
    // increment would throw the state far away)
    const f64 ts = trust_scale(dx, 0.25 * L_->h, 0.2);
    for (size_t k = 0; k < m; ++k) x_[k] += ts * dx[k];
    if (max_pcg > 0 && res.pcg >= max_pcg) break;  // (unconverged: the caller's budget)
  }
  std::fill(v_.begin(), v_.end(), 0.0);
  std::fill(v_prev_.begin(), v_prev_.end(), 0.0);
  std::fill(d_prev_.begin(), d_prev_.end(), 0.0);
  be_next_ = true;
  return res;
}

void Bubble::total_displacement(std::vector<f64>& u) const {
  const Lattice& L = *L_;
  u.assign(6 * size_t(L.n), 0.0);
  composite_prolong(L, C_, x_.data(), u.data());
  for (i32 i = 0; i < L.n; ++i) {
    if (L.dead[i] || L.anchored[i]) {
      for (int q = 0; q < 6; ++q) u[6 * size_t(i) + q] = 0.0;
      continue;
    }
    for (int q = 0; q < 6; ++q) u[6 * size_t(i) + q] += u0_[6 * size_t(i) + q];
  }
}

std::vector<DetachedIsland> Bubble::take_islands() {
  std::vector<DetachedIsland> out;
  out.swap(islands_);
  return out;
}

}  // namespace svx
