// The core's C API (svx/svx_core.h).
#include <cmath>
#include <cstring>
#include <vector>

#include "doctest.h"
#include "svx/svx_core.h"
#include "vehicle_materials.hpp"

namespace {

// 6 m x 6 m of rock, a free 2 m concrete block on legs of 25 cm at its corners.
std::vector<uint8_t> scene(int* nx, int* ny, int* nz) {
  *nx = 48;
  *ny = 48;
  *nz = 40;
  std::vector<uint8_t> v(size_t(*nx) * size_t(*ny) * size_t(*nz), 0);
  auto at = [&](int x, int y, int z) -> uint8_t& { return v[(size_t(x) * size_t(*ny) + size_t(y)) * size_t(*nz) + size_t(z)]; };
  for (int x = 0; x < *nx; ++x)
    for (int y = 0; y < *ny; ++y) {
      for (int z = 0; z < 4; ++z) at(x, y, z) = svxc_vox(SVXC_ROCK, 1);
      const bool leg = (x % 30 < 2) && (y % 30 < 2) && x < 32 && y < 32;
      for (int z = 4; z < 20; ++z)
        if (leg) at(x, y, z) = svxc_vox(SVXC_CONCRETE, 0);
      for (int z = 20; z < 36; ++z)
        if (x < 32 && y < 32) at(x, y, z) = svxc_vox(SVXC_CONCRETE, 0);
    }
  return v;
}

int generate_flat(void* user, int cx, int cy, int cz, uint8_t* out) {
  (void)user, (void)cx, (void)cy;
  if (cz != -1) return 0;
  std::memset(out, svxc_vox(SVXC_ROCK, 1), 32768);
  return 1;
}

}  // namespace

TEST_CASE("capi: tunables by name, a scene, commands, events, pieces, persistence") {
  svxc_world* w = svxc_create(0.125);
  CHECK(svxc_set(w, "max_bodies", 500) == 0);
  CHECK(svxc_get(w, "max_bodies") == 500.0);
  CHECK(svxc_set(w, "rigid.gravity", 9.81) == 0);
  CHECK(svxc_set(w, "fragility", 1.5) == 0);
  CHECK(svxc_get(w, "fragility") == 1.5);
  CHECK(svxc_set(w, "no.such.knob", 1.0) == -1);
  CHECK(std::isnan(svxc_get(w, "no.such.knob")));
  svxc_set(w, "fragility", 1.0);
  int nx, ny, nz;
  const std::vector<uint8_t> v = scene(&nx, &ny, &nz);
  svxc_load_box(w, v.data(), nx, ny, nz, -8, -8, -4);
  CHECK(svxc_bake(w) == 1);
  const uint64_t h0 = svxc_state_hash(w);
  // the legs go
  for (double lx : {0.0, 30.0})
    for (double ly : {0.0, 30.0}) svxc_carve(w, 0.125 * (lx - 8 + 0.5), 0.125 * (ly - 8 + 0.5), 0.125 * 4, 0.3);
  int added = 0;
  for (int t = 0; t < 180; ++t) {
    svxc_tick(w);
    const int n = svxc_poll_events(w);
    for (int i = 0; i < n; ++i) {
      svxc_event e;
      REQUIRE(svxc_event_at(w, i, &e) == 1);
      added += e.kind == SVXC_PIECE_ADDED;
    }
  }
  CHECK(added >= 1);
  CHECK(svxc_state_hash(w) != h0);
  const int np = svxc_poll_pieces(w);
  REQUIRE(np >= 1);
  svxc_piece p;
  REQUIRE(svxc_piece_at(w, 0, &p) == 1);
  int lo[3], dim[3];
  const uint8_t* shape = svxc_piece_voxels(w, p.id, lo, dim);
  REQUIRE(shape != nullptr);
  int solid = 0;
  for (int i = 0; i < dim[0] * dim[1] * dim[2]; ++i) solid += shape[i] != 0;
  CHECK(solid == p.voxels);
  const int32_t* chunks = nullptr;
  CHECK(svxc_poll_changed_chunks(w, &chunks) >= 1);
  std::vector<uint8_t> cv(32768);
  CHECK(svxc_chunk_voxels(w, chunks[0], chunks[1], chunks[2], cv.data()) >= 0);
  // persistence: the same scene, the delta
  size_t size = 0;
  const uint8_t* d = svxc_save_delta(w, &size);
  REQUIRE(size > 0);
  std::vector<uint8_t> delta(d, d + size);
  svxc_world* w2 = svxc_create(0.125);
  svxc_load_box(w2, v.data(), nx, ny, nz, -8, -8, -4);
  svxc_bake(w2);
  CHECK(svxc_load_delta(w2, delta.data(), delta.size()) == 0);
  CHECK(svxc_state_hash(w2) == svxc_state_hash(w));
  CHECK(svxc_load_delta(w2, delta.data(), delta.size() / 2) == -1);
  // queries
  const double o[3] = {0.5, 0.5, 10.0}, dn[3] = {0, 0, -1};
  const svxc_hit hit = svxc_raycast(w2, o, dn, 30.0);
  CHECK(hit.hit == 1);
  double out[4];
  const double mn[3] = {-0.9, -0.9, 3.0}, mx[3] = {-0.6, -0.6, 4.7}, mv[3] = {0, 0, -5};
  svxc_collide(w2, mn, mx, mv, out);
  CHECK(out[3] == 1.0);  // (lands on the rock)
  svxc_destroy(w2);
  svxc_destroy(w);
}

TEST_CASE("capi: materials and a streamed world from a C callback") {
  svxc_material m{};
  m.name = "ice";
  m.E = 9e9;
  m.G = 3.5e9;
  m.rho = 917;
  m.ft = m.fb = 0.2e6;
  m.fc = 5e6;
  m.cohesion = 0.3e6;
  m.friction = 0.1;
  m.Gf = 10;
  m.frag[0] = m.frag[1] = m.frag[2] = 0.0;  // (sanitized: at least a voxel)
  m.frag_noise = 0.3;
  const int id = svxc_material_set(-1, &m);
  REQUIRE(id >= 7);
  CHECK(svxc_material_find("ice") == id);
  svxc_material back{};
  REQUIRE(svxc_material_get(id, &back) == 1);
  CHECK(back.frag[0] >= 1.0);
  CHECK(back.rho == 917);
  svxc_materials_reset();
  CHECK(svxc_material_find("ice") == -1);

  svxc_world* w = svxc_create(0.125);
  const int lo[3] = {-4, -4, -1}, hi[3] = {4, 4, 1};
  svxc_stream sc = svxc_stream_defaults();
  sc.load_radius = 12.0;
  sc.evict_radius = 16.0;
  sc.chunks_per_tick = 64;
  sc.archive_mb = 1.0;
  svxc_enable_streaming(w, generate_flat, nullptr, lo, hi, &sc);
  const double f[3] = {0, 0, 1};
  svxc_set_focus(w, f, 1);
  for (int t = 0; t < 5; ++t) svxc_tick(w);
  svxc_stats s;
  svxc_get_stats(w, &s);
  CHECK(s.resident_chunks > 10);
  CHECK(s.archive_capacity_mb == doctest::Approx(1.0));
  svxc_memory mem;
  svxc_get_memory(w, &mem);
  CHECK(mem.archive < 1 << 20);  // (the arena is reserved once; its pages count as they are used)
  CHECK(mem.total == mem.grid + mem.fragments + mem.structures + mem.pieces + mem.archive + mem.caches + mem.queues);
  CHECK(svxc_set(w, "memory.piece_mb", 32.0) == 0);
  CHECK(svxc_get(w, "memory.piece_mb") == 32.0);
  const double o[3] = {1, 1, 5}, dn[3] = {0, 0, -1};
  CHECK(svxc_raycast(w, o, dn, 20.0).hit == 1);
  svxc_destroy(w);
}

namespace {

struct Host {
  int steps = 0;
  svxc_world* seen = nullptr;
  int64_t lift = 0;  // a piece to push up, with a force of five times its weight
  double mass = 0.0;
};

void host_step(void* user, svxc_world* w, double dt) {
  Host* h = static_cast<Host*>(user);
  ++h->steps;
  h->seen = w;
  (void)dt;
  if (h->lift) {
    const double p[3] = {0, 0, 0}, f[3] = {0.0, 0.0, 5.0 * 9.81 * h->mass};
    svxc_piece pc{};
    for (int i = 0, n = svxc_poll_pieces(w); i < n; ++i)
      if (svxc_piece_at(w, i, &pc) && pc.id == h->lift) {
        const double at[3] = {pc.pos[0], pc.pos[1], pc.pos[2]};
        svxc_apply_force(w, h->lift, at, f);
      }
    (void)p;
  }
}

}  // namespace

TEST_CASE("capi: extension points: layers, damage, loads, a host's system, forces on pieces") {
  svxc_world* w = svxc_create(0.125);
  Host host;
  svxc_add_system(w, "host", host_step, &host);
  const int soot = svxc_add_layer(w, "soot", 1, SVXC_BIND_PLACE);
  CHECK(soot > 0);
  CHECK(svxc_layer_index(w, "soot") == soot);
  CHECK(svxc_layer_index(w, "damage") == SVXC_DAMAGE_LAYER);
  CHECK(svxc_add_layer(w, "", 1, SVXC_BIND_PLACE) == -1);
  CHECK(svxc_add_layer(w, "soot", 0, SVXC_BIND_PLACE) == -1);  // (another layer under that name)
  CHECK(svxc_add_layer(w, "soot", 1, SVXC_BIND_PLACE) == soot);
  int nx, ny, nz;
  const std::vector<uint8_t> v = scene(&nx, &ny, &nz);
  svxc_load_box(w, v.data(), nx, ny, nz, -8, -8, -4);
  CHECK(svxc_bake(w) == 1);
  const int32_t p[3] = {0, 0, 20};
  const uint8_t val = 77;
  CHECK(svxc_set_layer(w, soot, p, &val, 1) == 1);
  CHECK(svxc_layer(w, soot, 0, 0, 20) == 77);
  for (int t = 0; t < 30; ++t) svxc_tick(w);
  CHECK(host.steps == 30);
  CHECK(host.seen == w);
  CHECK(svxc_poll_pieces(w) == 0);
  // loads: 12.8 MN on the block's top: its legs give way
  std::vector<int32_t> xyz;
  std::vector<double> f;
  for (int x = 4; x < 12; ++x)
    for (int y = 4; y < 12; ++y) {
      xyz.insert(xyz.end(), {x, y, 31});
      f.insert(f.end(), {0.0, 0.0, -2e5});
    }
  svxc_set_loads(w, 7, xyz.data(), f.data(), 64);
  for (int t = 0; t < 60; ++t) svxc_tick(w);
  svxc_stats st;
  svxc_get_stats(w, &st);
  CHECK(st.bonds_broken > 0);
  svxc_set_loads(w, 7, nullptr, nullptr, 0);
  for (int t = 0; t < 240; ++t) svxc_tick(w);
  // a cube dropped beside the pile comes to rest; the host's system pushes it up
  std::vector<int32_t> cube;
  std::vector<uint8_t> conc;
  for (int x = 32; x < 36; ++x)
    for (int y = 32; y < 36; ++y)
      for (int z = 8; z < 12; ++z) {
        cube.insert(cube.end(), {x, y, z});
        conc.push_back(svxc_vox(SVXC_CONCRETE, 0));
      }
  svxc_set_voxels(w, cube.data(), conc.data(), 64, 0);
  svxc_piece pc{}, big{};
  for (int t = 0; t < 240; ++t) svxc_tick(w);
  for (int i = 0, n = svxc_poll_pieces(w); i < n; ++i)
    if (svxc_piece_at(w, i, &pc) && pc.voxels == 64 && pc.pos[0] > 3.5 && pc.pos[1] > 3.5) big = pc;
  REQUIRE(big.id != 0);
  host.lift = big.id;
  host.mass = big.mass;
  svxc_wake_piece(w, big.id);
  for (int t = 0; t < 30; ++t) svxc_tick(w);
  bool up = false;
  for (int i = 0, n = svxc_poll_pieces(w); i < n; ++i)
    if (svxc_piece_at(w, i, &pc) && pc.id == big.id) up = pc.vel[2] > 5.0 && pc.pos[2] > big.pos[2] + 1.0;
  CHECK(up);
  // a piece's layers, and its voxels taken away
  int lo[3], dim[3];
  const uint8_t* pv = svxc_piece_voxels(w, big.id, lo, dim);
  REQUIRE(pv);
  int32_t sv[3] = {0, 0, 0};
  bool found = false;
  for (int i = 0; i < dim[0] * dim[1] * dim[2] && !found; ++i)
    if (pv[i]) {
      sv[0] = lo[0] + i / (dim[1] * dim[2]);
      sv[1] = lo[1] + (i / dim[2]) % dim[1];
      sv[2] = lo[2] + i % dim[2];
      found = true;
    }
  REQUIRE(found);
  const uint8_t d = 200;
  CHECK(svxc_set_piece_layer(w, big.id, SVXC_DAMAGE_LAYER, sv, &d, 1) == 1);
  CHECK(svxc_piece_layer(w, big.id, SVXC_DAMAGE_LAYER, sv[0], sv[1], sv[2]) == 200);
  CHECK(svxc_remove_piece_voxels(w, big.id, sv, 1, 1) == 1);
  svxc_destroy(w);
}

namespace {

struct HostSys {
  int loads = 0, changed = 0, steps = 0;
};

}  // namespace

TEST_CASE("capi: a host's system with all its callbacks; layer changes and chunk layers") {
  svxc_world* w = svxc_create(0.125);
  HostSys hs;
  svxc_system s{};
  s.name = "host";
  s.user = &hs;
  s.step = [](void* u, svxc_world*, double) { ++static_cast<HostSys*>(u)->steps; };
  s.on_load = [](void* u, svxc_world*) { ++static_cast<HostSys*>(u)->loads; };
  s.on_chunks = [](void* u, svxc_world*, int kind, const int32_t*, int n) {
    if (kind == SVXC_CHUNKS_CHANGED) static_cast<HostSys*>(u)->changed += n;
  };
  s.memory_bytes = [](void*) -> int64_t { return 4321; };
  s.state_hash = [](void* u) -> uint64_t { return static_cast<uint64_t>(static_cast<HostSys*>(u)->steps); };
  CHECK(svxc_add_system_ex(w, &s) == 1);
  svxc_system bad{};
  CHECK(svxc_add_system_ex(w, &bad) == 0);  // (no step)
  const int soot = svxc_add_layer(w, "soot", 1, SVXC_BIND_SOLID);
  int nx, ny, nz;
  const std::vector<uint8_t> v = scene(&nx, &ny, &nz);
  svxc_load_box(w, v.data(), nx, ny, nz, -8, -8, -4);
  CHECK(hs.loads == 1);
  const uint64_t h1 = svxc_session_hash(w);
  svxc_tick(w);
  CHECK(svxc_session_hash(w) != h1);  // (its state hash is part of the session's)
  svxc_carve(w, 0.0, 0.0, 2.0, 0.3);
  svxc_tick(w);
  CHECK(hs.changed > 0);
  const int32_t p[3] = {4, 4, 20};
  const uint8_t val = 33;
  REQUIRE(svxc_set_layer(w, soot, p, &val, 1) == 1);
  const int32_t* chunks = nullptr;
  REQUIRE(svxc_poll_layer_changes(w, soot, &chunks) == 1);
  CHECK(chunks[0] == 0);
  CHECK(chunks[2] == 0);
  CHECK(svxc_poll_layer_changes(w, soot, &chunks) == 0);
  std::vector<uint8_t> buf(32768, 7);
  CHECK(svxc_chunk_layer(w, soot, 0, 0, 0, buf.data()) == 1);
  CHECK(buf[(4 * 32 + 4) * 32 + 20] == 33);
  CHECK(svxc_chunk_layer(w, soot, 5, 5, 5, buf.data()) == 0);
  CHECK(buf[0] == 0);
  svxc_memory m;
  svxc_get_memory(w, &m);
  CHECK(m.systems == 4321);
  // n > 0 without arrays: nothing changes (it does not clear the group)
  const int32_t lv[3] = {4, 4, 30};
  const double lf[3] = {0.0, 0.0, -10.0};
  svxc_set_loads(w, 3, lv, lf, 1);
  svxc_set_loads(w, 3, nullptr, nullptr, 1);
  svxc_destroy(w);
}

TEST_CASE("capi: every tunable by name reads back what was set") {
  svxc_world* w = svxc_create(0.125);
  CHECK(svxc_set(w, "rigid.gravity", 7.5) == 0);
  CHECK(svxc_get(w, "rigid.gravity") == 7.5);
  CHECK(svxc_set(w, "pulverize", 0.0) == 0);
  CHECK(svxc_get(w, "pulverize") == 0.0);
  CHECK(svxc_set(w, "paused", 1.0) == 0);
  CHECK(svxc_get(w, "paused") == 1.0);
  CHECK(svxc_set(w, "max_bodies", 1e12) == 0);  // (clamped to the field's range)
  CHECK(svxc_get(w, "max_bodies") <= 2e9);
  CHECK(svxc_set(w, "fragility", NAN) == -1);
  svxc_destroy(w);
}

TEST_CASE("capi: an oriented grid on the world grid, its events, pieces of two shapes, rays and sweeps") {
  svxc_world* w = svxc_create(0.125);
  // the ground, and a free concrete column standing on it
  const int nx = 64, ny = 64, nz = 30;
  std::vector<uint8_t> v(size_t(nx) * size_t(ny) * size_t(nz), 0);
  auto at = [&](int x, int y, int z) -> uint8_t& { return v[(size_t(x) * size_t(ny) + size_t(y)) * size_t(nz) + size_t(z)]; };
  for (int x = 0; x < nx; ++x)
    for (int y = 0; y < ny; ++y) {
      for (int z = 0; z < 4; ++z) at(x, y, z) = svxc_vox(SVXC_ROCK, 1);
      if (x >= 28 && x < 36 && y >= 28 && y < 36)
        for (int z = 4; z < 24; ++z) at(x, y, z) = svxc_vox(SVXC_CONCRETE, 0);
    }
  svxc_load_box(w, v.data(), nx, ny, nz, -32, -32, -4);
  // a slab turned 30 degrees about z, resting on the column's top (z = 20 - 1/2 voxels)
  std::vector<uint8_t> slab(16 * 16 * 2, svxc_vox(SVXC_CONCRETE, 0));
  const double origin[3] = {0.0, 0.0, 0.125 * 20};
  const double t = 0.5 * 30.0 * 3.14159265358979323846 / 180.0;
  const double rot[4] = {0.0, 0.0, std::sin(t), std::cos(t)};
  const uint32_t id = svxc_add_grid(w, slab.data(), 16, 16, 2, -8, -8, 0, origin, rot, 1);
  REQUIRE(id > 0);
  uint32_t ids[4] = {0, 0, 0, 0};
  CHECK(svxc_grids(w, ids, 4) == 1);
  CHECK(ids[0] == id);
  double o[3], r[4];
  REQUIRE(svxc_grid_frame(w, id, o, r) == 1);
  CHECK(std::abs(o[2] - 2.5) < 1e-12);
  CHECK(std::abs(r[2] - std::sin(t)) < 1e-12);
  CHECK(svxc_bake(w) == 1);
  // it stands (bonded to the column's top) and is hit where it is
  for (int k = 0; k < 30; ++k) svxc_tick(w);
  CHECK(svxc_poll_pieces(w) == 0);
  const double ro[3] = {0.1, 0.1, 6.0}, rd[3] = {0.0, 0.0, -1.0};
  const svxc_hit hit = svxc_raycast(w, ro, rd, 20.0);
  REQUIRE(hit.hit);
  CHECK(hit.grid == id);
  CHECK(std::abs(hit.pos[2] - (2.5 + 1.5 * 0.125)) < 1e-6);
  const double mn[3] = {-0.1, -0.1, 4.0}, mx[3] = {0.1, 0.1, 4.5}, mv[3] = {0.0, 0.0, -3.0};
  double sw[5];
  REQUIRE(svxc_sweep(w, mn, mx, mv, sw) == 1);
  CHECK(static_cast<uint32_t>(sw[4]) == id);
  CHECK(std::abs(4.0 + sw[0] * -3.0 - (2.5 + 1.5 * 0.125)) < 1e-6);
  // the column's foot goes: column and slab fall as one piece of two shapes
  std::vector<int32_t> xyz;
  std::vector<uint8_t> air;
  for (int x = -4; x < 4; ++x)
    for (int y = -4; y < 4; ++y) {
      xyz.insert(xyz.end(), {x, y, 0});
      air.push_back(0);
    }
  CHECK(svxc_set_voxels(w, xyz.data(), air.data(), static_cast<int>(air.size()), 0) == 64);
  svxc_tick(w);
  const int np = svxc_poll_pieces(w);
  int two = 0;
  for (int i = 0; i < np; ++i) {
    svxc_piece p;
    REQUIRE(svxc_piece_at(w, i, &p) == 1);
    if (svxc_piece_shape_count(w, p.id) != 2) continue;
    ++two;
    int lo[3], dim[3];
    double off[3], sr[4];
    uint32_t g = 99;
    REQUIRE(svxc_piece_shape(w, p.id, 1, lo, dim, off, sr, &g) != nullptr);
    CHECK(g == id);
    CHECK(std::abs(std::abs(sr[2]) - std::sin(t)) < 1e-9);
  }
  CHECK(two == 1);
  // events: the grid came, and goes
  svxc_poll_events(w);
  CHECK(svxc_remove_grid(w, id) == 1);
  const int ne = svxc_poll_events(w);
  bool removed = false;
  for (int i = 0; i < ne; ++i) {
    svxc_event e;
    svxc_event_at(w, i, &e);
    removed = removed || (e.kind == SVXC_GRID_REMOVED && e.id == static_cast<int64_t>(id));
  }
  CHECK(removed);
  svxc_stats s;
  svxc_get_stats(w, &s);
  CHECK(s.grids == 0);
  svxc_destroy(w);
}

TEST_CASE("capi: grids of their own voxel size and priority, moved; a lift's car on a driven slider a controller rides") {
  svxc_world* w = svxc_create(0.125);
  // the level: 8 m of rock ground, a lift's car (a 2 m timber deck, a voxel up) on a slider held
  // by the ground under it
  std::vector<uint8_t> g(64 * 64 * 4, svxc_vox(SVXC_ROCK, 1));
  svxc_load_box(w, g.data(), 64, 64, 4, -32, -32, -4);
  std::vector<uint8_t> deck(16 * 16 * 2, svxc_vox(SVXC_WOOD, 0));
  svxc_grid_desc dd{};
  dd.rot[3] = 1.0;
  dd.base = 1;
  const uint32_t dg = svxc_add_grid_desc(w, deck.data(), 16, 16, 2, -8, -8, 1, &dd);
  REQUIRE(dg != 0);
  svxc_joint_desc jd;
  svxc_joint_defaults(&jd);
  jd.type = SVXC_JOINT_SLIDER;
  jd.a.kind = SVXC_ANCHOR_GRID;
  jd.a.id = 0;
  jd.b.kind = SVXC_ANCHOR_GRID;
  jd.b.id = dg;
  jd.a.point[2] = jd.b.point[2] = 0.0;  // (the gap between the ground's voxel and the car's)
  jd.drive.kind = SVXC_DRIVE_TARGET;
  jd.drive.max = 20000.0;
  jd.drive.speed = 0.5;
  const uint32_t slider = svxc_add_joint(w, &jd);
  REQUIRE(slider != 0);
  CHECK(svxc_bake(w) == 1);
  // a fine grid (0.0625 m) of priority 2, of this session
  std::vector<uint8_t> blk(8 * 8 * 8, svxc_vox(SVXC_STEEL, 1));
  svxc_grid_desc d{};
  d.origin[0] = 2.0;
  d.origin[2] = 0.5;
  d.rot[3] = 1.0;
  d.voxel_size = 0.0625;
  d.priority = 2;
  d.base = 0;
  const uint32_t fine = svxc_add_grid_desc(w, blk.data(), 8, 8, 8, 0, 0, 0, &d);
  REQUIRE(fine != 0);
  CHECK(svxc_grid_voxel_size(w, fine) == 0.0625);
  CHECK(svxc_grid_priority(w, fine) == 2);
  const double o2[3] = {3.0, 0.0, 0.5}, r2[4] = {0, 0, 0, 1};
  CHECK(svxc_set_grid_frame(w, fine, o2, r2) == 1);
  double at[3], rot[4];
  REQUIRE(svxc_grid_frame(w, fine, at, rot) == 1);
  CHECK(at[0] == 3.0);
  int moved = 0;
  for (int i = 0, n = svxc_poll_events(w); i < n; ++i) {
    svxc_event e;
    svxc_event_at(w, i, &e);
    moved += e.kind == SVXC_GRID_MOVED && e.id == fine ? 1 : 0;
  }
  CHECK(moved == 1);
  // the car rises at 0.5 m/s towards 2 m
  svxc_tick(w);
  jd.drive.target = 2.0;
  REQUIRE(svxc_set_joint_drive(w, slider, &jd.drive) == 1);
  for (int t = 0; t < 60; ++t) svxc_tick(w);
  CHECK(svxc_time(w) == doctest::Approx(61.0 / 60.0).epsilon(1e-9));
  svxc_joint_state s;
  REQUIRE(svxc_joint(w, slider, &s) == 1);
  CHECK(s.value == doctest::Approx(0.5).epsilon(0.05));
  REQUIRE(s.piece_b != 0);
  // a controller's box standing on the car: it rides it (the car's top at 2.5 h + its move)
  const double top = 2.5 * 0.125 + s.value;
  const double mn[3] = {-0.2, -0.2, top + 0.01}, mx[3] = {0.2, 0.2, top + 1.8}, fall[3] = {0.0, 0.0, -0.1};
  svxc_collision c;
  svxc_collide_ex(w, mn, mx, fall, &c);
  CHECK(c.on_ground == 1);
  CHECK(c.ground_piece == s.piece_b);
  CHECK(c.ground_velocity[2] == doctest::Approx(0.5).epsilon(0.05));
  svxc_sweep_hit sh;
  REQUIRE(svxc_sweep_ex(w, mn, mx, fall, &sh) == 1);
  CHECK(sh.piece == s.piece_b);
  CHECK(sh.velocity[2] == doctest::Approx(0.5).epsilon(0.05));
  // a box sunk into the car rises out of it
  const double mn2[3] = {-0.2, -0.2, top - 0.1}, mx2[3] = {0.2, 0.2, top + 1.7};
  CHECK(svxc_overlaps(w, mn2, mx2) == 1);
  const double rise = svxc_depenetrate(w, mn2, mx2, 0.5);
  CHECK(rise >= 0.1);
  CHECK(rise < 0.2);
  // the grids' changes saved and restored
  size_t n = 0;
  const uint8_t* bytes = svxc_save_delta(w, &n);
  const std::vector<uint8_t> delta(bytes, bytes + n);
  svxc_world* w2 = svxc_create(0.125);
  svxc_load_box(w2, g.data(), 64, 64, 4, -32, -32, -4);
  REQUIRE(svxc_add_grid_desc(w2, deck.data(), 16, 16, 2, -8, -8, 1, &dd) == dg);  // (the level's grids)
  CHECK(svxc_bake(w2) == 1);
  REQUIRE(svxc_load_delta(w2, delta.data(), delta.size()) == 0);
  CHECK(svxc_grid_voxel_size(w2, fine) == 0.0625);
  double at2[3], rot2[4];
  REQUIRE(svxc_grid_frame(w2, fine, at2, rot2) == 1);
  CHECK(at2[0] == 3.0);
  svxc_destroy(w2);
  svxc_destroy(w);
}

TEST_CASE("capi: joints - a hinged block swinging, its state, a drive, breaking") {
  svxc_world* w = svxc_create(0.125);
  std::vector<uint8_t> g(64 * 64 * 4, svxc_vox(SVXC_ROCK, 1));
  svxc_load_box(w, g.data(), 64, 64, 4, -32, -32, -4);
  CHECK(svxc_bake(w) == 1);
  // a 0.5 m wooden block of this session, hinged at its edge to the world about x
  std::vector<uint8_t> blk(4 * 4 * 4, svxc_vox(SVXC_WOOD, 0));
  const double o[3] = {0.0, 0.0, 3.0}, r[4] = {0, 0, 0, 1};
  const uint32_t bg = svxc_add_grid(w, blk.data(), 4, 4, 4, -2, 0, -2, o, r, 0);
  REQUIRE(bg != 0);
  svxc_joint_desc d;
  svxc_joint_defaults(&d);
  CHECK(d.length == -1.0);
  d.type = SVXC_JOINT_HINGE;
  d.a.kind = SVXC_ANCHOR_WORLD;
  d.b.kind = SVXC_ANCHOR_GRID;
  d.b.id = bg;
  for (int k = 0; k < 3; ++k) d.a.point[k] = d.b.point[k] = 0.0;
  d.a.point[1] = d.b.point[1] = -0.0625;
  d.a.point[2] = d.b.point[2] = 3.0;
  d.axis[0] = 1.0;
  d.axis[2] = 0.0;
  const uint32_t j = svxc_add_joint(w, &d);
  REQUIRE(j != 0);
  uint32_t ids[4];
  CHECK(svxc_joints(w, ids, 4) == 1);
  for (int t = 0; t < 30; ++t) svxc_tick(w);
  svxc_joint_state s;
  REQUIRE(svxc_joint(w, j, &s) == 1);
  CHECK(s.type == SVXC_JOINT_HINGE);
  CHECK(s.piece_b != 0);
  CHECK(std::abs(s.value) > 0.2);  // (it swung down about its hinge)
  // a drive at speed 0 holds it against its weight
  svxc_joint_drive dr = d.drive;
  CHECK(dr.kind == SVXC_DRIVE_OFF);
  CHECK(dr.period == 10.0);
  dr.kind = SVXC_DRIVE_SPEED;
  dr.speed = 0.0;
  dr.max = 1e5;
  CHECK(svxc_set_joint_drive(w, j, &dr) == 1);
  dr.kind = 7;
  CHECK(svxc_set_joint_drive(w, j, &dr) == 0);
  for (int t = 0; t < 30; ++t) svxc_tick(w);
  svxc_joint_state s2;
  REQUIRE(svxc_joint(w, j, &s2) == 1);
  for (int t = 0; t < 30; ++t) svxc_tick(w);
  svxc_joint_state s3;
  REQUIRE(svxc_joint(w, j, &s3) == 1);
  CHECK(std::abs(s3.value - s2.value) < 0.02);
  CHECK(svxc_remove_joint(w, j) == 1);
  CHECK(svxc_joints(w, ids, 4) == 0);
  // a rope too weak for it: it breaks (an event)
  svxc_joint_defaults(&d);
  d.type = SVXC_JOINT_DISTANCE;
  d.a.kind = SVXC_ANCHOR_WORLD;
  d.a.point[0] = 0.0;
  d.a.point[1] = 0.0;
  d.a.point[2] = 5.0;
  svxc_piece pc;
  REQUIRE(svxc_poll_pieces(w) == 1);
  REQUIRE(svxc_piece_at(w, 0, &pc) == 1);
  d.b.kind = SVXC_ANCHOR_PIECE;
  d.b.id = static_cast<uint64_t>(pc.id);
  for (int k = 0; k < 3; ++k) d.b.point[k] = pc.pos[k];
  d.break_force = 50.0;
  const uint32_t rope = svxc_add_joint(w, &d);
  REQUIRE(rope != 0);
  int broke = 0;
  for (int t = 0; t < 120 && !broke; ++t) {
    svxc_tick(w);
    for (int i = 0, n = svxc_poll_events(w); i < n; ++i) {
      svxc_event e;
      svxc_event_at(w, i, &e);
      broke += e.kind == SVXC_JOINT_BROKEN && e.id == rope ? 1 : 0;
    }
  }
  CHECK(broke == 1);
  svxc_destroy(w);
}

TEST_CASE("capi: wheels - a chassis on four wheels settles, drives, loses a wheel") {
  // (a host's own materials: a vehicle's frame and sheet metal, the road)
  testmat::ensure();
  const int asphalt = static_cast<int>(testmat::Asphalt), frame = static_cast<int>(testmat::CarFrame), sheet = static_cast<int>(testmat::Sheet);
  svxc_world* w = svxc_create(0.125);
  std::vector<uint8_t> g(320 * 64 * 4, svxc_vox(asphalt, 1));
  svxc_load_box(w, g.data(), 320, 64, 4, -160, -32, -4);
  CHECK(svxc_bake(w) == 1);
  // a chassis of car frame and a sheet metal shell (6.25 cm voxels), 4 m x 1.5 m
  const int nx = 64, ny = 24, nz = 8;
  std::vector<uint8_t> c(size_t(nx) * ny * nz, 0);
  for (int x = 0; x < nx; ++x)
    for (int y = 0; y < ny; ++y)
      for (int z = 0; z < nz; ++z) {
        const bool shell = z == 0 || z == nz - 1 || x == 0 || x == nx - 1 || y == 0 || y == ny - 1;
        if (shell) c[(size_t(x) * ny + y) * nz + z] = svxc_vox(z == 0 ? frame : sheet, 0);
      }
  svxc_grid_desc gd{};
  gd.rot[3] = 1.0;
  gd.origin[2] = 0.7;
  gd.voxel_size = 0.0625;
  gd.base = 0;
  const uint32_t grid = svxc_add_grid_desc(w, c.data(), nx, ny, nz, -nx / 2, -ny / 2, 0, &gd);
  REQUIRE(grid != 0);
  uint32_t wheel[4];
  for (int k = 0; k < 4; ++k) {
    svxc_wheel_desc d;
    svxc_wheel_defaults(&d);
    d.material = static_cast<int>(testmat::Tyre);
    CHECK(d.radius == 0.33);
    CHECK(d.down[2] == -1.0);
    d.mount.kind = SVXC_ANCHOR_GRID;
    d.mount.id = grid;
    d.mount.point[0] = k < 2 ? 1.5 : -1.5;
    d.mount.point[1] = (k % 2 == 0) ? 0.6 : -0.6;
    d.mount.point[2] = 0.7;
    d.group = 7;
    d.tag = static_cast<uint32_t>(k);
    d.break_force = k == 0 ? 1.0 : 0.0;  // (the front left one: it comes off at once)
    wheel[k] = svxc_add_wheel(w, &d);
    REQUIRE(wheel[k] != 0);
  }
  uint32_t ids[8];
  CHECK(svxc_wheels(w, ids, 8) == 4);
  int detached = 0;
  for (int t = 0; t < 120; ++t) {
    svxc_tick(w);
    for (int i = 0, n = svxc_poll_events(w); i < n; ++i) {
      svxc_event e;
      svxc_event_at(w, i, &e);
      detached += e.kind == SVXC_WHEEL_DETACHED && e.id == wheel[0] ? 1 : 0;
    }
  }
  CHECK(detached == 1);
  CHECK(svxc_wheels(w, ids, 8) == 3);
  svxc_wheel_state s;
  CHECK(svxc_wheel(w, wheel[0], &s) == 0);
  REQUIRE(svxc_wheel(w, wheel[3], &s) == 1);
  CHECK(s.piece != 0);
  CHECK(s.group == 7);
  CHECK(s.tag == 3);
  CHECK(s.contact == 1);
  CHECK(s.load > 0.0);
  CHECK(s.material == asphalt);
  // driven on its rear wheels, fast: its chassis may go faster than rubble
  CHECK(svxc_set_piece_max_speed(w, s.piece, 60.0) == 1);
  CHECK(svxc_set_wheel_input(w, wheel[2], 800.0, 0.0, 0.0) == 1);
  CHECK(svxc_set_wheel_input(w, wheel[3], 800.0, 0.0, 0.0) == 1);
  for (int t = 0; t < 60; ++t) svxc_tick(w);
  REQUIRE(svxc_wheel(w, wheel[3], &s) == 1);
  CHECK(s.drive == 800.0);
  CHECK(s.spin > 1.0);
  CHECK(svxc_remove_wheel(w, wheel[1]) == 1);
  CHECK(svxc_wheels(w, ids, 8) == 2);
  svxc_destroy(w);
}
