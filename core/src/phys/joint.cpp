// structvox — the joint solver (phys/joint.hpp): each joint's rows, solved with the contacts.
//
// Rows act between the ends' anchors (linear) and their frames (angular). Blocks are solved
// whole (a point: 3 rows; a hinge's two square turns; a slider's two square moves; a lock of
// rotation: 3 rows), a motor and a limit as single rows, clamped. The impulses accumulate over a
// substep and warm-start the next; position error is removed on the pseudo velocities afterwards
// (split impulse: the correction adds no energy). Joints are solved one after another in id order
// after each sweep of the contacts: the same on every thread count.
#include <algorithm>
#include <array>
#include <cmath>

#include "svx/base/dmath.hpp"
#include "svx/base/rotation.hpp"
#include "svx/phys/rigid.hpp"

namespace svx {

namespace {

M3 skew(const V3& r) {
  M3 S;
  S.m = {0.0, -r.z, r.y, r.z, 0.0, -r.x, -r.y, r.x, 0.0};
  return S;
}

M3 plus(const M3& A, const M3& B) {
  M3 R;
  for (int k = 0; k < 9; ++k) R.m[size_t(k)] = A.m[size_t(k)] + B.m[size_t(k)];
  return R;
}

M3 diag3(f64 s) {
  M3 R;
  R.m = {s, 0.0, 0.0, 0.0, s, 0.0, 0.0, 0.0, s};
  return R;
}

// The rotational part of a point's effective mass: (r x .)^T I (r x .).
M3 arm_block(const V3& r, const M3& I) {
  const M3 S = skew(r);
  return transpose(S) * I * S;
}

M3 inverted(const M3& K) {
  M3 R;
  return inverse(K, R) ? R : M3{};
}

void squares(const V3& n, V3& t1, V3& t2) {
  t1 = normalized(std::abs(n.x) < 0.57 ? cross(n, V3{1, 0, 0}) : cross(n, V3{0, 1, 0}));
  t2 = cross(n, t1);
}

// The effective mass of a row: a linear direction d at the arms, and an angular direction u.
f64 row_mass(f64 ma, f64 mb, const M3& Ia, const M3& Ib, const V3& ra, const V3& rb, const V3& d, const V3& u) {
  const V3 ad = cross(ra, d) + u, bd = cross(rb, d) + u;
  return dot(d, d) * (ma + mb) + dot(ad, Ia * ad) + dot(bd, Ib * bd);
}

// The inverse of the symmetric 2x2 [[a, b], [b, d]] (row-major into out).
void inv2(f64 a, f64 b, f64 d, f64 out[4]) {
  const f64 det = a * d - b * b;
  if (!(std::abs(det) > 1e-300)) {
    out[0] = out[1] = out[2] = out[3] = 0.0;
    return;
  }
  const f64 id = 1.0 / det;
  out[0] = d * id;
  out[1] = -b * id;
  out[2] = -b * id;
  out[3] = a * id;
}

V3 capped(const V3& v, f64 cap) {
  const f64 n = norm(v);
  return n > cap ? v * (cap / n) : v;
}

}  // namespace

void JointDrive::goal(f64 t, f64* x, f64* rate) const {
  if (kind != Kind::Oscillate || !(period > 0.0)) {
    *x = target;
    *rate = 0.0;
    return;
  }
  // (eased: from target to target2 and back, a cosine)
  constexpr f64 kTau = 6.283185307179586;
  const f64 a = kTau * (t + phase) / period;
  *x = target + (target2 - target) * 0.5 * (1.0 - dm::cos(a));
  *rate = (target2 - target) * 0.5 * dm::sin(a) * kTau / period;
}

void RigidWorld::prepare_joints(f64 dt, const std::vector<M3>& Iw) {
  jprep_.assign(joints.size(), JointPrep{});
  joint_dt_ = dt;
  const f64 beta = par.joint_baumgarte, slop = par.joint_slop, cap = par.max_correction, warm = par.joint_warm;
  auto index_of = [&](i64 id) -> i32 {
    const auto it = std::lower_bound(bodies.begin(), bodies.end(), id, [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
    return (it != bodies.end() && (*it)->id == id) ? static_cast<i32>(it - bodies.begin()) : -1;
  };
  // (beyond slop, a position error is taken out at beta per substep, no faster than cap)
  auto pull = [&](f64 e, f64 s) -> f64 {
    const f64 m = std::max(0.0, std::abs(e) - s);
    return std::clamp(-beta * (e < 0.0 ? -m : m) / dt, -cap, cap);
  };
  for (size_t k = 0; k < joints.size(); ++k) {
    Joint& j = joints[k];
    JointPrep& P = jprep_[k];
    if (j.broken) continue;
    const i32 ia = j.a.body != 0 ? index_of(j.a.body) : -1, ib = j.b.body != 0 ? index_of(j.b.body) : -1;
    if ((j.a.body != 0 && ia < 0) || (j.b.body != 0 && ib < 0)) continue;
    // the ends: anchors, frames (their lattice's rotation in the world), velocities
    Quat Qa = j.a.q, Qb = j.b.q;
    V3 xa, xb;
    if (ia >= 0) {
      const Body& A = *bodies[size_t(ia)];
      P.pa = A.x + rotate(A.q, j.a.p);
      Qa = A.q * j.a.q;
      xa = A.x;
      if (!A.asleep) {
        P.ia = ia;
        P.ma = A.inv_mass;
        P.Ia = Iw[size_t(ia)];
      }
    } else {
      P.pa = j.a.p;
    }
    if (ib >= 0) {
      const Body& B = *bodies[size_t(ib)];
      P.pb = B.x + rotate(B.q, j.b.p);
      Qb = B.q * j.b.q;
      xb = B.x;
      if (!B.asleep) {
        P.ib = ib;
        P.mb = B.inv_mass;
        P.Ib = Iw[size_t(ib)];
      }
    } else {
      P.pb = j.b.p;
    }
    if (P.ia < 0 && P.ib < 0) continue;  // (both immovable now: nothing to solve)
    P.on = true;
    P.ra = P.ia >= 0 ? P.pa - xa : V3{};
    P.rb = P.ib >= 0 ? P.pb - xb : V3{};
    P.ax = normalized(rotate(Qa, j.a.axis));
    squares(P.ax, P.t1, P.t2);
    const M3 Isum = plus(P.Ia, P.Ib);
    const V3 d = P.pb - P.pa;
    const JointType t = j.type;
    if (t == JointType::Ball || t == JointType::Hinge || t == JointType::Fixed) {
      P.Kp = inverted(plus(diag3(P.ma + P.mb), plus(arm_block(P.ra, P.Ia), arm_block(P.rb, P.Ib))));
      const f64 e = norm(d);
      P.ep = e > 0.0 ? d * (pull(e, slop) / e) : V3{};
    }
    if (t == JointType::Hinge) {
      const f64 a = dot(P.t1, Isum * P.t1), b = dot(P.t1, Isum * P.t2), c = dot(P.t2, Isum * P.t2);
      inv2(a, b, c, P.K2);
      // (b's axis tilted from a's: the turn that tilts it back)
      const V3 tilt = cross(P.ax, normalized(rotate(Qb, j.b.axis)));
      P.e2[0] = pull(dot(tilt, P.t1), 0.0);
      P.e2[1] = pull(dot(tilt, P.t2), 0.0);
      // b's reference turned about the axis from a's
      const V3 ra_ = rotate(Qa, j.a.ref), rb_ = rotate(Qb, j.b.ref);
      P.value = angle_about(ra_ - P.ax * dot(ra_, P.ax), rb_ - P.ax * dot(rb_, P.ax), P.ax);
      const f64 k = dot(P.ax, Isum * P.ax);
      P.kax = k > 0.0 ? 1.0 / k : 0.0;
    }
    if (t == JointType::Slider) {
      // (the linear rows act at b's anchor: a's arm reaches it)
      if (P.ia >= 0) P.ra = P.pb - xa;
      const f64 a = row_mass(P.ma, P.mb, P.Ia, P.Ib, P.ra, P.rb, P.t1, V3{});
      const f64 c = row_mass(P.ma, P.mb, P.Ia, P.Ib, P.ra, P.rb, P.t2, V3{});
      const V3 a1 = cross(P.ra, P.t1), a2 = cross(P.ra, P.t2), b1 = cross(P.rb, P.t1), b2 = cross(P.rb, P.t2);
      const f64 b = dot(P.t1, P.t2) * (P.ma + P.mb) + dot(a1, P.Ia * a2) + dot(b1, P.Ib * b2);
      inv2(a, b, c, P.K2);
      P.e2[0] = pull(dot(d, P.t1), slop);
      P.e2[1] = pull(dot(d, P.t2), slop);
      P.value = dot(d, P.ax);
      const f64 k = row_mass(P.ma, P.mb, P.Ia, P.Ib, P.ra, P.rb, P.ax, V3{});
      P.kax = k > 0.0 ? 1.0 / k : 0.0;
    }
    if (t == JointType::Slider || t == JointType::Fixed) {
      P.Ka = inverted(Isum);
      // (b turned from where it is held: the turn back)
      const V3 phi = rotation_vector(Qb * conj(Qa * j.rel));
      P.ea = capped(phi * (-beta / dt), cap);
    }
    if (t == JointType::Distance) {
      const f64 L = norm(d);
      P.n = L > 1e-9 ? d * (1.0 / L) : V3{0, 0, 1};
      P.value = L;
      const f64 k = row_mass(P.ma, P.mb, P.Ia, P.Ib, P.ra, P.rb, P.n, V3{});
      P.kax = k > 0.0 ? 1.0 / k : 0.0;
      if (j.stiffness > 0.0) {
        // (a spring beyond its range: soft, gamma = 1 / (dt (c + dt k)); the stretch is taken
        // out at the rate the spring and damper give, not by position correction)
        const f64 c = std::max(0.0, j.damping), denom = c + dt * j.stiffness;
        P.gamma = 1.0 / (dt * denom);
        const f64 over = L > j.max_length ? L - j.max_length : L < j.min_length ? L - j.min_length : 0.0;
        P.soft = (j.stiffness / denom) * over;
        P.ksoft = 1.0 / (k + P.gamma);
      } else {
        if (L > j.max_length + slop) P.eax = pull(L - j.max_length, slop);
        else if (L < j.min_length - slop) P.eax = pull(L - j.min_length, slop);
      }
    }
    // the limit that bears (the nearer bound), and how far past it the ends are
    if (j.limited && (t == JointType::Hinge || t == JointType::Slider)) {
      if (j.lower >= j.upper) {
        P.lim = 2;
        P.eax = pull(P.value - j.lower, t == JointType::Slider ? slop : 0.0);
      } else {
        P.lim = P.value - j.lower < j.upper - P.value ? -1 : 1;
        if (P.value < j.lower) P.eax = pull(P.value - j.lower, t == JointType::Slider ? slop : 0.0);
        if (P.value > j.upper) P.eax = pull(P.value - j.upper, t == JointType::Slider ? slop : 0.0);
      }
    }
    // the drive's goal now
    const bool driven = (t == JointType::Hinge || t == JointType::Slider) && j.drive.kind != JointDrive::Kind::Off && j.drive.max > 0.0;
    if (driven) j.drive.goal(time, &P.goal, &P.goal_rate);
    // warm start (the rows that do not act now start from nothing)
    if (!driven) j.motor = 0.0;
    if (P.lim == 0 && t != JointType::Distance) j.limit = 0.0;
    j.lin *= warm;
    j.ang *= warm;
    j.axial *= warm;
    j.limit *= warm;
    j.motor *= warm;
    // (apply them as the rows do)
    const f64 lim_drive = j.limit + j.motor;
    V3 J = j.lin, L = j.ang;
    if (t == JointType::Hinge) L += P.ax * lim_drive;
    if (t == JointType::Slider) J += P.ax * lim_drive;
    if (t == JointType::Distance) J += P.n * (j.axial + j.limit);
    auto apply = [&](const V3& Jl, const V3& Ja) {
      if (P.ib >= 0) {
        Body& B = *bodies[size_t(P.ib)];
        B.v += Jl * P.mb;
        B.w += P.Ib * (cross(P.rb, Jl) + Ja);
      }
      if (P.ia >= 0) {
        Body& A = *bodies[size_t(P.ia)];
        A.v -= Jl * P.ma;
        A.w -= P.Ia * (cross(P.ra, Jl) + Ja);
      }
    };
    apply(J, L);
    P.J = J;
    P.L = L;
  }
}

void RigidWorld::solve_joints() {
  for (size_t k = 0; k < joints.size(); ++k) {
    JointPrep& P = jprep_[k];
    if (!P.on) continue;
    Joint& j = joints[k];
    Body* A = P.ia >= 0 ? bodies[size_t(P.ia)].get() : nullptr;
    Body* B = P.ib >= 0 ? bodies[size_t(P.ib)].get() : nullptr;
    auto va = [&](const V3& X) { return A ? A->v + cross(A->w, X - A->x) : V3{}; };
    auto vb = [&](const V3& X) { return B ? B->v + cross(B->w, X - B->x) : V3{}; };
    auto wa = [&]() { return A ? A->w : V3{}; };
    auto wb = [&]() { return B ? B->w : V3{}; };
    // (linear impulse Jl at the arms and angular Ja: on b, the opposite on a)
    auto apply = [&](const V3& Jl, const V3& Ja) {
      if (B) {
        B->v += Jl * P.mb;
        B->w += P.Ib * (cross(P.rb, Jl) + Ja);
      }
      if (A) {
        A->v -= Jl * P.ma;
        A->w -= P.Ia * (cross(P.ra, Jl) + Ja);
      }
      P.J += Jl;
      P.L += Ja;
    };
    const JointType t = j.type;
    // the drive, then the limit: the motion along (hinge: about) the axis
    const bool driven = j.drive.kind != JointDrive::Kind::Off && j.drive.max > 0.0;
    if ((t == JointType::Hinge || t == JointType::Slider) && (driven || P.lim != 0)) {
      const bool turn = t == JointType::Hinge;
      auto rate = [&]() { return turn ? dot(P.ax, wb() - wa()) : dot(P.ax, vb(P.pb) - va(P.pb)); };
      auto push = [&](f64 l) {
        if (turn) apply(V3{}, P.ax * l);
        else apply(P.ax * l, V3{});
      };
      if (driven) {
        // the rate it asks for: its speed, or towards its goal (a hinge the short way round)
        const f64 top = std::abs(j.drive.speed);
        f64 want = j.drive.speed;
        if (j.drive.kind != JointDrive::Kind::Speed) {
          f64 e = P.goal - P.value;
          if (turn) e = std::remainder(e, 6.283185307179586);
          want = std::clamp(P.goal_rate + j.drive.stiffness * e, -top, top);
        }
        const f64 lim = j.drive.max * joint_dt_;
        const f64 nd = std::clamp(j.motor + P.kax * (want - rate()), -lim, lim);
        push(nd - j.motor);
        j.motor = nd;
      }
      if (P.lim == 2) {
        const f64 l = -P.kax * rate();
        j.limit += l;
        push(l);
      } else if (P.lim != 0) {
        // (speculative: it may close on the bound within this substep, not pass it)
        const f64 bound = P.lim < 0 ? j.lower : j.upper;
        const f64 target = (bound - P.value) / joint_dt_;
        const f64 l = P.kax * (target - rate());
        const f64 nl = P.lim < 0 ? std::max(0.0, j.limit + l) : std::min(0.0, j.limit + l);
        push(nl - j.limit);
        j.limit = nl;
      }
    }
    switch (t) {
      case JointType::Ball:
      case JointType::Hinge:
      case JointType::Fixed: {
        const V3 l = P.Kp * (va(P.pa) - vb(P.pb));
        j.lin += l;
        apply(l, V3{});
        break;
      }
      case JointType::Slider: {
        const V3 dv = vb(P.pb) - va(P.pb);
        const f64 r1 = -dot(P.t1, dv), r2 = -dot(P.t2, dv);
        const V3 l = P.t1 * (P.K2[0] * r1 + P.K2[1] * r2) + P.t2 * (P.K2[2] * r1 + P.K2[3] * r2);
        j.lin += l;
        apply(l, V3{});
        break;
      }
      case JointType::Distance: {
        const f64 rate = dot(P.n, vb(P.pb) - va(P.pa));
        if (j.stiffness > 0.0 && (P.value > j.max_length || P.value < j.min_length)) {
          // (stretched beyond its range: a spring; a rope pulls only, a rod pushes back too)
          const f64 l = -P.ksoft * (rate + P.soft + P.gamma * j.axial);
          const f64 nl = j.min_length < j.max_length ? (P.value > j.max_length ? std::min(0.0, j.axial + l) : std::max(0.0, j.axial + l)) : j.axial + l;
          apply(P.n * (nl - j.axial), V3{});
          j.axial = nl;
        } else if (j.min_length >= j.max_length) {
          const f64 l = -P.kax * rate;
          j.axial += l;
          apply(P.n * l, V3{});
        } else {
          // (a rope pulls: its length may grow to the maximum within this substep, no further)
          const f64 l = P.kax * ((j.max_length - P.value) / joint_dt_ - rate);
          const f64 nl = std::min(0.0, j.axial + l);
          apply(P.n * (nl - j.axial), V3{});
          j.axial = nl;
          if (j.min_length > 0.0) {
            const f64 rate2 = dot(P.n, vb(P.pb) - va(P.pa));
            const f64 l2 = P.kax * ((j.min_length - P.value) / joint_dt_ - rate2);
            const f64 nl2 = std::max(0.0, j.limit + l2);
            apply(P.n * (nl2 - j.limit), V3{});
            j.limit = nl2;
          }
        }
        break;
      }
    }
    if (t == JointType::Hinge) {
      const V3 dw = wb() - wa();
      const f64 r1 = -dot(P.t1, dw), r2 = -dot(P.t2, dw);
      const V3 l = P.t1 * (P.K2[0] * r1 + P.K2[1] * r2) + P.t2 * (P.K2[2] * r1 + P.K2[3] * r2);
      j.ang += l;
      apply(V3{}, l);
    }
    if (t == JointType::Slider || t == JointType::Fixed) {
      const V3 l = P.Ka * (wa() - wb());
      j.ang += l;
      apply(V3{}, l);
    }
  }
}

void RigidWorld::solve_joints_position(std::vector<V3>& pv, std::vector<V3>& pw) {
  for (size_t k = 0; k < joints.size(); ++k) {
    JointPrep& P = jprep_[k];
    if (!P.on) continue;
    const Joint& j = joints[k];
    const Body* A = P.ia >= 0 ? bodies[size_t(P.ia)].get() : nullptr;
    const Body* B = P.ib >= 0 ? bodies[size_t(P.ib)].get() : nullptr;
    // (pseudo velocities: the immovable ends have none)
    auto va = [&](const V3& X) { return A ? pv[size_t(P.ia)] + cross(pw[size_t(P.ia)], X - A->x) : V3{}; };
    auto vb = [&](const V3& X) { return B ? pv[size_t(P.ib)] + cross(pw[size_t(P.ib)], X - B->x) : V3{}; };
    auto wa = [&]() { return A ? pw[size_t(P.ia)] : V3{}; };
    auto wb = [&]() { return B ? pw[size_t(P.ib)] : V3{}; };
    auto apply = [&](const V3& Jl, const V3& Ja) {
      if (B) {
        pv[size_t(P.ib)] += Jl * P.mb;
        pw[size_t(P.ib)] += P.Ib * (cross(P.rb, Jl) + Ja);
      }
      if (A) {
        pv[size_t(P.ia)] -= Jl * P.ma;
        pw[size_t(P.ia)] -= P.Ia * (cross(P.ra, Jl) + Ja);
      }
    };
    const JointType t = j.type;
    if (P.eax != 0.0) {
      // (a limit passed, a rope over its length: pushed back only)
      if (t == JointType::Distance) {
        const f64 rate = dot(P.n, vb(P.pb) - va(P.pa));
        f64 l = P.kax * (P.eax - rate);
        if (j.min_length < j.max_length) l = P.eax < 0.0 ? std::min(0.0, l) : std::max(0.0, l);
        apply(P.n * l, V3{});
      } else {
        const bool turn = t == JointType::Hinge;
        const f64 rate = turn ? dot(P.ax, wb() - wa()) : dot(P.ax, vb(P.pb) - va(P.pb));
        f64 l = P.kax * (P.eax - rate);
        if (P.lim != 2) l = P.eax < 0.0 ? std::min(0.0, l) : std::max(0.0, l);
        if (turn) apply(V3{}, P.ax * l);
        else apply(P.ax * l, V3{});
      }
    }
    if (t == JointType::Ball || t == JointType::Hinge || t == JointType::Fixed) apply(P.Kp * (P.ep - (vb(P.pb) - va(P.pa))), V3{});
    if (t == JointType::Slider) {
      const V3 dv = vb(P.pb) - va(P.pb);
      const f64 r1 = P.e2[0] - dot(P.t1, dv), r2 = P.e2[1] - dot(P.t2, dv);
      apply(P.t1 * (P.K2[0] * r1 + P.K2[1] * r2) + P.t2 * (P.K2[2] * r1 + P.K2[3] * r2), V3{});
    }
    if (t == JointType::Hinge) {
      const V3 dw = wb() - wa();
      const f64 r1 = P.e2[0] - dot(P.t1, dw), r2 = P.e2[1] - dot(P.t2, dw);
      apply(V3{}, P.t1 * (P.K2[0] * r1 + P.K2[1] * r2) + P.t2 * (P.K2[2] * r1 + P.K2[3] * r2));
    }
    if (t == JointType::Slider || t == JointType::Fixed) apply(V3{}, P.Ka * (P.ea - (wb() - wa())));
  }
}

void RigidWorld::finish_joints(f64 dt) {
  for (size_t k = 0; k < joints.size(); ++k) {
    const JointPrep& P = jprep_[k];
    Joint& j = joints[k];
    if (!P.on) continue;  // (its bodies asleep: it carries what it carried when they fell asleep)
    j.force = P.J * (1.0 / dt);
    // (the turn it gives b about its anchor)
    j.torque = P.L * (1.0 / dt);
    j.value = P.value;
    if ((j.break_force > 0.0 && norm(j.force) > j.break_force) || (j.break_torque > 0.0 && norm(j.torque) > j.break_torque)) j.broken = true;
    // (a hinge turned past its capacity: a plastic hinge torn through, a door off its hinge)
    if (j.type == JointType::Hinge && j.break_angle > 0.0 && std::abs(P.value) > j.break_angle) j.broken = true;
  }
}

void RigidWorld::joint_partner_speeds(std::vector<f64>& partner) const {
  for (size_t k = 0; k < joints.size(); ++k) {
    const JointPrep& P = jprep_[k];
    if (!P.on) continue;
    auto speed = [&](i32 i) {
      if (i < 0) return 0.0;
      const Body& b = *bodies[size_t(i)];
      return norm(b.v_pre) + b.radius * norm(b.w_pre);
    };
    const f64 sa = speed(P.ia), sb = speed(P.ib);
    if (P.ia >= 0) partner[size_t(P.ia)] = std::max(partner[size_t(P.ia)], sb);
    if (P.ib >= 0) partner[size_t(P.ib)] = std::max(partner[size_t(P.ib)], sa);
  }
}

bool RigidWorld::driving(const Joint& j, const JointPrep& P) {
  if (!P.on || j.drive.kind == JointDrive::Kind::Off || !(j.drive.max > 0.0)) return false;
  if (j.type != JointType::Hinge && j.type != JointType::Slider) return false;
  switch (j.drive.kind) {
    case JointDrive::Kind::Speed:
      return j.drive.speed != 0.0;
    case JointDrive::Kind::Oscillate:
      return true;
    default: {
      f64 e = P.goal - P.value;
      if (j.type == JointType::Hinge) e = std::remainder(e, 6.283185307179586);
      return std::abs(e) > 1e-3;
    }
  }
}

void RigidWorld::wake_jointed() {
  auto index_of = [&](i64 id) -> i32 {
    const auto it = std::lower_bound(bodies.begin(), bodies.end(), id, [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
    return (it != bodies.end() && (*it)->id == id) ? static_cast<i32>(it - bodies.begin()) : -1;
  };
  for (const Joint& j : joints) {
    if (j.broken) continue;
    const i32 ia = j.a.body != 0 ? index_of(j.a.body) : -1, ib = j.b.body != 0 ? index_of(j.b.body) : -1;
    auto moving = [&](i32 i) {
      if (i < 0) return false;
      const Body& b = *bodies[size_t(i)];
      return !b.asleep && norm(b.v) + b.radius * norm(b.w) > 2.0 * sleep_speed_;
    };
    if (ia >= 0 && bodies[size_t(ia)]->asleep && moving(ib)) wake(*bodies[size_t(ia)]);
    if (ib >= 0 && bodies[size_t(ib)]->asleep && moving(ia)) wake(*bodies[size_t(ib)]);
  }
}

std::vector<std::array<i32, 2>> RigidWorld::joint_bodies() const {
  // (by id: the body list may have changed since the joints were prepared - a fracture)
  std::vector<std::array<i32, 2>> out(joints.size(), {-1, -1});
  for (size_t k = 0; k < joints.size(); ++k) {
    const Joint& j = joints[k];
    if (j.broken) continue;
    for (int e = 0; e < 2; ++e) {
      const i64 id = e ? j.b.body : j.a.body;
      if (id == 0) continue;
      const auto it = std::lower_bound(bodies.begin(), bodies.end(), id, [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
      if (it != bodies.end() && (*it)->id == id) out[k][size_t(e)] = static_cast<i32>(it - bodies.begin());
    }
  }
  return out;
}

std::vector<u8> RigidWorld::machine_parts() const {
  std::vector<u8> out(bodies.size(), 0);
  const std::vector<std::array<i32, 2>> ends = joint_bodies();
  for (size_t k = 0; k < joints.size() && k < jprep_.size(); ++k) {
    if (!driving(joints[k], jprep_[k])) continue;
    for (const i32 i : ends[k])
      if (i >= 0) out[size_t(i)] = 1;
  }
  return out;
}

std::vector<u8> RigidWorld::hanging(const std::vector<u8>& held) const {
  std::vector<u8> out(bodies.size(), 0);
  const std::vector<std::array<i32, 2>> ends = joint_bodies();
  for (int pass = 0; pass < 16; ++pass) {
    bool more = false;
    for (size_t k = 0; k < joints.size(); ++k) {
      const i32 ia = ends[k][0], ib = ends[k][1];
      if (joints[k].broken || (ia < 0 && ib < 0)) continue;
      // (an end held by what does not move, by what rests, or by what hangs itself)
      auto holds = [&](i32 i) { return i < 0 || bodies[size_t(i)]->asleep || held[size_t(i)] || out[size_t(i)]; };
      if (ib >= 0 && !out[size_t(ib)] && holds(ia)) {
        out[size_t(ib)] = 1;
        more = true;
      }
      if (ia >= 0 && !out[size_t(ia)] && holds(ib)) {
        out[size_t(ia)] = 1;
        more = true;
      }
    }
    if (!more) break;
  }
  return out;
}

void RigidWorld::joint_stillness() {
  auto index_of = [&](i64 id) -> i32 {
    const auto it = std::lower_bound(bodies.begin(), bodies.end(), id, [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
    return (it != bodies.end() && (*it)->id == id) ? static_cast<i32>(it - bodies.begin()) : -1;
  };
  std::vector<std::pair<i32, i32>> pairs;
  for (size_t k = 0; k < joints.size(); ++k) {
    const Joint& j = joints[k];
    if (j.broken) continue;
    const i32 ia = j.a.body != 0 ? index_of(j.a.body) : -1, ib = j.b.body != 0 ? index_of(j.b.body) : -1;
    // (a drive at work keeps its ends awake: running, or short of its target)
    if (k < jprep_.size() && driving(j, jprep_[k])) {
      if (ia >= 0) bodies[size_t(ia)]->still = 0;
      if (ib >= 0) bodies[size_t(ib)]->still = 0;
    }
    if (ia >= 0 && ib >= 0 && !bodies[size_t(ia)]->asleep && !bodies[size_t(ib)]->asleep) pairs.push_back({ia, ib});
  }
  // (the least still of a chain holds the rest)
  for (int pass = 0; pass < 16; ++pass) {
    bool more = false;
    for (const auto& [a, b] : pairs) {
      Body& A = *bodies[size_t(a)];
      Body& B = *bodies[size_t(b)];
      const i32 m = std::min(A.still, B.still);
      if (A.still != m || B.still != m) {
        A.still = B.still = m;
        more = true;
      }
    }
    if (!more) break;
  }
}

}  // namespace svx
