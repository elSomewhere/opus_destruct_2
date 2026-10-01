#include "svx/anim/brawl.hpp"

#include <array>

namespace svx::anim {

namespace {

// Each strike's force (~1 a punch) and where it aims (a bone of the opponent's).
struct StrikeSpec {
  std::string_view name;
  f64 force;
  i32 aim;
};
constexpr StrikeSpec kStrikeSpecs[] = {
    {"jab", 0.7, H::head},        {"cross", 1.2, H::head},           {"hook", 1.45, H::head},      {"uppercut", 1.55, H::head},
    {"frontKick", 1.9, H::spine}, {"roundhouse", 2.3, H::chest},     {"stab", 1.1, H::spine},      {"slash", 0.9, H::chest},
    {"gutStab", 1.2, H::spine},   {"forehandSlash", 0.95, H::chest}, {"riflePush", 1.3, H::chest},
};

// (null: not a strike)
const StrikeSpec* strike_spec(std::string_view name) {
  for (const StrikeSpec& s : kStrikeSpecs)
    if (s.name == name) return &s;
  return nullptr;
}

// An action's name without the mirror image's ".m".
std::string base_of(std::string_view name) {
  std::string s(name);
  const size_t i = s.find(".m");
  if (i != std::string::npos) s.erase(i, 2);
  return s;
}

bool blade(std::string_view s) { return s == "stab" || s == "slash" || s == "gutStab" || s == "forehandSlash"; }

}  // namespace

Brawler::Brawler(Character& c, const BrawlerOptions& o) : self(c), rng_(o.seed * 977.0 + 3.0), aggression_(o.aggression), skill_(o.skill) {}

// The opponent's body point a strike aims at.
V3 Brawler::aim_point(std::string_view strike) const {
  const Character& o = *opponent;
  const StrikeSpec* s = strike_spec(strike);
  const i32 b = s ? s->aim : H::chest;
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
  if (me.controlled() && !(bh.mode == BodyMode::Reacting && bh.balance_error < 0.0 && feet[0].planted && feet[1].planted)) return;
  const f64 want = o_down ? 1.7 : knife() ? 0.82 : last_strike_ == "frontKick" || last_strike_ == "roundhouse" ? 1.08 : 0.92;
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
  if (!theirs.empty() && theirs != reacted_ && strike_spec(base_of(theirs)) && !a.busy()) {
    reacted_ = theirs;
    const f64 r = rng_.next();
    if (r < skill_) {
      a.play("block");
    } else if (r < skill_ + 0.2) {
      // step back out of it
      move.x -= n.x * 1.6;
      move.y -= n.y * 1.6;
    }
  }
  if (theirs.empty()) reacted_.clear();
  // attack
  cooldown_ -= dt;
  if (!o_down && cooldown_ <= 0.0 && !a.busy() && d < want + 0.3) {
    static constexpr std::array<std::string_view, 3> kFar = {"frontKick", "roundhouse", "jab"};
    static constexpr std::array<std::string_view, 6> kNear = {"jab", "jab", "cross", "hook", "uppercut", "frontKick"};
    static constexpr std::array<std::string_view, 2> kFists = {"jab", "cross"};
    std::string_view strike;
    if (knife()) strike = rng_.pick(kKnifeAttacks);
    else if (last_strike_ == "jab" && rng_.chance(0.55)) strike = "cross";
    else if (d > 1.05) strike = rng_.pick(kFar);
    else strike = rng_.pick(kNear);
    // (not off one leg while still finding its feet)
    if (me.controlled() && (strike == "frontKick" || strike == "roundhouse")) strike = rng_.pick(kFists);
    if (me.weapon && me.weapon->kind != PropKind::Knife && me.weapon->kind != PropKind::Pistol) strike = "riflePush";
    a.play(strike, aim_point(strike));
    last_strike_ = strike;
    const f64 combo = strike == "jab" ? 0.15 : 0.0;
    cooldown_ = combo != 0.0 ? combo : (0.45 + rng_.next() * 1.1) * (1.4 - aggression_);
  }
  if (a.busy() && !a.action_name().empty()) {
    const std::string base = base_of(a.action_name());
    if (strike_spec(base)) a.aim_action(aim_point(base));
  }
}

std::vector<LandedBlow> Brawler::resolve(const std::vector<AnimEvent>& events) {
  std::vector<LandedBlow> out;
  Character* o = opponent;
  if (!o) return out;
  for (const AnimEvent& e : events) {
    if (e.name != "strike") continue;
    const std::string base = base_of(e.action);
    const HitKind kind = blade(base) ? HitKind::Blade : HitKind::Blunt;
    // where the limb is against the opponent's body
    const i32 bone = o->nearest_bone(e.pos);
    const V3 bp = o->pose.p[size_t(bone)];
    const V3 tail = o->pose.tail(bone);
    const V3 mid{(bp.x + tail.x) / 2.0, (bp.y + tail.y) / 2.0, (bp.z + tail.z) / 2.0};
    const f64 reach = e.limb == Limb::FootR || e.limb == Limb::FootL ? 0.38 : 0.3;
    if (vdist(e.pos, mid) > reach + (bone == H::chest || bone == H::spine ? 0.12 : 0.0)) continue;
    const V3 dir = vnorm((e.target ? *e.target : mid) - self.pose.p[H::chest]);
    const bool blocking =
        o->motion.action_name() == "block" && (bone == H::head || bone == H::neck || bone == H::chest || (bone >= H::upperarmL && bone <= H::handR));
    const StrikeSpec* s = strike_spec(base);
    const f64 force = (s ? s->force : 1.0) * (blocking ? 0.3 : 1.0);
    LandedBlow blow;
    blow.result = o->melee(e.pos, dir, blocking ? HitKind::Blunt : kind, force);
    blow.attacker = &self;
    blow.victim = o;
    blow.point = e.pos;
    blow.dir = dir;
    blow.kind = kind;
    blow.blocked = blocking;
    out.push_back(std::move(blow));
  }
  return out;
}

}  // namespace svx::anim
