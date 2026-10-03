// DamageModel::Zones (CharacterProfile; legacy_profile()): svx_anim's damage before tissue mechanics
// and physiology (engine main 0eda3ca) - hit points scaled by the zone hit, spherical carves, limbs
// severed when their part falls apart, the head off or the hit points gone: dead; a blast that
// tears the body apart near its centre. Kept as it was so the behavioural baseline stays
// reproducible.
#include "svx/anim/character.hpp"

namespace svx::anim {

namespace {

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

WoundResult Character::zone_damage(const DamageDescriptor& source) {
  DamageDescriptor d = source;
  d.direction = vnorm(d.direction);
  switch (d.kind) {
    case DamageKind::Projectile:
    case DamageKind::Point: {
      const auto hit = raycast(d.point - d.direction * .3, d.direction, .6);
      if (!hit) return {};
      return zone_wound(*hit, d.direction, d.energy() * .07, clamp(d.diameter * 5, .02, .1), 2.5);
    }
    case DamageKind::Blunt:
    case DamageKind::Edge:
    case DamageKind::Crush:
      return zone_melee(d.point, d.direction, d.kind == DamageKind::Edge && d.alignment > .15 ? HitKind::Blade : HitKind::Blunt, clamp(d.speed / 6, 0.0, 8.0));
    case DamageKind::Blast: {
      const BlastResult b = zone_blast(d.point, d.radius, d.pressure / 100000);
      WoundResult out;
      out.damage = b.damage;
      out.killed = b.killed;
      out.gibs = b.gibs;
      return out;
    }
    case DamageKind::Thermal: break;
  }
  return {};
}

WoundResult Character::zone_wound(const CharacterHit& hit, const V3& dir, f64 damage, f64 radius, f64 impulse_speed) {
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
  for (GibSpec& g : zone_sever(hit.bone, dir)) res.gibs.push_back(std::move(g));
  if (was_alive) {
    pain_ = 0.25;
    HitInfo info;
    info.point = hit.point;
    info.dir = dir;
    info.force = damage / 30.0;
    info.kind = HitKind::Bullet;
    info.bone = hit.bone;
    res.zone = hit_at(info);
    bool head_off = false;
    for (const GibSpec& g : res.gibs) head_off = head_off || g.part.bone == H::head;
    const Injuries& inj = behaviours.damage.injuries();
    if (health <= 0.0 || head_off) {
      // (a head shot drops the body at once; elsewhere it goes over a moment)
      die(nullptr, nullptr, res.headshot ? 0.08 : 0.55 + random() * 0.6);
      res.killed = true;
    } else if (health < max_health * 0.3 || std::max(inj.legL, inj.legR) > 0.8) {
      // too hurt to stand: down, writhing
      if (random() < 0.75) behaviours.collapse(5.0 + random() * 9.0);
    }
  } else {
    impulse(hit.point, vnorm(dir) * (impulse_speed * 1.4));
  }
  return res;
}

WoundResult Character::zone_melee(const V3& point, const V3& dir, HitKind kind, f64 force) {
  WoundResult res;
  if (!alive()) {
    impulse(point, vnorm(dir) * (2.0 * force));
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
    for (GibSpec& g : zone_sever(bone, dir)) res.gibs.push_back(std::move(g));
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
    behaviours.knock_out(0.8 + random(), true);
  }
  if (health <= 0.0) {
    die(nullptr, nullptr, 0.5);
    res.killed = true;
  }
  return res;
}

std::vector<GibSpec> Character::zone_sever(i32 bone, const V3& dir) {
  std::vector<GibSpec> out;
  VoxelModel& m = *model;
  const Skeleton& sk = *m.skeleton;
  std::vector<i32> bones{bone, sk.parents[size_t(bone)]};
  for (i32 c : sk.children[size_t(bone)]) bones.push_back(c);
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
      if (i >= B::upperarmR && i <= B::handR) drop_weapon();
    }
  }
  return out;
}

BlastResult Character::zone_blast(const V3& center, f64 radius, f64 strength) {
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
    blast_push(center, reach, 7.0 * strength);
    pain_ = 0.3;
  }
  if (!was_alive && !gibbed) blast_push(center, reach, 9.0 * f * strength);
  res.damage = damage;
  res.killed = was_alive && !alive();
  res.gibbed = gibbed;
  return res;
}

}  // namespace svx::anim
