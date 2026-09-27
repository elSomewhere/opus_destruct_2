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
