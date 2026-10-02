// structvox — articulations (svx/world/articulation.hpp, docs/MOTION.md §6): bodies of links held
// by joints with limits and muscles, pulled by targets. What they are made of (links: bodies of
// rigid_.bodies with LinkData; joints of rigid_.joints whose ends are of kind Link; targets of
// rigid_.targets; their collision rules), how their hosts drive them (ArticulationControl, into
// the solver when a tick begins) and what they report.
#include <algorithm>
#include <cmath>

#include "archive.hpp"
#include "bytes.hpp"
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

World::Impl::ArticulationRec* World::Impl::art(ArticulationId id) {
  const auto it = std::lower_bound(arts_.begin(), arts_.end(), id, [](const std::unique_ptr<ArticulationRec>& a, ArticulationId v) { return a->id < v; });
  return it != arts_.end() && (*it)->id == id ? it->get() : nullptr;
}

const World::Impl::ArticulationRec* World::Impl::art(ArticulationId id) const { return const_cast<Impl*>(this)->art(id); }

Body* World::Impl::art_link(const ArticulationRec& a, u16 link) {
  if (link >= a.links.size()) return nullptr;
  Body* b = rigid_.find(a.links[link]);
  return b && b->link ? b : nullptr;
}

const Body* World::Impl::art_link(const ArticulationRec& a, u16 link) const { return const_cast<Impl*>(this)->art_link(a, link); }

// ---------------------------------------------------------------------------------------------
// The API

ArticulationId World::Impl::add_articulation(const ArticulationDesc& d) {
  if (in_tick_ && !systems_phase_) return 0;
  return add_articulation_now(d);
}

ArticulationId World::Impl::add_articulation_now(const ArticulationDesc& d, ArticulationId want) {
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

  // (its id: the one it had, when it comes back and that one is free)
  const ArticulationId id = want != 0 && !art(want) ? want : next_art_;
  next_art_ = std::max(next_art_, id + 1);
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
    j.id = att_.next_joint++;
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
    j.supple = true;
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
    r.articulation = rec->id;
    const size_t k = insert_joint(r, j);
    fill_joint_end(att_.joints[k], false);
    fill_joint_end(att_.joints[k], true);
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
  // (in id order - the solver finds an articulation's rules by search - however it comes: one
  // back from the archive keeps its id, lower than those made since)
  const auto rt = std::lower_bound(rigid_.articulations.begin(), rigid_.articulations.end(), id,
                                   [](const ArticulationRules& r, ArticulationId v) { return r.id < v; });
  rigid_.articulations.insert(rt, std::move(R));
  // its drive: nothing yet
  rec->control = std::make_unique<ArticulationControl>();
  ArticulationControl& C = *rec->control;
  C.muscles.resize(d.joints.size());
  C.targets.resize(d.targets.size());
  C.force.resize(nl);
  C.torque.resize(nl);
  C.ghost.assign(nl, 0);
  const auto at = std::lower_bound(arts_.begin(), arts_.end(), id, [](const std::unique_ptr<ArticulationRec>& a, ArticulationId v) { return a->id < v; });
  arts_.insert(at, std::move(rec));
  return id;
}

bool World::Impl::remove_articulation(ArticulationId id) {
  if ((in_tick_ && !systems_phase_) || !art(id)) return false;
  drop_articulation(id, PieceEnd::Removed);
  return true;
}

void World::Impl::drop_articulation(ArticulationId id, PieceEnd end) {
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
    for (size_t k = 0; k < att_.joints.size(); ++k)
      if (att_.joints[k].id == jid) {
        att_.joints.erase(att_.joints.begin() + static_cast<std::ptrdiff_t>(k));
        rigid_.joints.erase(rigid_.joints.begin() + static_cast<std::ptrdiff_t>(k));
        break;
      }
  std::vector<u32> tids = a.targets;
  std::sort(tids.begin(), tids.end());
  rigid_.targets.erase(std::remove_if(rigid_.targets.begin(), rigid_.targets.end(), [&](const Target& t) { return std::binary_search(tids.begin(), tids.end(), t.id); }),
                       rigid_.targets.end());
  rigid_.articulations.erase(std::remove_if(rigid_.articulations.begin(), rigid_.articulations.end(), [&](const ArticulationRules& r) { return r.id == id; }),
                             rigid_.articulations.end());
  // its links (what rested on them falls - not when it is archived out of range: it comes back
  // under it)
  std::vector<i64> ids = a.links;
  std::sort(ids.begin(), ids.end());
  for (i64 bid : ids) {
    dead_loads_.erase(bid);
    if (const Body* b = rigid_.find(bid); b && end != PieceEnd::Unloaded) wake_around(*b);
  }
  rigid_.remove_if([&](const Body& b) { return std::binary_search(ids.begin(), ids.end(), b.id); });
  arts_.erase(it);
}

void World::Impl::clear_articulations() {
  // (the archived ones too: their records with them)
  for (const auto& [key, chunks] : strm_.archived_arts) strm_.archive->erase(key);
  strm_.archived_arts.clear();
  st_.archived_articulations = 0;
  std::vector<ArticulationId> ids;
  for (const auto& a : arts_) ids.push_back(a->id);
  for (ArticulationId id : ids) drop_articulation(id, PieceEnd::Removed);
  rigid_.targets.clear();
  rigid_.articulations.clear();
}

void World::Impl::articulations_out_of_world(f64 floor_z) {
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

std::vector<ArticulationId> World::Impl::articulations() const {
  std::vector<ArticulationId> out;
  out.reserve(arts_.size());
  for (const auto& a : arts_) out.push_back(a->id);
  return out;
}

ArticulationControl* World::Impl::articulation_control(ArticulationId id) {
  ArticulationRec* a = art(id);
  return a ? a->control.get() : nullptr;
}

bool World::Impl::articulation_state(ArticulationId id, ArticulationState* out) const {
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

const std::vector<u8>* World::Impl::articulation_data(ArticulationId id) const {
  const ArticulationRec* a = art(id);
  return a ? &a->data : nullptr;
}

bool World::Impl::set_articulation_data(ArticulationId id, std::vector<u8> data) {
  ArticulationRec* a = art(id);
  if (!a) return false;
  a->data = std::move(data);
  return true;
}

bool World::Impl::set_link(ArticulationId id, u16 link, const V3& pos, const Quat& rot, const V3& vel, const V3& ang) {
  ArticulationRec* a = art(id);
  Body* b = a ? art_link(*a, link) : nullptr;
  if (!b || (in_tick_ && !systems_phase_) || !in_range(pos) || !finite_q(rot) || !finite3(vel) || !finite3(ang)) return false;
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

bool World::Impl::add_link_velocity(ArticulationId id, u16 link, const V3& dv, const V3& dw) {
  ArticulationRec* a = art(id);
  Body* b = a ? art_link(*a, link) : nullptr;
  if (!b || !finite3(dv) || !finite3(dw) || b->link->kinematic) return false;
  b->v += dv;
  b->w += dw;
  wake_articulation(id);
  return true;
}

bool World::Impl::apply_link_impulse(ArticulationId id, u16 link, const V3& point, const V3& impulse) {
  ArticulationRec* a = art(id);
  Body* b = a ? art_link(*a, link) : nullptr;
  if (!b || !finite3(point) || !finite3(impulse) || b->link->kinematic) return false;
  b->v += impulse * b->inv_mass;
  b->w += b->inv_inertia_world() * cross(point - b->x, impulse);
  wake_articulation(id);
  return true;
}

bool World::Impl::set_link_mass(ArticulationId id, u16 link, f64 mass, const V3& inertia) {
  ArticulationRec* a = art(id);
  Body* b = a ? art_link(*a, link) : nullptr;
  if (!b || b->link->kinematic || !std::isfinite(mass) || mass <= 0 || !finite3(inertia) || inertia.x <= 0 || inertia.y <= 0 || inertia.z <= 0) return false;
  b->mass = mass;
  b->inv_mass = 1 / mass;
  b->inertia = M3{};
  b->inv_inertia = M3{};
  b->inertia.m[0] = inertia.x;
  b->inertia.m[4] = inertia.y;
  b->inertia.m[8] = inertia.z;
  b->inv_inertia.m[0] = 1 / inertia.x;
  b->inv_inertia.m[4] = 1 / inertia.y;
  b->inv_inertia.m[8] = 1 / inertia.z;
  wake_articulation(id);
  return true;
}

bool World::Impl::lose_link(ArticulationId id, u16 link, f64 mass_scale) {
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

bool World::Impl::wake_articulation(ArticulationId id) {
  ArticulationRec* a = art(id);
  if (!a) return false;
  for (u16 i = 0; i < a->links.size(); ++i)
    if (Body* b = art_link(*a, i)) rigid_.wake(*b);
  return true;
}

bool World::Impl::articulation_asleep(ArticulationId id) const {
  const ArticulationRec* a = art(id);
  if (!a) return false;
  for (u16 i = 0; i < a->links.size(); ++i)
    if (const Body* b = art_link(*a, i); b && !b->asleep) return false;
  return true;
}

i64 World::Impl::link_body(ArticulationId id, u16 link) const {
  const ArticulationRec* a = art(id);
  const Body* b = a ? art_link(*a, link) : nullptr;
  return b ? b->id : 0;
}

// ---------------------------------------------------------------------------------------------
// Each tick

void World::Impl::apply_articulation_controls() {
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
      const auto it = std::lower_bound(att_.joints.begin(), att_.joints.end(), jid, [](const JointRec& r, JointId v) { return r.id < v; });
      if (it == att_.joints.end() || it->id != jid) continue;
      // (its anchors, as its host moves them)
      {
        const size_t q = size_t(it - att_.joints.begin());
        if (k < C.anchor_parent.size() && finite3(C.anchor_parent[k]) && norm(C.anchor_parent[k]) < 8.0) att_.joints[q].a.point = rigid_.joints[q].a.p = C.anchor_parent[k];
        if (k < C.anchor_child.size() && finite3(C.anchor_child[k]) && norm(C.anchor_child[k]) < 8.0) att_.joints[q].b.point = rigid_.joints[q].b.p = C.anchor_child[k];
      }
      JointMuscle m = C.muscles[k];
      if (!finite_q(m.target) || !finite3(m.target_rate) || !finite3(m.feed) || !std::isfinite(m.stiffness) || !std::isfinite(m.damping) ||
          !std::isfinite(m.max_torque) || !std::isfinite(m.inertia))
        m = JointMuscle{};
      m.target = qnormalized(m.target);
      rigid_.joints[size_t(it - att_.joints.begin())].muscle = m;
    }
    for (size_t k = 0; k < a.targets.size() && k < C.targets.size(); ++k) {
      const u32 tid = a.targets[k];
      const auto it = std::lower_bound(rigid_.targets.begin(), rigid_.targets.end(), tid, [](const Target& t, u32 v) { return t.id < v; });
      if (it == rigid_.targets.end() || it->id != tid) continue;
      if (it->kind == Target::Kind::Point && k < C.target_local.size() && finite3(C.target_local[k]) && norm(C.target_local[k]) < 8.0) {
        if (norm2(it->local - C.target_local[k]) > 1e-18) {
          // Impulses accumulated at the heel must not warm-start the toe.
          it->imp = it->imp_d = V3{};
          it->step = 0.0;
        }
        it->local = C.target_local[k];
      }
      i64 reference = 0;
      if (k < C.target_reference.size() && k < C.target_reference_local.size() && C.target_reference[k] >= 0 &&
          size_t(C.target_reference[k]) < a.links.size() && finite3(C.target_reference_local[k]) && norm(C.target_reference_local[k]) < 8.0) {
        if (const Body* b = art_link(a, u16(C.target_reference[k])); b && b->id != it->body) reference = b->id;
      }
      if (reference != it->reference_body) {
        it->imp = it->imp_d = V3{};
        it->step = 0;
      }
      it->reference_body = reference;
      if (reference) it->reference_local = C.target_reference_local[k];
      TargetDrive d = C.targets[k];
      if (!finite3(d.pos) || !finite3(d.vel) || !finite_q(d.rot) || !finite3(d.up) || !std::isfinite(d.stiffness) || !std::isfinite(d.damping) ||
          !std::isfinite(d.max))
        d.on = false;
      d.rot = qnormalized(d.rot);
      it->drive = d;
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Records: the streaming archive and sessions

namespace {

using world_detail::put3;
using world_detail::put32;
using world_detail::putf;
using world_detail::putq;
using world_detail::Rd;

// v1: links (their state, flags), joints (anchors as they are now, limits, muscle), targets (and
// their drives), rules, control, host data
constexpr u8 kArticulationVersion = 2;

void put_drive(std::vector<u8>& out, const TargetDrive& d) {
  out.push_back(static_cast<u8>((d.on ? 1 : 0) | (d.axes[0] ? 2 : 0) | (d.axes[1] ? 4 : 0) | (d.axes[2] ? 8 : 0) | (d.tilt_only ? 16 : 0)));
  put3(out, d.pos);
  put3(out, d.vel);
  putq(out, d.rot);
  put3(out, d.up);
  putf(out, d.stiffness);
  putf(out, d.damping);
  putf(out, d.max);
}

bool read_drive(Rd& in, TargetDrive* d) {
  const u8 f = in.u8_();
  d->on = (f & 1) != 0;
  d->axes[0] = (f & 2) != 0;
  d->axes[1] = (f & 4) != 0;
  d->axes[2] = (f & 8) != 0;
  d->tilt_only = (f & 16) != 0;
  d->pos = in.v3();
  d->vel = in.v3();
  d->rot = in.q4();
  d->up = in.v3();
  d->stiffness = in.f64_();
  d->damping = in.f64_();
  d->max = in.f64_();
  return in.ok && (f & ~31u) == 0 && finite3(d->pos) && finite3(d->vel) && finite_q(d->rot) && finite3(d->up) && std::isfinite(d->stiffness) &&
         std::isfinite(d->damping) && std::isfinite(d->max);
}

void put_muscle(std::vector<u8>& out, const JointMuscle& m) {
  putq(out, m.target);
  put3(out, m.target_rate);
  putf(out, m.stiffness);
  putf(out, m.damping);
  putf(out, m.max_torque);
  putf(out, m.inertia);
  put3(out, m.feed);
}

bool read_muscle(Rd& in, JointMuscle* m) {
  m->target = in.q4();
  m->target_rate = in.v3();
  m->stiffness = in.f64_();
  m->damping = in.f64_();
  m->max_torque = in.f64_();
  m->inertia = in.f64_();
  m->feed = in.v3();
  return in.ok && finite_q(m->target) && finite3(m->target_rate) && std::isfinite(m->stiffness) && std::isfinite(m->damping) &&
         std::isfinite(m->max_torque) && std::isfinite(m->inertia) && finite3(m->feed);
}

}  // namespace

std::vector<u8> World::Impl::articulation_record(const ArticulationRec& a) const {
  std::vector<u8> out;
  out.push_back(kArticulationVersion);
  put32(out, a.id);
  put32(out, a.group);
  put32(out, a.tag);
  put32(out, static_cast<u32>(a.data.size()));
  out.insert(out.end(), a.data.begin(), a.data.end());
  const ArticulationControl& C = *a.control;
  // the links, as they are now
  put32(out, static_cast<u32>(a.links.size()));
  for (u16 i = 0; i < a.links.size(); ++i) {
    const Body* b = art_link(a, i);
    const LinkData* L = b ? b->link.get() : nullptr;
    if (!b || !L) {
      putf(out, -1.0);  // (never: its links go with it)
      continue;
    }
    putf(out, L->kinematic ? 0.0 : b->mass);
    put3(out, V3{b->inertia.m[0], b->inertia.m[4], b->inertia.m[8]});
    put3(out, b->x);
    putq(out, b->q);
    put3(out, b->v);
    put3(out, b->w);
    out.push_back(static_cast<u8>(L->spheres.size()));
    for (const BodySphere& sp : L->spheres) {
      put3(out, sp.c);
      putf(out, sp.r);
    }
    putf(out, L->friction);
    put3(out, L->long_axis);
    putf(out, L->twist_damping);
    out.push_back(static_cast<u8>((L->gone ? 1 : 0) | (b->asleep ? 2 : 0) | (i < C.ghost.size() && C.ghost[i] ? 4 : 0)));
  }
  // the joints: their anchors as they are now, their muscles
  put32(out, static_cast<u32>(a.joint_desc.size()));
  for (size_t k = 0; k < a.joint_desc.size(); ++k) {
    ArticulationJointDesc J = a.joint_desc[k];
    if (k < a.joints.size()) {
      const JointId jid = a.joints[k];
      const auto it = std::lower_bound(att_.joints.begin(), att_.joints.end(), jid, [](const JointRec& r, JointId v) { return r.id < v; });
      if (it != att_.joints.end() && it->id == jid) {
        J.anchor_parent = it->a.point;
        J.anchor_child = it->b.point;
      }
    }
    put32(out, J.parent);
    put32(out, J.child);
    out.push_back(static_cast<u8>(J.type));
    put3(out, J.anchor_parent);
    put3(out, J.anchor_child);
    putq(out, J.frame_parent);
    putq(out, J.frame_child);
    out.push_back(static_cast<u8>((J.swing_limited ? 1 : 0) | (J.twist_limited ? 2 : 0) | (J.hinge_limited ? 4 : 0)));
    for (f64 x : J.swing) putf(out, x);
    putf(out, J.twist_lower);
    putf(out, J.twist_upper);
    putf(out, J.hinge_lower);
    putf(out, J.hinge_upper);
    put_muscle(out, k < C.muscles.size() ? C.muscles[k] : JointMuscle{});
  }
  // the targets and their drives
  put32(out, static_cast<u32>(a.target_desc.size()));
  for (size_t k = 0; k < a.target_desc.size(); ++k) {
    const ArticulationTargetDesc& T = a.target_desc[k];
    put32(out, T.link);
    out.push_back(static_cast<u8>(T.kind));
    V3 local = T.local;
    if (k < a.targets.size()) {
      const u32 tid = a.targets[k];
      const auto it = std::lower_bound(rigid_.targets.begin(), rigid_.targets.end(), tid, [](const Target& t, u32 v) { return t.id < v; });
      if (it != rigid_.targets.end() && it->id == tid) local = it->local;
    }
    put3(out, local);
    put_drive(out, k < C.targets.size() ? C.targets[k] : TargetDrive{});
    put32(out, k < C.target_reference.size() ? u32(C.target_reference[k] + 1) : 0);
    put3(out, k < C.target_reference_local.size() ? C.target_reference_local[k] : V3{});
  }
  put32(out, static_cast<u32>(a.collide.size()));
  for (const auto& [x, y] : a.collide) {
    put32(out, x);
    put32(out, y);
  }
  putf(out, C.max_spin);
  putf(out, C.keep_linear);
  putf(out, C.keep_angular);
  out.push_back(static_cast<u8>((C.self_collide ? 1 : 0) | (C.can_sleep ? 2 : 0)));
  return out;
}

bool World::Impl::read_articulation_record(Rd& in, ArticulationSaved* out) const {
  const u8 version = in.u8_();
  if (version < 1 || version > kArticulationVersion) return false;
  ArticulationSaved& s = *out;
  s.id = in.u32_();
  s.desc.group = in.u32_();
  s.desc.tag = in.u32_();
  const u32 nd = in.u32_();
  if (!in.ok || !in.need(nd)) return false;
  s.desc.data.assign(in.b.begin() + static_cast<long>(in.p), in.b.begin() + static_cast<long>(in.p + nd));
  in.p += nd;
  const u32 nl = in.u32_();
  if (!in.ok || nl == 0 || nl > 256) return false;
  s.control.ghost.assign(nl, 0);
  for (u32 i = 0; i < nl; ++i) {
    LinkDesc L;
    L.mass = in.f64_();
    L.inertia = in.v3();
    L.pos = in.v3();
    L.rot = in.q4();
    L.vel = in.v3();
    L.ang = in.v3();
    const u8 ns = in.u8_();
    if (!in.ok || ns == 0 || ns > 16) return false;
    for (u8 q = 0; q < ns; ++q) {
      BodySphere sp;
      sp.c = in.v3();
      sp.r = in.f64_();
      L.spheres.push_back(sp);
    }
    L.friction = in.f64_();
    L.long_axis = in.v3();
    L.twist_damping = in.f64_();
    const u8 f = in.u8_();
    if (!in.ok || (f & ~7u) != 0) return false;
    s.gone.push_back((f & 1) != 0);
    s.asleep.push_back((f & 2) != 0);
    s.control.ghost[i] = (f & 4) != 0;
    s.desc.links.push_back(std::move(L));
  }
  const u32 nj = in.u32_();
  if (!in.ok || nj > 1024) return false;
  for (u32 k = 0; k < nj; ++k) {
    ArticulationJointDesc J;
    const u32 pa = in.u32_(), ch = in.u32_();
    J.parent = static_cast<u16>(pa);
    J.child = static_cast<u16>(ch);
    const u8 t = in.u8_();
    J.type = static_cast<JointType>(t);
    J.anchor_parent = in.v3();
    J.anchor_child = in.v3();
    J.frame_parent = in.q4();
    J.frame_child = in.q4();
    const u8 f = in.u8_();
    J.swing_limited = (f & 1) != 0;
    J.twist_limited = (f & 2) != 0;
    J.hinge_limited = (f & 4) != 0;
    for (f64& x : J.swing) x = in.f64_();
    J.twist_lower = in.f64_();
    J.twist_upper = in.f64_();
    J.hinge_lower = in.f64_();
    J.hinge_upper = in.f64_();
    JointMuscle m;
    if (!in.ok || pa >= nl || ch >= nl || (f & ~7u) != 0 || !read_muscle(in, &m)) return false;
    s.desc.joints.push_back(J);
    s.control.muscles.push_back(m);
    s.control.anchor_parent.push_back(J.anchor_parent);
    s.control.anchor_child.push_back(J.anchor_child);
  }
  const u32 nt = in.u32_();
  if (!in.ok || nt > 1024) return false;
  for (u32 k = 0; k < nt; ++k) {
    ArticulationTargetDesc T;
    const u32 l = in.u32_();
    T.link = static_cast<u16>(l);
    const u8 kind = in.u8_();
    T.kind = static_cast<Target::Kind>(kind);
    T.local = in.v3();
    TargetDrive d;
    if (!in.ok || l >= nl || kind > 1 || !finite3(T.local) || !read_drive(in, &d)) return false;
    s.desc.targets.push_back(T);
    s.control.targets.push_back(d);
    s.control.target_local.push_back(T.local);
    const u32 reference = version >= 2 ? in.u32_() : 0;
    const V3 local = version >= 2 ? in.v3() : V3{};
    if (!in.ok || reference > nl || !finite3(local)) return false;
    s.control.target_reference.push_back(i32(reference) - 1);
    s.control.target_reference_local.push_back(local);
  }
  const u32 nc = in.u32_();
  if (!in.ok || u64(nc) * 8 > in.b.size()) return false;
  for (u32 k = 0; k < nc; ++k) {
    const u32 x = in.u32_(), y = in.u32_();
    if (!in.ok || x >= nl || y >= nl) return false;
    s.desc.collide.emplace_back(static_cast<u16>(x), static_cast<u16>(y));
  }
  s.control.max_spin = in.f64_();
  s.control.keep_linear = in.f64_();
  s.control.keep_angular = in.f64_();
  const u8 cf = in.u8_();
  s.control.self_collide = (cf & 1) != 0;
  s.control.can_sleep = (cf & 2) != 0;
  s.control.force.assign(nl, V3{});
  s.control.torque.assign(nl, V3{});
  return in.ok && (cf & ~3u) == 0 && std::isfinite(s.control.max_spin) && std::isfinite(s.control.keep_linear) && std::isfinite(s.control.keep_angular);
}

ArticulationId World::Impl::restore_articulation(ArticulationSaved&& s) {
  const ArticulationId id = add_articulation_now(s.desc, s.id);
  if (id == 0) return 0;
  ArticulationRec* a = art(id);
  *a->control = std::move(s.control);
  for (u16 i = 0; i < a->links.size(); ++i) {
    Body* b = art_link(*a, i);
    if (!b) continue;
    // (its mass was saved as it was: a part lost keeps what it had left)
    if (i < s.gone.size() && s.gone[i]) b->link->gone = true;
    if (i < s.asleep.size() && s.asleep[i]) {
      b->asleep = true;
      b->was_asleep = true;
      b->v = b->v_pre = V3{};
      b->w = b->w_pre = V3{};
    }
  }
  WorldEvent ev;
  ev.kind = WorldEvent::Kind::ArticulationAdded;
  ev.id = id;
  if (const Body* b0 = art_link(*a, 0)) {
    ev.pos = b0->x;
    ev.rot = b0->q;
    ev.vel = b0->v;
  }
  ev.voxels = static_cast<i32>(a->links.size());
  events_.push_back(std::move(ev));
  return id;
}

void World::Impl::archive_articulation(ArticulationId id, const std::function<void(const Body&, const std::function<void(u64)>&)>& chunks_of) {
  ArticulationRec* a = art(id);
  if (!a || !strm_.source) return;
  const std::vector<u8> rec = articulation_record(*a);
  // (the chunks that must be resident for it to come back: its links')
  std::vector<u64> chunks;
  const IVec3 lo = strm_.lo, hi = strm_.hi;
  auto add = [&](u64 k) {
    const IVec3 c = unkey3(k);
    for (int q = 0; q < 3; ++q)
      if (c[q] < lo[q] || c[q] >= hi[q]) return;
    chunks.push_back(k);
  };
  for (u16 i = 0; i < a->links.size(); ++i)
    if (const Body* b = art_link(*a, i)) chunks_of(*b, add);
  std::sort(chunks.begin(), chunks.end());
  chunks.erase(std::unique(chunks.begin(), chunks.end()), chunks.end());
  const Body* b0 = art_link(*a, 0);
  const V3 at = b0 ? b0->x : V3{};
  const IVec3 home = chunk_of(world_detail::voxel_of(at, grid_.h));
  const u64 key = (3ull << 62) | (1ull << 61) | static_cast<u64>(id);
  archive_record(key, rec, region_of(key3(home[0], home[1], home[2])));
  if (!strm_.archive->has(key)) {
    ++st_.forgotten_articulations;  // (no room at all: it is gone)
  } else {
    strm_.archived_arts[key] = std::move(chunks);
    ++st_.archived_articulations;
  }
  drop_articulation(id, PieceEnd::Unloaded);
}

void World::Impl::restore_articulations() {
  if (strm_.archived_arts.empty()) return;
  std::vector<u64> ready;
  for (const auto& [key, chunks] : strm_.archived_arts)
    if (std::all_of(chunks.begin(), chunks.end(), [&](u64 c) { return chunk_known(c); })) ready.push_back(key);
  for (u64 key : ready) {
    strm_.archived_arts.erase(key);
    --st_.archived_articulations;
    const std::vector<u8> rec = strm_.archive->get(key);
    strm_.archive->erase(key);
    Rd in{rec};
    ArticulationSaved s;
    if (!read_articulation_record(in, &s) || in.p != rec.size()) continue;  // (checked when made: never)
    restore_articulation(std::move(s));
  }
}

void World::Impl::forget_articulation(u64 key) {
  const auto it = strm_.archived_arts.find(key);
  if (it == strm_.archived_arts.end()) return;
  strm_.archived_arts.erase(it);
  --st_.archived_articulations;
  ++st_.forgotten_articulations;
}

}  // namespace svx
