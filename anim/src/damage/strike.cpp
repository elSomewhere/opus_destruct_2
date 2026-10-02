#include "svx/anim/damage/strike.hpp"
namespace svx::anim {
std::vector<StrikeSweep> StrikeTracker::sample(const Character& c, f64 dt) {
  std::vector<StrikeSweep> out;
  const auto* action = action_def(c.motion.action_name());
  if (!action) {
    previous_.clear();
    return out;
  }
  if (serial_ != c.motion.action_serial) {
    previous_.clear();
    serial_ = c.motion.action_serial;
  }
  for (const auto& event : action->events)
    if (event.name == "strike") {
      StrikeSweep s;
      s.serial = serial_;
      auto& d = s.descriptor;
      d.attacker = c.motion.props.owner;
      d.feature = event.feature;
      const auto item = c.motion.props.held();
      const auto* f = item ? item->archetype->feature(event.feature) : nullptr;
      if (f) {
        s.a = item->pos + rotate(item->rotation, f->a);
        s.b = item->pos + rotate(item->rotation, f->b);
        d.prop = item->id;
        d.kind = f->impact == ImpactClass::Edge ? DamageKind::Edge : f->impact == ImpactClass::Point ? DamageKind::Point : DamageKind::Blunt;
        d.mass = item->archetype->mass + 1.5;
        d.sharpness = f->sharpness;
        d.diameter = std::max(.004, 2 * f->radius);
        d.area = kPi * f->radius * f->radius;
      } else {
        s.a = s.b = c.limb_pos(event.limb);
        d.kind = DamageKind::Blunt;
        d.mass = event.limb == Limb::FootL || event.limb == Limb::FootR ? 5 : 2.5;
        d.area = event.limb == Limb::FootL || event.limb == Limb::FootR ? .018 : .0095;
      }
      const std::string key = event.feature + std::to_string(int(event.limb));
      const auto old = previous_.find(key);
      s.previous_a = old == previous_.end() ? s.a : old->second.first;
      s.previous_b = old == previous_.end() ? s.b : old->second.second;
      previous_[key] = {s.a, s.b};
      const V3 movement = ((s.a + s.b) - (s.previous_a + s.previous_b)) * .5;
      d.speed = std::min(f ? 22.0 : (event.limb == Limb::FootL || event.limb == Limb::FootR ? 16.0 : 12.0), norm(movement) / std::max(1e-6, dt));
      d.direction = vnorm(movement, V3{0, 1, 0});
      d.point = (s.a + s.b) * .5;
      d.edge_a = s.a;
      d.edge_b = s.b;
      d.swept_length = vdist(s.a, s.b);
      if (f) {
        const V3 edge = vnorm(s.b - s.a);
        d.alignment = clamp(norm(cross(edge, d.direction)), .05, 1.0);
      }
      const size_t hand = action->left_handed ? 0 : 1;
      const bool active = c.motion.strike_weight[hand] > .05 || c.motion.strike_weight[2] > .05 || c.motion.strike_weight[3] > .05 ||
                          std::abs(c.motion.action_time() - event.t) < .1;
      if (active && old != previous_.end() && d.speed > .05) out.push_back(std::move(s));
    }
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
      if (hit && hit->bone >= 0 && vdist(origin, hit->point) < best) {
        best = vdist(origin, hit->point);
        auto d = s.descriptor;
        d.point = hit->point;
        d.bone = hit->bone;
        const auto& body = *target.body.parts[size_t(HumanoidBody::body_of_bone(hit->bone))];
        d.speed = std::max(.05, d.speed - dot(body.v, d.direction));
        result = d;
      }
      for (const auto& p : target.motion.props.slots)
        if (p) {
          std::array<f32, 16> skin;
          write_rigid(skin.data(), p->pos, p->rotation, {});
          auto ph = raycast_model(p->model(), skin, origin, direction, length + radius);
          if (ph && vdist(origin, ph->point) < best) {
            best = vdist(origin, ph->point);
            auto d = s.descriptor;
            d.point = ph->point;
            d.target_prop = p->id;
            d.blocked = true;
            result = d;
          }
        }
    }
  }
  return result;
}
}  // namespace svx::anim
