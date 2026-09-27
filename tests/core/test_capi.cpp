// The core's C API (svx/svx_core.h).
#include <cmath>
#include <cstring>
#include <vector>

#include "doctest.h"
#include "svx/svx_core.h"

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
