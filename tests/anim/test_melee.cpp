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
#include "svx/anim/damage/strike.hpp"
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

// A civilian standing a moment, facing +y.
Character& civilian(Scene& s) {
  Character& c = s.add(make_civilian(5), 1.0, kPi / 2.0, V3{0, 0, s.ground});
  for (i32 i = 0; i < 20; ++i) s.frame({&c});
  return c;
}


}  // namespace

// ---- knives --------------------------------------------------------------------------------------

TEST_CASE("melee: a knife fighter holds a knife guard, blade forward") {
  auto a = standing();
  a->weapon = prop_archetype("knife");
  a->input.guard = true;
  live(*a, 1.0);
  CHECK(a->pose_action_name() == "knifeGuard");
  const V3 tip = a->prop_point(a->weapon->tip);
  const V3 chest = a->world.p[H::chest];
  INFO("the blade is " << tip.y - chest.y << " m in front of the chest");
  CHECK(tip.y - chest.y > 0.3);
}

TEST_CASE("melee: every knife attack reaches a body in front of it") {
  for (const std::string_view name : kKnifeAttacks) {
    const std::string nm(name);
    INFO(nm);
    auto a = standing();
    a->weapon = prop_archetype("knife");
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

TEST_CASE("melee: an armed body standing easy fidgets (helmet, brow, shoulders, weapon, a look round); aiming, it does not") {
  for (const Carry carry : {Carry::Ready, Carry::Aim}) {
    const std::string cn = carry == Carry::Ready ? "ready" : "aim";
    INFO(cn);
    auto a = standing();
    a->weapon = prop_archetype("rifle");
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
  a->weapon = prop_archetype("rifle");
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
