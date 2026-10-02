// svx_anim melee (the port of the original's test/melee.test.ts, and of the brawls and melee of its
// test/motion.test.ts): knives (the guard, every attack reaching a body in front, a thug's knife
// fight), a soldier's pauses, thugs, brawls with fists and with a knife, melee blows on a character.
// What has bodies runs on both paths; the plan's own on the plan alone.
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <set>

#include "doctest.h"
#include "scene.hpp"
#include "svx/anim/brawl.hpp"
#include "svx/anim/characters/props.hpp"

using namespace scene;

namespace {

const Path kPaths[] = {Path::Shallow, Path::Deep};

const FlatGround& flat_ground() {
  static const FlatGround g(0.0);
  return g;
}

// A motion plan at the origin facing +y (yaw pi/2: model space = world space).
std::unique_ptr<MotionPlan> standing(f64 yaw = kPi / 2.0, f64 seed = 5.0) {
  auto a = std::make_unique<MotionPlan>(humanoid_skeleton(), &flat_ground(), seed);
  a->place(V3{0, 0, 0}, yaw);
  return a;
}

// Runs the plan with the host moving the root (as games do).
void live(MotionPlan& a, f64 seconds, f64 speed = 0.0, const std::function<void(MotionPlan&, f64)>& each = nullptr) {
  V3 pos = a.root_pos;
  const V3 f{std::cos(a.root_yaw), std::sin(a.root_yaw), 0.0};
  const i32 n = static_cast<i32>(std::floor(seconds * 60.0 + 0.5));
  for (i32 i = 0; i < n; ++i) {
    pos.x += f.x * speed * DT;
    pos.y += f.y * speed * DT;
    a.set_root(pos, a.root_yaw);
    a.update(DT);
    if (each) each(a, (i + 1) * DT);
    for (const V3& p : a.world.p) REQUIRE((std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)));
  }
}

// An action's name without the mirror image's ".m".
std::string base_of(std::string_view name) {
  std::string s(name);
  const size_t i = s.find(".m");
  if (i != std::string::npos) s.erase(i, 2);
  return s;
}

template <size_t N>
bool one_of(const std::string& s, const std::array<std::string_view, N>& list) {
  return std::find(list.begin(), list.end(), s) != list.end();
}

struct Fighter {
  Character* c;
  Brawler br;
  V3 pos;  // where the host has it
};

// A fight, frame by frame: each fighter's choices and the host moving it (with the body while that
// leads, else by its footwork), the characters' frame, `each`, then the blows their strikes land.
// (The original takes the fighters in turn - one's choices, frame and blows, then the other's; a
// world's tick steps both bodies at once, so on both paths both go together.)
std::vector<LandedBlow> fight(Scene& s, std::vector<Fighter>& f, i32 frames, const std::function<void()>& each = nullptr) {
  std::vector<Character*> cs;
  for (Fighter& x : f) cs.push_back(x.c);
  std::vector<LandedBlow> blows;
  for (i32 i = 0; i < frames; ++i) {
    s.frame(cs, [&] {
      for (Fighter& x : f) {
        x.br.update(DT);
        if (!x.c->alive()) continue;
        // (the body leads when it is knocked about: the fighter follows it)
        const V3 rm = x.c->take_root_motion();
        if (x.c->controlled()) {
          x.pos.x += rm.x;
          x.pos.y += rm.y;
        } else if (!x.c->motion.transitioning()) {
          x.pos.x += x.br.move.x * DT;
          x.pos.y += x.br.move.y * DT;
        }
        x.c->set_root(x.pos, x.br.yaw);
      }
    });
    if (each) each();
    for (Fighter& x : f)
      for (LandedBlow& b : x.br.resolve(x.c->take_events())) blows.push_back(std::move(b));
  }
  return blows;
}

}  // namespace

// ---- knives --------------------------------------------------------------------------------------

TEST_CASE("melee: a knife fighter holds a knife guard, blade forward") {
  auto a = standing();
  a->weapon = make_knife();
  a->input.guard = true;
  live(*a, 1.0);
  CHECK(a->pose_action_name() == "knifeGuard");
  const V3 tip = a->prop_point(a->weapon->muzzle);
  const V3 chest = a->world.p[H::chest];
  INFO("the blade is " << tip.y - chest.y << " m in front of the chest");
  CHECK(tip.y - chest.y > 0.3);
}

TEST_CASE("melee: every knife attack reaches a body in front of it") {
  for (const std::string_view name : kKnifeAttacks) {
    const std::string nm(name);
    INFO(nm);
    auto a = standing();
    a->weapon = make_knife();
    a->input.guard = true;
    live(*a, 0.8);
    a->take_events();
    const V3 target{0, 0.85, name == "gutStab" ? 1.0 : 1.2};
    CHECK(a->play(name, target));
    std::optional<AnimEvent> hit;
    live(*a, 1.0, 0.0, [&](MotionPlan& b, f64) {
      for (const AnimEvent& e : b.take_events())
        if (e.name == "strike") hit = e;
    });
    REQUIRE(hit);
    CHECK(hit->limb == Limb::Blade);
    INFO("the blade " << vdist(hit->pos, target) << " m from the target");
    MESSAGE(nm << " tip " << hit->pos.x << "," << hit->pos.y << "," << hit->pos.z << " error " << vdist(hit->pos, target));
    CHECK(vdist(hit->pos, target) < 0.3);
  }
}

TEST_CASE("melee: a thug with a knife cuts a civilian in a fight, with a variety of attacks") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    Scene s(path);
    const V3 at0{0, 0, s.ground}, at1{1, 0, s.ground};
    Character& thug = s.add(make_thug(3), 3.0, 0.0, at0, make_knife());
    Character& civ = s.add(make_civilian(8), 8.0, kPi, at1);
    // (a style sets the stance of the feet: the original sets it before placing, so placed again)
    thug.motion.style = random_style(3, StyleKind::Thug);
    thug.place(at0, 0.0);
    std::vector<Fighter> f;
    f.push_back(Fighter{&thug, Brawler(thug, BrawlerOptions{.aggression = 0.85, .skill = 0.2, .seed = 3.0}), at0});
    f.push_back(Fighter{&civ, Brawler(civ, BrawlerOptions{.aggression = 0.3, .skill = 0.3, .seed = 8.0}), at1});
    f[0].br.opponent = &civ;
    f[1].br.opponent = &thug;
    std::set<std::string> attacks;
    const std::vector<LandedBlow> blows = fight(s, f, 10 * 60, [&] {
      if (!thug.motion.action_name().empty()) attacks.insert(base_of(thug.motion.action_name()));
    });
    i32 cuts = 0, used = 0;
    for (const LandedBlow& b : blows) cuts += b.attacker == &thug && b.kind == HitKind::Blade;
    std::string names;
    for (const std::string& n : attacks) {
      if (!one_of(n, kKnifeAttacks)) continue;
      names += (used++ > 0 ? ", " : "") + n;
    }
    INFO(cuts << " cuts; civilian health " << civ.health << "; knife attacks used: " << names);
    MESSAGE(pn << ": " << cuts << " cuts; civilian health " << civ.health << "; knife attacks used: " << names);
    CHECK(cuts >= 2);
    CHECK((civ.health < civ.max_health * 0.8 || !civ.alive()));
    CHECK(used >= 2);
  }
}

// ---- a soldier's pauses --------------------------------------------------------------------------

TEST_CASE("melee: an armed body standing easy fidgets (helmet, brow, shoulders, weapon, a look round); aiming, it does not") {
  for (const Carry carry : {Carry::Ready, Carry::Aim}) {
    const std::string cn = carry == Carry::Ready ? "ready" : "aim";
    INFO(cn);
    auto a = standing();
    a->weapon = make_rifle();
    a->input.carry = carry;
    a->input.aim_at = carry == Carry::Aim ? std::optional<V3>(V3{0, 20, 1.4}) : std::nullopt;
    std::set<std::string> seen;
    live(*a, 40.0, 0.0, [&](MotionPlan& b, f64) {
      if (!b.action_name().empty()) seen.insert(base_of(b.action_name()));
    });
    i32 n = 0;
    std::string fidgets;
    for (const std::string& s : seen) {
      if (!one_of(s, kArmedFidgets)) continue;
      fidgets += (n++ > 0 ? ", " : "") + s;
    }
    INFO("fidgets: " << fidgets);
    if (carry == Carry::Ready) CHECK(n >= 2);
    else CHECK(n == 0);
  }
}

TEST_CASE("melee: a soldier catches a breath with the weapon lowered, and reloads") {
  auto a = standing();
  a->weapon = make_rifle();
  a->input.carry = Carry::Ready;
  live(*a, 1.0);
  CHECK(a->play("catchBreath"));
  live(*a, 3.0);
  CHECK(!a->busy());
  a->take_events();
  a->play("reloadRifle");
  std::vector<std::string> names;
  live(*a, 3.0, 0.0, [&](MotionPlan& b, f64) {
    for (const AnimEvent& e : b.take_events()) names.push_back(e.name);
  });
  CHECK(std::find(names.begin(), names.end(), "reloaded") != names.end());
}

// ---- thugs ---------------------------------------------------------------------------------------

TEST_CASE("melee: thugs: their own looks and a swagger") {
  for (i32 i = 1; i <= 4; ++i) {
    const HumanVariant t = make_thug(i);
    INFO("thug " << i << ": " << t.model->voxel_count() << " voxels");
    CHECK(t.model->voxel_count() > 3000);
    CHECK(t.spec.bottom == BottomStyle::Trousers);
  }
  f64 thug = 0.0, civ = 0.0;
  for (i32 s = 1; s <= 20; ++s) {
    thug += random_style(s, StyleKind::Thug).sway + random_style(s, StyleKind::Thug).arms;
    civ += random_style(s, StyleKind::Civilian).sway + random_style(s, StyleKind::Civilian).arms;
  }
  INFO("the swing in the walk: thugs " << thug << ", civilians " << civ);
  CHECK(thug > civ);
}

// ---- brawls and blows on a character (the original's motion tests) -------------------------------

namespace {

struct Brawl {
  std::vector<LandedBlow> blows;
  Character* a;
  Character* b;
};

// Two civilians a metre apart, the first with a knife or not, fight for 8 s.
Brawl brawl(Scene& s, bool knife) {
  const V3 at0{0, 0, s.ground}, at1{1, 0, s.ground};
  Character& a = s.add(make_civilian(70), 70.0, 0.0, at0, knife ? make_knife() : nullptr);
  Character& b = s.add(make_civilian(71), 71.0, kPi, at1);
  std::vector<Fighter> f;
  f.push_back(Fighter{&a, Brawler(a, BrawlerOptions{.aggression = 0.55, .skill = 0.3, .seed = 70.0}), at0});
  f.push_back(Fighter{&b, Brawler(b, BrawlerOptions{.aggression = 0.55, .skill = 0.3, .seed = 71.0}), at1});
  f[0].br.opponent = &b;
  f[1].br.opponent = &a;
  return Brawl{fight(s, f, 8 * 60), &a, &b};
}

// A civilian standing a moment, facing +y.
Character& civilian(Scene& s) {
  Character& c = s.add(make_civilian(5), 1.0, kPi / 2.0, V3{0, 0, s.ground});
  for (i32 i = 0; i < 20; ++i) s.frame({&c});
  return c;
}

}  // namespace

TEST_CASE("melee: fists land blows that hurt") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    Scene s(path);
    const Brawl r = brawl(s, false);
    i32 landed = 0;
    bool blunt = true;
    for (const LandedBlow& x : r.blows) {
      landed += x.result.damage > 0.0;
      blunt = blunt && x.kind == HitKind::Blunt;
    }
    INFO(landed << " blows landed; health " << r.a->health << ", " << r.b->health);
    MESSAGE(pn << ": " << landed << " blows landed; health " << r.a->health << ", " << r.b->health);
    CHECK(landed >= 3);
    CHECK(blunt);
    CHECK((r.a->health < r.a->max_health && r.b->health < r.b->max_health));
  }
}

TEST_CASE("melee: a knife cuts voxels out of the opponent") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    Scene s(path);
    const Brawl r = brawl(s, true);
    i32 cuts = 0;
    size_t removed = 0;
    for (const LandedBlow& x : r.blows) {
      if (x.attacker == r.b || x.kind != HitKind::Blade) continue;
      ++cuts;
      removed += x.result.removed.size();
    }
    INFO(cuts << " cuts, " << removed << " voxels cut out; health " << r.b->health);
    MESSAGE(pn << ": " << cuts << " cuts, " << removed << " voxels cut out; health " << r.b->health);
    CHECK(cuts >= 1);
    CHECK(removed > 0);
    CHECK(r.b->health < r.b->max_health);
  }
}

TEST_CASE("melee: a blade cut carves voxels; a punch to the head hurts and is a head hit") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    {
      Scene s(path);
      Character& c = civilian(s);
      const u32 v0 = c.geometry_version;
      const V3 chest = c.pose.p[H::chest];
      const WoundResult cut = c.melee(V3{chest.x, chest.y + 0.12, chest.z + 0.05}, V3{0, -1, 0}, HitKind::Blade, 1.0);
      INFO(cut.removed.size() << " voxels cut out");
      CHECK(!cut.removed.empty());
      CHECK(c.geometry_version > v0);
      CHECK(c.owns_model);
    }
    Scene s(path);
    Character& d = civilian(s);
    const f64 h0 = d.health;
    const V3 head = d.pose.p[H::head];
    const WoundResult punch = d.melee(V3{head.x, head.y + 0.1, head.z + 0.1}, V3{0, -1, 0}, HitKind::Blunt, 1.0);
    const std::string zone = zone_name(punch.zone);
    INFO("a punch: " << zone << ", health " << h0 << " -> " << d.health << ", " << punch.removed.size() << " voxels cut out");
    CHECK(punch.zone == Zone::Head);
    CHECK(punch.headshot);
    CHECK((d.health < h0 && punch.damage > 0.0));
    CHECK(punch.removed.empty());
  }
}
