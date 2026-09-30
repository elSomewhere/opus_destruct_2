// structvox — the joint solver (phys/joint.hpp): each joint's rows, solved with the contacts.
//
// Rows act between the ends' anchors (linear) and their frames (angular). Blocks are solved
// whole (a point: 3 rows; a hinge's two square turns; a slider's two square moves; a lock of
// rotation: 3 rows), a motor and a limit as single rows, clamped. The impulses accumulate over a
// substep and warm-start the next (a link's joint: its point rows all of them -
// RigidParams::link_warm - and scaled to the step's length); position error is removed on the
// pseudo velocities afterwards (split impulse: the correction adds no energy). Joints are solved
// one after another in id order after each sweep of the contacts: the same on every thread count.
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

// The rotation whose columns are the right-handed orthonormal basis (x, axis x x, axis): a joint
// end's frame from its axis and reference direction.
Quat frame_of(const V3& axis, const V3& ref) {
  const V3 z = normalized(axis);
  const V3 x = normalized(ref - z * dot(ref, z));
  const V3 y = cross(z, x);
  const f64 m00 = x.x, m10 = x.y, m20 = x.z, m01 = y.x, m11 = y.y, m21 = y.z, m02 = z.x, m12 = z.y, m22 = z.z;
  const f64 tr = m00 + m11 + m22;
  Quat q;
  if (tr > 0.0) {
    const f64 s = std::sqrt(tr + 1.0) * 2.0;
    q = Quat{(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s};
  } else if (m00 > m11 && m00 > m22) {
    const f64 s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
    q = Quat{0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
  } else if (m11 > m22) {
    const f64 s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
    q = Quat{(m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s};
  } else {
    const f64 s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
    q = Quat{(m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s};
  }
  return qnormalized(q);
}

// A cone and a twist limit bear (their rows act) within this of their bounds (rad): a joint
// swinging fast towards its bound is met there, not past it.
constexpr f64 kLimitNear = 0.25;
// A supple joint (an articulation's) past a limit is put back this far a step at most (rad): as
// the XPBD bodies its characters were made with are.
constexpr f64 kSuppleStep = 0.025;
constexpr f64 kPi = 3.141592653589793;

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

void RigidWorld::prepare_joints(f64 dt, const std::vector<M3>& Iw, const std::vector<u8>* only) {
  jprep_.assign(joints.size(), JointPrep{});
  joint_dt_ = dt;
  for (size_t k = 0; k < joints.size(); ++k) prepare_joint(k, dt, Iw, only);
}

void RigidWorld::prepare_joint(size_t k, f64 dt, const std::vector<M3>& Iw, const std::vector<u8>* only) {

  const f64 beta = par.joint_baumgarte, slop = par.joint_slop, cap = par.max_correction;
  auto index_of = [&](i64 id) -> i32 {
    const auto it = std::lower_bound(bodies.begin(), bodies.end(), id, [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
    return (it != bodies.end() && (*it)->id == id) ? static_cast<i32>(it - bodies.begin()) : -1;
  };
  // (beyond slop, a position error is taken out at beta per substep, no faster than cap)
  auto pull = [&](f64 e, f64 s) -> f64 {
    const f64 m = std::max(0.0, std::abs(e) - s);
    return std::clamp(-beta * (e < 0.0 ? -m : m) / dt, -cap, cap);
  };
  // (a supple joint's limit: all of it, up to kSuppleStep a step)
  auto pull_limit = [&](const Joint& jt, f64 e, f64 s) -> f64 {
    if (!jt.supple) return pull(e, s);
    const f64 m = std::min(kSuppleStep, std::max(0.0, std::abs(e) - s));
    return -(e < 0.0 ? -m : m) / dt;
  };

  // (the room a limit's speculative row leaves: past a supple joint's bound, it turns back at most
  // kSuppleStep a step)
  auto room_of = [&](const Joint& jt, f64 r, int side) -> f64 {
    if (!jt.supple) return r;
    return side > 0 ? std::max(r, -kSuppleStep) : std::min(r, kSuppleStep);
  };

  Joint& j = joints[k];
  JointPrep& P = jprep_[k];
  if (j.broken) return;
  const i32 ia = j.a.body != 0 ? index_of(j.a.body) : -1, ib = j.b.body != 0 ? index_of(j.b.body) : -1;
  if ((j.a.body != 0 && ia < 0) || (j.b.body != 0 && ib < 0)) return;
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
  if (P.ia < 0 && P.ib < 0) return;  // (both immovable now: nothing to solve)
  // (of the bodies asked for: the fine ones, or the rest)
  if (only && !((P.ia >= 0 && (*only)[size_t(P.ia)]) || (P.ib >= 0 && (*only)[size_t(P.ib)]))) return;
  P.on = true;
  // (a link's joint: its point rows - what carries a chain's weight - from link_warm of their
  // last step's impulses, the rest - its lock rows, limits, drives, muscle - from joint_warm's
  // share, all scaled to this step: they are of a fine step, or of a substep, when its
  // articulation went from one to the other)
  f64 warm = par.joint_warm, warm_point = par.joint_warm;
  if ((ia >= 0 && bodies[size_t(ia)]->link) || (ib >= 0 && bodies[size_t(ib)]->link)) {
    const f64 scale = j.step > 0.0 ? dt / j.step : 1.0;
    warm = par.joint_warm * scale;
    warm_point = par.link_warm * scale;
    j.step = dt;
  }
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
  // the limit that bears (the nearer bound), and how far past it the ends are; latched shut, a
  // hinge is held where it was made
  if (t == JointType::Hinge && j.latched) {
    P.lim = 2;
    P.eax = pull(P.value, 0.0);
  } else if (j.limited && (t == JointType::Hinge || t == JointType::Slider)) {
    if (j.lower >= j.upper) {
      P.lim = 2;
      P.eax = pull(P.value - j.lower, t == JointType::Slider ? slop : 0.0);
    } else {
      P.lim = P.value - j.lower < j.upper - P.value ? -1 : 1;
      if (P.value < j.lower) P.eax = pull_limit(j, P.value - j.lower, t == JointType::Slider ? slop : 0.0);
      if (P.value > j.upper) P.eax = pull_limit(j, P.value - j.upper, t == JointType::Slider ? slop : 0.0);
    }
  }
  // (ball) the cone and the twist limit, near their bounds
  if (t == JointType::Ball && (j.swing_limited || j.twist_limited)) {
    const V3 za = P.ax, xa = normalized(rotate(Qa, j.a.ref)), ya = cross(za, xa);
    const V3 zb = normalized(rotate(Qb, j.b.axis));
    if (j.swing_limited) {
      // b's axis tilted from a's, and the elliptical cone's bound that way (in a's frame)
      const V3 c = cross(za, zb);
      const f64 sn = norm(c), angle = dm::atan2(sn, dot(za, zb));
      const f64 ux0 = dot(zb, xa), uy0 = dot(zb, ya), ul = std::sqrt(ux0 * ux0 + uy0 * uy0);
      if (sn > 1e-6 && ul > 1e-9) {
        const f64 ux = ux0 / ul, uy = uy0 / ul;
        const f64 lx = std::max(1e-3, ux >= 0.0 ? j.swing[0] : j.swing[1]);
        const f64 ly = std::max(1e-3, uy >= 0.0 ? j.swing[2] : j.swing[3]);
        const f64 bound = 1.0 / std::sqrt((ux * ux) / (lx * lx) + (uy * uy) / (ly * ly));
        if (angle > bound - kLimitNear) {
          P.swing_on = true;
          P.swing_ax = c * (1.0 / sn);  // (turning b about it swings b further out)
          const f64 k = dot(P.swing_ax, Isum * P.swing_ax);
          P.swing_k = k > 0.0 ? 1.0 / k : 0.0;
          P.swing_room = room_of(j, bound - angle, 1);
          if (angle > bound) P.swing_e = pull_limit(j, angle - bound, 0.0);
        }
      }
    }
    if (j.twist_limited) {
      // swing-twist: the frames' relative rotation, its turn about the axis (turning b about
      // its own axis changes the twist alone)
      const Quat Fa = Qa * frame_of(j.a.axis, j.a.ref), Fb = Qb * frame_of(j.b.axis, j.b.ref);
      const Quat r = conj(Fa) * Fb;
      f64 tw = 2.0 * dm::atan2(r.z, r.w);
      if (tw > kPi) tw -= 2.0 * kPi;
      else if (tw < -kPi) tw += 2.0 * kPi;
      const f64 mid = 0.5 * (j.twist_lower + j.twist_upper);
      if (tw > j.twist_upper - kLimitNear && tw >= mid) {
        P.twist_side = 1;
        P.twist_room = room_of(j, j.twist_upper - tw, 1);
        if (tw > j.twist_upper) P.twist_e = pull_limit(j, tw - j.twist_upper, 0.0);
      } else if (tw < j.twist_lower + kLimitNear && tw < mid) {
        P.twist_side = -1;
        P.twist_room = room_of(j, j.twist_lower - tw, -1);
        if (tw < j.twist_lower) P.twist_e = pull_limit(j, tw - j.twist_lower, 0.0);
      }
      if (P.twist_side != 0) {
        P.twist_on = true;
        P.twist_ax = zb;
        const f64 k = dot(zb, Isum * zb);
        P.twist_k = k > 0.0 ? 1.0 / k : 0.0;
      }
    }
  }
  // the muscle: a soft drive to the relative rotation it is given (body frames)
  if ((t == JointType::Ball || t == JointType::Hinge) && j.muscle.on()) {
    const JointMuscle& M = j.muscle;
    const Quat qa = ia >= 0 ? bodies[size_t(ia)]->q : j.a.q, qb = ib >= 0 ? bodies[size_t(ib)]->q : j.b.q;
    const V3 e = rotation_vector(qb * conj(qa * M.target));  // (b turned past its target, world)
    P.mus_rate = rotate(qa, M.target_rate);
    f64 c = std::max(0.0, M.damping);
    const f64 k = std::max(0.0, M.stiffness);
    if (t == JointType::Hinge) {
      const f64 kk = dot(P.ax, Isum * P.ax);
      // (the damper sized for the limb it moves, not for the two bodies alone)
      if (M.inertia > 0.0 && kk > 0.0) c *= std::min(1.0, (1.0 / kk) / M.inertia);
      const f64 denom = c + dt * k;
      if (denom > 0.0 && kk > 0.0) {
        P.mus_on = true;
        P.mus_gamma = 1.0 / (dt * denom);
        P.mus_bias = V3{(k / denom) * dot(e, P.ax), 0.0, 0.0};
        P.mus_k1 = 1.0 / (kk + P.mus_gamma);
      }
    } else {
      if (M.inertia > 0.0) {
        // (along the relative spin it damps, else the error)
        const V3 wa0 = P.ia >= 0 ? bodies[size_t(P.ia)]->w : V3{}, wb0 = P.ib >= 0 ? bodies[size_t(P.ib)]->w : V3{};
        V3 u = wb0 - wa0 - P.mus_rate;
        if (!(norm2(u) > 1e-18)) u = e;
        u = normalized(u);
        const f64 ku = dot(u, Isum * u);
        if (ku > 0.0) c *= std::min(1.0, (1.0 / ku) / M.inertia);
      }
      const f64 denom = c + dt * k;
      if (denom > 0.0) {
        P.mus_on = true;
        P.mus_gamma = 1.0 / (dt * denom);
        P.mus_bias = e * (k / denom);
        P.mus_K = inverted(plus(Isum, diag3(P.mus_gamma)));
      }
    }
    P.mus_cap = M.max_torque > 0.0 ? M.max_torque * dt : 0.0;
  }
  // the drive's goal now
  const bool driven = (t == JointType::Hinge || t == JointType::Slider) && j.drive.kind != JointDrive::Kind::Off && j.drive.max > 0.0;
  if (driven) j.drive.goal(time, &P.goal, &P.goal_rate);
  // warm start (the rows that do not act now start from nothing)
  if (!driven) j.motor = 0.0;
  if (P.lim == 0 && t != JointType::Distance) j.limit = 0.0;
  j.lin *= warm_point;
  j.ang *= warm;
  j.axial *= warm;
  j.limit *= warm;
  j.motor *= warm;
  j.swing_imp = P.swing_on ? j.swing_imp * warm : 0.0;
  j.twist_imp = P.twist_on ? j.twist_imp * warm : 0.0;
  if (!P.mus_on) j.muscle_imp = V3{};
  else if (t == JointType::Hinge) j.muscle_imp = P.ax * (dot(j.muscle_imp, P.ax) * warm);  // (about the axis it has now)
  else j.muscle_imp *= warm;
  if (P.mus_cap > 0.0) j.muscle_imp = capped(j.muscle_imp, P.mus_cap);
  // (apply them as the rows do)
  const f64 lim_drive = j.limit + j.motor;
  V3 J = j.lin, L = j.ang;
  if (t == JointType::Hinge) L += P.ax * lim_drive;
  if (t == JointType::Slider) J += P.ax * lim_drive;
  if (t == JointType::Distance) J += P.n * (j.axial + j.limit);
  L += P.swing_ax * j.swing_imp + P.twist_ax * j.twist_imp + j.muscle_imp;
  // (a muscle's feed-forward torque: this substep's impulse, not the joint's to carry)
  if ((t == JointType::Ball || t == JointType::Hinge) && norm2(j.muscle.feed) > 0.0) {
    const V3 f = j.muscle.feed * dt;
    if (P.ib >= 0) bodies[size_t(P.ib)]->w += P.Ib * f;
    if (P.ia >= 0) bodies[size_t(P.ia)]->w -= P.Ia * f;
  }
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

void RigidWorld::solve_joints(bool reverse) {
  const size_t nj = joints.size();
  for (size_t q = 0; q < nj; ++q) solve_joint(reverse ? nj - 1 - q : q);
}

void RigidWorld::solve_joint(size_t k) {
  JointPrep& P = jprep_[k];
  if (!P.on) return;
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
  // the muscle: a spring and a damper towards its target, of limited torque
  if (P.mus_on) {
    if (t == JointType::Hinge) {
      const f64 jv = dot(P.ax, wb() - wa() - P.mus_rate);
      const f64 lam = dot(j.muscle_imp, P.ax);
      f64 nl = lam - P.mus_k1 * (jv + P.mus_bias.x + P.mus_gamma * lam);
      if (P.mus_cap > 0.0) nl = std::clamp(nl, -P.mus_cap, P.mus_cap);
      apply(V3{}, P.ax * (nl - lam));
      j.muscle_imp = P.ax * nl;
    } else {
      const V3 jv = wb() - wa() - P.mus_rate;
      V3 nl = j.muscle_imp - P.mus_K * (jv + P.mus_bias + j.muscle_imp * P.mus_gamma);
      if (P.mus_cap > 0.0) nl = capped(nl, P.mus_cap);
      apply(V3{}, nl - j.muscle_imp);
      j.muscle_imp = nl;
    }
  }
  // (ball) the cone and the twist limit: speculative - it may close on its bound within this
  // substep, not pass it
  if (P.swing_on) {
    const f64 rate = dot(P.swing_ax, wb() - wa());
    const f64 nl = std::min(0.0, j.swing_imp + P.swing_k * (P.swing_room / joint_dt_ - rate));
    apply(V3{}, P.swing_ax * (nl - j.swing_imp));
    j.swing_imp = nl;
  }
  if (P.twist_on) {
    const f64 rate = dot(P.twist_ax, wb() - wa());
    const f64 l = j.twist_imp + P.twist_k * (P.twist_room / joint_dt_ - rate);
    const f64 nl = P.twist_side > 0 ? std::min(0.0, l) : std::max(0.0, l);
    apply(V3{}, P.twist_ax * (nl - j.twist_imp));
    j.twist_imp = nl;
  }
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
      f64 l = -P.kax * rate();
      if (t == JointType::Hinge && j.latched) {
        // (a latch holds up to its strength; beyond, it gives way - finish_joints - and what it
        // could not hold passes on: a door knocked open swings)
        const f64 cap = j.latch * joint_dt_;
        l = std::clamp(j.limit + l, -cap, cap) - j.limit;
      }
      j.limit += l;
      push(l);
    } else if (P.lim != 0) {
      // (speculative: it may close on the bound within this substep, not pass it)
      const f64 bound = P.lim < 0 ? j.lower : j.upper;
      f64 target = (bound - P.value) / joint_dt_;
      if (j.supple) target = P.lim < 0 ? std::min(target, kSuppleStep / joint_dt_) : std::max(target, -kSuppleStep / joint_dt_);
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

void RigidWorld::solve_joints_position(std::vector<V3>& pv, std::vector<V3>& pw) {
  for (size_t k = 0; k < joints.size(); ++k) solve_joint_position(k, pv, pw);
}

void RigidWorld::solve_joint_position(size_t k, std::vector<V3>& pv, std::vector<V3>& pw) {
  JointPrep& P = jprep_[k];
  if (!P.on) return;
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
  // (ball) past its cone or its twist range: turned back only
  if (P.swing_e != 0.0) {
    const f64 l = std::min(0.0, P.swing_k * (P.swing_e - dot(P.swing_ax, wb() - wa())));
    apply(V3{}, P.swing_ax * l);
  }
  if (P.twist_e != 0.0) {
    const f64 l = P.twist_k * (P.twist_e - dot(P.twist_ax, wb() - wa()));
    apply(V3{}, P.twist_ax * (P.twist_e < 0.0 ? std::min(0.0, l) : std::max(0.0, l)));
  }
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

void RigidWorld::finish_joints(f64 dt) {
  for (size_t k = 0; k < joints.size(); ++k) finish_joint(k, dt);
}

void RigidWorld::finish_joint(size_t k, f64 dt) {
  const JointPrep& P = jprep_[k];
  Joint& j = joints[k];
  if (!P.on) return;  // (its bodies asleep: it carries what it carried when they fell asleep)
  j.force = P.J * (1.0 / dt);
  // (the turn it gives b about its anchor)
  j.torque = P.L * (1.0 / dt);
  j.value = P.value;
  // (a latched hinge: what its latch holds shut is not its hinge's; past its strength the latch
  // gives way, and it swings from the next substep)
  V3 held = j.torque;
  if (j.type == JointType::Hinge && j.latched) {
    held -= P.ax * dot(P.ax, j.torque);
    if (std::abs(j.limit) >= j.latch * dt * (1.0 - 1e-9)) {  // (it held all it could)
      j.latched = false;
      j.limit = 0.0;
    }
  }
  if ((j.break_force > 0.0 && norm(j.force) > j.break_force) || (j.break_torque > 0.0 && norm(held) > j.break_torque)) j.broken = true;
  // (a hinge turned past its capacity: a plastic hinge torn through, a door off its hinge)
  if (j.type == JointType::Hinge && j.break_angle > 0.0 && std::abs(P.value) > j.break_angle) j.broken = true;
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
  // A body awake (hit, driven, pushed by the host, woken by one moving near) wakes what is joined
  // to it, and what is joined to that: asleep, a body is a static support to the solver, and would
  // pin what it holds - a body would hang its weight on the doors joined to it.
  auto index_of = [&](i64 id) -> i32 {
    const auto it = std::lower_bound(bodies.begin(), bodies.end(), id, [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
    return (it != bodies.end() && (*it)->id == id) ? static_cast<i32>(it - bodies.begin()) : -1;
  };
  std::vector<std::pair<i32, i32>> pairs;
  for (const Joint& j : joints) {
    if (j.broken || j.a.body == 0 || j.b.body == 0) continue;
    const i32 ia = index_of(j.a.body), ib = index_of(j.b.body);
    if (ia >= 0 && ib >= 0) pairs.push_back({ia, ib});
  }
  // (chains through sleepers: until no pair is half asleep)
  for (bool more = true; more;) {
    more = false;
    for (const auto& [a, b] : pairs) {
      Body& A = *bodies[size_t(a)];
      Body& B = *bodies[size_t(b)];
      if (A.asleep == B.asleep) continue;
      wake(A.asleep ? A : B);
      more = true;
    }
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
