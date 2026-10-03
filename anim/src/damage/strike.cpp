#include "svx/anim/damage/strike.hpp"
namespace svx::anim {
V3 StrikeSweep::velocity(f64 along) const {
  if (!(descriptor.duration > 0)) return {};
  const V3 va = (a - previous_a) * (1 / descriptor.duration), vb = (b - previous_b) * (1 / descriptor.duration);
  const f64 fastest = std::max(norm(va), norm(vb));
  return vlerp(va, vb, clamp(along, 0.0, 1.0)) * (fastest > speed_limit ? speed_limit / fastest : 1);
}
DamageDescriptor StrikeSweep::impact(f64 along, const V3& point, const V3& target_velocity, f64 time) const {
  auto d = descriptor;
  const V3 relative = velocity(along) - target_velocity;
  d.point = point;
  d.speed = norm(relative);
  d.direction = vnorm(relative, descriptor.direction);
  d.edge_a = vlerp(previous_a, a, clamp(time, 0.0, 1.0));
  d.edge_b = vlerp(previous_b, b, clamp(time, 0.0, 1.0));
  if (d.kind == DamageKind::Edge && norm2(normal) > 1e-12) {
    const V3 edge = vnorm(d.edge_b - d.edge_a);
    const V3 flat = vnorm(vlerp(previous_normal, normal, clamp(time, 0.0, 1.0)), normal);
    d.alignment = clamp(std::abs(dot(vnorm(cross(flat, edge)), d.direction)), 0.0, 1.0);
  }
  return d;
}
std::vector<StrikeSweep> StrikeTracker::sample(const Character& c, f64 dt) {
  std::vector<StrikeSweep> out;
  if (!(dt > 0) || !std::isfinite(dt)) return out;
  const auto* action = action_def(c.motion.action_name());
  if (!action) {
    previous_.clear();
    return out;
  }
  if (serial_ != c.motion.action_serial) {
    previous_.clear();
    serial_ = c.motion.action_serial;
  }
  std::map<std::string, FeaturePose> current;
  for (const auto& event : action->events)
    if (event.name == "strike") {
      StrikeSweep s;
      s.serial = serial_;
      auto& d = s.descriptor;
      d.attacker = c.motion.props.owner;
      d.feature = event.feature;
      d.duration = dt;
      const auto item = c.motion.props.held();
      const auto* f = item ? item->archetype->feature(event.feature) : nullptr;
      // Losing the striking feature does not turn a weapon attack into a fist.
      if (!event.feature.empty() && !f) continue;
      if (f) {
        s.a = item->pos + rotate(item->rotation, f->a);
        s.b = item->pos + rotate(item->rotation, f->b);
        d.prop = item->id;
        d.kind = f->impact == ImpactClass::Edge ? DamageKind::Edge : f->impact == ImpactClass::Point ? DamageKind::Point : DamageKind::Blunt;
        d.mass = item->archetype->mass + 1.5;
        d.sharpness = f->sharpness;
        d.diameter = std::max(.004, 2 * f->radius);
        d.area = kPi * f->radius * f->radius;
        if (f->impact == ImpactClass::Edge) {
          const V3 size = item->archetype->dimensions, n = vnorm(f->normal);
          d.area = std::max(d.area, std::abs(n.x) * size.y * size.z + std::abs(n.y) * size.x * size.z + std::abs(n.z) * size.x * size.y);
        }
        s.normal = rotate(item->rotation, f->normal);
      } else {
        s.a = s.b = c.limb_pos(event.limb);
        d.kind = DamageKind::Blunt;
        d.mass = event.limb == Limb::FootL || event.limb == Limb::FootR ? 5 : 2.5;
        d.area = event.limb == Limb::FootL || event.limb == Limb::FootR ? .018 : .0095;
      }
      const std::string key = std::to_string(d.prop) + "/" + event.feature + "/" + std::to_string(int(event.limb));
      const auto old = previous_.find(key);
      s.previous_a = old == previous_.end() ? s.a : old->second.a;
      s.previous_b = old == previous_.end() ? s.b : old->second.b;
      s.previous_normal = old == previous_.end() ? s.normal : old->second.normal;
      current[key] = {s.a, s.b, s.normal};
      s.speed_limit = f ? 22.0 : (event.limb == Limb::FootL || event.limb == Limb::FootR ? 16.0 : 12.0);
      d.swept_length = vdist(s.a, s.b);
      d = s.impact(.5, (s.a + s.b) * .5);
      const size_t hand = action->left_handed ? 0 : 1;
      const bool active = c.motion.strike_weight[hand] > .05 || c.motion.strike_weight[2] > .05 || c.motion.strike_weight[3] > .05 ||
                          std::abs(c.motion.action_time() - event.t) < .1;
      if (active && old != previous_.end() && std::max(norm(s.velocity(0)), norm(s.velocity(1))) > .05) out.push_back(std::move(s));
    }
  previous_ = std::move(current);
  return out;
}
std::optional<DamageDescriptor> StrikeTracker::contact(const StrikeSweep& s, const Character& target) {
  std::optional<DamageDescriptor> result;
  f64 best = 1e30;
  // Trace the swept ruled surface at intervals no wider than a target voxel.
  const int samples =
      std::clamp(int(std::ceil(std::max(vdist(s.a, s.b), vdist(s.previous_a, s.previous_b)) / std::max(.004, target.model->voxel_size * .5))), 1, 256);
  for (int i = 0; i <= samples; ++i) {
    const f64 t = f64(i) / samples;
    const V3 from = vlerp(s.previous_a, s.previous_b, t), to = vlerp(s.a, s.b, t);
    const V3 delta = to - from;
    const f64 length = norm(delta);
    if (length < 1e-9) continue;
    const V3 direction = delta * (1 / length), u = vnorm(cross(direction, V3{0, 0, 1}), V3{1, 0, 0}), v = cross(direction, u);
    const f64 radius = s.descriptor.kind == DamageKind::Blunt ? std::sqrt(s.descriptor.area / kPi) : s.descriptor.diameter * .5;
    for (const V3 offset : {V3{}, u * radius, -u * radius, v * radius, -v * radius}) {
      const V3 origin = from + offset;
      const auto hit = target.raycast(origin, direction, length + radius);
      if (hit && hit->bone >= 0 && hit->distance / length < best) {
        const i32 part = HumanoidBody::body_of_bone(hit->bone);
        auto d = s.impact(t, hit->point, target.velocity_at(part, hit->point), hit->distance / length);
        d.bone = hit->bone;
        // A surface moving with or away from the feature is not an impact.
        if (d.speed > .05 && dot(d.direction, direction) > 1e-6) {
          best = hit->distance / length;
          result = d;
        }
      }
      for (const auto& p : target.motion.props.slots)
        if (p) {
          std::array<f32, 16> skin;
          write_rigid(skin.data(), p->pos, p->rotation, {});
          auto ph = raycast_model(p->model(), skin, origin, direction, length + radius);
          if (ph && ph->distance / length < best) {
            const i32 part = HumanoidBody::body_of_bone(attachment_bone(p->point));
            auto d = s.impact(t, ph->point, target.velocity_at(part, ph->point), ph->distance / length);
            d.target_prop = p->id;
            d.blocked = true;
            if (d.speed > .05 && dot(d.direction, direction) > 1e-6) {
              best = ph->distance / length;
              result = d;
            }
          }
        }
    }
  }
  return result;
}
}  // namespace svx::anim
