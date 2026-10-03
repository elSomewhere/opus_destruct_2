// Contact velocity belongs to the point that landed, on each moving body.
#include "doctest.h"
#include "scene.hpp"
#include "svx/anim/brawl.hpp"

using namespace scene;

namespace {
StrikeSweep crossing(const V3& point) {
  StrikeSweep s;
  s.descriptor.kind = DamageKind::Point;
  s.descriptor.mass = 2;
  s.descriptor.duration = DT;
  s.descriptor.speed = 6;
  s.descriptor.direction = {0, -1, 0};
  s.descriptor.diameter = .001;
  s.previous_a = s.previous_b = point + V3{0, .05, 0};
  s.a = s.b = point - V3{0, .05, 0};
  return s;
}
void still(Character& c) {
  for (auto* part : c.body.parts) part->v = part->w = {};
  c.prev_pose.copy_from(c.pose);
}
PropPtr shield(f64 retention) {
  auto p = std::make_shared<Prop>(*prop_archetype("sword"));
  VoxelPart cells;
  cells.bone = 0;
  cells.origin = {-2, -1, -2};
  cells.dims = {4, 2, 4};
  cells.cells.assign(32, Slot::Metal + 1);
  cells.count = cells.initial_count = 32;
  p->model = std::make_shared<VoxelModel>(p->model->skeleton, .05, std::vector<VoxelPart>{cells});
  p->centre = {};
  for (auto& socket : p->sockets) socket.retention = retention;
  return p;
}
V3 momentum(const Character& c) {
  V3 p;
  for (const auto* part : c.body.parts) p += part->v * part->mass;
  return p;
}
}  // namespace

TEST_CASE("strikes: relative velocity includes the target's motion at the contact point") {
  for (Path path : {Path::Shallow, Path::Deep})
    for (bool physical : {false, true}) {
      Scene s(path);
      auto& c = s.civilian();
      c.physics = physical;
      run(s, c, 1);
      REQUIRE(c.behaviours.physical == physical);
      still(c);
      const auto entry = c.raycast(c.pose.p[H::chest] + V3{0, 1, .08}, {0, -1, 0}, 2);
      REQUIRE(entry);
      const auto sweep = crossing(entry->point);
      const V3 translation{1, -2, .25}, angular{0, 0, 3};
      for (size_t part = 0; part < kBodyCount; ++part) {
        auto& b = *c.body.parts[part];
        b.v = translation;
        b.w = angular;
        const size_t bone = size_t(kBodyBone[part]);
        c.prev_pose.p[bone] = c.pose.p[bone] - translation * DT;
        c.prev_pose.q[bone] = qz(-3 * DT) * c.pose.q[bone];
      }
      const auto hit = StrikeTracker::contact(sweep, c);
      REQUIRE(hit);
      const size_t part = size_t(HumanoidBody::body_of_bone(hit->bone));
      const V3 pivot = physical ? c.body.parts[part]->x : c.pose.p[size_t(hit->bone)];
      const V3 expected = V3{0, -6, 0} - translation - cross(angular, hit->point - pivot);
      CHECK(norm(hit->direction * hit->speed - expected) < 1e-8);
      CHECK(hit->energy() == doctest::Approx(norm2(expected)));
      still(c);
      for (auto* b : c.body.parts) b->v = {0, -6, 0};
      for (size_t bone = 0; bone < c.pose.p.size(); ++bone) c.prev_pose.p[bone] = c.pose.p[bone] + V3{0, 6 * DT, 0};
      CHECK_FALSE(StrikeTracker::contact(sweep, c));
    }
}

TEST_CASE("strikes: moving blocking props use their anchor's contact velocity") {
  for (Path path : {Path::Shallow, Path::Deep}) {
    Scene s(path);
    auto& c = s.civilian();
    run(s, c, 1);
    REQUIRE(c.swap(shield(10000), AttachPoint::LeftHand));
    const auto item = c.attachments().held();
    item->rotation = {};
    item->pos = c.pose.p[H::chest] + V3{0, .5, .05};
    still(c);
    auto& arm = *c.body.parts[B::handL];
    arm.v = {1, 1, 0};
    arm.w = {0, 0, 2};
    const auto sweep = crossing(item->pos + V3{0, .05, 0});
    const auto hit = StrikeTracker::contact(sweep, c);
    REQUIRE(hit);
    CHECK(hit->blocked);
    CHECK(hit->target_prop == item->id);
    CHECK(hit->bone == -1);
    const V3 expected = V3{0, -6, 0} - arm.v - cross(arm.w, hit->point - arm.x);
    CHECK(norm(hit->direction * hit->speed - expected) < 1e-8);
    arm.v = {0, -8, 0};
    arm.w = {};
    CHECK_FALSE(StrikeTracker::contact(sweep, c));
  }
}

TEST_CASE("strikes: a rotating edge sweeps even when its centre stays still") {
  Scene s(Path::Shallow);
  auto& c = s.civilian();
  REQUIRE(c.swap(prop_archetype("knife"), AttachPoint::RightHand));
  REQUIRE(c.motion.play("slash"));
  c.motion.strike_weight[1] = 1;
  auto item = c.attachments().held();
  const auto* edge = item->archetype->feature("edge");
  REQUIRE(edge);
  const V3 centre = (edge->a + edge->b) * .5;
  auto pose = [&](f64 angle) {
    item->rotation = qx(angle);
    item->pos = V3{0, 0, 1} - rotate(item->rotation, centre);
  };
  StrikeTracker tracker;
  pose(-.2);
  CHECK(tracker.sample(c, DT).empty());
  pose(.2);
  const auto sweeps = tracker.sample(c, DT);
  REQUIRE(sweeps.size() == 1);
  CHECK(norm(sweeps[0].velocity(.5)) < 1e-8);
  CHECK(norm(sweeps[0].velocity(0)) > .5);
  CHECK(norm(sweeps[0].velocity(1)) > .5);
  c.detach(AttachPoint::RightHand);
  CHECK(tracker.sample(c, DT).empty());
  REQUIRE(c.swap(prop_archetype("knife"), AttachPoint::RightHand));
  CHECK(tracker.sample(c, DT).empty());  // a new instance starts a new path
}

TEST_CASE("strikes: the first contact is chosen by time along the sweep") {
  for (Path path : {Path::Shallow, Path::Deep}) {
    Scene s(path);
    auto& c = s.civilian();
    run(s, c, .2);
    REQUIRE(c.swap(shield(10000), AttachPoint::LeftHand));
    REQUIRE(c.swap(shield(10000), AttachPoint::RightHand));
    const auto slow = c.attachments().at(AttachPoint::LeftHand), fast = c.attachments().at(AttachPoint::RightHand);
    slow->rotation = fast->rotation = {};
    slow->pos = {-.4, 2.09, 2.5};
    fast->pos = {.4, 1.15, 2.5};
    still(c);
    StrikeSweep sweep;
    sweep.descriptor.kind = DamageKind::Edge;
    sweep.descriptor.diameter = .001;
    sweep.descriptor.duration = DT;
    sweep.previous_a = {-.4, 2, 2.5};
    sweep.a = {-.4, 2.2, 2.5};
    sweep.previous_b = {.4, .8, 2.5};
    sweep.b = {.4, 2.8, 2.5};
    const auto hit = StrikeTracker::contact(sweep, c);
    REQUIRE(hit);
    // The right end travels farther before contact, but arrives earlier.
    CHECK(hit->target_prop == fast->id);
  }
}

TEST_CASE("strikes: blade roll distinguishes the edge from its flat side") {
  StrikeSweep s;
  s.descriptor.kind = DamageKind::Edge;
  s.descriptor.duration = DT;
  s.previous_a = {0, -.2, 0};
  s.previous_b = {0, .2, 0};
  s.a = {0, -.2, .1};
  s.b = {0, .2, .1};
  s.normal = s.previous_normal = {1, 0, 0};
  CHECK(s.impact(.5, {}).alignment == doctest::Approx(1));
  s.descriptor.mass = 2;
  s.descriptor.swept_length = .4;
  const auto cut = s.impact(.5, {});
  s.normal = s.previous_normal = {0, 0, 1};
  CHECK(s.impact(.5, {}).alignment == doctest::Approx(0));
  const auto flat = s.impact(.5, {});
  s.normal = s.previous_normal = vnorm(V3{1, 0, 1});
  CHECK(s.impact(.5, {}).alignment == doctest::Approx(std::sqrt(.5)));
  VoxelPart cells;
  cells.bone = 0;
  cells.origin = {-2, -3, 0};
  cells.dims = {4, 6, 6};
  cells.cells.assign(144, Slot::Flesh + 1);
  cells.count = cells.initial_count = 144;
  VoxelModel model(humanoid_skeleton(), .02, {cells});
  std::array<f32, 16> skin;
  write_rigid(skin.data(), {}, {}, {});
  auto edged_model = model.clone(), flat_model = model.clone();
  const auto edged = wound_mechanics(*edged_model, skin, cut), blunted = wound_mechanics(*flat_model, skin, flat);
  CHECK_FALSE(edged.removed.empty());
  CHECK(blunted.removed.empty());
  CHECK(blunted.remaining_energy == 0);
}

TEST_CASE("strikes: a stopped attack transfers momentum to the blocking arm and the striking grip") {
  for (Path path : {Path::Shallow, Path::Deep})
    for (bool retained : {false, true}) {
      Scene s(path);
      auto& attacker = s.civilian(3, kPi / 2, {0, 1, 0});
      auto& defender = s.civilian();
      run(s, attacker, .2);
      run(s, defender, .2);
      auto weapon = std::make_shared<Prop>(*prop_archetype("knife"));
      for (auto& socket : weapon->sockets) socket.retention = retained ? 10000 : 10;
      REQUIRE(attacker.swap(weapon, AttachPoint::RightHand));
      REQUIRE(defender.swap(shield(retained ? 10000 : 10), AttachPoint::LeftHand));
      auto source = attacker.attachments().held(), block = defender.attachments().held();
      still(attacker);
      still(defender);
      block->rotation = {};
      block->pos = defender.pose.p[H::chest] + V3{0, .5, .05};
      const V3 point = block->pos + V3{0, .05, 0};
      source->rotation = {};
      source->pos = point + V3{0, .05, 0} - weapon->feature("tip")->b;
      REQUIRE(attacker.motion.play("stab"));
      attacker.motion.strike_weight[1] = 1;
      Brawler fighter(attacker);
      fighter.opponent = &defender;
      CHECK(fighter.resolve({}).empty());
      source->pos.y -= .1;
      const auto blows = fighter.resolve({});
      REQUIRE(blows.size() == 1);
      const auto& hit = blows[0];
      CHECK(hit.blocked);
      CHECK(hit.result.damage == 0);
      CHECK(hit.result.absorbed_energy == doctest::Approx(hit.descriptor.energy()));
      CHECK(norm(hit.result.impulse - hit.descriptor.direction * hit.descriptor.momentum()) < 1e-8);
      if (retained) {
        CHECK(source->location == PropLocation::Attached);
        CHECK(block->location == PropLocation::Attached);
        CHECK(norm(momentum(defender) - hit.result.impulse) < 1e-8);
        CHECK(norm(momentum(attacker) + hit.result.impulse) < 1e-8);
        CHECK(norm(defender.body.parts[B::handL]->w) > .01);
        CHECK(norm(attacker.body.parts[B::handR]->w) > .01);
      } else {
        CHECK(source->location == PropLocation::Loose);
        CHECK(block->location == PropLocation::Loose);
        CHECK(source->last_release == ReleaseReason::Wrenched);
        CHECK(block->last_release == ReleaseReason::Wrenched);
      }
    }
}

TEST_CASE("strikes: an immediate wrench uses both grips and current capability") {
  for (Path path : {Path::Shallow, Path::Deep}) {
    Scene s(path);
    auto& c = s.civilian();
    run(s, c, .2);
    REQUIRE(c.swap(prop_archetype("sword"), AttachPoint::RightHand, "primary", WieldStyle::TwoHands));
    const auto item = c.attachments().held();
    CHECK(item->strength > 1.8);
    c.wrench(AttachPoint::RightHand, {0, 40, 0});
    CHECK(item->location == PropLocation::Attached);
    auto cap = c.capabilities();
    cap.arms[0].grip = cap.arms[1].grip = 0;
    c.behaviours.damage.override_capabilities(cap);
    c.wrench(AttachPoint::RightHand, {0, 1, 0});
    CHECK(item->location == PropLocation::Loose);
    CHECK(item->last_release == ReleaseReason::Wrenched);
  }
}
