// structvox — kinematic bodies (docs/MOTION.md §1): rigid frames the host drives (doors, lifts,
// drawbridges, cranes' arms), and the grids that move with them.
//
// A body moves steadily through a tick: from where it was to where it is driven (or by the
// velocity it keeps). Its grids are placed at each substep's pose as static grids moving with the
// body's velocity field: pieces are pushed and carried by them, with no give. Its structures are
// solved in the body's frame (their geometry does not change as it moves): gravity turned into
// the frame and the frame's inertia (its acceleration, the centrifugal and Euler loads of its
// rotation) are their body loads. Rotations from poses use the bundled deterministic math.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "svx/base/rotation.hpp"
#include "svx/world/world.hpp"
#include "world_internal.hpp"

namespace svx {

using namespace world_detail;

namespace {

bool finite_q(const Quat& q) { return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w); }

Quat unit_q(const Quat& q) {
  Quat r = qnormalized(q);
  if (r.w < 0.0) r = Quat{-r.x, -r.y, -r.z, -r.w};
  return r;
}

// q turned further by the rotation vector r.
Quat turned(const Quat& q, const V3& r) {
  if (!(norm2(r) > 0.0)) return q;
  return unit_q(rotation_of(r) * q);
}

// The rotation a fraction t of the way from a to b (b on a's side).
Quat nlerp(const Quat& a, const Quat& b, f64 t) {
  const f64 u = 1.0 - t;
  return unit_q(Quat{a.x * u + b.x * t, a.y * u + b.y * t, a.z * u + b.z * t, a.w * u + b.w * t});
}

// A lattice placed in a moving frame is never the identity (its arithmetic must not take the
// world grid's short cut).
LatticeXf never_identity(LatticeXf X) {
  if (X.identity) {
    X.identity = false;
    X.R = M3::identity();
    X.Rt = M3::identity();
  }
  return X;
}

void box_of(const LatticeXf& xf, const V3& lo, const V3& hi, V3* wlo, V3* whi) {
  *wlo = V3{INFINITY, INFINITY, INFINITY};
  *whi = V3{-INFINITY, -INFINITY, -INFINITY};
  for (int c = 0; c < 8; ++c) {
    const V3 w = xf.to(V3{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z});
    for (int a = 0; a < 3; ++a) {
      (*wlo)[a] = std::min((*wlo)[a], w[a]);
      (*whi)[a] = std::max((*whi)[a], w[a]);
    }
  }
}

}  // namespace

i32 World::kin_slot_of(KinematicId id) const {
  if (id == 0) return 0;
  const auto it = kin_slots_.find(id);
  return it == kin_slots_.end() ? -1 : static_cast<i32>(it->second);
}

LatticeXf World::body_pose(u16 body) const {
  if (body == 0) return LatticeXf{};
  const KinState& K = *kins_[body];
  return never_identity(LatticeXf::make(K.x, K.q));
}

KinematicId World::add_kinematic(const Pose& pose, bool base) {
  if (in_tick_) return 0;
  return add_kinematic_impl(pose, base, 0);
}

KinematicId World::add_kinematic_impl(const Pose& pose, bool base, KinematicId want) {
  if (!in_range(pose.pos) || !finite_q(pose.rot)) return 0;
  const Quat& r = pose.rot;
  if (!(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w > 1e-12)) return 0;
  if (want != 0 && kin_slot_of(want) >= 0) return 0;
  if (kins_.empty()) kins_.emplace_back();  // (slot 0: the static world)
  u16 slot = 0;
  for (size_t k = 1; k < kins_.size(); ++k)
    if (!kins_[k]) {
      slot = static_cast<u16>(k);
      break;
    }
  if (slot == 0) {
    if (kins_.size() >= 0xFFFF) return 0;
    slot = static_cast<u16>(kins_.size());
    kins_.emplace_back();
  }
  auto K = std::make_unique<KinState>();
  K->id = want != 0 ? want : next_kin_;
  next_kin_ = std::max(next_kin_, K->id + 1);
  K->base = base;
  K->x = K->x0 = K->x1 = pose.pos;
  K->q = K->q0 = K->q1 = unit_q(pose.rot);
  const KinematicId id = K->id;
  kins_[slot] = std::move(K);
  kin_slots_[id] = slot;
  return id;
}

bool World::remove_kinematic(KinematicId id, bool release) {
  if (in_tick_) return false;
  const i32 ks = kin_slot_of(id);
  if (ks <= 0) return false;
  const u16 k = static_cast<u16>(ks);
  const std::vector<u16> grids = kins_[k]->grids;
  if (release) {
    // its voxels come loose (the drive holds nothing any more) and fall as one piece with its
    // velocity (it breaks where the pieces' checks find it in parts)
    std::vector<FragKey> frags;
    for (u16 g : grids) {
      if (!live(g)) continue;
      VoxelGrid& G = vg(g);
      std::vector<u64> keys;
      for (const auto& [key, c] : G.chunks())
        if (!c.uniform || vox_solid(c.value)) keys.push_back(key);
      std::sort(keys.begin(), keys.end());
      for (u64 key : keys) {
        const IVec3 cc = unkey3(key);
        const IVec3 b{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
        for (int i = 0; i < kChunkVox; ++i) {
          const IVec3 l = local_of(i);
          const IVec3 p{b[0] + l[0], b[1] + l[1], b[2] + l[2]};
          const Vox v = G.get(p);
          if (vox_anchored(v)) G.set(p, static_cast<Vox>(v & ~kAnchorBit));
        }
        const FragChunk& fc = frag_chunk(g, cc);
        for (i32 f = 0; f < static_cast<i32>(fc.frags.size()); ++f)
          if (fc.frags[size_t(f)].count > 0) frags.push_back(FragKey{key, f, g});
      }
    }
    const KinState& K = *kins_[k];
    if (!frags.empty()) make_body_from_world(frags, K.v, K.w, &K.x);
  }
  for (u16 g : grids)
    if (live(g)) remove_grid(id_of(g));
  if (kins_[k]->base) removed_kin_.push_back(id);
  if (release) announce_bodies();  // (in place of its grids, now)
  kin_slots_.erase(id);
  kins_[k].reset();
  while (kins_.size() > 1 && !kins_.back()) kins_.pop_back();
  return true;
}

bool World::drive_kinematic(KinematicId id, const Pose& target) {
  const i32 ks = kin_slot_of(id);
  if (ks <= 0 || !in_range(target.pos) || !finite_q(target.rot)) return false;
  const Quat& r = target.rot;
  if (!(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w > 1e-12)) return false;
  KinState& K = *kins_[size_t(ks)];
  K.driven = true;
  K.hold = false;
  K.tx = target.pos;
  K.tq = unit_q(target.rot);
  return true;
}

bool World::set_kinematic_velocity(KinematicId id, const V3& vel, const V3& ang) {
  const i32 ks = kin_slot_of(id);
  if (ks <= 0 || !finite3(vel) || !finite3(ang)) return false;
  KinState& K = *kins_[size_t(ks)];
  K.driven = false;
  K.hold = norm2(vel) > 0.0 || norm2(ang) > 0.0;
  K.cv = vel;
  K.cw = ang;
  return true;
}

bool World::kinematic(KinematicId id, KinematicState* out) const {
  const i32 ks = kin_slot_of(id);
  if (ks <= 0) return false;
  const KinState& K = *kins_[size_t(ks)];
  out->pose = Pose{K.x, K.q};
  out->vel = K.v;
  out->ang = K.w;
  out->grids.clear();
  for (u16 g : K.grids)
    if (live(g)) out->grids.push_back(id_of(g));
  return true;
}

std::vector<KinematicId> World::kinematics() const {
  std::vector<KinematicId> out;
  for (const auto& [id, k] : kin_slots_) out.push_back(id);
  std::sort(out.begin(), out.end());
  return out;
}

KinematicId World::grid_body(GridId id) const {
  const i32 s = slot_of(id);
  if (s < 0) return 0;
  const u16 b = grids_[size_t(s)]->body;
  return b == 0 ? 0u : kins_[b]->id;
}

bool World::kinematics_moving() const {
  for (size_t k = 1; k < kins_.size(); ++k)
    if (kins_[k] && (norm2(kins_[k]->v) > 0.0 || norm2(kins_[k]->w) > 0.0)) return true;
  return false;
}

void World::begin_kinematics(f64 dt) {
  for (size_t k = 1; k < kins_.size(); ++k) {
    if (!kins_[k]) continue;
    KinState& K = *kins_[k];
    const V3 v0 = K.v, w0 = K.w;
    K.x0 = K.x;
    K.q0 = K.q;
    if (K.driven) {
      K.x1 = K.tx;
      K.q1 = K.tq;
      K.driven = false;
    } else if (K.hold) {
      K.x1 = K.x0 + K.cv * dt;
      K.q1 = turned(K.q0, K.cw * dt);
    } else {
      K.x1 = K.x0;
      K.q1 = K.q0;
    }
    // (the short way round)
    if (K.q0.x * K.q1.x + K.q0.y * K.q1.y + K.q0.z * K.q1.z + K.q0.w * K.q1.w < 0.0) K.q1 = Quat{-K.q1.x, -K.q1.y, -K.q1.z, -K.q1.w};
    K.v = (K.x1 - K.x0) * (1.0 / dt);
    K.w = K.hold ? K.cw : rotation_vector(K.q1 * conj(K.q0)) * (1.0 / dt);
    K.a = (K.v - v0) * (1.0 / dt);
    K.alpha = (K.w - w0) * (1.0 / dt);
    // (what rests on it or is in its way takes part in contacts: awake)
    if (norm2(K.v) > 0.0 || norm2(K.w) > 0.0) {
      const LatticeXf P1 = never_identity(LatticeXf::make(K.x1, K.q1));
      for (u16 g : K.grids) {
        if (!live(g) || !gs(g).any) continue;
        const GridState& st = gs(g);
        const V3 m{2 * st.g.h, 2 * st.g.h, 2 * st.g.h};
        rigid_.wake_box(st.lo - m, st.hi + m);
        V3 lo, hi;
        box_of(compose(P1, st.local), st.llo, st.lhi, &lo, &hi);
        rigid_.wake_box(lo - m, hi + m);
      }
    }
  }
}

void World::place_kinematics(f64 s) {
  for (size_t k = 1; k < kins_.size(); ++k) {
    if (!kins_[k]) continue;
    KinState& K = *kins_[k];
    if (s >= 1.0) {
      K.x = K.x1;
      K.q = K.q1;
    } else {
      K.x = K.x0 + (K.x1 - K.x0) * s;
      K.q = nlerp(K.q0, K.q1, s);
    }
    const LatticeXf P = body_pose(static_cast<u16>(k));
    for (u16 g : K.grids) {
      if (!live(g)) continue;
      GridState& st = gs(g);
      st.xf = never_identity(compose(P, st.local));
      if (st.any) box_of(st.xf, st.llo, st.lhi, &st.lo, &st.hi);
    }
  }
}

void World::restore_kinematic(u16 k, const KinState& saved) {
  KinState& K = *kins_[k];
  K.x = K.x0 = K.x1 = saved.x;
  K.q = K.q0 = K.q1 = saved.q;  // (as saved: bit for bit)
  K.v = saved.v;
  K.w = saved.w;
  K.a = K.alpha = V3{};
  K.driven = saved.driven;
  K.tx = saved.tx;
  K.tq = saved.tq;
  K.hold = saved.hold;
  K.cv = saved.cv;
  K.cw = saved.cw;
  place_kinematics_of(k);
}

void World::place_kinematics_of(u16 k) {
  const LatticeXf P = body_pose(k);
  for (u16 g : kins_[k]->grids) {
    if (!live(g)) continue;
    GridState& st = gs(g);
    st.xf = never_identity(compose(P, st.local));
    if (st.any) box_of(st.xf, st.llo, st.lhi, &st.lo, &st.hi);
  }
}

V3 World::body_velocity(u16 body, const V3& X) const {
  if (body == 0) return V3{};
  const KinState& K = *kins_[body];
  return K.v + cross(K.w, X - K.x);
}

V3 World::body_angular(u16 body) const { return body == 0 ? V3{} : kins_[body]->w; }

V3 World::frame_accel(u16 body, const V3& c) const {
  const V3 g{0.0, 0.0, -cfg_.rigid.gravity};
  if (body == 0) return g;
  const KinState& K = *kins_[body];
  // (in the frame: gravity less the frame's acceleration, the Euler and centrifugal loads)
  const V3 G = rotate_inv(K.q, g - K.a);
  const V3 w = rotate_inv(K.q, K.w), al = rotate_inv(K.q, K.alpha);
  return G - cross(al, c) - cross(w, cross(w, c));
}

void World::to_body(u16 body, V3* F, V3* p) const {
  if (body == 0) return;
  const KinState& K = *kins_[body];
  *F = rotate_inv(K.q, *F);
  *p = rotate_inv(K.q, *p - K.x);
}

std::vector<u8> World::kinematic_entries() const {
  // u32 count, the level's bodies removed (ids); u32 count, then per body (ascending ids): id |
  // flags (1: a level's, 2: it keeps a velocity, 4: it is driven to a pose) | pose (7 f64) | the
  // last tick's velocity (6 f64) | [the velocity it keeps (6 f64)] | [the pose it is driven to (7 f64)]
  std::vector<u8> out;
  auto put32 = [&](u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
  };
  auto putf = [&](f64 x) {
    u64 u;
    std::memcpy(&u, &x, 8);
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>(u >> (8 * i)));
  };
  std::vector<KinematicId> removed = removed_kin_;
  std::sort(removed.begin(), removed.end());
  removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
  put32(static_cast<u32>(removed.size()));
  for (KinematicId id : removed) put32(id);
  const std::vector<KinematicId> ids = kinematics();
  put32(static_cast<u32>(ids.size()));
  for (KinematicId id : ids) {
    const KinState& K = *kins_[size_t(kin_slot_of(id))];
    put32(id);
    out.push_back(static_cast<u8>((K.base ? 1 : 0) | (K.hold ? 2 : 0) | (K.driven ? 4 : 0)));
    for (f64 x : {K.x.x, K.x.y, K.x.z, K.q.x, K.q.y, K.q.z, K.q.w, K.v.x, K.v.y, K.v.z, K.w.x, K.w.y, K.w.z}) putf(x);
    if (K.hold)
      for (f64 x : {K.cv.x, K.cv.y, K.cv.z, K.cw.x, K.cw.y, K.cw.z}) putf(x);
    if (K.driven)
      for (f64 x : {K.tx.x, K.tx.y, K.tx.z, K.tq.x, K.tq.y, K.tq.z, K.tq.w}) putf(x);
  }
  return out;
}

}  // namespace svx
