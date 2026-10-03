#include "svx/anim/characters/attachments.hpp"
#include "svx/anim/rig.hpp"
#include <limits>
namespace svx::anim {
const char* attachment_name(AttachPoint p) {
  constexpr const char* names[] = {"rightHand", "leftHand", "back", "shoulder", "hip", "thigh", "chest", "head", "arms"};
  return size_t(p) < size_t(AttachPoint::Count) ? names[size_t(p)] : "invalid";
}
const char* release_name(ReleaseReason r) {
  constexpr const char* names[] = {
      "voluntary", "wrenched", "grip failed", "knocked out", "death", "breaking a fall", "anchor part lost", "strap cut", "hand or forearm damaged"};
  return size_t(r) < 9 ? names[size_t(r)] : "invalid";
}
bool hand_point(AttachPoint p) { return p == AttachPoint::LeftHand || p == AttachPoint::RightHand; }
i32 attachment_bone(AttachPoint p) {
  constexpr i32 bones[] = {H::handR, H::handL, H::chest, H::chest, H::pelvis, H::thighR, H::chest, H::head, H::chest};
  return size_t(p) < size_t(AttachPoint::Count) ? bones[size_t(p)] : H::chest;
}
V3 PropInstance::centre_of_mass() const {
  if (!damaged_model || geometry_version == 0) return archetype->centre;
  // Damaged and merged geometry uses the same voxel centre as loose debris.
  V3 sum;
  size_t count = 0;
  const auto& m = *damaged_model;
  for (const auto& p : m.parts)
    for (i32 z = 0; z < p.dims[2]; ++z)
      for (i32 y = 0; y < p.dims[1]; ++y)
        for (i32 x = 0; x < p.dims[0]; ++x)
          if (p.cells[size_t(p.index(x, y, z))]) {
            sum += V3{x + p.origin[0] + .5, y + p.origin[1] + .5, z + p.origin[2] + .5} * m.voxel_size;
            ++count;
          }
  return count ? sum * (1.0 / count) : archetype->centre;
}
PropRegistry::PropRegistry(const CollisionWorld* c) : loose_(c) {
  // Persistent items are never subject to the bounded blood/debris eviction policy.
  loose_.max_gibs = std::numeric_limits<i32>::max();
  loose_.kill_z = -std::numeric_limits<f64>::infinity();
}
PropInstancePtr PropRegistry::create(PropPtr a) {
  if (!a || !a->model) return {};
  auto p = std::make_shared<PropInstance>();
  p->id = next_++;
  p->archetype = std::move(a);
  items_[p->id] = p;
  return p;
}
PropInstancePtr PropRegistry::get(u64 id) const {
  auto it = items_.find(id);
  return it == items_.end() ? PropInstancePtr{} : it->second;
}
std::vector<PropInstancePtr> PropRegistry::nearby(const V3& at, f64 radius) const {
  std::vector<PropInstancePtr> out;
  for (const auto& [id, p] : items_)
    if (p->location == PropLocation::Loose && vdist(at, p->pos) <= radius) out.push_back(p);
  return out;
}
PropInstancePtr PropRegistry::restore(const PropInstancePtr& saved) {
  if (!saved || !saved->id) return {};
  if (auto previous = get(saved->id)) reclaim(previous);
  items_[saved->id] = saved;
  next_ = std::max(next_, saved->id + 1);
  return saved;
}
void PropRegistry::release(const PropInstancePtr& p) {
  p->location = PropLocation::Loose;
  p->character = 0;
  // Physical allocation happens in update, in instance order, after parallel character work.
}
void PropRegistry::reclaim(const PropInstancePtr& p) {
  if (p->loose_body) {
    loose_.remove(p->loose_body);
    p->loose_body = nullptr;
  }
}
void PropRegistry::retire(const PropInstancePtr& p) {
  reclaim(p);
  p->location = PropLocation::Gone;
  items_.erase(p->id);
}
void PropRegistry::update(f64 dt) {
  for (auto& [id, p] : items_)
    if (p->location == PropLocation::Loose && !p->loose_body && !p->model().parts.empty()) {
      p->loose_body = loose_.spawn(p->model().parts[0], p->model().voxel_size, p->pos, p->rotation, {}, p->velocity, p->angular, p->id);
      if (p->loose_body) {
        const V3 centre = p->centre_of_mass();
        // spawn uses the voxel pivot; set_mass can replace it with an authored
        // centre while preserving every point's position and velocity.
        p->loose_body->vel -= cross(p->angular, rotate(p->rotation, centre - p->loose_body->pivot));
        loose_.set_mass(p->loose_body, p->archetype->mass + p->retained_mass, p->archetype->inertia, &centre);
      }
    }
  loose_.update(dt);
  for (auto& [id, p] : items_)
    if (p->location == PropLocation::Loose && p->loose_body) {
      const auto& g = *p->loose_body;
      p->rotation = g.rot;
      p->pos = g.pos - rotate(g.rot, g.pivot);
      p->velocity = g.vel;
      p->angular = g.ang;
    }
}
Attachments::Attachments() : registry(std::make_shared<PropRegistry>()) {}
PropInstancePtr Attachments::at(AttachPoint p) const { return size_t(p) < slots.size() ? slots[size_t(p)] : PropInstancePtr{}; }
PropInstancePtr Attachments::held() const {
  const auto a = at(wield.left_handed ? AttachPoint::LeftHand : AttachPoint::RightHand);
  if (a) return a;
  return at(wield.left_handed ? AttachPoint::RightHand : AttachPoint::LeftHand);
}
bool Attachments::occupied(AttachPoint p) const {
  if (at(p)) return true;
  if (hand_point(p))
    for (const auto& x : slots)
      if (x && (x->style == WieldStyle::TwoHands || x->point == AttachPoint::Arms)) return true;
  return false;
}
bool Attachments::free_hand(bool left) const { return !occupied(left ? AttachPoint::LeftHand : AttachPoint::RightHand); }
bool Attachments::accepts(const Prop& archetype, AttachPoint point, std::string_view socket, WieldStyle style) {
  refusal.clear();
  if (!archetype.model || size_t(point) >= slots.size() || size_t(style) > size_t(WieldStyle::Stowed)) {
    refusal = "invalid archetype, attachment or wield style";
    return false;
  }
  if (occupied(point)) {
    refusal = "attachment is occupied";
    return false;
  }
  if (std::find(archetype.attachments.begin(), archetype.attachments.end(), attachment_name(point)) == archetype.attachments.end()) {
    refusal = "archetype does not allow this attachment";
    return false;
  }
  if (!archetype.socket(socket)) {
    refusal = "unknown socket";
    return false;
  }
  if (style == WieldStyle::TwoHands &&
      (!hand_point(point) || !archetype.socket("secondary") || !archetype.has("two_handed") || !free_hand(point == AttachPoint::RightHand))) {
    refusal = "two free hands and a secondary grip are required";
    return false;
  }
  if (hand_point(point) && style != WieldStyle::TwoHands && !archetype.has("one_handed")) {
    refusal = "archetype requires two hands";
    return false;
  }
  if (hand_point(point) && (style == WieldStyle::Worn || style == WieldStyle::Stowed)) {
    refusal = "worn or stowed props need a body anchor";
    return false;
  }
  if (!hand_point(point) && (style != WieldStyle::Worn && style != WieldStyle::Stowed)) {
    refusal = "body anchors require a worn or stowed attachment";
    return false;
  }
  if (style == WieldStyle::Reverse && !archetype.socket("reverse")) {
    refusal = "no reverse grip";
    return false;
  }
  return true;
}
bool Attachments::attach(const PropInstancePtr& p, AttachPoint point, std::string_view socket, WieldStyle style) {
  refusal.clear();
  if (!p || !p->archetype || !registry || !p->id || registry->get(p->id) != p) {
    refusal = "instance does not belong to this registry";
    return false;
  }
  if (p->location != PropLocation::Loose) {
    refusal = "instance is unavailable";
    return false;
  }
  if (!accepts(*p->archetype, point, socket, style)) return false;
  registry->reclaim(p);
  p->point = point;
  p->socket = style == WieldStyle::Reverse ? "reverse" : std::string(socket);
  p->style = style;
  p->location = PropLocation::Attached;
  p->character = owner;
  slots[size_t(point)] = p;
  ++revision;
  events.push_back({p->id, point, true, ReleaseReason::Voluntary});
  return true;
}
bool Attachments::regrip(AttachPoint from, AttachPoint to, std::string_view socket, WieldStyle style, ReleaseReason reason) {
  refusal.clear();
  const auto p = at(from);
  if (!p || !registry || registry->get(p->id) != p || p->location != PropLocation::Attached || p->character != owner) {
    refusal = "attachment has no owned instance";
    return false;
  }
  auto trial = *this;
  trial.slots[size_t(from)].reset();
  if (!trial.accepts(*p->archetype, to, socket, style)) {
    refusal = trial.refusal;
    return false;
  }
  slots[size_t(from)].reset();
  p->point = to;
  p->socket = style == WieldStyle::Reverse ? "reverse" : std::string(socket);
  p->style = style;
  slots[size_t(to)] = p;
  ++revision;
  events.push_back({p->id, from, false, reason});
  events.push_back({p->id, to, true, reason});
  return true;
}
PropInstancePtr Attachments::detach(AttachPoint point, ReleaseReason reason) {
  auto p = at(point);
  if (!p) return {};
  slots[size_t(point)].reset();
  ++revision;
  p->last_release = reason;
  events.push_back({p->id, point, false, reason});
  registry->release(p);
  return p;
}
HeldPropView& HeldPropView::operator=(const PropPtr& p) {
  reset();
  if (p) {
    auto x = a_->registry->create(p);
    a_->attach(x, AttachPoint::RightHand, "primary", p->has("two_handed") ? WieldStyle::TwoHands : WieldStyle::OneHand);
  }
  return *this;
}
void HeldPropView::reset() {
  if (auto p = a_->held()) a_->detach(p->point, ReleaseReason::Voluntary);
}
}  // namespace svx::anim
