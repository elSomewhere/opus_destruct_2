/* structvox — flat C ABI for the engine worker (docs/API.md).
 *
 * All coordinates are metres, z up. Buffers returned by pointer (mesh vertices / indices,
 * event meshes) stay valid until the next poll of the same kind. Vertex layout: 28 bytes
 * (float32x3 position, snorm8x4 normal + AO, float32x2 texel uv, uint16 texture,
 * uint8 light, uint8 debug); indices uint32, CCW seen from outside. */
#ifndef SVX_API_H
#define SVX_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct svx_engine svx_engine;

svx_engine* svx_create(double voxel_size);
void svx_destroy(svx_engine* e);
void svx_set_threads(int threads);
/* Knobs (docs/V2_DESIGN.md §6): fragility (divides every bond strength), impact (contact force =
 * impact x impulse / dt), dif (dynamic increase factor of sudden changes), reserved, debug view
 * (0 none, 1 utilization, 2 fragments), paused. */
void svx_set_params(svx_engine* e, double fragility, double impact, double dif, double reserved, int debug_view,
                    int paused);

/* Worlds. Return 0 on success. */
int svx_load_procedural(svx_engine* e, const char* kind, double seed);
/* Doom map from an in-memory WAD; mode 0 = rock, 1 = air. */
int svx_load_wad(svx_engine* e, const uint8_t* data, size_t size, const char* map, int mode, int shell_voxels);
const char* svx_last_error(svx_engine* e);
/* Decoded textures of the loaded world (Doom): count, then per texture width / height, name and
 * RGBA8 pixels (row 0 = top). Ids are the indices (below 0xFF00). */
int svx_texture_count(svx_engine* e);
void svx_texture_size(svx_engine* e, int i, int* out2);
const char* svx_texture_name(svx_engine* e, int i);
const void* svx_texture_rgba(svx_engine* e, int i);
/* Persistence: the gameplay delta since load (valid until the next save) and applying a saved
 * delta to the freshly loaded world (0 on success). svx_modified: 1 if anything changed. */
const void* svx_save_delta(svx_engine* e, double* out_size);
int svx_load_delta(svx_engine* e, const uint8_t* data, size_t size);
int svx_modified(svx_engine* e);
/* Static baselines + structural design pass; returns 1 if the whole world was baked. */
int svx_bake(svx_engine* e);
/* out[13]: bounds min xyz, max xyz, voxel count, spawn pos xyz, spawn dir xyz */
void svx_world_info(svx_engine* e, double* out);

/* Simulation (fixed 60 Hz tick) and actions (applied at the next tick). */
void svx_tick(svx_engine* e);
/* Streaming / bake focus (player position). */
void svx_viewer(svx_engine* e, double x, double y, double z);
void svx_carve(svx_engine* e, double x, double y, double z, double radius);
void svx_blast(svx_engine* e, double x, double y, double z, double radius, double energy);
/* Fire (docs/ENV.md): sets fire to what burns in the sphere (the rest heats up); puts out and
 * cools the sphere. */
void svx_ignite(svx_engine* e, double x, double y, double z, double radius);
void svx_extinguish(svx_engine* e, double x, double y, double z, double radius);
/* Water (docs/ENV.md): fills the air in the sphere; removes the water in the sphere. Heat:
 * brings the solids in the sphere to (at least) this temperature. */
void svx_pour(svx_engine* e, double x, double y, double z, double radius);
void svx_drain(svx_engine* e, double x, double y, double z, double radius);
void svx_heat(svx_engine* e, double x, double y, double z, double radius, double celsius);
/* Settings by name, logged in replays: the environment's ("fire.flame_reach", "smoke.wind_x",
 * "water.loads", ...: svx_env_param_*) and the world's ("fragility", "rigid.gravity",
 * "max_bodies", ...: svx_tunable_*; setup ones are meant for before a load). Set returns 0, or
 * -1 for an unknown name; get NaN for one. */
int svx_set_env(svx_engine* e, const char* name, double value);
double svx_get_env(svx_engine* e, const char* name);
int svx_env_param_count(void);
const char* svx_env_param_name(int i);
void svx_env_param_range(int i, double* out2);
int svx_set_tunable(svx_engine* e, const char* name, double value);
double svx_get_tunable(svx_engine* e, const char* name);
int svx_tunable_count(void);
const char* svx_tunable_name(int i);
int svx_tunable_setup(int i);
/* Water surface meshes of chunks whose water changed: count, then per mesh info[8] as
 * svx_mesh_info and the buffers (texture 0xFFFE); chunks whose water is gone (3 ints each). */
int svx_poll_water(svx_engine* e);
void svx_water_info(svx_engine* e, int i, double* out);
const void* svx_water_vertices(svx_engine* e, int i);
const void* svx_water_indices(svx_engine* e, int i);
int svx_poll_water_removed(svx_engine* e);
void svx_water_removed(svx_engine* e, int i, int* out3);
/* Environment output for the renderer, taken after a tick: returns the number of flames (at
 * most max_flames, an even sample of the burning voxels); svx_env_flames then holds float32
 * x, y, z, heat (degC) per flame. The smoke: svx_env_smoke_count cells (at most max_smoke, the
 * densest), svx_env_smoke float32 x, y, z (the cell's centre; cells of 4 voxels), density
 * (about 1: thick) each. */
int svx_poll_env(svx_engine* e, int max_flames, int max_smoke);
const float* svx_env_flames(svx_engine* e);
int svx_env_smoke_count(svx_engine* e);
const float* svx_env_smoke(svx_engine* e);

/* out[8]: pos xyz, normal xyz, distance, material; returns 1 on hit. */
/* The player's "use" (doors, lift switches): returns 1 if a mover was activated. */
int svx_use(svx_engine* e, double ox, double oy, double oz, double dx, double dy, double dz);
int svx_raycast(svx_engine* e, double ox, double oy, double oz, double dx, double dy, double dz, double max_dist,
                double* out);
/* out[9]: move xyz, on_ground, the grid stood on (on_ground), the velocity there xyz of what it
 * stands on (a piece's - a lift's car, a turntable: a controller riding it adds it x dt to its
 * next move), the piece stood on (0: none) */
void svx_collide(svx_engine* e, double minx, double miny, double minz, double maxx, double maxy, double maxz,
                 double mx, double my, double mz, double* out);

/* Meshes of changed chunks: count, then per mesh info[10] = chunk xyz, origin xyz, vertex count,
 * index count, decoration only (1: charring or glow changed, the voxels did not - its occupancy
 * is as before), grid (0: the world grid; else an oriented grid's chunk: its vertices and origin
 * in the grid's lattice (metres), placed in the world by the grid's frame (svx_poll_grids); key
 * it by grid and chunk, its occupancy is svx_grid_chunk_occupancy's), and the buffers. */
int svx_poll_meshes(svx_engine* e);
void svx_mesh_info(svx_engine* e, int i, double* out);
const void* svx_mesh_vertices(svx_engine* e, int i);
const void* svx_mesh_indices(svx_engine* e, int i);
/* Chunks that became empty: count, then chunk coordinates (3 ints). */
int svx_poll_removed(svx_engine* e);
void svx_removed_chunk(svx_engine* e, int i, int* out3);
/* Oriented grids' chunks whose meshes are gone (emptied, or their grid removed): count, then
 * grid and chunk coordinates (4 ints). */
int svx_poll_removed_grid(svx_engine* e);
void svx_removed_grid_chunk(svx_engine* e, int i, int* out4);
/* The oriented grids' places (after a tick): count of the grids that came or moved, then per
 * grid info[9] = id, origin xyz, rotation xyzw, voxel size. The grids gone: count, then ids. */
int svx_poll_grids(svx_engine* e);
void svx_grid_info(svx_engine* e, int i, double* out9);
int svx_poll_grids_removed(svx_engine* e);
unsigned svx_grid_removed(svx_engine* e, int i);
/* Occupancy of an oriented grid's chunk in its lattice (as svx_chunk_occupancy). */
int svx_grid_chunk_occupancy(svx_engine* e, unsigned grid, int cx, int cy, int cz, uint8_t* out4096);
/* The joints now (to draw): count, then info[8] = id, type (0 ball, 1 hinge, 2 slider, 3 fixed,
 * 4 distance), end a xyz, end b xyz. */
int svx_poll_joints(svx_engine* e);
void svx_joint_info(svx_engine* e, int i, double* out8);
/* Far render tier of streamed worlds: new far tile meshes (count, then info[8] = tile x, y, 0,
 * origin xyz, vertex count, index count, and the buffers) and removed far tiles (x, y). */
int svx_poll_far(svx_engine* e);
void svx_far_info(svx_engine* e, int i, double* out);
const void* svx_far_vertices(svx_engine* e, int i);
const void* svx_far_indices(svx_engine* e, int i);
int svx_poll_far_removed(svx_engine* e);
void svx_far_removed(svx_engine* e, int i, int* out2);
/* Occupancy of chunk (cx, cy, cz) of the world grid for client-side collision: returns 0 (all
 * air or not resident), 1 (all solid) or 2 (mixed: out4096 receives 32^3 bits, bit v of byte
 * v >> 3 for voxel index v = (x * 32 + y) * 32 + z, local coordinates). The oriented grids' are
 * their own (svx_grid_chunk_occupancy). */
int svx_chunk_occupancy(svx_engine* e, int cx, int cy, int cz, uint8_t* out4096);

/* Events: count, then per event info[21] = kind (0 detached, 1 crack, 2 impact, 3 bubble (v1), 4 splash,
 * 5 remesh: a piece's new mesh, as detached but without its effects), id,
 * pos xyz, velocity xyz, angular xyz, normal xyz, radius, strength, voxels, level, vertex count,
 * index count, rigid; detached events carry a world-space mesh (pos = its centre of mass). A
 * rigid detached piece is simulated as debris: its pose comes from svx_debris until it is gone. */
int svx_poll_events(svx_engine* e);
void svx_event_info(svx_engine* e, int i, double* out);
const void* svx_event_vertices(svx_engine* e, int i);
const void* svx_event_indices(svx_engine* e, int i);
/* A piece's voxels for a front end's collision (Detached, Remesh: its shapes at the event's pose;
 * size in bytes to out_size; NULL: none): the layout of svx::piece_occupancy (game.hpp). */
const void* svx_event_occupancy(svx_engine* e, int i, double* out_size);

/* GPU displacement (v1; v2 has none: svx_poll_fields returns 0): chunks under running bubbles are meshed per face once and
 * move by displacement fields instead of being re-meshed every tick. svx_poll_fields returns
 * the complete current set; per field info[10] = bubble id, world position of texel (0,0,0)'s
 * centre xyz, size xyz (texels), max displacement (m), stride (voxels per texel and axis: a
 * large bubble's field is coarser), version (the bubble's commit: an unchanged version is the
 * same field); svx_field_data points at size.x*size.y*size.z RGBA binary16 texels (x fastest):
 * (w dx, w dy, w dz, w), w the solid fraction of the texel's voxels and dx its mean over them
 * (static solids count 0) (sample trilinearly at the vertex, divide xyz by w). */
void svx_set_gpu_displacement(svx_engine* e, int enabled);
int svx_poll_fields(svx_engine* e);
void svx_field_info(svx_engine* e, int i, double* out);
const void* svx_field_data(svx_engine* e, int i);

/* Rigid debris: count of live pieces, then svx_debris_data holds 15 doubles per piece: id (of
 * its detached event), centre xyz, rotation since detachment as quaternion xyzw, opacity 0..1,
 * velocity of its centre xyz, angular velocity xyz. A mesh vertex p (world, at detachment) is
 * drawn at centre + R (p - event pos). */
int svx_debris(svx_engine* e);
const double* svx_debris_data(svx_engine* e);
/* Rigid debris on (default) or off (detached pieces are only reported). */
void svx_set_debris(svx_engine* e, int enabled);

/* The number of doubles svx_stats writes. */
int svx_stats_count(void);
/* out[svx_stats_count()]: 0 tick ms, 1 structural ms, 2 event ms, 3 rigid ms, 4 mesh ms, 5 voxels, 6 chunks,
 * 7 memory MB, 8 events, 9 ticks, 10 structures (registered), 11 structures solving, 12 their
 * nodes, 13 extractions, 14 converged solves, 15 PCG iterations, 16 bonds broken, 17 detached
 * voxels, 18 detached pieces, 19 max utilization (last judge), 20 pieces, 21 awake pieces,
 * 22 contacts, 23 piece stress checks, 24 piece splits, 25 impact load cases, 26 resident
 * chunks, 27 archived chunks, 28 stream ms, 29 evicted chunks, 30 movers, 31 design max
 * utilization, 32 strengthened voxels, 33 floating voxels removed, 34 bake ms, 35 world memory
 * MB (all kinds), 36 fragment caches MB, 37 structures MB, 38 pieces MB, 39 archive used MB,
 * 40 archive capacity MB, 41 forgotten regions, 42 culled pieces, 43 hot voxels (fire),
 * 44 burning voxels, 45 environment ms, 46 smoke cells, 47 smoke blocks, 48 moving water voxels,
 * 49 water loads, 50 pieces in water (51 doubles). */
void svx_stats(svx_engine* e, double* out);
/* Deterministic digest of the session (voxels, bonds, damage, debris poses), split in two
 * 32-bit halves (JS numbers). */
void svx_state_hash(svx_engine* e, double* out2);

#ifdef __cplusplus
}
#endif

#endif
