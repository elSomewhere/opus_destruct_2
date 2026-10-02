// Public seams: the same calls drive a shallow body, a World articulation and the workbench.
#include "doctest.h"
#include "scene.hpp"
#include "svx/anim/damage/strike.hpp"
#include "svx/anim/damage/anatomy.hpp"
using namespace scene;

TEST_CASE("props: every catalog definition attaches, carries mass and releases with identity on both backends") {
  for (Path path : {Path::Shallow, Path::Deep})
    for (const auto& archetype : prop_catalog()) {
      INFO(path_name(path));
      CAPTURE(archetype->id);
      Scene s(path);
      auto& c = s.civilian();
      s.frame({&c});
      const f64 mass = c.body.total_mass;
      for (const auto& name : archetype->attachments) {
        AttachPoint point = AttachPoint::RightHand;
        for (int i = 0; i < 9; ++i)
          if (name == attachment_name(AttachPoint(i))) point = AttachPoint(i);
        const auto style = hand_point(point) ? (archetype->has("two_handed") ? WieldStyle::TwoHands
                                                : archetype->has("hanging")  ? WieldStyle::Hanging
                                                                             : WieldStyle::OneHand)
                                             : WieldStyle::Worn;
        const char* socket = hand_point(point) ? "primary" : archetype->socket("strap") ? "strap" : "wear";
        auto instance = c.attachments().registry->create(archetype);
        REQUIRE(c.attach(instance, point, socket, style));
        CHECK(c.body.parts.size() == 16);
        CHECK(c.body.total_mass == doctest::Approx(mass + archetype->mass));
        const auto id = instance->id;
        const int part = HumanoidBody::body_of_bone(attachment_bone(point));
        c.body.parts[size_t(part)]->v = {.4, .3, .2};
        c.body.parts[size_t(part)]->w = {0, 0, 0};
        auto released = c.detach(point);
        REQUIRE(released);
        CHECK(released->id == id);
        CHECK(released->location == PropLocation::Loose);
        CHECK(norm(released->velocity - V3{.4, .3, .2}) < 1e-8);
        CHECK(c.body.total_mass == doctest::Approx(mass));
        c.attachments().registry->update(DT);
        REQUIRE(released->loose_body);
        CHECK(c.attach(released, point, socket, style));
        CHECK_FALSE(released->loose_body);
        c.detach(point);
      }
    }
}
TEST_CASE("props: invalid swaps preserve occupancy and overloaded or disabled hands release") {
  for (Path path : {Path::Shallow, Path::Deep}) {
    Scene s(path);
    auto& c = s.civilian();
    REQUIRE(c.swap(prop_archetype("bat"), AttachPoint::RightHand, "primary", WieldStyle::TwoHands));
    const auto bat = c.attachments().held();
    CHECK_FALSE(c.swap(prop_archetype("phone"), AttachPoint::LeftHand, "primary", WieldStyle::OneHand));
    CHECK(c.attachments().held() == bat);
    c.wrench(AttachPoint::RightHand, {100, 0, 0});
    CHECK(bat->location == PropLocation::Loose);
    CHECK(bat->last_release == ReleaseReason::Wrenched);
    REQUIRE(c.swap(prop_archetype("knife"), AttachPoint::LeftHand));
    auto knife = c.attachments().held();
    auto cap = c.capabilities();
    cap.arms[0].grip = 0;
    c.behaviours.damage.override_capabilities(cap);
    s.frame({&c});
    CHECK(knife->location == PropLocation::Loose);
    CHECK_FALSE(c.motion.play("batSwing"));
    CHECK_FALSE(c.motion.action_refusal.empty());
  }
}
TEST_CASE("props: the support hand stays at the physical secondary grip during a swing") {
  for (Path path : {Path::Shallow, Path::Deep})
    for (bool left : {false, true}) {
      INFO(path_name(path));
      CAPTURE(left);
      Scene s(path);
      auto& c = s.civilian();
      REQUIRE(c.swap(prop_archetype("bat"), left ? AttachPoint::LeftHand : AttachPoint::RightHand, "primary", WieldStyle::TwoHands));
      for (int i = 0; i < 120; ++i) s.frame({&c});
      REQUIRE(c.motion.play(left ? "batSwing.m" : "batSwing"));
      f64 error2 = 0, peak = 0;
      for (int i = 0; i < 180; ++i) {
        s.frame({&c});
        const auto item = c.attachments().held();
        REQUIRE(item);
        const size_t bone = left ? H::handR : H::handL;
        const Side side = left ? Side::R : Side::L;
        const V3 palm = c.pose.p[bone] + rotate(c.pose.q[bone], c.motion.arms.palm_offset(side));
        const V3 socket = item->pos + rotate(item->rotation, item->archetype->socket("secondary")->point);
        const f64 error = norm(palm - socket);
        error2 += error * error;
        peak = std::max(peak, error);
      }
      const f64 rms = std::sqrt(error2 / 180);
      CAPTURE(rms);
      CAPTURE(peak);
      CHECK(rms < .09);
      CHECK(peak < .20);
    }
}
TEST_CASE("wounds: projectile channels, blunt fractures and persistent anatomy on both backends") {
  for (Path path : {Path::Shallow, Path::Deep}) {
    INFO(path_name(path));
    Scene s(path);
    auto& c = s.civilian();
    s.frame({&c});
    const V3 at = c.body.parts[B::thighL]->x;
    auto hit = c.raycast(at + V3{0, 1, 0}, {0, -1, 0}, 2);
    REQUIRE(hit);
    DamageDescriptor shot;
    shot.kind = DamageKind::Projectile;
    shot.point = hit->point;
    shot.direction = {0, -1, 0};
    shot.bone = hit->bone;
    const int count = c.model->voxel_count();
    auto result = c.damage(shot);
    CHECK_FALSE(result.removed.empty());
    CHECK(c.model->voxel_count() < count);
    CHECK(c.capabilities().legs[0].support < 1);
    CHECK(c.behaviours.damage.inspect().wounds.size() > 0);
    const f64 blood = c.behaviours.damage.inspect().blood;
    c.behaviours.damage.update(5);
    CHECK(c.behaviours.damage.inspect().blood < blood);
    DamageDescriptor blunt;
    blunt.kind = DamageKind::Blunt;
    blunt.mass = 4;
    blunt.speed = 15;
    blunt.area = .008;
    blunt.point = c.body.parts[B::shinR]->x;
    blunt.bone = H::shinR;
    const int before = c.model->voxel_count();
    c.damage(blunt);
    CHECK(c.model->voxel_count() == before);
    CHECK(c.behaviours.damage.inspect().parts[B::shinR].bone == BoneState::Shattered);
    CHECK_FALSE(c.body.parts[B::shinR]->gone);
    const auto record = c.damage_record();
    auto& restored = s.civilian();
    REQUIRE(restored.restore_damage(record));
    CHECK(restored.damage_record() == record);
    auto bad = record;
    bad.pop_back();
    auto& intact = s.civilian();
    const auto original = intact.model;
    CHECK_FALSE(intact.restore_damage(bad));
    CHECK(intact.model == original);
  }
}
TEST_CASE("wounds: capabilities drive automatic crawl and action refusal without prone input") {
  for (Path path : {Path::Shallow, Path::Deep}) {
    Scene s(path);
    auto& c = s.civilian();
    auto cap = c.capabilities();
    cap.legs[0] = {0, 0, 0};
    cap.legs[1] = {0, 0, 0};
    cap.mobility = Mobility::Crawl;
    cap.max_speed = .32;
    for (size_t i = 10; i < 16; ++i) cap.muscle[i] = 0;
    c.behaviours.damage.override_capabilities(cap);
    for (int i = 0; i < 180; ++i) s.frame({&c});
    CHECK(c.motion.input.stance == Stance::Prone);
    CHECK_FALSE(c.motion.play("frontKick"));
    CHECK(c.pose.p[H::pelvis].z - s.ground < .5);
    for (const auto& p : c.pose.p) CHECK(std::isfinite(norm(p)));
  }
}
TEST_CASE("props: all release reasons preserve the loose item instead of evicting it") {
  Scene s(Path::Shallow);
  auto& c = s.civilian();
  for (int i = 0; i < 9; ++i) {
    REQUIRE(c.swap(prop_archetype("bottle"), AttachPoint::RightHand));
    auto p = c.detach(AttachPoint::RightHand, ReleaseReason(i));
    REQUIRE(p);
    CHECK(p->last_release == ReleaseReason(i));
  }
  auto registry = c.attachments().registry;
  CHECK(registry->all().size() == 9);
  for (int i = 0; i < 300; ++i) registry->update(DT);
  CHECK(registry->nearby({0, 0, 0}, 10).size() == 9);
}

TEST_CASE("props: loose records retain state and reject partial input atomically") {
  PropRegistry original;
  auto item = original.create(prop_archetype("shopping_bag"));
  item->state.contents = {"reserved contents"};
  item->state.condition = .6;
  item->pos = {1, 2, 3};
  item->velocity = {.2, .3, .4};
  item->last_release = ReleaseReason::Wrenched;
  const auto bytes = original.record_loose();
  PropRegistry restored;
  REQUIRE(restored.restore_loose(bytes));
  CHECK(restored.record_loose() == bytes);
  REQUIRE(restored.get(item->id));
  CHECK(restored.get(item->id)->state.contents == item->state.contents);
  auto bad = bytes;
  bad.pop_back();
  CHECK_FALSE(restored.restore_loose(bad));
  CHECK(restored.record_loose() == bytes);
  CHECK(restored.create(prop_archetype("phone"))->id == item->id + 1);
}

TEST_CASE("props: sustained carrying keeps a suitcase while shortening the gait on both backends") {
  for (Path path : {Path::Shallow, Path::Deep}) {
    Scene s(path);
    auto& c = s.civilian();
    for (int i = 0; i < 90; ++i) s.frame({&c});
    REQUIRE(c.swap(prop_archetype("suitcase"), AttachPoint::RightHand, "primary", WieldStyle::Hanging));
    const auto item = c.attachments().held();
    for (int i = 0; i < 240; ++i) {
      c.set_root(c.motion.root_pos + V3{0, DT, 0}, kPi / 2);
      s.frame({&c});
    }
    CHECK(item->location == PropLocation::Attached);
    CHECK(c.motion.load_fraction > .5);
    CHECK(norm(c.motion.load_lean) > .05);
  }
}

#include "svx/anim/damage/scenarios.hpp"
TEST_CASE("wounds: anatomical presets distinguish soft tissue, fractures, severing and disabled legs") {
  for (Path path : {Path::Shallow, Path::Deep})
    for (auto name : kDamageScenarios) {
      INFO(path_name(path));
      CAPTURE(name);
      Scene s(path);
      auto& c = s.civilian();
      for (int i = 0; i < 60; ++i) s.frame({&c});
      const auto descriptors = damage_scenario(c, name);
      REQUIRE_FALSE(descriptors.empty());
      const int cells = c.model->voxel_count();
      for (const auto& d : descriptors) c.damage(d);
      const auto state = c.behaviours.damage.inspect();
      CHECK_FALSE(state.wounds.empty());
      if (name == "thighShot") {
        CHECK(c.capabilities().legs[0].support < 1);
        CHECK(c.capabilities().legs[0].support > .3);
      }
      if (name == "femoralBleed") CHECK(std::any_of(state.wounds.begin(), state.wounds.end(), [](const auto& w) { return w.arterial; }));
      if (name == "shatteredKnee") {
        CHECK(state.parts[B::shinL].bone == BoneState::Shattered);
        CHECK(c.model->voxel_count() == cells);
      }
      if (name == "forearmSever") CHECK(c.behaviours.lost[B::handR]);
      if (name == "shotgunLegs") CHECK(c.capabilities().mobility == Mobility::Crawl);
      for (int i = 0; i < 180; ++i) s.frame({&c});
      for (const auto& point : c.pose.p) CHECK(std::isfinite(norm(point)));
      if (name == "shotgunLegs") CHECK(c.motion.input.stance == Stance::Prone);
      if (name == "blast3m") CHECK(c.model->voxel_count() > cells / 2);
    }
}
TEST_CASE("wounds: pressure reduces only the pressed wound, and blood loss incapacitates") {
  Scene s(Path::Shallow);
  auto& c = s.civilian();
  s.frame({&c});
  for (const auto& d : damage_scenario(c, "femoralBleed")) c.damage(d);
  auto open = c.behaviours.damage, pressed = open, elsewhere = open;
  open.update(10);
  pressed.update(10, B::thighL);
  elsewhere.update(10, B::forearmL);
  CHECK(pressed.inspect().blood > open.inspect().blood);
  CHECK(elsewhere.inspect().blood == open.inspect().blood);
  open.update(120);
  CHECK(open.capabilities().vigor < .15);
  CHECK(open.capabilities().mobility == Mobility::Immobile);
  for (int i = 0; i < 20; ++i) {
    DamageDescriptor d;
    d.kind = DamageKind::Blunt;
    d.mass = 1;
    d.speed = 2;
    d.point = c.body.parts[B::chest]->x;
    c.damage(d);
  }
  CHECK(c.behaviours.damage.inspect().wounds.size() > 12);
}
TEST_CASE("props: physiological releases and strong severed grips keep their identity") {
  for (Path path : {Path::Shallow, Path::Deep}) {
    Scene s(path);
    auto& c = s.civilian();
    REQUIRE(c.swap(prop_archetype("knife"), AttachPoint::RightHand));
    for (int i = 0; i < 30; ++i) s.frame({&c});
    auto knife = c.attachments().held();
    REQUIRE(knife);
    for (const auto& d : damage_scenario(c, "forearmSever")) c.damage(d);
    CHECK(knife->location == PropLocation::Loose);
    CHECK(knife->last_release == ReleaseReason::AnchorLost);
    CHECK(knife->retained_mass > 0);
    CHECK(knife->model().voxel_count() > knife->archetype->model->voxel_count());
    auto& other = s.civilian();
    REQUIRE(other.swap(prop_archetype("backpack"), AttachPoint::Back, "wear", WieldStyle::Worn));
    const auto bag = other.attachments().at(AttachPoint::Back);
    bag->state.strap = 0;
    s.frame({&other});
    CHECK(bag->last_release == ReleaseReason::StrapCut);
    REQUIRE(other.swap(prop_archetype("phone"), AttachPoint::LeftHand));
    auto phone = other.attachments().held();
    other.knock_out(3);
    s.frame({&other});
    CHECK(phone->last_release == ReleaseReason::KnockedOut);
  }
}
