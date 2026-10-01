// Doors and lifts (plan Phase 7): movers open / lower, wait and return deterministically, never
// bond to the structure, reverse when a closing door meets the player, and Doom maps get their
// manual doors and lifts.
#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>

#include "doctest.h"
#include "svx/game/doom/movers.hpp"
#include "svx/game/game.hpp"

using namespace svx;

namespace {

constexpr f64 kH = 0.125;

// Ground, a wall across x = 20..22 with a 1 m doorway (y 0..8, z 0..16) holding a door mover.
Game door_world(i32* door) {
  VoxelGrid g;
  g.h = kH;
  const Vox rock = make_vox(MaterialId::Rock, true), rc = make_vox(MaterialId::Rc, false);
  for (i32 x = -10; x < 50; ++x)
    for (i32 y = -20; y < 28; ++y) g.fill_column(x, y, -4, 0, rock);
  for (i32 x = 20; x < 22; ++x)
    for (i32 y = -20; y < 28; ++y) g.fill_column(x, y, 0, 24, rc);
  for (i32 x = 20; x < 22; ++x)
    for (i32 y = 0; y < 8; ++y) g.fill_column(x, y, 0, 16, kAir);  // the doorway
  g.lo = {-10, -20, -4};
  g.hi = {50, 28, 24};
  g.compact();
  Game eng;
  eng.load(std::move(g), {0, 0, 0}, {1, 0, 0});
  eng.bake();
  MoverDef d;
  d.kind = MoverDef::Kind::Door;
  for (i32 x = 20; x < 22; ++x)
    for (i32 y = 0; y < 8; ++y) d.cols.push_back({x, y});
  d.z0 = 0;
  d.z1 = 16;
  d.vox = make_vox(MaterialId::Wood, true);
  d.speed = 2.0;
  d.wait = 1.0;
  *door = eng.add_mover(d);
  return eng;
}

bool door_closed(const Game& e) {
  for (i32 z = 0; z < 16; ++z)
    if (!vox_solid(e.grid().get(21, 4, z))) return false;
  return true;
}

bool door_open(const Game& e) {
  for (i32 z = 0; z < 16; ++z)
    if (vox_solid(e.grid().get(21, 4, z))) return false;
  return true;
}

}  // namespace

TEST_CASE("movers: a door opens on use, waits, closes, and is never bonded to the wall") {
  i32 id = -1;
  Game eng = door_world(&id);
  CHECK(door_closed(eng));
  CHECK(eng.mover_at({21, 4, 3}) == id);
  // the leaf carries nothing: no bonds to the wall beside it or the lintel above
  CHECK_FALSE(eng.grid().bond({20, 7, 5}, 1));   // leaf -> wall (+y)
  CHECK_FALSE(eng.grid().bond({21, 3, 15}, 2));  // leaf -> lintel (+z)
  // use from 1.5 m in front, aiming at the door
  eng.set_viewer({kH * 10, kH * 4, 1.6});
  CHECK(eng.use({kH * 10, kH * 4, 1.2}, {1, 0, 0}));
  for (int t = 0; t < 40; ++t) eng.tick();  // 0.67 s at 2 spans / s: fully open
  CHECK(door_open(eng));
  CHECK(eng.mover_position(id) == doctest::Approx(1.0));
  for (int t = 0; t < 60 + 40; ++t) eng.tick();  // wait 1 s, close 0.5 s
  CHECK(door_closed(eng));
  // the wall is untouched: its voxels and bonds are intact
  CHECK(eng.grid().bond({20, 10, 5}, 1));
  CHECK(eng.stats().bonds_broken == 0);
}

TEST_CASE("movers: a closing door goes back up when the player stands in the doorway") {
  i32 id = -1;
  Game eng = door_world(&id);
  eng.set_viewer({kH * 10, kH * 4, 1.6});
  CHECK(eng.use({kH * 10, kH * 4, 1.2}, {1, 0, 0}));
  for (int t = 0; t < 45; ++t) eng.tick();
  CHECK(door_open(eng));
  eng.set_viewer({kH * 21, kH * 4, 1.6});  // walking through
  for (int t = 0; t < 120; ++t) eng.tick();
  CHECK_FALSE(door_closed(eng));  // it never closed on the player
  eng.set_viewer({kH * 40, kH * 4, 1.6});  // through
  for (int t = 0; t < 240; ++t) eng.tick();
  CHECK(door_closed(eng));
}

TEST_CASE("movers: a shot-up door stays as it is") {
  i32 id = -1;
  Game eng = door_world(&id);
  eng.carve({kH * 21, kH * 4, kH * 6}, 0.2);
  eng.tick();
  CHECK_FALSE(vox_solid(eng.grid().get(21, 4, 6)));
  CHECK_FALSE(eng.use({kH * 10, kH * 4, kH * 14}, {1, 0, 0}));  // disabled
}

TEST_CASE("movers: Freedoom MAP01 gets its doors; using one opens it") {
  std::ifstream f(std::string(SVX_SOURCE_DIR) + "/data/freedoom/freedoom2.wad", std::ios::binary);
  if (!f) {
    MESSAGE("freedoom2.wad not found: skipped");
    return;
  }
  std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  doom::Wad wad;
  std::string err;
  REQUIRE(wad.load_memory(std::move(bytes), &err));
  doom::DoomWorld w;
  REQUIRE(doom::build_doom_world(wad, "MAP01", {}, kH, false, &w, &err));
  int kinds[4] = {0, 0, 0, 0};
  for (const auto& m : w.movers) ++kinds[static_cast<int>(m.kind)];
  MESSAGE("MAP01: " << kinds[0] << " doors, " << kinds[1] << " lifts, " << kinds[2] << " floors, " << kinds[3]
                    << " ceilings");
  CHECK(kinds[0] > 0);
  REQUIRE(w.movers[0].kind == doom::MoverInfo::Kind::Door);
  CHECK(w.movers[0].manual);
  Game eng;
  VoxelGrid g = std::move(w.grid);
  w.grid = VoxelGrid{};
  eng.load(std::move(g), w.spawn_pos, w.spawn_dir);
  w.live = &eng.grid();
  CHECK(doom::attach_doom_movers(eng, w) == static_cast<int>(w.movers.size()));
  // find a door column next to an air column of another sector; use it from there
  const doom::MoverInfo& m0 = w.movers[0];
  bool used = false;
  i32 mid = (m0.z0 + m0.z1) / 2;
  for (i32 j = 1; j + 1 < w.ny && !used; ++j)
    for (i32 i = 1; i + 1 < w.nx && !used; ++i) {
      if (w.sector[size_t(j) * w.nx + i] != m0.sector) continue;
      const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
      for (int d = 0; d < 4 && !used; ++d) {
        const i32 ni = i + 6 * di[d], nj = j + 6 * dj[d];
        if (ni < 0 || nj < 0 || ni >= w.nx || nj >= w.ny) continue;
        if (vox_solid(eng.grid().get(ni, nj, m0.z0 + 4))) continue;
        const V3 eye{kH * ni, kH * nj, kH * mid};
        eng.set_viewer(eye);
        used = eng.use(eye, {-f64(di[d]), -f64(dj[d]), 0.0});
      }
    }
  REQUIRE(used);
  for (int t = 0; t < 180; ++t) eng.tick();
  CHECK(eng.mover_position(0) > 0.99);
}

TEST_CASE("movers: walk-over triggers fire when the viewer's path crosses them, once if W1") {
  i32 id = -1;
  Game eng = door_world(&id);
  int calls = 0;
  bool fired = false;
  // a W1 line across the corridor at x = 1.5 m, from y = -1 .. 2 m
  eng.walk_resolver = [&](const V3& a, const V3& b) {
    std::vector<MoverTrigger> out;
    ++calls;
    const bool cross = (a[0] - 1.5) * (b[0] - 1.5) < 0.0 && std::min(a[1], b[1]) < 2.0 && std::max(a[1], b[1]) > -1.0;
    if (cross && !fired) {
      fired = true;
      out.push_back({id, 0});
    }
    return out;
  };
  eng.set_viewer({0.5, 0.5, 1.6});  // the first update only records the position
  eng.set_viewer({1.0, 0.5, 1.6});
  for (int t = 0; t < 10; ++t) eng.tick();
  CHECK(eng.mover_position(id) == doctest::Approx(0.0));  // not crossed yet
  eng.set_viewer({2.0, 0.5, 1.6});                         // crosses x = 1.5
  for (int t = 0; t < 45; ++t) eng.tick();
  CHECK(eng.mover_position(id) == doctest::Approx(1.0));
  CHECK(calls == 2);
}

namespace {

// Ground (z < 0) over a 24 x 24 area, a slab from z = 16 over x, y in [0, 8) and `d` added.
Game plane_world(MoverDef d, i32* id) {
  VoxelGrid g;
  g.h = kH;
  const Vox rock = make_vox(MaterialId::Rock, true);
  for (i32 x = -8; x < 16; ++x)
    for (i32 y = -8; y < 16; ++y) g.fill_column(x, y, -4, 0, rock);
  for (i32 x = 0; x < 8; ++x)
    for (i32 y = 0; y < 8; ++y) g.fill_column(x, y, 16, 18, rock);  // the room's ceiling
  g.lo = {-8, -8, -4};
  g.hi = {16, 16, 18};
  g.compact();
  Game eng;
  eng.load(std::move(g), {0, 0, 0}, {1, 0, 0});
  eng.bake();
  for (i32 x = 0; x < 8; ++x)
    for (i32 y = 0; y < 8; ++y) d.cols.push_back({x, y});
  d.vox = make_vox(MaterialId::Concrete, true);
  *id = eng.add_mover(d);
  return eng;
}

}  // namespace

TEST_CASE("movers: a rising floor carries the player and waits while the player would not fit") {
  MoverDef d;
  d.kind = MoverDef::Kind::Floor;
  d.z0 = 0;
  d.z1 = 8;
  d.rows = 0;
  d.usable = false;
  MoverMove up;
  up.type = MoverMove::Type::To;
  up.target = 8;
  up.speed = 16.0;
  d.moves.push_back(up);
  i32 id = -1;
  Game eng = plane_world(d, &id);
  auto stand = [&]() {  // the client keeps the player on the floor's top
    eng.set_viewer({kH * 4, kH * 4, kH * (eng.mover_rows(id) - 0.5) + 1.6});
  };
  stand();
  CHECK(eng.activate_mover(id));
  for (int t = 0; t < 60; ++t) {
    eng.tick();
    stand();
  }
  // standing on it the player needs 1.9 m under the ceiling at z = 16: the floor stops at 1 row
  CHECK(eng.mover_rows(id) == 1);
  CHECK(eng.mover_busy(id));
  eng.set_viewer({kH * 40, kH * 40, 1.6});  // steps off
  for (int t = 0; t < 60; ++t) eng.tick();
  CHECK(eng.mover_rows(id) == 8);
  CHECK_FALSE(eng.mover_busy(id));
  for (i32 z = 0; z < 8; ++z) CHECK(vox_solid(eng.grid().get(3, 3, z)));
  CHECK_FALSE(eng.activate_mover(id));  // already there
}

TEST_CASE("movers: a crusher cycles, waits over the player, stops and resumes") {
  MoverDef d;
  d.kind = MoverDef::Kind::Ceiling;
  d.z0 = 2;
  d.z1 = 16;
  d.rows = 0;
  d.usable = false;
  MoverMove crush;
  crush.type = MoverMove::Type::Cycle;
  crush.target = 14;
  crush.speed = 28.0;  // levels per second: down in 0.5 s
  d.moves.push_back(crush);
  MoverMove stop;
  stop.type = MoverMove::Type::Stop;
  d.moves.push_back(stop);
  i32 id = -1;
  Game eng = plane_world(d, &id);
  eng.set_viewer({kH * 4, kH * 4, 1.6});  // under it
  CHECK(eng.activate_mover(id, 0));
  for (int t = 0; t < 60; ++t) eng.tick();
  // it never reaches into the player's box (eye + 0.2 m = 1.8 m; level 14 starts at 1.69 m)
  CHECK(eng.mover_rows(id) == 1);
  CHECK_FALSE(vox_solid(eng.grid().get(4, 4, 14)));
  eng.set_viewer({kH * 40, kH * 40, 1.6});
  int lowest = 99, highest = -1, bottom_hits = 0;
  for (int t = 0; t < 180; ++t) {
    eng.tick();
    lowest = std::min(lowest, eng.mover_rows(id));
    highest = std::max(highest, eng.mover_rows(id));
    if (eng.mover_rows(id) == 14) ++bottom_hits;
  }
  CHECK(lowest == 0);   // back up to the top ...
  CHECK(highest == 14);  // ... and down to the bottom (z = 2), repeatedly
  CHECK(bottom_hits >= 2);
  CHECK(eng.activate_mover(id, 1));  // stop
  const i32 held = eng.mover_rows(id);
  for (int t = 0; t < 60; ++t) eng.tick();
  CHECK(eng.mover_rows(id) == held);
  CHECK_FALSE(eng.mover_busy(id));
  CHECK_FALSE(eng.activate_mover(id, 1));  // already stopped
  CHECK(eng.activate_mover(id, 0));        // resumed
  CHECK(eng.mover_busy(id));
  for (int t = 0; t < 40; ++t) eng.tick();
  CHECK(eng.mover_rows(id) != held);
  CHECK(eng.stats().bonds_broken == 0);
}

TEST_CASE("movers: a close-only door waits for the player in the doorway") {
  MoverDef d;
  d.kind = MoverDef::Kind::Door;
  d.z0 = 0;
  d.z1 = 16;
  d.rows = 0;  // open
  MoverMove close;
  close.type = MoverMove::Type::To;
  close.target = 16;
  close.speed = 32.0;
  d.moves.push_back(close);
  i32 id = -1;
  Game eng = plane_world(d, &id);
  eng.set_viewer({kH * 4, kH * 4, 1.6});
  CHECK(eng.activate_mover(id));
  for (int t = 0; t < 60; ++t) eng.tick();
  CHECK(eng.mover_rows(id) < 3);  // held above the player's head (a door that only closes)
  CHECK(eng.mover_busy(id));
  eng.set_viewer({kH * 40, kH * 40, 1.6});
  for (int t = 0; t < 60; ++t) eng.tick();
  CHECK(eng.mover_rows(id) == 16);
  CHECK_FALSE(eng.mover_busy(id));
}

namespace {

bool load_doom_map(const std::string& name, doom::DoomWorld* w, Game* eng) {
  std::ifstream f(std::string(SVX_SOURCE_DIR) + "/data/freedoom/freedoom2.wad", std::ios::binary);
  if (!f) return false;
  std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  doom::Wad wad;
  std::string err;
  if (!wad.load_memory(std::move(bytes), &err)) return false;
  REQUIRE(doom::build_doom_world(wad, name, {}, kH, false, w, &err));
  VoxelGrid g = std::move(w->grid);
  w->grid = VoxelGrid{};
  eng->load(std::move(g), w->spawn_pos, w->spawn_dir);
  w->live = &eng->grid();
  eng->set_viewer({-1e3, -1e3, -1e3});
  REQUIRE(doom::attach_doom_movers(*eng, *w) == static_cast<int>(w->movers.size()));  // ids = indices
  return true;
}

// Runs move `special` on every mover that has it; returns the (mover, move) pairs started.
std::vector<std::pair<i32, i32>> run_special(Game& eng, u16 special, const doom::DoomWorld& w) {
  std::vector<std::pair<i32, i32>> out;
  for (i32 id = 0; id < eng.mover_count(); ++id) {
    const auto& moves = w.movers[static_cast<size_t>(id)].moves;
    for (size_t k = 0; k < moves.size(); ++k)
      if (moves[k].special == special && eng.activate_mover(id, static_cast<i32>(k)))
        out.emplace_back(id, static_cast<i32>(k));
  }
  return out;
}

}  // namespace

TEST_CASE("movers: Freedoom MAP20's staircase rises a step per sector") {
  doom::DoomWorld w;
  Game eng;
  if (!load_doom_map("MAP20", &w, &eng)) {
    MESSAGE("freedoom2.wad not found: skipped");
    return;
  }
  u16 special = 0;
  for (const auto& m : w.movers)
    for (const auto& pm : m.moves)
      if (pm.special == 7 || pm.special == 8 || pm.special == 100 || pm.special == 127) special = pm.special;
  REQUIRE(special != 0);
  const auto started = run_special(eng, special, w);
  MESSAGE("MAP20 stairs (special " << special << "): " << started.size() << " steps");
  REQUIRE(started.size() >= 3);
  for (int t = 0; t < 60 * 30; ++t) {
    bool busy = false;
    for (const auto& [id, k] : started) busy = busy || eng.mover_busy(id);
    if (!busy) break;
    eng.tick();
  }
  const i32 step = special == 100 || special == 127 ? 4 : 2;  // 16 / 8 map units in levels
  std::vector<i32> tops;
  for (const auto& [id, k] : started) {
    const doom::MoverInfo& mi = w.movers[static_cast<size_t>(id)];
    CHECK_FALSE(eng.mover_busy(id));
    CHECK(eng.mover_rows(id) == mi.moves[static_cast<size_t>(k)].target);
    tops.push_back(mi.z0 + eng.mover_rows(id));
    // the step's top voxel is solid in every one of its columns
    const i32 top = mi.z0 + eng.mover_rows(id) - 1;
    for (i32 j = 0; j < w.ny; j += 3)
      for (i32 i = 0; i < w.nx; i += 3)
        if (w.sector[size_t(j) * w.nx + i] == mi.sector) CHECK(vox_solid(eng.grid().get(i, j, top)));
  }
  // (one line may start several staircases: every height once per staircase)
  std::sort(tops.begin(), tops.end());
  tops.erase(std::unique(tops.begin(), tops.end()), tops.end());
  CHECK(tops.size() >= 3);
  for (size_t q = 1; q < tops.size(); ++q) CHECK(tops[q] - tops[q - 1] == step);
}

TEST_CASE("movers: Freedoom MAP04's crushers cycle; MAP05's donut; MAP08's gun line") {
  {
    doom::DoomWorld w;
    Game eng;
    if (!load_doom_map("MAP04", &w, &eng)) {
      MESSAGE("freedoom2.wad not found: skipped");
      return;
    }
    int crushers = 0;
    for (i32 id = 0; id < eng.mover_count(); ++id) {
      const auto& moves = eng.mover_def(id)->moves;
      for (size_t k = 0; k < moves.size(); ++k)
        if (moves[k].type == MoverMove::Type::Cycle && eng.mover_def(id)->ceiling()) {
          CHECK(eng.activate_mover(id, static_cast<i32>(k)));
          ++crushers;
        }
    }
    REQUIRE(crushers > 0);
    std::vector<i32> lo(size_t(eng.mover_count()), 1 << 30), hi(size_t(eng.mover_count()), -1);
    for (int t = 0; t < 60 * 12; ++t) {
      eng.tick();
      for (i32 id = 0; id < eng.mover_count(); ++id) {
        lo[size_t(id)] = std::min(lo[size_t(id)], eng.mover_rows(id));
        hi[size_t(id)] = std::max(hi[size_t(id)], eng.mover_rows(id));
      }
    }
    int cycled = 0;
    for (i32 id = 0; id < eng.mover_count(); ++id)
      if (eng.mover_def(id)->ceiling() && eng.mover_busy(id) && hi[size_t(id)] > lo[size_t(id)] + 4) ++cycled;
    MESSAGE("MAP04: " << crushers << " crusher moves, " << cycled << " cycling");
    CHECK(cycled >= 1);
    CHECK(eng.stats().bonds_broken == 0);
  }
  {
    doom::DoomWorld w;
    Game eng;
    REQUIRE(load_doom_map("MAP05", &w, &eng));
    const auto started = run_special(eng, 9, w);
    MESSAGE("MAP05 donut: " << started.size() << " planes");
    CHECK(started.size() >= 2);  // holes and rings
    CHECK(started.size() % 2 == 0);
    for (int t = 0; t < 60 * 20; ++t) eng.tick();
    for (const auto& [id, k] : started) {
      CHECK_FALSE(eng.mover_busy(id));
      CHECK(eng.mover_rows(id) == w.movers[size_t(id)].moves[size_t(k)].target);
    }
  }
  {
    doom::DoomWorld w;
    Game eng;
    REQUIRE(load_doom_map("MAP08", &w, &eng));
    // the gun line: a hitscan impact on its wall runs its tagged movers
    i32 line = -1;
    for (size_t li = 0; li < w.map.linedefs.size() && line < 0; ++li) {
      const u16 s = w.map.linedefs[li].special;
      if ((s == 24 || s == 46 || s == 47) && w.map.linedefs[li].tag != 0) line = static_cast<i32>(li);
    }
    REQUIRE(line >= 0);
    const doom::Linedef& L = w.map.linedefs[size_t(line)];
    const auto& A = w.map.vertices[L.v1];
    const auto& B = w.map.vertices[L.v2];
    const i32 upv = w.vstats.units_per_voxel;
    const f64 mx = 0.5 * (A.x + B.x), my = 0.5 * (A.y + B.y);
    const V3 at{kH * ((mx - w.vstats.origin_x) / upv - 0.5), kH * ((my - w.vstats.origin_y) / upv - 0.5), 1.0};
    int busy_before = 0, busy_after = 0;
    for (i32 id = 0; id < eng.mover_count(); ++id) busy_before += eng.mover_busy(id) ? 1 : 0;
    eng.carve(at, 0.05);
    for (i32 id = 0; id < eng.mover_count(); ++id) busy_after += eng.mover_busy(id) ? 1 : 0;
    MESSAGE("MAP08 gun line " << line << " (special " << L.special << "): " << busy_after << " movers set going");
    CHECK(busy_before == 0);
    CHECK(busy_after >= 1);
  }
}
