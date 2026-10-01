#include "svx/anim/skeleton.hpp"

#include <algorithm>

namespace svx::anim {

Skeleton::Skeleton(const std::vector<BoneDef>& defs) {
  count = static_cast<i32>(defs.size());
  children.resize(defs.size());
  for (size_t i = 0; i < defs.size(); ++i) {
    const BoneDef& d = defs[i];
    names.push_back(d.name);
    i32 p = -1;
    if (!d.parent.empty())
      for (size_t k = 0; k < i; ++k)
        if (defs[k].name == d.parent) p = static_cast<i32>(k);
    parents.push_back(p);
    rest_head.push_back(d.head);
    rest_tail.push_back(d.tail);
    rest_local.push_back(p < 0 ? d.head : d.head - defs[size_t(p)].head);
    if (p >= 0) children[size_t(p)].push_back(static_cast<i32>(i));
  }
}

i32 Skeleton::index(const std::string& name) const {
  for (i32 i = 0; i < count; ++i)
    if (names[size_t(i)] == name) return i;
  return -1;
}

Pose::Pose(SkeletonPtr sk) : skeleton(std::move(sk)) {
  t = skeleton->rest_local;
  r.assign(t.size(), Quat{});
}

Pose& Pose::reset() {
  t = skeleton->rest_local;
  std::fill(r.begin(), r.end(), Quat{});
  return *this;
}

Pose& Pose::copy_from(const Pose& p) {
  t = p.t;
  r = p.r;
  return *this;
}

Pose& Pose::blend(const Pose& p, f64 w, const std::vector<f32>* mask) {
  for (size_t i = 0; i < t.size(); ++i) {
    const f64 k = mask ? w * (*mask)[i] : w;
    if (k <= 0.0) continue;
    if (k >= 1.0) {
      t[i] = p.t[i];
      r[i] = p.r[i];
      continue;
    }
    t[i] = vlerp(t[i], p.t[i], k);
    r[i] = qnlerp(r[i], p.r[i], k);
  }
  return *this;
}

WorldPose::WorldPose(SkeletonPtr sk) : skeleton(std::move(sk)) {
  p = skeleton->rest_head;
  q.assign(p.size(), Quat{});
}

WorldPose& WorldPose::compute(const Pose& pose, const V3& root_pos, const Quat& root_rot) {
  const Skeleton& s = *skeleton;
  for (i32 i = 0; i < s.count; ++i) {
    const i32 par = s.parents[size_t(i)];
    const V3& pp = par < 0 ? root_pos : p[size_t(par)];
    const Quat& pq = par < 0 ? root_rot : q[size_t(par)];
    p[size_t(i)] = pp + rotate(pq, pose.t[size_t(i)]);
    q[size_t(i)] = pq * pose.r[size_t(i)];
  }
  return *this;
}

WorldPose& WorldPose::copy_from(const WorldPose& w) {
  p = w.p;
  q = w.q;
  return *this;
}

void WorldPose::write_skin(f32* out) const {
  const Skeleton& s = *skeleton;
  for (i32 i = 0; i < s.count; ++i) write_rigid(out + 16 * i, p[size_t(i)], q[size_t(i)], s.rest_head[size_t(i)]);
}

}  // namespace svx::anim
