#include "svx/anim/character.hpp"
namespace svx::anim {
bool Character::attach(const PropInstancePtr& p, AttachPoint point, std::string_view socket, WieldStyle style) {
  if (!motion.props.attach(p, point, socket, style)) return false;
  motion.weapon_hand = attachment_bone(point);
  update_props(0);
  update_load();
  return true;
}
PropInstancePtr Character::detach(AttachPoint point, ReleaseReason reason) {
  auto p = motion.props.at(point);
  if (!p) return {};
  const i32 bone = attachment_bone(point), part = HumanoidBody::body_of_bone(bone);
  const auto& anchor = *body.parts[size_t(part)];
  p->velocity =
      behaviours.physical ? anchor.v + cross(anchor.w, p->pos - anchor.x) : (pose.p[size_t(bone)] - prev_pose.p[size_t(bone)]) * (1 / std::max(last_dt_, 1e-6));
  p->angular = behaviours.physical ? anchor.w : V3{};
  auto out = motion.props.detach(point, reason);
  update_load();
  return out;
}
bool Character::swap(PropPtr a, AttachPoint point, std::string_view socket, WieldStyle style) {
  if (size_t(point) >= size_t(AttachPoint::Count)) {
    motion.props.refusal = "invalid attachment";
    return false;
  }
  if (!a) {
    detach(point);
    return true;
  }
  // Validate using the same occupancy rules before replacing anything.
  Attachments trial;
  trial.slots = motion.props.slots;
  trial.slots[size_t(point)].reset();
  auto candidate = std::make_shared<PropInstance>();
  candidate->archetype = a;
  if (!trial.attach(candidate, point, socket, style)) {
    motion.props.refusal = trial.refusal;
    return false;
  }
  detach(point);
  return attach(motion.props.registry->create(a), point, socket, style);
}
void Character::wrench(AttachPoint point, const V3& impulse) {
  if (auto p = motion.props.at(point)) {
    p->impulse_load += norm(impulse) / .08;
    pushed_at(HumanoidBody::body_of_bone(attachment_bone(point)), impulse, p->pos);
    if (p->impulse_load > p->archetype->socket(p->socket)->retention * p->strength) detach(point, ReleaseReason::Wrenched);
  }
}
std::vector<AttachmentEvent> Character::take_attachment_events() {
  std::vector<AttachmentEvent> out;
  out.swap(motion.props.events);
  return out;
}
void Character::update_load() {
  load_revision_ = motion.props.revision;
  auto masses = bare_mass_;
  auto inertias = bare_inertia_;
  V3 moment;
  f64 total = 0;
  for (const auto& p : motion.props.slots)
    if (p) {
      const auto& a = *p->archetype;
      const size_t part = size_t(HumanoidBody::body_of_bone(attachment_bone(p->point)));
      const auto* socket = a.socket(p->socket);
      const V3 offset = a.centre - (socket ? socket->point : V3{});
      masses[part] += a.mass;
      inertias[part] +=
          a.inertia +
          V3{offset.y * offset.y + offset.z * offset.z, offset.x * offset.x + offset.z * offset.z, offset.x * offset.x + offset.y * offset.y} * a.mass;
      total += a.mass;
      const V3 local = p->point == AttachPoint::Back       ? V3{0, -.22, 0}
                       : p->point == AttachPoint::Shoulder ? V3{-.25, -.08, 0}
                       : hand_point(p->point)              ? V3{p->point == AttachPoint::LeftHand ? -.4 : .4, .05, 0}
                                                           : V3{};
      moment += (local + rotate(a.hanging_rotation, offset)) * a.mass;
    }
  for (size_t i = 0; i < kBodyCount; ++i) {
    auto& b = *body.parts[i];
    if (b.gone) continue;
    b.mass = masses[i];
    b.inv_mass = 1 / b.mass;
    b.inv_i = {1 / inertias[i].x, 1 / inertias[i].y, 1 / inertias[i].z};
    b.update_inertia();
    if (bound() && world_) world_->set_link_mass(articulation(), u16(i), b.mass, inertias[i]);
  }
  body.refresh_mass();
  if (std::none_of(body.parts.begin(), body.parts.end(), [](const auto* b) { return b->gone; })) body.total_mass = bare_total_ + total;
  motion.load_fraction = clamp(total / 20, 0.0, 1.0);
  motion.load_lean = {clamp(moment.y * .025, -.22, .22), clamp(-moment.x * .025, -.22, .22), 0};
}
void Character::update_props(f64 dt) {
  for (size_t i = 0; i < motion.props.slots.size(); ++i) {
    const auto p = motion.props.slots[i];
    if (!p) continue;
    const i32 bone = attachment_bone(p->point), part = HumanoidBody::body_of_bone(bone);
    const auto& anchor = *body.parts[size_t(part)];
    const auto& a = *p->archetype;
    const auto* socket = a.socket(p->socket);
    const bool held = hand_point(p->point), soft = p->style == WieldStyle::Hanging || p->point == AttachPoint::Back || p->point == AttachPoint::Shoulder;
    if (held && p == motion.props.held()) {
      p->pos = weapon_pos;
      p->rotation = weapon_rot;
    } else {
      V3 offset;
      if (p->point == AttachPoint::Back)
        offset = {0, -.19, .05};
      else if (p->point == AttachPoint::Shoulder)
        offset = {-.25, -.05, -.15};
      else if (p->point == AttachPoint::Hip)
        offset = {.19, -.04, 0};
      else if (p->point == AttachPoint::Thigh)
        offset = {.12, 0, -.12};
      else if (p->point == AttachPoint::Chest || p->point == AttachPoint::Arms)
        offset = {0, .2, 0};
      p->rotation = pose.q[size_t(bone)];
      if (held) p->rotation = p->rotation * conj(motion.arms.canonical(p->point == AttachPoint::LeftHand ? Side::L : Side::R)) * a.hanging_rotation;
      p->rotation = p->rotation * (socket ? conj(socket->rotation) : Quat{});
      p->pos = pose.p[size_t(bone)] + rotate(pose.q[size_t(bone)], offset * motion.k) - rotate(p->rotation, socket ? socket->point : V3{});
    }
    if (p->style == WieldStyle::Hanging) {
      // A handle transmits force while letting the load hang under gravity.
      const V3 pivot = p->pos + rotate(p->rotation, socket->point);
      p->rotation = qz(motion.root_yaw - kPi / 2);
      p->pos = pivot - rotate(p->rotation, socket->point);
    }
    if (dt > 0) {
      const V3 acceleration = (anchor.v - p->previous_velocity) * (1 / dt);
      p->previous_velocity = anchor.v;
      if (soft) {
        const V3 local = rotate(conj(pose.q[size_t(bone)]), acceleration);
        const V3 target{clamp(local.y * .014, -.3, .3), clamp(-local.x * .014, -.3, .3), 0};
        for (int axis = 0; axis < 3; ++axis) spring_step(p->swing[axis], p->swing_velocity[axis], target[axis], soft ? 3.5 : 8, 0.8, dt);
        const V3 anchor_point = p->pos + rotate(p->rotation, socket ? socket->point : V3{});
        p->rotation = p->rotation * qeuler(p->swing.x, p->swing.y, p->swing.z);
        p->pos = anchor_point - rotate(p->rotation, socket ? socket->point : V3{});
      }
      p->filtered_acceleration = vlerp(p->filtered_acceleration, acceleration, 1 - exp(-dt * 12));
      const f64 accel = std::min(120.0, norm(p->filtered_acceleration));
      const V3 alpha = (anchor.w - p->previous_angular) * (1 / dt);
      p->previous_angular = anchor.w;
      p->filtered_angular = vlerp(p->filtered_angular, alpha, 1 - exp(-dt * 12));
      const V3 angular = p->filtered_angular;
      const V3 lever = rotate(p->rotation, a.centre - socket->point);
      const f64 torque = norm(cross(lever, V3{0, 0, -9.81 * a.mass})) +
                         norm(V3{angular.x * a.inertia.x, angular.y * a.inertia.y, angular.z * a.inertia.z}) * (soft ? .1 : 1.0);
      p->load = a.mass * (9.81 + .15 * accel) + .25 * std::min(100.0, torque) / .05 + p->impulse_load;
      p->impulse_load *= exp(-dt * 12);
    }
    p->strength = held ? (alive() && behaviours.conscious ? capabilities().arms[p->point == AttachPoint::LeftHand ? 0 : 1].grip * capabilities().vigor : 0.0)
                       : p->state.strap;
    if (held && p->style == WieldStyle::TwoHands) p->strength += capabilities().arms[p->point == AttachPoint::LeftHand ? 1 : 0].grip * capabilities().vigor;
    if (held && behaviours.physical) {
      const size_t region = p->point == AttachPoint::LeftHand ? size_t(Region::ArmL) : size_t(Region::ArmR);
      p->strength *= clamp(behaviours.region_tone[region] / .45, 0.0, 1.0);
    }
    if (held && p->style == WieldStyle::TwoHands && std::min(capabilities().arms[0].grip, capabilities().arms[1].grip) < .25) {
      if (!a.has("one_handed")) {
        detach(p->point, ReleaseReason::GripFailed);
        continue;
      }
      p->style = WieldStyle::OneHand;
      const size_t primary = p->point == AttachPoint::LeftHand ? 0 : 1;
      if (capabilities().arms[primary].grip < .25 && capabilities().arms[1 - primary].grip >= .25) {
        const auto from = p->point, to = primary == 0 ? AttachPoint::RightHand : AttachPoint::LeftHand;
        motion.props.events.push_back({p->id, from, false, ReleaseReason::HandDamaged});
        motion.props.slots[size_t(from)].reset();
        p->point = to;
        motion.props.slots[size_t(to)] = p;
        motion.props.events.push_back({p->id, to, true, ReleaseReason::HandDamaged});
        motion.weapon_hand = attachment_bone(to);
        ++motion.props.revision;
        continue;
      }
    }
    if (behaviours.lost[size_t(part)]) {
      detach(p->point, ReleaseReason::AnchorLost);
      continue;
    }
    if (!held && p->state.strap <= .05) {
      detach(p->point, ReleaseReason::StrapCut);
      continue;
    }
    if (held && behaviours.mode == BodyMode::Falling && motion.control.arms[p->point == AttachPoint::LeftHand ? 0 : 1]) {
      detach(p->point, ReleaseReason::BreakingFall);
      continue;
    }
    if (held && !alive()) {
      detach(p->point, ReleaseReason::Death);
      continue;
    }
    if (held && !behaviours.conscious) {
      detach(p->point, ReleaseReason::KnockedOut);
      continue;
    }
    if (p->load > (socket ? socket->retention : 350) * p->strength) {
      detach(p->point, ReleaseReason::GripFailed);
      continue;
    }
  }
}
}  // namespace svx::anim
