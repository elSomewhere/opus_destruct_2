#include "svx/anim/damage/scenarios.hpp"
#include "svx/anim/character.hpp"
namespace svx::anim {
std::vector<DamageDescriptor> damage_scenario(const Character& c, std::string_view name) {
  std::vector<DamageDescriptor> out;
  const auto& sk = *c.model->skeleton;
  auto on_bone = [&](i32 bone, f64 along) { return vlerp(c.pose.p[size_t(bone)], c.pose.tail(bone), along); };
  auto entry = [&](DamageDescriptor& d, const V3& target) {
    const V3 from = target - d.direction * .4;
    const i32 pi = c.model->part_of_bone[size_t(d.bone)];
    if (pi >= 0) {
      VoxelModel part(c.model->skeleton, c.model->voxel_size, {c.model->parts[size_t(pi)]});
      const auto hit = raycast_model(part, c.skin, from, d.direction, .8);
      d.point = hit ? hit->point : target;
    } else
      d.point = target;
  };
  DamageDescriptor d;
  d.kind = DamageKind::Projectile;
  d.mass = .008;
  d.speed = 350;
  d.diameter = .009;
  d.impact_scale = 30;
  d.bone = H::thighL;
  d.direction = rotate(c.pose.q[H::pelvis], V3{0, -1, 0});
  V3 target = on_bone(H::thighL, .6) + rotate(c.pose.q[H::thighL], V3{-.05, 0, 0});
  if (name == "femoralBleed") {
    for (const auto& v : anatomy_regions(sk))
      if (v.name == "left femoral") target = c.pose.point_of(v.bone, v.centre);
    d.speed = 440;
  } else if (name == "shatteredKnee" || name == "batKnee") {
    target = on_bone(H::shinL, .05);
    d.bone = H::shinL;
    d.kind = DamageKind::Blunt;
    d.mass = name == "batKnee" ? 1.8 : 5;
    d.speed = name == "batKnee" ? 11 : 16;
    d.area = .006;
  } else if (name == "forearmSever") {
    d.kind = DamageKind::Edge;
    d.bone = H::forearmR;
    d.mass = 3;
    d.speed = 36;
    d.swept_length = .4;
    target = on_bone(d.bone, .55);
    const V3 axis = vnorm(c.pose.tail(d.bone) - c.pose.p[size_t(d.bone)]);
    d.direction = vnorm(cross(axis, V3{0, 0, 1}), V3{0, -1, 0});
    const V3 edge = vnorm(cross(axis, d.direction));
    d.edge_a = target - edge * .2;
    d.edge_b = target + edge * .2;
  } else if (name == "backSlash") {
    d.kind = DamageKind::Edge;
    d.bone = H::chest;
    d.mass = 1.4;
    d.speed = 15;
    d.swept_length = .5;
    target = on_bone(d.bone, .3);
    d.direction = -d.direction;
    const V3 edge = rotate(c.pose.q[H::chest], V3{1, 0, 0});
    d.edge_a = target - edge * .25;
    d.edge_b = target + edge * .25;
  } else if (name == "shotgunLegs") {
    for (int i = 0; i < 8; ++i) {
      d.bone = i < 4 ? H::shinL : H::shinR;
      d.mass = .0035;
      d.speed = 410;
      d.diameter = .0084;
      d.construction = ProjectileConstruction::Buckshot;
      d.impact_scale = 1;
      target = on_bone(d.bone, .07 + .2 * (i % 4));
      entry(d, target);
      out.push_back(d);
    }
    return out;
  } else if (name == "blast3m") {
    d.kind = DamageKind::Blast;
    d.point = c.pose.p[H::pelvis] + V3{0, 3, 0};
    d.radius = 3;
    d.pressure = 100000;
    d.fragments = 24;
    out.push_back(d);
    return out;
  } else if (name == "gutStab") {
    d.kind = DamageKind::Point;
    d.bone = H::spine;
    d.mass = 2;
    d.speed = 11;
    d.diameter = .018;
    target = on_bone(d.bone, .2);
  } else if (name != "thighShot")
    return out;
  entry(d, target);
  out.push_back(d);
  return out;
}
}  // namespace svx::anim
