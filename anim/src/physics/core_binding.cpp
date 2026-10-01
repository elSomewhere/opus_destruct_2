#include "svx/anim/physics/core_binding.hpp"

#include <algorithm>

namespace svx::anim {

namespace {

// (a rigid assist of the XPBD solver: very stiff in the core's)
constexpr f64 kRigid = 1e9;

f64 finite_or(f64 v, f64 inf_value) { return std::isinf(v) ? inf_value : v; }

bool same(const V3& a, const V3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool same(const Quat& a, const Quat& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }

}  // namespace

ArticulationDesc CoreBinding::desc_of(const RigidSystem& s) {
  ArticulationDesc d;
  d.links.reserve(s.bodies.size());
  for (const auto& bp : s.bodies) {
    const RigidBody& b = *bp;
    LinkDesc L;
    L.mass = b.inv_mass > 0.0 ? b.mass : 0.0;
    L.inertia = V3{b.inv_i.x > 0.0 ? 1.0 / b.inv_i.x : 1e-6, b.inv_i.y > 0.0 ? 1.0 / b.inv_i.y : 1e-6, b.inv_i.z > 0.0 ? 1.0 / b.inv_i.z : 1e-6};
    L.pos = b.x;
    L.rot = b.q;
    L.vel = b.v;
    L.ang = b.w;
    for (const Sphere& sp : b.spheres) L.spheres.push_back(BodySphere{sp.c, sp.r});
    L.friction = b.friction;
    L.long_axis = b.long_axis;
    L.twist_damping = b.twist_damping;
    d.links.push_back(std::move(L));
  }
  for (const auto& jp : s.joints) {
    const Joint& j = *jp;
    ArticulationJointDesc J;
    J.parent = static_cast<u16>(j.a->index);
    J.child = static_cast<u16>(j.b->index);
    J.type = j.kind == JointKind::Hinge ? JointType::Hinge : JointType::Ball;
    J.anchor_parent = j.anchor_a;
    J.anchor_child = j.anchor_b;
    J.frame_parent = j.frame_a;
    J.frame_child = j.frame_b;
    if (j.swing) {
      J.swing_limited = true;
      J.swing[0] = j.swing->x_pos;
      J.swing[1] = j.swing->x_neg;
      J.swing[2] = j.swing->y_pos;
      J.swing[3] = j.swing->y_neg;
    }
    if (j.twist) {
      J.twist_limited = true;
      J.twist_lower = j.twist->first;
      J.twist_upper = j.twist->second;
    }
    if (j.hinge) {
      J.hinge_limited = true;
      J.hinge_lower = j.hinge->first;
      J.hinge_upper = j.hinge->second;
    }
    d.joints.push_back(J);
  }
  // the targets: the attachments, then the orienters
  for (const auto& ap : s.attachments) {
    ArticulationTargetDesc T;
    T.link = static_cast<u16>(ap->body->index);
    T.kind = Target::Kind::Point;
    T.local = ap->local;
    d.targets.push_back(T);
  }
  for (const auto& op : s.orienters) {
    ArticulationTargetDesc T;
    T.link = static_cast<u16>(op->body->index);
    T.kind = Target::Kind::Rotation;
    d.targets.push_back(T);
  }
  // the link pairs that collide (the system's sphere pairs, per pair of bodies)
  for (const SpherePair& p : s.pairs) {
    u16 a = static_cast<u16>(p.a->index), b = static_cast<u16>(p.b->index);
    if (a > b) std::swap(a, b);
    d.collide.emplace_back(a, b);
  }
  std::sort(d.collide.begin(), d.collide.end());
  d.collide.erase(std::unique(d.collide.begin(), d.collide.end()), d.collide.end());
  return d;
}

bool CoreBinding::bind(World& w, RigidSystem& s, u32 group, u32 tag, std::vector<u8> data) {
  if (id_ != 0) unbind(w);
  ArticulationDesc d = desc_of(s);
  d.group = group;
  d.tag = tag;
  d.data = std::move(data);
  const ArticulationId id = w.add_articulation(d);
  if (id == 0) return false;
  return adopt(w, s, id);
}

bool CoreBinding::adopt(World& w, RigidSystem& s, ArticulationId id) {
  ArticulationState st;
  if (!w.articulation_state(id, &st) || st.links.size() != s.bodies.size()) return false;
  const ArticulationControl* c = w.articulation_control(id);
  if (!c || c->muscles.size() != s.joints.size() || c->targets.size() != s.attachments.size() + s.orienters.size()) return false;
  sys_ = &s;
  id_ = id;
  s.external = true;
  s.may_sleep = false;
  gone_.assign(s.bodies.size(), 0);
  for (size_t i = 0; i < s.bodies.size(); ++i) {
    const RigidBody& b = *s.bodies[i];
    // (a part lost before: gone in the core too, its mass as it is)
    if (b.gone && !st.links[i].gone) w.lose_link(id, static_cast<u16>(i), 1.0);
    gone_[i] = b.gone || st.links[i].gone ? 1 : 0;
  }
  // (the articulation is the truth now: the bodies where its links are)
  for (size_t i = 0; i < s.bodies.size(); ++i) {
    RigidBody& b = *s.bodies[i];
    const LinkState& L = st.links[i];
    b.x = L.pos;
    b.q = L.rot;
    b.v = L.vel;
    b.w = L.ang;
    b.update_inertia();
  }
  core_asleep_ = st.asleep;
  s.asleep = st.asleep;
  start_.assign(s.bodies.size(), V3{});
  for (size_t i = 0; i < s.bodies.size(); ++i) start_[i] = s.bodies[i]->x;
  snapshot();
  return true;
}

void CoreBinding::unbind(World& w) {
  if (id_ != 0) w.remove_articulation(id_);
  id_ = 0;
  if (sys_) {
    sys_->external = false;
    sys_->may_sleep = false;
    sys_->wake();
  }
  sys_ = nullptr;
}

void CoreBinding::snapshot() {
  const size_t n = sys_->bodies.size();
  x_.resize(n);
  v_.resize(n);
  w_.resize(n);
  q_.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const RigidBody& b = *sys_->bodies[i];
    x_[i] = b.x;
    q_[i] = b.q;
    v_[i] = b.v;
    w_[i] = b.w;
  }
}

void CoreBinding::push(World& w) {
  if (id_ == 0 || !sys_) return;
  RigidSystem& s = *sys_;
  ArticulationControl* c = w.articulation_control(id_);
  if (!c) return;
  // the muscles (and the anchors, which a body may move: a shoulder follows its clavicle)
  c->anchor_parent.resize(c->muscles.size());
  c->anchor_child.resize(c->muscles.size());
  for (size_t k = 0; k < s.joints.size() && k < c->muscles.size(); ++k) {
    const Joint& j = *s.joints[k];
    c->anchor_parent[k] = j.anchor_a;
    c->anchor_child[k] = j.anchor_b;
    JointMuscle& m = c->muscles[k];
    m.target = j.target;
    m.target_rate = j.target_vel;
    m.stiffness = j.stiffness;
    m.damping = j.damping;
    m.max_torque = finite_or(j.max_torque, 0.0);
    m.inertia = j.eff_inertia;
    m.feed = j.feed;
  }
  // the assists
  const size_t na = s.attachments.size();
  for (size_t k = 0; k < na && k < c->targets.size(); ++k) {
    const Attachment& a = *s.attachments[k];
    TargetDrive& t = c->targets[k];
    t.on = a.enabled;
    t.pos = a.target;
    t.vel = a.target_vel;
    for (int ax = 0; ax < 3; ++ax) t.axes[ax] = a.axes[ax];
    t.stiffness = finite_or(a.stiffness, kRigid);
    t.damping = a.damping;
    t.max = finite_or(a.max_force, 0.0);
  }
  for (size_t k = 0; k < s.orienters.size() && na + k < c->targets.size(); ++k) {
    const Orienter& o = *s.orienters[k];
    TargetDrive& t = c->targets[na + k];
    t.on = o.enabled;
    t.rot = o.target;
    t.tilt_only = o.tilt_only;
    t.up = o.up;
    t.stiffness = finite_or(o.stiffness, kRigid);
    t.damping = o.damping;
    t.max = finite_or(o.max_torque, 0.0);
  }
  // forces for this tick (then spent), flags
  const size_t n = s.bodies.size();
  c->force.resize(n);
  c->torque.resize(n);
  c->ghost.resize(n);
  for (size_t i = 0; i < n; ++i) {
    RigidBody& b = *s.bodies[i];
    c->force[i] = b.force;
    c->torque[i] = b.torque;
    b.force = V3{};
    b.torque = V3{};
    c->ghost[i] = b.ghost ? 1 : 0;
  }
  c->max_spin = s.spin_cap;
  c->keep_linear = s.linear_drag;
  c->keep_angular = s.angular_drag;
  c->self_collide = s.pairs_enabled;
  c->can_sleep = s.may_sleep;
  s.may_sleep = false;
  // what the host did to the bodies since the pull: to the links
  bool moved = false;
  for (size_t i = 0; i < n; ++i) {
    RigidBody& b = *s.bodies[i];
    if (b.gone && !gone_[i]) {
      w.lose_link(id_, static_cast<u16>(i), b.mass > 0.0 && b.inv_mass > 0.0 ? 0.05 : 1.0);
      gone_[i] = 1;
    }
    if (same(b.x, x_[i]) && same(b.q, q_[i]) && same(b.v, v_[i]) && same(b.w, w_[i])) continue;
    w.set_link(id_, static_cast<u16>(i), b.x, b.q, b.v, b.w);
    moved = true;
  }
  if (moved) core_asleep_ = false;
  // (woken by its host: a blow on a body at rest)
  if (core_asleep_ && !s.asleep) {
    w.wake_articulation(id_);
    core_asleep_ = false;
  }
  snapshot();
  for (size_t i = 0; i < n; ++i) start_[i] = s.bodies[i]->x;
}

bool CoreBinding::pull(World& w, f64 dt) {
  if (id_ == 0 || !sys_) return false;
  RigidSystem& s = *sys_;
  ArticulationState st;
  if (!w.articulation_state(id_, &st) || st.links.size() != s.bodies.size()) {
    // (gone from the world: the system is its own again, where it last was)
    id_ = 0;
    s.external = false;
    sys_ = nullptr;
    return false;
  }
  for (size_t i = 0; i < s.bodies.size(); ++i) {
    RigidBody& b = *s.bodies[i];
    const LinkState& L = st.links[i];
    b.px = b.x;
    b.pq = b.q;
    b.x = L.pos;
    b.q = L.rot;
    b.v = L.vel;
    b.w = L.ang;
    b.contact = L.contact;
    b.contact_normal = L.contact_normal;
    b.contact_point = L.contact_point;
    b.impact = L.impact;
    b.bumped = L.bumped;
    b.update_inertia();
  }
  for (size_t k = 0; k < s.attachments.size() && k < st.target_applied.size(); ++k) s.attachments[k]->applied = st.target_applied[k];
  core_asleep_ = st.asleep;
  s.asleep = st.asleep;
  s.reactions.clear();  // (the core pushed back what they hit)
  if (dt > 0.0) s.note_motion(dt, start_);
  snapshot();
  return true;
}

}  // namespace svx::anim
