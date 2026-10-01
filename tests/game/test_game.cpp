// Game core (plan Phases 3-6): determinism, queries, persistence round trip.
#include <algorithm>
#include <cmath>
#include <vector>

#include <unordered_set>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/game/game.hpp"
#include "svx/procgen/city.hpp"
#include "svx/procgen/levels.hpp"

using namespace svx;

namespace {

// A scripted session on the procedural rooms world: bullets into the walls and a rocket.
void play(Game& eng, int ticks) {
  const auto sp = eng.spawn_pos();
  const V3 eye{sp[0], sp[1], sp[2] + 1.6};
  for (int t = 0; t < ticks; ++t) {
    if (t < 30 && t % 3 == 0) {
      const f64 ang = 0.2 * t / 3 - 0.9;
      const RayHit hit = eng.world().raycast(eye, {std::cos(ang), std::sin(ang), 0.05}, 60.0);
      if (hit.hit) eng.carve(hit.pos, 0.12);
    }
    if (t == 40) {
      const RayHit hit = eng.world().raycast(eye, {1.0, 0.1, 0.3}, 60.0);
      if (hit.hit) eng.blast(hit.pos, 0.5, 1e6);
    }
    eng.tick();
    eng.take_events();
  }
}

Game fresh_rooms() {
  Level w = make_procedural("rooms", 7);
  Game eng;
  eng.load(std::move(w.grid), w.spawn_pos, w.spawn_dir);
  eng.bake();
  return eng;
}

}  // namespace

TEST_CASE("game: queries — ray casts hit walls, AABB sweeps stop at them") {
  Game eng = fresh_rooms();
  const auto sp = eng.spawn_pos();
  const RayHit down = eng.world().raycast({sp[0], sp[1], sp[2] + 1.0}, {0, 0, -1}, 10.0);
  REQUIRE(down.hit);
  CHECK(down.normal[2] == doctest::Approx(1.0));
  CHECK(std::abs(down.pos[2] - (-0.0625)) < 0.02);  // floor top at the ground cells' upper face
  // standing box moving down stays on the floor; moving into a wall stops at it
  const CollideResult r = eng.world().collide({sp[0] - 0.3, sp[1] - 0.3, sp[2] + 0.01}, {sp[0] + 0.3, sp[1] + 0.3, sp[2] + 1.8}, {0, 0, -0.5});
  CHECK(r.on_ground);
  CHECK(r.move[2] == doctest::Approx(-(sp[2] + 0.01 + 0.0625)).epsilon(1e-2));  // lands on the floor top
  const CollideResult w = eng.world().collide({sp[0] - 0.3, sp[1] - 0.3, sp[2] + 0.1}, {sp[0] + 0.3, sp[1] + 0.3, sp[2] + 1.8}, {-20.0, 0, 0});
  CHECK(w.move[0] > -20.0);
  CHECK(w.move[0] < 0.0);
}

TEST_CASE("game: a scripted session is bitwise identical for 1 and N threads") {
  const int nt = num_threads();
  set_num_threads(1);
  Game a = fresh_rooms();
  play(a, 120);
  set_num_threads(8);
  Game b = fresh_rooms();
  play(b, 120);
  set_num_threads(nt);
  CHECK(a.stats().events >= 10);
  CHECK(a.session_hash() == b.session_hash());
  MESSAGE("engine session hash " << a.session_hash());
}

TEST_CASE("game: persistence round trip restores the exact world") {
  Game a = fresh_rooms();
  play(a, 150);
  REQUIRE(a.world().modified());
  const std::vector<u8> delta = a.save_delta();
  CHECK(delta.size() > 16);
  Game b = fresh_rooms();
  CHECK(b.world().state_hash() != a.world().state_hash());
  REQUIRE(b.load_delta(delta));
  CHECK(b.world().state_hash() == a.world().state_hash());
  // a corrupted delta is rejected and leaves the world unchanged
  Game c = fresh_rooms();
  const u64 h0 = c.world().state_hash();
  std::vector<u8> bad = delta;
  bad.resize(bad.size() / 2);
  CHECK(!c.load_delta(bad));
  CHECK(c.world().state_hash() == h0);
}

TEST_CASE("game: a streamed city stays bounded, and edits survive eviction exactly") {
  Game eng;
  GameParams p;
  eng.set_params(p);
  auto src = make_city_source(3, 1000.0, 0.125);
  const auto sp = src->spawn_pos();
  VoxelGrid g;
  eng.load(std::move(g), sp, src->spawn_dir());
  StreamConfig sc;
  sc.load_radius = 48.0;
  sc.evict_radius = 64.0;
  sc.chunks_per_tick = 64;
  eng.load_streaming(std::move(src), eng.grid().h, sc);
  eng.set_viewer(sp);
  for (int t = 0; t < 20; ++t) eng.tick();
  const i64 near = eng.world().stats().resident_chunks;
  CHECK(near > 50);
  // shoot a wall near the spawn, then walk 300 m away and back
  const RayHit hit = eng.world().raycast({sp[0], sp[1], sp[2] + 1.6}, {0.0, 0.6, -1.0}, 60.0);  // the street
  REQUIRE(hit.hit);
  eng.carve(hit.pos, 0.3);
  for (int t = 0; t < 10; ++t) eng.tick();
  CHECK(!vox_solid(eng.grid().get(hit.voxel)));
  const u64 local = eng.world().state_hash();
  const std::vector<u8> delta0 = eng.save_delta();
  i64 max_resident = 0;
  for (int s = 1; s <= 30; ++s) {
    eng.set_viewer({sp[0] + 10.0 * s, sp[1], sp[2]});
    for (int t = 0; t < 4; ++t) eng.tick();
    max_resident = std::max(max_resident, eng.world().stats().resident_chunks);
  }
  CHECK(eng.stats().archived_chunks >= 1);
  CHECK(!vox_solid(eng.grid().get(hit.voxel)) );  // evicted: absent, not regenerated solid
  CHECK(max_resident < 4 * near + 200);            // residency stays bounded while moving
  for (int s = 30; s >= 0; --s) {
    eng.set_viewer({sp[0] + 10.0 * s, sp[1], sp[2]});
    for (int t = 0; t < 4; ++t) eng.tick();
  }
  for (int t = 0; t < 20; ++t) eng.tick();
  CHECK(!vox_solid(eng.grid().get(hit.voxel)));    // the carve came back with the chunk
  // the saved delta (resident + archived) restores the same edits in a fresh city
  const std::vector<u8> delta1 = eng.save_delta();
  CHECK(delta1.size() >= delta0.size() / 2);
  Game fresh;
  fresh.set_params(p);
  VoxelGrid g2;
  auto src2 = make_city_source(3, 1000.0, 0.125);
  fresh.load(std::move(g2), sp, src2->spawn_dir());
  fresh.load_streaming(std::move(src2), fresh.grid().h, sc);
  REQUIRE(fresh.load_delta(delta1));
  fresh.set_viewer(sp);
  for (int t = 0; t < 30; ++t) fresh.tick();
  CHECK(!vox_solid(fresh.grid().get(hit.voxel)));
  (void)local;
}

TEST_CASE("game: the streamed city has a far render tier beyond the resident radius (plan Phase 6)") {
  Game eng;
  VoxelGrid g;
  g.h = 0.125;
  auto src = make_city_source(3, 1000.0, 0.125);
  const auto sp = src->spawn_pos(), sd = src->spawn_dir();
  eng.load(std::move(g), sp, sd);
  StreamConfig sc;
  eng.load_streaming(std::move(src), eng.grid().h, sc);
  eng.set_viewer({sp[0], sp[1], sp[2] + 1.6});
  std::vector<ChunkMesh> far;
  for (int t = 0; t < 200; ++t) {
    eng.tick();
    for (auto& m : eng.take_far_meshes()) far.push_back(std::move(m));
  }
  CHECK(far.size() >= 100);  // one tile per tick once the near field is resident
  const f64 tile_m = 0.125 * 32 * FarConfig{}.tile;
  size_t verts = 0;
  for (const ChunkMesh& m : far) {
    verts += m.vertices.size();
    // wholly beyond the eviction radius
    const f64 x0 = m.chunk[0] * tile_m, y0 = m.chunk[1] * tile_m;
    const f64 vx = sp[0] + 0.0625, vy = sp[1] + 0.0625;
    const f64 dx = vx < x0 ? x0 - vx : vx > x0 + tile_m ? vx - x0 - tile_m : 0.0;
    const f64 dy = vy < y0 ? y0 - vy : vy > y0 + tile_m ? vy - y0 - tile_m : 0.0;
    CHECK(std::sqrt(dx * dx + dy * dy) > sc.evict_radius);
  }
  MESSAGE(far.size() << " far tiles, " << verts << " vertices (" << verts * 28 / 1024 << " KB)");
  // flying 200 m on: tiles that came near are dropped
  eng.set_viewer({sp[0], sp[1] + 200.0, sp[2] + 20.0});
  eng.tick();
  CHECK_FALSE(eng.take_far_removed().empty());
}

TEST_CASE("game: a streamed world keeps to its byte budget, farthest chunks first (plan B8)") {
  auto fly = [](f64 budget_mb, i64* evicted_by_budget, f64* peak_mb, bool* near_resident) {
    Game eng;
    auto src = make_city_source(3, 1000.0, 0.125);
    const auto sp = src->spawn_pos();
    VoxelGrid g;
    eng.load(std::move(g), sp, src->spawn_dir());
    StreamConfig sc;
    sc.load_radius = 40.0;
    sc.evict_radius = 80.0;
    sc.chunks_per_tick = 64;
    sc.max_resident_mb = budget_mb;
    eng.load_streaming(std::move(src), eng.grid().h, sc);
    f64 peak = 0.0;
    for (int t = 0; t < 360; ++t) {
      eng.set_viewer({sp[0] + 0.25 * t, sp[1], sp[2]});
      eng.tick();
      if (t % 30 == 29) peak = std::max(peak, eng.stats().memory_mb);  // (right after a budget check)
    }
    *evicted_by_budget = eng.stats().budget_evicted;
    *peak_mb = peak;
    // the viewer's own neighbourhood is never given up for the budget: the street under it
    const f64 x = sp[0] + 0.25 * 359;
    *near_resident = vox_solid(eng.grid().get(static_cast<i32>(std::floor(x / 0.125 + 0.5)),
                                              static_cast<i32>(std::floor(sp[1] / 0.125 + 0.5)),
                                              static_cast<i32>(std::floor(sp[2] / 0.125 + 0.5)) - 1));
  };
  i64 ev0 = 0, ev1 = 0;
  f64 mb0 = 0.0, mb1 = 0.0;
  bool near0 = false, near1 = false;
  fly(0.0, &ev0, &mb0, &near0);
  const f64 budget = 0.7 * mb0;
  fly(budget, &ev1, &mb1, &near1);
  MESSAGE("without a budget: " << mb0 << " MB; with " << budget << " MB: " << mb1 << " MB, " << ev1 << " chunks evicted for it");
  CHECK(ev0 == 0);
  CHECK(ev1 > 0);
  CHECK(mb1 <= budget * 1.05);
  CHECK(near1);
}

TEST_CASE("game: a level loaded again keeps its meshes (chunks and water): no removal of what was just sent") {
  Game g;
  auto load = [&] {
    Level w = make_procedural("yard", 1);
    g.load(std::move(w.grid), w.spawn_pos, w.spawn_dir);
    g.bake();
  };
  load();
  for (int t = 0; t < 60; ++t) {
    g.tick();
    g.take_meshes({});
    g.take_removed_chunks();
    g.take_water_meshes();
    g.take_water_removed();
  }
  load();
  std::unordered_set<u64> sent, water_sent;
  for (int t = 0; t < 12; ++t) {
    g.tick();
    for (const ChunkMesh& m : g.take_meshes({})) sent.insert(key3(m.chunk[0], m.chunk[1], m.chunk[2]));
    for (u64 k : g.take_removed_chunks()) CHECK_FALSE(sent.count(k));
    for (const ChunkMesh& m : g.take_water_meshes()) water_sent.insert(key3(m.chunk[0], m.chunk[1], m.chunk[2]));
    for (u64 k : g.take_water_removed()) CHECK_FALSE(water_sent.count(k));
  }
  CHECK(sent.size() > 50);
  CHECK(water_sent.size() > 3);  // (the reservoir, the water tower)
}

TEST_CASE("game: burning pieces are meshed again (charring, glow); charring chunks for their decoration only") {
  Game g;
  Level w = make_procedural("yard", 1);
  g.load(std::move(w.grid), w.spawn_pos, w.spawn_dir);
  g.bake();
  g.set_env("fire.wood.burn_s", 12.0);
  const f64 h = g.world().voxel_size();
  // the water tower's legs burn: it falls, burning
  for (int x : {250, 263})
    for (int y : {92, 105}) g.ignite({h * (x + 1), h * (y + 1), h * 2}, 0.35);
  int remesh = 0, decor = 0;
  for (int t = 0; t < 60 * 60 && remesh == 0; ++t) {
    g.tick();
    for (const ChunkMesh& m : g.take_meshes({})) decor += g.decoration_only(key3(m.chunk[0], m.chunk[1], m.chunk[2])) ? 1 : 0;
    for (const GameEvent& e : g.take_events()) remesh += e.kind == GameEvent::Kind::Remesh ? 1 : 0;
  }
  CHECK(decor > 0);
  CHECK(remesh > 0);
}

TEST_CASE("game: an oriented grid is meshed where its frame puts it, felt by the client's collision, and goes with it") {
  const f64 h = 0.125;
  auto box = [](VoxelGrid& g, const IVec3& lo, const IVec3& hi, Vox v) {
    for (i32 x = lo[0]; x < hi[0]; ++x)
      for (i32 y = lo[1]; y < hi[1]; ++y) g.fill_column(x, y, lo[2], hi[2], v);
  };
  Game g;
  VoxelGrid w;
  w.h = h;
  box(w, {-32, -32, -4}, {96, 96, 0}, make_vox(MaterialId::Rock, true));
  w.compact();
  g.load(std::move(w), V3{0, 0, 0}, V3{1, 0, 0});
  // a masonry wall (8 m, 3 voxels thick, 3 m high) turned 45 degrees about z, centred at (6, 6)
  VoxelGrid wall;
  wall.h = h;
  box(wall, {-32, -1, 0}, {32, 2, 24}, make_vox(MaterialId::Masonry, false));
  wall.compact();
  const Quat rot{0.0, 0.0, std::sin(0.125 * 3.14159265358979323846), std::cos(0.125 * 3.14159265358979323846)};
  const V3 origin{6.0, 6.0, 0.0};
  const GridId id = g.world().add_grid(GridFrame{origin, rot}, std::move(wall));
  REQUIRE(id != 0);
  g.bake();
  g.tick();
  // its chunks' meshes: its id, their vertices in its lattice (placed in the world by its view)
  i32 meshes = 0;
  bool placed = true;
  std::vector<IVec3> chunks;
  for (const ChunkMesh& m : g.take_meshes({})) {
    if (m.grid != id) continue;
    ++meshes;
    chunks.push_back(m.chunk);
    for (const MeshVertex& v : m.vertices)
      placed = placed && v.pos[0] >= -32.5 * h - 1e-4 && v.pos[0] <= 31.5 * h + 1e-4 && std::abs(v.pos[1]) <= 1.5 * h + 1e-4 &&
               v.pos[2] >= -0.5 * h - 1e-4 && v.pos[2] <= 23.5 * h + 1e-4;
  }
  CHECK(meshes > 0);
  CHECK(placed);
  // its view: where it is
  const std::vector<GridView> views = g.take_grid_views();
  REQUIRE(views.size() == 1);
  CHECK(views[0].id == id);
  CHECK(views[0].origin.x == origin.x);
  CHECK(views[0].rot.z == rot.z);
  CHECK(views[0].voxel_size == h);
  CHECK(g.take_grid_views().empty());  // (unchanged: not again)
  // the client's collision: its chunks' occupancy in its lattice
  std::vector<u8> bits(kChunkVox / 8);
  const IVec3 on{0, 0, 8}, off{0, 5, 8};
  REQUIRE(g.grid_chunk_occupancy(id, chunk_of(on), bits.data()) == 2);
  auto bit = [&](const IVec3& p) { return (bits[size_t(chunk_index(p) >> 3)] >> (chunk_index(p) & 7)) & 1; };
  CHECK(bit(on) == 1);
  CHECK(bit(off) == 0);
  // the world grid's occupancy is its own
  REQUIRE(g.chunk_occupancy(IVec3{1, 1, 0}, bits.data()) == 0);
  // removed: its meshes and its view go
  REQUIRE(g.world().remove_grid(id));
  g.tick();
  g.take_meshes({});
  CHECK(static_cast<i32>(g.take_removed_grid_chunks().size()) == meshes);
  CHECK(g.take_grid_views().empty());
  const std::vector<GridId> gone = g.take_removed_grids();
  REQUIRE(gone.size() == 1);
  CHECK(gone[0] == id);
  CHECK(g.grid_chunk_occupancy(id, chunk_of(on), bits.data()) == 0);
}

TEST_CASE("game: the machines world - its machines run on their drives, its free parts hang on their joints, the ball knocks the wall") {
  struct Run {
    i32 pieces = 0, joints = 0;
    i64 wall = 0;
    f64 lift = 0.0, bridge = 0.0, spin = 0.0, jib = 0.0;
    u64 hash = 0;
  };
  auto run = [](int threads) {
    set_num_threads(threads);
    Run r;
    Game g;
    load_level(g, make_procedural("machines", 1));
    g.bake();
    // (the joints in the order the level made them: the lift's slider, the turntable's hinge, the
    // drawbridge's, the jib's, the rope, the rod, the chain's four, the door's hinge; the crane's
    // wall is grid 6, after the car, the disc, the deck, the jib and the ball)
    CHECK(g.world().joints().size() == 11);
    auto wall_voxels = [&]() { return g.world().grid(6)->solid_count(); };
    const i64 wall0 = wall_voxels();
    for (int t = 0; t < 600; ++t) {
      g.tick();
      JointState s;
      if (g.world().joint(1, &s)) r.lift = std::max(r.lift, s.value);
      if (g.world().joint(3, &s)) r.bridge = std::max(r.bridge, s.value);
      if (g.world().joint(4, &s)) r.jib = std::max(r.jib, s.value);
    }
    JointState s;
    if (g.world().joint(2, &s))
      if (const Body* b = g.world().piece(s.piece_b)) r.spin = b->w.z;
    r.pieces = static_cast<i32>(g.world().pieces().size());
    r.joints = static_cast<i32>(g.world().joints().size());
    r.wall = wall0 - wall_voxels();
    r.hash = g.session_hash();
    return r;
  };
  const int hw = num_threads();
  const Run a = run(1), b = run(4);
  set_num_threads(hw);
  MESSAGE("machines after 10 s: " << a.pieces << " pieces, " << a.joints << " joints, the wall lost " << a.wall << " voxels; the lift up to "
                                  << a.lift << " m, the drawbridge to " << a.bridge << " rad, the jib to " << a.jib << " rad, the turntable at "
                                  << a.spin << " rad/s");
  CHECK(a.joints == 11);                               // (all hold)
  CHECK(a.pieces >= 15);                               // (the machines' parts, the hanging ones, the crates: and the wall's rubble)
  CHECK(a.wall > 100);                                 // (the wrecking ball went through it)
  CHECK(a.lift == doctest::Approx(4.5).epsilon(0.01));  // (its program: up 4.5 m in 6 s)
  CHECK(a.bridge > 1.0);                               // (raised on its program: 1.2 rad at 8 s)
  CHECK(a.jib > 2.0);                                  // (swung round: 2.2 rad at 5 s)
  CHECK(a.spin == doctest::Approx(0.4).epsilon(0.05));
  CHECK(a.hash == b.hash);
}

TEST_CASE("game: the machines come down with what holds them - shot, carved, blasted") {
  Game g;
  load_level(g, make_procedural("machines", 1));
  g.bake();
  for (int t = 0; t < 120; ++t) g.tick();
  // (the joints: 1 the lift's slider, 2 the turntable's hinge, 3 the drawbridge's, 4 the jib's,
  // 5 the rope, 6 the pendulum's rod)
  auto piece_z = [&](JointId j) {
    JointState s;
    if (!g.world().joint(j, &s)) return -1.0;
    const Body* b = g.world().piece(s.piece_b);
    return b ? b->x.z : -1.0;
  };
  JointState s;
  REQUIRE(g.world().joint(1, &s));
  const i64 car = s.piece_b;
  REQUIRE(g.world().joint(4, &s));
  const i64 jib = s.piece_b;
  REQUIRE(g.world().joint(6, &s));
  const i64 bob = s.piece_b;
  REQUIRE(g.world().joint(3, &s));
  const i64 deck = s.piece_b;
  const f64 car0 = piece_z(1), jib0 = piece_z(4), deck0 = piece_z(3);
  // the lift's slider shot off the tower's face, the pendulum's beam shot at its pivot, a rocket
  // at the crane mast's foot, one at the drawbridge's hinge seat
  g.carve(V3{15.9375, 6.5, 0.1875}, 0.35);
  g.carve(V3{30.6875, 27.9375, 5.4375}, 0.35);
  g.blast(V3{12.6, 27.94, 0.5}, 0.6, 3e5);
  g.blast(V3{42.0, 6.5, 1.9}, 0.5, 2e5);
  for (int t = 0; t < 240; ++t) g.tick();
  auto z_of = [&](i64 id) {
    const Body* b = g.world().piece(id);
    if (b) return b->x.z;
    // (broken up: the highest of its parts - pieces that came of it - is where it went)
    f64 z = -1.0;
    for (const PieceState& p : g.world().pieces()) {
      i64 up = g.world().piece(p.id)->parent;
      for (int k = 0; k < 16 && up != 0 && up != id; ++k) {
        const Body* a = g.world().piece(up);
        up = a ? a->parent : 0;
      }
      if (up == id) z = std::max(z, p.pos.z);
    }
    return z;
  };
  MESSAGE("shot away: the lift's car from z " << car0 << " to " << z_of(car) << ", the jib from " << jib0 << " to " << z_of(jib) << ", the pendulum's bob to "
                                             << z_of(bob) << ", the drawbridge's deck from " << deck0 << " to " << z_of(deck) << "; joints left "
                                             << g.world().joints().size());
  CHECK_FALSE(g.world().joint(1, &s));  // (the slider let go)
  CHECK_FALSE(g.world().joint(6, &s));  // (the rod let go)
  CHECK(z_of(car) < 0.4);
  CHECK(z_of(jib) < jib0 - 2.0);
  CHECK(z_of(bob) < 0.6);
  CHECK(z_of(deck) < deck0 - 0.5);
  // the turntable, blasted: its disc breaks
  REQUIRE(g.world().joint(2, &s));
  const i64 disc = s.piece_b;
  const i32 voxels0 = g.world().piece(disc)->count;
  g.blast(V3{31.5, 8.0, 0.9}, 0.5, 2e5);
  for (int t = 0; t < 60; ++t) g.tick();
  const Body* d = g.world().piece(disc);
  MESSAGE("the turntable blasted: its disc of " << voxels0 << " voxels now " << (d ? d->count : 0));
  CHECK((d == nullptr || d->count < voxels0));
}

TEST_CASE("game: the machines world saved in play comes back as it was - its pieces, machines and clock") {
  Game a;
  load_level(a, make_procedural("machines", 1));
  a.bake();
  for (int t = 0; t < 180; ++t) a.tick();
  const std::vector<u8> delta = a.save_delta();
  // (the level made again, as a host loads it, then the saved session)
  Game b;
  load_level(b, make_procedural("machines", 1));
  b.bake();
  REQUIRE(b.load_delta(delta));
  MESSAGE("machines saved at 3 s: " << delta.size() << " bytes, " << a.world().pieces().size() << " pieces, " << a.world().joints().size()
                                    << " joints; restored " << b.world().pieces().size() << " pieces, " << b.world().joints().size() << " joints");
  CHECK(b.world().pieces().size() == a.world().pieces().size());
  CHECK(b.world().joints().size() == a.world().joints().size());
  CHECK(b.world().time() == a.world().time());
  // (its voxels, pieces and joints as they were: the environment's transient state - fire's step
  // clock, smoke - is not part of a delta)
  CHECK(b.world().state_hash() == a.world().state_hash());
  i32 same = 0;
  for (const PieceState& p : a.world().pieces()) {
    const Body* pb = b.world().piece(p.id);
    same += pb && pb->x.x == p.pos.x && pb->x.y == p.pos.y && pb->x.z == p.pos.z && pb->v.z == p.vel.z ? 1 : 0;
  }
  CHECK(same == static_cast<i32>(a.world().pieces().size()));
  for (JointId id : a.world().joints()) {
    JointState x, y;
    REQUIRE(b.world().joint(id, &y));
    a.world().joint(id, &x);
    CHECK(x.value == y.value);
    CHECK(x.piece_b == y.piece_b);
  }
  // (and no crates dropped in again: the session had its drops - they would come as new grids)
  const std::vector<GridId> grids = b.world().grids();
  b.tick();
  CHECK(b.world().grids() == grids);
}

TEST_CASE("game: the far render tier draws the oriented grids of a streamed world too") {
  // the same city, its buildings in the world grid, and with about one in eight turned in grids of
  // their own: the far tiles hold about as much of them either way (a turned building's voxels
  // are splatted into the coarse cells their centres fall in)
  auto far_vertices = [](bool turned, i32* tiles) {
    Game eng;
    VoxelGrid g;
    g.h = 0.125;
    auto src = make_city_source(3, 1000.0, 0.125, turned);
    const auto sp = src->spawn_pos(), sd = src->spawn_dir();
    eng.load(std::move(g), sp, sd);
    eng.load_streaming(std::move(src), eng.grid().h, StreamConfig{});
    eng.set_viewer({sp[0], sp[1], sp[2] + 1.6});
    size_t verts = 0;
    *tiles = 0;
    for (int t = 0; t < 200; ++t) {
      eng.tick();
      for (const ChunkMesh& m : eng.take_far_meshes()) {
        verts += m.vertices.size();
        ++*tiles;
      }
    }
    return verts;
  };
  i32 n0 = 0, n1 = 0;
  const size_t plain = far_vertices(false, &n0), turned = far_vertices(true, &n1);
  MESSAGE("far tier: " << plain << " vertices in " << n0 << " tiles; with turned buildings " << turned << " in " << n1);
  CHECK(n1 == n0);
  CHECK(turned > 0.9 * static_cast<f64>(plain));
  CHECK(turned != plain);  // (the turned ones are there, turned)
}

TEST_CASE("game: a burning turned grid is meshed again as it glows and chars") {
  const f64 h = 0.125;
  auto box = [](VoxelGrid& g, const IVec3& lo, const IVec3& hi, Vox v) {
    for (i32 x = lo[0]; x < hi[0]; ++x)
      for (i32 y = lo[1]; y < hi[1]; ++y) g.fill_column(x, y, lo[2], hi[2], v);
  };
  Game g;
  VoxelGrid w;
  w.h = h;
  box(w, {-32, -32, -4}, {32, 32, 0}, make_vox(MaterialId::Rock, true));
  w.compact();
  g.load(std::move(w), V3{0, 0, 0}, V3{1, 0, 0});
  VoxelGrid wall;
  wall.h = h;
  box(wall, {-12, 0, 0}, {12, 1, 24}, make_vox(MaterialId::Wood, false));
  wall.compact();
  const Quat rot{0.0, 0.0, std::sin(0.25), std::cos(0.25)};
  const GridId id = g.world().add_grid(GridFrame{V3{0.0, 0.0, 0.0}, rot}, std::move(wall));
  REQUIRE(id != 0);
  g.bake();
  g.tick();
  g.take_meshes({});
  g.ignite(g.world().grid_to_world(id, V3{0.0, 0.0, h}), 0.3);
  i32 remeshed = 0, glowing = 0;
  for (int t = 0; t < 5 * 60; ++t) {
    g.tick();
    for (const ChunkMesh& m : g.take_meshes({})) {
      if (m.grid != id) continue;
      ++remeshed;
      for (const MeshVertex& v : m.vertices) glowing += (v.texture & 0xFF00) == 0xFE00 ? 1 : 0;
    }
  }
  MESSAGE("a burning turned grid: " << remeshed << " chunk meshes again, " << glowing << " glowing vertices");
  CHECK(remeshed > 0);
  CHECK(glowing > 0);
}

TEST_CASE("game: a front end that applies the events alone has the pieces pieces() has - culled ones until they fade") {
  const f64 h = 0.125;
  Game g;
  VoxelGrid w;
  w.h = h;
  for (i32 x = -24; x < 40; ++x)
    for (i32 y = -24; y < 40; ++y) w.fill_column(x, y, -4, 0, make_vox(MaterialId::Rock, true));
  // a brick tower (1.5 m square, 6 m high) on the rock
  for (i32 x = 0; x < 12; ++x)
    for (i32 y = 0; y < 12; ++y) w.fill_column(x, y, 0, 48, make_vox(MaterialId::Masonry, false));
  w.compact();
  g.load(std::move(w), V3{-2.0, -2.0, 0.0}, V3{1, 0, 0});
  g.bake();
  REQUIRE(g.set_tunable("max_bodies", 3));  // (most of what comes down is culled)
  g.blast({0.75, 0.75, 0.6}, 1.0, 2e6);
  std::unordered_set<i64> shown;  // (Detached, less Removed)
  i32 removed = 0, culled_late = 0;
  std::unordered_set<i64> fading_seen;
  for (int t = 0; t < 60 * 6; ++t) {
    g.tick();
    for (const GameEvent& e : g.take_events()) {
      if (e.kind == GameEvent::Kind::Detached) {
        CHECK(shown.insert(e.id).second);
      } else if (e.kind == GameEvent::Kind::Removed) {
        CHECK(shown.erase(e.id) == 1);
        ++removed;
        culled_late += fading_seen.count(e.id) ? 1 : 0;
      }
    }
    std::unordered_set<i64> posed;
    for (const PiecePose& p : g.pieces()) {
      posed.insert(p.id);
      if (p.opacity < 1.0) fading_seen.insert(p.id);
    }
    CHECK(posed == shown);
  }
  MESSAGE(removed << " removed, " << culled_late << " of them after fading out");
  CHECK(removed > 0);
  CHECK(culled_late > 0);
}
