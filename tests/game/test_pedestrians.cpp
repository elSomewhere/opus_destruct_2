// structvox game — pedestrians (game.hpp: PedestrianConfig; docs/ANIM.md): the drive city's people.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>

#include "doctest.h"
#include "svx/anim/system.hpp"
#include "svx/game/drive_city.hpp"
#include "svx/game/game.hpp"

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
  // the nearest person, shot (a round no one survives)
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
      if (game.wound_character(v.id, h.pos, 0.05, 1.0e5)) {
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
  for (int t = 0; t < 60 * 10; ++t) game.tick();
  for (const CharacterView& v : game.character_views()) {
    const f64 d = std::hypot(v.centre.x - at.x, v.centre.y - at.y);
    if (d < off) {
      off = d;
      dead = !(v.flags & CharacterView::kAlive);
    }
  }
  const std::string state = dead ? "dead" : "alive";
  MESSAGE("the body lay at " << at.x << ", " << at.y << "; back, the nearest character is " << off << " m from there, " << state);
  CHECK(off < 0.3);
  CHECK(dead);
}
