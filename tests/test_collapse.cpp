// Structural destruction end to end (docs/V2_DESIGN.md): standing at rest, collapse after
// supports go, pieces breaking on impact, determinism.

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/engine/engine.hpp"
#include "svx/world/procgen.hpp"

using namespace svx;

namespace {

Engine world(const char* kind) {
  ProcWorld w = make_procedural(kind, 1);
  Engine eng;
  eng.load(std::move(w.grid), w.spawn_pos, w.spawn_dir);
  eng.bake();
  return eng;
}

void run(Engine& eng, int ticks) {
  for (int t = 0; t < ticks; ++t) {
    eng.tick();
    (void)eng.take_events();
  }
}

}  // namespace

TEST_CASE("collapse: designed structures stand at rest") {
  for (const char* kind : {"tower", "rooms", "bridge", "slab"}) {
    Engine eng = world(kind);
    const i64 v0 = eng.grid().solid_count();
    run(eng, 90);
    const EngineStats s = eng.stats();
    CAPTURE(kind);
    CHECK(s.bonds_broken == 0);
    CHECK(s.detached_voxels == 0);
    CHECK(eng.grid().solid_count() == v0);
  }
}

TEST_CASE("collapse: the design pass leaves nothing floating (a second pass removes nothing)") {
  for (const char* kind : {"tower", "rooms", "bridge", "chimney"}) {
    Engine eng = world(kind);
    const i64 v0 = eng.grid().solid_count();
    eng.bake();
    CAPTURE(kind);
    CHECK(eng.grid().solid_count() == v0);
  }
}

TEST_CASE("collapse: a slab whose columns are cut falls and breaks where it lands") {
  Engine eng = world("slab");
  const f64 h = 0.125;
  for (int cx : {25, 70})
    for (int cy : {25, 70}) eng.carve({h * cx, h * cy, h * 60}, 0.45);
  run(eng, 60 * 4);
  const EngineStats s = eng.stats();
  MESSAGE("slab: " << s.detached_voxels << " voxels detached, " << s.bodies << " pieces, " << s.body_splits << " splits");
  CHECK(s.detached_voxels >= 48 * 48 * 2);  // the whole slab came down
  CHECK(s.body_splits >= 1);                // and something broke on the way
  CHECK(s.bodies >= 5);
  // (nearly) everything is on the ground now: at most bits of the slab left on the column stumps
  i64 low = 0, all = 0;
  for (const auto& b : eng.rigid().bodies) {
    all += b->shape.count;
    low += b->x.z < 4.0 ? b->shape.count : 0;
  }
  CHECK(low >= 0.9 * all);
}

TEST_CASE("collapse: a bridge losing a pier drops its deck") {
  Engine eng = world("bridge");
  const f64 h = 0.125;
  eng.blast({h * 144, h * 32, 1.0}, 1.2, 1e6);
  run(eng, 20);
  eng.blast({h * 152, h * 32, 3.0}, 1.2, 1e6);
  run(eng, 60 * 4);
  const EngineStats s = eng.stats();
  MESSAGE("bridge: " << s.detached_voxels << " voxels detached, " << s.bonds_broken << " bonds broken, " << s.bodies << " pieces");
  CHECK(s.bonds_broken > 0);
  CHECK(s.detached_voxels > 4000);
}

TEST_CASE("collapse: the tower losing its two west rows of columns comes down (and replays bit for bit on any thread count)") {
  const f64 h = 0.125;
  auto session = [&](int threads, EngineStats* st) {
    set_num_threads(threads);
    Engine eng = world("tower");
    for (int ix = 0; ix <= 1; ++ix)
      for (int iy = 0; iy <= 3; ++iy) {
        eng.blast({h * (41 + 26 * ix), h * (40 + iy * 26 + 1), 1.0}, 0.9, 1e6);
        run(eng, 15);
      }
    run(eng, 60 * 3);
    *st = eng.stats();
    return eng.session_hash();
  };
  EngineStats s1, s4;
  const u64 h1 = session(1, &s1);
  const u64 h4 = session(4, &s4);
  set_num_threads(1);
  MESSAGE("tower: " << s1.detached_voxels << " voxels detached, " << s1.bonds_broken << " bonds broken, " << s1.bodies
                    << " pieces, " << s1.body_splits << " splits");
  CHECK(s1.detached_voxels > 20000);  // far more than the blasts removed: the structure failed
  CHECK(s1.bonds_broken > 100);
  CHECK(h1 == h4);
}
