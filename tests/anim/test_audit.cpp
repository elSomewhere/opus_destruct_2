// The architecture audit's fixes, each pinned: the damage mechanics bit for bit on every platform,
// an empty blast, archetypes of the host's own, bounded and reported state, stains that leave what
// a cell is made of, world writes out of the parallel phase, the arm a strike is thrown with, the
// weight of a prop at rest, girth, the loss of the head, a blunt blow through a prop, and the
// legacy profile, a volley of pellets and the fixtures a host would otherwise write itself.
#include <bit>
#include <set>

#include "scene.hpp"
#include "svx/anim/content.hpp"
#include "svx/anim/damage/anatomy.hpp"
#include "svx/anim/damage/scenarios.hpp"
#include "svx/anim/damage/strike.hpp"
#include "svx/anim/system.hpp"
#include "svx/base/parallel.hpp"

using namespace scene;

namespace {

u64 fnv(std::span<const u8> data) {
  u64 h = 14695981039346656037ULL;
  for (u8 v : data) h = (h ^ v) * 1099511628211ULL;
  return h;
}

// Absorbed energy of a blast on a fresh standing civilian (`pressure`: over the ambient).
f64 blast_absorbed(f64 pressure, f64 distance, i32 fragments = 0, f64 speed = 0) {
  Scene s(Path::Shallow);
  auto& c = s.civilian();
  s.frame({&c});
  DamageDescriptor d;
  d.kind = DamageKind::Blast;
  d.point = c.bounds_center() + V3{0, distance, 0};
  d.direction = V3{0, -1, 0};
  d.radius = 1;
  d.pressure = 101325 + pressure;
  d.fragments = fragments;
  d.mass = .001;
  d.speed = speed;
  return c.damage(d).absorbed_energy;
}

}  // namespace

TEST_CASE("audit: blunt tissue mechanics give the same bits on every platform (a pinned transcript)") {
  // (the same deposits natively and in WASM: the bundled exp, no platform libm on the way)
  const auto human = make_civilian(3);
  VoxelPart part;
  part.bone = H::chest;
  part.origin = {-4, 0, -4};
  part.dims = {9, 6, 9};
  part.cells.resize(9 * 6 * 9, Slot::Flesh + 1);
  part.count = part.initial_count = i32(part.cells.size());
  std::array<f32, 23 * 16> skin{};
  for (int i = 0; i < 23; i++) write_rigid(skin.data() + i * 16, {}, Quat{}, {});
  std::vector<u8> bytes;
  for (int n = 0; n < 100; n++) {
    VoxelModel model(human.model->skeleton, .02, {part});
    DamageDescriptor d;
    d.kind = DamageKind::Blunt;
    d.mass = 2;
    d.speed = 12;
    d.point = {.001 + n * .00013, 0, .003};
    for (const auto& t : wound_mechanics(model, skin, d).tissue) {
      const auto b = std::bit_cast<u64>(t.energy);
      for (int i = 0; i < 8; i++) bytes.push_back(u8(b >> (8 * i)));
    }
  }
  CHECK(bytes.size() == 141800);
  CHECK(fnv(bytes) == 7302497290508645330ull);
}

TEST_CASE("audit: an empty blast does nothing; a blast scales with its source and fades with distance") {
  {
    Scene s(Path::Shallow);
    auto& c = s.civilian();
    const i32 before = c.model->voxel_count();
    DamageDescriptor zero;
    zero.kind = DamageKind::Blast;
    zero.speed = 0;
    zero.pressure = 0;
    zero.fragments = 0;
    zero.point = c.bounds_center();
    zero.radius = 3;
    REQUIRE(zero.valid());
    const auto r = c.damage(zero);
    CHECK(r.absorbed_energy == 0);
    CHECK(r.removed.empty());
    CHECK(c.model->voxel_count() == before);
    CHECK(c.alive());
  }
  // (continuous at zero, more with more pressure, less further away)
  const f64 faint = blast_absorbed(1.0, 1.0), weak = blast_absorbed(1e4, 1.0), strong = blast_absorbed(1e5, 1.0);
  MESSAGE("blast absorbed: 1 Pa " << faint << " J, 1e4 Pa " << weak << " J, 1e5 Pa " << strong << " J; 3e4 " << blast_absorbed(3e4, 1.0));
  CHECK(faint < 1.0);
  CHECK(weak > faint);
  CHECK(strong > weak);
  const f64 near = blast_absorbed(1e5, .8), mid = blast_absorbed(1e5, 1.6), far = blast_absorbed(1e5, 3.0);
  CHECK(near > mid);
  CHECK(mid > far);
  // fragments with no speed are no source; with speed they are
  CHECK(blast_absorbed(0, 1.0, 256, 0) == 0);
  CHECK(blast_absorbed(0, 1.0, 4000, 350) > 0);
  // (and the ambient air is no overpressure)
  CHECK(blast_absorbed(0, 1.0) == 0);
}

TEST_CASE("audit: a blast wounds a body as the scripted one did: limbs torn only within half its radius") {
  // (the game's blast and the workbench's grenade, two seconds on)
  auto after = [](auto hit) {
    Scene s(Path::Shallow);
    auto& c = s.civilian();
    for (int i = 0; i < 30; ++i) s.frame({&c});
    const i32 before = c.model->voxel_count();
    hit(c);
    for (int i = 0; i < 120; ++i) s.frame({&c});
    i32 lost = 0;
    for (bool l : c.behaviours.lost) lost += l;
    return std::tuple{before - c.model->voxel_count(), lost, c.alive()};
  };
  for (const f64 d : {1.0, 2.0, 3.0}) {
    const auto [removed, lost, alive] = after([&](Character& c) { c.blast(c.bounds_center() + V3{0, d, -.4}, 2, 1); });
    MESSAGE("a rocket's blast (radius 2) at " << d << " m: " << removed << " cells, " << lost << " parts lost");
    CHECK(removed > 0);
    CHECK(removed < 200);
    CHECK(lost == 0);
    CHECK(alive);
  }
  const auto [removed, lost, alive] = after([](Character& c) {
    for (const auto& d : damage_scenario(c, "blast3m")) c.damage(d);
  });
  MESSAGE("the grenade at 3 m: " << removed << " cells, " << lost << " parts lost");
  CHECK(removed > 0);
  CHECK(lost == 0);
  CHECK(alive);
  // (the grenade against the chest: two limbs, as the scripted blast tore; a rocket's at .4 m: one)
  const auto [contact, torn, _] = after([](Character& c) {
    DamageDescriptor d;
    d.kind = DamageKind::Blast;
    d.point = c.pose.p[H::chest] + V3{0, .15, 0};
    d.radius = 3;
    d.pressure = 101325 + 38000;
    d.fragments = 3900;
    d.mass = .001;
    d.speed = 350;
    c.damage(d);
  });
  MESSAGE("the grenade at the chest: " << contact << " cells, " << torn << " parts lost");
  CHECK(torn == 2);
  CHECK(contact < 600);
  const auto [near, one, __] = after([](Character& c) { c.blast(c.bounds_center() + V3{0, .4, -.4}, 2, 1); });
  MESSAGE("a rocket's at .4 m: " << near << " cells, " << one << " parts lost");
  CHECK(one == 1);
}

TEST_CASE("audit: injuries past the cap join others of their zone; what the body feels stays") {
  Injuries kept, all;
  all.cap = 100000;
  auto hurt = [&](Zone zone, i32 part, f64 severity) {
    Injury i;
    i.zone = zone;
    i.part = part;
    i.severity = severity;
    i.lasting = severity * .4;
    i.hold_until = 3;
    kept.add(i);
    all.add(i);
    kept.update(.5);
    all.update(.5);
  };
  hurt(Zone::LegL, B::thighL, .7);  // (the leg first, then a fight's worth of blows elsewhere)
  for (int n = 0; n < 40; ++n) hurt(n % 3 == 0 ? Zone::Chest : n % 3 == 1 ? Zone::ArmR : Zone::Gut, B::chest, .3 + .01 * n);
  for (int n = 0; n < 60; ++n) {
    kept.update(.5);
    all.update(.5);
    CHECK(kept.legL == doctest::Approx(all.legL).epsilon(.02));
    CHECK(kept.armR == doctest::Approx(all.armR).epsilon(.02));
    CHECK(kept.trunk == doctest::Approx(all.trunk).epsilon(.02));
    CHECK(kept.pain == doctest::Approx(all.pain).epsilon(.05));
  }
  CHECK(kept.list.size() <= kept.cap);
  CHECK(kept.legL > .2);  // (the limp stays)
}

TEST_CASE("audit: a prop of the host's own is saved and restored; one that does not resolve is refused") {
  auto custom = std::make_shared<Prop>(*prop_archetype("sword"));
  custom->id = "audit-custom";
  PropRegistry authored;
  REQUIRE(authored.create(custom));
  const auto record = authored.record_loose();
  CHECK(authored.restore_loose(record));  // (made from it: defined in its registry)
  PropRegistry fresh;
  CHECK_FALSE(fresh.restore_loose(record));
  PropRegistry defined;
  defined.define(custom);
  REQUIRE(defined.restore_loose(record));
  REQUIRE(defined.all().size() == 1);
  CHECK(defined.all().begin()->second->archetype == custom);
  PropRegistry resolved;
  resolved.resolver = [&](std::string_view id) { return id == "audit-custom" ? PropPtr(custom) : PropPtr{}; };
  CHECK(resolved.restore_loose(record));
  // a character carrying it: its damage record comes back with it
  Scene s(Path::Shallow);
  auto& c = s.civilian();
  REQUIRE(c.swap(custom, AttachPoint::RightHand));
  for (const auto& d : damage_scenario(c, "thighShot")) c.damage(d);
  const auto saved = c.damage_record();
  auto& again = s.civilian();
  again.attachments().registry->define(custom);
  CHECK(again.restore_damage(saved));
  REQUIRE(again.attachments().held());
  CHECK(again.attachments().held()->archetype == custom);
}

TEST_CASE("audit: memory reports what is kept; loose props and wounds stay bounded") {
  World world;
  CharacterSystem system;
  system.attach(world);
  const i64 empty = system.memory_bytes();
  for (int n = 0; n < 200; n++) system.props->create(prop_archetype("sword"));
  CHECK(system.memory_bytes() > empty + 200 * i64(sizeof(PropInstance)));
  // loose ones beyond the registry's bound go, the longest loose first
  PropRegistry reg;
  reg.options.max_loose = 50;
  std::vector<PropInstancePtr> made;
  for (int n = 0; n < 120; n++) {
    auto p = reg.create(prop_archetype("knife"));
    reg.release(p);
    made.push_back(p);
  }
  reg.update(1.0 / 60);
  CHECK(reg.all().size() == 50);
  CHECK(made.front()->location == PropLocation::Gone);
  CHECK(made.back()->location == PropLocation::Loose);
  // below the kill plane: gone
  auto fall = reg.create(prop_archetype("knife"));
  fall->pos = V3{0, 0, -1000};
  reg.release(fall);
  reg.update(1.0 / 60);
  reg.update(1.0 / 60);
  CHECK(fall->location == PropLocation::Gone);
  // wounds: many small deposits join, and the report counts them
  Scene s(Path::Shallow);
  auto& c = s.civilian();
  const i64 before = c.memory_bytes();
  WoundMechanics tiny;
  tiny.tissue.push_back({H::shinL, c.model->skeleton->rest_head[H::shinL], 1e-6, 0, false});
  DamageDescriptor blunt;
  blunt.kind = DamageKind::Blunt;
  for (int n = 0; n < 10000; n++) c.behaviours.damage.apply(*c.model, c.pose, blunt, tiny);
  CHECK(c.behaviours.damage.inspect().wounds.size() <= size_t(c.profile.max_wounds));
  CHECK(c.memory_bytes() > before);
  CHECK(c.behaviours.damage.record().size() < 20000);
}

TEST_CASE("audit: blood on a cell leaves what it is made of") {
  const auto human = make_civilian(3);
  VoxelPart part;
  part.bone = H::chest;
  part.origin = {-2, -2, 20};
  part.dims = {5, 5, 5};
  part.cells.resize(125, Slot::Metal + 1);
  part.count = part.initial_count = 125;
  VoxelModel model(human.model->skeleton, 1.0 / 32, {part});
  std::array<f32, 23 * 16> skin{};
  for (int i = 0; i < 23; i++) write_rigid(skin.data() + i * 16, {}, Quat{}, {});
  DamageDescriptor shot;
  shot.kind = DamageKind::Projectile;
  shot.mass = .008;
  shot.speed = 900;
  shot.diameter = .009;
  shot.point = model.cell_centre(0, -4, 22);
  shot.direction = {0, 1, 0};
  const auto r = wound_mechanics(model, skin, shot);
  REQUIRE_FALSE(r.removed.empty());
  const auto& p = model.parts[0];
  bool stained = false;
  for (size_t n = 0; n < p.cells.size(); ++n)
    if (p.cells[n] && !p.stain.empty() && p.stain[n]) {
      stained = true;
      CHECK(p.cells[n] == Slot::Metal + 1);
      CHECK(model.tissue_at(p, n) == Tissue::Metal);
      CHECK(tissue_resistance(model.tissue_at(p, n)) == 2e6);
      CHECK(p.shown_cell(n) == Slot::Blood + 1);
    }
  CHECK(stained);
}

TEST_CASE("audit: deaths in the parallel character phase reach the world one at a time, the same on any thread count") {
  const int threads_before = num_threads();
  auto run = [&](int threads) {
    set_num_threads(threads);
    World world;
    VoxelGrid g;
    g.h = kH;
    for (i32 x = -96; x < 96; ++x)
      for (i32 y = -96; y < 96; ++y) g.fill_column(x, y, -4, 1, make_vox(MaterialId::Rock, true));
    g.compact();
    g.lo = {-96, -96, -4};
    g.hi = {96, 96, 64};
    world.load(std::move(g));
    CharacterSystemConfig cfg;
    cfg.policy = BodyPolicy::Deep;
    auto system = std::make_shared<CharacterSystem>(cfg);
    world.add_system(system);
    system->focus = {V3{1000, 1000, 0}};  // (far: calm bodies rest on their plans)
    std::vector<CharacterId> ids;
    for (int i = 0; i < 8; ++i) {
      CharacterDesc d;
      const auto human = make_civilian(i + 1);
      d.model = human.model;
      d.palette = human.palette;
      d.seed = i + 1;
      d.pos = V3{-6.0 + 1.6 * i, 0, .0625};
      ids.push_back(system->spawn(d));
    }
    for (int t = 0; t < 90; ++t) world.tick();
    for (CharacterId id : ids) {
      Character* c = system->get(id);
      REQUIRE(c);
      auto cap = c->capabilities();
      cap.fatal = true;
      c->behaviours.damage.override_capabilities(cap);
    }
    for (int t = 0; t < 120; ++t) world.tick();
    i32 dead = 0, bound = 0;
    for (CharacterId id : ids)
      if (const Character* c = system->get(id)) {
        dead += c->alive() ? 0 : 1;
        bound += c->bound() ? 1 : 0;
      }
    CHECK(dead == 8);
    CHECK(bound == 8);
    const u64 h = system->state_hash();
    set_num_threads(threads_before);
    return h;
  };
  const u64 one = run(1), many = run(4);
  CHECK(one == many);
}

TEST_CASE("audit: a strike is gated by the arm that throws it") {
  Scene s(Path::Shallow);
  auto& c = s.civilian();
  for (int i = 0; i < 10; ++i) s.frame({&c});
  const ActionDef* jab = action_def("jab");
  REQUIRE(jab);
  CHECK(jab->strike_arm() == 0);  // (the lead hand: left)
  auto weak_right = c.capabilities();
  weak_right.arms[1].strength = .05;
  c.behaviours.damage.override_capabilities(weak_right);
  s.frame({&c});
  CHECK(c.motion.play("jab"));
  for (int i = 0; i < 60; ++i) s.frame({&c});
  auto weak_left = c.capabilities();
  weak_left.arms[0].strength = .05;
  weak_left.arms[1].strength = 1;
  c.behaviours.damage.override_capabilities(weak_left);
  s.frame({&c});
  const i32 hand = c.motion.weapon_hand;
  CHECK_FALSE(c.motion.play("jab"));
  CHECK(c.motion.action_refusal == "striking arm is too weak");
  CHECK(c.motion.weapon_hand == hand);  // (a refused action changes nothing)
}

TEST_CASE("audit: the weight of a held prop does not build up while the body rests on its plan") {
  Scene s(Path::Shallow);
  auto& c = s.add(make_soldier(4), 4.0, kPi / 2, V3{0, 0, s.ground}, prop_archetype("rifle"));
  c.physics = false;
  for (int i = 0; i < 240; ++i) s.frame({&c});
  REQUIRE_FALSE(c.behaviours.physical);
  for (const auto* b : c.body.parts) CHECK(norm(b->torque) == 0.0);
}

TEST_CASE("audit: girth widens the body's collision") {
  const auto human = make_civilian(3);
  CharacterOptions o;
  o.model = human.model;
  FlatGround ground(0);
  o.collision = &ground;
  Character slim(o);
  o.girth = 1.3;
  Character broad(o);
  CHECK(broad.body.parts[B::chest]->spheres[0].r > slim.body.parts[B::chest]->spheres[0].r);
}

TEST_CASE("audit: a head that comes off is death") {
  Scene s(Path::Shallow);
  auto& c = s.civilian();
  for (int i = 0; i < 10; ++i) s.frame({&c});
  for (const auto& d : damage_scenario(c, "thighShot")) c.damage(d);
  REQUIRE(c.alive());
  REQUIRE(c.owns_model);
  auto& head = c.model->parts[size_t(c.model->part_of_bone[H::head])];
  for (size_t n = 0; n < head.cells.size(); ++n) head.clear_cell(n);
  head.count = 0;
  for (const auto& d : damage_scenario(c, "gutStab")) c.damage(d);
  CHECK(c.behaviours.lost[B::head]);
  for (int i = 0; i < 5; ++i) s.frame({&c});
  CHECK_FALSE(c.alive());
}

TEST_CASE("audit: a prop absorbs what deforming it takes of a blunt blow; the rest passes") {
  const auto phone = prop_archetype("phone");
  REQUIRE(phone);
  auto run = [&](f64 fracture) {
    auto model = phone->model->clone();
    std::array<f32, 16> m{};
    write_rigid(m.data(), {}, Quat{}, {});
    V3 lo, hi;
    part_bounds(model->parts[0], model->voxel_size, &lo, &hi);
    DamageDescriptor d;
    d.kind = DamageKind::Blunt;
    d.mass = 1;
    d.speed = 30;  // (450 J: a bat)
    d.area = .003;
    d.direction = {0, 0, -1};
    d.point = (lo + hi) * .5 + V3{0, 0, (hi.z - lo.z) * .5};
    PropMaterial mat = phone->material;
    mat.fracture = fracture;
    return wound_mechanics(*model, std::span<const f32>(m.data(), 16), d, &mat).remaining_energy;
  };
  CHECK(run(phone->material.fracture) > 250);
  CHECK(run(kInf) == 0);
}

TEST_CASE("audit: the legacy profile is the zone damage model, and profiles read and write by name") {
  CharacterProfile p = legacy_profile();
  f64 v = 0;
  REQUIRE(profile_get(p, "arms_at_ease", &v));
  CHECK(v == .62);
  REQUIRE(profile_get(p, "damage", &v));
  CHECK(v == f64(DamageModel::Zones));
  CHECK(profile_set(p, "arms_at_ease", .7));
  CHECK(p.arms_at_ease == .7);
  CHECK_FALSE(profile_set(p, "arms_at_ease", 99));
  CHECK_FALSE(profile_set(p, "no such knob", 1));
  CHECK_FALSE(profile_set(p, "solver_clamps", .5));
  // a head shot kills on the zones' hit points; a blast at the body's centre tears it apart
  Scene s(Path::Shallow);
  CharacterOptions o;
  const auto human = make_soldier(4);
  o.profile = legacy_profile();
  o.model = human.model;
  o.collision = s.col.get();
  Character c(o);
  c.place({}, kPi / 2);
  for (int i = 0; i < 20; ++i) c.update(DT);
  const V3 head = c.pose.p[H::head];
  const auto hit = c.raycast(head + V3{0, 3, .05}, V3{0, -1, 0}, 10);
  REQUIRE(hit);
  const auto w = c.wound(*hit, V3{0, -1, 0}, 35.0);
  CHECK(w.killed);
  Character d(o);
  d.place({}, kPi / 2);
  for (int i = 0; i < 20; ++i) d.update(DT);
  CHECK(d.blast(d.bounds_center(), 1, 1).gibbed);
}

TEST_CASE("audit: models and props are data - written, read back the same, and checked") {
  const auto human = make_civilian(3);
  HumanoidBuild build;
  build.height = 1.05;
  const std::string text = model_json(*human.model, &build);
  HumanoidBuild back_build;
  std::string error;
  const ModelPtr back = read_model(text, &error, &back_build);
  REQUIRE_MESSAGE(back, error);
  CHECK(back_build.height == 1.05);
  REQUIRE(back->parts.size() == human.model->parts.size());
  for (size_t i = 0; i < back->parts.size(); ++i) {
    CHECK(back->parts[i].cells == human.model->parts[i].cells);
    CHECK(back->parts[i].shade == human.model->parts[i].shade);
    CHECK(back->parts[i].origin == human.model->parts[i].origin);
  }
  CHECK(model_json(*back, &back_build) == text);
  // every prop of the catalogue as its data says
  REQUIRE(prop_catalog().size() == 18);
  for (const PropPtr& p : prop_catalog()) {
    const std::string pj = prop_json(*p);
    const PropPtr q = read_prop(pj, &error);
    REQUIRE_MESSAGE(q, p->id << ": " << error);
    CHECK(prop_json(*q) == pj);
  }
  CHECK(legacy_prop(1)->id == "rifle");
  CHECK(legacy_prop(5)->id == "knife");
  // a host's prop, from its data: held, saved and restored as any
  std::string json = prop_json(*prop_archetype("bat"));
  const std::string from = "\"id\":\"bat\"";
  REQUIRE(json.find(from) != std::string::npos);
  json.replace(json.find(from), from.size(), "\"id\":\"club_of_the_host\"");
  const PropPtr own = read_prop(json, &error);
  REQUIRE_MESSAGE(own, error);
  Scene s(Path::Shallow);
  auto& c = s.civilian();
  c.attachments().registry->define(own);
  CHECK(c.swap(c.attachments().registry->archetype("club_of_the_host"), AttachPoint::RightHand));
  // malformed: refused, with a reason
  CHECK_FALSE(read_prop("{\"format\": \"svx-anim-prop\"}", &error));
  CHECK_FALSE(error.empty());
  CHECK_FALSE(read_model("not json", &error));
  CHECK_FALSE(read_model("{\"format\": \"svx-anim-voxel-model\", \"version\": 2, \"rig\": \"humanoid\", \"voxel_size\": 0.03, \"parts\": [{\"bone\": 99, "
                         "\"origin\": [0,0,0], \"dims\": [1,1,1], \"cells\": [1,1]}]}",
                         &error));
}

TEST_CASE("audit: a volley is its pellets, spread about its direction by its seed - the same for the same seed") {
  auto shoot = [](i32 pellets, u32 seed) {
    Scene s(Path::Shallow);
    auto& c = s.civilian();
    s.frame({&c});
    DamageDescriptor d;
    d.kind = DamageKind::Projectile;
    d.construction = ProjectileConstruction::Buckshot;
    d.mass = .0035;
    d.speed = 400;
    d.diameter = .008;
    d.direction = V3{0, -1, 0};
    d.point = c.body.parts[B::chest]->x + V3{0, .6, 0};
    d.pellets = pellets;
    d.spread = .06;
    d.seed = seed;
    const WoundResult r = c.damage(d);
    return std::tuple{r.removed.size(), norm(r.impulse), c.damage_hash()};
  };
  const auto [one, one_impulse, one_hash] = shoot(1, 5);
  const auto [nine, nine_impulse, nine_hash] = shoot(9, 5);
  CHECK(nine > one);
  CHECK(nine_impulse > 3 * one_impulse);
  CHECK(std::get<2>(shoot(9, 5)) == nine_hash);
  CHECK(std::get<2>(shoot(9, 6)) != nine_hash);
  DamageDescriptor bad;
  bad.pellets = 65;
  CHECK_FALSE(bad.valid());
  bad.pellets = 2;
  bad.spread = .6;
  CHECK_FALSE(bad.valid());
}

TEST_CASE("audit: a host's fixtures are the library's - a strike pad, muscles from limbs, a chair, a held force") {
  // a sweep through a still box: the first contact, on its near face
  StrikeSweep sweep;
  sweep.descriptor.kind = DamageKind::Blunt;
  sweep.descriptor.duration = 1.0 / 60;
  sweep.previous_a = {-.05, 0, 1};
  sweep.previous_b = {.05, 0, 1};
  sweep.a = {-.05, .2, 1};
  sweep.b = {.05, .2, 1};
  const auto contact = StrikeTracker::box_contact(sweep, {-.3, .1, .8}, {.3, .15, 1.2});
  REQUIRE(contact);
  CHECK(contact->point.y == doctest::Approx(.1));
  CHECK(contact->speed == doctest::Approx(12));
  CHECK_FALSE(StrikeTracker::box_contact(sweep, {-.3, .3, .8}, {.3, .4, 1.2}));
  // muscles from what each limb can do
  Capabilities caps;
  caps.legs[0].support = .2;
  caps.arms[1].strength = .4;
  caps.neck = .5;
  caps.trunk = .9;
  derive_muscles(caps);
  CHECK(caps.muscle[B::shinL] == .2);
  CHECK(caps.muscle[B::footR] == 1);
  CHECK(caps.muscle[B::handR] == .4);
  CHECK(caps.muscle[B::head] == .5);
  CHECK(caps.muscle[B::spine] == .9);
  // a chair behind the heels, a desk only for the desk variant
  const SeatInfo seat = chair_behind({1, 2, 0}, kPi / 2, 1, SitVariant::Upright);
  CHECK(seat.pos.y == doctest::Approx(1.82));
  CHECK(seat.pos.z == doctest::Approx(.48));
  CHECK(seat.backrest);
  CHECK_FALSE(seat.desk_height);
  CHECK(chair_behind({}, 0, 1, SitVariant::Desk).desk_height);
  // a held force pushes the body (against its twin left alone) until it is let go
  auto pushed = [](f64 newtons) {
    Scene s(Path::Shallow);
    auto& c = s.civilian();
    for (int i = 0; i < 30; ++i) s.frame({&c});
    c.hold_force(B::chest, {newtons, 0, 0});
    for (int i = 0; i < 30; ++i) s.frame({&c});
    const f64 x = c.body.com().x;
    c.hold_force(B::chest, {});
    for (int i = 0; i < 120; ++i) s.frame({&c});
    CHECK(std::isfinite(norm(c.body.com())));
    return x;
  };
  MESSAGE("held 300 N: " << pushed(300) - pushed(0) << " m");
  CHECK(pushed(300) - pushed(0) > .01);
}

TEST_CASE("audit: what a character does by itself standing still is its profile's") {
  // (the idle postures and fidgets it starts on its own, over half a minute)
  auto idles = [](const CharacterProfile& profile) {
    const auto look = make_civilian(3);
    FlatGround ground(0);
    CharacterOptions o;
    o.model = look.model;
    o.collision = &ground;
    o.seed = 7;
    o.profile = profile;
    Character c(o);
    c.place(V3{}, kPi / 2);
    c.motion.input.idle = true;
    std::set<std::string> seen;
    for (int i = 0; i < 1800; ++i) {
      c.set_root(V3{}, kPi / 2);
      c.update(1.0 / 60);
      if (const auto pose = c.motion.pose_action_name(); !pose.empty()) seen.insert(std::string(pose));
      if (const auto act = c.motion.action_name(); !act.empty()) seen.insert(std::string(act));
    }
    return seen;
  };
  CHECK_FALSE(idles(CharacterProfile{}).empty());
  CharacterProfile still;
  still.idle.pose_after = still.idle.fidget_after = still.idle.armed_after = 1e6;
  CHECK(idles(still).empty());
  CharacterProfile p;
  f64 v = 0;
  REQUIRE(profile_set(p, "idle_fidget_every", 20));
  REQUIRE(profile_get(p, "idle_fidget_every", &v));
  CHECK(v == 20);
  CHECK(legacy_profile().idle.pose_every == CharacterProfile{}.idle.pose_every);
}
