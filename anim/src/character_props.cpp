#include "svx/anim/character.hpp"
namespace svx::anim {
bool Character::attach(const PropInstancePtr& p, AttachPoint point, std::string_view socket, WieldStyle style) {
  const auto previous = motion.props.held();
  if (!motion.props.attach(p, point, socket, style)) return false;
  if (motion.props.held() != previous) refresh_held_prop();
  place_prop(*p);
  const auto& anchor = *body.parts[size_t(HumanoidBody::body_of_bone(attachment_bone(point)))];
  p->previous_velocity = anchor.v;
  p->previous_angular = anchor.w;
  p->filtered_acceleration = p->filtered_angular = p->swing = p->swing_velocity = V3{};
  p->load = p->impulse_load = p->support_gap_time = 0;
  p->strength = attachment_strength(*p);
  update_load();
  return true;
}
PropInstancePtr Character::detach(AttachPoint point, ReleaseReason reason) {
  auto p = motion.props.at(point);
  if (!p) return {};
  const i32 bone = attachment_bone(point), part = HumanoidBody::body_of_bone(bone);
  const auto& anchor = *body.parts[size_t(part)];
  const V3 centre = p->pos + rotate(p->rotation, p->centre_of_mass());
  if (behaviours.physical) {
    p->angular = anchor.w;
    p->velocity = anchor.v + cross(anchor.w, centre - anchor.x);
  } else {
    const f64 idt = 1 / std::max(last_dt_, 1e-6);
    p->angular = qerror(pose.q[size_t(bone)], prev_pose.q[size_t(bone)]) * idt;
    p->velocity = (pose.p[size_t(bone)] - prev_pose.p[size_t(bone)]) * idt + cross(p->angular, centre - pose.p[size_t(bone)]);
  }
  const auto previous = motion.props.held();
  auto out = motion.props.detach(point, reason);
  if (motion.props.held() != previous) refresh_held_prop();
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
  if (!trial.accepts(*a, point, socket, style)) {
    motion.props.refusal = trial.refusal;
    return false;
  }
  detach(point);
  return attach(motion.props.registry->create(a), point, socket, style);
}
void Character::wrench(AttachPoint point, const V3& impulse, std::optional<V3> at) {
  if (!(norm2(impulse) > 0)) return;
  if (auto p = motion.props.at(point)) {
    p->strength = attachment_strength(*p);
    const f64 force = norm(impulse) / .08;
    p->impulse_load += force;
    p->load += force;
    pushed_at(HumanoidBody::body_of_bone(attachment_bone(point)), impulse, at.value_or(p->pos));
    const auto* socket = p->archetype->socket(p->socket);
    if (p->load > (socket ? socket->retention : 350.0) * p->strength) detach(point, ReleaseReason::Wrenched);
  }
}
f64 Character::attachment_strength(const PropInstance& p) const {
  if (!hand_point(p.point)) return p.state.strap;
  if (!alive() || !behaviours.conscious) return 0;
  auto strength = [&](size_t side) {
    const auto& arm = capabilities().arms[side];
    const size_t region = side == 0 ? size_t(Region::ArmL) : size_t(Region::ArmR);
    const f64 tone = behaviours.physical ? clamp(behaviours.region_tone[region] / .45, 0.0, 1.0) : 1;
    return std::min({arm.grip, arm.strength, arm.control}) * capabilities().vigor * tone;
  };
  const size_t primary = p.point == AttachPoint::LeftHand ? 0 : 1;
  f64 total = strength(primary);
  if (p.style == WieldStyle::TwoHands) {
    const auto* socket = p.archetype->socket(p.socket);
    const auto* secondary = p.archetype->socket("secondary");
    if (socket && secondary) total += strength(1 - primary) * secondary->retention / std::max(1e-6, socket->retention);
  }
  return total;
}
void Character::set_wield(WieldProfile profile) {
  const auto previous = motion.props.held();
  motion.props.wield = profile;
  if (motion.props.held() != previous) refresh_held_prop();
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
      const V3 offset = p->centre_of_mass() - (socket ? socket->point : V3{});
      const f64 mass = p->mass();
      masses[part] += mass;
      inertias[part] += a.inertia * (mass / a.mass) +
                        V3{offset.y * offset.y + offset.z * offset.z, offset.x * offset.x + offset.z * offset.z, offset.x * offset.x + offset.y * offset.y} * mass;
      total += mass;
      const V3 local = p->point == AttachPoint::Back       ? V3{0, -.22, 0}
                       : p->point == AttachPoint::Shoulder ? V3{-.25, -.08, 0}
                       : hand_point(p->point)              ? V3{p->point == AttachPoint::LeftHand ? -.4 : .4, .05, 0}
                                                           : V3{};
      moment += (local + rotate(a.hanging_rotation, offset)) * mass;
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
void Character::refresh_held_prop() {
  motion.refresh_prop();
  place_weapon();
  if (const auto p = motion.props.held()) {
    place_prop(*p);
    weapon_pos = p->pos;
    weapon_rot = p->rotation;
  }
}
void Character::place_prop(PropInstance& p) {
  const i32 bone = attachment_bone(p.point);
  const auto& a = *p.archetype;
  const auto* socket = a.socket(p.socket);
  const bool held = hand_point(p.point);
  if (held && &p == motion.props.held().get()) {
    p.pos = weapon_pos;
    p.rotation = weapon_rot;
  } else {
    V3 offset;
    if (p.point == AttachPoint::Back)
      offset = V3{0, -.19, .05} * motion.k;
    else if (p.point == AttachPoint::Shoulder)
      offset = V3{-.25, -.05, -.15} * motion.k;
    else if (p.point == AttachPoint::Hip)
      offset = V3{.19, -.04, 0} * motion.k;
    else if (p.point == AttachPoint::Thigh)
      offset = V3{.12, 0, -.12} * motion.k;
    else if (p.point == AttachPoint::Chest || p.point == AttachPoint::Arms)
      offset = V3{0, .2, 0} * motion.k;
    else if (held)
      offset = motion.arms.palm_offset(p.point == AttachPoint::LeftHand ? Side::L : Side::R);
    p.rotation = pose.q[size_t(bone)];
    if (held) p.rotation = p.rotation * conj(motion.arms.canonical(p.point == AttachPoint::LeftHand ? Side::L : Side::R)) * a.hanging_rotation;
    p.rotation = p.rotation * (socket ? conj(socket->rotation) : Quat{});
    p.pos = pose.p[size_t(bone)] + rotate(pose.q[size_t(bone)], offset) - rotate(p.rotation, socket ? socket->point : V3{});
  }
  if (p.style == WieldStyle::Hanging) {
    // A handle transmits force while letting the load hang under gravity.
    const V3 handle = socket ? socket->point : V3{};
    const V3 pivot = p.pos + rotate(p.rotation, handle);
    p.rotation = qz(motion.root_yaw - kPi / 2);
    p.pos = pivot - rotate(p.rotation, handle);
  }
}
void Character::update_props(f64 dt) {
  // A handoff must not update an item twice when its new slot comes later.
  const auto items = motion.props.slots;
  for (const auto& p : items) {
    if (!p || p->location != PropLocation::Attached) continue;
    place_prop(*p);
    const bool held = hand_point(p->point);
    if (behaviours.lost[size_t(HumanoidBody::body_of_bone(attachment_bone(p->point)))]) {
      detach(p->point, ReleaseReason::AnchorLost);
      continue;
    }
    if (!held && p->state.strap <= .05) {
      detach(p->point, ReleaseReason::StrapCut);
      continue;
    }
    if (held && (!alive() || !behaviours.conscious)) {
      detach(p->point, alive() ? ReleaseReason::KnockedOut : ReleaseReason::Death);
      continue;
    }
    const auto& a = *p->archetype;
    auto capacity = [&](size_t side) {
      const auto& arm = capabilities().arms[side];
      return std::min({arm.grip, arm.strength, arm.control});
    };
    if (held && p->style == WieldStyle::TwoHands && std::min(capacity(0), capacity(1)) < .25) {
      if (!a.has("one_handed") || std::max(capacity(0), capacity(1)) < .25) {
        detach(p->point, ReleaseReason::GripFailed);
        continue;
      }
      const auto from = p->point;
      const size_t primary = from == AttachPoint::LeftHand ? 0 : 1;
      const auto to = capacity(primary) >= .25 ? from : primary == 0 ? AttachPoint::RightHand : AttachPoint::LeftHand;
      // The supporting hand keeps its own socket when the primary arm fails.
      const std::string socket = to == from ? p->socket : "secondary";
      if (!motion.props.regrip(from, to, socket, WieldStyle::OneHand, ReleaseReason::HandDamaged)) {
        detach(from, ReleaseReason::GripFailed);
        continue;
      }
      refresh_held_prop();
      const auto& anchor = *body.parts[size_t(HumanoidBody::body_of_bone(attachment_bone(to)))];
      p->previous_velocity = anchor.v;
      p->previous_angular = anchor.w;
      p->filtered_acceleration = p->filtered_angular = V3{};
    }
    if (held && p->style == WieldStyle::TwoHands && behaviours.physical) {
      const size_t side = p->point == AttachPoint::RightHand ? 0 : 1;
      const size_t bone = side == 0 ? H::handL : H::handR;
      const V3 palm = pose.p[bone] + rotate(pose.q[bone], motion.arms.palm_offset(side == 0 ? Side::L : Side::R));
      const auto* support = a.socket("secondary");
      const bool pulling = body.hands[side]->enabled && body.hands[side]->reference;
      const f64 gap = support ? norm(palm - p->pos - rotate(p->rotation, support->point)) : 0;
      p->support_gap_time = pulling && gap > .18 * motion.k ? p->support_gap_time + dt : 0;
      // A grip can catch up briefly after a shove; it cannot remain stretched
      // across open space. Keep the primary hand when the archetype allows it.
      if (p->support_gap_time > .2) {
        if (a.has("one_handed") && motion.props.regrip(p->point,p->point,p->socket,WieldStyle::OneHand,ReleaseReason::Wrenched)) {
          p->support_gap_time = 0;refresh_held_prop();
        } else { detach(p->point,ReleaseReason::Wrenched);continue; }
      }
    }
    const i32 bone = attachment_bone(p->point), part = HumanoidBody::body_of_bone(bone);
    const auto& anchor = *body.parts[size_t(part)];
    const auto* socket = a.socket(p->socket);
    const bool soft = p->style == WieldStyle::Hanging || p->point == AttachPoint::Back || p->point == AttachPoint::Shoulder;
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
      const f64 mass = p->mass();
      const V3 lever = rotate(p->rotation, p->centre_of_mass() - (socket ? socket->point : V3{}));
      const f64 torque = norm(cross(lever, V3{0, 0, -9.81 * mass})) +
                         norm(V3{angular.x * a.inertia.x, angular.y * a.inertia.y, angular.z * a.inertia.z}) * (mass / a.mass) * (soft ? .1 : 1.0);
      p->load = mass * (9.81 + .15 * accel) + .25 * std::min(100.0, torque) / .05 + p->impulse_load;
      p->impulse_load *= exp(-dt * 12);
    }
    p->strength = attachment_strength(*p);
    if (held && behaviours.mode == BodyMode::Falling && motion.control.arms[p->point == AttachPoint::LeftHand ? 0 : 1]) {
      detach(p->point, ReleaseReason::BreakingFall);
      continue;
    }
    if (p->load > (socket ? socket->retention : 350) * p->strength) {
      detach(p->point, ReleaseReason::GripFailed);
      continue;
    }
  }
  if (load_revision_ != motion.props.revision) update_load();
  if (const auto p = motion.props.held()) {
    weapon_pos = p->pos;
    weapon_rot = p->rotation;
  }
}
}  // namespace svx::anim
