// structvox game — pedestrians (pedestrians.hpp; game.hpp: PedestrianConfig; docs/ANIM.md).
//
// Every tick, before the world's: each walker minds (every tenth of a second) and moves - its
// root and inputs for its character's frame (CharacterSystem: the plans and drives before the
// mechanics, the bodies after them). After it: the blows the bodies took (a car's), deaths, and
// every half second the population about the viewer: the living out of range go, the dead
// beyond a few too; where there are fewer than there should be, people come on a sidewalk out of
// sight whose ground is resident. Everything follows from the world's state and the tick (the
// walkers' own draws are seeded by their ids): a replay has the same people doing the same.
#include "pedestrians.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "svx/base/diag.hpp"
#include "svx/base/dmath.hpp"
#include "svx/game/replay.hpp"

namespace svx {

namespace {

inline u64 mix(u64 x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

f64 flat_dist(const V3& a, const V3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }
V3 flat(const V3& v) { return V3{v.x, v.y, 0.0}; }
f64 hypot2(f64 x, f64 y) { return std::sqrt(x * x + y * y); }

constexpr u32 kPedestrian = 1;       // (the characters' kind)
constexpr i32 kLooks = 12;           // the looks people are made from
constexpr i32 kMaxCorpses = 10;      // the dead kept about (beyond: the longest dead go)
constexpr f64 kBodyRadius = 0.24;    // the root's box: half its width (m) ...
constexpr f64 kStepUp = 0.4;         // ... what it steps up (a kerb, a step)

// ---- steering (the original's steer.ts): a body's weight in how it moves

f64 smooth(f64 e0, f64 e1, f64 x) {
  const f64 t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

f64 wrap(f64 a) { return dm::atan2(dm::sin(a), dm::cos(a)); }

struct SteerOptions {
  f64 accel = 0.0, decel = 0.0;  // speeding up, slowing down (m/s^2)
};

// (walkers speed up gently, runners harder; everyone brakes a little harder)
SteerOptions steer_options(f64 want) {
  const f64 t = smooth(1.5, 4.0, want);
  return SteerOptions{1.9 + 1.6 * t, 3.6 + 1.2 * t};
}

// (a walker turns within half a metre, a runner in metres)
f64 turn_rate_at(f64 speed) { return std::max(1.2, std::min(3.2, 4.4 / std::max(speed, 0.8))); }

// The point `ahead` metres along the path from pos, and the path's length left.
V3 pursue(const V3& pos, const std::vector<V3>& path, f64 ahead, f64* remaining) {
  V3 from = pos;
  f64 travelled = 0.0, left = 0.0;
  bool found = false;
  V3 point = path.empty() ? pos : path.back();
  for (const V3& p : path) {
    const f64 seg = hypot2(p.x - from.x, p.y - from.y);
    if (!found && travelled + seg >= ahead && seg > 1e-6) {
      const f64 t = (ahead - travelled) / seg;
      point = from + (p - from) * t;
      found = true;
    }
    travelled += seg;
    left += seg;
    from = p;
  }
  *remaining = left;
  return point;
}

// The new horizontal velocity: towards want_dir (unit) at want_speed, from vel, the heading turning
// at most turn_rate_at(speed) and the speed changing within the options' rates.
V3 steer(const V3& vel, const V3& want_dir, f64 want_speed, f64 dt, const SteerOptions& o) {
  const f64 sp = hypot2(vel.x, vel.y);
  const f64 want = dm::atan2(want_dir.y, want_dir.x);
  f64 heading = want, off = 0.0;
  if (sp > 0.15) {
    const f64 cur = dm::atan2(vel.y, vel.x);
    off = wrap(want - cur);
    const f64 max = turn_rate_at(sp) * dt;
    heading = cur + std::clamp(off, -max, max);
  }
  // (a sharp turn is taken slower)
  const f64 target = want_speed * (1.0 - 0.6 * smooth(0.6, 2.0, std::abs(off)));
  const f64 next = sp + std::clamp(target - sp, -o.decel * dt, o.accel * dt);
  const f64 s = std::max(0.0, next);
  return V3{dm::cos(heading) * s, dm::sin(heading) * s, 0.0};
}

// A facing turning towards `want` with angular momentum: accelerating at most `accel` up to
// `max_rate`, braking in time to stop on it.
void turn(f64& yaw, f64& rate, f64 want, f64 dt, f64 max_rate, f64 accel) {
  const f64 d = wrap(want - yaw);
  const f64 brake = std::sqrt(2.0 * accel * std::abs(d)) * 0.92;
  const f64 wanted = (d < 0.0 ? -1.0 : d > 0.0 ? 1.0 : 0.0) * std::min(max_rate, brake);
  f64 r = rate + std::clamp(wanted - rate, -accel * dt, accel * dt);
  f64 step = r * dt;
  // (never past the target)
  if ((d >= 0.0 && step > d) || (d < 0.0 && step < d)) {
    step = d;
    r = 0.0;
  }
  yaw = wrap(yaw + step);
  rate = r;
}

}  // namespace

Pedestrians::Pedestrians(Game& g) : g_(&g), mesher_(std::make_unique<anim::ModelMesher>()) {}

Pedestrians::~Pedestrians() = default;

anim::CharacterSystem& Pedestrians::chars() { return *g_->chars_; }

const RoadNetwork* Pedestrians::roads() const { return g_->source_ ? g_->source_->roads() : nullptr; }

V3 Pedestrians::focus() const {
  const Body* car = g_->player_car();
  return car ? car->x : g_->viewer_;
}

f64 Pedestrians::rnd(Walker& w, f64 a, f64 b) {
  w.rng += 0x9E3779B97F4A7C15ull;
  u64 z = w.rng;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return a + (b - a) * (static_cast<f64>(z >> 11) * (1.0 / 9007199254740992.0));
}

void Pedestrians::clear() {
  walkers_.clear();
  noises_.clear();
  heard_.clear();
  mesh_of_.clear();
  palette_of_.clear();
  for (const auto& [k, e] : meshes_)
    if (e.sent) removed_out_.push_back(e.id);
  meshes_.clear();
  mesher_->clear();
  clock_ = 0.0;
}

// ---- the mind -------------------------------------------------------------------------------------

// Onto a walkway, heading for its end `toward`: the way along its walking line (off the corners'
// line towards the buildings in its middle). From the start (a corner), or from where it is.
void Pedestrians::on_walk(Walker& w, u64 walk, int toward, bool from_start) {
  Walk wk;
  const RoadNetwork* r = roads();
  if (!r || !r->walk(walk, &wk)) {
    w.path.clear();
    return;
  }
  w.walk = walk;
  w.toward = toward;
  const V3 from = toward == 1 ? wk.a : wk.b, to = toward == 1 ? wk.b : wk.a;
  const f64 len = flat_dist(from, to);
  const V3 d = len > 1e-9 ? flat(to - from) * (1.0 / len) : V3{1.0, 0.0, 0.0};
  w.path.clear();
  if (!wk.crossing && len > 5.0) {
    w.path.push_back(from + d * 2.0 + wk.inset);
    w.path.push_back(to - d * 2.0 + wk.inset);
  }
  w.path.push_back(to);
  if (!from_start)
    while (w.path.size() > 1 && dot(flat(w.path[0] - w.pos), d) < 0.0) w.path.erase(w.path.begin());
}

// At the corner at the end of its walkway: on along another - round the corner, or over a road
// (waiting at the kerb for the lights). Fleeing, the one leading furthest from the fright.
void Pedestrians::at_corner(Walker& w, bool fleeing) {
  const RoadNetwork* r = roads();
  if (!r) return;
  std::vector<std::pair<u64, int>> next;
  r->walk_next(w.walk, w.toward, next);
  struct Option {
    u64 id;
    int toward;
    bool crossing;
    f64 score;
  };
  std::vector<Option> opts;
  for (const auto& [id, end] : next) {
    Walk wk;
    if (!r->walk(id, &wk)) continue;
    const int toward = 1 - end;
    const V3 far = toward == 1 ? wk.b : wk.a;
    f64 score = wk.crossing ? 0.7 : 1.0;
    if (fleeing) {
      // (away from the fright; a road is crossed in a panic only when it leads away)
      const V3 away = flat(w.pos - w.threat);
      const V3 go = flat(far - w.pos);
      const f64 la = norm(away), lg = norm(go);
      score = (la > 1e-6 && lg > 1e-6 ? dot(away, go) / (la * lg) : 0.0) + 1.2 - (wk.crossing ? 0.3 : 0.0);
      score = std::max(0.05, score);
    }
    opts.push_back(Option{id, toward, wk.crossing, score});
  }
  if (opts.empty()) {
    // (a dead end: back the way it came)
    on_walk(w, w.walk, 1 - w.toward, true);
    return;
  }
  f64 total = 0.0;
  for (const Option& o : opts) total += o.score;
  f64 pick = rnd(w, 0.0, total);
  const Option* o = &opts.back();
  for (const Option& c : opts) {
    if (pick < c.score) {
      o = &c;
      break;
    }
    pick -= c.score;
  }
  on_walk(w, o->id, o->toward, true);
  if (o->crossing && !fleeing && w.mind != Mind::Flee) {
    w.mind = Mind::Cross;
    w.timer = rnd(w, 25.0, 45.0);  // (tired of waiting: it turns away)
  }
}

void Pedestrians::choose(Walker& w) {
  const f64 r = rnd(w, 0.0, 1.0);
  if (w.jogger && r < 0.35) {
    w.mind = Mind::Walk;
    w.speed = rnd(w, 2.6, 3.2);
    w.timer = rnd(w, 20.0, 50.0);
  } else if (r < 0.12) {
    w.mind = Mind::Wait;
    w.speed = 0.0;
    w.timer = rnd(w, 4.0, 12.0);
  } else {
    w.mind = Mind::Walk;
    w.speed = w.pace;
    w.timer = rnd(w, 25.0, 60.0);
  }
  if (w.path.empty()) on_walk(w, w.walk, w.toward, false);
}

// Running away from the fright: along the walkways, away from it (its end furthest from it).
void Pedestrians::flee(Walker& w) {
  w.mind = Mind::Flee;
  w.speed = w.fear > 0.8 ? rnd(w, 4.6, 5.6) : rnd(w, 3.2, 4.2);
  w.timer = rnd(w, 6.0, 12.0);
  Walk wk;
  const RoadNetwork* r = roads();
  if (r && r->walk(w.walk, &wk)) {
    const f64 da = flat_dist(wk.a, w.threat), db = flat_dist(wk.b, w.threat);
    on_walk(w, w.walk, db >= da ? 1 : 0, false);
  }
}

void Pedestrians::hear(Walker& w, const Noise& n) {
  const f64 d = flat_dist(w.pos, n.pos);
  if (d > n.radius) return;
  const f64 near = 1.0 - d / n.radius;
  static constexpr f64 kGain[6] = {0.5, 0.45, 1.4, 0.9, 0.35, 0.8};  // shot, impact, explosion, death, scream, crash
  const f64 gain = kGain[std::clamp(n.kind, 0, 5)] * (0.4 + near) / w.bravery;
  w.fear = std::min(2.0, w.fear + gain);
  w.threat = n.pos;
  if (n.kind == kExplosion && d < 9.0) {
    w.mind = Mind::Cower;
    w.timer = rnd(w, 2.5, 5.0);
    w.speed = 0.0;
  } else if ((w.mind == Mind::Walk || w.mind == Mind::Wait || w.mind == Mind::Cross) && w.fear > 0.45) {
    flee(w);
  }
}

// A car coming at it: it jumps aside (the way it is already off the car's line) - and is afraid.
void Pedestrians::watch_traffic(Walker& w, anim::Character& c) {
  if (!w.on_ground || c.controlled() || w.mind == Mind::Dodge) return;
  for (const auto& [vid, v] : g_->vehicles_) {
    const Body* b = v.chassis ? g_->world_.piece(v.chassis) : nullptr;
    if (!b) continue;
    const V3 rel = flat(w.pos - b->x);
    if (norm(rel) > 30.0) continue;
    const V3 rv = flat(b->v) - flat(w.vel);
    const f64 v2 = dot(rv, rv);
    if (v2 < 6.0) continue;
    const f64 t = dot(rel, rv) / v2;
    if (t < 0.0 || t > 1.6) continue;
    const V3 miss = rel - rv * t;
    const f64 md = norm(miss);
    if (md > 2.4) continue;
    w.fear = std::min(2.0, w.fear + 0.6 / w.bravery);
    w.threat = b->x;
    w.touched = true;
    const f64 lv = std::sqrt(v2);
    w.dodge = md > 0.05 ? miss * (1.0 / md) : V3{-rv.y / lv, rv.x / lv, 0.0};
    w.mind = Mind::Dodge;
    w.timer = 0.9;
    anim::Perception p;
    p.point = b->x;
    p.strength = 1.0;
    p.kind = anim::PerceptionKind::Impact;
    c.perceive(p);
    return;
  }
}

void Pedestrians::think(Walker& w, anim::Character& c, f64 dt) {
  w.fear = std::max(0.0, w.fear - dt * 0.05 * w.bravery);
  w.timer -= dt;
  for (const Noise& n : heard_) hear(w, n);
  watch_traffic(w, c);
  const RoadNetwork* r = roads();
  switch (w.mind) {
    case Mind::Walk:
      if (w.fear > 0.45) return flee(w);
      if (w.timer <= 0.0 && chance(w, 0.3)) {
        w.mind = Mind::Wait;
        w.timer = rnd(w, 4.0, 12.0);
      } else if (w.timer <= 0.0) {
        w.timer = rnd(w, 25.0, 60.0);
      }
      break;
    case Mind::Wait:
      if (w.fear > 0.45) return flee(w);
      if (w.timer <= 0.0) choose(w);
      break;
    case Mind::Cross:
      if (w.fear > 0.45) return flee(w);
      if (r && r->walk_open(w.walk, g_->world_.time())) {
        w.mind = Mind::Walk;
        w.speed = std::max(w.pace * 1.2, w.speed);
      } else if (w.timer <= 0.0) {
        // (tired of waiting: back along the sidewalk it came by)
        at_corner(w, false);
      }
      break;
    case Mind::Flee:
      if (w.fear > 0.9 && time_ - w.screamed > 4.0) {
        w.screamed = time_;
        noise(w.pos, 14.0, kScream);
      }
      if (w.timer <= 0.0 && w.fear < 0.35) choose(w);
      break;
    case Mind::Cower:
      if (w.timer <= 0.0) flee(w);
      break;
    case Mind::Dodge:
      if (w.timer <= 0.0) {
        if (w.fear > 0.45) flee(w);
        else choose(w);
      }
      break;
  }
}

// ---- the body ------------------------------------------------------------------------------------

// The original actor world's move: where the walker wants to go, kept apart from the others, the
// body's own move while it leads, a box swept against the world (a step up kerbs), the facing.
void Pedestrians::move(Walker& w, anim::Character& c, f64 dt) {
  const anim::MotionPlan& an = c.motion;
  const bool led = c.controlled();
  const bool locked = led || an.transitioning() || an.stance != anim::Stance::Stand;
  const f64 hs = hypot2(w.vel.x, w.vel.y);
  V3 want_dir;
  bool has_dir = false;
  f64 want_speed = 0.0;
  if (!locked) {
    if (w.mind == Mind::Dodge) {
      want_dir = w.dodge;
      want_speed = 3.8;
      has_dir = true;
    } else if ((w.mind == Mind::Walk || w.mind == Mind::Flee) && !w.path.empty()) {
      while (w.path.size() > 1 && flat_dist(w.path[0], w.pos) < 0.45) w.path.erase(w.path.begin());
      f64 remaining = 0.0;
      const V3 point = pursue(w.pos, w.path, 0.6 + 0.3 * hs, &remaining);
      const f64 d = hypot2(point.x - w.pos.x, point.y - w.pos.y);
      if (remaining < 0.5) {
        at_corner(w, w.mind == Mind::Flee);
      } else if (d > 1e-3) {
        const anim::Injuries& inj = c.behaviours.injuries;
        const f64 limp = std::max(inj.legL, inj.legR);
        want_speed = w.speed * (1.0 - 0.5 * limp) * (1.0 - 0.3 * inj.pain);
        want_dir = V3{(point.x - w.pos.x) / d, (point.y - w.pos.y) / d, 0.0};
        has_dir = true;
        // (setting off away from where the body faces: it turns first, then walks)
        if (hs < 0.6 && std::abs(wrap(dm::atan2(want_dir.y, want_dir.x) - w.yaw)) > 1.1) want_speed = std::min(want_speed, 0.3);
      }
    }
  }
  // keep apart: a little space between shoulders, and a step aside for someone coming
  V3 sep;
  if (!locked) {
    for (const auto& [oid, o] : walkers_) {
      if (oid == w.id || !o.alive) continue;
      const f64 ox = w.pos.x - o.pos.x, oy = w.pos.y - o.pos.y;
      const f64 d2 = ox * ox + oy * oy;
      if (d2 > 4.0 || d2 < 1e-6 || std::abs(w.pos.z - o.pos.z) > 1.0) continue;
      const f64 d = std::sqrt(d2);
      if (d < 0.8) {
        const f64 push = (0.8 - d) * (d < 0.55 ? 9.0 : 4.0);
        sep.x += (ox / d) * push;
        sep.y += (oy / d) * push;
      }
      const f64 rvx = w.vel.x - o.vel.x, rvy = w.vel.y - o.vel.y;
      const f64 rv2 = rvx * rvx + rvy * rvy;
      if (rv2 > 0.25) {
        const f64 t = std::clamp(-(ox * rvx + oy * rvy) / rv2, 0.0, 0.6);
        const f64 px = ox + rvx * t, py = oy + rvy * t;
        const f64 pd = hypot2(px, py);
        if (t > 0.0 && pd < 0.75) {
          const f64 k = (0.75 - pd) * 5.0 * (1.0 - t / 0.6);
          const f64 rl = std::sqrt(rv2);
          sep.x += (pd > 1e-3 ? px / pd : -rvy / rl) * k;
          sep.y += (pd > 1e-3 ? py / pd : rvx / rl) * k;
        }
      }
    }
  }
  // the body's move this frame when it leads; else the walker's, with the body's weight
  const V3 rm = c.take_root_motion();
  if (led) {
    w.vel.x = dt > 0.0 ? rm.x / dt : 0.0;
    w.vel.y = dt > 0.0 ? rm.y / dt : 0.0;
  } else if (w.on_ground) {
    const V3 v = steer(w.vel, has_dir ? want_dir : V3{dm::cos(w.yaw), dm::sin(w.yaw), 0.0}, has_dir ? want_speed : 0.0, dt, steer_options(has_dir ? want_speed : w.pace));
    const f64 k = 1.0 - std::exp(-6.0 * dt);
    w.vel.x = v.x + sep.x * k;
    w.vel.y = v.y + sep.y * k;
  }
  w.vel.z = std::max(-40.0, w.vel.z - 20.0 * dt);
  const anim::Stance st = an.stance;
  const f64 height = st == anim::Stance::Prone || st == anim::Stance::Ground || st == anim::Stance::Down ? 0.5 : st == anim::Stance::Kneel || st == anim::Stance::Sit ? 1.2 : 1.72;
  V3 lo{w.pos.x - kBodyRadius, w.pos.y - kBodyRadius, w.pos.z}, hi{w.pos.x + kBodyRadius, w.pos.y + kBodyRadius, w.pos.z + height};
  const V3 want = led ? V3{rm.x, rm.y, w.vel.z * dt} : w.vel * dt;
  // (feet pushed into - a piece come to rest on them - lifted out first)
  const f64 lift = g_->world_.depenetrate(lo, hi, 1.0);
  if (lift > 0.0) {
    w.pos.z += lift;
    lo.z += lift;
    hi.z += lift;
  }
  // the sweep, and a step up when a grounded move is blocked (stepmove.ts)
  CollideResult res = g_->world_.collide(lo, hi, want);
  const f64 wanted = hypot2(want.x, want.y), got = hypot2(res.move.x, res.move.y);
  bool stepped = false;
  if (wanted > 1e-4 && got < wanted - 1e-4 && (w.on_ground || (want.z < 0.0 && res.move.z > want.z + 1e-4))) {
    const CollideResult up = g_->world_.collide(lo, hi, V3{0.0, 0.0, kStepUp});
    if (up.move.z > 1e-4) {
      const CollideResult across = g_->world_.collide(lo + up.move, hi + up.move, V3{want.x, want.y, 0.0});
      if (hypot2(across.move.x, across.move.y) > got + 1e-4) {
        const V3 raised = up.move + across.move;
        const CollideResult down = g_->world_.collide(lo + raised, hi + raised, V3{0.0, 0.0, -up.move.z + std::min(0.0, want.z)});
        res = down;
        res.move = raised + down.move;
        stepped = true;
      }
    }
  }
  w.pos = w.pos + res.move;
  if (!stepped) {
    if (std::abs(res.move.x - want.x) > 1e-5) w.vel.x = 0.0;
    if (std::abs(res.move.y - want.y) > 1e-5) w.vel.y = 0.0;
  }
  w.on_ground = res.on_ground;
  if (w.on_ground && w.vel.z < 0.0) w.vel.z = 0.0;
  // stuck (a wreck across the sidewalk, rubble): back the way it came
  const f64 gotv = hypot2(res.move.x, res.move.y) / std::max(dt, 1e-4);
  if (has_dir && want_speed > 0.3 && gotv < want_speed * 0.25 && !locked) w.stuck += dt;
  else w.stuck = std::max(0.0, w.stuck - dt);
  if (w.stuck > 2.5) {
    w.stuck = 0.0;
    on_walk(w, w.walk, 1 - w.toward, false);
  }
  // the facing: with angular momentum; waiting at the kerb, across the road
  if (led) {
    w.yaw = an.root_yaw;
    w.yaw_rate = 0.0;
  } else {
    std::optional<f64> want_yaw;
    if (w.mind == Mind::Cross && !w.path.empty()) want_yaw = dm::atan2(w.path.back().y - w.pos.y, w.path.back().x - w.pos.x);
    else if (has_dir && !locked) want_yaw = dm::atan2(want_dir.y, want_dir.x);
    else if (hypot2(w.vel.x, w.vel.y) > 0.3 && !locked) want_yaw = dm::atan2(w.vel.y, w.vel.x);
    if (want_yaw) turn(w.yaw, w.yaw_rate, *want_yaw, dt, 3.2, 9.0);
    else w.yaw_rate *= std::exp(-12.0 * dt);
  }
}

// The blow its body took this tick (the core's contacts with other bodies: a car, debris, someone
// barging in): hurt by its violence - what it changed of the body's speed - knocked down by the
// physics itself, and afraid.
void Pedestrians::blows(Walker& w, anim::Character& c) {
  if (!c.bound()) return;
  f64 bump = 0.0;
  for (const anim::RigidBody* p : c.body.parts) bump += p->bumped;
  const f64 dv = bump / std::max(1.0, c.body.total_mass);
  if (dv < 2.2) return;
  w.touched = true;
  const f64 damage = (dv - 2.0) * 14.0;
  c.health -= damage;
  c.flash = 1.0;
  w.fear = 2.0;
  w.threat = c.bounds_center();
  // (the nearest vehicle: what hit it)
  f64 best = 8.0;
  for (const auto& [vid, v] : g_->vehicles_) {
    const Body* b = v.chassis ? g_->world_.piece(v.chassis) : nullptr;
    if (!b) continue;
    const f64 d = flat_dist(b->x, w.pos);
    if (d < best) {
      best = d;
      w.threat = b->x;
    }
  }
  if (c.health <= 0.0 && c.alive()) {
    c.health = 0.0;
    c.die();
    noise(w.pos, 18.0, kDeath);
  } else if (w.mind != Mind::Flee) {
    flee(w);
  }
}

// A blast (strength 1: a rocket's): the bodies near it thrown, hurt, killed (Character::blast);
// everyone about hears it.
void Pedestrians::blast(const V3& pos, f64 radius, f64 energy) {
  if (!std::isfinite(energy) || !(energy > 0.0) || !(radius > 0.0)) return;
  const f64 strength = std::min(4.0, energy / 1.0e6);
  noise(pos, 30.0 + 40.0 * std::min(2.0, strength), kExplosion);
  if (!g_->chars_) return;
  anim::CharacterSystem& cs = chars();
  for (auto& [id, w] : walkers_) {
    anim::Character* c = cs.get(id);
    if (!c) continue;
    const f64 d = flat_dist(c->bounds_center(), pos);
    if (d > radius * 6.0 + 2.0) {
      if (c->alive() && d < 14.0) {
        anim::Perception p;
        p.point = pos;
        p.strength = std::min(1.4, strength);
        p.kind = anim::PerceptionKind::Blast;
        c->perceive(p);
      }
      continue;
    }
    const bool was = c->alive();
    const anim::BlastResult r = c->blast(pos, radius, strength);
    w.touched = true;
    if (was && r.killed) {
      w.alive = false;
      noise(c->bounds_center(), 18.0, kDeath);
    }
  }
}

// A round into a body (from `from`): its voxels carved, the body hit (a flinch, a stagger, a fall;
// Character::wound), hurt or killed; the others hear it.
bool Pedestrians::wound(u32 id, const V3& from, const V3& pos, f64 radius, f64 energy) {
  if (!g_->chars_) return false;
  anim::Character* c = chars().get(id);
  if (!c || !std::isfinite(energy) || !(energy > 0.0)) return false;
  V3 dir = pos - from;
  const f64 l = norm(dir);
  dir = l > 1e-9 ? dir * (1.0 / l) : V3{1.0, 0.0, 0.0};
  const std::optional<anim::CharacterHit> hit = c->raycast(pos - dir * 0.6, dir, 1.2);
  if (!hit) return false;
  const bool was = c->alive();
  // (a pistol round's 500 J: 35; a pellet's 150 J: about 10)
  const anim::WoundResult r = c->wound(*hit, dir, 0.07 * energy, std::clamp(radius, 0.02, 0.12), std::min(4.0, 1.0 + energy / 400.0));
  noise(pos, 12.0, kImpact);
  const auto it = walkers_.find(id);
  if (it == walkers_.end()) return true;
  Walker& w = it->second;
  w.touched = true;
  w.fear = 2.0;
  w.threat = from;
  if (was && r.killed) {
    w.alive = false;
    noise(pos, 18.0, kDeath);
  } else if (was && w.mind != Mind::Flee) {
    flee(w);
  }
  return true;
}

// ---- the tick ------------------------------------------------------------------------------------

void Pedestrians::before_tick() {
  const f64 dt = g_->world_.config().dt;
  time_ += dt;
  heard_.swap(noises_);
  noises_.clear();
  if (!g_->chars_) return;
  anim::CharacterSystem& cs = chars();
  cs.focus.assign(1, focus());
  for (auto& [id, w] : walkers_) {
    anim::Character* c = cs.get(id);
    if (!c || !w.alive) continue;
    if (!c->alive()) {
      w.alive = false;
      continue;
    }
    w.think += dt;
    if (w.think >= 0.1) {
      think(w, *c, w.think);
      w.think = 0.0;
    } else if (!heard_.empty()) {
      for (const Noise& n : heard_) hear(w, n);
    }
    move(w, *c, dt);
    anim::MotionInput& in = c->motion.input;
    in.mood = w.mind == Mind::Cower ? anim::Mood::Cower : w.mind == Mind::Flee && w.fear > 0.8 ? anim::Mood::Panic : anim::Mood::Normal;
    in.crouch = 0.0;
    in.look_at = w.fear > 0.3 && flat_dist(w.threat, w.pos) < 25.0 ? std::optional<V3>(w.threat + V3{0.0, 0.0, 1.0}) : std::nullopt;
    c->set_root(w.pos, w.yaw);
    if (w.pos.z < -60.0) c->die();
  }
}

void Pedestrians::after_tick() {
  const f64 dt = g_->world_.config().dt;
  if (g_->chars_) {
    anim::CharacterSystem& cs = chars();
    for (auto& [id, w] : walkers_) {
      anim::Character* c = cs.get(id);
      if (!c) continue;
      if (w.alive && c->alive()) blows(w, *c);
      if (!c->alive()) {
        w.alive = false;
        w.dead_for += dt;
      }
    }
  }
  clock_ += dt;
  if (clock_ >= 0.5) {
    clock_ = 0.0;
    populate();
    // (SVX_TRACE_PEDESTRIANS: the first few walkers, every half second)
    static const bool trace = diag("SVX_TRACE_PEDESTRIANS");
    if (trace) {
      int k = 0;
      for (const auto& [id, w] : walkers_) {
        if (k++ >= 4) break;
        Walk wk;
        const bool on = roads() && roads()->walk(w.walk, &wk);
        const anim::Character* c = g_->chars_ ? g_->chars_->get(id) : nullptr;
        std::printf("  [ped %u] t %.1f mind %d fear %.2f pos %.2f %.2f %.2f v %.2f yaw %.2f walk %s %d path %zu left, body %s%s\n", id, time_, static_cast<int>(w.mind), w.fear, w.pos.x, w.pos.y,
                    w.pos.z, hypot2(w.vel.x, w.vel.y), w.yaw, on ? (wk.crossing ? "crossing" : "sidewalk") : "none", w.toward, w.path.size(),
                    c && c->bound() ? "deep" : c && c->behaviours.physical ? "shallow" : "plan", c && c->controlled() ? " (leads)" : "");
      }
    }
  }
  output();
}

void Pedestrians::populate() {
  const RoadNetwork* r = roads();
  const PedestrianConfig& cfg = g_->peds_;
  const V3 at = focus();
  const f64 load = g_->stream_.load_radius;
  const f64 radius = std::max(10.0, std::min(cfg.radius, load - 15.0));
  const f64 near = std::min(cfg.near_radius, radius - 10.0);
  const f64 far = std::min(radius + 15.0, load - 5.0);
  // out of range: the living go (people come and go); the dead are the world's - a body at rest
  // goes with its region and comes back with it (CharacterSystem) - only the longest dead beyond
  // the few kept in range go for good
  std::vector<u32> gone, forget;
  i32 living = 0;
  for (const auto& [id, w] : walkers_) {
    const anim::Character* c = g_->chars_ ? chars().get(id) : nullptr;
    if (!c) {
      forget.push_back(id);
      continue;
    }
    const f64 d = flat_dist(w.pos, at);
    if (!w.alive) {
      if (d > far) forget.push_back(id);
      continue;
    }
    if (d > far && !(w.touched && d < far + 20.0)) gone.push_back(id);
    else ++living;
  }
  if (g_->chars_) {
    std::vector<std::pair<f64, u32>> dead;
    for (anim::CharacterId id : chars().ids()) {
      const anim::Character* c = chars().get(id);
      if (c && !c->alive() && chars().kind_of(id) == kPedestrian && flat_dist(c->bounds_center(), at) < far) dead.push_back({-c->dead_time, id});
    }
    if (static_cast<i32>(dead.size()) > kMaxCorpses) {
      std::sort(dead.begin(), dead.end());
      for (size_t k = size_t(kMaxCorpses); k < dead.size(); ++k) gone.push_back(dead[k].second);
    }
  }
  for (u32 id : forget) walkers_.erase(id);
  for (u32 id : gone) {
    walkers_.erase(id);
    if (g_->chars_) chars().despawn(id);
  }
  if (!cfg.enabled || !r || cfg.count <= living) return;
  // on a sidewalk out of sight, its ground resident, room about it
  std::vector<Walk> walks;
  r->walks_in(at - V3{radius, radius, 0.0}, at + V3{radius, radius, 0.0}, walks);
  struct Spot {
    V3 p;
    const Walk* w;
  };
  std::vector<Spot> spots;
  for (const Walk& wk : walks) {
    if (wk.crossing) continue;
    for (f64 u : {0.2, 0.5, 0.8}) {
      const V3 p = wk.a + (wk.b - wk.a) * u + wk.inset;
      const f64 d = flat_dist(p, at);
      if (d < near || d > radius) continue;
      spots.push_back(Spot{p, &wk});
    }
  }
  if (spots.empty()) return;
  // (people come: the characters' system, the looks)
  make_looks();
  if (!g_->chars_) {
    g_->chars_ = std::make_shared<anim::CharacterSystem>();
    g_->world_.add_system(g_->chars_);
  }
  anim::CharacterSystem& cs = chars();
  cs.config.policy = cfg.bodies == 0 ? anim::BodyPolicy::Deep : cfg.bodies == 1 ? anim::BodyPolicy::Shallow : anim::BodyPolicy::Hybrid;
  cs.config.max_deep = cfg.max_deep;
  // (the dead the world gives back: their looks by what they recorded)
  if (!cs.restore)
    cs.restore = [this](u32 kind, const std::vector<u8>& data, anim::CharacterDesc* out) {
      if (kind != kPedestrian || data.empty()) return false;
      make_looks();
      const anim::HumanVariant& look = looks_[size_t(data[0]) % looks_.size()];
      out->model = look.model;
      out->palette = look.palette;
      out->health = 0.0;
      return true;
    };
  auto resident = [&](const V3& p) {
    const f64 h = g_->world_.voxel_size();
    const IVec3 c = chunk_of(IVec3{static_cast<i32>(std::floor(p.x / h + 0.5)), static_cast<i32>(std::floor(p.y / h + 0.5)), static_cast<i32>(std::floor(p.z / h + 0.5)) - 1});
    return g_->world_.chunk_resident(c) && g_->world_.chunk_resident({c[0], c[1], c[2] + 1});
  };
  const u64 salt = mix(static_cast<u64>(g_->world_.ticks()) * 0xD1B54A32D192ED03ull ^ 0x9ED5);
  // (a few at a time; more while far short - a crowd asked for, a fast drive through)
  const i32 per_round = std::min(8, 2 + (cfg.count - living) / 6);
  for (i32 tries = 0, made = 0; tries < 2 * per_round && made < per_round && living < cfg.count; ++tries) {
    const u64 h = mix(salt + static_cast<u64>(tries));
    const Spot& s = spots[size_t(h % spots.size())];
    bool free = true;
    for (const auto& [id, o] : walkers_)
      if (flat_dist(o.pos, s.p) < 4.0) free = false;
    if (!free || !resident(s.p)) continue;
    const size_t li = size_t(mix(h ^ 0x100C) % looks_.size());
    const anim::HumanVariant& look = looks_[li];
    const int toward = (h >> 20) & 1;
    const V3 dir = flat(toward == 1 ? s.w->b - s.w->a : s.w->a - s.w->b);
    anim::CharacterDesc d;
    d.model = look.model;
    d.palette = look.palette;
    d.health = 100.0;
    d.seed = static_cast<f64>((h >> 24) & 0xFFFF) + 1.0;
    d.pos = s.p;
    d.yaw = dm::atan2(dir.y, dir.x);
    d.kind = kPedestrian;
    d.data = {static_cast<u8>(li)};
    const u32 id = cs.spawn(d);
    if (!id) continue;
    Walker w;
    w.id = id;
    w.rng = mix(h ^ (static_cast<u64>(id) << 32));
    w.pos = s.p;
    w.yaw = d.yaw;
    w.bravery = rnd(w, 0.7, 1.3);
    w.pace = rnd(w, 1.05, 1.6);
    w.jogger = chance(w, 0.12);
    w.walk = s.w->id;
    w.toward = toward;
    on_walk(w, s.w->id, toward, false);
    choose(w);
    walkers_[id] = std::move(w);
    ++living;
    ++made;
  }
}

void Pedestrians::make_looks() {
  if (looks_.empty())
    for (i32 k = 0; k < kLooks; ++k) looks_.push_back(anim::make_civilian(k + 1));
}

// ---- the front end's meshes and palettes ---------------------------------------------------------

void Pedestrians::output() {
  const f64 dt = g_->world_.config().dt;
  for (auto& [k, e] : meshes_) e.unused += dt;
  mesh_of_.clear();
  palette_of_.clear();
  if (g_->chars_) {
    anim::CharacterSystem& cs = chars();
    for (anim::CharacterId id : cs.ids()) {
      const anim::Character* c = cs.get(id);
      if (!c || !c->model) continue;
      const auto key = std::make_pair(c->model.get(), c->geometry_version);
      auto it = meshes_.find(key);
      if (it == meshes_.end()) {
        MeshEntry e;
        e.model = c->model;
        e.id = next_mesh_++;
        it = meshes_.emplace(key, std::move(e)).first;
      }
      it->second.unused = 0.0;
      mesh_of_[id] = it->second.id;
      // (a palette by its colours)
      u64 digest = 0x9A1E77E5ull;
      for (const auto& slot : c->palette)
        for (f32 v : slot) {
          u32 b;
          std::memcpy(&b, &v, sizeof b);
          digest = mix(digest ^ b);
        }
      auto pit = palettes_.find(digest);
      if (pit == palettes_.end()) {
        CharacterPalette p;
        p.id = next_palette_++;
        for (size_t s = 0; s < c->palette.size() && s < 16; ++s)
          for (int ch = 0; ch < 3; ++ch) p.rgb[s * 3 + size_t(ch)] = c->palette[s][size_t(ch)];
        palettes_out_.push_back(p);
        pit = palettes_.emplace(digest, p.id).first;
      }
      palette_of_[id] = pit->second;
    }
  }
  // (meshes nothing drew for a while go; the mesher's part cache with them - it knows parts by
  // their address, and would keep those of models gone)
  bool dropped = false;
  for (auto it = meshes_.begin(); it != meshes_.end();) {
    if (it->second.unused > 2.0) {
      if (it->second.sent) removed_out_.push_back(it->second.id);
      it = meshes_.erase(it);
      dropped = true;
    } else {
      ++it;
    }
  }
  if (dropped) mesher_->clear();
}

std::vector<CharacterView> Pedestrians::views() const {
  std::vector<CharacterView> out;
  if (!g_->chars_) return out;
  const anim::CharacterSystem& cs = *g_->chars_;
  for (anim::CharacterId id : cs.ids()) {
    const anim::Character* c = cs.get(id);
    const auto m = mesh_of_.find(id);
    const auto p = palette_of_.find(id);
    if (!c || m == mesh_of_.end() || p == palette_of_.end() || c->skin.size() < size_t(kCharacterBones) * 16) continue;
    CharacterView v;
    v.id = id;
    v.mesh = m->second;
    v.palette = p->second;
    v.flags = static_cast<u8>((c->alive() ? CharacterView::kAlive : 0) | (c->bound() ? CharacterView::kDeep : 0) | (c->behaviours.physical ? CharacterView::kPhysical : 0) |
                              (c->asleep() ? CharacterView::kAsleep : 0) | (c->down() ? CharacterView::kDown : 0));
    v.centre = c->bounds_center();
    v.radius = c->bounds_radius();
    v.flash = c->flash;
    v.health = c->max_health > 0.0 ? std::clamp(c->health / c->max_health, 0.0, 1.0) : 0.0;
    v.skin = c->skin.data();
    out.push_back(v);
  }
  return out;
}

// (meshed when the front end asks: a host that draws nothing pays nothing)
std::vector<CharacterMeshData> Pedestrians::take_meshes() {
  std::vector<CharacterMeshData> out;
  for (auto& [key, e] : meshes_) {
    if (e.sent) continue;
    e.sent = true;
    anim::CharacterMesh m = mesher_->mesh(*e.model);
    CharacterMeshData d;
    d.id = e.id;
    d.vertex_count = m.vertex_count;
    d.vertices = std::move(m.vertices);
    d.indices = std::move(m.indices);
    out.push_back(std::move(d));
  }
  return out;
}

std::vector<u32> Pedestrians::take_removed_meshes() {
  std::vector<u32> out;
  out.swap(removed_out_);
  return out;
}

std::vector<CharacterPalette> Pedestrians::take_palettes() {
  std::vector<CharacterPalette> out;
  out.swap(palettes_out_);
  return out;
}

i64 Pedestrians::memory_bytes() const {
  i64 b = static_cast<i64>(sizeof(*this));
  for (const auto& [id, w] : walkers_) b += static_cast<i64>(sizeof(w) + w.path.capacity() * sizeof(V3) + 48);
  for (const anim::HumanVariant& l : looks_)
    if (l.model)
      for (const anim::VoxelPart& part : l.model->parts) b += static_cast<i64>(part.cells.capacity() + sizeof(part));
  b += static_cast<i64>(meshes_.size() * 96 + palettes_.size() * 48 + (mesh_of_.size() + palette_of_.size()) * 48);
  return b;
}

// ---- Game ----------------------------------------------------------------------------------------

void Game::set_pedestrians(const PedestrianConfig& c) {
  if (log_)
    log_->push({world_.ticks(), Command::Type::Pedestrians,
                {c.enabled ? 1.0 : 0.0, static_cast<f64>(c.count), c.near_radius, c.radius, static_cast<f64>(c.bodies), static_cast<f64>(c.max_deep)}});
  peds_ = c;
  peds_.count = std::clamp(peds_.count, 0, 256);
  if (!std::isfinite(peds_.near_radius)) peds_.near_radius = 30.0;
  if (!std::isfinite(peds_.radius)) peds_.radius = 70.0;
  peds_.radius = std::clamp(peds_.radius, 20.0, 300.0);
  peds_.near_radius = std::clamp(peds_.near_radius, 0.0, peds_.radius - 10.0);
  peds_.bodies = std::clamp(peds_.bodies, 0, 2);
  peds_.max_deep = std::clamp(peds_.max_deep, 0, 256);
}

// (the people, and their world, told where this game is: it may have been moved)
Pedestrians* Game::people() const {
  if (!people_) return nullptr;
  Game& self = const_cast<Game&>(*this);
  people_->rebind(self);
  if (chars_) chars_->rebind(self.world_);
  return people_.get();
}

void Game::noise(const V3& pos, f64 radius, int kind) {
  if (Pedestrians* p = people()) p->noise(pos, radius, kind);
}

void Game::pedestrians_before_tick() {
  const RoadNetwork* roads = source_ ? source_->roads() : nullptr;
  if (!people_ && roads && peds_.enabled) people_ = std::make_unique<Pedestrians>(*this);
  if (Pedestrians* p = people()) p->before_tick();
}

void Game::pedestrians_after_tick() {
  if (Pedestrians* p = people()) p->after_tick();
}

Game::ShotHit Game::raycast_shot(const V3& origin, const V3& dir, f64 max_dist) const {
  ShotHit out;
  if (!world_.in_range(origin) || !(max_dist > 0.0)) return out;
  const RayHit h = world_.raycast(origin, dir, max_dist);
  if (h.hit) {
    out.hit = true;
    out.pos = h.pos;
    out.normal = h.normal;
    out.distance = h.distance;
    out.material = h.material;
  }
  if (chars_) {
    const std::optional<anim::CharacterSystem::Hit> c = chars_->raycast(origin, dir, h.hit ? h.distance : max_dist);
    if (c) {
      out.hit = true;
      out.pos = c->hit.point;
      out.normal = c->hit.normal;
      out.distance = c->hit.distance;
      out.material = -1;
      out.character = c->id;
      out.bone = c->hit.bone;
    }
  }
  return out;
}

bool Game::wound_character(u32 id, const V3& pos, f64 radius, f64 energy) {
  if (log_) log_->push({world_.ticks(), Command::Type::Wound, {static_cast<f64>(id), pos.x, pos.y, pos.z, radius, energy}});
  Pedestrians* p = people();
  return p && p->wound(id, viewer_, pos, radius, energy);
}

std::vector<CharacterView> Game::character_views() const {
  const Pedestrians* p = people();
  return p ? p->views() : std::vector<CharacterView>{};
}
std::vector<CharacterMeshData> Game::take_character_meshes() {
  Pedestrians* p = people();
  return p ? p->take_meshes() : std::vector<CharacterMeshData>{};
}
std::vector<u32> Game::take_removed_character_meshes() {
  Pedestrians* p = people();
  return p ? p->take_removed_meshes() : std::vector<u32>{};
}
std::vector<CharacterPalette> Game::take_character_palettes() {
  Pedestrians* p = people();
  return p ? p->take_palettes() : std::vector<CharacterPalette>{};
}

}  // namespace svx
