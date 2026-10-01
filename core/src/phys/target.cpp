// structvox — targets (phys/target.hpp): a body's point pulled towards a world point, or the body
// turned towards a world rotation, by a spring and a damper of limited force. Soft rows (implicit:
// stable at any stiffness): gamma = 1 / (dt (c + dt k)) softens a row's mass, and the spring's
// pull k / (c + dt k) x error is the rate the row drives out; the accumulated impulse is capped at
// the target's strength. Warm-started; solved after the joints in every iteration, in id order.
#include <algorithm>
#include <cmath>

#include "svx/base/dmath.hpp"
#include "svx/base/rotation.hpp"
#include "svx/phys/rigid.hpp"

namespace svx {

namespace {

void squares(const V3& n, V3& t1, V3& t2) {
  t1 = normalized(std::abs(n.x) < 0.57 ? cross(n, V3{1, 0, 0}) : cross(n, V3{0, 1, 0}));
  t2 = cross(n, t1);
}

V3 capped(const V3& v, f64 cap) {
  const f64 n = norm(v);
  return n > cap ? v * (cap / n) : v;
}

}  // namespace

void RigidWorld::prepare_targets(f64 dt, const std::vector<M3>& Iw, const std::vector<u8>* only) {
  tprep_.assign(targets.size(), TargetPrep{});
  for (size_t k = 0; k < targets.size(); ++k) prepare_target(k, dt, Iw, only);
}

void RigidWorld::prepare_target(size_t k, f64 dt, const std::vector<M3>& Iw, const std::vector<u8>* only) {
  Target& t = targets[k];
  TargetPrep& P = tprep_[k];
  const TargetDrive& D = t.drive;
  const auto it = std::lower_bound(bodies.begin(), bodies.end(), t.body, [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
  const i32 ib = (it != bodies.end() && (*it)->id == t.body) ? static_cast<i32>(it - bodies.begin()) : -1;
  if (ib < 0 || (only && !(*only)[size_t(ib)])) return;
  const Body& B = *bodies[size_t(ib)];
  const f64 ks = std::max(0.0, D.stiffness), c = std::max(0.0, D.damping);
  if (!D.on || B.asleep || !(B.inv_mass > 0.0) || !(ks > 0.0 || c > 0.0)) {
    t.imp = t.imp_d = V3{};
    t.applied = V3{};
    return;
  }
  P.ib = ib;
  P.mb = B.inv_mass;
  P.Ib = Iw[size_t(ib)];
  // the spring (implicit: soft, stable at any stiffness) and the damper, as rows of their own
  // (a spring or damper so weak its softness is not finite acts on nothing: off, never Inf x 0)
  P.gs = ks > 0.0 ? 1.0 / (dt * dt * ks) : 0.0;
  P.gd = c > 0.0 ? 1.0 / (dt * c) : 0.0;
  P.spring = ks > 0.0 && std::isfinite(P.gs);
  P.damper = c > 0.0 && std::isfinite(P.gd);
  if (!P.spring) P.gs = 0.0;
  if (!P.damper) P.gd = 0.0;
  P.cap = D.max > 0.0 ? D.max * dt : 0.0;
  auto row = [&](f64 K, f64 err) {
    P.ks[P.rows] = P.spring && K + P.gs > 0.0 ? 1.0 / (K + P.gs) : 0.0;
    P.kd[P.rows] = P.damper && K + P.gd > 0.0 ? 1.0 / (K + P.gd) : 0.0;
    P.bias[P.rows] = P.spring ? err / dt : 0.0;
    ++P.rows;
  };
  if (t.kind == Target::Kind::Point) {
    P.r = rotate(B.q, t.local);
    const V3 C = B.x + P.r - D.pos;
    P.vel = D.vel;
    for (int a = 0; a < 3; ++a) {
      if (!D.axes[a]) continue;
      V3 e;
      e[a] = 1.0;
      const V3 re = cross(P.r, e);
      P.axis[P.rows] = e;
      row(P.mb + dot(re, P.Ib * re), C[a]);
    }
  } else {
    // how far the body is turned past its target (world); tilting only: its up axis onto the
    // target's, about the two directions square to it
    V3 e;
    V3 ax[3];
    i32 n = 3;
    if (D.tilt_only) {
      const V3 u = normalized(rotate(B.q, D.up)), tu = normalized(rotate(D.rot, D.up));
      const V3 cr = cross(tu, u);
      const f64 sn = norm(cr);
      if (sn > 1e-12) e = cr * (dm::atan2(sn, dot(tu, u)) / sn);
      squares(u, ax[0], ax[1]);
      n = 2;
    } else {
      e = rotation_vector(B.q * conj(D.rot));
      ax[0] = V3{1, 0, 0};
      ax[1] = V3{0, 1, 0};
      ax[2] = V3{0, 0, 1};
    }
    for (i32 r = 0; r < n; ++r) {
      P.axis[P.rows] = ax[r];
      row(dot(ax[r], P.Ib * ax[r]), dot(e, ax[r]));
    }
  }
  P.on = P.rows > 0;
  // warm start: last substep's impulses along the rows it has now (a link's: scaled to this step
  // - a fine step's, or a substep's)
  f64 warm = par.joint_warm;
  if (B.link) {
    warm *= t.step > 0.0 ? dt / t.step : 1.0;
    t.step = dt;
  }
  V3 imp, imp_d;
  for (i32 r = 0; r < P.rows; ++r) {
    if (P.spring) imp += P.axis[r] * (dot(t.imp, P.axis[r]) * warm);
    if (P.damper) imp_d += P.axis[r] * (dot(t.imp_d, P.axis[r]) * warm);
  }
  if (P.cap > 0.0) {
    imp = capped(imp, P.cap);
    imp_d = capped(imp_d, P.cap);
  }
  t.imp = imp;
  t.imp_d = imp_d;
  const V3 J = imp + imp_d;
  Body& Bm = *bodies[size_t(ib)];
  if (t.kind == Target::Kind::Point) {
    Bm.v += J * P.mb;
    Bm.w += P.Ib * cross(P.r, J);
  } else {
    Bm.w += P.Ib * J;
  }
}

void RigidWorld::solve_targets() {
  for (size_t k = 0; k < targets.size(); ++k) solve_target(k);
}

void RigidWorld::solve_target(size_t k) {
  const TargetPrep& P = tprep_[k];
  if (!P.on) return;
  Target& t = targets[k];
  Body& B = *bodies[size_t(P.ib)];
  const bool point = t.kind == Target::Kind::Point;
  auto push = [&](const V3& J) {
    if (point) {
      B.v += J * P.mb;
      B.w += P.Ib * cross(P.r, J);
    } else {
      B.w += P.Ib * J;
    }
  };
  // (the point's velocity relative to the target's; the body's spin)
  auto rate = [&](const V3& n) { return point ? dot(n, B.v + cross(B.w, P.r) - P.vel) : dot(n, B.w); };
  // the spring, then the damper, each within its most
  for (int which = 0; which < 2; ++which) {
    if (which == 0 ? !P.spring : !P.damper) continue;
    V3& acc = which == 0 ? t.imp : t.imp_d;
    const f64 g = which == 0 ? P.gs : P.gd;
    V3 imp = acc;
    for (i32 r = 0; r < P.rows; ++r) {
      const V3& n = P.axis[r];
      const f64 lam = dot(imp, n);
      const f64 d = -(which == 0 ? P.ks[r] : P.kd[r]) * (rate(n) + (which == 0 ? P.bias[r] : 0.0) + g * lam);
      push(n * d);
      imp += n * d;
    }
    if (P.cap > 0.0) {
      const V3 c = capped(imp, P.cap);
      push(c - imp);
      imp = c;
    }
    acc = imp;
  }
}

void RigidWorld::finish_targets(f64 dt) {
  for (size_t k = 0; k < targets.size(); ++k) finish_target(k, dt);
}

void RigidWorld::finish_target(size_t k, f64 dt) {
  if (tprep_[k].on) targets[k].applied = targets[k].imp * (1.0 / dt);
}

}  // namespace svx
