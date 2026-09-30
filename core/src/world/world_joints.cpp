// structvox — joints (docs/MOTION.md §1): what their ends hold on to, and how they follow it.
//
// An end holds on to a point of the world, or a voxel (of a grid, or of a piece: kept as its
// grid's voxel). A voxel's end goes where the voxel goes: into the piece
// it breaks off in (make_body_from_world), into the part of a piece it stays with when the piece
// splits (flush_body_changes); when the voxel is gone the joint lets go. Each substep the
// solver's ends (phys/joint.hpp) are filled from the anchors at the bodies' poses now; the
// solver's forces load the structures and pieces the ends hold on to.
#include <algorithm>
#include <cmath>

#include "svx/world/world.hpp"
#include "world_internal.hpp"

namespace svx {

using namespace world_detail;

namespace {

JointDrive sane_drive(JointDrive d) {
  d.max = std::max(0.0, d.max);
  d.period = std::max(1e-3, d.period);
  d.stiffness = std::clamp(d.stiffness, 0.0, 1e3);
  return d;
}

void squares(const V3& n, V3* t1, V3* t2) {
  *t1 = normalized(std::abs(n.x) < 0.57 ? cross(n, V3{1, 0, 0}) : cross(n, V3{0, 1, 0}));
  *t2 = cross(n, *t1);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The API

JointId World::Impl::add_joint(const JointDesc& d) {
  if (in_tick_) return 0;
  return add_joint_impl(d, 0);
}

JointId World::Impl::add_joint_impl(const JointDesc& d, JointId want) {
  if (want != 0 && std::any_of(att_.joints.begin(), att_.joints.end(), [&](const JointRec& r) { return r.id == want; })) return 0;
  if (!in_range(d.a.point) || !in_range(d.b.point) || !finite3(d.axis) || !(norm2(d.axis) > 1e-12)) return 0;
  for (f64 x : {d.length, d.lower, d.upper, d.break_force, d.break_torque, d.stiffness, d.damping, d.drive.speed, d.drive.max, d.drive.target,
                d.drive.target2, d.drive.period, d.drive.phase, d.drive.stiffness})
    if (!std::isfinite(x)) return 0;
  const V3 axis = normalized(d.axis);
  V3 ref, t2;
  squares(axis, &ref, &t2);
  // each end: its anchor, and its lattice's rotation in the world now
  JointRec r;
  Quat Q[2];
  const JointAnchor* src[2] = {&d.a, &d.b};
  JointRec::End* dst[2] = {&r.a, &r.b};
  for (int e = 0; e < 2; ++e) {
    const JointAnchor& A = *src[e];
    JointRec::End& E = *dst[e];
    E.kind = A.kind;
    switch (A.kind) {
      case JointAnchor::Kind::World:
        E.point = A.point;
        E.axis = axis;
        E.ref = ref;
        Q[e] = Quat{};
        break;
      case JointAnchor::Kind::Grid: {
        const i32 s = A.id <= 0xFFFFFFFFull ? slot_of(static_cast<GridId>(A.id)) : -1;
        if (s < 0) return 0;
        const LatticeXf& X = gs(static_cast<u16>(s)).xf;
        const VoxelGrid& G = vg(static_cast<u16>(s));
        const V3 L = X.from(A.point);
        f64 d2 = 0.0;
        if (!nearest_solid(L, G.h, [&](const IVec3& p) { return vox_solid(G.get(p)); }, &E.voxel, &d2)) return 0;
        E.grid = static_cast<GridId>(A.id);
        E.point = L;
        E.axis = X.dir_from(axis);
        E.ref = X.dir_from(ref);
        Q[e] = X.q;
        break;
      }
      case JointAnchor::Kind::Link: {
        // (a point of a link: in its body frame, relative to its centre of mass)
        const Body* b = rigid_.find(static_cast<i64>(A.id));
        if (!b || !b->link) return 0;
        E.piece = b->id;
        E.point = rotate_inv(b->q, A.point - b->x);
        E.axis = rotate_inv(b->q, axis);
        E.ref = rotate_inv(b->q, ref);
        Q[e] = b->q;
        break;
      }
      case JointAnchor::Kind::Piece: {
        const Body* b = rigid_.find(static_cast<i64>(A.id));
        if (!b || !b->announced) return 0;
        // (its voxel there, in whichever of its shapes is nearest)
        i32 best = -1;
        f64 bd = 0.0;
        IVec3 bv{0, 0, 0};
        for (size_t k = 0; k < b->shapes.size(); ++k) {
          const BodyShape& S = b->shapes[k];
          IVec3 v;
          f64 d2 = 0.0;
          if (!nearest_solid(b->world_to_lattice(k, A.point), S.h, [&](const IVec3& p) { return shape_solid(S, p); }, &v, &d2)) continue;
          if (best < 0 || d2 < bd) {
            best = static_cast<i32>(k);
            bd = d2;
            bv = v;
          }
        }
        if (best < 0) return 0;
        const BodyShape& S = b->shapes[size_t(best)];
        E.kind = JointAnchor::Kind::Grid;
        E.grid = S.grid;
        E.voxel = bv;
        E.point = b->world_to_lattice(size_t(best), A.point);
        E.piece = b->id;
        E.shape = best;
        const Quat Ql = b->lattice_rot(size_t(best));
        E.axis = rotate_inv(Ql, axis);
        E.ref = rotate_inv(Ql, ref);
        Q[e] = Ql;
        break;
      }
    }
  }
  Joint j;
  j.id = want != 0 ? want : att_.next_joint;
  j.type = d.type;
  if (d.type == JointType::Distance) {
    const f64 L = d.length >= 0.0 ? d.length : norm(d.b.point - d.a.point);
    j.max_length = L;
    j.min_length = d.rope ? 0.0 : L;
    j.stiffness = std::max(0.0, d.stiffness);
    j.damping = std::max(0.0, d.damping);
  }
  j.limited = d.limited;
  j.lower = d.lower;
  j.upper = d.upper;
  j.drive = sane_drive(d.drive);
  j.break_force = std::max(0.0, d.break_force);
  j.break_torque = std::max(0.0, d.break_torque);
  j.break_angle = std::isfinite(d.break_angle) ? std::max(0.0, d.break_angle) : 0.0;
  j.collide = d.collide;
  j.latch = d.type == JointType::Hinge && std::isfinite(d.latch) ? std::max(0.0, d.latch) : 0.0;
  j.latched = j.latch > 0.0;
  j.rel = conj(Q[0]) * Q[1];
  r.id = j.id;
  // (in id order: the solver's order, the same on every run)
  const size_t k = insert_joint(r, j);
  if (!fill_joint_end(att_.joints[k], false) || !fill_joint_end(att_.joints[k], true)) {
    att_.joints.erase(att_.joints.begin() + static_cast<std::ptrdiff_t>(k));
    rigid_.joints.erase(rigid_.joints.begin() + static_cast<std::ptrdiff_t>(k));
    return 0;
  }
  if (want == 0) ++att_.next_joint;
  wake_joint(k);  // (what hangs on it moves now)
  return j.id;
}

size_t World::Impl::insert_joint(const JointRec& r, const Joint& j) {
  const auto it = std::lower_bound(att_.joints.begin(), att_.joints.end(), r.id, [](const JointRec& x, JointId v) { return x.id < v; });
  const size_t k = static_cast<size_t>(it - att_.joints.begin());
  att_.joints.insert(it, r);
  rigid_.joints.insert(rigid_.joints.begin() + static_cast<std::ptrdiff_t>(k), j);
  return k;
}

bool World::Impl::remove_joint(JointId id) {
  if (in_tick_) return false;
  for (size_t k = 0; k < att_.joints.size(); ++k)
    if (att_.joints[k].id == id) {
      wake_joint(k);
      att_.joints.erase(att_.joints.begin() + static_cast<std::ptrdiff_t>(k));
      rigid_.joints.erase(rigid_.joints.begin() + static_cast<std::ptrdiff_t>(k));
      return true;
    }
  return false;
}

bool World::Impl::set_joint_drive(JointId id, const JointDrive& d) {
  for (f64 x : {d.speed, d.max, d.target, d.target2, d.period, d.phase, d.stiffness})
    if (!std::isfinite(x)) return false;
  for (size_t k = 0; k < rigid_.joints.size(); ++k) {
    Joint& j = rigid_.joints[k];
    if (j.id != id || j.broken) continue;
    j.drive = sane_drive(d);
    wake_joint(k);
    return true;
  }
  return false;
}

bool World::Impl::set_joint_limits(JointId id, bool on, f64 lower, f64 upper) {
  if (!std::isfinite(lower) || !std::isfinite(upper)) return false;
  for (size_t k = 0; k < rigid_.joints.size(); ++k) {
    Joint& j = rigid_.joints[k];
    if (j.id != id || j.broken) continue;
    j.limited = on;
    j.lower = lower;
    j.upper = upper;
    wake_joint(k);
    return true;
  }
  return false;
}

void World::Impl::wake_joint(size_t k) {
  for (const JointRec::End* E : {&att_.joints[k].a, &att_.joints[k].b})
    if (E->piece > 0)
      if (Body* b = rigid_.find(E->piece)) rigid_.wake(*b);
}

bool World::Impl::joint(JointId id, JointState* out) const {
  for (size_t k = 0; k < att_.joints.size(); ++k) {
    const Joint& j = rigid_.joints[k];
    if (j.id != id || j.broken) continue;
    auto world_of = [&](const JointEnd& e) {
      if (e.body == 0) return e.p;
      for (const auto& bp : rigid_.bodies)
        if (bp->id == e.body) return bp->x + rotate(bp->q, e.p);
      return e.p;
    };
    out->type = j.type;
    out->a = world_of(j.a);
    out->b = world_of(j.b);
    out->force = j.force;
    out->torque = j.torque;
    out->value = j.value;
    out->piece_a = att_.joints[k].a.piece > 0 ? att_.joints[k].a.piece : 0;
    out->piece_b = att_.joints[k].b.piece > 0 ? att_.joints[k].b.piece : 0;
    out->latched = j.latched;
    return true;
  }
  return false;
}

std::vector<i64> World::Impl::joined_pieces(i64 piece) const {
  std::vector<i64> out;
  if (piece <= 0) return out;
  for (size_t k = 0; k < att_.joints.size(); ++k) {
    if (rigid_.joints[k].broken) continue;
    const i64 a = att_.joints[k].a.piece, b = att_.joints[k].b.piece;
    if (a == piece && b > 0 && b != piece) out.push_back(b);
    else if (b == piece && a > 0 && a != piece) out.push_back(a);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<JointId> World::Impl::joints() const {
  std::vector<JointId> out;
  for (size_t k = 0; k < rigid_.joints.size(); ++k)
    if (!rigid_.joints[k].broken && att_.joints[k].articulation == 0) out.push_back(rigid_.joints[k].id);
  return out;
}

// ---------------------------------------------------------------------------------------------
// The solver's ends

bool World::Impl::fill_joint_end(JointRec& r, bool b_end) {
  const size_t k = static_cast<size_t>(&r - att_.joints.data());
  JointRec::End& E = b_end ? r.b : r.a;
  JointEnd& e = b_end ? rigid_.joints[k].b : rigid_.joints[k].a;
  e = JointEnd{};
  e.axis = E.axis;
  e.ref = E.ref;
  switch (E.kind) {
    case JointAnchor::Kind::World:
      e.p = E.point;
      return true;
    case JointAnchor::Kind::Link: {
      // (a link's point and frame, in its body frame: it moves with the link)
      const Body* b = rigid_.find(E.piece);
      if (!b || !b->link) return false;
      e.body = b->id;
      e.p = E.point;
      return true;
    }
    case JointAnchor::Kind::Grid:
    case JointAnchor::Kind::Piece:
      break;
  }
  if (E.piece < 0) return false;  // (lost with the part of a piece it was in)
  if (E.piece == 0) {
    const i32 s = slot_of(E.grid);
    if (s < 0) return false;
    const u16 g = static_cast<u16>(s);
    if (!vox_solid(vg(g).get(E.voxel))) return false;
    const GridState& st = gs(g);
    e.p = st.xf.to(E.point);
    e.q = st.xf.q;
    return true;
  }
  const Body* b = rigid_.find(E.piece);
  if (!b) return false;
  // (its shape of the voxel's grid: the one it was in, else found again)
  i32 sk = E.shape;
  if (sk < 0 || sk >= static_cast<i32>(b->shapes.size()) || b->shapes[size_t(sk)].grid != E.grid || !shape_solid(b->shapes[size_t(sk)], E.voxel)) {
    sk = -1;
    for (size_t q = 0; q < b->shapes.size() && sk < 0; ++q)
      if (b->shapes[q].grid == E.grid && shape_solid(b->shapes[q], E.voxel)) sk = static_cast<i32>(q);
    if (sk < 0) return false;
    E.shape = sk;
  }
  const BodyShape& S = b->shapes[size_t(sk)];
  e.body = b->id;
  e.p = S.xf.to(E.point) - b->com;
  e.q = S.xf.q;
  return true;
}

void World::Impl::update_joint_ends() {
  for (size_t k = 0; k < att_.joints.size(); ++k) {
    Joint& j = rigid_.joints[k];
    if (j.broken) continue;
    if (!fill_joint_end(att_.joints[k], false) || !fill_joint_end(att_.joints[k], true)) {
      // (an end lost its hold: the voxel, the grid, the piece or the body is gone)
      j.broken = true;
      j.force = V3{};
    }
  }
}

void World::Impl::reap_joints() {
  for (size_t k = att_.joints.size(); k-- > 0;) {
    const Joint& j = rigid_.joints[k];
    if (!j.broken) continue;
    // where: between its ends
    V3 at;
    i32 n = 0;
    for (const JointEnd* e : {&j.a, &j.b}) {
      if (e->body == 0) {
        at += e->p;
        ++n;
      } else if (const Body* b = rigid_.find(e->body)) {
        at += b->x + rotate(b->q, e->p);
        ++n;
      }
    }
    drop_joint(k, norm(j.force), n ? at * (1.0 / n) : at);
  }
}

void World::Impl::drop_joint(size_t k, f64 force, const V3& at) {
  WorldEvent ev;
  ev.kind = WorldEvent::Kind::JointBroken;
  ev.id = att_.joints[k].id;
  ev.pos = at;
  ev.strength = force;
  events_.push_back(ev);
  wake_joint(k);
  att_.joints.erase(att_.joints.begin() + static_cast<std::ptrdiff_t>(k));
  rigid_.joints.erase(rigid_.joints.begin() + static_cast<std::ptrdiff_t>(k));
}

// ---------------------------------------------------------------------------------------------
// Following the voxels

void World::Impl::joints_to_piece(const Body& b) {
  for (size_t k = 0; k < att_.joints.size(); ++k) {
    for (int e = 0; e < 2; ++e) {
      JointRec::End& E = e ? att_.joints[k].b : att_.joints[k].a;
      if (E.kind != JointAnchor::Kind::Grid || E.piece != 0) continue;
      for (size_t q = 0; q < b.shapes.size(); ++q)
        if (b.shapes[q].grid == E.grid && shape_solid(b.shapes[q], E.voxel)) {
          E.piece = b.id;
          E.shape = static_cast<i32>(q);
          if (!rigid_.joints[k].broken && !fill_joint_end(att_.joints[k], e != 0)) rigid_.joints[k].broken = true;
          break;
        }
    }
  }
}

void World::Impl::joints_follow_splits() {
  if (att_.joints.empty() || (pw_.pending_retire.empty() && pw_.split_kept.empty())) return;
  for (size_t k = 0; k < att_.joints.size(); ++k)
    for (int e = 0; e < 2; ++e) {
      JointRec::End& E = e ? att_.joints[k].b : att_.joints[k].a;
      if (E.piece <= 0 || E.kind == JointAnchor::Kind::Link) continue;  // (a link never splits)
      if (!std::binary_search(pw_.pending_retire.begin(), pw_.pending_retire.end(), E.piece)) {
        // (a piece split in place: an end on a part that came off follows it)
        if (!std::binary_search(pw_.split_kept.begin(), pw_.split_kept.end(), E.piece)) continue;
        const Body* b = rigid_.find(E.piece);
        if (b && E.shape >= 0 && size_t(E.shape) < b->shapes.size() && shape_solid(b->shapes[size_t(E.shape)], E.voxel)) continue;
      }
      // (the part its voxel is in; none: it went to dust, it was carved)
      i64 to = -1;
      i32 shape = -1;
      for (const auto& c : pw_.pending_add) {
        if (c->origin != E.piece) continue;
        for (size_t q = 0; q < c->shapes.size() && to < 0; ++q)
          if (c->shapes[q].grid == E.grid && shape_solid(c->shapes[q], E.voxel)) {
            to = c->id;
            shape = static_cast<i32>(q);
          }
        if (to >= 0) break;
      }
      E.piece = to;
      E.shape = shape;
      if (to < 0) rigid_.joints[k].broken = true;
    }
}

bool World::Impl::jointed(const std::vector<FragKey>& members) {
  for (const JointRec& r : att_.joints)
    for (const JointRec::End* E : {&r.a, &r.b}) {
      if (E->kind != JointAnchor::Kind::Grid || E->piece != 0) continue;
      const i32 s = slot_of(E->grid);
      FragKey f;
      if (s < 0 || !frag_at(GVox{E->voxel, static_cast<u16>(s)}, &f)) continue;
      for (const FragKey& m : members)
        if (m.grid == f.grid && m.chunk == f.chunk && m.idx == f.idx) return true;
    }
  return false;
}

// ---------------------------------------------------------------------------------------------
// Loads

void World::Impl::joint_structure_loads(f64 dt_sub) {
  (void)dt_sub;
  for (size_t k = 0; k < att_.joints.size(); ++k) {
    const Joint& j = rigid_.joints[k];
    if (j.broken || (norm2(j.force) == 0.0 && norm2(j.torque) == 0.0)) continue;
    for (int e = 0; e < 2; ++e) {
      const JointRec::End& E = e ? att_.joints[k].b : att_.joints[k].a;
      if (E.kind != JointAnchor::Kind::Grid || E.piece != 0) continue;
      const i32 s = slot_of(E.grid);
      if (s < 0) continue;
      // (b receives the force and torque; a the opposite)
      const V3 F = e ? j.force : j.force * -1.0, M = e ? j.torque : j.torque * -1.0;
      if (!finite3(F) || !finite3(M)) continue;
      const GVox wv{E.voxel, static_cast<u16>(s)};
      FragKey f;
      if (!frag_at(wv, &f)) continue;
      const i64 o = owner_of(f);
      Structure* st = o ? structure(o) : nullptr;
      if (!st) {
        if (norm(F) > 4.0 * cfg_.load_trigger_abs) seeds_.push_back(wv);
        continue;
      }
      const i32 i = st->node(f);
      if (i < 0) continue;
      const V3 Fl = F, pl = (e ? j.b : j.a).p;
      const V3 Mt = cross(pl - st->P.nodes[size_t(i)].c, Fl) + M;
      f64* a = &st->acc[6 * size_t(i)];
      a[0] += Fl.x;
      a[1] += Fl.y;
      a[2] += Fl.z;
      a[3] += Mt.x;
      a[4] += Mt.y;
      a[5] += Mt.z;
      const f64 mag = norm(Fl);
      if (mag > st->peak_mag[size_t(i)]) {
        st->peak_mag[size_t(i)] = mag;
        f64* pk = &st->peak[6 * size_t(i)];
        pk[0] = Fl.x;
        pk[1] = Fl.y;
        pk[2] = Fl.z;
        pk[3] = Mt.x;
        pk[4] = Mt.y;
        pk[5] = Mt.z;
      }
    }
  }
}

void World::Impl::joint_piece_forces(std::vector<std::vector<PointForce>>& per, std::vector<f64>& fsum) const {
  for (size_t k = 0; k < att_.joints.size(); ++k) {
    const Joint& j = rigid_.joints[k];
    if (j.broken || norm2(j.force) == 0.0) continue;
    for (int e = 0; e < 2; ++e) {
      const JointRec::End& E = e ? att_.joints[k].b : att_.joints[k].a;
      if (E.piece <= 0 || E.shape < 0) continue;
      const auto it = std::lower_bound(rigid_.bodies.begin(), rigid_.bodies.end(), E.piece,
                                       [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
      if (it == rigid_.bodies.end() || (*it)->id != E.piece) continue;
      const size_t bi = static_cast<size_t>(it - rigid_.bodies.begin());
      if (bi >= per.size()) continue;
      const Body& b = **it;
      if (E.shape >= static_cast<i32>(b.shapes.size())) continue;
      const BodyShape& S = b.shapes[size_t(E.shape)];
      const i32 vi = S.index(E.voxel);
      if (vi < 0 || S.frag.empty()) continue;
      const i32 fr = static_cast<i32>(S.frag[size_t(vi)]) - 1;
      const V3 F = e ? j.force : j.force * -1.0;
      per[bi].push_back({fr, F, b.x + rotate(b.q, (e ? j.b : j.a).p)});
      fsum[bi] += norm(F);
    }
  }
}

}  // namespace svx
