#include "svx/anim/physics/rigid.hpp"

#include <algorithm>

namespace svx::anim {

namespace {

// The most a joint limit turns a joint back in one substep (rad).
constexpr f64 kLimitStep = 0.025;
// How fast an overlap with another body is undone (m/s): an approach is met in full, a body found
// deep inside another leaves it without being flung.
constexpr f64 kDeepSpeed = 3.0;
constexpr f64 kMaxSpeed = 60.0;
constexpr f64 kMaxSpin = 80.0;

void clamp_length(V3& v, f64 max) {
  const f64 l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (l <= max) return;
  const f64 k = max / l;
  v = v * k;
}

void clamp_change(V3& v, const V3& v0, f64 max) {
  const V3 d = v - v0;
  const f64 l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  if (l <= max) return;
  const f64 k = max / l;
  v = v0 + d * k;
}

bool finite_body(const RigidBody& b) { return std::isfinite(b.x.x + b.x.y + b.x.z) && std::isfinite(b.q.x + b.q.y + b.q.z + b.q.w); }

}  // namespace

// ---- bodies ----------------------------------------------------------------------------------

RigidBody::RigidBody(f64 m, const V3& inertia, const V3& x0, const Quat& q0, std::vector<Sphere> sph)
    : x(x0), q(q0), mass(m), inv_mass(m > 0.0 ? 1.0 / m : 0.0), spheres(std::move(sph)) {
  inv_i = V3{inertia.x > 0.0 ? 1.0 / inertia.x : 0.0, inertia.y > 0.0 ? 1.0 / inertia.y : 0.0, inertia.z > 0.0 ? 1.0 / inertia.z : 0.0};
  update_inertia();
}

void RigidBody::lose() {
  if (gone) return;
  gone = true;
  const f64 k = 0.05;
  mass *= k;
  inv_mass /= k;
  inv_i = inv_i * (1.0 / k);
  update_inertia();
}

void RigidBody::update_inertia() {
  const f64 X = q.x, Y = q.y, Z = q.z, W = q.w;
  // rotation matrix columns
  const f64 r00 = 1 - 2 * (Y * Y + Z * Z), r01 = 2 * (X * Y - Z * W), r02 = 2 * (X * Z + Y * W);
  const f64 r10 = 2 * (X * Y + Z * W), r11 = 1 - 2 * (X * X + Z * Z), r12 = 2 * (Y * Z - X * W);
  const f64 r20 = 2 * (X * Z - Y * W), r21 = 2 * (Y * Z + X * W), r22 = 1 - 2 * (X * X + Y * Y);
  const f64 a = inv_i.x, b = inv_i.y, c = inv_i.z;
  iw[0] = r00 * r00 * a + r01 * r01 * b + r02 * r02 * c;
  iw[1] = r10 * r10 * a + r11 * r11 * b + r12 * r12 * c;
  iw[2] = r20 * r20 * a + r21 * r21 * b + r22 * r22 * c;
  iw[3] = r00 * r10 * a + r01 * r11 * b + r02 * r12 * c;
  iw[4] = r00 * r20 * a + r01 * r21 * b + r02 * r22 * c;
  iw[5] = r10 * r20 * a + r11 * r21 * b + r12 * r22 * c;
}

V3 RigidBody::inv_inertia_mul(const V3& t) const {
  return V3{iw[0] * t.x + iw[3] * t.y + iw[4] * t.z, iw[3] * t.x + iw[1] * t.y + iw[5] * t.z, iw[4] * t.x + iw[5] * t.y + iw[2] * t.z};
}

f64 RigidBody::inv_mass_at(const V3& r, const V3& n) const {
  const V3 c{r.y * n.z - r.z * n.y, r.z * n.x - r.x * n.z, r.x * n.y - r.y * n.x};
  const V3 t = inv_inertia_mul(c);
  return inv_mass + c.x * t.x + c.y * t.y + c.z * t.z;
}

f64 RigidBody::inv_mass_rot(const V3& n) const {
  const V3 t = inv_inertia_mul(n);
  return n.x * t.x + n.y * t.y + n.z * t.z;
}

void RigidBody::apply_pos(const V3& p, const V3& r) {
  if (inv_mass == 0.0) return;
  x.x += p.x * inv_mass;
  x.y += p.y * inv_mass;
  x.z += p.z * inv_mass;
  V3 turn = inv_inertia_mul(V3{r.y * p.z - r.z * p.y, r.z * p.x - r.x * p.z, r.x * p.y - r.y * p.x});
  // A deeply trapped distal link is recovered over several solves. Rotating it
  // all the way out in one anchor correction caused visible 90-degree snaps.
  clamp_length(turn, .15);
  rotate(turn);
}

void RigidBody::apply_rot(const V3& l) {
  if (inv_mass == 0.0) return;
  V3 turn = inv_inertia_mul(l);
  clamp_length(turn, .15);
  rotate(turn);
}

void RigidBody::rotate(const V3& a) {
  const f64 qx = q.x, qy = q.y, qz = q.z, qw = q.w;
  f64 X = qx + 0.5 * (a.x * qw + a.y * qz - a.z * qy);
  f64 Y = qy + 0.5 * (a.y * qw + a.z * qx - a.x * qz);
  f64 Z = qz + 0.5 * (a.z * qw + a.x * qy - a.y * qx);
  f64 W = qw + 0.5 * (-a.x * qx - a.y * qy - a.z * qz);
  const f64 l = 1.0 / std::sqrt(X * X + Y * Y + Z * Z + W * W);
  q = Quat{X * l, Y * l, Z * l, W * l};
}

void RigidBody::apply_impulse(const V3& j, const V3& r) {
  if (inv_mass == 0.0) return;
  v.x += j.x * inv_mass;
  v.y += j.y * inv_mass;
  v.z += j.z * inv_mass;
  w += inv_inertia_mul(V3{r.y * j.z - r.z * j.y, r.z * j.x - r.x * j.z, r.x * j.y - r.y * j.x});
}

V3 RigidBody::point_velocity(const V3& local) const {
  const V3 r = rotate_by(q, local);
  return V3{v.x + w.y * r.z - w.z * r.y, v.y + w.z * r.x - w.x * r.z, v.z + w.x * r.y - w.y * r.x};
}

Joint::Joint(RigidBody* a_, RigidBody* b_, const JointOptions& o)
    : a(a_), b(b_), kind(o.kind), anchor_a(o.anchor_a), anchor_b(o.anchor_b), frame_a(o.frame_a), frame_b(o.frame_b), swing(o.swing), twist(o.twist), hinge(o.hinge) {}

// ---- the system ------------------------------------------------------------------------------

RigidSystem::RigidSystem(const CollisionWorld* c, const RigidSystemOptions& o) : collision(c), gravity(o.gravity), max_substep(o.max_substep), margin(o.margin) {}

RigidBody* RigidSystem::add(std::unique_ptr<RigidBody> b) {
  b->index = static_cast<i32>(bodies.size());
  bodies.push_back(std::move(b));
  frame_start_.assign(bodies.size(), V3{});
  still_at_.assign(bodies.size(), V3{});
  anchored_ = false;
  return bodies.back().get();
}

Joint* RigidSystem::add_joint(std::unique_ptr<Joint> j) {
  joints.push_back(std::move(j));
  return joints.back().get();
}

Attachment* RigidSystem::attach(RigidBody* body, const V3& local) {
  attachments.push_back(std::make_unique<Attachment>(body, local));
  return attachments.back().get();
}

Orienter* RigidSystem::orienter(RigidBody* body) {
  orienters.push_back(std::make_unique<Orienter>(body));
  return orienters.back().get();
}

void RigidSystem::wake() {
  asleep = false;
  still = 0.0;
  anchored_ = false;
}

void RigidSystem::step(f64 dt) {
  if (dt <= 0.0 || external) return;
  if (asleep) {
    for (auto& b : bodies) {
      b->force = V3{};
      b->torque = V3{};
    }
    return;
  }
  // (never let a runaway body hang the host: velocities are bounded, a broken state reset)
  for (auto& bp : bodies) {
    RigidBody& b = *bp;
    if (!finite_body(b)) {
      b.x = b.px;
      b.q = b.pq;
      if (!finite_body(b)) {
        b.x = V3{};
        b.q = Quat{};
      }
      b.v = V3{};
      b.w = V3{};
    }
    clamp_length(b.v, kMaxSpeed);
    clamp_length(b.w, kMaxSpin);
  }
  const i32 n = std::max(1, static_cast<i32>(std::ceil(dt / max_substep - 1e-6)));
  const f64 h = dt / n;
  substep = h;
  for (size_t i = 0; i < bodies.size(); ++i) frame_start_[i] = bodies[i]->x;
  find_contacts(dt);
  find_pairs(dt);
  for (auto& b : bodies) {
    b->impact = 0.0;
    b->bumped = 0.0;
  }
  for (i32 s = 0; s < n; ++s) substep_once(h);
  for (auto& b : bodies) {
    b->force = V3{};
    b->torque = V3{};
  }
  // what the obstacles' bodies get back (equal and opposite, handed on by the host)
  reactions.clear();
  for (size_t i = 0; i < contact_count_; ++i) {
    const WorldContact& k = contacts_[i];
    if (k.other_owner < 0 || k.took <= 0.0) continue;
    const V3 p = k.body->point(k.c);
    const f64 took = std::min(k.took, 60.0);
    Reaction r;
    r.owner = k.other_owner;
    r.part = k.other_part;
    r.j = V3{-k.n.x * took, -k.n.y * took, -k.n.z * took};
    r.at = V3{p.x - k.n.x * k.r, p.y - k.n.y * k.r, p.z - k.n.z * k.r};
    reactions.push_back(r);
  }
  note_motion(dt, frame_start_);
}

void RigidSystem::note_motion(f64 dt, const std::vector<V3>& start) {
  if (still_at_.size() != bodies.size()) still_at_.assign(bodies.size(), V3{});
  // still: nothing has gone further than `still_distance` from where it was when the stillness
  // began (a body at rest may jitter on its contacts, but it goes nowhere)
  f64 fast = 0.0, far = 0.0;
  for (size_t i = 0; i < bodies.size() && i < start.size(); ++i) {
    const RigidBody& b = *bodies[i];
    const V3 ds = b.x - start[i];
    const f64 v = hypot3(ds.x, ds.y, ds.z) / dt;
    if (v > fast) fast = v;
    // (light parts - a hand, a foot - may shift a little more before they count)
    const V3 da = b.x - still_at_[i];
    const f64 d = hypot3(da.x, da.y, da.z) / (b.mass < 2.0 ? 3.0 : 1.0);
    if (d > far) far = d;
  }
  last_speed = fast;
  if (far < still_distance && anchored_) {
    still += dt;
  } else {
    still = 0.0;
    anchored_ = true;
    for (size_t i = 0; i < bodies.size(); ++i) still_at_[i] = bodies[i]->x;
  }
}

bool RigidSystem::try_sleep(f64 after) {
  if (external) {
    may_sleep = true;
    return asleep;
  }
  if (still >= after) {
    asleep = true;
    for (auto& b : bodies) {
      b->v = V3{};
      b->w = V3{};
    }
  }
  return asleep;
}

i64 RigidSystem::memory_bytes() const {
  i64 n = static_cast<i64>(sizeof(*this));
  for (const auto& b : bodies) n += static_cast<i64>(sizeof(RigidBody) + b->spheres.capacity() * sizeof(Sphere));
  n += static_cast<i64>(joints.size() * sizeof(Joint) + attachments.size() * sizeof(Attachment) + orienters.size() * sizeof(Orienter));
  n += static_cast<i64>((pairs.capacity() + near_pairs_.capacity()) * sizeof(SpherePair) + obstacles.capacity() * sizeof(Obstacle) +
                        reactions.capacity() * sizeof(Reaction) + contacts_.capacity() * sizeof(WorldContact) +
                        (still_at_.capacity() + frame_start_.capacity()) * sizeof(V3) + limit_hits_.capacity() * sizeof(LimitHit));
  return n;
}

// ---- contacts --------------------------------------------------------------------------------

void RigidSystem::find_contacts(f64 dt) {
  contact_count_ = 0;
  for (auto& b : bodies) b->contact = false;
  if (!obstacles.empty()) find_obstacles(dt);
  if (!collision) return;
  SphereContact p;
  for (auto& bp : bodies) {
    RigidBody* b = bp.get();
    if (b->gone) continue;
    const f64 speed = hypot3(b->v.x, b->v.y, b->v.z);
    for (const Sphere& s : b->spheres) {
      const f64 reach = speed + hypot3(b->w.x, b->w.y, b->w.z) * hypot3(s.c.x, s.c.y, s.c.z);
      const f64 m = std::min(margin + reach * dt, 0.6);
      const V3 c = b->point(s.c);
      if (!collision->sphere(c, s.r + m, &p)) continue;
      add_contact(b, s, p.normal, c + p.push, s.r + m);
      // a second surface (a corner: the floor and a wall)
      const V3 c2 = c + p.push;
      const V3 n0 = p.normal;
      if (collision->sphere(c2, s.r + m, &p) && dot(p.normal, n0) < 0.8) add_contact(b, s, p.normal, c2 + p.push, s.r + m);
    }
  }
}

void RigidSystem::find_obstacles(f64 dt) {
  for (auto& bp : bodies) {
    RigidBody* b = bp.get();
    if (b->ghost || b->gone) continue;
    const f64 speed = hypot3(b->v.x, b->v.y, b->v.z);
    for (const Sphere& s : b->spheres) {
      const V3 c = b->point(s.c);
      const f64 m = std::min(margin + speed * dt, 0.6);
      for (const Obstacle& o : obstacles) {
        const f64 dx = c.x - o.c.x, dy = c.y - o.c.y, dz = c.z - o.c.z;
        const f64 rr = s.r + m + o.r;
        if (dx * dx + dy * dy + dz * dz >= rr * rr) continue;
        f64 d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d == 0.0) d = 1e-6;
        const V3 n{dx / d, dy / d, dz / d};
        // the sphere freed: touching the obstacle's surface at s.r + margin
        const f64 f = o.r + s.r + m;
        WorldContact& k = add_contact(b, s, n, o.c + n * f, s.r + m);
        k.obstacle = true;
        k.other_owner = o.owner;
        k.other_part = o.part;
        k.other_v = o.v;
      }
    }
  }
}

void RigidSystem::find_pairs(f64 dt) {
  near_pairs_.clear();
  if (!pairs_enabled) return;
  for (const SpherePair& p : pairs) {
    const RigidBody* A = p.a;
    const RigidBody* B = p.b;
    if (A->gone || B->gone) continue;
    const Sphere& sa = A->spheres[size_t(p.sa)];
    const Sphere& sb = B->spheres[size_t(p.sb)];
    const V3 ca = A->point(sa.c);
    const V3 cb = B->point(sb.c);
    const f64 rv = hypot3(B->v.x - A->v.x, B->v.y - A->v.y, B->v.z - A->v.z) + 0.5 * (hypot3(A->w.x, A->w.y, A->w.z) + hypot3(B->w.x, B->w.y, B->w.z));
    const f64 d = hypot3(cb.x - ca.x, cb.y - ca.y, cb.z - ca.z);
    if (d < sa.r + sb.r + margin + rv * dt) near_pairs_.push_back(p);
  }
}

RigidSystem::WorldContact& RigidSystem::add_contact(RigidBody* b, const Sphere& s, const V3& n, const V3& c, f64 rr) {
  if (contact_count_ == contacts_.size()) contacts_.emplace_back();
  WorldContact& k = contacts_[contact_count_++];
  k.body = b;
  k.c = s.c;
  k.r = s.r;
  k.n = n;
  // the freed centre touches the surface at distance rr
  k.d = n.x * c.x + n.y * c.y + n.z * c.z - rr;
  k.lambda = 0.0;
  k.obstacle = false;
  k.other_owner = -1;
  k.other_part = -1;
  k.other_v = V3{};
  k.took = 0.0;
  return k;
}

// ---- the substep -----------------------------------------------------------------------------

void RigidSystem::substep_once(f64 h) {
  const f64 g = gravity;
  const f64 drag_l = pow(linear_drag, h);
  const f64 drag_a = pow(angular_drag, h);
  for (auto& bp : bodies) {
    RigidBody& b = *bp;
    b.px = b.x;
    b.pq = b.q;
    if (b.inv_mass == 0.0) continue;
    b.update_inertia();
    b.v.x = (b.v.x + h * b.force.x * b.inv_mass) * drag_l;
    b.v.y = (b.v.y + h * b.force.y * b.inv_mass) * drag_l;
    b.v.z = (b.v.z + h * (b.force.z * b.inv_mass - g)) * drag_l;
    const V3 t = b.inv_inertia_mul(b.torque);
    b.w.x = (b.w.x + h * t.x) * drag_a;
    b.w.y = (b.w.y + h * t.y) * drag_a;
    b.w.z = (b.w.z + h * t.z) * drag_a;
    if (b.twist_damping > 0.0) {
      const V3 a = RigidBody::rotate_by(b.q, b.long_axis);
      const f64 spin = (b.w.x * a.x + b.w.y * a.y + b.w.z * a.z) * (1.0 - exp(-b.twist_damping * h));
      b.w = b.w - a * spin;
    }
    b.x.x += h * b.v.x;
    b.x.y += h * b.v.y;
    b.x.z += h * b.v.z;
    b.rotate(V3{h * b.w.x, h * b.w.y, h * b.w.z});
    b.update_inertia();
    b.v0 = b.v;
    b.w0 = b.w;
  }
  for (auto& j : joints) solve_joint(*j, h);
  for (auto& a : attachments)
    if (a->enabled) solve_attachment(*a, h);
  for (auto& o : orienters)
    if (o->enabled) solve_orienter(*o, h);
  for (const SpherePair& p : near_pairs_) solve_pair(p);
  for (size_t i = 0; i < contact_count_; ++i) solve_contact(contacts_[i]);
  // velocities from the positions
  const f64 ih = 1.0 / h;
  for (auto& bp : bodies) {
    RigidBody& b = *bp;
    if (b.inv_mass == 0.0) continue;
    // The pose must obey the same angular travel bound as its velocity.
    // Capping only reconstructed w left a light wrist teleporting through a
    // large projection and reporting a perfectly modest spin afterwards.
    const f64 angle = norm(qerror(b.q, b.pq));
    if (angle > spin_cap * h) {
      b.q = qslerp(b.pq, b.q, spin_cap * h / angle);
      b.update_inertia();
    }
    b.v = (b.x - b.px) * ih;
    // dq = q pq^-1
    const Quat& q = b.q;
    const Quat& p = b.pq;
    const f64 dx = q.w * -p.x + q.x * p.w + q.y * -p.z - q.z * -p.y;
    const f64 dy = q.w * -p.y + q.y * p.w + q.z * -p.x - q.x * -p.z;
    const f64 dz = q.w * -p.z + q.z * p.w + q.x * -p.y - q.y * -p.x;
    const f64 dw = q.w * p.w - q.x * -p.x - q.y * -p.y - q.z * -p.z;
    const f64 s = dw < 0.0 ? -2.0 * ih : 2.0 * ih;
    b.w = V3{dx * s, dy * s, dz * s};
    // a correction may move a body but not launch it (a limb folded past its range, a contact
    // found deep, is put right without the energy it would take to do it that fast)
    clamp_change(b.v, b.v0, max_dv);
    clamp_change(b.w, b.w0, max_dw);
    if (spin_cap < kMaxSpin) clamp_length(b.w, spin_cap);
  }
  limit_velocity();
  for (auto& j : joints)
    if (j->damping > 0.0) damp_joint(*j, h);
  for (auto& a : attachments)
    if (a->enabled && a->damping > 0.0) damp_attachment(*a, h);
  for (auto& o : orienters)
    if (o->enabled && o->damping > 0.0) damp_orienter(*o, h);
  for (size_t i = 0; i < contact_count_; ++i) contact_velocity(contacts_[i], h);
  // Damping and friction also apply impulses. The next substep must begin
  // within the same spin bound as the position solve, including a tiny wrist.
  for (auto& b : bodies) clamp_length(b->w, spin_cap);
}

// ---- joints ----------------------------------------------------------------------------------

void RigidSystem::solve_joint(Joint& j, f64 h) {
  RigidBody* A = j.a;
  RigidBody* B = j.b;
  // the anchors meet
  const V3 ra = RigidBody::rotate_by(A->q, j.anchor_a);
  const V3 rb = RigidBody::rotate_by(B->q, j.anchor_b);
  f64 dx = B->x.x + rb.x - A->x.x - ra.x;
  f64 dy = B->x.y + rb.y - A->x.y - ra.y;
  f64 dz = B->x.z + rb.z - A->x.z - ra.z;
  const f64 c = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (c > 1e-9) {
    dx /= c;
    dy /= c;
    dz /= c;
    const V3 d{dx, dy, dz};
    const f64 w = A->inv_mass_at(ra, d) + B->inv_mass_at(rb, d);
    if (w > 0.0) {
      const f64 p = c / w;
      A->apply_pos(d * p, ra);
      B->apply_pos(d * -p, rb);
    }
  }
  // the muscle: towards the target relative rotation
  if (j.stiffness > 0.0) {
    // e = log(qB (qA target)^-1): how far B is turned past its target (world)
    const V3 e = qerror(B->q, A->q * j.target);
    const f64 th = hypot3(e.x, e.y, e.z);
    if (th > 1e-7) turn(A, B, e * (1.0 / th), th, 1.0 / (j.stiffness * h * h), j.max_torque * h * h);
  }
  if (j.feed.x != 0.0 || j.feed.y != 0.0 || j.feed.z != 0.0) {
    const f64 hh = h * h;
    A->apply_rot(j.feed * -hh);
    B->apply_rot(j.feed * hh);
  }
  // limits
  const Quat fa = A->q * j.frame_a;
  const Quat fb = B->q * j.frame_b;
  if (j.kind == JointKind::Hinge) {
    // the hinge axes line up
    align(A, B, RigidBody::rotate_by(fa, V3{1, 0, 0}), RigidBody::rotate_by(fb, V3{1, 0, 0}));
    if (j.hinge) {
      const Quat fa2 = A->q * j.frame_a;
      const Quat fb2 = B->q * j.frame_b;
      const V3 n = RigidBody::rotate_by(fa2, V3{1, 0, 0});
      const V3 n1 = RigidBody::rotate_by(fa2, V3{0, 0, 1});
      const V3 n2 = RigidBody::rotate_by(fb2, V3{0, 0, 1});
      limit_angle(A, B, n, n1, n2, j.hinge->first, j.hinge->second);
    }
    return;
  }
  const V3 az = RigidBody::rotate_by(fa, V3{0, 0, 1});
  const V3 bz = RigidBody::rotate_by(fb, V3{0, 0, 1});
  if (j.swing) {
    // the tilt of B's twist axis in A's joint frame, and the limit in that direction
    const f64 cx = az.y * bz.z - az.z * bz.y;
    const f64 cy = az.z * bz.x - az.x * bz.z;
    const f64 cz = az.x * bz.y - az.y * bz.x;
    const f64 sn = std::sqrt(cx * cx + cy * cy + cz * cz);
    const f64 cs = az.x * bz.x + az.y * bz.y + az.z * bz.z;
    const f64 angle = atan2(sn, cs);
    if (sn > 1e-6) {
      const V3 d = rotate_inv(fa, bz);
      f64 l = hypot2(d.x, d.y);
      if (l == 0.0) l = 1.0;
      const f64 ux = d.x / l, uy = d.y / l;
      const f64 lx = ux >= 0.0 ? j.swing->x_pos : j.swing->x_neg;
      const f64 ly = uy >= 0.0 ? j.swing->y_pos : j.swing->y_neg;
      const f64 max = 1.0 / std::sqrt((ux * ux) / (lx * lx) + (uy * uy) / (ly * ly));
      if (angle > max) {
        const V3 n{cx / sn, cy / sn, cz / sn};
        turn(A, B, n, std::min(angle - max, kLimitStep), 0.0, kInf);
        limit_hit(A, B, n, 1.0);
      }
    }
  }
  if (j.twist) {
    // swing-twist: the relative rotation fa^-1 fb = swing * twist, twist about z; turning B about
    // its own z changes the twist alone (it cannot fight the swing limit)
    const Quat fa2 = A->q * j.frame_a;
    const Quat fb2 = B->q * j.frame_b;
    // r = fa2^-1 fb2 (only z and w are needed)
    const f64 ax = -fa2.x, ay = -fa2.y, az2 = -fa2.z, aw = fa2.w;
    const f64 rz = aw * fb2.z + az2 * fb2.w + ax * fb2.y - ay * fb2.x;
    const f64 rw = aw * fb2.w - ax * fb2.x - ay * fb2.y - az2 * fb2.z;
    f64 tw = 2.0 * atan2(rz, rw);
    if (tw > kPi) tw -= 2.0 * kPi;
    else if (tw < -kPi) tw += 2.0 * kPi;
    const f64 lo = j.twist->first, hi = j.twist->second;
    if (tw > hi || tw < lo) {
      const V3 n = RigidBody::rotate_by(fb2, V3{0, 0, 1});
      const f64 excess = tw > hi ? std::min(tw - hi, kLimitStep) : std::max(tw - lo, -kLimitStep);
      turn(A, B, n, excess, 0.0, kInf);
      limit_hit(A, B, n, excess > 0.0 ? 1.0 : -1.0);
    }
  }
}

// Turns B towards A so that B's axis `bx` lines up with A's `ax`.
void RigidSystem::align(RigidBody* A, RigidBody* B, const V3& ax, const V3& bx) {
  const f64 cx = ax.y * bx.z - ax.z * bx.y;
  const f64 cy = ax.z * bx.x - ax.x * bx.z;
  const f64 cz = ax.x * bx.y - ax.y * bx.x;
  const f64 sn = std::sqrt(cx * cx + cy * cy + cz * cz);
  if (sn < 1e-7) return;
  const f64 angle = atan2(sn, ax.x * bx.x + ax.y * bx.y + ax.z * bx.z);
  turn(A, B, V3{cx / sn, cy / sn, cz / sn}, angle, 0.0, kInf);
}

// Keeps the angle from n1 (on A) to n2 (on B) about n within [lo, hi] (n1, n2 orthogonal to n,
// not necessarily unit).
void RigidSystem::limit_angle(RigidBody* A, RigidBody* B, const V3& n, const V3& n1, const V3& n2, f64 lo, f64 hi) {
  const f64 cx = n1.y * n2.z - n1.z * n2.y;
  const f64 cy = n1.z * n2.x - n1.x * n2.z;
  const f64 cz = n1.x * n2.y - n1.y * n2.x;
  const f64 phi = atan2(cx * n.x + cy * n.y + cz * n.z, n1.x * n2.x + n1.y * n2.y + n1.z * n2.z);
  // (a joint past its range is brought back over a few substeps: corrections that fight each other
  // at the edge of a range would otherwise pump energy into the limb)
  if (phi > hi) {
    turn(A, B, n, std::min(phi - hi, kLimitStep), 0.0, kInf);
    limit_hit(A, B, n, 1.0);
  } else if (phi < lo) {
    turn(A, B, n, std::max(phi - lo, -kLimitStep), 0.0, kInf);
    limit_hit(A, B, n, -1.0);
  }
}

// A limit turned B back about -n: its rebound is taken out in the velocity pass.
void RigidSystem::limit_hit(RigidBody* A, RigidBody* B, const V3& n, f64 sign) {
  if (limit_count_ == limit_hits_.size()) limit_hits_.emplace_back();
  LimitHit& hit = limit_hits_[limit_count_++];
  hit.a = A;
  hit.b = B;
  hit.n = n * sign;
}

// Limits are inelastic: B does not spin on away from where the limit put it.
void RigidSystem::limit_velocity() {
  for (size_t i = 0; i < limit_count_; ++i) {
    const LimitHit& hit = limit_hits_[i];
    RigidBody* A = hit.a;
    RigidBody* B = hit.b;
    const V3& n = hit.n;
    const f64 rel = (B->w.x - A->w.x) * n.x + (B->w.y - A->w.y) * n.y + (B->w.z - A->w.z) * n.z;
    // (the limit turned B towards -n; spinning on that way is the bounce)
    if (rel >= 0.0) continue;
    const f64 w = A->inv_mass_rot(n) + B->inv_mass_rot(n);
    if (w <= 0.0) continue;
    const f64 l = -rel / w;
    A->w = A->w - A->inv_inertia_mul(n * l);
    B->w = B->w + B->inv_inertia_mul(n * l);
  }
  limit_count_ = 0;
}

// Reduces B's rotation relative to A about unit n by `angle` (A turns +, B turns -, by their
// inverse inertias), with compliance and an impulse limit.
void RigidSystem::turn(RigidBody* A, RigidBody* B, const V3& n, f64 angle, f64 compliance, f64 max_impulse) {
  const f64 w = A->inv_mass_rot(n) + B->inv_mass_rot(n);
  if (w <= 0.0) return;
  f64 p = angle / (w + compliance);
  if (p > max_impulse) p = max_impulse;
  else if (p < -max_impulse) p = -max_impulse;
  A->apply_rot(n * p);
  B->apply_rot(n * -p);
}

void RigidSystem::damp_joint(Joint& j, f64 h) {
  RigidBody* A = j.a;
  RigidBody* B = j.b;
  const V3& tv = j.target_vel;
  const V3 t = tv.x != 0.0 || tv.y != 0.0 || tv.z != 0.0 ? RigidBody::rotate_by(A->q, tv) : V3{};
  f64 rx = B->w.x - A->w.x - t.x, ry = B->w.y - A->w.y - t.y, rz = B->w.z - A->w.z - t.z;
  const f64 r = std::sqrt(rx * rx + ry * ry + rz * rz);
  if (r < 1e-9) return;
  rx /= r;
  ry /= r;
  rz /= r;
  const V3 u{rx, ry, rz};
  const f64 w = A->inv_mass_rot(u) + B->inv_mass_rot(u);
  if (w <= 0.0) return;
  // the share of the relative spin a damper c takes out of the limb in a substep
  const f64 frac = j.eff_inertia > 0.0 ? std::min(1.0, (j.damping * h) / j.eff_inertia) : std::min(1.0, j.damping * h * w);
  // A driven damper is a muscle too. Without this bound a changed target rate
  // could inject an unlimited angular impulse into a light wrist in one step.
  const f64 l = std::min((r * frac) / w, j.max_torque * h);
  A->w = A->w + A->inv_inertia_mul(u * l);
  B->w = B->w - B->inv_inertia_mul(u * l);
}

// ---- assists ---------------------------------------------------------------------------------

void RigidSystem::solve_attachment(Attachment& a, f64 h) {
  RigidBody* B = a.body;
  const V3 r = RigidBody::rotate_by(B->q, a.local);
  RigidBody* A = a.reference && !a.reference->gone ? a.reference : nullptr;
  const V3 ra = A ? RigidBody::rotate_by(A->q, a.reference_local) : V3{};
  const V3 target = A ? A->x + ra : a.target;
  f64 dx = a.axes[0] ? target.x - B->x.x - r.x : 0.0;
  f64 dy = a.axes[1] ? target.y - B->x.y - r.y : 0.0;
  f64 dz = a.axes[2] ? target.z - B->x.z - r.z : 0.0;
  const f64 c = std::sqrt(dx * dx + dy * dy + dz * dz);
  a.applied = V3{};
  if (c < 1e-9) return;
  dx /= c;
  dy /= c;
  dz /= c;
  const V3 d{dx, dy, dz};
  const f64 w = B->inv_mass_at(r, d) + (A ? A->inv_mass_at(ra, d) : 0.0);
  if (w <= 0.0) return;
  const f64 compliance = std::isinf(a.stiffness) ? 0.0 : 1.0 / (a.stiffness * h * h);
  f64 p = c / (w + compliance);
  const f64 max = a.max_force * h * h;
  if (p > max) p = max;
  B->apply_pos(d * p, r);
  if (A) A->apply_pos(d * -p, ra);
  const f64 f = p / (h * h);
  a.applied = d * f;
}

void RigidSystem::damp_attachment(Attachment& a, f64 h) {
  RigidBody* B = a.body;
  const V3 r = RigidBody::rotate_by(B->q, a.local);
  RigidBody* A = a.reference && !a.reference->gone ? a.reference : nullptr;
  const V3 ra = A ? RigidBody::rotate_by(A->q, a.reference_local) : V3{};
  const V3 velocity = A ? A->v + cross(A->w, ra) : a.target_vel;
  f64 vx = a.axes[0] ? velocity.x - (B->v.x + B->w.y * r.z - B->w.z * r.y) : 0.0;
  f64 vy = a.axes[1] ? velocity.y - (B->v.y + B->w.z * r.x - B->w.x * r.z) : 0.0;
  f64 vz = a.axes[2] ? velocity.z - (B->v.z + B->w.x * r.y - B->w.y * r.x) : 0.0;
  const f64 s = std::sqrt(vx * vx + vy * vy + vz * vz);
  if (s < 1e-9) return;
  vx /= s;
  vy /= s;
  vz /= s;
  const V3 u{vx, vy, vz};
  const f64 w = B->inv_mass_at(r, u) + (A ? A->inv_mass_at(ra, u) : 0.0);
  if (w <= 0.0) return;
  f64 j = s * std::min(a.damping * h, 1.0 / w);
  const f64 max = a.max_force * h;
  if (j > max) j = max;
  B->apply_impulse(u * j, r);
  if (A) A->apply_impulse(u * -j, ra);
}

void RigidSystem::solve_orienter(Orienter& o, f64 h) {
  RigidBody* B = o.body;
  V3 e;
  if (o.tilt_only) {
    // turn the body's up axis onto the target's
    const V3 u = RigidBody::rotate_by(B->q, o.up);
    const V3 t = RigidBody::rotate_by(o.target, o.up);
    const f64 cx = t.y * u.z - t.z * u.y;
    const f64 cy = t.z * u.x - t.x * u.z;
    const f64 cz = t.x * u.y - t.y * u.x;
    const f64 sn = std::sqrt(cx * cx + cy * cy + cz * cz);
    if (sn < 1e-7) return;
    const f64 ang = atan2(sn, t.x * u.x + t.y * u.y + t.z * u.z);
    e = V3{(cx / sn) * ang, (cy / sn) * ang, (cz / sn) * ang};
  } else {
    e = qerror(B->q, o.target);
  }
  const f64 th = std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z);
  if (th < 1e-7) return;
  const V3 n = e * (1.0 / th);
  const f64 w = B->inv_mass_rot(n);
  if (w <= 0.0) return;
  const f64 compliance = std::isinf(o.stiffness) ? 0.0 : 1.0 / (o.stiffness * h * h);
  f64 p = th / (w + compliance);
  const f64 max = o.max_torque * h * h;
  if (p > max) p = max;
  B->apply_rot(n * -p);
}

void RigidSystem::damp_orienter(Orienter& o, f64 h) {
  RigidBody* B = o.body;
  V3 wv = B->w;
  if (o.tilt_only) {
    // only the tilting part of the spin
    const V3 u = RigidBody::rotate_by(B->q, o.up);
    const f64 d = wv.x * u.x + wv.y * u.y + wv.z * u.z;
    wv = wv - u * d;
  }
  const f64 s = std::sqrt(wv.x * wv.x + wv.y * wv.y + wv.z * wv.z);
  if (s < 1e-9) return;
  const V3 u = wv * (1.0 / s);
  const f64 w = B->inv_mass_rot(u);
  if (w <= 0.0) return;
  f64 l = s * std::min(o.damping * h, 1.0 / w);
  const f64 max = o.max_torque * h;
  if (l > max) l = max;
  B->w = B->w - B->inv_inertia_mul(u * l);
}

// ---- contacts --------------------------------------------------------------------------------

void RigidSystem::solve_pair(const SpherePair& p) {
  RigidBody* A = p.a;
  RigidBody* B = p.b;
  const Sphere& sa = A->spheres[size_t(p.sa)];
  const Sphere& sb = B->spheres[size_t(p.sb)];
  const V3 ca = A->point(sa.c);
  const V3 cb = B->point(sb.c);
  f64 nx = cb.x - ca.x, ny = cb.y - ca.y, nz = cb.z - ca.z;
  const f64 d = std::sqrt(nx * nx + ny * ny + nz * nz);
  const f64 pen = sa.r + sb.r - d;
  if (pen <= 0.0 || d < 1e-9) return;
  nx /= d;
  ny /= d;
  nz /= d;
  const V3 n{nx, ny, nz};
  const V3 ra{ca.x + nx * sa.r - A->x.x, ca.y + ny * sa.r - A->x.y, ca.z + nz * sa.r - A->x.z};
  const V3 rb{cb.x - nx * sb.r - B->x.x, cb.y - ny * sb.r - B->x.y, cb.z - nz * sb.r - B->x.z};
  const f64 w = A->inv_mass_at(ra, n) + B->inv_mass_at(rb, n);
  if (w <= 0.0) return;
  // (a deep overlap - limbs folded into each other - comes apart over a few substeps, not with a
  // bang)
  const f64 l = std::min(pen, pair_speed * substep) / w;
  A->apply_pos(n * -l, ra);
  B->apply_pos(n * l, rb);
}

void RigidSystem::solve_contact(WorldContact& k) {
  RigidBody* B = k.body;
  const V3 c = B->point(k.c);
  const V3& n = k.n;
  const f64 pen = k.d + k.r - (n.x * c.x + n.y * c.y + n.z * c.z);
  if (pen <= 0.0) {
    k.lambda = 0.0;
    return;
  }
  B->contact = true;
  B->contact_normal = n;
  // the contact point on the sphere, relative to the centre of mass
  const V3 r{c.x - n.x * k.r - B->x.x, c.y - n.y * k.r - B->x.y, c.z - n.z * k.r - B->x.z};
  B->contact_point = B->x + r;
  const f64 w = B->inv_mass_at(r, n);
  if (w <= 0.0) return;
  // (an obstacle found deep inside - two bodies overlapping - is left gradually; another body's
  // part is pushed out of as fast as the two come together, not faster: people standing too close
  // ease apart, a body barged into goes)
  f64 cap = kDeepSpeed;
  if (k.other_owner >= 0) {
    const f64 approach = -((B->v.x - k.other_v.x) * n.x + (B->v.y - k.other_v.y) * n.y + (B->v.z - k.other_v.z) * n.z);
    cap = clamp(approach + 0.3, 0.4, kDeepSpeed);
  }
  const f64 l = (k.obstacle ? std::min(pen, cap * substep) : pen) / w;
  B->apply_pos(n * l, r);
  k.lambda = l;
  if (k.other_owner >= 0) k.took += std::min(l / substep, 40.0);
  if (k.obstacle) B->bumped += std::min(l / substep, 40.0) * hypot2(n.x, n.y);
  // static friction: the contact point does not slide within the friction cone
  const V3 loc = rotate_inv(B->q, r);
  const V3 p0 = RigidBody::rotate_by(B->pq, loc);
  const V3 p1 = B->x + r;
  V3 t = p1 - (B->px + p0);
  const f64 dn = dot(t, n);
  t = t - n * dn;
  const f64 tl = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
  if (tl > 1e-9) {
    t = t * (1.0 / tl);
    const f64 wt = B->inv_mass_at(r, t);
    const f64 lt = tl / wt;
    if (lt < B->friction * 1.1 * l) B->apply_pos(t * -lt, r);
  }
}

void RigidSystem::contact_velocity(WorldContact& k, f64 h) {
  if (k.lambda <= 0.0) return;
  RigidBody* B = k.body;
  const V3& n = k.n;
  const V3 c = B->point(k.c);
  const V3 r{c.x - n.x * k.r - B->x.x, c.y - n.y * k.r - B->x.y, c.z - n.z * k.r - B->x.z};
  const V3 v{B->v.x + B->w.y * r.z - B->w.z * r.y, B->v.y + B->w.z * r.x - B->w.x * r.z, B->v.z + B->w.x * r.y - B->w.y * r.x};
  const f64 vn = dot(v, n);
  V3 t = v - n * vn;
  const f64 vt = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
  // dynamic friction, bounded by the normal impulse
  if (vt > 1e-6) {
    t = t * (1.0 / vt);
    const f64 wt = B->inv_mass_at(r, t);
    const f64 jt = std::min((B->friction * k.lambda) / h, vt / wt);
    B->apply_impulse(t * -jt, r);
  }
  // no bounce
  if (vn < 0.0) {
    const f64 wn = B->inv_mass_at(r, n);
    const f64 jn = -vn / wn;
    B->apply_impulse(n * jn, r);
    if (jn > B->impact) B->impact = jn;
    if (k.obstacle) B->bumped += jn * hypot2(n.x, n.y);
    if (k.other_owner >= 0) k.took += jn;
  }
}

}  // namespace svx::anim
