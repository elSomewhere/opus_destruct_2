#include "svx/api/svx_api.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "svx/base/parallel.hpp"
#include "svx/doom/movers.hpp"
#include "svx/doom/world.hpp"
#include "svx/engine/engine.hpp"
#include "svx/world/procgen.hpp"
#include "svx/world/streaming.hpp"

using namespace svx;

struct svx_engine {
  Engine eng;
  f64 h = 0.125;
  std::string error;
  std::vector<ChunkMesh> meshes;
  std::vector<u64> removed;
  std::vector<EngineEvent> events;
  std::unique_ptr<doom::DoomWorld> doom;  // texturing / light for Doom worlds
  std::vector<u8> delta;
  std::vector<f64> debris;
  std::vector<ChunkMesh> far;
  std::vector<std::array<i32, 2>> far_removed;
  MeshOptions mesh_base() const {
    MeshOptions mo;
    if (doom) {
      const doom::DoomWorld* w = doom.get();
      mo.texture = [w](const IVec3& p, int face) { return w->face_texture(p, face); };
      mo.light = [w](const IVec3& p, int face) { return w->face_light(p, face); };
      mo.concurrent = false;  // (face_texture memoizes its linedef search)
    }
    return mo;
  }
};

extern "C" {

svx_engine* svx_create(double voxel_size) {
  auto* e = new svx_engine;
  e->h = voxel_size > 0.0 ? voxel_size : 0.125;
  return e;
}

void svx_destroy(svx_engine* e) { delete e; }

void svx_set_threads(int threads) { set_num_threads(threads < 1 ? 1 : threads); }

void svx_set_params(svx_engine* e, double fragility, double impact, double dif, double reserved, int debug_view,
                    int paused) {
  (void)reserved;
  EngineParams p;
  p.fragility = std::clamp(fragility, 0.05, 50.0);
  p.impact = std::clamp(impact, 0.0, 20.0);
  p.dif = std::clamp(dif, 1.0, 3.0);
  p.debug_view = debug_view;
  p.paused = paused != 0;
  e->eng.set_params(p);
}

int svx_load_procedural(svx_engine* e, const char* kind, double seed) {
  const std::string k = kind ? kind : "rooms";
  e->doom.reset();
  if (k == "city") {
    // the 1 km^2 city streams around the viewer (plan Phase 6)
    auto src = make_city_source(static_cast<u64>(seed), 1000.0, e->h);
    VoxelGrid g;
    g.h = e->h;
    const auto sp = src->spawn_pos(), sd = src->spawn_dir();
    e->eng.load(std::move(g), sp, sd);
    e->eng.enable_streaming(std::move(src), StreamConfig{});
    return 0;
  }
  ProcWorld w = make_procedural(k, static_cast<u64>(seed), e->h);
  e->eng.load(std::move(w.grid), w.spawn_pos, w.spawn_dir);
  return 0;
}

int svx_load_wad(svx_engine* e, const uint8_t* data, size_t size, const char* map, int mode, int shell_voxels) {
  doom::Wad wad;
  std::string err;
  if (!wad.load_memory(std::vector<u8>(data, data + size), &err)) {
    e->error = err;
    return 1;
  }
  doom::VoxelizeOptions vo;
  vo.void_mode = mode == 1 ? doom::VoidMode::Air : doom::VoidMode::Rock;
  if (shell_voxels > 0) vo.shell_voxels = shell_voxels;
  auto w = std::make_unique<doom::DoomWorld>();
  if (!doom::build_doom_world(wad, map ? map : "", vo, e->h, true, w.get(), &err)) {
    e->error = err;
    return 1;
  }
  VoxelGrid g = std::move(w->grid);
  w->grid = VoxelGrid{};
  w->grid.h = g.h;
  const std::array<f64, 3> sp = w->spawn_pos, sd = w->spawn_dir;
  e->eng.load(std::move(g), sp, sd);
  w->live = &e->eng.grid();  // texturing reads the live (mutated) grid
  doom::attach_doom_movers(e->eng, *w);  // doors and lifts (the world object outlives them)
  e->doom = std::move(w);
  return 0;
}

const char* svx_last_error(svx_engine* e) { return e->error.c_str(); }

int svx_texture_count(svx_engine* e) { return e->doom ? static_cast<int>(e->doom->textures.textures.size()) : 0; }

void svx_texture_size(svx_engine* e, int i, int* out2) {
  const doom::Texture& t = e->doom->textures.textures[i];
  out2[0] = t.width;
  out2[1] = t.height;
}

const char* svx_texture_name(svx_engine* e, int i) { return e->doom->textures.textures[i].name.c_str(); }

const void* svx_texture_rgba(svx_engine* e, int i) { return e->doom->textures.textures[i].rgba.data(); }

const void* svx_save_delta(svx_engine* e, double* out_size) {
  e->delta = e->eng.save_delta();
  *out_size = static_cast<double>(e->delta.size());
  return e->delta.data();
}

int svx_load_delta(svx_engine* e, const uint8_t* data, size_t size) {
  return e->eng.load_delta(std::vector<u8>(data, data + size)) ? 0 : 1;
}

int svx_modified(svx_engine* e) { return e->eng.modified() ? 1 : 0; }

int svx_bake(svx_engine* e) { return e->eng.bake() ? 1 : 0; }

void svx_world_info(svx_engine* e, double* out) {
  const VoxelGrid& g = e->eng.grid();
  for (int q = 0; q < 3; ++q) {
    out[q] = g.h * (g.lo[q] - 0.5);
    out[3 + q] = g.h * (g.hi[q] - 0.5);
  }
  out[6] = static_cast<double>(g.solid_count());
  const auto sp = e->eng.spawn_pos(), sd = e->eng.spawn_dir();
  for (int q = 0; q < 3; ++q) {
    out[7 + q] = sp[q];
    out[10 + q] = sd[q];
  }
}

void svx_tick(svx_engine* e) { e->eng.tick(); }

void svx_viewer(svx_engine* e, double x, double y, double z) { e->eng.set_viewer({x, y, z}); }

void svx_carve(svx_engine* e, double x, double y, double z, double radius) { e->eng.carve({x, y, z}, radius); }

void svx_blast(svx_engine* e, double x, double y, double z, double radius, double energy) {
  e->eng.blast({x, y, z}, radius, energy);
}

int svx_use(svx_engine* e, double ox, double oy, double oz, double dx, double dy, double dz) {
  return e->eng.use({ox, oy, oz}, {dx, dy, dz}) ? 1 : 0;
}

int svx_raycast(svx_engine* e, double ox, double oy, double oz, double dx, double dy, double dz, double max_dist,
                double* out) {
  const RayHit h = e->eng.raycast({ox, oy, oz}, {dx, dy, dz}, max_dist);
  if (!h.hit) return 0;
  for (int q = 0; q < 3; ++q) {
    out[q] = h.pos[q];
    out[3 + q] = h.normal[q];
  }
  out[6] = h.distance;
  out[7] = h.material;
  return 1;
}

void svx_collide(svx_engine* e, double minx, double miny, double minz, double maxx, double maxy, double maxz,
                 double mx, double my, double mz, double* out) {
  const CollideResult r = e->eng.collide({minx, miny, minz}, {maxx, maxy, maxz}, {mx, my, mz});
  for (int q = 0; q < 3; ++q) out[q] = r.move[q];
  out[3] = r.on_ground ? 1.0 : 0.0;
}

int svx_poll_meshes(svx_engine* e) {
  e->meshes = e->eng.take_meshes(e->mesh_base());
  return static_cast<int>(e->meshes.size());
}

void svx_mesh_info(svx_engine* e, int i, double* out) {
  const ChunkMesh& m = e->meshes[i];
  for (int q = 0; q < 3; ++q) {
    out[q] = m.chunk[q];
    out[3 + q] = e->h * (m.chunk[q] * kChunk - 0.5);
  }
  out[6] = static_cast<double>(m.vertices.size());
  out[7] = static_cast<double>(m.indices.size());
}

const void* svx_mesh_vertices(svx_engine* e, int i) { return e->meshes[i].vertices.data(); }
const void* svx_mesh_indices(svx_engine* e, int i) { return e->meshes[i].indices.data(); }

int svx_poll_removed(svx_engine* e) {
  e->removed = e->eng.take_removed_chunks();
  return static_cast<int>(e->removed.size());
}

void svx_removed_chunk(svx_engine* e, int i, int* out3) {
  const IVec3 c = unkey3(e->removed[i]);
  for (int q = 0; q < 3; ++q) out3[q] = c[q];
}

int svx_poll_far(svx_engine* e) {
  e->far = e->eng.take_far_meshes();
  return static_cast<int>(e->far.size());
}

void svx_far_info(svx_engine* e, int i, double* out) {
  const ChunkMesh& m = e->far[i];
  const f64 tile = kChunk * static_cast<f64>(StreamConfig{}.far_tile);
  for (int q = 0; q < 3; ++q) out[q] = m.chunk[q];
  out[3] = e->h * (m.chunk[0] * tile - 0.5);
  out[4] = e->h * (m.chunk[1] * tile - 0.5);
  out[5] = 0.0;
  out[6] = static_cast<double>(m.vertices.size());
  out[7] = static_cast<double>(m.indices.size());
}

const void* svx_far_vertices(svx_engine* e, int i) { return e->far[i].vertices.data(); }
const void* svx_far_indices(svx_engine* e, int i) { return e->far[i].indices.data(); }

int svx_poll_far_removed(svx_engine* e) {
  e->far_removed = e->eng.take_far_removed();
  return static_cast<int>(e->far_removed.size());
}

void svx_far_removed(svx_engine* e, int i, int* out2) {
  out2[0] = e->far_removed[i][0];
  out2[1] = e->far_removed[i][1];
}

int svx_chunk_occupancy(svx_engine* e, int cx, int cy, int cz, uint8_t* out4096) {
  const Chunk* ch = e->eng.grid().chunk({cx, cy, cz});
  if (!ch) return 0;
  if (ch->uniform) return vox_solid(ch->value) ? 1 : 0;
  std::fill(out4096, out4096 + kChunkVox / 8, uint8_t{0});
  int any = 0, all = 1;
  for (int v = 0; v < kChunkVox; ++v) {
    if (vox_solid(ch->v[static_cast<size_t>(v)])) {
      out4096[v >> 3] = static_cast<uint8_t>(out4096[v >> 3] | (1u << (v & 7)));
      any = 1;
    } else {
      all = 0;
    }
  }
  return all ? 1 : any ? 2 : 0;
}

int svx_poll_events(svx_engine* e) {
  e->events = e->eng.take_events();
  return static_cast<int>(e->events.size());
}

void svx_event_info(svx_engine* e, int i, double* out) {
  const EngineEvent& v = e->events[i];
  out[0] = static_cast<double>(static_cast<int>(v.kind));
  out[1] = static_cast<double>(v.id);
  for (int q = 0; q < 3; ++q) {
    out[2 + q] = v.pos[q];
    out[5 + q] = v.vel[q];
    out[8 + q] = v.ang[q];
    out[11 + q] = v.normal[q];
  }
  out[14] = v.radius;
  out[15] = v.strength;
  out[16] = v.voxels;
  out[17] = v.level;
  out[18] = static_cast<double>(v.mesh.vertices.size());
  out[19] = static_cast<double>(v.mesh.indices.size());
  out[20] = v.rigid ? 1.0 : 0.0;
}

const void* svx_event_vertices(svx_engine* e, int i) { return e->events[i].mesh.vertices.data(); }
const void* svx_event_indices(svx_engine* e, int i) { return e->events[i].mesh.indices.data(); }

// v2 has no displacement fields (pieces are rigid; structures stand still until they break).
void svx_set_gpu_displacement(svx_engine* e, int enabled) {
  (void)e;
  (void)enabled;
}

int svx_poll_fields(svx_engine* e) {
  (void)e;
  return 0;
}

void svx_field_info(svx_engine* e, int i, double* out) {
  (void)e;
  (void)i;
  (void)out;
}

const void* svx_field_data(svx_engine* e, int i) {
  (void)e;
  (void)i;
  return nullptr;
}

int svx_debris(svx_engine* e) {
  const std::vector<PiecePose> ps = e->eng.pieces();
  e->debris.resize(9 * ps.size());
  f64* o = e->debris.data();
  for (const PiecePose& p : ps) {
    o[0] = static_cast<double>(p.id);
    o[1] = p.pos.x;
    o[2] = p.pos.y;
    o[3] = p.pos.z;
    o[4] = p.rot.x;
    o[5] = p.rot.y;
    o[6] = p.rot.z;
    o[7] = p.rot.w;
    o[8] = p.opacity;
    o += 9;
  }
  return static_cast<int>(ps.size());
}

const double* svx_debris_data(svx_engine* e) { return e->debris.data(); }

void svx_set_debris(svx_engine* e, int enabled) {
  (void)e;
  (void)enabled;
}

void svx_stats(svx_engine* e, double* out) {
  const EngineStats s = e->eng.stats();
  const double v[] = {
      s.tick_ms,                                       // 0
      s.structural_ms,                                 // 1
      s.event_ms,                                      // 2
      s.rigid_ms,                                      // 3
      s.mesh_ms,                                       // 4
      static_cast<double>(s.voxels),                   // 5
      static_cast<double>(s.chunks),                   // 6
      s.memory_mb,                                     // 7
      static_cast<double>(s.events),                   // 8
      static_cast<double>(s.ticks),                    // 9
      static_cast<double>(s.structures),               // 10
      static_cast<double>(s.solving),                  // 11
      static_cast<double>(s.solve_nodes),              // 12
      static_cast<double>(s.extractions),              // 13
      static_cast<double>(s.solves),                   // 14
      static_cast<double>(s.pcg_iters),                // 15
      static_cast<double>(s.bonds_broken),             // 16
      static_cast<double>(s.detached_voxels),          // 17
      static_cast<double>(s.detached_pieces),          // 18
      s.max_utilization,                               // 19
      static_cast<double>(s.bodies),                   // 20
      static_cast<double>(s.awake),                    // 21
      static_cast<double>(s.contacts),                 // 22
      static_cast<double>(s.body_checks),              // 23
      static_cast<double>(s.body_splits),              // 24
      static_cast<double>(s.impacts),                  // 25
      static_cast<double>(s.resident_chunks),          // 26
      static_cast<double>(s.archived_chunks),          // 27
      s.stream_ms,                                     // 28
      static_cast<double>(s.evicted_total),            // 29
      static_cast<double>(s.movers),                   // 30
      s.design_max_utilization,                        // 31
      static_cast<double>(s.strengthened_voxels),      // 32
      static_cast<double>(s.floating_voxels),          // 33
      s.bake_ms,                                       // 34
  };
  for (size_t k = 0; k < sizeof(v) / sizeof(v[0]); ++k) out[k] = v[k];
}

void svx_state_hash(svx_engine* e, double* out2) {
  const u64 h = e->eng.session_hash();
  out2[0] = static_cast<double>(h >> 32);
  out2[1] = static_cast<double>(h & 0xFFFFFFFFu);
}

}  // extern "C"
