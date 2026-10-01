/* structvox core — a flat C API of the destruction physics (docs/CORE.md), for hosts in other
 * languages and engines (C, C#, Rust, Python, JavaScript through WASM, ...). It wraps
 * svx::World (svx/world/world.hpp) one to one; C++ hosts can use World directly.
 *
 * Units: metres, kilograms, seconds; z is up. Voxel p (integer coordinates) is the cube of side
 * h centred at h p. A voxel byte is 0 for air, else 1 + material id in the low 7 bits and bit 7
 * set for an anchored voxel (a support that never moves: bedrock, foundations): svxc_vox(material,
 * anchored).
 *
 * Grids (docs/GRIDS.md): the world grid (id 0) has the world's axes; oriented grids are voxel
 * lattices of the same voxel size placed with a frame of their own (their voxel p is centred at
 * origin + rot (h p)), bonded to the world grid and to each other where their voxels meet. The
 * functions without a grid argument address the world grid.
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

/* ---- voxels and materials: the process's table (svxc_material_*) is what a world starts with
 * when it is made; each world has its own after (svxc_world_material_*) */

static inline uint8_t svxc_vox(int material, int anchored) {
  return (uint8_t)(((1 + material) & 0x7F) | (anchored ? 0x80 : 0));
}
enum {
  SVXC_RC = 0,
  SVXC_CONCRETE,
  SVXC_STEEL,
  SVXC_MASONRY,
  SVXC_SOIL,
  SVXC_ROCK,
  SVXC_BEDROCK,
  SVXC_WOOD,
  SVXC_STONE,
  SVXC_GLASS,
  SVXC_REBAR,
  SVXC_STEEL_SECTION /* a smeared section (docs/DAMAGE.md): a rolled steel member */
  /* (a host's own materials - a vehicle's sheet metal, its tyres: svxc_material_set) */
};

typedef struct svxc_material {
  const char* name;
  double E, G, rho;          /* Young's and shear modulus (Pa), density (kg/m^3) */
  double ft, fb, fc;         /* interface tensile, flexural tensile, compressive strength (Pa) */
  double cohesion, friction; /* Mohr-Coulomb shear: cohesion (Pa), friction coefficient */
  double Gf;                 /* fracture energy (J/m^2) */
  double frag[3];            /* rubble size: fragment seed spacing in voxels (x, y, z) */
  double frag_noise;         /* relative jitter of the fragment seams (0..1) */
  int indestructible;        /* carves and blasts leave it */
  int ductile;               /* it bends where brittle material crushes: never turned to dust */
  int reinforcement;         /* bars: part of the fragments of the material around them */
  double crush;              /* Pa: contact pressure at which it crumples (0: it does not) */
  double penetration;        /* J/m^3: the energy density an impact needs to remove it (0: any) */
  double grip;               /* a wheel's friction coefficient on it (0: from friction) */
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
/* The world's own materials (a copy of the process's when it was made): as svxc_material_*.
 * Changed before svxc_load: what the world builds from them keeps what it was built with. */
int svxc_world_material_set(svxc_world* w, int id, const svxc_material* m);
int svxc_world_material_get(svxc_world* w, int id, svxc_material* out);
int svxc_world_material_find(svxc_world* w, const char* name);
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
 * save): the grids' changes, and the session - its pieces (all of each: shapes, voxels, bonds,
 * pose, motion, sleep), joints and clock. Applying one after the same load (the host makes the
 * level again - its grids and joints - then loads the delta: the session's pieces and joints take
 * the place of the level's) returns 0, or -1 for a malformed delta: nothing applied.
 * svxc_modified: 1 if there is anything to save. */
const uint8_t* svxc_save_delta(svxc_world* w, size_t* size);
int svxc_load_delta(svxc_world* w, const uint8_t* data, size_t size);
int svxc_modified(svxc_world* w);

/* ---- grids (docs/GRIDS.md) */

/* Adds an oriented grid of a dense box of voxels (as svxc_load_box, in the grid's own
 * coordinates) placed at origin[3], turned by rot[4] (a quaternion x, y, z, w). base: part of the
 * level (1: not saved in deltas, only its changes; a host adds the level's grids again, in the same
 * order, after load and before svxc_load_delta) or of this session (0: saved whole). Returns its
 * id (> 0), or 0 if refused. */
uint32_t svxc_add_grid(svxc_world* w, const uint8_t* voxels, int nx, int ny, int nz, int ox, int oy, int oz, const double origin[3],
                       const double rot[4], int base);
int svxc_remove_grid(svxc_world* w, uint32_t id);
/* The oriented grids' ids (ascending) into out (up to max): their count. */
int svxc_grids(svxc_world* w, uint32_t* out, int max);
int svxc_grid_frame(svxc_world* w, uint32_t id, double origin[3], double rot[4]); /* 1, or 0: none */
/* Voxel edits and chunks in a grid's coordinates (grid 0: the world grid's). */
int svxc_set_grid_voxels(svxc_world* w, uint32_t grid, const int32_t* xyz, const uint8_t* values, int n, unsigned flags);
int svxc_grid_chunk_voxels(svxc_world* w, uint32_t grid, int cx, int cy, int cz, uint8_t* out);
/* The oriented grids' chunks whose voxels changed since the last poll: count, then 4 ints each
 * (grid id, chunk x, y, z). */
int svxc_poll_changed_grid_chunks(svxc_world* w, const int32_t** chunks);
int svxc_set_grid_layer(svxc_world* w, uint32_t grid, int layer, const int32_t* xyz, const uint8_t* values, int n);
uint8_t svxc_grid_layer(svxc_world* w, uint32_t grid, int layer, int x, int y, int z);
/* Loads on voxels of grids (grids: n ids, or NULL for the world grid's). */
void svxc_set_grid_loads(svxc_world* w, uint64_t group, const uint32_t* grids, const int32_t* xyz, const double* forces, int n);
/* A grid in full (svxc_add_grid_desc): its frame, voxel size (0: the world's; a finer grid has
 * finer surfaces, its rubble keeps the world's size in metres), priority (where grids overlap,
 * the higher keeps its voxels, then the newer) and whether it is the level's. */
typedef struct svxc_grid_desc {
  double origin[3];
  double rot[4]; /* x, y, z, w */
  double voxel_size;
  int priority;
  int base;
} svxc_grid_desc;
uint32_t svxc_add_grid_desc(svxc_world* w, const uint8_t* voxels, int nx, int ny, int nz, int ox, int oy, int oz, const svxc_grid_desc* d);
/* Places a grid anew: what it was bonded to lets go, it bonds where it is (GRID_MOVED). 1, or 0:
 * refused. */
int svxc_set_grid_frame(svxc_world* w, uint32_t id, const double origin[3], const double rot[4]);
double svxc_grid_voxel_size(svxc_world* w, uint32_t id); /* (0: none) */
int svxc_grid_priority(svxc_world* w, uint32_t id);

/* ---- joints (docs/MOTION.md): hold two things together - pieces, grids' voxels (their
 * structures take the load), the world. A machine is pieces on driven joints (a lift's car on a
 * slider, a turntable on a hinge). An end on a voxel follows it into the piece it breaks off in;
 * the joint gives way beyond its strength, or when its voxel is gone (SVXC_JOINT_BROKEN). */
enum { SVXC_JOINT_BALL = 0, SVXC_JOINT_HINGE, SVXC_JOINT_SLIDER, SVXC_JOINT_FIXED, SVXC_JOINT_DISTANCE };
enum { SVXC_ANCHOR_WORLD = 0, SVXC_ANCHOR_GRID, SVXC_ANCHOR_PIECE };
typedef struct svxc_anchor {
  int kind;        /* SVXC_ANCHOR_* */
  uint64_t id;     /* the grid (0: the world grid) or piece */
  double point[3]; /* where, in the world now (a grid's or piece's: at a solid voxel of it) */
} svxc_anchor;
/* A joint's drive: a motor of limited strength moving a hinge's turn (rad) or a slider's move (m)
 * at a speed, or to a target (a servo), or between two targets and back every period on the
 * world's clock (a machine's program: svxc_time). */
enum { SVXC_DRIVE_OFF = 0, SVXC_DRIVE_SPEED, SVXC_DRIVE_TARGET, SVXC_DRIVE_OSCILLATE };
typedef struct svxc_joint_drive {
  int kind;               /* SVXC_DRIVE_* */
  double speed;           /* rad/s, m/s: its speed (SPEED), the fastest it goes (TARGET, OSCILLATE) */
  double max;             /* N m, N: the most it gives */
  double target, target2; /* (TARGET: target) (OSCILLATE: from target to target2 and back, eased) */
  double period, phase;   /* s (OSCILLATE) */
  double stiffness;       /* 1/s: how hard it closes on its target */
} svxc_joint_drive;
typedef struct svxc_joint_desc {
  int type; /* SVXC_JOINT_* */
  svxc_anchor a, b;
  double axis[3];      /* (hinge, slider) in the world now */
  double length;       /* (distance) < 0: the ends' distance now */
  int rope;            /* (distance) 1: pulls only; 0: a rod */
  double stiffness, damping; /* (distance) stretching beyond its length: N/m, N s/m (0: rigid) */
  int limited;         /* (hinge: b's turn from now, rad; slider: its move, m) */
  double lower, upper;
  svxc_joint_drive drive; /* (hinge, slider) */
  double break_force, break_torque; /* N, N m (0: never) */
} svxc_joint_desc;
/* a ball joint, axis z, length -1, a rope, no limit, its drive off (speed 1, period 10 s, stiffness 4/s) */
void svxc_joint_defaults(svxc_joint_desc* d);
uint32_t svxc_add_joint(svxc_world* w, const svxc_joint_desc* d); /* its id, or 0: refused */
int svxc_remove_joint(svxc_world* w, uint32_t id);
int svxc_set_joint_drive(svxc_world* w, uint32_t id, const svxc_joint_drive* d);
int svxc_set_joint_limits(svxc_world* w, uint32_t id, int on, double lower, double upper);
typedef struct svxc_joint_state {
  int type;
  double a[3], b[3];            /* its ends in the world now */
  double force[3], torque[3];   /* what b received through it in the last substep (a: the opposite) */
  double value;                 /* hinge: b's turn; slider: b's move; distance: the ends' distance */
  int64_t piece_a, piece_b;     /* the pieces its ends are on (0: none) */
} svxc_joint_state;
int svxc_joint(svxc_world* w, uint32_t id, svxc_joint_state* out); /* 1, or 0: none */
int svxc_joints(svxc_world* w, uint32_t* out, int max);             /* ids (ascending) into out: their count */

/* ---- wheels (docs/MOTION.md §7): the wheels of a body, their carrier (a vehicle, a trolley, a
 * machine's undercarriage) - cast against what is under them (the statics, other pieces), on a
 * sprung suspension, with a tyre; driven, braked and steered by the host. A wheel comes off
 * beyond its strength, or when its mount voxel is gone (crushed, carved): SVXC_WHEEL_DETACHED,
 * and a wheel-shaped piece rolls on. Wheels are saved with the session and archived with their
 * carrier when it streams out. */
typedef struct svxc_wheel_desc {
  svxc_anchor mount;        /* the top of its suspension: a grid's (the carrier dropped in) or a piece's solid voxel */
  double down[3], axle[3];  /* its suspension's axis, its spin axis at zero steer (in the world now) */
  double radius, width;     /* m */
  double rest, travel;      /* m: its suspension at full droop; how far it compresses to the bump stop */
  double stiffness, damping; /* N/m, N s/m */
  double inertia;           /* kg m^2: the wheel's and its driveline's spin inertia */
  double grip;              /* x the surface's grip */
  double break_force;       /* N: it comes off beyond (0: never) */
  int material;             /* what it is made of when it comes off (a material id) */
  uint32_t group, tag;      /* the host's: what it belongs to (a vehicle, a machine), what it is to it (saved with it) */
} svxc_wheel_desc;
/* down -z, axle +y, radius 0.33, width 0.22, rest 0.35, travel 0.2, 35 kN/m, 3.5 kN s/m, inertia
 * 1.2, grip 1, never breaks, steel, group and tag 0 */
void svxc_wheel_defaults(svxc_wheel_desc* d);
uint32_t svxc_add_wheel(svxc_world* w, const svxc_wheel_desc* d); /* its id, or 0: refused */
int svxc_remove_wheel(svxc_world* w, uint32_t id);
/* drive: torque on its spin (N m, forward positive); brake: the most braking torque (N m, >= 0);
 * steer: rad about its suspension's axis (left positive). They hold until changed. */
int svxc_set_wheel_input(svxc_world* w, uint32_t id, double drive, double brake, double steer);
typedef struct svxc_wheel_state {
  int64_t piece;            /* its carrier now (0: not a piece yet) */
  double mount[3], centre[3];
  double rot[4];            /* x the way it rolls, y its axle (steered), z up; spun by its angle about y */
  double radius, width, length, compression; /* m; 0 at full droop .. 1 at the bump stop */
  double steer, spin, angle; /* rad, rad/s (rolling forward: positive), rad */
  double drive, brake;      /* the input now */
  int contact;              /* 1: on the ground */
  double point[3], normal[3];
  int64_t ground_piece;     /* the piece it stands on (0: a grid, or nothing) */
  int material;             /* the surface's (-1: none) */
  double load;              /* N: its suspension's force */
  double force[3];          /* N: what the carrier received through it */
  double slip_long, slip_lat; /* m/s: the tyre sliding over the ground (skids, smoke) */
  uint32_t group, tag;
} svxc_wheel_state;
int svxc_wheel(svxc_world* w, uint32_t id, svxc_wheel_state* out); /* 1, or 0: none */
int svxc_wheels(svxc_world* w, uint32_t* out, int max);             /* ids (ascending) into out: their count */
/* The fastest a piece moves (m/s; 0: the world's limit): a vehicle goes faster than rubble may. */
int svxc_set_piece_max_speed(svxc_world* w, int64_t piece, double max_speed);

/* ---- commands (carve / blast: the next tick; edits: now) */

void svxc_carve(svxc_world* w, double x, double y, double z, double radius);
/* An impact of this energy (J: a bullet): removes what its energy density penetrates
 * (svxc_material.penetration). */
void svxc_shoot(svxc_world* w, double x, double y, double z, double radius, double energy);
void svxc_blast(svxc_world* w, double x, double y, double z, double radius, double energy);
enum { SVXC_EDIT_UNTRACKED = 1, SVXC_EDIT_ISOLATED = 2 };
/* n voxels: positions xyz (3 each) and values; returns the number changed. */
int svxc_set_voxels(svxc_world* w, const int32_t* xyz, const uint8_t* values, int n, unsigned flags);
int svxc_apply_impulse(svxc_world* w, int64_t piece, const double point[3], const double impulse[3]);
int svxc_remove_piece(svxc_world* w, int64_t piece);
/* A piece the host keeps: never culled (a joint's pieces are kept anyway); its parts keep it. */
int svxc_set_piece_keep(svxc_world* w, int64_t piece, int keep);
void svxc_tick(svxc_world* w);
double svxc_time(svxc_world* w); /* the world's clock: s simulated since the level loaded (paused ticks do not count) */

/* ---- extension points (docs/CORE.md §5): layers, damage, loads, piece forces, systems */

enum { SVXC_DAMAGE_LAYER = 0 };
/* A named byte channel per voxel; persistent: saved in deltas and archived with its chunk; bind:
 * what its values belong to (SVXC_BIND_SOLID: cleared when the voxel goes or is replaced;
 * SVXC_BIND_AIR: cleared when a solid takes the place; SVXC_BIND_PLACE: kept). Returns its
 * index (the existing one for the same layer added before), -1 when full (8 layers) or when a
 * different layer has the name. */
enum { SVXC_BIND_PLACE = 0, SVXC_BIND_SOLID = 1, SVXC_BIND_AIR = 2 };
int svxc_add_layer(svxc_world* w, const char* name, int persistent, int bind);
int svxc_layer_index(svxc_world* w, const char* name); /* -1: none */
/* n values at voxels xyz (3 each); returns the number changed. The damage layer (0 intact ..
 * 255 no strength left) takes the strength of the bond sections it is in. */
int svxc_set_layer(svxc_world* w, int layer, const int32_t* xyz, const uint8_t* values, int n);
uint8_t svxc_layer(svxc_world* w, int layer, int x, int y, int z);
/* A chunk's values of a layer (32768 bytes, index (x * 32 + y) * 32 + z): 1 if it holds any,
 * 0 if none (out zeroed). */
int svxc_chunk_layer(svxc_world* w, int layer, int cx, int cy, int cz, uint8_t* out);
/* The chunks whose values of a layer changed since the last poll (chunk xyz, 3 ints each,
 * valid until the next poll): count. */
int svxc_poll_layer_changes(svxc_world* w, int layer, const int32_t** chunks);
/* A piece's layer values at its shape voxels (the coordinates of svxc_piece_voxels). */
int svxc_set_piece_layer(svxc_world* w, int64_t piece, int layer, const int32_t* xyz, const uint8_t* values, int n);
uint8_t svxc_piece_layer(svxc_world* w, int64_t piece, int layer, int x, int y, int z);
/* Removes n shape voxels of a piece (burnt out, melted): its remains become new pieces. */
int svxc_remove_piece_voxels(svxc_world* w, int64_t piece, const int32_t* xyz, int n, int dust);
/* Forces (N, 3 each) on voxels of the static world, by group: replaces the group's loads
 * (n = 0 removes them; n > 0 without arrays changes nothing). Structures under new or changed
 * loads are solved again. */
void svxc_set_loads(svxc_world* w, uint64_t group, const int32_t* xyz, const double* forces, int n);
/* A force (N) at a world point on a piece during the next tick (it does not wake it). */
void svxc_apply_force(svxc_world* w, int64_t piece, const double point[3], const double force[3]);
void svxc_wake_piece(svxc_world* w, int64_t piece);
/* A system of the host's, stepped at the end of every tick (after the mechanics), in the order
 * added: its own physics on the world through this API. */
typedef void (*svxc_step_fn)(void* user, svxc_world* w, double dt);
void svxc_add_system(svxc_world* w, const char* name, svxc_step_fn step, void* user);
/* The same with all of a system's callbacks (any but step may be NULL): on_load (the world
 * loaded a new grid), on_chunks (kind SVXC_CHUNKS_*: chunks generated, evicted, or whose
 * voxels changed this tick; xyz, 3 ints each), memory_bytes and state_hash (reported in
 * svxc_get_memory and mixed into svxc_session_hash). A system must not tick, load or destroy
 * the world from its callbacks (tick and load are refused). Returns 1 if added. */
enum { SVXC_CHUNKS_GENERATED = 0, SVXC_CHUNKS_EVICTED = 1, SVXC_CHUNKS_CHANGED = 2 };
typedef struct svxc_system {
  const char* name;
  void* user;
  void (*step)(void* user, svxc_world* w, double dt);
  void (*on_load)(void* user, svxc_world* w);
  void (*on_chunks)(void* user, svxc_world* w, int kind, const int32_t* chunks, int n);
  int64_t (*memory_bytes)(void* user);
  uint64_t (*state_hash)(void* user);
} svxc_system;
int svxc_add_system_ex(svxc_world* w, const svxc_system* s);

/* ---- output */

enum {
  SVXC_PIECE_ADDED = 0,
  SVXC_PIECE_REMOVED,
  SVXC_CRACK,
  SVXC_IMPACT,
  SVXC_DUST,
  SVXC_FORGOTTEN,
  SVXC_GRID_ADDED,
  SVXC_GRID_REMOVED,
  SVXC_GRID_MOVED,
  SVXC_JOINT_BROKEN,
  SVXC_WHEEL_DETACHED,
  SVXC_PIECE_RESHAPED
};
enum { SVXC_END_SPLIT = 0, SVXC_END_CULLED, SVXC_END_OUT_OF_WORLD, SVXC_END_REMOVED, SVXC_END_UNLOADED };
typedef struct svxc_event {
  int kind;       /* SVXC_PIECE_ADDED, ... */
  int end;        /* PIECE_REMOVED: SVXC_END_* */
  int64_t id;     /* the piece */
  int64_t parent; /* PIECE_ADDED: the piece it broke from (0: the static world) */
                  /* FORGOTTEN: id is the region, pos its centre, voxels its chunks */
                  /* GRID_ADDED, GRID_REMOVED, GRID_MOVED: id is the grid, pos its origin, rot its turn */
                  /* JOINT_BROKEN: id is the joint, pos where, strength the force it carried (0: it lost its hold) */
                  /* WHEEL_DETACHED: id is the wheel (gone), parent its carrier, pos and vel its centre, normal
                     its axle, voxels the wheel piece it became (its id; 0: none), strength its force (0: its mount went) */
                  /* PIECE_RESHAPED: id is a piece whose voxels changed in place (crumpled): same id and pose; mesh it again */
  double pos[3], vel[3], ang[3], normal[3];
  double rot[4];  /* x, y, z, w */
  double radius, strength; /* CRACK: utilization; IMPACT: energy (J); DUST: 1 crushed, 0 a shard */
  int voxels;
  int material;   /* DUST: its material (-1: unknown) - glass shattering, a wall's brick dust */
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
 * (index (x * dim1 + y) * dim2 + z). Returns the voxel bytes, or NULL. (Its first shape: a piece
 * made of several grids has a shape per grid, below.) */
const uint8_t* svxc_piece_voxels(svxc_world* w, int64_t id, int lo[3], int dim[3]);
/* A piece's shapes: one per grid its voxels came from. Shape k's voxels (as svxc_piece_voxels),
 * placed in the piece's shape frame: its voxel p is at off + rot (h p) there (the first shape:
 * the identity); grid: the id of the grid it came from. */
int svxc_piece_shape_count(svxc_world* w, int64_t id);
const uint8_t* svxc_piece_shape(svxc_world* w, int64_t id, int shape, int lo[3], int dim[3], double off[3], double rot[4], uint32_t* grid);

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
  int voxel[3];  /* the grid's voxel, or the piece's shape voxel */
  int64_t piece; /* 0: a grid */
  uint32_t grid; /* (piece 0) the grid hit */
  int shape;     /* (a piece) its shape hit */
} svxc_hit;
svxc_hit svxc_raycast(svxc_world* w, const double origin[3], const double dir[3], double max_dist);
/* Moves the box [mn, mx] by move as far as the world's voxels (grids and pieces) let it (x, then
 * y, then z); out: the move made (3) and on_ground (1). */
void svxc_collide(svxc_world* w, const double mn[3], const double mx[3], const double move[3], double out[4]);
/* How far the box [mn, mx] moves along move (any direction) before it touches a grid's or a
 * piece's voxels: out[0] the free fraction (1: nothing), out[1..3] the touched surface's normal,
 * out[4] its grid id (0 for a piece); returns 1 if it touches. */
int svxc_sweep(svxc_world* w, const double mn[3], const double mx[3], const double move[3], double out[5]);
/* The same, with what a controller riding machines needs: what it stands on (a grid, or a piece:
 * a lift's car, a turntable) and its velocity under the box (add ground_velocity x dt to the next
 * move); the touched surface and its velocity. */
typedef struct svxc_collision {
  double move[3];
  int on_ground;
  uint32_t ground;      /* the grid stood on (ground_piece 0) ... */
  int64_t ground_piece; /* ... or the piece */
  double ground_velocity[3];
} svxc_collision;
void svxc_collide_ex(svxc_world* w, const double mn[3], const double mx[3], const double move[3], svxc_collision* out);
typedef struct svxc_sweep_hit {
  int hit;
  double t; /* the free fraction (1: nothing) */
  double normal[3];
  uint32_t grid; /* the grid touched (piece 0) ... */
  int64_t piece; /* ... or the piece */
  double velocity[3];
} svxc_sweep_hit;
int svxc_sweep_ex(svxc_world* w, const double mn[3], const double mx[3], const double move[3], svxc_sweep_hit* out);
/* Whether the box overlaps a grid's or a piece's voxels (1), and how far it must rise to overlap
 * nothing (0: it does not; -1: not within max_rise): a rider lifted out of a rising lift's car. */
int svxc_overlaps(svxc_world* w, const double mn[3], const double mx[3]);
double svxc_depenetrate(svxc_world* w, const double mn[3], const double mx[3], double max_rise);

typedef struct svxc_stats {
  double tick_ms, structural_ms, rigid_ms, stream_ms, memory_mb;
  int64_t ticks, voxels, structures, pieces, awake, contacts;
  int64_t bonds_broken, detached_pieces, pulverized_voxels, resident_chunks;
  int64_t archived_chunks, forgotten_regions, forgotten_chunks, culled_pieces, dropped_events;
  double archive_used_mb, archive_capacity_mb;
  int64_t grids; /* oriented grids */
} svxc_stats;
void svxc_get_stats(svxc_world* w, svxc_stats* out);
/* What the world holds, by kind (bytes): see svx::MemoryReport. */
typedef struct svxc_memory {
  int64_t grid, fragments, structures, pieces, archive, caches, queues, total;
  int64_t systems; /* (in total) */
} svxc_memory;
void svxc_get_memory(svxc_world* w, svxc_memory* out);
uint64_t svxc_state_hash(svxc_world* w);   /* voxels and broken bonds */
uint64_t svxc_session_hash(svxc_world* w); /* + the pieces' poses (determinism checks) */

#ifdef __cplusplus
}
#endif

#endif /* SVX_CORE_H */
