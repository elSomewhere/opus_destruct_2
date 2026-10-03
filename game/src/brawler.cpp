#include "svx/game/brawler.hpp"

#include <array>

namespace svx {

using namespace svx::anim;

namespace {

// Where each strike aims (a bone of the opponent's).
struct StrikeAim {
  std::string_view name;
  i32 aim;
};
constexpr StrikeAim kStrikeAims[] = {
    {"jab", H::head},        {"cross", H::head},          {"hook", H::head},           {"uppercut", H::head},
    {"frontKick", H::spine}, {"roundhouse", H::chest},    {"stab", H::spine},          {"slash", H::chest},
    {"gutStab", H::spine},   {"forehandSlash", H::chest}, {"riflePush", H::chest},
};

i32 aim_of(std::string_view name) {
  for (const StrikeAim& s : kStrikeAims)
    if (s.name == name) return s.aim;
  return H::chest;
}

// An action's name without the mirror image's ".m".
std::string base_of(std::string_view name) {
  std::string s(name);
  const size_t i = s.find(".m");
  if (i != std::string::npos) s.erase(i, 2);
  return s;
}

bool striking(std::string_view name) {
  const auto* a = action_def(name);
  return a && a->targeted;
}

}  // namespace

Brawler::Brawler(Character& c, const BrawlerOptions& o) : self(c), rng_(o.seed * 977.0 + 3.0), aggression_(o.aggression), skill_(o.skill), resolver_(c) {}

// The opponent's body point a strike aims at.
V3 Brawler::aim_point(std::string_view strike) const {
  const Character& o = *opponent;
  const i32 b = aim_of(strike);
  const WorldPose& w = o.pose;
  if (b == H::head) {
    const V3 h = o.model->skeleton->rest_head[H::head];
    return w.point_of(H::head, V3{h.x, h.y + 0.06, h.z + 0.07});
  }
  const V3 p = w.p[size_t(b)];
  const Quat& q = w.q[size_t(b)];
  // the front of the body
  const V3 f{2.0 * (q.x * q.y - q.z * q.w), 1.0 - 2.0 * (q.x * q.x + q.z * q.z), 2.0 * (q.y * q.z + q.x * q.w)};
  return V3{p.x + f.x * 0.1, p.y + f.y * 0.1, p.z + f.z * 0.1 + 0.06};
}

void Brawler::update(f64 dt) {
  dt_ = dt;
  Character& me = self;
  MotionPlan& a = me.motion;
  Character* o = opponent;
  move.x = 0.0;
  move.y = 0.0;
  if (!me.alive() || !o) {
    a.input.guard = false;
    return;
  }
  const f64 d = vdist(a.root_pos, o->motion.root_pos);
  const V3 to = o->motion.root_pos - a.root_pos;
  yaw = atan2(to.y, to.x);
  const bool o_down = !o->alive() || o->down();
  a.input.guard = !o_down || d < 2.5;
  a.input.look_at = o->eyes();
  // (knocked about, it fights on once its feet are under it again)
  const Behaviours& bh = me.behaviours;
  const std::array<Foot, 2>& feet = a.feet_planner.feet;
  if (me.controlled() && !(bh.mode == BodyMode::Reacting && bh.balance_error < .08 && (feet[0].planted || feet[1].planted) &&
                           std::max(me.capabilities().legs[0].support, me.capabilities().legs[1].support) > .65))
    return;
  f64 reach = .85;
  if (auto p = a.props.held()) {
    reach = .55;
    for (const auto& f : p->archetype->features) reach = std::max(reach, .55 + norm(f.b - p->archetype->grip));
  }
  const f64 want = o_down ? 1.7 : reach;
  // close or open the distance, circle to the side
  circle_t_ -= dt;
  if (circle_t_ <= 0.0) {
    circle_t_ = 1.5 + rng_.next() * 2.5;
    circle_ = rng_.chance(0.5) ? 1.0 : -1.0;
  }
  const V3 n = vnorm(V3{to.x, to.y, 0.0});
  const f64 radial = clamp((d - want) * 2.8, -1.4, 1.6);
  const f64 lateral = (o_down ? 0.0 : 0.45) * circle_;
  move.x = n.x * radial - n.y * lateral;
  move.y = n.y * radial + n.x * lateral;
  // react to what the opponent throws
  const std::string_view theirs = o->motion.action_name();
  if (!theirs.empty() && theirs != reacted_ && striking(theirs) && !a.busy()) {
    reacted_ = theirs;
    const f64 r = rng_.next();
    if (r < skill_) {
      a.play(a.props.held() ? "propBlock" : "block");
    } else if (r < skill_ + 0.2) {
      // step back out of it
      move.x -= n.x * 1.6;
      move.y -= n.y * 1.6;
    }
  }
  if (theirs.empty()) reacted_.clear();
  // attack: what the body can throw now (MotionPlan::can_play), as this fighter would
  cooldown_ -= dt;
  if (!o_down && cooldown_ <= 0.0 && !a.busy() && d < want + 0.3) {
    std::vector<const ActionDef*> candidates;
    const auto held = a.props.held();
    const auto& cap = me.capabilities();
    const bool left = cap.arms[0].strength == cap.arms[1].strength ? a.props.wield.left_handed : cap.arms[0].strength > cap.arms[1].strength;
    for (const auto& action : actions()) {
      if (!action.targeted || (held && action.left_handed != left)) continue;
      if (held ? action.requires_tags.empty() : !action.requires_tags.empty()) continue;  // (with a prop, its strikes; without, the body's)
      if (!a.can_play(action)) continue;
      // (a careful fighter: no kick close in, off balance or on a weak leg; the stronger arm)
      const bool kick = action.drives(Channel::StrikeFootR) || action.drives(Channel::StrikeFootL);
      if (kick && (d < 1.05 || me.controlled() || std::min(cap.legs[0].support, cap.legs[1].support) < .6)) continue;
      const size_t arm = action.strike_arm();
      if (cap.arms[1 - arm].strength - cap.arms[arm].strength > .2) continue;
      candidates.push_back(&action);
    }
    if (!candidates.empty()) {
      const auto& action = *candidates[std::min(candidates.size() - 1, size_t(rng_.next() * candidates.size()))];
      a.play(action.name, aim_point(base_of(action.name)));
      cooldown_ = (.45 + rng_.next() * 1.1) * (1.4 - aggression_) / std::max(.25, cap.vigor);
    }
  }
  const f64 speed = hypot2(move.x, move.y);
  if (speed > me.capabilities().max_speed) move = move * (me.capabilities().max_speed / speed);

  if (a.busy() && !a.action_name().empty()) {
    const std::string base = base_of(a.action_name());
    if (striking(a.action_name())) a.aim_action(aim_point(base));
  }
}

std::vector<LandedBlow> Brawler::resolve() {
  resolver_.opponent = opponent;
  return resolver_.resolve(dt_);
}

}  // namespace svx
