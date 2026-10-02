#include <set>
#include "svx/anim/character.hpp"
namespace svx::anim {
namespace {
// A retained hand becomes part of the persistent loose body's geometry. It cannot be
// evicted by the bounded flesh-debris pool while the item still exists.
void retain_hand(PropInstance& item, const GibSpec& hand) {
  if (!item.damaged_model) item.damaged_model = item.archetype->model->clone();
  auto& model = *item.damaged_model;
  const f64 pitch = model.voxel_size;
  std::map<std::array<i32, 3>, std::pair<u8, u8>> cells;
  const auto& original = model.parts[0];
  for (int z = 0; z < original.dims[2]; ++z)
    for (int y = 0; y < original.dims[1]; ++y)
      for (int x = 0; x < original.dims[0]; ++x) {
        const auto n = size_t(original.index(x, y, z));
        if (original.cells[n])
          cells[{x + original.origin[0], y + original.origin[1], z + original.origin[2]}] = {original.cells[n],
                                                                                             original.shade.empty() ? 128 : original.shade[n]};
      }
  const auto& p = hand.part;
  const int subdivisions = std::max(1, int(std::ceil(hand.voxel_size / pitch)));
  for (int z = 0; z < p.dims[2]; ++z)
    for (int y = 0; y < p.dims[1]; ++y)
      for (int x = 0; x < p.dims[0]; ++x) {
        const auto n = size_t(p.index(x, y, z));
        if (!p.cells[n]) continue;
        for (int a = 0; a < subdivisions; ++a)
          for (int b = 0; b < subdivisions; ++b)
            for (int c = 0; c < subdivisions; ++c) {
              const V3 rest =
                  V3{x + p.origin[0] + (a + .5) / subdivisions, y + p.origin[1] + (b + .5) / subdivisions, z + p.origin[2] + (c + .5) / subdivisions} *
                  hand.voxel_size;
              const V3 world = hand.bone_pos + rotate(hand.bone_rot, rest - hand.bone_rest_head), local = rotate(conj(item.rotation), world - item.pos);
              cells[{i32(std::floor(local.x / pitch)), i32(std::floor(local.y / pitch)), i32(std::floor(local.z / pitch))}] = {
                  p.cells[n], p.shade.empty() ? 128 : p.shade[n]};
            }
      }
  VoxelPart merged;
  merged.bone = 0;
  std::array<i32, 3> hi{-100000, -100000, -100000};
  merged.origin = {100000, 100000, 100000};
  for (const auto& [at, value] : cells)
    for (int i = 0; i < 3; ++i) {
      merged.origin[i] = std::min(merged.origin[i], at[i]);
      hi[i] = std::max(hi[i], at[i]);
    }
  size_t size = 1;
  for (int i = 0; i < 3; ++i) {
    merged.dims[i] = hi[i] - merged.origin[i] + 1;
    size *= size_t(merged.dims[i]);
  }
  merged.cells.resize(size);
  merged.shade.resize(size, 128);
  for (const auto& [at, value] : cells) {
    const auto n = size_t(merged.index(at[0] - merged.origin[0], at[1] - merged.origin[1], at[2] - merged.origin[2]));
    merged.cells[n] = value.first;
    merged.shade[n] = value.second;
  }
  merged.count = merged.initial_count = i32(cells.size());
  model.parts[0] = std::move(merged);
  item.retained_mass = .45;
  ++item.geometry_version;
}
}  // namespace

WoundResult Character::damage(const DamageDescriptor& source) {
  WoundResult out;
  if (!source.valid() || source.kind == DamageKind::Thermal) return out;
  if (source.kind != DamageKind::Blast && source.energy() == 0) return out;
  std::array<PropInstancePtr, 2> hands{motion.props.at(AttachPoint::LeftHand), motion.props.at(AttachPoint::RightHand)};
  std::array<bool, 2> strong{hands[0] && hands[0]->strength > .7, hands[1] && hands[1]->strength > .7};
  DamageDescriptor d = source;
  d.direction = vnorm(d.direction);
  const bool was_alive = alive();
  const f64 before = health;
  if (d.kind == DamageKind::Blast) {
    const f64 distance = vdist(d.point, bounds_center());
    if (distance > d.radius * 3.5) return out;
    const f64 falloff = std::max(0.0, 1 - distance / (d.radius * 3.5));
    // The body remains coherent. Nearby fragments can sever a limb through the
    // same anatomy.
    for (i32 i = 0; i < d.fragments; ++i) {
      const i32 part = i % kBodyCount;
      const V3 at = pose.p[size_t(kBodyBone[size_t(part)])];
      DamageDescriptor f;
      f.kind = DamageKind::Projectile;
      f.mass = .001;
      f.speed = 350 * falloff;
      f.diameter = .004;
      f.point = d.point;
      f.direction = vnorm(at - d.point);
      f.bone = kBodyBone[size_t(part)];
      if (auto hit = raycast(f.point, f.direction, d.radius * 3.5)) {
        f.point = hit->point;
        auto result = damage(f);
        out.removed.insert(out.removed.end(), result.removed.begin(), result.removed.end());
        out.gibs.insert(out.gibs.end(), result.gibs.begin(), result.gibs.end());
      }
    }
    if (distance < d.radius * .5) {
      std::array<i32, 4> limbs{H::forearmL, H::forearmR, H::shinL, H::shinR};
      std::stable_sort(limbs.begin(), limbs.end(),
                       [&](i32 a, i32 b) { return vdist(pose.p[size_t(a)], d.point) < vdist(pose.p[size_t(b)], d.point); });
      const int torn = distance < d.radius * .2 ? 2 : 1;
      for (int i = 0; i < torn; ++i) {
        DamageDescriptor crush = d;
        crush.kind = DamageKind::Crush;
        crush.bone = limbs[size_t(i)];
        crush.point = vlerp(pose.p[size_t(crush.bone)], pose.tail(crush.bone), .5);
        crush.direction = vnorm(crush.point - d.point);
        crush.mass = 12;
        crush.speed = std::sqrt(2 * 6000 * (1 - distance / (d.radius * .5)) / crush.mass);
        crush.area = .018;
        auto result = damage(crush);
        out.removed.insert(out.removed.end(), result.removed.begin(), result.removed.end());
        out.gibs.insert(out.gibs.end(), result.gibs.begin(), result.gibs.end());
      }
    }
    DamageDescriptor pressure;
    pressure.kind = DamageKind::Crush;
    pressure.point = pose.p[H::chest];
    pressure.direction = d.direction;
    pressure.mass = 8;
    pressure.speed = std::sqrt(d.pressure / 100000) * 10 * falloff;
    pressure.area = .25;
    pressure.bone = H::chest;
    auto result = damage(pressure);
    out.removed.insert(out.removed.end(), result.removed.begin(), result.removed.end());
    out.gibs.insert(out.gibs.end(), result.gibs.begin(), result.gibs.end());
    blast_push(d.point, d.radius * 3.5, 7 * std::sqrt(d.pressure / 100000));
    out.killed = was_alive && !alive();
    out.damage = before - health;
    return out;
  }
  // A held or worn object in front of the entry point absorbs energy first.
  struct ShieldHit {
    PropInstancePtr item;
    CharacterHit hit;
  };
  std::vector<ShieldHit> shields;
  const V3 origin = d.point - d.direction * .7;
  for (const auto& p : motion.props.slots)
    if (p) {
      std::array<f32, 16> matrix{};
      write_rigid(matrix.data(), p->pos, p->rotation, {});
      const auto hit = raycast_model(p->model(), matrix, origin, d.direction, .75);
      if (hit && (!d.target_prop || d.target_prop == p->id)) shields.push_back({p, *hit});
    }
  std::stable_sort(shields.begin(), shields.end(), [](const auto& a, const auto& b) { return a.hit.distance < b.hit.distance; });
  for (const auto& shield : shields) {
    const auto& p = shield.item;
    const auto* hit = &shield.hit;
    std::array<f32, 16> matrix{};
    write_rigid(matrix.data(), p->pos, p->rotation, {});
    if (!p->damaged_model) p->damaged_model = p->archetype->model->clone();
    DamageDescriptor pd = d;
    pd.point = hit->point;
    auto result = wound_mechanics(*p->damaged_model, matrix, pd, &p->archetype->material);
    if (!result.changed_bones.empty()) ++p->geometry_version;
    const f64 absorbed = std::max(0.0, d.energy() - result.remaining_energy);
    p->state.condition = clamp(p->state.condition - absorbed / 800, 0.0, 1.0);
    p->impulse_load += norm(result.impulse(pd)) / .08;
    if (!hand_point(p->point) && d.kind == DamageKind::Edge &&
        vdist(hit->point, p->pos + rotate(p->rotation, p->archetype->socket(p->socket)->point)) < .12)
      p->state.strap = std::max(0.0, p->state.strap - d.energy() / 250);
    const f64 remaining = d.energy() - absorbed;
    d.speed = std::sqrt(2 * remaining / d.mass);
    d.blocked = true;
    if (remaining <= 0) {
      update_props(0);
      return out;
    }
  }
  own_model();
  auto result = wound_mechanics(*model, skin, d);
  if (result.tissue.empty()) return out;
  const f64 absorbed = std::max(0.0, d.energy() - result.remaining_energy);
  const V3 impulse = result.impulse(d);
  const auto entry = d.kind == DamageKind::Projectile || d.kind == DamageKind::Point
                         ? result.tissue.begin()
                         : std::min_element(result.tissue.begin(), result.tissue.end(), [&](const auto& a, const auto& b) {
                             return vdist(pose.point_of(a.bone, a.rest), d.point) < vdist(pose.point_of(b.bone, b.rest), d.point);
                           });
  behaviours.damage.apply(*model, pose, d, result);
  out.removed = result.removed;
  std::set<i32> cut_bones;
  for (const auto& cell : result.removed) cut_bones.insert(cell.bone);
  for (i32 bone : cut_bones)
    for (auto& g : sever_after_damage(bone, d.direction)) out.gibs.push_back(std::move(g));
  if (!result.changed_bones.empty()) ++geometry_version;
  behaviours.damage.update(0);
  HitInfo hit;
  hit.point = pose.point_of(entry->bone, entry->rest);
  hit.dir = vnorm(impulse, d.direction);
  hit.bone = entry->bone;
  hit.kind = d.kind == DamageKind::Projectile                            ? HitKind::Bullet
             : d.kind == DamageKind::Edge || d.kind == DamageKind::Point ? HitKind::Blade
                                                                         : HitKind::Blunt;
  hit.force = clamp(std::sqrt(absorbed / 100), 0.0, 8.0);
  hit.impulse_ns = d.impulse_delivered ? 0 : norm(impulse);
  out.zone = hit_at(hit);
  out.headshot = hit.bone == H::head || hit.bone == H::neck;
  health = was_alive ? max_health * behaviours.damage.health_fraction() : 0;
  f64 head_energy = 0;
  for (const auto& tissue : result.tissue)
    if (HumanoidBody::body_of_bone(tissue.bone) == B::head) head_energy += tissue.energy;
  if ((d.kind == DamageKind::Blunt || d.kind == DamageKind::Crush) && head_energy > 300 && alive()) knock_out(clamp(head_energy / 40, 2.0, 12.0));

  flash = 1;
  out.killed = was_alive && !alive();
  out.damage = std::max(0.0, before - health);
  auto debris = wound_gibs(effects, *this, out, d.point, d.direction);
  for (size_t side = 0; side < 2; ++side)
    if (hands[side] && behaviours.lost[side == 0 ? B::handL : B::handR]) {
      auto item = detach(hands[side]->point, ReleaseReason::AnchorLost);
      if (!item) continue;
      if (strong[side])
        for (size_t i = 0; i < out.gibs.size(); ++i)
          if (out.gibs[i].part.bone == (side == 0 ? H::handL : H::handR)) {
            retain_hand(*item, out.gibs[i]);
            if (i < debris.size()) effects.remove(debris[i]);
            break;
          }
    }
  if (capabilities().fatal) die(nullptr, nullptr, out.headshot ? .08 : .55);
  out.killed = was_alive && !alive();
  out.damage = std::max(0.0, before - health);
  if (result.exited) effects.spray(result.exit, d.direction, 8, 3, .25);
  const i32 part = HumanoidBody::body_of_bone(hit.bone);
  for (const auto& p : motion.props.slots)
    if (p && hand_point(p->point)) {
      const i32 arm = p->point == AttachPoint::LeftHand ? B::upperarmL : B::upperarmR;
      if (part >= arm && part <= arm + 2) {
        p->impulse_load += norm(impulse) / .06;
        if (capabilities().arms[p->point == AttachPoint::LeftHand ? 0 : 1].grip < .2) detach(p->point, ReleaseReason::HandDamaged);
      }
    }
  return out;
}
}  // namespace svx::anim
