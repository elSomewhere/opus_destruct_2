// Rigid debris (plan Phase 7): detached pieces fall, collide with the voxel world, rest and
// fade; heavy landings load the structure they hit.
#include <cmath>
#include <vector>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/engine/debris.hpp"
#include "svx/engine/engine.hpp"

using namespace svx;

namespace {

constexpr f64 kH = 0.125;

VoxelGrid ground_grid() {
  VoxelGrid g;
  g.h = kH;
  for (i32 x = -24; x < 24; ++x)
    for (i32 y = -24; y < 24; ++y) g.fill_column(x, y, -4, 0, make_vox(MaterialId::Rock, true));
  g.lo = {-24, -24, -4};
  g.hi = {24, 24, 32};
  return g;
}

void box_voxels(std::vector<IVec3>& out, IVec3 lo, IVec3 hi) {
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y)
      for (i32 z = lo[2]; z < hi[2]; ++z) out.push_back({x, y, z});
}

f64 energy(const DebrisBody& b) {
  const f64 ke = 0.5 * b.mass * (b.v[0] * b.v[0] + b.v[1] * b.v[1] + b.v[2] * b.v[2]);
  const f64 kr = 0.5 * (b.w[0] * b.L[0] + b.w[1] * b.L[1] + b.w[2] * b.L[2]);
  return ke + kr + b.mass * 9.81 * b.x[2];
}

}  // namespace

TEST_CASE("debris: a dropped block lands flat on the ground, sleeps, fades and is removed") {
  const VoxelGrid g = ground_grid();
  std::vector<IVec3> vox;
  box_voxels(vox, {0, 0, 16}, {4, 4, 20});  // bottom face 1.9375 m above z = 0
  const std::vector<f64> m(vox.size(), 2400.0 * kH * kH * kH);
  DebrisSystem ds;
  DebrisParams p;
  REQUIRE(ds.add(7, vox, m, kH, {0, 0, 0}, {0, 0, 0}, p));
  const DebrisBody& b0 = ds.bodies()[0];
  CHECK(b0.mass == doctest::Approx(64 * m[0]));
  CHECK(b0.x[2] == doctest::Approx(kH * 17.5));
  std::vector<DebrisImpact> impacts;
  bool slept = false;
  f64 rest_z = 0.0, rest_qw = 0.0;
  std::vector<i64> done;
  for (int t = 0; t < 900 && done.empty(); ++t) {
    ds.step(1.0 / 60.0, g, p, &impacts);
    if (!ds.bodies().empty() && ds.bodies()[0].asleep && !slept) {
      slept = true;
      rest_z = ds.bodies()[0].x[2];
      rest_qw = std::abs(ds.bodies()[0].q[3]);
    }
    done = ds.take_finished();
  }
  REQUIRE(impacts.size() == 1);  // one landing, the bounce is below the reporting speed
  const f64 fall = kH * 15.5 + 0.5 * kH;  // bottom face to the ground top face (-h/2)
  CHECK(impacts[0].speed == doctest::Approx(std::sqrt(2 * 9.81 * fall)).epsilon(0.08));
  CHECK(impacts[0].impulse[2] < 0.0);  // on the world: downwards
  CHECK(impacts[0].pos[2] < 0.0);
  CHECK(slept);
  CHECK(std::abs(rest_z - (-0.5 * kH + 2.0 * kH)) < 0.02);  // resting on the ground top
  CHECK(rest_qw > 0.999);                                   // still flat
  REQUIRE(done.size() == 1);
  CHECK(done[0] == 7);
  CHECK(ds.bodies().empty());
}

TEST_CASE("debris: a tumbling beam never gains energy and comes to rest lying down") {
  const VoxelGrid g = ground_grid();
  std::vector<IVec3> vox;
  box_voxels(vox, {-6, -1, 8}, {6, 1, 10});
  const std::vector<f64> m(vox.size(), 2400.0 * kH * kH * kH);
  DebrisSystem ds;
  DebrisParams p;
  REQUIRE(ds.add(1, vox, m, kH, {1.0, 0.0, 0.5}, {0.5, 3.0, 1.0}, p));
  const f64 e0 = energy(ds.bodies()[0]);
  f64 emax = e0;
  bool slept = false;
  f64 rest_z = 1e9;
  for (int t = 0; t < 600 && !slept; ++t) {
    ds.step(1.0 / 60.0, g, p, nullptr);
    const DebrisBody& b = ds.bodies()[0];
    emax = std::max(emax, energy(b));
    if (b.asleep) {
      slept = true;
      rest_z = b.x[2];
    }
  }
  CHECK(emax <= e0 * 1.02 + 1.0);
  CHECK(slept);
  CHECK(rest_z < -0.5 * kH + 1.5 * kH);  // on a long face (centre h above the ground top)
  CHECK(rest_z > -0.5 * kH + 0.5 * kH);
}

TEST_CASE("debris: engine pieces become rigid debris; heavy landings load the structure") {
  // Two anchored rock walls carry a free RC slab (4 m span, 25 cm); above it an anchored rock
  // beam holds an 8^3 masonry block by a 2 x 2 RC hook. Cutting the hook drops the block 2 m.
  VoxelGrid g;
  g.h = kH;
  const Vox rock = make_vox(MaterialId::Rock, true);
  for (i32 y = 0; y < 16; ++y) {
    for (i32 x = -4; x < 0; ++x) g.fill_column(x, y, -8, 30, rock);
    for (i32 x = 32; x < 36; ++x) g.fill_column(x, y, -8, 30, rock);
    for (i32 x = 0; x < 32; ++x) g.fill_column(x, y, 0, 2, make_vox(MaterialId::Rc, false));
  }
  for (i32 x = 0; x < 32; ++x)
    for (i32 y = 4; y < 12; ++y) g.fill_column(x, y, 28, 30, rock);
  for (i32 x = 15; x < 17; ++x)
    for (i32 y = 7; y < 9; ++y) g.fill_column(x, y, 26, 28, make_vox(MaterialId::Rc, false));
  for (i32 x = 12; x < 20; ++x)
    for (i32 y = 4; y < 12; ++y) g.fill_column(x, y, 18, 26, make_vox(MaterialId::Masonry, false));
  g.lo = {-4, 0, -8};
  g.hi = {36, 16, 30};
  g.compact();
  auto run = [&](int threads, u64* hash, EngineStats* st) {
    set_num_threads(threads);
    Engine eng;
    EngineParams par;
    par.fragility = 0.25;
    eng.set_params(par);
    VoxelGrid copy = g;
    eng.load(std::move(copy), {kH * 2, kH * 2, 0.5}, {1, 0, 0});
    eng.bake();
    eng.carve({kH * 15.5, kH * 7.5, kH * 27.0}, 0.2);
    bool rigid = false;
    int max_bodies = 0;
    for (int t = 0; t < 360; ++t) {
      eng.tick();
      for (const EngineEvent& ev : eng.take_events())
        if (ev.kind == EngineEvent::Kind::Detached && ev.voxels >= 400) rigid = rigid || ev.rigid;
      max_bodies = std::max(max_bodies, static_cast<int>(eng.debris().size()));
    }
    CHECK(rigid);
    CHECK(max_bodies >= 1);
    *hash = eng.session_hash();
    *st = eng.stats();
  };
  u64 h1 = 0, h4 = 0;
  EngineStats s1, s4;
  run(1, &h1, &s1);
  run(4, &h4, &s4);
  set_num_threads(1);
  CHECK(s1.debris_landings >= 1);
  CHECK(s1.impact_loads >= 1);  // the block hit the free slab (not bedrock)
  CHECK(s1.static_settles + s1.bubbles_spawned >= 2);  // the carve, then the impact
  MESSAGE("landings " << s1.debris_landings << " impact loads " << s1.impact_loads << " ruptures " << s1.ruptures
                      << " bubbles " << s1.bubbles_spawned << " settles " << s1.static_settles << " detached "
                      << s1.detached_voxels);
  CHECK(h1 == h4);
}

namespace {

// A rock pillar carries an overloaded RC cantilever (2 x 1 voxels, 3 m; the design pass is off,
// so it stands only because the bake is elastic); a free RC column stands 2.45 m from its root.
VoxelGrid cantilever_scene() {
  VoxelGrid g;
  g.h = kH;
  const Vox rock = make_vox(MaterialId::Rock, true), rc = make_vox(MaterialId::Rc, false);
  for (i32 x = -40; x < 40; ++x)
    for (i32 y = -8; y < 16; ++y) g.fill_column(x, y, -4, 0, rock);
  for (i32 x = -8; x < 0; ++x)
    for (i32 y = 0; y < 8; ++y) g.fill_column(x, y, 0, 16, rock);
  for (i32 x = 0; x < 24; ++x)
    for (i32 y = 3; y < 5; ++y) g.fill_column(x, y, 12, 13, rc);
  for (i32 x = -20; x < -18; ++x)
    for (i32 y = 3; y < 5; ++y) g.fill_column(x, y, 0, 16, rc);
  g.lo = {-40, -8, -4};
  g.hi = {40, 16, 16};
  g.compact();
  return g;
}

}  // namespace

TEST_CASE("verification: the result is bitwise the same for any team size and with no thread") {
  auto run = [&](int team, bool async, EngineStats* st) {
    Engine eng;
    EngineConfig cfg = eng.config();
    cfg.structure_max_cells = 0;  // (a window bubble: the verification finds the far failure)
    cfg.design_utilization = 1e9;
    cfg.verify_threads = team;
    cfg.verify_async = async;
    eng.configure(cfg);
    EngineParams par;
    par.fragility = 0.25;
    eng.set_params(par);
    eng.load(cantilever_scene(), {kH * -30, kH * 4, 0.1}, {1, 0, 0});
    eng.bake();
    eng.blast({kH * -19, kH * 4, kH * 8}, 0.3, 1e6);
    for (int t = 0; t < 600; ++t) {
      eng.tick();
      (void)eng.take_events();
    }
    *st = eng.stats();
    return eng.session_hash();
  };
  EngineStats s1, s4, s0;
  const u64 h1 = run(1, true, &s1), h4 = run(4, true, &s4), h0 = run(3, false, &s0);
  CHECK(s1.verifications >= 1);
  CHECK(s1.verify_failures >= 1);
  CHECK(h1 == h4);
  CHECK(h1 == h0);
  CHECK(s1.ruptures == s4.ruptures);
}

TEST_CASE("grid: a snapshot is isolated from later edits (baselines copy-on-write)") {
  VoxelGrid g = cantilever_scene();
  const IVec3 p{5, 3, 12};
  const f32 b0[6] = {1, 2, 3, 4, 5, 6};
  g.set_baseline(p, b0);
  g.set_damage(p, 0, 0.25f);
  const IVec3 cc = chunk_of(p);
  const u64 keys[1] = {key3(cc[0], cc[1], cc[2])};
  const u32 v0 = g.chunk(cc)->version;
  const VoxelGrid s = g.snapshot(keys);
  CHECK(s.partial());
  CHECK(s.known(cc));
  CHECK_FALSE(s.known({cc[0] + 5, cc[1], cc[2]}));
  // the live grid changes: voxel, damage, baseline
  const f32 b1[6] = {9, 9, 9, 9, 9, 9};
  g.set_baseline(p, b1);
  g.set_damage(p, 0, 0.5f);
  g.set(p, kAir);
  CHECK(g.chunk(cc)->version != v0);
  f32 out[6];
  REQUIRE(s.baseline(p, out));
  CHECK(out[0] == 1.0f);
  CHECK(out[5] == 6.0f);
  CHECK(s.damage(p, 0) == 0.25f);
  CHECK(vox_solid(s.get(p)));
  CHECK(s.chunk(cc)->version == v0);
  REQUIRE(g.baseline(p, out));
  CHECK(out[0] == 9.0f);
  // a structure search stops (truncated) at the snapshot's edge
  bool truncated = false;
  const IVec3 seed[1] = {{2, 3, 12}};
  const auto set = structures_of(s, seed, 1 << 20, &truncated);
  CHECK(truncated);
  truncated = false;
  (void)structures_of(g, seed, 1 << 20, &truncated);
  CHECK_FALSE(truncated);
}

TEST_CASE("verification: a far member the bubble's coarse field cannot fail is caught and fails") {
  // A rocket hits the free column 2.45 m from the cantilever's root. In a window bubble the root
  // lies in the coarse (level >= 1) region, where the law is not evaluated: only the background
  // verification (full-fine static check of the settled window's structures) finds the failure
  // and continues it dynamically. A bubble spanning the structures (the default) nominates the
  // root, refines it and fails it itself.
  VoxelGrid g;
  g.h = kH;
  const Vox rock = make_vox(MaterialId::Rock, true), rc = make_vox(MaterialId::Rc, false);
  for (i32 x = -40; x < 40; ++x)
    for (i32 y = -8; y < 16; ++y) g.fill_column(x, y, -4, 0, rock);
  for (i32 x = -8; x < 0; ++x)
    for (i32 y = 0; y < 8; ++y) g.fill_column(x, y, 0, 16, rock);
  for (i32 x = 0; x < 24; ++x)
    for (i32 y = 3; y < 5; ++y) g.fill_column(x, y, 12, 13, rc);
  for (i32 x = -20; x < -18; ++x)
    for (i32 y = 3; y < 5; ++y) g.fill_column(x, y, 0, 16, rc);
  g.lo = {-40, -8, -4};
  g.hi = {40, 16, 16};
  g.compact();
  auto run = [&](bool verify, EngineStats* st, bool windows = true) {
    Engine eng;
    EngineConfig cfg = eng.config();
    cfg.verify = verify;
    if (windows) cfg.structure_max_cells = 0;
    cfg.design_utilization = 1e9;  // no strengthening: the cantilever is overloaded at rest
    eng.configure(cfg);
    EngineParams par;
    par.fragility = 0.25;
    eng.set_params(par);
    VoxelGrid copy = g;
    eng.load(std::move(copy), {kH * -30, kH * 4, 0.1}, {1, 0, 0});
    eng.bake();
    eng.blast({kH * -19, kH * 4, kH * 8}, 0.3, 1e6);
    for (int t = 0; t < 900; ++t) {
      eng.tick();
      (void)eng.take_events();
    }
    *st = eng.stats();
    return vox_solid(eng.grid().get(12, 4, 12));  // mid-span of the cantilever
  };
  EngineStats off, on;
  const bool stands_off = run(false, &off);
  const bool stands_on = run(true, &on);
  MESSAGE("verify off: ruptures " << off.ruptures << " detached " << off.detached_voxels << "; on: ruptures " << on.ruptures
                                  << " detached " << on.detached_voxels << " verifications " << on.verifications
                                  << " failures " << on.verify_failures << " (" << on.verify_ms << " ms)");
  CHECK(stands_off);            // without verification the far failure is missed
  CHECK(on.verifications >= 1);
  CHECK(on.verify_failures >= 1);
  CHECK_FALSE(stands_on);       // with it, the cantilever breaks off
  CHECK(on.ruptures > off.ruptures);
  EngineStats spans;
  const bool stands_spanning = run(false, &spans, false);
  MESSAGE("spanning bubble, verify off: ruptures " << spans.ruptures << " detached " << spans.detached_voxels);
  CHECK(spans.structure_bubbles >= 1);
  CHECK_FALSE(stands_spanning);  // the bubble spanning both fails the root itself
}

TEST_CASE("verification: a quiet window is verified while the world is busy elsewhere") {
  // The far-member scene, and a wall 10 m away that is shot every 0.4 s throughout: the
  // cantilever's window is quiet after the rocket, so its verification runs (and fails it)
  // although the world never is.
  VoxelGrid g;
  g.h = kH;
  const Vox rock = make_vox(MaterialId::Rock, true), rc = make_vox(MaterialId::Rc, false);
  for (i32 x = -40; x < 90; ++x)
    for (i32 y = -8; y < 16; ++y) g.fill_column(x, y, -4, 0, rock);
  for (i32 x = -8; x < 0; ++x)
    for (i32 y = 0; y < 8; ++y) g.fill_column(x, y, 0, 16, rock);
  for (i32 x = 0; x < 24; ++x)
    for (i32 y = 3; y < 5; ++y) g.fill_column(x, y, 12, 13, rc);
  for (i32 x = -20; x < -18; ++x)
    for (i32 y = 3; y < 5; ++y) g.fill_column(x, y, 0, 16, rc);
  for (i32 x = 70; x < 80; ++x)  // the busy wall
    for (i32 y = 3; y < 5; ++y) g.fill_column(x, y, 0, 16, rc);
  g.lo = {-40, -8, -4};
  g.hi = {90, 16, 16};
  g.compact();
  Engine eng;
  EngineConfig cfg = eng.config();
  cfg.structure_max_cells = 0;  // (a window bubble: the verification finds the far failure)
  cfg.design_utilization = 1e9;
  eng.configure(cfg);
  EngineParams par;
  par.fragility = 0.25;
  eng.set_params(par);
  eng.load(std::move(g), {kH * -30, kH * 4, 0.1}, {1, 0, 0});
  eng.bake();
  eng.blast({kH * -19, kH * 4, kH * 8}, 0.3, 1e6);
  int shots = 0;
  for (int t = 0; t < 900; ++t) {
    if (t % 24 == 12) {
      eng.carve({kH * (70 + (shots * 3) % 10), kH * 4, kH * (2 + (shots * 5) % 12)}, 0.15);
      ++shots;
    }
    eng.tick();
    (void)eng.take_events();
  }
  const EngineStats st = eng.stats();
  MESSAGE(shots << " shots elsewhere; verifications " << st.verifications << " failures " << st.verify_failures
                << " ruptures " << st.ruptures);
  CHECK(st.verifications >= 1);
  CHECK(st.verify_failures >= 1);
  CHECK_FALSE(vox_solid(eng.grid().get(12, 4, 12)));  // the cantilever broke off all the same
}
