// structvox — wheels (docs/MOTION.md §7): what they hang from, how they follow it, what they load,
// and what is left when one comes off.
//
// A wheel hangs from a voxel of its carrier (a grid's, going with the piece it becomes; a
// piece's, going with the part it is in when the piece breaks), like a joint's end (JointRec::End,
// world_joints.cpp). Each substep its solver mount is filled from that anchor at the carrier's
// pose. When the voxel is gone (crushed in a crash, shot away) or the wheel's force passes its
// breaking strength, it comes off: a wheel-shaped piece of its material where it was, moving as
// it moved (WheelDetached).
#include <algorithm>
#include <cmath>

#include "svx/base/dmath.hpp"
#include "svx/world/world.hpp"
#include "world_internal.hpp"

namespace svx {

using namespace world_detail;

namespace {

// The rotation whose matrix has columns x, y, z (orthonormal, right-handed): sqrt only.
Quat quat_from_basis(const V3& x, const V3& y, const V3& z) {
  const f64 m00 = x.x, m11 = y.y, m22 = z.z;
  const f64 tr = m00 + m11 + m22;
  Quat q;
  if (tr > 0.0) {
    const f64 s = std::sqrt(tr + 1.0) * 2.0;
    q.w = 0.25 * s;
    q.x = (y.z - z.y) / s;
    q.y = (z.x - x.z) / s;
    q.z = (x.y - y.x) / s;
  } else if (m00 > m11 && m00 > m22) {
    const f64 s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
    q.w = (y.z - z.y) / s;
    q.x = 0.25 * s;
    q.y = (y.x + x.y) / s;
    q.z = (z.x + x.z) / s;
  } else if (m11 > m22) {
    const f64 s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
    q.w = (z.x - x.z) / s;
    q.x = (y.x + x.y) / s;
    q.y = 0.25 * s;
    q.z = (z.y + y.z) / s;
  } else {
    const f64 s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
    q.w = (x.y - y.x) / s;
    q.x = (z.x + x.z) / s;
    q.y = (z.y + y.z) / s;
    q.z = 0.25 * s;
  }
  return qnormalized(q);
}

// A wheel's frame in the world at its carrier's pose: its axis down, its axle (steered) and the
// way it rolls; its mount and centre.
struct WheelFrame {
  V3 d, a, f, M, C;
};
WheelFrame wheel_frame(const Wheel& w, const Body& A) {
  WheelFrame F;
  const M3 R = to_matrix(A.q);
  F.d = normalized(R * w.down);
  const V3 a0 = normalized(R * w.axle), u = F.d * -1.0;
  const f64 cs = dm::cos(w.steer), sn = dm::sin(w.steer);
  F.a = normalized(a0 * cs + cross(u, a0) * sn + u * (dot(u, a0) * (1.0 - cs)));
  F.f = normalized(cross(F.d, F.a));
  F.M = A.x + R * w.p;
  F.C = F.M + F.d * w.length;
  return F;
}

// Its orientation: x the way it rolls, y its axle, z up; turned by its spin's angle about y.
Quat wheel_rot(const WheelFrame& F, f64 angle) {
  const Quat base = quat_from_basis(F.f, F.a, F.d * -1.0);
  const Quat spin{0.0, dm::sin(0.5 * angle), 0.0, dm::cos(0.5 * angle)};
  return qnormalized(base * spin);
}

constexpr u32 kNoGrid = 0xFFFFFFFFu;  // (a wheel piece's shape came from no grid)

}  // namespace

// ---------------------------------------------------------------------------------------------
// The API

WheelId World::Impl::add_wheel(const WheelDesc& d) {
  if (in_tick_) return 0;
  return add_wheel_impl(d, 0);
}

WheelId World::Impl::add_wheel_impl(const WheelDesc& d, WheelId want) {
  if (want != 0 && std::any_of(att_.wheels.begin(), att_.wheels.end(), [&](const WheelRec& r) { return r.id == want; })) return 0;
  if (!in_range(d.mount.point) || !finite3(d.down) || !finite3(d.axle) || !(norm2(d.down) > 1e-12) || !(norm2(d.axle) > 1e-12)) return 0;
  for (f64 x : {d.radius, d.width, d.rest, d.travel, d.stiffness, d.damping, d.inertia, d.grip, d.break_force})
    if (!std::isfinite(x) || x < 0.0) return 0;
  if (!(d.radius > 0.01) || d.radius > 5.0 || d.width > 5.0 || d.rest > 10.0) return 0;
  const V3 down = normalized(d.down);
  // (the axle square to the suspension's axis)
  V3 axle = d.axle - down * dot(d.axle, down);
  if (!(norm2(axle) > 1e-12)) return 0;
  axle = normalized(axle);
  WheelRec r;
  JointRec::End& E = r.mount;
  switch (d.mount.kind) {
    case JointAnchor::Kind::World:
    case JointAnchor::Kind::Link:
      return 0;  // (a wheel hangs from something that moves, of voxels)
    case JointAnchor::Kind::Grid: {
      const i32 s = d.mount.id <= 0xFFFFFFFFull ? slot_of(static_cast<GridId>(d.mount.id)) : -1;
      if (s < 0) return 0;
      const LatticeXf& X = gs(static_cast<u16>(s)).xf;
      const VoxelGrid& G = vg(static_cast<u16>(s));
      const V3 L = X.from(d.mount.point);
      f64 d2 = 0.0;
      if (!nearest_solid(L, G.h, [&](const IVec3& p) { return vox_solid(G.get(p)); }, &E.voxel, &d2)) return 0;
      E.kind = JointAnchor::Kind::Grid;
      E.grid = static_cast<GridId>(d.mount.id);
      E.point = L;
      E.axis = X.dir_from(down);
      E.ref = X.dir_from(axle);
      break;
    }
    case JointAnchor::Kind::Piece: {
      const Body* b = rigid_.find(static_cast<i64>(d.mount.id));
      if (!b || !b->announced) return 0;
      i32 best = -1;
      f64 bd = 0.0;
      IVec3 bv{0, 0, 0};
      for (size_t k = 0; k < b->shapes.size(); ++k) {
        const BodyShape& S = b->shapes[k];
        IVec3 v;
        f64 d2 = 0.0;
        if (!nearest_solid(b->world_to_lattice(k, d.mount.point), S.h, [&](const IVec3& p) { return shape_solid(S, p); }, &v, &d2)) continue;
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
      E.point = b->world_to_lattice(size_t(best), d.mount.point);
      E.piece = b->id;
      E.shape = best;
      const Quat Ql = b->lattice_rot(size_t(best));
      E.axis = rotate_inv(Ql, down);
      E.ref = rotate_inv(Ql, axle);
      break;
    }
  }
  Wheel w;
  w.id = want != 0 ? want : att_.next_wheel;
  w.radius = d.radius;
  w.width = d.width;
  w.rest = d.rest;
  w.travel = std::min(d.travel, d.rest);
  w.stiffness = d.stiffness;
  w.damping = d.damping;
  w.inertia = std::max(1e-3, d.inertia);
  w.grip = d.grip;
  w.break_force = d.break_force;
  w.length = d.rest;
  r.id = w.id;
  r.group = d.group;
  r.tag = d.tag;
  r.material = static_cast<int>(d.material) < kMaxMaterials ? d.material : MaterialId::Steel;
  const size_t k = insert_wheel(r, w);
  if (!fill_wheel_mount(k)) {
    att_.wheels.erase(att_.wheels.begin() + static_cast<std::ptrdiff_t>(k));
    rigid_.wheels.erase(rigid_.wheels.begin() + static_cast<std::ptrdiff_t>(k));
    return 0;
  }
  if (want == 0) ++att_.next_wheel;
  if (Body* b = rigid_.find(rigid_.wheels[k].body)) rigid_.wake(*b);
  return w.id;
}

size_t World::Impl::insert_wheel(const WheelRec& r, const Wheel& w) {
  const auto it = std::lower_bound(att_.wheels.begin(), att_.wheels.end(), r.id, [](const WheelRec& x, WheelId v) { return x.id < v; });
  const size_t k = static_cast<size_t>(it - att_.wheels.begin());
  att_.wheels.insert(it, r);
  rigid_.wheels.insert(rigid_.wheels.begin() + static_cast<std::ptrdiff_t>(k), w);
  return k;
}

bool World::Impl::remove_wheel(WheelId id) {
  if (in_tick_) return false;
  for (size_t k = 0; k < att_.wheels.size(); ++k)
    if (att_.wheels[k].id == id) {
      if (Body* b = rigid_.find(rigid_.wheels[k].body)) rigid_.wake(*b);
      att_.wheels.erase(att_.wheels.begin() + static_cast<std::ptrdiff_t>(k));
      rigid_.wheels.erase(rigid_.wheels.begin() + static_cast<std::ptrdiff_t>(k));
      return true;
    }
  return false;
}

bool World::Impl::set_wheel_input(WheelId id, f64 drive, f64 brake, f64 steer) {
  if (!std::isfinite(drive) || !std::isfinite(brake) || !std::isfinite(steer)) return false;
  for (Wheel& w : rigid_.wheels) {
    if (w.id != id || w.broken) continue;
    drive = std::clamp(drive, -1e6, 1e6);
    brake = std::clamp(brake, 0.0, 1e6);
    steer = std::clamp(steer, -1.5, 1.5);
    const bool change = drive != w.drive || brake != w.brake || steer != w.steer;
    w.drive = drive;
    w.brake = brake;
    w.steer = steer;
    // (a carrier at rest wakes when it is driven, released or steered)
    if (change)
      if (Body* b = rigid_.find(w.body); b && b->asleep && (drive != 0.0 || steer != 0.0 || brake == 0.0)) rigid_.wake(*b);
    return true;
  }
  return false;
}

bool World::Impl::wheel(WheelId id, WheelState* out) const {
  for (size_t k = 0; k < att_.wheels.size(); ++k) {
    const Wheel& w = rigid_.wheels[k];
    if (w.id != id || w.broken) continue;
    const WheelRec& r = att_.wheels[k];
    WheelState s;
    s.radius = w.radius;
    s.width = w.width;
    s.length = w.length;
    s.compression = w.travel > 0.0 ? std::clamp((w.rest - w.length) / w.travel, 0.0, 1.0) : 0.0;
    s.steer = w.steer;
    s.spin = w.spin;
    s.angle = w.angle;
    s.drive = w.drive;
    s.brake = w.brake;
    s.contact = w.contact;
    s.point = w.point;
    s.normal = w.normal;
    s.ground_piece = w.contact ? w.ground_body : 0;
    s.material = w.contact ? static_cast<int>(w.ground_mat) : -1;
    s.load = w.load;
    s.force = w.force;
    s.slip_long = w.slip_long;
    s.slip_lat = w.slip_lat;
    s.group = r.group;
    s.tag = r.tag;
    if (const Body* b = w.body != 0 ? rigid_.find(w.body) : nullptr) {
      const WheelFrame F = wheel_frame(w, *b);
      s.piece = b->id;
      s.mount = F.M;
      s.centre = F.C;
      s.rot = wheel_rot(F, w.angle);
    } else if (r.mount.piece == 0) {
      // (not a piece yet: its grid's voxel, where it hangs at full droop)
      const i32 sl = slot_of(r.mount.grid);
      if (sl >= 0) {
        const LatticeXf& X = gs(static_cast<u16>(sl)).xf;
        s.mount = X.to(r.mount.point);
        const V3 d = normalized(X.dir_to(r.mount.axis)), a = normalized(X.dir_to(r.mount.ref));
        s.centre = s.mount + d * w.length;
        s.rot = quat_from_basis(normalized(cross(d, a)), a, d * -1.0);
      }
    }
    *out = s;
    return true;
  }
  return false;
}

std::vector<WheelId> World::Impl::wheels() const {
  std::vector<WheelId> out;
  for (const Wheel& w : rigid_.wheels)
    if (!w.broken) out.push_back(w.id);
  return out;
}

bool World::Impl::set_piece_max_speed(i64 piece, f64 max_speed) {
  Body* b = rigid_.find(piece);
  if (!b || !std::isfinite(max_speed) || max_speed < 0.0) return false;
  b->max_speed = std::min(max_speed, 1000.0);
  return true;
}

// ---------------------------------------------------------------------------------------------
// The solver's mounts

bool World::Impl::fill_wheel_mount(size_t k) {
  JointRec::End& E = att_.wheels[k].mount;
  Wheel& w = rigid_.wheels[k];
  w.body = 0;
  if (E.piece < 0) return false;  // (lost with the part of a piece it was in)
  if (E.piece == 0) {
    // (its grid's voxel, not a piece yet: it hangs there, waiting)
    const i32 s = slot_of(E.grid);
    return s >= 0 && vox_solid(vg(static_cast<u16>(s)).get(E.voxel));
  }
  const Body* b = rigid_.find(E.piece);
  if (!b) return false;
  i32 sk = E.shape;
  if (sk < 0 || sk >= static_cast<i32>(b->shapes.size()) || b->shapes[size_t(sk)].grid != E.grid || !shape_solid(b->shapes[size_t(sk)], E.voxel)) {
    sk = -1;
    for (size_t q = 0; q < b->shapes.size() && sk < 0; ++q)
      if (b->shapes[q].grid == E.grid && shape_solid(b->shapes[q], E.voxel)) sk = static_cast<i32>(q);
    if (sk < 0) return false;
    E.shape = sk;
  }
  const BodyShape& S = b->shapes[size_t(sk)];
  w.body = b->id;
  w.p = S.xf.to(E.point) - b->com;
  w.down = normalized(S.xf.dir_to(E.axis));
  w.axle = normalized(S.xf.dir_to(E.ref));
  return true;
}

void World::Impl::update_wheel_mounts() {
  for (size_t k = 0; k < att_.wheels.size(); ++k) {
    Wheel& w = rigid_.wheels[k];
    if (w.broken) continue;
    if (!fill_wheel_mount(k)) {
      w.broken = true;  // (its mount voxel is gone: it comes off)
      w.force = V3{};
      continue;
    }
    // (where it is: what comes off becomes a piece there)
    if (const Body* b = w.body != 0 ? rigid_.find(w.body) : nullptr) {
      const WheelFrame F = wheel_frame(w, *b);
      WheelRec& r = att_.wheels[k];
      r.placed = true;
      r.centre = F.C;
      r.rot = wheel_rot(F, w.angle);
      r.vel = b->v + cross(b->w, F.C - b->x);
      r.ang = b->w + F.a * w.spin;
    }
  }
}

void World::Impl::reap_wheels() {
  for (size_t k = att_.wheels.size(); k-- > 0;) {
    const Wheel& w = rigid_.wheels[k];
    if (!w.broken) continue;
    const WheelRec& r = att_.wheels[k];
    WorldEvent ev;
    ev.kind = WorldEvent::Kind::WheelDetached;
    ev.id = r.id;
    ev.parent = r.mount.piece > 0 ? r.mount.piece : 0;
    ev.pos = r.centre;
    ev.vel = r.vel;
    ev.ang = r.ang;
    ev.rot = r.rot;
    ev.strength = norm(w.force);
    if (r.placed) ev.voxels = static_cast<i32>(make_wheel_body(w));
    events_.push_back(ev);
    att_.wheels.erase(att_.wheels.begin() + static_cast<std::ptrdiff_t>(k));
    rigid_.wheels.erase(rigid_.wheels.begin() + static_cast<std::ptrdiff_t>(k));
  }
}

i64 World::Impl::make_wheel_body(const Wheel& w) {
  size_t k = 0;
  while (k < rigid_.wheels.size() && rigid_.wheels[k].id != w.id) ++k;
  if (k == att_.wheels.size()) return 0;
  const WheelRec& r = att_.wheels[k];
  // a disc of rubber on its rim: its axle along its lattice's y, a few voxels across
  const f64 hw = std::clamp(w.radius / 3.5, 0.25 * grid_.h, grid_.h);
  const i32 n = std::max(1, static_cast<i32>(std::floor(w.radius / hw + 0.5)));
  const i32 m = std::max(0, static_cast<i32>(std::floor(0.5 * w.width / hw)));
  auto b = std::make_unique<Body>();
  b->shapes.resize(1);
  BodyShape& S = b->shapes[0];
  S.grid = kNoGrid;
  S.h = hw;
  S.lo = {-n, -m, -n};
  S.dim = {2 * n + 1, 2 * m + 1, 2 * n + 1};
  const size_t cells = size_t(S.dim[0]) * size_t(S.dim[1]) * size_t(S.dim[2]);
  S.vox.assign(cells, kAir);
  S.frag.assign(cells, 0);
  S.brk.assign(cells, 0);
  const f64 rr = (w.radius / hw) * (w.radius / hw);
  for (i32 x = -n; x <= n; ++x)
    for (i32 z = -n; z <= n; ++z) {
      if (static_cast<f64>(x * x + z * z) > rr) continue;
      for (i32 y = -m; y <= m; ++y) {
        const i32 i = S.index({x, y, z});
        S.vox[size_t(i)] = make_vox(r.material, false);
        S.frag[size_t(i)] = 1;
      }
    }
  b->frags.resize(1);
  b->frags[0].mat = r.material;
  refragment_body(*b);
  if (b->count < cfg_.min_body_voxels) return 0;
  body_refresh(*b, grid_.h, cfg_.rigid.max_points);
  if (!(b->mass > 0.0)) return 0;
  b->id = next_id_++;
  b->q = r.rot;
  b->x = r.centre + rotate(r.rot, b->com);
  b->v = r.vel;
  b->w = r.ang;
  b->v_pre = b->v;
  b->w_pre = b->w;
  b->parent = r.mount.piece > 0 ? r.mount.piece : 0;
  b->graph_dirty = true;
  b->refresh_box();
  const i64 id = b->id;
  rigid_.add(std::move(b));
  return id;
}

// ---------------------------------------------------------------------------------------------
// Following the voxels

void World::Impl::wheels_to_piece(const Body& b) {
  for (size_t k = 0; k < att_.wheels.size(); ++k) {
    JointRec::End& E = att_.wheels[k].mount;
    if (E.piece != 0) continue;
    for (size_t q = 0; q < b.shapes.size(); ++q)
      if (b.shapes[q].grid == E.grid && shape_solid(b.shapes[q], E.voxel)) {
        E.piece = b.id;
        E.shape = static_cast<i32>(q);
        if (!rigid_.wheels[k].broken && !fill_wheel_mount(k)) rigid_.wheels[k].broken = true;
        break;
      }
  }
}

void World::Impl::wheels_follow_splits() {
  if (att_.wheels.empty() || (pw_.pending_retire.empty() && pw_.split_kept.empty())) return;
  for (size_t k = 0; k < att_.wheels.size(); ++k) {
    JointRec::End& E = att_.wheels[k].mount;
    if (E.piece <= 0) continue;
    if (!std::binary_search(pw_.pending_retire.begin(), pw_.pending_retire.end(), E.piece)) {
      // (a piece split in place: a mount on a part that came off follows it)
      if (!std::binary_search(pw_.split_kept.begin(), pw_.split_kept.end(), E.piece)) continue;
      const Body* b = rigid_.find(E.piece);
      if (b && E.shape >= 0 && size_t(E.shape) < b->shapes.size() && shape_solid(b->shapes[size_t(E.shape)], E.voxel)) continue;
    }
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
    if (to < 0) rigid_.wheels[k].broken = true;
  }
}

// ---------------------------------------------------------------------------------------------
// Loads

void World::Impl::wheel_structure_loads(f64 dt_sub) {
  (void)dt_sub;
  const f64 imp = par_.impact;
  // (a wheel rolling onto a fragment is a load that moves, not a blow: it counts as an impact
  // - a load case solved at once - only beyond 2.5 x its share of its carrier's weight: a landing,
  // a step struck at speed. Otherwise its load creeps, and is solved again as loads do.)
  std::vector<std::pair<i64, i32>> on;  // (carrier, its wheels on the ground)
  for (const Wheel& w : rigid_.wheels)
    if (!w.broken && w.contact && w.body != 0) {
      auto it = std::lower_bound(on.begin(), on.end(), std::make_pair(w.body, 0));
      if (it == on.end() || it->first != w.body) it = on.insert(it, {w.body, 0});
      ++it->second;
    }
  for (const Wheel& w : rigid_.wheels) {
    if (w.broken || !w.contact || w.ground_body != 0 || !live(w.ground_grid)) continue;
    const V3 F = w.force * -imp;  // (on the ground)
    if (!finite3(F) || norm2(F) == 0.0) continue;
    const GVox wv{IVec3{w.ground_voxel[0], w.ground_voxel[1], w.ground_voxel[2]}, w.ground_grid};
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
    const V3 M = cross(w.point - st->P.nodes[size_t(i)].c, F);
    f64* a = &st->acc[6 * size_t(i)];
    a[0] += F.x;
    a[1] += F.y;
    a[2] += F.z;
    a[3] += M.x;
    a[4] += M.y;
    a[5] += M.z;
    const f64 mag = norm(F);
    f64 share = 0.0;
    if (const Body* b = rigid_.find(w.body)) {
      const auto it = std::lower_bound(on.begin(), on.end(), std::make_pair(w.body, 0));
      if (it != on.end() && it->first == w.body) share = imp * b->mass * rigid_.par.gravity / it->second;
    }
    if (mag > 2.5 * share && mag > st->peak_mag[size_t(i)]) {
      st->peak_mag[size_t(i)] = mag;
      f64* pk = &st->peak[6 * size_t(i)];
      pk[0] = F.x;
      pk[1] = F.y;
      pk[2] = F.z;
      pk[3] = M.x;
      pk[4] = M.y;
      pk[5] = M.z;
    }
  }
}

void World::Impl::wheel_piece_forces(std::vector<std::vector<PointForce>>& per, std::vector<f64>& fsum) const {
  auto index_of = [&](i64 id) -> i64 {
    const auto it = std::lower_bound(rigid_.bodies.begin(), rigid_.bodies.end(), id, [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
    return (it != rigid_.bodies.end() && (*it)->id == id) ? static_cast<i64>(it - rigid_.bodies.begin()) : -1;
  };
  for (size_t k = 0; k < att_.wheels.size(); ++k) {
    const Wheel& w = rigid_.wheels[k];
    if (w.broken || !w.contact || w.body == 0 || norm2(w.force) == 0.0) continue;
    // the carrier: through its mount's voxel
    const JointRec::End& E = att_.wheels[k].mount;
    const i64 ia = index_of(w.body);
    if (ia >= 0 && size_t(ia) < per.size() && E.shape >= 0) {
      const Body& A = *rigid_.bodies[size_t(ia)];
      if (E.shape < static_cast<i32>(A.shapes.size())) {
        const BodyShape& S = A.shapes[size_t(E.shape)];
        const i32 vi = S.index(E.voxel);
        if (vi >= 0 && !S.frag.empty() && S.frag[size_t(vi)] > 0) {
          per[size_t(ia)].push_back({static_cast<i32>(S.frag[size_t(vi)]) - 1, w.force, A.x + rotate(A.q, w.p)});
          fsum[size_t(ia)] += norm(w.force);
        }
      }
    }
    // what it stands on: at its contact's voxel (ground_grid: that piece's shape)
    if (w.ground_body != 0) {
      const i64 ib = index_of(w.ground_body);
      if (ib < 0 || size_t(ib) >= per.size()) continue;
      const Body& B = *rigid_.bodies[size_t(ib)];
      if (w.ground_grid >= B.shapes.size()) continue;
      const BodyShape& S = B.shapes[w.ground_grid];
      const i32 vi = S.index(IVec3{w.ground_voxel[0], w.ground_voxel[1], w.ground_voxel[2]});
      if (vi < 0 || S.frag.empty() || S.frag[size_t(vi)] == 0) continue;
      per[size_t(ib)].push_back({static_cast<i32>(S.frag[size_t(vi)]) - 1, w.force * -1.0, w.point});
      fsum[size_t(ib)] += norm(w.force);
    }
  }
}

}  // namespace svx
