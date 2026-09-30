// structvox — articulations (svx/world/articulation.hpp, docs/MOTION.md §6): bodies of links held
// by joints with limits and muscles, pulled by targets. What they are made of (links: bodies of
// rigid_.bodies with LinkData; joints of rigid_.joints whose ends are of kind Link; targets of
// rigid_.targets; their collision rules), how their hosts drive them (ArticulationControl, into
// the solver when a tick begins) and what they report.
#include <algorithm>
#include <cmath>

#include "svx/world/world.hpp"
#include "world_internal.hpp"

namespace svx {

using namespace world_detail;

namespace {

bool finite_q(const Quat& q) {
  return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w) && (q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w) > 1e-12;
}

M3 diag3(const V3& d) {
  M3 R;
  R.m = {d.x, 0.0, 0.0, 0.0, d.y, 0.0, 0.0, 0.0, d.z};
  return R;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Records

World::ArticulationRec* World::art(ArticulationId id) {
  const auto it = std::lower_bound(arts_.begin(), arts_.end(), id, [](const std::unique_ptr<ArticulationRec>& a, ArticulationId v) { return a->id < v; });
  return it != arts_.end() && (*it)->id == id ? it->get() : nullptr;
}

const World::ArticulationRec* World::art(ArticulationId id) const { return const_cast<World*>(this)->art(id); }

Body* World::art_link(const ArticulationRec& a, u16 link) {
  if (link >= a.links.size()) return nullptr;
  Body* b = rigid_.find(a.links[link]);
  return b && b->link ? b : nullptr;
}

const Body* World::art_link(const ArticulationRec& a, u16 link) const { return const_cast<World*>(this)->art_link(a, link); }

// ---------------------------------------------------------------------------------------------
// The API

ArticulationId World::add_articulation(const ArticulationDesc& d) {
  if (in_tick_) return 0;
  const size_t nl = d.links.size();
  if (nl == 0 || nl > 256 || d.joints.size() > 1024 || d.targets.size() > 1024) return 0;
  for (const LinkDesc& L : d.links) {
    if (!in_range(L.pos) || !finite3(L.vel) || !finite3(L.ang) || !finite_q(L.rot) || !std::isfinite(L.mass) || L.mass < 0.0) return 0;
    if (L.mass > 0.0 && !(finite3(L.inertia) && L.inertia.x > 0.0 && L.inertia.y > 0.0 && L.inertia.z > 0.0)) return 0;
    if (L.spheres.empty() || L.spheres.size() > 16 || !std::isfinite(L.friction) || !std::isfinite(L.twist_damping) || !finite3(L.long_axis)) return 0;
    for (const BodySphere& s : L.spheres)
      if (!finite3(s.c) || !(s.r > 0.0) || !(s.r < 4.0) || norm(s.c) > 8.0) return 0;
  }
  for (const ArticulationJointDesc& J : d.joints) {
    if (J.parent >= nl || J.child >= nl || J.parent == J.child) return 0;
    if (J.type != JointType::Ball && J.type != JointType::Hinge && J.type != JointType::Fixed) return 0;
    if (!finite3(J.anchor_parent) || !finite3(J.anchor_child) || !finite_q(J.frame_parent) || !finite_q(J.frame_child)) return 0;
    for (f64 x : {J.swing[0], J.swing[1], J.swing[2], J.swing[3], J.twist_lower, J.twist_upper, J.hinge_lower, J.hinge_upper})
      if (!std::isfinite(x)) return 0;
  }
  for (const ArticulationTargetDesc& T : d.targets)
    if (T.link >= nl || !finite3(T.local)) return 0;
  for (const auto& [a, b] : d.collide)
    if (a >= nl || b >= nl || a == b) return 0;

  const ArticulationId id = next_art_++;
  auto rec = std::make_unique<ArticulationRec>();
  rec->id = id;
  rec->joint_desc = d.joints;
  rec->target_desc = d.targets;
  rec->collide = d.collide;
  rec->group = d.group;
  rec->tag = d.tag;
  rec->data = d.data;
  // the links: bodies of no voxels, colliding as spheres (never pieces: not announced, not culled)
  std::vector<Quat> rot0(nl);
  for (size_t i = 0; i < nl; ++i) {
    const LinkDesc& L = d.links[i];
    auto b = std::make_unique<Body>();
    b->id = next_id_++;
    b->link = std::make_unique<LinkData>();
    LinkData& ld = *b->link;
    ld.articulation = id;
    ld.index = static_cast<u16>(i);
    ld.spheres = L.spheres;
    ld.friction = std::max(0.0, L.friction);
    ld.long_axis = norm2(L.long_axis) > 1e-18 ? normalized(L.long_axis) : V3{0, 0, 1};
    ld.twist_damping = std::max(0.0, L.twist_damping);
    ld.kinematic = !(L.mass > 0.0);
    b->mass = ld.kinematic ? 0.0 : L.mass;
    b->inv_mass = ld.kinematic ? 0.0 : 1.0 / L.mass;
    b->inertia = ld.kinematic ? M3{} : diag3(L.inertia);
    b->inv_inertia = ld.kinematic ? M3{} : diag3(V3{1.0 / L.inertia.x, 1.0 / L.inertia.y, 1.0 / L.inertia.z});
    b->x = L.pos;
    b->q = qnormalized(L.rot);
    b->v = L.vel;
    b->w = L.ang;
    b->v_pre = b->v;
    b->w_pre = b->w;
    rot0[i] = b->q;
    for (const BodySphere& s : L.spheres) b->radius = std::max(b->radius, norm(s.c) + s.r);
    b->keep = true;
    b->refresh_box();
    rec->links.push_back(b->id);
    rigid_.add(std::move(b));
  }
  // the joints: ends of kind Link, their frames from the desc (a ball's and a fixed joint's axis
  // is the frame's z, its reference x; a hinge turns about x, its angle measured from z)
  for (const ArticulationJointDesc& J : d.joints) {
    JointRec r;
    Joint j;
    j.id = next_joint_++;
    j.type = J.type;
    const bool hinge = J.type == JointType::Hinge;
    const V3 ax = hinge ? V3{1, 0, 0} : V3{0, 0, 1}, rf = hinge ? V3{0, 0, 1} : V3{1, 0, 0};
    JointRec::End* ends[2] = {&r.a, &r.b};
    const u16 which[2] = {J.parent, J.child};
    const V3 anchor[2] = {J.anchor_parent, J.anchor_child};
    const Quat frame[2] = {qnormalized(J.frame_parent), qnormalized(J.frame_child)};
    for (int e = 0; e < 2; ++e) {
      JointRec::End& E = *ends[e];
      E.kind = JointAnchor::Kind::Link;
      E.piece = rec->links[which[e]];
      E.point = anchor[e];
      E.axis = rotate(frame[e], ax);
      E.ref = rotate(frame[e], rf);
    }
    j.collide = false;  // (the articulation's rules say which of its links collide)
    if (hinge) {
      j.limited = J.hinge_limited;
      j.lower = J.hinge_lower;
      j.upper = J.hinge_upper;
    } else if (J.type == JointType::Ball) {
      j.swing_limited = J.swing_limited;
      for (int q = 0; q < 4; ++q) j.swing[q] = std::max(0.0, J.swing[q]);
      j.twist_limited = J.twist_limited;
      j.twist_lower = std::min(J.twist_lower, J.twist_upper);
      j.twist_upper = std::max(J.twist_lower, J.twist_upper);
    }
    j.rel = conj(rot0[J.parent]) * rot0[J.child];
    r.id = j.id;
    const size_t k = insert_joint(r, j);
    fill_joint_end(jrecs_[k], false);
    fill_joint_end(jrecs_[k], true);
    rec->joints.push_back(j.id);
  }
  // the targets (ids ascending: after every one there is)
  for (const ArticulationTargetDesc& T : d.targets) {
    Target t;
    t.id = next_target_++;
    t.body = rec->links[T.link];
    t.kind = T.kind;
    t.local = T.local;
    rigid_.targets.push_back(t);
    rec->targets.push_back(t.id);
  }
  // its rules
  ArticulationRules R;
  R.id = id;
  for (const auto& [a, b] : d.collide) R.pairs.push_back((static_cast<u32>(std::min(a, b)) << 16) | std::max(a, b));
  std::sort(R.pairs.begin(), R.pairs.end());
  R.pairs.erase(std::unique(R.pairs.begin(), R.pairs.end()), R.pairs.end());
  rigid_.articulations.push_back(std::move(R));
  // its drive: nothing yet
  rec->control = std::make_unique<ArticulationControl>();
  ArticulationControl& C = *rec->control;
  C.muscles.resize(d.joints.size());
  C.targets.resize(d.targets.size());
  C.force.resize(nl);
  C.torque.resize(nl);
  C.ghost.assign(nl, 0);
  arts_.push_back(std::move(rec));
  return id;
}

bool World::remove_articulation(ArticulationId id) {
  if (in_tick_ || !art(id)) return false;
  drop_articulation(id, PieceEnd::Removed);
  return true;
}

void World::drop_articulation(ArticulationId id, PieceEnd end) {
  const auto it = std::lower_bound(arts_.begin(), arts_.end(), id, [](const std::unique_ptr<ArticulationRec>& a, ArticulationId v) { return a->id < v; });
  if (it == arts_.end() || (*it)->id != id) return;
  ArticulationRec& a = **it;
  WorldEvent ev;
  ev.kind = WorldEvent::Kind::ArticulationRemoved;
  ev.end = end;
  ev.id = id;
  if (const Body* b0 = art_link(a, 0)) {
    ev.pos = b0->x;
    ev.rot = b0->q;
    ev.vel = b0->v;
  }
  ev.voxels = static_cast<i32>(a.links.size());
  events_.push_back(std::move(ev));
  // its joints (what the host joined to its links gives way: its end is gone), its targets, its rules
  for (JointId jid : a.joints)
    for (size_t k = 0; k < jrecs_.size(); ++k)
      if (jrecs_[k].id == jid) {
        jrecs_.erase(jrecs_.begin() + static_cast<std::ptrdiff_t>(k));
        rigid_.joints.erase(rigid_.joints.begin() + static_cast<std::ptrdiff_t>(k));
        break;
      }
  std::vector<u32> tids = a.targets;
  std::sort(tids.begin(), tids.end());
  rigid_.targets.erase(std::remove_if(rigid_.targets.begin(), rigid_.targets.end(), [&](const Target& t) { return std::binary_search(tids.begin(), tids.end(), t.id); }),
                       rigid_.targets.end());
  rigid_.articulations.erase(std::remove_if(rigid_.articulations.begin(), rigid_.articulations.end(), [&](const ArticulationRules& r) { return r.id == id; }),
                             rigid_.articulations.end());
  // its links (what rested on them falls)
  std::vector<i64> ids = a.links;
  std::sort(ids.begin(), ids.end());
  for (i64 bid : ids) {
    dead_loads_.erase(bid);
    if (const Body* b = rigid_.find(bid)) wake_around(*b);
  }
  rigid_.remove_if([&](const Body& b) { return std::binary_search(ids.begin(), ids.end(), b.id); });
  arts_.erase(it);
}

void World::clear_articulations() {
  std::vector<ArticulationId> ids;
  for (const auto& a : arts_) ids.push_back(a->id);
  for (ArticulationId id : ids) drop_articulation(id, PieceEnd::Removed);
  rigid_.targets.clear();
  rigid_.articulations.clear();
}

void World::articulations_out_of_world(f64 floor_z) {
  std::vector<ArticulationId> out;
  for (const auto& a : arts_)
    for (i64 bid : a->links) {
      const Body* b = rigid_.find(bid);
      if (!b) continue;
      const bool finite = finite3(b->x) && finite3(b->v) && finite3(b->w) && std::isfinite(b->q.w);
      if (!finite || b->x.z < floor_z) {
        out.push_back(a->id);
        break;
      }
    }
  for (ArticulationId id : out) drop_articulation(id, PieceEnd::OutOfWorld);
}

std::vector<ArticulationId> World::articulations() const {
  std::vector<ArticulationId> out;
  out.reserve(arts_.size());
  for (const auto& a : arts_) out.push_back(a->id);
  return out;
}

ArticulationControl* World::articulation_control(ArticulationId id) {
  ArticulationRec* a = art(id);
  return a ? a->control.get() : nullptr;
}

bool World::articulation_state(ArticulationId id, ArticulationState* out) const {
  const ArticulationRec* a = art(id);
  if (!a || !out) return false;
  out->links.resize(a->links.size());
  out->asleep = true;
  for (size_t i = 0; i < a->links.size(); ++i) {
    LinkState& s = out->links[i];
    const Body* b = art_link(*a, static_cast<u16>(i));
    if (!b) {
      s = LinkState{};
      continue;
    }
    const LinkData& L = *b->link;
    s.pos = b->x;
    s.rot = b->q;
    s.vel = b->v;
    s.ang = b->w;
    s.mass = b->mass;
    s.gone = L.gone;
    s.contact = L.contact;
    s.contact_normal = L.contact_normal;
    s.contact_point = L.contact_point;
    s.impact = L.impact;
    s.bumped = L.bumped;
    out->asleep = out->asleep && b->asleep;
  }
  out->target_applied.resize(a->targets.size());
  for (size_t k = 0; k < a->targets.size(); ++k) {
    out->target_applied[k] = V3{};
    const u32 tid = a->targets[k];
    const auto it = std::lower_bound(rigid_.targets.begin(), rigid_.targets.end(), tid, [](const Target& t, u32 v) { return t.id < v; });
    if (it != rigid_.targets.end() && it->id == tid) out->target_applied[k] = it->applied;
  }
  out->group = a->group;
  out->tag = a->tag;
  return true;
}

const std::vector<u8>* World::articulation_data(ArticulationId id) const {
  const ArticulationRec* a = art(id);
  return a ? &a->data : nullptr;
}

bool World::set_articulation_data(ArticulationId id, std::vector<u8> data) {
  ArticulationRec* a = art(id);
  if (!a) return false;
  a->data = std::move(data);
  return true;
}

bool World::set_link(ArticulationId id, u16 link, const V3& pos, const Quat& rot, const V3& vel, const V3& ang) {
  ArticulationRec* a = art(id);
  Body* b = a ? art_link(*a, link) : nullptr;
  if (!b || in_tick_ || !in_range(pos) || !finite_q(rot) || !finite3(vel) || !finite3(ang)) return false;
  b->x = pos;
  b->q = qnormalized(rot);
  b->v = vel;
  b->w = ang;
  b->v_pre = b->v;
  b->w_pre = b->w;
  b->refresh_box();
  wake_articulation(id);
  return true;
}

bool World::add_link_velocity(ArticulationId id, u16 link, const V3& dv, const V3& dw) {
  ArticulationRec* a = art(id);
  Body* b = a ? art_link(*a, link) : nullptr;
  if (!b || !finite3(dv) || !finite3(dw) || b->link->kinematic) return false;
  b->v += dv;
  b->w += dw;
  wake_articulation(id);
  return true;
}

bool World::apply_link_impulse(ArticulationId id, u16 link, const V3& point, const V3& impulse) {
  ArticulationRec* a = art(id);
  Body* b = a ? art_link(*a, link) : nullptr;
  if (!b || !finite3(point) || !finite3(impulse) || b->link->kinematic) return false;
  b->v += impulse * b->inv_mass;
  b->w += b->inv_inertia_world() * cross(point - b->x, impulse);
  wake_articulation(id);
  return true;
}

bool World::lose_link(ArticulationId id, u16 link, f64 mass_scale) {
  ArticulationRec* a = art(id);
  Body* b = a ? art_link(*a, link) : nullptr;
  if (!b || !(mass_scale > 0.0) || !(mass_scale <= 1.0)) return false;
  if (b->link->gone) return true;
  b->link->gone = true;
  if (!b->link->kinematic) {
    b->mass *= mass_scale;
    b->inv_mass /= mass_scale;
    for (f64& x : b->inertia.m) x *= mass_scale;
    for (f64& x : b->inv_inertia.m) x /= mass_scale;
  }
  wake_articulation(id);
  return true;
}

bool World::wake_articulation(ArticulationId id) {
  ArticulationRec* a = art(id);
  if (!a) return false;
  for (u16 i = 0; i < a->links.size(); ++i)
    if (Body* b = art_link(*a, i)) rigid_.wake(*b);
  return true;
}

i64 World::link_body(ArticulationId id, u16 link) const {
  const ArticulationRec* a = art(id);
  const Body* b = a ? art_link(*a, link) : nullptr;
  return b ? b->id : 0;
}

// ---------------------------------------------------------------------------------------------
// Each tick

void World::apply_articulation_controls() {
  rigid_.begin_tick();
  for (auto& ap : arts_) {
    ArticulationRec& a = *ap;
    ArticulationControl& C = *a.control;
    {
      auto& rules = rigid_.articulations;
      const auto r = std::lower_bound(rules.begin(), rules.end(), a.id, [](const ArticulationRules& x, u32 v) { return x.id < v; });
      if (r != rules.end() && r->id == a.id) {
        r->self_collide = C.self_collide;
        r->can_sleep = C.can_sleep;
      }
    }
    const f64 spin = std::isfinite(C.max_spin) ? std::max(0.0, C.max_spin) : 0.0;
    const f64 kl = std::isfinite(C.keep_linear) ? std::clamp(C.keep_linear, 0.0, 1.0) : 1.0;
    const f64 ka = std::isfinite(C.keep_angular) ? std::clamp(C.keep_angular, 0.0, 1.0) : 1.0;
    for (u16 i = 0; i < a.links.size(); ++i) {
      Body* b = art_link(a, i);
      if (!b) continue;
      LinkData& L = *b->link;
      L.ghost = i < C.ghost.size() && C.ghost[i] != 0;
      L.max_spin = spin;
      L.keep_linear = kl;
      L.keep_angular = ka;
      if (i < C.force.size() && finite3(C.force[i])) b->force += C.force[i];
      if (i < C.torque.size() && finite3(C.torque[i])) b->torque += C.torque[i];
    }
    // (forces are for one tick)
    for (V3& f : C.force) f = V3{};
    for (V3& t : C.torque) t = V3{};
    for (size_t k = 0; k < a.joints.size() && k < C.muscles.size(); ++k) {
      const JointId jid = a.joints[k];
      const auto it = std::lower_bound(jrecs_.begin(), jrecs_.end(), jid, [](const JointRec& r, JointId v) { return r.id < v; });
      if (it == jrecs_.end() || it->id != jid) continue;
      JointMuscle m = C.muscles[k];
      if (!finite_q(m.target) || !finite3(m.target_rate) || !finite3(m.feed) || !std::isfinite(m.stiffness) || !std::isfinite(m.damping) ||
          !std::isfinite(m.max_torque) || !std::isfinite(m.inertia))
        m = JointMuscle{};
      m.target = qnormalized(m.target);
      rigid_.joints[size_t(it - jrecs_.begin())].muscle = m;
    }
    for (size_t k = 0; k < a.targets.size() && k < C.targets.size(); ++k) {
      const u32 tid = a.targets[k];
      const auto it = std::lower_bound(rigid_.targets.begin(), rigid_.targets.end(), tid, [](const Target& t, u32 v) { return t.id < v; });
      if (it == rigid_.targets.end() || it->id != tid) continue;
      TargetDrive d = C.targets[k];
      if (!finite3(d.pos) || !finite3(d.vel) || !finite_q(d.rot) || !finite3(d.up) || !std::isfinite(d.stiffness) || !std::isfinite(d.damping) ||
          !std::isfinite(d.max))
        d.on = false;
      d.rot = qnormalized(d.rot);
      it->drive = d;
    }
  }
}

}  // namespace svx
