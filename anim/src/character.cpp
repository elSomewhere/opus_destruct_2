#include "svx/anim/character.hpp"

#include <algorithm>
#include <map>

namespace svx::anim {

namespace {

// Damage multipliers by bone (rifle rounds ~ 30-40 damage, health 100).
f64 zone_mult(i32 bone, f64 fallback) {
  switch (bone) {
    case H::head: return 4.0;
    case H::neck: return 3.0;
    case H::chest: return 1.1;
    case H::spine: return 1.0;
    case H::pelvis: return 0.9;
    default: return fallback;
  }
}

bool near_tail(const VoxelPart& p, const V3& tail, f64 s) {
  const i32 nx = p.dims[0], ny = p.dims[1], nz = p.dims[2];
  const f64 r2 = (2.5 * s) * (2.5 * s);
  for (i32 z = 0; z < nz; ++z)
    for (i32 y = 0; y < ny; ++y)
      for (i32 x = 0; x < nx; ++x) {
        if (p.cells[size_t(x + nx * (y + ny * z))] == 0) continue;
        const f64 cx = (p.origin[0] + x + 0.5) * s - tail.x;
        const f64 cy = (p.origin[1] + y + 0.5) * s - tail.y;
        const f64 cz = (p.origin[2] + z + 0.5) * s - tail.z;
        if (cx * cx + cy * cy + cz * cz < r2) return true;
      }
  return false;
}

}  // namespace

Character::Character(const CharacterOptions& o)
    : model(o.model),
      palette(o.palette),
      motion(o.model->skeleton, o.collision, o.seed),
      body(o.model->skeleton, o.collision, HumanoidBodyOptions{o.mass, 1.0}),
      behaviours(motion, body, o.seed),
      pose(o.model->skeleton),
      prev_pose(o.model->skeleton),
      weapon(o.weapon),
      health(o.health),
      max_health(o.health),
      switch_from_(o.model->skeleton),
      rng_(o.seed * 7919.0 + 13.0) {
  motion.weapon = weapon;
  skin.assign(size_t(model->skeleton->count) * 16, 0.0f);
  for (const VoxelPart& p : model->parts) part_full_.push_back(p.count);
  whole_ = model;
  backend_ = o.backend == BodyBackend::Deep && o.world ? BodyBackend::Deep : BodyBackend::Shallow;
  world_ = o.world;
  group_ = o.group;
  tag_ = o.tag;
}

// (its articulation is its host's to remove: a character outlived by its world does not touch it)
Character::~Character() = default;

bool Character::down() const {
  const BodyMode m = behaviours.mode;
  return m == BodyMode::Falling || m == BodyMode::Lying || m == BodyMode::Rising;
}

bool Character::asleep() const { return !alive() && body.system.asleep; }

// ---- where the body is simulated ----------------------------------------------------------------

bool Character::bind_body() {
  if (!world_) return false;
  if (binding_.bound()) return true;
  return binding_.bind(*world_, body.system, group_, tag_);
}

bool Character::set_backend(BodyBackend b, World* world) {
  if (world) world_ = world;
  if (b == backend_) return true;
  if (b == BodyBackend::Deep) {
    if (!world_) return false;
    backend_ = BodyBackend::Deep;
    if (behaviours.physical && !bind_body()) {
      backend_ = BodyBackend::Shallow;
      return false;
    }
    return true;
  }
  // deep -> shallow: the body is its own again, where the core left it
  backend_ = BodyBackend::Shallow;
  if (binding_.bound()) binding_.unbind(*world_);
  if (pending_post_) {
    // (the drives were set for a tick that will not come: the body's own step carries them out)
    pending_post_ = false;
    prev_pose.copy_from(pose);
    body.system.step(last_dt_);
    behaviours.drive_post(last_dt_, pose);
    finish_frame();
  }
  return true;
}

bool Character::adopt(ArticulationId id) {
  if (!world_) return false;
  if (!binding_.adopt(*world_, body.system, id)) return false;
  backend_ = BodyBackend::Deep;
  behaviours.physical = true;
  pending_post_ = false;
  body.write_pose(pose);
  prev_pose.copy_from(pose);
  pose.write_skin(skin.data());
  placed_ = true;
  return true;
}

void Character::unbound() {
  if (binding_.bound() && world_) binding_.unbind(*world_);
  pending_post_ = false;
  body.system.external = false;
}

// ---- the host's side ----------------------------------------------------------------------------

void Character::place(const V3& pos, f64 yaw) {
  motion.place(pos, yaw);
  pose.copy_from(motion.world);
  prev_pose.copy_from(pose);
  placed_ = true;
  if (behaviours.physical) {
    body.set_from_pose(pose, nullptr, 0.0);
    // (bound: the links go where the bodies went, at the next push)
  }
  pose.write_skin(skin.data());
}

void Character::set_root(const V3& pos, f64 yaw) {
  if (!alive() || controlled()) return;
  motion.set_root(pos, yaw);
}

std::vector<AnimEvent> Character::take_events() {
  std::vector<AnimEvent> ev = motion.take_events();
  if (behaviours.physical)
    for (AnimEvent& e : ev)
      if (e.limb != Limb::None) e.pos = limb_pos(e.limb);
  return ev;
}

V3 Character::take_root_motion() { return behaviours.take_root_motion(); }

void Character::fire() {
  if (!alive()) return;
  motion.fire();
  firing_ = 0.15;
  if (behaviours.physical && weapon) {
    // the kick goes into the hands and the shoulder
    const bool pistol = weapon->kind == PropKind::Pistol;
    const V3 dir = rotate(weapon_rot, V3{0, -1, 0.35});
    const f64 j = pistol ? 1.2 : 2.6;
    body.parts[B::handR]->apply_impulse(dir * j, V3{});
    if (!pistol) body.parts[B::chest]->apply_impulse(V3{dir.x * j * 0.8, dir.y * j * 0.8, 0.0}, V3{0, 0, 0.1});
  }
}

// ---- the frame ------------------------------------------------------------------------------------

bool Character::begin_start(f64 dt_in) {
  const f64 dt = std::min(0.05, std::max(0.0, dt_in));
  Behaviours& b = behaviours;
  pending_post_ = false;
  frame_dt_ = dt;
  if (dt > 0.0) last_dt_ = dt;
  flash = std::max(0.0, flash - dt * 6.0);
  pain_ = std::max(0.0, pain_ - dt);
  firing_ = std::max(0.0, firing_ - dt);
  if (!placed_) place(motion.root_pos, motion.root_yaw);
  if (!alive()) dead_time += dt;
  if (b.mode == BodyMode::Dead && body.system.asleep) {
    // (deep: unless the world woke it - something ran into the body)
    if (!(binding_.bound() && world_ && !world_->articulation_asleep(binding_.id()))) {
      pose.write_skin(skin.data());
      return false;
    }
    body.system.asleep = false;
  }
  // physics on or off (level of detail)
  const bool need = b.needs_physics() || !alive();
  calm_for_ = need ? 0.0 : calm_for_ + dt;
  if (!b.physical && (need || physics)) wake();
  else if (b.physical && !need && !physics && calm_for_ > 0.6) rest();
  return true;
}

void Character::begin_body() {
  const f64 dt = frame_dt_;
  Behaviours& b = behaviours;
  b.prepare(dt, pose);
  motion.update(dt);
  prev_pose.copy_from(pose);
  if (b.physical && backend_ == BodyBackend::Deep && binding_.bound() && world_) {
    // (the drives for the world's tick; begin_push takes them, and what the host did to the
    // body, to the core; end takes the rest)
    b.drive_pre(dt);
    pending_post_ = true;
    return;
  }
  if (b.physical) {
    b.drive(dt, pose);
    if (b.mode == BodyMode::Dying || b.mode == BodyMode::Dead || b.mode == BodyMode::Falling) limit_turns(0.6);
  } else {
    pose.copy_from(motion.world);
  }
  finish_frame();
}

void Character::begin_push() {
  if (pending_post_ && binding_.bound() && world_) binding_.push(*world_);
}

void Character::end() {
  if (!pending_post_) return;
  pending_post_ = false;
  const f64 dt = frame_dt_;
  Behaviours& b = behaviours;
  if (binding_.bound() && world_ && binding_.pull(*world_, dt)) {
    b.drive_post(dt, pose);
  } else {
    // (its articulation is gone - out of the world, removed: the body is its own again, and its
    // own step carries out the drives)
    unbound();
    body.system.step(dt);
    b.drive_post(dt, pose);
  }
  // a body thrown about is shown turning no bone more than about 35 degrees a frame (a limb
  // spinning about its length reads as a flip)
  if (b.mode == BodyMode::Dying || b.mode == BodyMode::Dead || b.mode == BodyMode::Falling) limit_turns(0.6);
  finish_frame();
}

void Character::finish_frame() {
  if (switch_blend_ < 1.0) {
    // (a switch between physics and plan eases over a few frames)
    switch_blend_ = std::min(1.0, switch_blend_ + last_dt_ / 0.2);
    const f64 w = switch_blend_ * switch_blend_ * (3.0 - 2.0 * switch_blend_);
    for (size_t i = 0; i < pose.p.size(); ++i) {
      pose.p[i] = vlerp(switch_from_.p[i], pose.p[i], w);
      pose.q[i] = qnlerp(switch_from_.q[i], pose.q[i], w);
    }
  }
  if (knocked_out && behaviours.conscious && behaviours.mode != BodyMode::Lying) knocked_out = false;
  place_weapon();
  pose.write_skin(skin.data());
}

// Limits how far each shown bone turns from the frame before (rad), about the bone's head.
void Character::limit_turns(f64 max) {
  for (size_t i = 0; i < pose.q.size(); ++i) {
    const Quat& a = prev_pose.q[i];
    const Quat& b = pose.q[i];
    const f64 c = std::min(1.0, std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w));
    const f64 angle = 2.0 * acos(c);
    if (angle > max) pose.q[i] = qnlerp(a, b, max / angle);
  }
}

// Starts the physics from the shown pose (with its momentum).
void Character::wake() {
  body.set_from_pose(pose, &prev_pose, last_dt_);
  behaviours.physical = true;
  if (backend_ == BodyBackend::Deep && !bind_body()) backend_ = BodyBackend::Shallow;  // (no room in the world: on its own)
}

// Back to the plan alone (calm and far): eases over from the body's pose.
void Character::rest() {
  behaviours.physical = false;
  if (binding_.bound() && world_) binding_.unbind(*world_);
  pending_post_ = false;
  switch_from_.copy_from(pose);
  switch_blend_ = 0.0;
}

// The prop in the (physical) right hand, held as the plan holds it.
void Character::place_weapon() {
  if (!weapon) return;
  if (!behaviours.physical) {
    weapon_pos = motion.weapon_pos;
    weapon_rot = motion.weapon_rot;
    return;
  }
  // the plan's prop relative to the plan's hand, carried by the body's hand
  const WorldPose& ph = motion.world;
  const i32 hand = weapon->kind == PropKind::Knife ? motion.weapon_hand : H::handR;
  const Quat hq = ph.q[hand];
  const V3 hp = ph.p[hand];
  const Quat inv = conj(hq);
  const V3 rel_p = rotate(inv, motion.weapon_pos - hp);
  const Quat rel_q = inv * motion.weapon_rot;
  const Quat q = pose.q[hand];
  weapon_pos = pose.p[hand] + rotate(q, rel_p);
  weapon_rot = q * rel_q;
}

// ---- senses and blows -------------------------------------------------------------------------------

void Character::perceive(const Perception& p) { behaviours.perceive(p); }

Zone Character::hit_at(const HitInfo& info) {
  if (!behaviours.physical) wake();
  const i32 part = info.bone >= 0 ? HumanoidBody::body_of_bone(info.bone) : body.nearest_part(info.point);
  return behaviours.hit(info, part, pose);
}

void Character::push(const V3& dir, f64 strength) {
  if (!alive()) return;
  if (!behaviours.physical) wake();
  const V3 d = vnorm(V3{dir.x, dir.y, 0.0});
  const f64 s = clamp(strength, 0.0, 4.0);
  behaviours.push(V3{d.x * s * 0.85, d.y * s * 0.85, s * 0.25}, s > 1.8 ? clamp((s - 1.8) * 0.45, 0.0, 0.8) : 0.0);
}

void Character::trip() {
  if (!alive()) return;
  if (!behaviours.physical) wake();
  behaviours.trip();
}

void Character::knock_out(f64 seconds) {
  if (!alive()) return;
  if (!behaviours.physical) wake();
  knocked_out = true;
  behaviours.knock_out(seconds);
}

void Character::add_injury(i32 bone, f64 severity) {
  const i32 part = HumanoidBody::body_of_bone(bone);
  const f64 s = clamp(severity, 0.0, 1.0);
  Injury i;
  i.part = part;
  i.local = V3{0.0, 0.06 * motion.k, 0.0};
  i.normal = V3{0, 1, 0};
  i.zone = zone_of_part(part);
  i.kind = HitKind::Bullet;
  i.severity = s;
  i.lasting = s;
  i.age = 30.0;
  i.hold_until = 0.0;
  behaviours.injuries.add(i);
}

void Character::collision_spheres(std::vector<Obstacle>& out, i32 owner) const { body.spheres_of(behaviours.physical ? nullptr : &pose, out, owner); }

void Character::set_obstacles(const std::vector<Obstacle>& list) {
  body.system.obstacles = list;
  // (the planner sees the body's own copy: it lives as long as the body)
  motion.feet_planner.obstacles = std::span<const Obstacle>(body.system.obstacles.data(), body.system.obstacles.size());
}

void Character::pushed_at(i32 part, const V3& j, const V3& at) {
  // (a body at rest on its plan wakes where it is: the push goes to the bodies there)
  if (!behaviours.physical) wake();
  body.system.wake();
  // (a light part takes what it can - a few m/s - the body the rest: a shoulder barged moves the
  // man, it does not fling his forearm)
  const f64 jl = hypot3(j.x, j.y, j.z);
  if (!(jl > 0.0) || !std::isfinite(jl)) return;
  RigidBody& p = *body.parts[size_t(part)];
  const f64 take = std::min(jl, 2.5 * p.mass);
  const f64 k = take / jl;
  p.update_inertia();
  p.apply_impulse(j * k, at - p.x);
  const f64 rest_share = (jl - take) / (jl * body.total_mass);
  if (rest_share > 0.0) body.shove(V3{j.x * rest_share, j.y * rest_share, j.z * rest_share * 0.3});
  behaviours.bumped(jl);
}

void Character::impulse(const V3& point, const V3& dv, f64 carry) {
  if (!behaviours.physical) wake();
  body.system.wake();
  const i32 i = body.nearest_part(point);
  RigidBody& b = *body.parts[size_t(i)];
  b.apply_impulse(dv * b.mass, point - b.x);
  if (carry > 0.0)
    for (i32 j : {B::spine, B::chest, B::head, B::upperarmL, B::upperarmR})
      if (j != i) body.parts[size_t(j)]->v += dv * carry;
}

void Character::blast_push(const V3& center, f64 radius, f64 speed) {
  if (!behaviours.physical) wake();
  body.system.wake();
  const V3 c = body.com();
  const V3 d0 = c - center;
  const f64 l0 = hypot3(d0.x, d0.y, d0.z);
  if (l0 >= radius) return;
  const V3 n0 = l0 > 1e-6 ? d0 * (1.0 / l0) : V3{0, 0, 1};
  const f64 s0 = speed * (1.0 - l0 / radius);
  for (i32 i = 0; i < kBodyCount; ++i) {
    RigidBody& b = *body.parts[size_t(i)];
    // (nearer the blast than the centre of mass: up to a quarter more)
    const f64 near = clamp(-((b.x.x - c.x) * n0.x + (b.x.y - c.y) * n0.y + (b.x.z - c.z) * n0.z) / 0.5, -1.0, 1.0);
    const f64 s = s0 * (1.0 + 0.25 * near);
    b.v.x += n0.x * s;
    b.v.y += n0.y * s;
    b.v.z += std::abs(n0.z) * s + s * 0.35;
  }
}

// ---- damage ---------------------------------------------------------------------------------------

V3 Character::bounds_center() const { return pose.p[H::pelvis]; }
f64 Character::bounds_radius() const { return alive() ? 1.05 : 1.2; }

std::optional<CharacterHit> Character::raycast(const V3& origin, const V3& dir, f64 max_dist) const {
  const V3 c = bounds_center();
  const f64 r = bounds_radius();
  const V3 oc = origin - c;
  const f64 t = -(oc.x * dir.x + oc.y * dir.y + oc.z * dir.z);
  const f64 c2 = oc.x * oc.x + oc.y * oc.y + oc.z * oc.z - t * t;
  if (c2 > r * r || t < -r || t - r > max_dist) return std::nullopt;
  return raycast_model(*model, skin, origin, dir, max_dist);
}

void Character::own_model() {
  if (owns_model) return;
  model = model->clone();
  owns_model = true;
  ++geometry_version;
}

WoundResult Character::wound(const CharacterHit& hit, const V3& dir, f64 damage, f64 radius, f64 impulse_ns) {
  WoundResult res;
  own_model();
  carve_model(*model, hit.rest_point, radius, nullptr, &res.removed);
  // the exit side: a round also opens the body a little further along its path
  const V3 exit = hit.rest_point + rest_dir(hit.bone, dir) * (radius * 2.2);
  carve_model(*model, exit, radius * 0.8, nullptr, &res.removed);
  ++geometry_version;
  const f64 mult = zone_mult(hit.bone, 0.6);
  res.damage = damage * mult;
  res.headshot = hit.bone == H::head || hit.bone == H::neck;
  flash = 1.0;
  const bool was_alive = alive();
  health -= res.damage;
  for (GibSpec& g : sever_after_damage(hit.bone, dir)) res.gibs.push_back(std::move(g));
  if (was_alive) {
    pain_ = 0.25;
    HitInfo info;
    info.point = hit.point;
    info.dir = dir;
    info.force = damage / 30.0;
    info.kind = HitKind::Bullet;
    info.impulse_ns = impulse_ns;
    info.bone = hit.bone;
    res.zone = hit_at(info);
    bool head_off = false;
    for (const GibSpec& g : res.gibs) head_off = head_off || g.part.bone == H::head;
    if (health <= 0.0 || head_off) {
      // (a head shot drops the body at once; elsewhere it goes over a moment)
      die(nullptr, nullptr, res.headshot ? 0.08 : 0.55 + random() * 0.6);
      res.killed = true;
    } else if (health < max_health * 0.3 || std::max(behaviours.injuries.legL, behaviours.injuries.legR) > 0.8) {
      // too hurt to stand: down, writhing
      if (random() < 0.75) behaviours.collapse(5.0 + random() * 9.0);
    }
  } else {
    HitInfo info;info.point = hit.point;info.dir = dir;info.force = damage / 30.0;
    info.kind = HitKind::Bullet;info.bone = hit.bone;info.impulse_ns = impulse_ns;
    res.zone = hit_at(info);
  }
  return res;
}

WoundResult Character::melee(const V3& point, const V3& dir, HitKind kind, f64 force) {
  WoundResult res;
  if (!alive()) {
    HitInfo info;info.point=point;info.dir=dir;info.kind=kind;info.force=kind==HitKind::Blade?force*0.8:force;
    res.zone=hit_at(info);
    return res;
  }
  const i32 bone = nearest_bone(point);
  const f64 mult = kind == HitKind::Blade ? zone_mult(bone, 0.6) * 1.2 : bone == H::head || bone == H::neck ? 1.6 : bone == H::spine ? 1.2 : 0.8;
  res.damage = (kind == HitKind::Blade ? 22.0 : 6.0) * force * mult;
  res.headshot = bone == H::head || bone == H::neck;
  if (kind == HitKind::Blade) {
    own_model();
    const Quat q = pose.q[size_t(bone)];
    // the edge meets the body at its surface: from the bone's axis out towards the blade, a body's
    // depth at most (a blade stopped by the skin still cuts into it)
    const V3 a = pose.p[size_t(bone)];
    const V3 tail = pose.tail(bone);
    const V3 ab = tail - a;
    const f64 l2 = dot(ab, ab);
    const f64 u = l2 > 0.0 ? clamp(dot(point - a, ab) / l2, 0.0, 1.0) : 0.0;
    const V3 axis = a + ab * u;
    const V3 out = point - axis;
    const f64 ol = hypot3(out.x, out.y, out.z);
    const f64 depth = (bone == H::chest || bone == H::spine || bone == H::pelvis ? 0.09 : bone == H::head ? 0.07 : 0.035) * motion.k;
    const V3 at = ol > depth ? axis + out * (depth / ol) : point;
    const V3 rest = model->skeleton->rest_head[size_t(bone)] + rotate(conj(q), at - pose.p[size_t(bone)]);
    carve_model(*model, rest, 0.028, nullptr, &res.removed);
    const V3 exit = rest + rest_dir(bone, dir) * 0.035;
    carve_model(*model, exit, 0.022, nullptr, &res.removed);
    ++geometry_version;
    for (GibSpec& g : sever_after_damage(bone, dir)) res.gibs.push_back(std::move(g));
  }
  flash = kind == HitKind::Blade ? 1.0 : 0.6;
  // fists and feet knock people out rather than kill them
  const bool knockout = kind == HitKind::Blunt && (health - res.damage <= 0.0 || (health - res.damage < max_health * 0.3 && res.headshot && force >= 1.2));
  health = kind == HitKind::Blunt ? std::max(1.0, health - res.damage) : health - res.damage;
  HitInfo info;
  info.point = point;
  info.dir = dir;
  info.force = kind == HitKind::Blade ? force * 0.8 : force;
  info.kind = kind;
  info.bone = bone;
  res.zone = hit_at(info);
  pain_ = 0.3;
  if (knockout) {
    knock_out(6.0 + random() * 5.0);
  } else if (kind == HitKind::Blunt && force >= 2.0 && (res.zone == Zone::Head || res.zone == Zone::Chest)) {
    // a hard blow puts the body down for a moment
    behaviours.knock_out(0.8 + random());
    knocked_out = false;
  }
  if (health <= 0.0) {
    die(nullptr, nullptr, 0.5);
    res.killed = true;
  }
  return res;
}

// Direction `dir` (world) in the rest space of bone b.
V3 Character::rest_dir(i32 b, const V3& dir) const { return rotate(conj(pose.q[size_t(b)]), vnorm(dir)); }

std::vector<GibSpec> Character::sever_after_damage(i32 bone, const V3& dir) {
  std::vector<GibSpec> out;
  VoxelModel& m = *model;
  const Skeleton& sk = *m.skeleton;
  std::vector<i32> bones{bone, sk.parents[size_t(bone)]};
  for (i32 c : sk.children[size_t(bone)]) bones.push_back(c);
  // (a set: each once, in the order first seen)
  std::vector<i32> uniq;
  for (i32 b : bones)
    if (std::find(uniq.begin(), uniq.end(), b) == uniq.end()) uniq.push_back(b);
  for (i32 b : uniq) {
    if (b <= H::pelvis || b == H::weapon || b == H::spine || b == H::chest) continue;
    const i32 pi = m.part_of_bone[size_t(b)];
    if (pi < 0) continue;
    const VoxelPart& part = m.parts[size_t(pi)];
    if (part.count == 0) continue;
    std::vector<VoxelPart> pieces;
    if (part_integrity(part) < 0.55) {
      pieces = detach_subtree(m, b, true);
    } else {
      pieces = sever_disconnected(m, pi, 0.045 * (sk.rest_head[H::pelvis].z / 0.97));
      const V3 tail = sk.rest_tail[size_t(b)];
      bool near = false;
      for (const VoxelPart& p : pieces) near = near || near_tail(p, tail, m.voxel_size);
      if (near)
        for (i32 c : sk.children[size_t(b)])
          for (VoxelPart& p : detach_subtree(m, c, true)) pieces.push_back(std::move(p));
    }
    for (VoxelPart& p : pieces) out.push_back(gib_spec(std::move(p), dir, 2.5));
  }
  if (!out.empty()) {
    ++geometry_version;
    // a limb that came off (most of it gone): the body has no use of it, or of what hung on it; a
    // gun hand gone lets go of the gun
    for (i32 i : {B::upperarmL, B::forearmL, B::handL, B::upperarmR, B::forearmR, B::handR, B::thighL, B::shinL, B::footL, B::thighR, B::shinR, B::footR}) {
      if (behaviours.lost[size_t(i)]) continue;
      const i32 pi = m.part_of_bone[size_t(kBodyBone[size_t(i)])];
      const i32 full = pi >= 0 && size_t(pi) < part_full_.size() ? part_full_[size_t(pi)] : 0;
      if (pi < 0 || full == 0 || m.parts[size_t(pi)].count > 0.4 * full) continue;
      behaviours.lose_limb(i);
      if (weapon && gun_hand_lost()) {
        if (std::optional<GibSpec> w = drop_weapon()) out.push_back(std::move(*w));
      }
    }
  }
  return out;
}

GibSpec Character::gib_spec(VoxelPart p, const V3& dir, f64 speed) {
  GibSpec g;
  const i32 b = p.bone;
  const V3 d = vnorm(dir);
  auto r = [this]() { return random() * 2.0 - 1.0; };
  g.voxel_size = model->voxel_size;
  g.bone_pos = pose.p[size_t(b)];
  g.bone_rot = pose.q[size_t(b)];
  g.bone_rest_head = model->skeleton->rest_head[size_t(b)];
  // (in the original's order: the velocity's randoms, then the spin's)
  const f64 r0 = r(), r1 = r();
  const f64 up = random();
  g.vel = V3{d.x * speed + r0 * 0.8, d.y * speed + r1 * 0.8, d.z * speed + 1.5 + up};
  const f64 a0 = r(), a1 = r(), a2 = r();
  g.ang = V3{a0 * 8.0, a1 * 8.0, a2 * 8.0};
  g.part = std::move(p);
  g.prop = false;
  return g;
}

void Character::die(const V3* point, const V3* dv, f64 collapse) {
  if (!alive()) return;
  health = std::min(health, 0.0);
  if (!behaviours.physical) wake();
  behaviours.die(collapse);
  if (point && dv) impulse(*point, *dv, 0.35);
}

std::optional<GibSpec> Character::drop_weapon() {
  const PropPtr prop = weapon;
  if (!prop) return std::nullopt;
  weapon.reset();
  motion.weapon.reset();
  if (!prop->model || prop->model->parts.empty() || prop->model->parts[0].count == 0) return std::nullopt;
  const RigidBody& hand = *body.parts[B::handR];
  const V3 v = behaviours.physical ? hand.v : (pose.p[H::chest] - prev_pose.p[H::chest]) * (1.0 / last_dt_);
  GibSpec g;
  g.part = prop->model->parts[0];
  g.voxel_size = prop->model->voxel_size;
  g.bone_pos = weapon_pos;
  g.bone_rot = weapon_rot;
  g.bone_rest_head = V3{};
  const f64 r0 = random(), r1 = random();
  g.vel = V3{v.x + (r0 - 0.5), v.y + (r1 - 0.5), v.z + 1.0};
  const f64 a0 = random(), a1 = random(), a2 = random();
  g.ang = V3{a0 * 6.0 - 3.0, a1 * 6.0 - 3.0, a2 * 6.0 - 3.0};
  g.prop = true;
  return g;
}

BlastResult Character::blast(const V3& center, f64 radius, f64 strength) {
  BlastResult res;
  const V3 c = bounds_center();
  const f64 d = vdist(c, center);
  const f64 reach = radius * 3.5;
  if (d > reach) return res;
  const f64 f = std::max(0.0, 1.0 - d / reach);
  const f64 damage = 330.0 * strength * f * f;
  const V3 away = vnorm(c - center, V3{0, 0, 1});
  const bool was_alive = alive();
  health -= damage;
  flash = 1.0;
  bool gibbed = false;
  if (health <= -40.0 || d < radius * 1.3) {
    gibbed = true;
    own_model();
    if (was_alive) die(nullptr, nullptr, 0.0);
    VoxelModel& m = *model;
    const Skeleton& sk = *m.skeleton;
    for (size_t pi = 0; pi < m.parts.size(); ++pi) {
      const VoxelPart& p = m.parts[pi];
      if (p.count == 0) continue;
      const V3 mid = (sk.rest_head[size_t(p.bone)] + sk.rest_tail[size_t(p.bone)]) * 0.5;
      const f64 j0 = random(), j1 = random();
      const std::vector<i32> only{static_cast<i32>(pi)};
      carve_model(m, V3{mid.x + (j0 - 0.5) * 0.1, mid.y + (j1 - 0.5) * 0.1, mid.z}, 0.05, &only, nullptr);
    }
    for (i32 b = 0; b < sk.count; ++b) {
      const i32 pi = m.part_of_bone[size_t(b)];
      if (pi < 0 || m.parts[size_t(pi)].count == 0) continue;
      for (VoxelPart& piece : detach_subtree(m, b, false)) {
        GibSpec g = gib_spec(std::move(piece), away, 4.0 + 8.0 * f * strength);
        const V3 pc = pose.p[size_t(b)];
        const V3 out = vnorm(pc - center, V3{0, 0, 1});
        const f64 r0 = random(), r1 = random(), r2 = random();
        g.vel = V3{out.x * (5.0 + 9.0 * f) + (r0 - 0.5) * 3.0, out.y * (5.0 + 9.0 * f) + (r1 - 0.5) * 3.0, std::abs(out.z) * 6.0 + 3.0 + r2 * 4.0};
        res.gibs.push_back(std::move(g));
      }
    }
    ++geometry_version;
  } else if (health <= 0.0) {
    if (was_alive) die(nullptr, nullptr, 0.0);
    blast_push(center, reach, 9.0 * f * strength + 2.0);
  } else {
    // thrown: a hit on the trunk, the whole body shoved away, dazed
    HitInfo info;
    info.point = pose.p[H::chest];
    info.dir = away;
    info.force = 2.5 * f * strength;
    info.kind = HitKind::Blast;
    info.bone = H::chest;
    hit_at(info);
    // (blast_push falls off with distance itself: near, thrown off the feet; at the edge, a stagger)
    blast_push(center, reach, 7.0 * strength);
    pain_ = 0.3;
  }
  if (!was_alive && !gibbed) blast_push(center, reach, 9.0 * f * strength);
  res.damage = damage;
  res.killed = was_alive && !alive();
  res.gibbed = gibbed;
  return res;
}

// ---- damage kept with the body -------------------------------------------------------------------
//
// The lost limbs (u16: a bit per body part) and the model's cells gone (encode_damage).

static_assert(kBodyCount <= 16);

std::vector<u8> Character::damage_record() const {
  u32 lost = 0;
  for (i32 i = 0; i < kBodyCount; ++i)
    if (behaviours.lost[size_t(i)]) lost |= 1u << i;
  const std::vector<u8> cells = owns_model && whole_ ? encode_damage(*whole_, *model) : std::vector<u8>{};
  if (lost == 0 && cells.empty()) return {};
  std::vector<u8> out{static_cast<u8>(lost), static_cast<u8>(lost >> 8)};
  out.insert(out.end(), cells.begin(), cells.end());
  return out;
}

bool Character::restore_damage(std::span<const u8> record) {
  if (record.empty()) return true;
  if (record.size() < 2) return false;
  const u32 lost = u32(record[0]) | (u32(record[1]) << 8);
  const std::span<const u8> cells = record.subspan(2);
  if (!cells.empty()) {
    // (on a copy: a record that does not fit leaves the model as it was)
    ModelPtr m = model->clone();
    if (!apply_damage(*m, cells)) return false;
    model = std::move(m);
    owns_model = true;
    ++geometry_version;
  }
  for (i32 i = 1; i < kBodyCount; ++i)
    if (lost & (1u << i)) behaviours.lose_limb(i);
  if (gun_hand_lost() && weapon) {
    weapon.reset();
    motion.weapon.reset();
  }
  return true;
}

// ---- where things are -------------------------------------------------------------------------------

V3 Character::muzzle() const { return weapon ? prop_point(weapon->muzzle) : eyes(); }

V3 Character::prop_point(const V3& p) const { return weapon_pos + rotate(weapon_rot, p); }

void Character::write_prop_skin(f32* out) const { write_rigid(out, weapon_pos, weapon_rot, V3{}); }

V3 Character::eyes() const {
  const Skeleton& sk = *model->skeleton;
  const f64 k = motion.k;
  const V3 h = sk.rest_head[H::head];
  return pose.point_of(H::head, V3{h.x, h.y + 0.09 * k, h.z + 0.12 * k});
}

V3 Character::limb_pos(Limb limb) const {
  const Skeleton& sk = *model->skeleton;
  switch (limb) {
    case Limb::HandR:
    case Limb::HandL: {
      const i32 b = limb == Limb::HandR ? H::handR : H::handL;
      return pose.point_of(b, vlerp(sk.rest_head[size_t(b)], sk.rest_tail[size_t(b)], 0.55));
    }
    case Limb::FootR:
    case Limb::FootL: {
      const i32 t = limb == Limb::FootR ? H::toeR : H::toeL;
      return pose.point_of(t, sk.rest_tail[size_t(t)]);
    }
    case Limb::Blade:
    case Limb::Muzzle:
      if (weapon) return prop_point(weapon->muzzle);
      return pose.point_of(H::handR, sk.rest_tail[H::handR]);
    default:
      break;
  }
  return pose.p[H::chest];
}

i32 Character::nearest_bone(const V3& p) const {
  i32 best = H::chest;
  f64 bd = kInf;
  const Skeleton& sk = *model->skeleton;
  for (i32 b = 1; b < sk.count; ++b) {
    if (b == H::weapon || b == H::toeL || b == H::toeR) continue;
    const V3 a = pose.p[size_t(b)];
    const V3 t = pose.tail(b);
    const V3 ab = t - a;
    const f64 l2 = dot(ab, ab);
    const f64 u = l2 > 0.0 ? clamp(dot(p - a, ab) / l2, 0.0, 1.0) : 0.0;
    const f64 d = vdist(p, a + ab * u);
    if (d < bd) {
      bd = d;
      best = b;
    }
  }
  return best;
}

i64 Character::memory_bytes() const {
  i64 n = static_cast<i64>(sizeof(*this)) + body.memory_bytes() - static_cast<i64>(sizeof(HumanoidBody));
  n += static_cast<i64>(skin.capacity() * sizeof(f32) + part_full_.capacity() * sizeof(i32));
  n += static_cast<i64>(3 * pose.p.capacity() * (sizeof(V3) + sizeof(Quat)));
  if (owns_model)
    for (const VoxelPart& p : model->parts) n += static_cast<i64>(p.cells.capacity() + p.shade.capacity());
  return n;
}

// ---- obstacles ------------------------------------------------------------------------------------

void gather_obstacles(const std::vector<Character*>& chars, f64 reach, const std::vector<Obstacle>& extra) {
  // what they did to each other last frame: every push handed on to the body it hit
  for (Character* c : chars)
    for (const Reaction& r : c->body.system.reactions)
      if (r.owner >= 0 && size_t(r.owner) < chars.size() && r.part >= 0 && r.part < kBodyCount) chars[size_t(r.owner)]->pushed_at(r.part, r.j, r.at);
  std::map<size_t, std::vector<Obstacle>> cache;
  auto spheres = [&](size_t i) -> const std::vector<Obstacle>& {
    auto it = cache.find(i);
    if (it == cache.end()) {
      std::vector<Obstacle> s;
      chars[i]->collision_spheres(s, static_cast<i32>(i));
      it = cache.emplace(i, std::move(s)).first;
    }
    return it->second;
  };
  for (size_t a = 0; a < chars.size(); ++a) {
    Character* A = chars[a];
    // (a body in the core collides there; one resting or asleep collides with nothing)
    if (!A->behaviours.physical || A->asleep() || A->bound()) {
      A->set_obstacles({});
      continue;
    }
    const V3 pa = A->pose.p[H::pelvis];
    std::vector<Obstacle> list;
    for (size_t b = 0; b < chars.size(); ++b) {
      if (b == a) continue;
      const V3 pb = chars[b]->pose.p[H::pelvis];
      if (std::abs(pa.x - pb.x) > reach || std::abs(pa.y - pb.y) > reach || std::abs(pa.z - pb.z) > reach) continue;
      const std::vector<Obstacle>& s = spheres(b);
      list.insert(list.end(), s.begin(), s.end());
    }
    for (const Obstacle& o : extra) {
      if (std::abs(pa.x - o.c.x) > reach || std::abs(pa.y - o.c.y) > reach || std::abs(pa.z - o.c.z) > reach) continue;
      list.push_back(o);
    }
    A->set_obstacles(list);
  }
}

}  // namespace svx::anim
