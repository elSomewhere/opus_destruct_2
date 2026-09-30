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
  const f64 warm = par.joint_warm;
  Target& t = targets[k];
  TargetPrep& P = tprep_[k];
  const TargetDrive& D = t.drive;
  const auto it = std::lower_bound(bodies.begin(), bodies.end(), t.body, [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
  const i32 ib = (it != bodies.end() && (*it)->id == t.body) ? static_cast<i32>(it - bodies.begin()) : -1;
  if (ib < 0 || (only && !(*only)[size_t(ib)])) return;
  const Body& B = *bodies[size_t(ib)];
  const f64 ks = std::max(0.0, D.stiffness), c = std::max(0.0, D.damping);
  const f64 denom = c + dt * ks;
  if (!D.on || B.asleep || !(B.inv_mass > 0.0) || !(denom > 0.0)) {
    t.imp = V3{};
    t.applied = V3{};
    return;
  }
  P.ib = ib;
  P.mb = B.inv_mass;
  P.Ib = Iw[size_t(ib)];
  P.gamma = 1.0 / (dt * denom);
  const f64 pull = ks / denom;  // (a rate per unit of error)
  P.cap = D.max > 0.0 ? D.max * dt : 0.0;
  if (t.kind == Target::Kind::Point) {
    P.r = rotate(B.q, t.local);
    const V3 C = B.x + P.r - D.pos;
    P.vel = D.vel;
    for (int a = 0; a < 3; ++a) {
      if (!D.axes[a]) continue;
      V3 e;
      e[a] = 1.0;
      const V3 re = cross(P.r, e);
      const f64 K = P.mb + dot(re, P.Ib * re);
      P.axis[P.rows] = e;
      P.k[P.rows] = 1.0 / (K + P.gamma);
      P.bias[P.rows] = pull * C[a];
      ++P.rows;
    }
  } else {
    // how far the body is turned past its target (world); tilting only: its up axis onto the
    // target's, about the two directions square to it
    V3 e;
    if (D.tilt_only) {
      const V3 u = normalized(rotate(B.q, D.up)), tu = normalized(rotate(D.rot, D.up));
      const V3 cr = cross(tu, u);
      const f64 sn = norm(cr);
      if (sn > 1e-12) e = cr * (dm::atan2(sn, dot(tu, u)) / sn);
      squares(u, P.axis[0], P.axis[1]);
      P.rows = 2;
    } else {
      e = rotation_vector(B.q * conj(D.rot));
      P.axis[0] = V3{1, 0, 0};
      P.axis[1] = V3{0, 1, 0};
      P.axis[2] = V3{0, 0, 1};
      P.rows = 3;
    }
    for (i32 r = 0; r < P.rows; ++r) {
      const f64 K = dot(P.axis[r], P.Ib * P.axis[r]);
      P.k[r] = K > 0.0 ? 1.0 / (K + P.gamma) : 0.0;
      P.bias[r] = pull * dot(e, P.axis[r]);
    }
  }
  P.on = P.rows > 0;
  // warm start: last substep's impulse along the rows it has now
  V3 imp;
  for (i32 r = 0; r < P.rows; ++r) imp += P.axis[r] * (dot(t.imp, P.axis[r]) * warm);
  if (P.cap > 0.0) imp = capped(imp, P.cap);
  t.imp = imp;
  Body& Bm = *bodies[size_t(ib)];
  if (t.kind == Target::Kind::Point) {
    Bm.v += imp * P.mb;
    Bm.w += P.Ib * cross(P.r, imp);
  } else {
    Bm.w += P.Ib * imp;
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
  V3 imp = t.imp;
  for (i32 r = 0; r < P.rows; ++r) {
    const V3& n = P.axis[r];
    const f64 jv = point ? dot(n, B.v + cross(B.w, P.r) - P.vel) : dot(n, B.w);
    const f64 lam = dot(imp, n);
    const f64 d = -P.k[r] * (jv + P.bias[r] + P.gamma * lam);
    push(n * d);
    imp += n * d;
  }
  if (P.cap > 0.0) {
    const V3 c = capped(imp, P.cap);
    push(c - imp);
    imp = c;
  }
  t.imp = imp;
}

void RigidWorld::finish_targets(f64 dt) {
  for (size_t k = 0; k < targets.size(); ++k) finish_target(k, dt);
}

void RigidWorld::finish_target(size_t k, f64 dt) {
  if (tprep_[k].on) targets[k].applied = targets[k].imp * (1.0 / dt);
}

}  // namespace svx
