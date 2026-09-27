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
void svx_set_params(svx_engine* e, double compliance, double amplification, double fragility, double damping,
                    int debug_view, int paused);

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

/* out[8]: pos xyz, normal xyz, distance, material; returns 1 on hit. */
/* The player's "use" (doors, lift switches): returns 1 if a mover was activated. */
int svx_use(svx_engine* e, double ox, double oy, double oz, double dx, double dy, double dz);
int svx_raycast(svx_engine* e, double ox, double oy, double oz, double dx, double dy, double dz, double max_dist,
                double* out);
/* out[4]: move xyz, on_ground */
void svx_collide(svx_engine* e, double minx, double miny, double minz, double maxx, double maxy, double maxz,
                 double mx, double my, double mz, double* out);

/* Meshes of changed chunks: count, then per mesh info[8] = chunk xyz, origin xyz, vertex count,
 * index count, and the buffers. */
int svx_poll_meshes(svx_engine* e);
void svx_mesh_info(svx_engine* e, int i, double* out);
const void* svx_mesh_vertices(svx_engine* e, int i);
const void* svx_mesh_indices(svx_engine* e, int i);
/* Chunks that became empty: count, then chunk coordinates (3 ints). */
int svx_poll_removed(svx_engine* e);
void svx_removed_chunk(svx_engine* e, int i, int* out3);
/* Far render tier of streamed worlds: new far tile meshes (count, then info[8] = tile x, y, 0,
 * origin xyz, vertex count, index count, and the buffers) and removed far tiles (x, y). */
int svx_poll_far(svx_engine* e);
void svx_far_info(svx_engine* e, int i, double* out);
const void* svx_far_vertices(svx_engine* e, int i);
const void* svx_far_indices(svx_engine* e, int i);
int svx_poll_far_removed(svx_engine* e);
void svx_far_removed(svx_engine* e, int i, int* out2);
/* Occupancy of chunk (cx, cy, cz) for client-side collision: returns 0 (all air or not
 * resident), 1 (all solid) or 2 (mixed: out4096 receives 32^3 bits, bit v of byte v >> 3 for
 * voxel index v = (x * 32 + y) * 32 + z, local coordinates). */
int svx_chunk_occupancy(svx_engine* e, int cx, int cy, int cz, uint8_t* out4096);

/* Events: count, then per event info[21] = kind (0 detached, 1 crack, 2 impact, 3 bubble), id,
 * pos xyz, velocity xyz, angular xyz, normal xyz, radius, strength, voxels, level, vertex count,
 * index count, rigid; detached events carry a world-space mesh (pos = its centre of mass). A
 * rigid detached piece is simulated as debris: its pose comes from svx_debris until it is gone. */
int svx_poll_events(svx_engine* e);
void svx_event_info(svx_engine* e, int i, double* out);
const void* svx_event_vertices(svx_engine* e, int i);
const void* svx_event_indices(svx_engine* e, int i);

/* GPU displacement (off by default): chunks under running bubbles are meshed per face once and
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

/* Rigid debris: count of live pieces, then svx_debris_data holds 9 doubles per piece: id (of
 * its detached event), centre xyz, rotation since detachment as quaternion xyzw, opacity 0..1.
 * A mesh vertex p (world, at detachment) is drawn at centre + R (p - event pos). */
int svx_debris(svx_engine* e);
const double* svx_debris_data(svx_engine* e);
/* Rigid debris on (default) or off (detached pieces are only reported). */
void svx_set_debris(svx_engine* e, int enabled);

/* out[38]: tick ms, structural ms, event ms, active bubbles, active nodes, voxels, chunks,
 * memory MB, events, bubbles spawned, static settles, ruptures, detached voxels, ticks,
 * design max utilization, strengthened voxels, unbaked chunks, bake ms, resident chunks,
 * archived chunks, stream ms, evicted chunks, debris pieces, debris landings, impact loads,
 * debris ms, verifications, verification failures, verification ms (main thread), bubble
 * steps, bubble step ms (cumulative), bubble PCG iterations (cumulative), movers (doors / lifts),
 * cracks (ruptures kept as contacts), coarse chunk summaries made (streamed worlds: non-resident
 * chunks a detachment search crossed), chunks made resident for a detached piece, S_p in
 * effect, S_p cap (the map's buckling margin; 0 = none), events merged into the running bubble
 * of their structure, bubbles that extracted a whole structure, bubble steps longer than a tick */
void svx_stats(svx_engine* e, double* out);
/* Deterministic digest of the session (voxels, bonds, damage, debris poses), split in two
 * 32-bit halves (JS numbers). */
void svx_state_hash(svx_engine* e, double* out2);

#ifdef __cplusplus
}
#endif

#endif
