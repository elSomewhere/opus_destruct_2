#include "svx/anim/ik.hpp"

namespace svx::anim {

ModelFK::ModelFK(SkeletonPtr sk) : skeleton(std::move(sk)) {
  p = skeleton->rest_head;
  q.assign(p.size(), Quat{});
}

ModelFK& ModelFK::update(const Pose& pose, i32 from) {
  for (i32 i = from; i < skeleton->count; ++i) update_bone(pose, i);
  return *this;
}

void ModelFK::update_bone(const Pose& pose, i32 i) {
  const i32 par = skeleton->parents[size_t(i)];
  if (par < 0) {
    p[size_t(i)] = pose.t[size_t(i)];
    q[size_t(i)] = pose.r[size_t(i)];
    return;
  }
  p[size_t(i)] = p[size_t(par)] + rotate(q[size_t(par)], pose.t[size_t(i)]);
  q[size_t(i)] = q[size_t(par)] * pose.r[size_t(i)];
}

void ModelFK::update_subtree(const Pose& pose, i32 i) {
  update_bone(pose, i);
  for (i32 c : skeleton->children[size_t(i)]) update_subtree(pose, c);
}

void set_model_rotation(Pose& pose, ModelFK& fk, i32 i, const Quat& q) {
  const i32 par = pose.skeleton->parents[size_t(i)];
  if (par < 0) pose.r[size_t(i)] = q;
  else pose.r[size_t(i)] = conj(fk.q[size_t(par)]) * q;
  fk.update_bone(pose, i);
}

namespace {

void basis(const V3& a, const V3& b, V3& x, V3& y, V3& z) {
  x = vnorm(a);
  z = cross(x, b);
  if (norm(z) < 1e-9) z = cross(x, std::abs(x.z) < 0.9 ? V3{0, 0, 1} : V3{1, 0, 0});
  z = vnorm(z);
  y = cross(z, x);
}

// The rest-space reference that maps to the pole: the pole's part square to the rest bone.
V3 rest_pole_for(const V3& rest_bone, const V3& pole) {
  const V3 n = vnorm(rest_bone);
  const V3 w = pole - n * dot(pole, n);
  if (norm(w) < 1e-6) return cross(n, V3{1, 0, 0});
  return vnorm(w);
}

}  // namespace

Quat frame_rotation(const V3& a, const V3& b, const V3& a2, const V3& b2) {
  V3 x0, y0, z0, x1, y1, z1;
  basis(a, b, x0, y0, z0);
  basis(a2, b2, x1, y1, z1);
  return qfrom_basis(x1, y1, z1) * conj(qfrom_basis(x0, y0, z0));
}

TwoBoneResult solve_two_bone(Pose& pose, ModelFK& fk, i32 upper, i32 lower, i32 end, const V3& target, const V3& pole, f64 soft, const V3* rest_pole) {
  const Skeleton& sk = *pose.skeleton;
  const f64 a = norm(sk.rest_head[size_t(lower)] - sk.rest_head[size_t(upper)]);
  const f64 b = norm(sk.rest_head[size_t(end)] - sk.rest_head[size_t(lower)]);
  const V3 root = fk.p[size_t(upper)];
  V3 dir = target - root;
  f64 d = norm(dir);
  TwoBoneResult res;
  res.reach = d / (a + b);
  const f64 maxd = a + b;
  // soft IK: full extension approached asymptotically
  const f64 ds = maxd - soft * maxd;
  if (soft > 0.0 && d > ds) d = ds + soft * maxd * (1.0 - exp(-(d - ds) / (soft * maxd)));
  d = clamp(d, std::abs(a - b) + 1e-4, maxd - 1e-5);
  dir = vnorm(dir, V3{0, 0, -1});
  // the bend plane: the pole's part square to the chain
  V3 w = pole - dir * dot(pole, dir);
  if (norm(w) < 1e-6) w = cross(dir, V3{1, 0, 0});
  w = vnorm(w);
  const f64 cosA = clamp((a * a + d * d - b * b) / (2.0 * a * d), -1.0, 1.0);
  const f64 sinA = std::sqrt(1.0 - cosA * cosA);
  const V3 mid = root + dir * (a * cosA) + w * (a * sinA);
  const V3 endp = root + dir * d;
  const V3 rest_up = sk.rest_head[size_t(lower)] - sk.rest_head[size_t(upper)];
  const V3 rest_lo = sk.rest_head[size_t(end)] - sk.rest_head[size_t(lower)];
  const V3 ref = rest_pole ? *rest_pole : pole;
  // Both segments share one bend-plane normal. Projecting the same pole onto
  // each segment flips the lower frame by 180 degrees when a bent elbow/knee
  // passes perpendicular to the target direction (common in guards and get-ups).
  const V3 normal = vnorm(cross(dir, w));
  const Quat qu = frame_rotation(rest_up, rest_pole_for(rest_up, ref), mid - root, cross(normal, mid - root));
  set_model_rotation(pose, fk, upper, qu);
  fk.update_bone(pose, lower);
  const Quat ql = frame_rotation(rest_lo, rest_pole_for(rest_lo, ref), endp - mid, cross(normal, endp - mid));
  set_model_rotation(pose, fk, lower, ql);
  fk.update_bone(pose, end);
  res.mid = mid;
  res.end = endp;
  return res;
}

void aim_bone(Pose& pose, ModelFK& fk, i32 i, const V3& axis, const V3& up, const V3& dir, const V3& up_wanted, f64 weight) {
  const Quat q = frame_rotation(axis, up, dir, up_wanted);
  const Quat cur = fk.q[size_t(i)];
  set_model_rotation(pose, fk, i, weight >= 1.0 ? q : qnlerp(cur, q, weight));
}

}  // namespace svx::anim
