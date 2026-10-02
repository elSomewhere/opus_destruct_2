// structvox game — pedestrians (game.hpp: PedestrianConfig; docs/ANIM.md): the drive city's people.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>

#include "doctest.h"
#include "svx/anim/system.hpp"
#include "svx/anim/damage/scenarios.hpp"
#include "svx/game/game.hpp"
#include "svx/procgen/drive_city.hpp"

using namespace svx;

namespace {

constexpr f64 h = 0.125;

void city(Game& game, u64 seed, f64 load = 90.0, i32 people = 16, bool traffic = true) {
  std::shared_ptr<GameSource> src = make_drive_city(seed);
  StreamConfig sc;
  sc.load_radius = load;
  sc.evict_radius = load + 30.0;
  sc.chunks_per_tick = 400;
  game.load_streaming(src, h, sc);
  TrafficConfig tc;
  tc.enabled = traffic;
  game.set_traffic(tc);
  PedestrianConfig pc;
  pc.count = people;
  game.set_pedestrians(pc);
  game.set_viewer(src->spawn_pos());
}

// The distance from p to the nearest walkway's line (its corners' line, or its walking line).
f64 off_walkways(const RoadNetwork& r, const V3& p) {
  std::vector<Walk> walks;
  r.walks_in(p - V3{60, 60, 0}, p + V3{60, 60, 0}, walks);
  f64 best = 1e9;
  for (const Walk& w : walks) {
    for (const V3& off : {V3{}, w.inset}) {
      const V3 a = w.a + off, b = w.b + off;
      const V3 d{b.x - a.x, b.y - a.y, 0.0};
      const f64 l2 = d.x * d.x + d.y * d.y;
      const f64 t = l2 > 0.0 ? std::clamp(((p.x - a.x) * d.x + (p.y - a.y) * d.y) / l2, 0.0, 1.0) : 0.0;
      best = std::min(best, std::hypot(a.x + d.x * t - p.x, a.y + d.y * t - p.y));
    }
  }
  return best;
}

// Whether p is on a carriageway: within a lane's width of a lane's centre line.
bool on_road(const RoadNetwork& r, const V3& p) {
  std::vector<Lane> lanes;
  r.lanes_in(p - V3{8, 8, 0}, p + V3{8, 8, 0}, lanes);
  for (const Lane& l : lanes) {
    const V3 d{l.b.x - l.a.x, l.b.y - l.a.y, 0.0};
    const f64 l2 = d.x * d.x + d.y * d.y;
    const f64 t = l2 > 0.0 ? ((p.x - l.a.x) * d.x + (p.y - l.a.y) * d.y) / l2 : 0.0;
    if (t < 0.0 || t > 1.0) continue;
    if (std::hypot(l.a.x + d.x * t - p.x, l.a.y + d.y * t - p.y) < 0.5 * l.width) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("pedestrians: people walk the drive city's sidewalks about the viewer, wait for the lights and cross on the zebras") {
  Game game;
  city(game, 11);
  const RoadNetwork* roads = make_drive_city(11)->roads();
  REQUIRE(roads);
  std::shared_ptr<GameSource> keep = make_drive_city(11);
  const RoadNetwork& r = *keep->roads();
  i32 most = 0, dead = 0, crossed = 0;
  f64 worst_off = 0.0, zlo = 1e9, zhi = -1e9;
  std::set<u32> seen, on_a_road;
  for (int t = 0; t < 60 * 40; ++t) {
    game.tick();
    if (t % 30 != 29) continue;
    const std::vector<CharacterView> views = game.character_views();
    most = std::max(most, static_cast<i32>(views.size()));
    dead = 0;
    for (const CharacterView& v : views) {
      seen.insert(v.id);
      if (!(v.flags & CharacterView::kAlive)) {
        ++dead;
        continue;
      }
      // (the feet: under the bounds' centre)
      const V3 feet{v.centre.x, v.centre.y, 0.0};
      if (!(v.flags & CharacterView::kDown)) worst_off = std::max(worst_off, off_walkways(r, feet));
      zlo = std::min(zlo, v.centre.z);
      zhi = std::max(zhi, v.centre.z);
      if (on_road(r, feet) && on_a_road.insert(v.id).second) ++crossed;
    }
  }
  const anim::CharacterSystem* cs = game.characters();
  REQUIRE(cs);
  const anim::CharacterStats st = cs->stats();
  MESSAGE(most << " people at most (" << seen.size() << " in all), " << dead << " dead; " << crossed << " crossed a road; the farthest from a walkway " << worst_off
                << " m; centres " << zlo << " .. " << zhi << " m; now " << st.deep << " deep, " << st.shallow << " shallow, " << st.plan_only << " on their plans");
  CHECK(most == 16);
  CHECK(dead <= 1);
  CHECK(crossed >= 2);
  CHECK(worst_off < 1.5);  // (on its walking line, or stepping aside for someone)
  CHECK(zlo > 0.5);
  CHECK(zhi < 1.4);
}

TEST_CASE("pedestrians: the same commands, the same people doing the same (replays, lockstep)") {
  u64 hashes[2] = {0, 0};
  std::vector<V3> where[2];
  for (int run = 0; run < 2; ++run) {
    Game game;
    city(game, 5, 80.0, 12);
    for (int t = 0; t < 60 * 12; ++t) game.tick();
    hashes[run] = game.session_hash();
    for (const CharacterView& v : game.character_views()) where[run].push_back(v.centre);
  }
  REQUIRE(where[0].size() == where[1].size());
  REQUIRE(!where[0].empty());
  bool same = true;
  for (size_t k = 0; k < where[0].size(); ++k) same = same && where[0][k].x == where[1][k].x && where[0][k].y == where[1][k].y && where[0][k].z == where[1][k].z;
  CHECK(same);
  CHECK(hashes[0] == hashes[1]);
}

TEST_CASE("pedestrians: people come and go with the viewer; the characters' memory stays bounded") {
  Game game;
  city(game, 3, 80.0, 16, false);
  for (int t = 0; t < 60 * 10; ++t) game.tick();
  std::set<u32> first;
  for (const CharacterView& v : game.character_views()) first.insert(v.id);
  REQUIRE(first.size() == 16);
  const i64 mem0 = game.world().memory().systems;
  // the viewer goes 600 m away (as a car would take it), and on another 600 m
  V3 eye = make_drive_city(3)->spawn_pos();
  for (int leg = 0; leg < 2; ++leg) {
    eye.x += 600.0;
    game.set_viewer(eye);
    for (int t = 0; t < 60 * 12; ++t) game.tick();
  }
  std::set<u32> now;
  i32 near = 0;
  for (const CharacterView& v : game.character_views()) {
    now.insert(v.id);
    if (std::hypot(v.centre.x - eye.x, v.centre.y - eye.y) < 100.0) ++near;
  }
  i32 kept = 0;
  for (u32 id : now) kept += first.count(id) ? 1 : 0;
  const i64 mem1 = game.world().memory().systems;
  MESSAGE(now.size() << " people now, " << near << " about the viewer, " << kept << " of the first; the systems' memory " << mem0 / 1024 << " KB, then " << mem1 / 1024 << " KB");
  CHECK(kept == 0);
  CHECK(near == static_cast<i32>(now.size()));
  CHECK(near >= 12);
  CHECK(mem1 < mem0 * 3 + 256 * 1024);
}

TEST_CASE("pedestrians: the dead stay where they fell - the world keeps a body with its region and gives it back") {
  Game game;
  city(game, 7, 80.0, 8, false);
  const V3 spawn = make_drive_city(7)->spawn_pos();
  const V3 eye{spawn.x, spawn.y, spawn.z + 1.6};
  // A fatal brain channel: energy alone no longer guarantees death through an arbitrary limb.
  u32 victim = 0;
  for (int t = 0; t < 60 * 30 && !victim; ++t) {
    game.tick();
    if (t % 30 != 29) continue;
    f64 best = 60.0;
    for (const CharacterView& v : game.character_views()) {
      const f64 d = std::hypot(v.centre.x - eye.x, v.centre.y - eye.y);
      if (!(v.flags & CharacterView::kAlive) || d > best) continue;
      const V3 dir{v.centre.x - eye.x, v.centre.y - eye.y, v.centre.z + 0.2 - eye.z};
      const f64 l = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
      const Game::ShotHit h = game.raycast_shot(eye, V3{dir.x / l, dir.y / l, dir.z / l}, 80.0);
      if (h.character != v.id) continue;
      anim::Character* character = game.characters()->get(v.id);
      REQUIRE(character);
      anim::DamageDescriptor shot;
      shot.kind = anim::DamageKind::Projectile;
      shot.mass = .008;
      shot.speed = 600;
      shot.diameter = .009;
      const auto regions = anim::anatomy_regions(*character->model->skeleton);
      const auto brain = std::find_if(regions.begin(), regions.end(), [](const auto& v) { return v.kind == anim::VitalKind::Brain; });
      REQUIRE(brain != regions.end());
      shot.point = character->pose.point_of(brain->bone, brain->centre) - V3{0, .2, 0};
      shot.direction = {0, 1, 0};
      if (game.damage_character(v.id, shot)) {
        victim = v.id;
        best = d;
      }
    }
  }
  REQUIRE(victim != 0);
  // it falls and comes to rest
  V3 at;
  bool rests = false;
  for (int t = 0; t < 60 * 10 && !rests; ++t) {
    game.tick();
    for (const CharacterView& v : game.character_views())
      if (v.id == victim) {
        CHECK(!(v.flags & CharacterView::kAlive));
        rests = (v.flags & CharacterView::kAsleep) && (v.flags & CharacterView::kDeep);
        at = v.centre;
      }
  }
  REQUIRE(rests);
  // (and shot again where it lies: the holes it has go with it)
  for (int k = 0; k < 3; ++k) {
    const anim::Character* body = game.characters()->get(victim);
    REQUIRE(body);
    const V3 c = body->pose.p[anim::H::chest];
    const V3 from{c.x, c.y, c.z + 2.0};
    const Game::ShotHit h = game.raycast_shot(from, V3{0, 0, -1}, 5.0);
    if (h.character == victim) game.wound_character(victim, h.pos, 0.15, 50.0);
    game.tick();
  }
  REQUIRE(game.characters()->get(victim));
  const i32 voxels = game.characters()->get(victim)->model->voxel_count();
  i32 whole = 0;
  for (const anim::VoxelPart& p : game.characters()->get(victim)->model->parts) whole += p.initial_count;
  CHECK(game.characters()->get(victim)->owns_model);
  // the viewer goes far away: the body goes with its region
  game.set_viewer(V3{spawn.x + 600.0, spawn.y, spawn.z});
  for (int t = 0; t < 60 * 10; ++t) game.tick();
  i32 near_body = 0;
  for (const CharacterView& v : game.character_views())
    if (std::hypot(v.centre.x - at.x, v.centre.y - at.y) < 1.0) ++near_body;
  CHECK(near_body == 0);
  CHECK(game.world().stats().archived_articulations > 0);
  // and back: the body is there, dead, where it lay
  game.set_viewer(spawn);
  f64 off = 1e9;
  bool dead = false;
  u32 back = 0;
  for (int t = 0; t < 60 * 10; ++t) game.tick();
  for (const CharacterView& v : game.character_views()) {
    const f64 d = std::hypot(v.centre.x - at.x, v.centre.y - at.y);
    if (d < off) {
      off = d;
      dead = !(v.flags & CharacterView::kAlive);
      back = v.id;
    }
  }
  const std::string state = dead ? "dead" : "alive";
  MESSAGE("the body lay at " << at.x << ", " << at.y << "; back, the nearest character is " << off << " m from there, " << state);
  CHECK(off < 0.3);
  CHECK(dead);
  // with its wounds
  const anim::Character* again = game.characters()->get(back);
  REQUIRE(again);
  MESSAGE("voxels: " << voxels << " of " << whole << " when it went, " << again->model->voxel_count() << " back");
  CHECK(voxels < whole);
  CHECK(again->owns_model);
  CHECK(again->model->voxel_count() == voxels);
}

TEST_CASE("pedestrians: a car driven into someone knocks them down and hurts them - their body is the world's") {
  Game game;
  city(game, 7, 80.0, 6, false);
  const V3 spawn = make_drive_city(7)->spawn_pos();
  // someone walking near the viewer, and the way they go (two looks half a second apart)
  u32 who = 0;
  V3 p0, p1;
  for (int t = 0; t < 60 * 30 && !who; ++t) {
    game.tick();
    if (t % 30 != 29) continue;
    for (const CharacterView& v : game.character_views()) {
      if (!(v.flags & CharacterView::kAlive) || std::hypot(v.centre.x - spawn.x, v.centre.y - spawn.y) > 35.0) continue;
      p0 = v.centre;
      for (int k = 0; k < 30; ++k) game.tick();
      for (const CharacterView& w : game.character_views())
        if (w.id == v.id) p1 = w.centre;
      if (std::hypot(p1.x - p0.x, p1.y - p0.y) > 0.4) who = v.id;
      break;
    }
  }
  REQUIRE(who != 0);
  const f64 l = std::hypot(p1.x - p0.x, p1.y - p0.y);
  const V3 dir{(p1.x - p0.x) / l, (p1.y - p0.y) / l, 0.0};
  // a car 3.5 m behind them, coming their way at 11 m/s
  const V3 at{p1.x - dir.x * 3.5, p1.y - dir.y * 3.5, spawn.z + 0.3};
  const u32 car = game.spawn_vehicle({VehicleKind::Sedan, Paint::Red}, at, std::atan2(dir.y, dir.x));
  REQUIRE(car != 0);
  game.tick();
  VehicleView vv;
  REQUIRE(game.vehicle(car, &vv));
  REQUIRE(vv.chassis != 0);
  const Body* chassis = game.world().piece(vv.chassis);
  REQUIRE(chassis);
  REQUIRE(game.world().apply_impulse(vv.chassis, chassis->x, V3{dir.x * chassis->mass * 11.0, dir.y * chassis->mass * 11.0, 0.0}));
  bool deep = false, down = false, alive = true;
  f64 health = 1.0, moved = 0.0;
  for (int t = 0; t < 60 * 4; ++t) {
    game.tick();
    for (const CharacterView& v : game.character_views())
      if (v.id == who) {
        deep = deep || (v.flags & CharacterView::kDeep);
        down = down || (v.flags & CharacterView::kDown);
        alive = v.flags & CharacterView::kAlive;
        health = std::min(health, v.health);
        moved = std::max(moved, std::hypot(v.centre.x - p1.x, v.centre.y - p1.y));
      }
  }
  const std::string state = alive ? "alive" : "dead", body = deep ? "a body of the world" : "its own body", fell = down ? "down" : "on its feet";
  MESSAGE("hit by a car at 11 m/s: " << body << ", knocked " << moved << " m, " << fell << ", health " << health << ", " << state);
  CHECK(deep);
  CHECK(down);
  CHECK(health < 0.8);
  CHECK(moved > 1.5);
}

TEST_CASE("pedestrians: a rocket wounds and throws a coherent body, with blood and detached parts") {
  Game game;
  city(game, 7, 80.0, 6, false);
  const V3 spawn = make_drive_city(7)->spawn_pos();
  V3 at;
  u32 who = 0;
  for (int t = 0; t < 60 * 30 && !who; ++t) {
    game.tick();
    if (t % 30 != 29) continue;
    for (const CharacterView& v : game.character_views())
      if ((v.flags & CharacterView::kAlive) && std::hypot(v.centre.x - spawn.x, v.centre.y - spawn.y) < 40.0) {
        who = v.id;
        at = v.centre;
        break;
      }
  }
  REQUIRE(who != 0);
  (void)game.take_character_meshes();
  game.blast(V3{at.x + 0.6, at.y, at.z - 0.4}, 1.0, 1.0e6);  // (a rocket at their feet)
  i32 gibs = 0, gib_meshes = 0, drops = 0, stains = 0;
  bool body = false;
  std::set<u32> meshes_had;
  for (int t = 0; t < 60 * 3; ++t) {
    game.tick();
    for (const CharacterMeshData& m : game.take_character_meshes()) meshes_had.insert(m.id);
    std::vector<f32> d, s;
    game.blood(&d, &s);
    drops = std::max(drops, static_cast<i32>(d.size() / 7));
    stains = static_cast<i32>(s.size() / 8);
  }
  for (const CharacterView& v : game.character_views()) {
    if (v.flags & CharacterView::kGib) {
      ++gibs;
      if (meshes_had.count(v.mesh)) ++gib_meshes;
      CHECK(v.bones == 1);
    }
    body = body || v.id == who;
  }
  MESSAGE("a rocket at someone's feet: " << gibs << " gibs (" << gib_meshes << " with their meshes), " << drops << " blood drops at most, " << stains << " stains");
  // Ordinary blasts retain the trunk; detached parts and blood are separate render instances.
  CHECK(body);
  REQUIRE(game.characters()->get(who));
  CHECK(game.characters()->get(who)->model->voxel_count() > 0);
  CHECK(game.characters()->get(who)->health < game.characters()->get(who)->max_health);
  CHECK(gib_meshes == gibs);
  CHECK(drops > 20);
  CHECK(stains > 10);
}

#include "svx/game/replay.hpp"
TEST_CASE("pedestrians: loadouts, runtime attachments and physical damage replay with their render meshes") {
  auto run = [](CommandLog* record, const CommandLog* replay) {
    Game game;
    city(game, 7, 80, 4, false);
    if (record) game.record_to(record);
    std::vector<u64> hashes;
    u32 id = 0;
    for (int t = 0; t < 240; ++t) {
      if (replay) {
        for (const auto& command : replay->commands())
          if (command.tick == game.ticks()) apply_command(game, command);
      } else {
        if (t == 0) game.set_loadout_shares(1, 1);
        if (t == 60) {
          const auto ids = game.characters()->ids();
          REQUIRE_FALSE(ids.empty());
          id = ids.front();
          REQUIRE(game.attach_prop(id, "sword", anim::AttachPoint::RightHand));
          CHECK_FALSE(game.attach_prop(id, "unknown-prop", anim::AttachPoint::Back));
        }
        if (t == 100) {
          auto* c = game.characters()->get(id);
          REQUIRE(c);
          for (const auto& d : anim::damage_scenario(*c, "thighShot")) REQUIRE(game.damage_character(id, d));
        }
        if (t == 120) REQUIRE(game.detach_prop(id, anim::AttachPoint::RightHand));
      }
      game.tick();
      if (t % 30 == 29) hashes.push_back(game.session_hash());
    }
    size_t attached = 0, loose = 0;
    for (const auto& [id, p] : game.characters()->props->all()) {
      attached += p->location == anim::PropLocation::Attached;
      loose += p->location == anim::PropLocation::Loose;
    }
    CHECK(attached > 0);
    CHECK(loose > 0);
    size_t meshes = 0;
    for (const auto& view : game.character_views()) meshes += (view.prop_mesh != 0) || ((view.flags & 64) != 0 && view.mesh != 0);
    CHECK(meshes > 0);
    const auto snapshot = game.save_delta(), items = game.characters()->props->record_loose();
    Game restored;
    city(restored, 7, 80, 0, false);
    REQUIRE(restored.load_delta(snapshot));
    REQUIRE(restored.characters());
    CHECK(restored.characters()->props->record_loose() == items);
    auto truncated = snapshot;
    truncated.pop_back();
    Game untouched;
    city(untouched, 7, 80, 0, false);
    const auto before = untouched.world().state_hash();
    CHECK_FALSE(untouched.load_delta(truncated));
    CHECK(untouched.world().state_hash() == before);
    return hashes;
  };
  CommandLog log;
  const auto first = run(&log, nullptr);
  CommandLog decoded;
  REQUIRE(CommandLog::parse(log.serialize(), &decoded));
  CHECK(first == run(nullptr, &decoded));
  for (auto type : {Command::Type::AttachProp, Command::Type::DetachProp, Command::Type::Damage, Command::Type::Loadouts})
    CHECK(std::any_of(log.commands().begin(), log.commands().end(), [&](const auto& command) { return command.type == type; }));
}
