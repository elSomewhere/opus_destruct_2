/* structvox core — a flat C API of the destruction physics (docs/CORE.md), for hosts in other
 * languages and engines (C, C#, Rust, Python, JavaScript through WASM, ...). It wraps
 * svx::World (svx/world/world.hpp) one to one; C++ hosts can use World directly.
 *
 * Units: metres, kilograms, seconds; z is up. Voxel p (integer coordinates) is the cube of side
 * h centred at h p. A voxel byte is 0 for air, else 1 + material id in the low 7 bits and bit 7
 * set for an anchored voxel (a support that never moves: bedrock, foundations, kinematic
 * parts): svxc_vox(material, anchored).
 *
 * Buffers returned by pointer stay valid until the next call that fills the same kind of
 * buffer on the same world, or the next tick. A world is not thread-safe: call it from one
 * thread (it runs its own work on a shared pool). */
#ifndef SVX_CORE_H
#define SVX_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct svxc_world svxc_world;

/* ---- voxels and materials (the registry is process-wide; change it before worlds step) */

static inline uint8_t svxc_vox(int material, int anchored) {
  return (uint8_t)(((1 + material) & 0x7F) | (anchored ? 0x80 : 0));
}
enum { SVXC_RC = 0, SVXC_CONCRETE, SVXC_STEEL, SVXC_MASONRY, SVXC_SOIL, SVXC_ROCK, SVXC_BEDROCK };

typedef struct svxc_material {
  const char* name;
  double E, G, rho;          /* Young's and shear modulus (Pa), density (kg/m^3) */
  double ft, fb, fc;         /* interface tensile, flexural tensile, compressive strength (Pa) */
  double cohesion, friction; /* Mohr-Coulomb shear: cohesion (Pa), friction coefficient */
  double Gf;                 /* fracture energy (J/m^2) */
  double frag[3];            /* rubble size: fragment seed spacing in voxels (x, y, z) */
  double frag_noise;         /* relative jitter of the fragment seams (0..1) */
  int indestructible;        /* carves and blasts leave it */
} svxc_material;

/* Registers a material (the next free id, returned; -1 when all 127 are taken) or overrides
 * one (id >= 0: returns id). Non-positive or non-finite properties become defaults. */
int svxc_material_set(int id, const svxc_material* m);
/* The properties of a material id (name valid until the registry changes). 0 if unregistered. */
int svxc_material_get(int id, svxc_material* out);
int svxc_material_find(const char* name); /* id, or -1 */
void svxc_materials_reset(void);          /* back to the standard presets */

/* ---- worlds */

svxc_world* svxc_create(double voxel_size); /* h (m): 0.125 is the tuned default */
void svxc_destroy(svxc_world* w);
void svxc_set_threads(int threads);         /* the shared pool (1 = serial); results never depend on it */

/* Tunables by name: the fields of svx::WorldConfig (e.g. "dt", "max_bodies", "stress_work",
 * "cluster_nodes", "rigid.gravity", "rigid.substeps", "rigid.friction", "blast_kinetic",
 * "design_utilization", the memory budgets "memory.fragment_cache_mb", "memory.structure_mb",
 * "memory.piece_mb", "memory.cache_mb", "memory.max_events") and of svx::WorldParams
 * ("fragility", "impact", "dif", "paused", "debug_fields"). Returns 0, or -1 for an unknown
 * name. */
int svxc_set(svxc_world* w, const char* name, double value);
double svxc_get(svxc_world* w, const char* name); /* NaN for an unknown name */

/* Content. load_box: a dense box of nx x ny x nz voxel bytes at voxel origin (ox, oy, oz),
 * index (x * ny + y) * nz + z; it replaces the world (pieces removed, changes tracked from
 * here). bake: the design pass (members that would not carry their own weight are
 * strengthened: a level stands as built); returns 1 when the whole world was designed. */
void svxc_load_box(svxc_world* w, const uint8_t* voxels, int nx, int ny, int nz, int ox, int oy, int oz);
int svxc_bake(svxc_world* w);

/* Streaming: chunks (32^3 voxels, index (x * 32 + y) * 32 + z) generated on demand around the
 * focus points. The generator is called from several threads at once and must be a pure
 * function of the chunk; it fills out[32768] and returns 1, or 0 for an all-air chunk.
 * lo / hi: the world's extent in chunks, [lo, hi). Call after svxc_create (the world is
 * emptied). The changes of chunks out of range are kept in a fixed arena of archive_mb (0:
 * unbounded); when it is full (or after forget_after_s out of range, if > 0) whole regions of
 * region_chunks x region_chunks chunk columns are forgotten, least recently seen first, and come
 * back from the generator as they were. */
typedef int (*svxc_generate_fn)(void* user, int cx, int cy, int cz, uint8_t* out);
typedef struct svxc_stream {
  double load_radius, evict_radius; /* m; defaults 96, 128 */
  int chunks_per_tick;              /* default 6 */
  double max_resident_mb;           /* 0: the radii alone */
  double archive_mb;                /* default 64; 0: keep every change */
  double forget_after_s;            /* 0: only when the archive is full */
  int region_chunks;                /* default 8 */
} svxc_stream;
svxc_stream svxc_stream_defaults(void);
void svxc_enable_streaming(svxc_world* w, svxc_generate_fn fn, void* user, const int lo[3], const int hi[3], const svxc_stream* cfg);
void svxc_set_focus(svxc_world* w, const double* xyz, int count); /* count points, xyz each */

/* Persistence: the changes since load as a delta against the base world (valid until the next
 * save); applying one after the same load (returns 0, or -1 for a malformed delta: nothing
 * applied). svxc_modified: 1 if there is anything to save. */
const uint8_t* svxc_save_delta(svxc_world* w, size_t* size);
int svxc_load_delta(svxc_world* w, const uint8_t* data, size_t size);
int svxc_modified(svxc_world* w);

/* ---- commands (carve / blast: the next tick; edits: now) */

void svxc_carve(svxc_world* w, double x, double y, double z, double radius);
void svxc_blast(svxc_world* w, double x, double y, double z, double radius, double energy);
enum { SVXC_EDIT_UNTRACKED = 1, SVXC_EDIT_ISOLATED = 2 };
/* n voxels: positions xyz (3 each) and values; returns the number changed. */
int svxc_set_voxels(svxc_world* w, const int32_t* xyz, const uint8_t* values, int n, unsigned flags);
int svxc_apply_impulse(svxc_world* w, int64_t piece, const double point[3], const double impulse[3]);
int svxc_remove_piece(svxc_world* w, int64_t piece);
void svxc_tick(svxc_world* w);

/* ---- output */

enum { SVXC_PIECE_ADDED = 0, SVXC_PIECE_REMOVED, SVXC_CRACK, SVXC_IMPACT, SVXC_DUST, SVXC_FORGOTTEN };
enum { SVXC_END_SPLIT = 0, SVXC_END_CULLED, SVXC_END_OUT_OF_WORLD, SVXC_END_REMOVED, SVXC_END_UNLOADED };
typedef struct svxc_event {
  int kind;       /* SVXC_PIECE_ADDED, ... */
  int end;        /* PIECE_REMOVED: SVXC_END_* */
  int64_t id;     /* the piece */
  int64_t parent; /* PIECE_ADDED: the piece it broke from (0: the static world) */
                  /* FORGOTTEN: id is the region, pos its centre, voxels its chunks */
  double pos[3], vel[3], ang[3], normal[3];
  double rot[4];  /* x, y, z, w */
  double radius, strength; /* CRACK: utilization; IMPACT: energy (J); DUST: 1 crushed, 0 a shard */
  int voxels;
} svxc_event;
int svxc_poll_events(svxc_world* w); /* the events since the last poll: count */
int svxc_event_at(svxc_world* w, int i, svxc_event* out);

typedef struct svxc_piece {
  int64_t id;
  double pos[3]; /* centre of mass */
  double rot[4]; /* shape frame -> world (x, y, z, w): shape point s is at pos + rot (s - com) */
  double vel[3], ang[3];
  double com[3]; /* centre of mass in the shape frame (m) */
  double mass;
  int voxels, asleep;
} svxc_piece;
int svxc_poll_pieces(svxc_world* w); /* the pieces now: count */
int svxc_piece_at(svxc_world* w, int i, svxc_piece* out);
/* A piece's voxels in its shape frame: a box of dim[0] x dim[1] x dim[2] from voxel lo
 * (index (x * dim1 + y) * dim2 + z). Returns the voxel bytes, or NULL. */
const uint8_t* svxc_piece_voxels(svxc_world* w, int64_t id, int lo[3], int dim[3]);

/* Chunks whose voxels changed (or were evicted) since the last poll: count, then their chunk
 * coordinates (3 ints each). */
int svxc_poll_changed_chunks(svxc_world* w, const int32_t** chunks);
int svxc_poll_evicted_chunks(svxc_world* w, const int32_t** chunks);
/* A resident chunk's voxels into out[32768]: 0 absent (air), 1 uniform (out filled), 2 mixed. */
int svxc_chunk_voxels(svxc_world* w, int cx, int cy, int cz, uint8_t* out);

/* ---- queries */

typedef struct svxc_hit {
  int hit;
  double pos[3], normal[3], distance;
  int material;
  int voxel[3];  /* the world's voxel, or the piece's shape voxel */
  int64_t piece; /* 0: the world */
} svxc_hit;
svxc_hit svxc_raycast(svxc_world* w, const double origin[3], const double dir[3], double max_dist);
/* Moves the box [mn, mx] by move as far as the world's voxels let it (x, then y, then z); out:
 * the move made (3) and on_ground (1). */
void svxc_collide(svxc_world* w, const double mn[3], const double mx[3], const double move[3], double out[4]);

typedef struct svxc_stats {
  double tick_ms, structural_ms, rigid_ms, stream_ms, memory_mb;
  int64_t ticks, voxels, structures, pieces, awake, contacts;
  int64_t bonds_broken, detached_pieces, pulverized_voxels, resident_chunks;
  int64_t archived_chunks, forgotten_regions, forgotten_chunks, culled_pieces, dropped_events;
  double archive_used_mb, archive_capacity_mb;
} svxc_stats;
void svxc_get_stats(svxc_world* w, svxc_stats* out);
/* What the world holds, by kind (bytes): see svx::MemoryReport. */
typedef struct svxc_memory {
  int64_t grid, fragments, structures, pieces, archive, caches, queues, total;
} svxc_memory;
void svxc_get_memory(svxc_world* w, svxc_memory* out);
uint64_t svxc_state_hash(svxc_world* w);   /* voxels and broken bonds */
uint64_t svxc_session_hash(svxc_world* w); /* + the pieces' poses (determinism checks) */

#ifdef __cplusplus
}
#endif

#endif /* SVX_CORE_H */
