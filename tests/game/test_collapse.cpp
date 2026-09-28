// Structural destruction end to end (docs/V2_DESIGN.md): standing at rest, collapse after
// supports go, pieces breaking on impact, determinism.

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/game/game.hpp"
#include "svx/game/procgen.hpp"
#include "svx/game/city.hpp"

using namespace svx;

namespace {

Game world(const char* kind) {
  ProcWorld w = make_procedural(kind, 1);
  Game eng;
  eng.load(std::move(w.grid), w.spawn_pos, w.spawn_dir);
  add_grids(eng.world(), std::move(w.grids));
  eng.bake();
  return eng;
}

void run(Game& eng, int ticks) {
  for (int t = 0; t < ticks; ++t) {
    eng.tick();
    (void)eng.take_events();
  }
}

}  // namespace

TEST_CASE("collapse: designed structures stand at rest") {
  for (const char* kind : {"tower", "rooms", "bridge", "slab"}) {
    Game eng = world(kind);
    const i64 v0 = eng.grid().solid_count();
    run(eng, 90);
    const GameStats s = eng.stats();
    CAPTURE(kind);
    CHECK(s.bonds_broken == 0);
    CHECK(s.detached_voxels == 0);
    CHECK(eng.grid().solid_count() == v0);
  }
}

TEST_CASE("collapse: the design pass leaves nothing floating (a second pass removes nothing)") {
  for (const char* kind : {"tower", "rooms", "bridge", "chimney"}) {
    Game eng = world(kind);
    const i64 v0 = eng.grid().solid_count();
    eng.bake();
    CAPTURE(kind);
    CHECK(eng.grid().solid_count() == v0);
  }
}

TEST_CASE("collapse: a slab whose columns are cut falls and breaks where it lands") {
  Game eng = world("slab");
  const f64 h = 0.125;
  for (int cx : {25, 70})
    for (int cy : {25, 70}) eng.carve({h * cx, h * cy, h * 60}, 0.45);
  run(eng, 60 * 4);
  const GameStats s = eng.stats();
  MESSAGE("slab: " << s.detached_voxels << " voxels detached, " << s.bodies << " pieces, " << s.body_splits << " splits");
  CHECK(s.detached_voxels >= 48 * 48 * 2);  // the whole slab came down
  CHECK(s.body_splits >= 1);                // and something broke on the way
  CHECK(s.bodies >= 5);
  // (nearly) everything is on the ground now: at most bits of the slab left on the column stumps
  i64 low = 0, all = 0;
  for (const auto& b : eng.world().rigid().bodies) {
    all += b->count;
    low += b->x.z < 4.0 ? b->count : 0;
  }
  CHECK(low >= 0.9 * all);
}

TEST_CASE("collapse: a bridge losing a pier drops its deck") {
  Game eng = world("bridge");
  const f64 h = 0.125;
  eng.blast({h * 144, h * 32, 1.0}, 1.2, 1e6);
  run(eng, 20);
  eng.blast({h * 152, h * 32, 3.0}, 1.2, 1e6);
  run(eng, 60 * 4);
  const GameStats s = eng.stats();
  MESSAGE("bridge: " << s.detached_voxels << " voxels detached, " << s.bonds_broken << " bonds broken, " << s.bodies << " pieces");
  CHECK(s.bonds_broken > 0);
  CHECK(s.detached_voxels > 4000);
}

TEST_CASE("collapse: the tower losing its two west rows of columns comes down (and replays bit for bit on any thread count)") {
  const f64 h = 0.125;
  auto session = [&](int threads, GameStats* st) {
    set_num_threads(threads);
    Game eng = world("tower");
    for (int ix = 0; ix <= 1; ++ix)
      for (int iy = 0; iy <= 3; ++iy) {
        eng.blast({h * (41 + 26 * ix), h * (40 + iy * 26 + 1), 1.0}, 0.9, 1e6);
        run(eng, 15);
      }
    run(eng, 60 * 3);
    *st = eng.stats();
    return eng.session_hash();
  };
  GameStats s1, s4;
  const u64 h1 = session(1, &s1);
  const u64 h4 = session(4, &s4);
  set_num_threads(1);
  MESSAGE("tower: " << s1.detached_voxels << " voxels detached, " << s1.bonds_broken << " bonds broken, " << s1.bodies
                    << " pieces, " << s1.body_splits << " splits");
  CHECK(s1.detached_voxels > 20000);  // far more than the blasts removed: the structure failed
  CHECK(s1.bonds_broken > 100);
  CHECK(h1 == h4);
}

TEST_CASE("collapse: a streamed city's buildings stand when touched (designed on first touch), and fall when their ground floor goes") {
  const f64 h = 0.125;
  auto city = [&](Game& eng) {
    auto src = make_city_source(1, 1000.0, h);
    const auto sp = src->spawn_pos();
    VoxelGrid g;
    g.h = h;
    eng.load(std::move(g), sp, src->spawn_dir());
    StreamConfig sc;
    eng.load_streaming(std::move(src), eng.grid().h, sc);
    eng.set_viewer(sp);
    return sp;
  };
  {
    // tiny carves on the first-floor slabs of the 3 x 3 blocks around the spawn, while the city is
    // still streaming in around them: nothing breaks
    Game eng;
    const auto sp = city(eng);
    const f64 ox = sp[0] + h * 48.0, oy = sp[1] - h * 72.0;  // the central block's corner
    for (int t = 0; t < 18; ++t) run(eng, 1);
    for (int i = -1; i <= 1; ++i)
      for (int j = -1; j <= 1; ++j) eng.carve({ox + 24.0 * i + 9.0625, oy + 24.0 * j + 9.0625, 3.1}, 0.06);
    run(eng, 60 * 3);
    const GameStats s = eng.stats();
    MESSAGE("city pokes: " << s.bonds_broken << " bonds broken, " << s.strengthened_voxels << " voxels strengthened");
    CHECK(s.bonds_broken == 0);
    CHECK(s.detached_voxels < 100);
  }
  {
    // the central building's ground floor blown out: it comes down, its neighbours stand
    Game eng;
    const auto sp = city(eng);
    const f64 ox = sp[0] + h * 48.0, oy = sp[1] - h * 72.0;
    run(eng, 18);
    for (int i = 0; i <= 6; ++i)
      for (int j = 0; j <= 6; ++j) eng.blast({ox + 0.2 + 3.0 * i, oy + 0.2 + 3.0 * j, 1.0}, 1.0, 1e6);
    run(eng, 60 * 4);
    const GameStats s = eng.stats();
    MESSAGE("city demolition: " << s.detached_voxels << " voxels detached, " << s.bodies << " pieces");
    CHECK(s.detached_voxels > 100000);
    CHECK(s.detached_voxels < 450000);  // (one building, ~300k voxels: its neighbours stand)
  }
}
