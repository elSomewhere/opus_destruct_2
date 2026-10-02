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
/* Presets (docs/PRESETS.md): a world as data - its generator and parameters, streaming, world
 * tunables, environment, population, atmosphere, spawn. svx_load_preset loads one by id (seed 0:
 * the preset's own; non-zero on failure: svx_last_error says why). svx_presets lists them as a JSON
 * array of {id, label, group, description, generator, experimental, available} (valid until the
 * next call); svx_default_preset is the default world's id; svx_preset_atmosphere is the loaded
 * preset's atmosphere for the front end (a JSON object, "{}" when it has none). */
int svx_load_preset(svx_engine* e, const char* id, double seed);
const char* svx_presets(svx_engine* e);
const char* svx_default_preset(void);
const char* svx_preset_atmosphere(svx_engine* e);
/* The loaded world's appearance table (svx/game/appearance.hpp; docs/API.md: texture ids
 * 0xC000 + i): its number of appearances, and 8 floats each - linear r, g, b, opacity, emissive,
 * noise, gloss, glow (1/0) - valid until the next load (0 and NULL: the world has none). */
int svx_appearance_count(svx_engine* e);
const float* svx_appearances(svx_engine* e);
/* What the loaded world means (svx/game/semantics.hpp), as JSON (valid until the next call; "[]"
 * or "{}" when it has none): the buildings in a box - [{id, program, footprint: [[x, y, z]...],
 * floors: [z...], entrances: [{pos, facing, street}]}] - the furniture in it - [{id, prefab, pos,
 * yaw, building, uses: [{kind, pos, yaw}]}] - and the zones at a point - {district, settlement,
 * flavor}. */
const char* svx_buildings_in(svx_engine* e, double x0, double y0, double z0, double x1, double y1, double z1);
const char* svx_furniture_in(svx_engine* e, double x0, double y0, double z0, double x1, double y1, double z1);
const char* svx_zone_at(svx_engine* e, double x, double y, double z);
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
 * 5 remesh: a piece's new mesh, as detached but without its effects, 6 removed: a detached piece
 * is gone - its mesh and collision go; one missed shows as the piece missing from svx_debris), id,
 * pos xyz, velocity xyz, angular xyz, normal xyz, radius, strength, voxels, material (dust: what
 * was crushed or shattered - a car's glass, a wall's brick; -1 unknown), vertex count,
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

/* Vehicles (docs/VEHICLES.md): cars on the core's cast wheels, their chassis a rigid piece (drawn
 * from its detached event and svx_debris poses like any piece; crumpled, it comes again as a
 * remesh event). Kinds 0 compact, 1 sedan, 2 van, 3 pickup, 4 truck; paints svx::Paint (1..).
 * Spawning, removing, entering, leaving and driving are logged in replays. svx_spawn_vehicle
 * returns its id (0: refused); svx_drive sets the player's vehicle's controls (throttle -1..1:
 * backwards reverses, or brakes while rolling forward; brake 0..1; steer -1 right .. 1 left;
 * handbrake) until changed. svx_vehicle_near: the nearest vehicle's id within reach (0: none). */
unsigned svx_spawn_vehicle(svx_engine* e, int kind, int paint, double x, double y, double z, double yaw);
int svx_remove_vehicle(svx_engine* e, unsigned id);
int svx_enter_vehicle(svx_engine* e, unsigned id);
void svx_exit_vehicle(svx_engine* e);
unsigned svx_player_vehicle(svx_engine* e);
void svx_drive(svx_engine* e, double throttle, double brake, double steer, int handbrake);
unsigned svx_vehicle_near(svx_engine* e, double x, double y, double z, double reach);
/* The vehicles now (after a tick): count, then svx_vehicles_data holds 36 doubles each: id, chassis
 * (its piece: the id of its detached event and poses; 0 none yet), kind, paint, centre of mass
 * xyz, frame rotation xyzw (x forward, y left, z up), velocity xyz, speed (m/s forward), engine
 * rpm, gear (-1 reverse, 0 neutral, 1..), controls (throttle, brake, steer, handbrake), flags (1
 * the player's, 2 a driver's, 4 parked, 8 a wreck), the driver's seat xyz, half extent xyz (its
 * box about its frame's origin, z from the ground), wheels on, damage 0..1, its frame's origin
 * xyz (world), its engine's redline (rpm), its parts still on (doors, bonnet, bumpers, ...: pieces
 * of their own on joints to the chassis) and those it was built with. */
int svx_vehicles(svx_engine* e);
const double* svx_vehicles_data(svx_engine* e);
/* Their wheels (to draw): count, then svx_wheels_data holds 15 doubles each: vehicle id, wheel id,
 * centre xyz, rotation xyzw (x the way it rolls, y its axle, turned and spun), radius, width,
 * on the ground (1/0), slip (m/s: skids, smoke), the material under it (-1 none), suspension
 * compression (m). */
int svx_wheels(svx_engine* e);
const double* svx_wheels_data(svx_engine* e);
/* A bullet's hit (logged): holes what its energy (J) gets through within the radius (sheet
 * metal, glass, brittle materials; not armour) - docs/VEHICLES.md. */
void svx_shoot(svx_engine* e, double x, double y, double z, double radius, double energy);
/* Traffic of a streamed world with roads (the "drive" city; logged): on/off, cars driving and
 * parked around the viewer, spawned beyond near_radius and within radius (m), speed x the limits. */
void svx_set_traffic(svx_engine* e, int enabled, int cars, int parked, double near_radius, double radius, double speed_scale);

/* Pedestrians of a streamed world with walkways (the "drive" city; logged): on/off, how many about
 * the viewer, spawned beyond near_radius and within radius (m), their bodies (0 deep: every
 * physical body an articulation of the world; 1 shallow: their own; 2 hybrid: deep near the viewer
 * and near moving pieces, shallow further, on their plans alone far), the most deep bodies. */
void svx_set_pedestrians(svx_engine* e, int enabled, int count, double near_radius, double radius, int bodies, int max_deep);
/* Characters (docs/ANIM.md) are drawn from meshes in the svx_anim character vertex format (20
 * bytes: position float32x3 in rest model space; normal snorm8x3 and ambient occlusion snorm8
 * (-1..1 = 0..1); uint32 bone | palette slot << 8 | shade << 12 (128 = 1.0); uint32 indices,
 * counter-clockwise from outside), with a palette (16 slots) and each character's skin matrices:
 * a vertex is drawn at skin[bone] x its position (rigid skinning).
 * Meshes new since the last poll: count, then per mesh info[3] = id, vertex count, index count,
 * and its buffers; meshes no character draws any more: count, then their ids. Palettes new since
 * the last poll: count, then per palette its id (returned) and 48 floats (16 slots' linear rgb). */
int svx_poll_character_meshes(svx_engine* e);
void svx_character_mesh_info(svx_engine* e, int i, double* out3);
const void* svx_character_mesh_vertices(svx_engine* e, int i);
const void* svx_character_mesh_indices(svx_engine* e, int i);
int svx_poll_character_meshes_removed(svx_engine* e);
unsigned svx_character_mesh_removed(svx_engine* e, int i);
int svx_poll_character_palettes(svx_engine* e);
unsigned svx_character_palette(svx_engine* e, int i, float* out48);
/* The characters now (after a tick): count, then svx_characters_data holds 12 doubles each: id,
 * mesh, palette, flags (1 alive, 2 deep: a body of the world, 4 physical, 8 asleep: a body at
 * rest, 16 down, 32 a gib: a piece of one, drawn with its first matrix), bounding sphere centre xyz
 * and radius, hit flash 0..1, its prop's mesh (0: none), health 0..1, 1 reserved; svx_characters_skin holds 23 x 16 floats each (column-major 4x4 skin matrices, bone
 * by bone: a gib's first alone counts; the root's rides with the pelvis of a body the physics
 * moves - the feet's and toes' (bones 16, 17, 20, 21) put things on the ground under it),
 * svx_characters_prop 16 floats each (its prop's matrix). Palettes are few (a look each) and
 * stay: an id once sent is valid for the session. */
int svx_characters(svx_engine* e);
const double* svx_characters_data(svx_engine* e);
const float* svx_characters_skin(svx_engine* e);
const float* svx_characters_prop(svx_engine* e);
/* A shot's line against the world and the characters: out[10] = pos xyz, normal xyz, distance,
 * material (-1: a character), character id (0: the world), bone; returns 0 (nothing), 1 (the
 * world) or 2 (a character). A round into a character (logged; where the ray found it, fired from
 * the viewer - a body that moved since is found along the same line at the bone nearest that
 * point; energy and radius as svx_shoot's, its hole some 0.3 of the radius): returns 1 if it hit. */
/* Blood (after a tick): the drops' count, then svx_blood_drops holds 7 floats each (x, y, z,
 * radius, linear rgb); the stains' count (svx_blood_stain_count), then svx_blood_stains holds 8
 * floats each (x, y, z, the surface's normal xyz, radius, age in s). */
int svx_blood(svx_engine* e);
const float* svx_blood_drops(svx_engine* e);
int svx_blood_stain_count(svx_engine* e);
const float* svx_blood_stains(svx_engine* e);
int svx_raycast_shot(svx_engine* e, double ox, double oy, double oz, double dx, double dy, double dz, double max_dist, double* out10);
/* SI damage command: 25 doubles: kind (projectile=0, edge=1, point=2, blunt=3,
 * blast=4, crush=5), point xyz, direction xyz, mass kg, speed m/s, diameter m,
 * contact area m2, sharpness, alignment, swept edge length m, blast radius m,
 * pressure Pa, fragment count, bone (-1: infer), construction (FMJ=0, expanding=1,
 * buckshot=2), edge start xyz, edge end xyz. Invalid commands have no effect.
 * All three commands below are logged for replay/lockstep. */
// Returns 19 values: 6 leg, 6 arm, trunk, neck, consciousness, vigor, pain, speed limit, mobility.
int svx_character_capabilities(svx_engine* e, unsigned id, double* out19);
int svx_damage_character(svx_engine* e, unsigned id, const double* descriptor, int count);
int svx_attach_character_prop(svx_engine* e, unsigned id, const char* archetype, int point, const char* socket, int style);
int svx_detach_character_prop(svx_engine* e, unsigned id, int point, int reason);
void svx_set_pedestrian_loadouts(svx_engine* e, double armed_share, double carrying_share);
int svx_wound_character(svx_engine* e, unsigned id, double x, double y, double z, double radius, double energy);

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
 * 49 water loads, 50 pieces in water, 51 characters, 52 deep bodies, 53 shallow bodies, 54 on their
 * plans alone, 55 at rest, 56 characters ms (57 doubles). */
void svx_stats(svx_engine* e, double* out);
/* Deterministic digest of the session (voxels, bonds, damage, debris poses), split in two
 * 32-bit halves (JS numbers). */
void svx_state_hash(svx_engine* e, double* out2);

#ifdef __cplusplus
}
#endif

#endif
