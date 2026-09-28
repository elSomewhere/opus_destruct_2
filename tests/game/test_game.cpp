// Game core (plan Phases 3-6): determinism, queries, persistence round trip.
#include <algorithm>
#include <cmath>
#include <vector>

#include <unordered_set>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/game/game.hpp"
#include "svx/game/procgen.hpp"
#include "svx/game/city.hpp"

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
  ProcWorld w = make_procedural("rooms", 7);
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
    ProcWorld w = make_procedural("yard", 1);
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
  ProcWorld w = make_procedural("yard", 1);
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
  const f64 s = std::sqrt(0.5);
  const Quat rot{0.0, 0.0, std::sin(0.125 * 3.14159265358979323846), std::cos(0.125 * 3.14159265358979323846)};
  const V3 origin{6.0, 6.0, 0.0};
  const GridId id = g.world().add_grid(GridFrame{origin, rot}, std::move(wall));
  REQUIRE(id != 0);
  g.bake();
  g.tick();
  // its chunks' meshes: its id, and vertices on its turned faces (in the wall's frame: across it
  // within its half thickness, along it within its half length)
  i32 meshes = 0;
  bool placed = true;
  for (const ChunkMesh& m : g.take_meshes({})) {
    if (m.grid != id) continue;
    ++meshes;
    for (const MeshVertex& v : m.vertices) {
      const V3 d = V3{v.pos[0], v.pos[1], v.pos[2]} - origin;
      const f64 along = s * (d.x + d.y), across = s * (d.y - d.x);
      placed = placed && std::abs(across) <= 1.5 * h + 1e-4 && std::abs(along + 0.5 * h) <= 32 * h + 1e-4 && d.z >= -0.5 * h - 1e-4 &&
               d.z <= 23.5 * h + 1e-4;
    }
  }
  CHECK(meshes > 0);
  CHECK(placed);
  // the client's collision: the world voxel at the wall's centre is solid, one 1 m off it is not
  const std::vector<u64> occ = g.take_occupancy_changed();
  const IVec3 on{48, 48, 8}, off{42, 54, 8};
  const IVec3 cc = chunk_of(on);
  CHECK(chunk_of(off) == cc);
  CHECK(std::find(occ.begin(), occ.end(), key3(cc[0], cc[1], cc[2])) != occ.end());
  std::vector<u8> bits(kChunkVox / 8);
  REQUIRE(g.chunk_occupancy(cc, bits.data()) == 2);
  auto bit = [&](const IVec3& p) { return (bits[size_t(chunk_index(p) >> 3)] >> (chunk_index(p) & 7)) & 1; };
  CHECK(bit(on) == 1);
  CHECK(bit(off) == 0);
  // removed: its meshes and its occupancy go
  REQUIRE(g.world().remove_grid(id));
  g.tick();
  g.take_meshes({});
  CHECK(static_cast<i32>(g.take_removed_grid_chunks().size()) == meshes);
  const std::vector<u64> occ2 = g.take_occupancy_changed();
  CHECK(std::find(occ2.begin(), occ2.end(), key3(cc[0], cc[1], cc[2])) != occ2.end());
  CHECK(g.chunk_occupancy(cc, bits.data()) == 0);
}
