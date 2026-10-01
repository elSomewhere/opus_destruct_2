// structvox — the destruction world: the public API of the physics core (docs/CORE.md).
//
// A World owns voxel grids (world/grid.hpp) and the rigid pieces that broke off them, and runs
// the fragment-graph mechanics:
//   - Grids: the world grid (id 0: its axes the world's; loaded or streamed) and oriented grids,
//     each a voxel lattice of the same voxel size placed with a frame of its own (a building at an
//     angle, a diagonal brace, a tilted slab: docs/GRIDS.md). Where the voxels of two grids meet,
//     they are bonded (junctions), so structures and pieces span grids.
//   - The free (non-anchored) voxels are grouped into fragments (frag/fragments.hpp): pre-scored
//     rubble pieces. Anchored voxels are supports (bedrock, foundations, a level's movers).
//   - A static structure is a connected set of fragments that reaches a support. It is extracted
//     when something changes it (a carve, a blast, an edit, new loads) and its equilibrium
//     K u = f (gravity, contact forces of pieces, blast loads) is solved over ticks under a work
//     budget. Converged, its bonds are judged; the worst overloaded ones break (a dynamic
//     increase factor on sudden changes), parts that lose their supports leave the grid as rigid
//     pieces, and the structure is solved again: collapses unfold over ticks.
//   - Pieces keep their fragments and bonds. After each contact solve their stress is checked
//     under contact forces and inertia; a piece whose bonds break splits, and the substep is
//     solved again with the parts. Crushed material turns to dust.
//   - Contact forces of pieces on the world load the structures under them: falling floors can
//     break the floors they land on.
//
// The world knows nothing about rendering, players or levels: a host (a game, a tool) feeds it
// voxels (load, streaming, edits), commands (carve, blast, impulses) and a focus (where the
// streamed world must be resident), steps it, and reads back what changed: events (pieces added
// and removed, cracks, impacts, dust), piece poses and shapes, and the chunks whose voxels
// changed. Materials are a process-wide registry (material/material.hpp).
//
// Deterministic: fixed tick, commands in call order, structures and pieces in id order, work
// budgets counted in solver operations (never wall-clock time), results bit-identical on any
// thread count (and between native and WASM builds).
//
// Threading: a World is not thread-safe; call it from one thread. It runs its own work on the
// shared pool of base/parallel.hpp.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "svx/base/vec.hpp"
#include "svx/frag/fragments.hpp"
#include "svx/phys/rigid.hpp"
#include "svx/world/articulation.hpp"
#include "svx/world/grid.hpp"
#include "svx/world/joint_desc.hpp"
#include "svx/world/source.hpp"

namespace svx {

// (GridId, kWorldGrid, JointId, JointAnchor, JointDesc: svx/world/joint_desc.hpp)
// Where a grid is: its voxel p (integer coordinates in the grid) is centred at
// origin + rot (h p) in the world.
struct GridFrame {
  V3 origin;
  Quat rot;
};

// A grid to add (World::add_grid).
struct GridDesc {
  GridFrame frame;
  // Its voxel size (m; 0: the world's). A finer grid has finer surfaces; its rubble (fragments)
  // keeps the world grid's size in metres, so a structure breaks the same way at any resolution.
  f64 voxel_size = 0.0;
  // Where grids overlap, the one of higher priority keeps its voxels and the other's there are
  // removed (a member cast into another displaces what it takes the place of); ties: the grid
  // added later wins, and the world grid (priority 0, id 0) loses to every grid of priority >= 0.
  // The owner's faces measure the interface (junctions).
  i32 priority = 0;
  // Part of the level (not saved in deltas: a host that loads the level adds its grids again, in
  // the same order, before load_delta; their changes are saved like the world grid's); else a
  // change of this session, saved whole.
  bool base = true;
};

struct JointState {
  JointType type = JointType::Ball;
  V3 a, b;          // its ends in the world now
  V3 force, torque; // what b received through it in the last substep (a: the opposite; N, N m)
  f64 value = 0.0;  // (hinge) b's turn from its start (rad); (slider) b's move (m); (distance) the ends' distance (m)
  i64 piece_a = 0, piece_b = 0;  // the pieces its ends are on now (0: a grid, the world)
  bool latched = false;          // (hinge) held shut by its latch still
};

// A wheel now (World::wheel; docs/MOTION.md §7).
struct WheelState {
  i64 piece = 0;        // its carrier now, the piece it hangs from (0: not a piece yet - a grid dropped in comes loose at the next tick)
  V3 mount;             // the top of its suspension (world)
  V3 centre;            // its centre (world)
  Quat rot;             // its orientation (world): x the way it rolls, y its axle (steered), z up; spun by its angle about y
  f64 radius = 0.0, width = 0.0;
  f64 length = 0.0;     // m: the suspension now
  f64 compression = 0.0;  // 0 at full droop .. 1 at the bump stop
  f64 steer = 0.0, spin = 0.0, angle = 0.0;  // rad, rad/s (rolling forward: positive), rad
  f64 drive = 0.0, brake = 0.0;              // the input now (N m)
  bool contact = false;
  V3 point, normal;     // the contact (world)
  i64 ground_piece = 0; // the piece it stands on (0: a grid, or nothing)
  int material = -1;    // the surface's material (-1: none)
  f64 load = 0.0;       // N: the suspension's force
  V3 force;             // N: what the carrier received through it
  f64 slip_long = 0.0, slip_lat = 0.0;  // m/s: the tyre sliding over the ground (skids, smoke)
  u32 group = 0, tag = 0;                // the host's (WheelDesc)
};

// A chunk of a grid (World::take_changed_grid_chunks).
struct GridChunk {
  GridId grid = 0;
  IVec3 chunk{0, 0, 0};
};

// Runtime knobs: may change between ticks.
struct WorldParams {
  f64 fragility = 1.0;  // divides every bond strength (> 1: more collapse)
  f64 impact = 1.0;     // scales contact loads (impulse / impact duration): impact severity
  f64 dif = 1.5;        // dynamic increase factor of sudden load changes (1 = static)
  bool paused = false;  // tick() streams only (no commands, no motion)
  bool debug_fields = false;  // judged structures report their chunks changed (debug_field views refresh)
};

// Memory budgets (MB) of what a world derives and keeps around; checked every half second.
// What they bound is either rebuilt on demand (fragment caches, structures, warm starts) or
// removed as the least important (the smallest pieces, sleeping first; cosmetic events), so a
// session never grows past them, however long it runs. (The grid is bounded by the level, or by
// the streaming radii; the change archive by StreamConfig::archive_mb.)
struct MemoryBudget {
  f64 fragment_cache_mb = 128.0;  // fragment caches of chunks no structure holds: least recently used dropped beyond
  f64 structure_mb = 256.0;       // registered structures: idle ones dropped beyond, the longest idle first
  f64 piece_mb = 256.0;           // rigid pieces: beyond, awake pieces' fracture solvers are released, then (as beyond max_bodies) the smallest pieces culled, sleeping first
  f64 cache_mb = 48.0;            // warm starts and reference loads (beyond: those of registered structures only)
  i32 max_events = 65536;         // events not taken (beyond: the oldest cosmetic ones are dropped)
};

// Setup: set before load() (the rigid and budget knobs may also change between ticks).
struct WorldConfig {
  f64 dt = 1.0 / 60.0;
  FragParams frag{};
  RigidParams rigid{};
  // static structures
  f64 stress_rtol = 3e-3;          // converged: relative residual of K u = f
  i64 stress_work = 4000000;       // solver block operations per tick (all structures)
  i32 structure_max_nodes = 60000; // extraction bound (beyond: the frontier is held fixed)
  // Resolution: a structure or piece of more fragments than this is solved on clusters of
  // fragments (1 m cells; 2 m beyond 6x): its cracks follow cluster seams, its pieces break
  // finer once they are smaller (docs/V2_DESIGN.md §2).
  i32 cluster_nodes = 2500;
  i32 body_cluster_nodes = 400;    // pieces (checked many times as they break): 1 m clusters beyond this, 2 m beyond 3x
  f64 structure_max_radius = 60.0; // m from the seed (beyond: held fixed)
  i32 max_breaks_per_round = 256;
  f64 break_band = 0.85;           // a round breaks the bonds with phi >= max(1, band x max phi)
  i32 max_rounds = 400;            // break rounds of one structure before it is left alone
  i32 solve_restarts = 60;         // solves restarted in a row unconverged before it is left alone
  i32 idle_drop_ticks = 1800;      // idle structures without loads are dropped after this
  f64 load_trigger = 0.25;         // re-solve when a node's external load changes by this x its weight ...
  f64 load_trigger_abs = 800.0;    // ... plus this (N)
  // A load that creeps on a large structure (something rolling over a bridge: its wheels cross a
  // fragment every few ticks) is solved again at most every this many ticks; a change of 4 x the
  // trigger, an impact, a structure breaking or one of fewer nodes (cheap to solve) at once.
  // 0: at once, always (the structural reference's way: docs/BASELINE.md).
  i32 load_trigger_gap = 6;
  i32 load_trigger_gap_nodes = 400;
  f64 dead_load_ema = 0.25;        // smoothing of resting contact loads per tick
  // pieces
  i32 max_bodies = 3000;           // beyond: the smallest sleeping pieces are culled (PieceEnd::Culled)
  i32 body_stress_maxit = 60;      // PCG iterations of a piece's stress solve
  f64 body_stress_rtol = 1e-2;
  f64 body_trigger = 1.8;          // contact force sum over weight that triggers a piece's stress check
  f64 body_impact_speed = 1.5;     // ... when a contact closes faster than this (m/s): a collision
  f64 small_impact_speed = 4.0;    // pieces lighter than small_piece_mass need up to this
  f64 small_piece_mass = 1500.0;   // kg
  i32 min_fracture_frags = 8;      // smaller pieces never break further (the smallest rubble)
  i32 big_piece_voxels = 20000;    // checked with all threads (smaller pieces: concurrently)
  i32 impact_rounds = 8;           // break rounds of an impact (it stops once the piece comes apart)
  f64 impact_chip_fraction = 0.04; // parts lighter than this x the piece are crushed chips (they pass the load on)
  f64 impact_round_fraction = 0.25;// an impact round breaks at least this fraction of the overloaded bonds (worst first)
  f64 crush_energy = 20.0;         // crushing a bond costs this x its fracture energy
  bool pulverize = true;           // crushed fragments turn to dust (the space they held opens)
  // plastic hinges (ductile members failing in bending: steel sections, rebar): the part beyond
  // turns about the section, holding hinge_shape x its elastic moment, and tears after
  // hinge_rotation (rad)
  bool plastic_hinges = true;
  f64 hinge_rotation = 0.35;
  f64 hinge_shape = 1.3;
  bool spread_contacts = true;     // stress checks share a piece's contact force over its contacts (least squares)
  // What changed since the structural reference (docs/BASELINE.md), each a switch - on, the engine
  // as it is; all off (with plastic_hinges, rigid.piece_ccd, rigid.busy_hold and
  // rigid.warm_to_step, load_trigger_gap 0 and evict_scan_ticks 1), the reference bit for bit:
  //   impact_penetration: impacts remove material by energy density against its penetration
  //     resistance, and a cut (a carve of no energy) cuts steel and bars too; off: carves and
  //     craters never remove ductile material.
  //   restart_diverging_solves: a structure solve whose residual diverges (many breaks at once)
  //     rebuilds its preconditioner at once; off: only after 120 iterations.
  //   jointed_keep_identity: a piece held by joints keeps its id through a split, on its largest
  //     part (so do pieces on wheels, and kept ones, always); off: its parts are new pieces.
  //   spread_per_partner: a piece's stress check spreads each partner's contact forces on their
  //     own (a block on a beam and its supports keep their places); off: all of them pooled.
  //   design_in_place: a streamed structure touched for the first time is designed in place
  //     (its members' new strengths written into it, its solve going on from the design's); off:
  //     designed, then extracted again (a new structure, solved afresh: a structure's work more).
  //   patch_cut_structures: a structure cut (voxels carved, burnt, crushed out of it) is patched
  //     where it was cut - a few chunks' work; off: extracted again whole about the cut.
  //   evict_scan_ticks (streaming): the scan for what to evict - it walks every resident chunk -
  //     runs every this many ticks, at once when a focus point moved 8 m since, and every 30
  //     ticks; 1: every tick.
  //   release_solvers: a piece's fracture solver (its stiffness matrix and multigrid: most of a
  //     piece's memory) is released when the piece falls asleep - and beyond the pieces' memory
  //     budget, the awake ones' too, the largest first, before any piece is culled - and assembled
  //     afresh at its next stress check; off: kept while it lives, and the budget culls pieces.
  //   recheck_vacated: where material leaves a grid (a piece comes loose, a shard turns to dust),
  //     what was next to it - edge to edge and corner to corner too - is checked for support again:
  //     what held on to nothing else falls; off: only what a carve or a blast cut is checked.
  //   true_solve_work: the structures' solver work is counted at its cost against stress_work - a
  //     multigrid's coarsest level solved densely by its blocks (the reference counted it by its
  //     unknowns, 36 times over: a large structure got an iteration or two a tick), an assembly
  //     by the products and the factorization that build its multigrid; off: as the reference.
  //   rebuild_stale_only: a structure solve slow to converge, or whose residual grows, has its
  //     preconditioner rebuilt only if that makes another (a stale one: bonds broke since it was
  //     built; a small structure's block-Jacobi one) - else its solve restarts on it - and a
  //     diverged iterate is never kept as the next solve's start; off: rebuilt in any case (a
  //     large structure's assembly every tick, the solve never getting anywhere), the iterate kept.
  //   coarsen_dense_levels: a stress solve's multigrid level grown dense (more than 80 blocks a
  //     row: a large damaged structure's third or fourth) is coarsened once more, by its
  //     aggregates' rigid motions, and that solved densely; off: it is the coarsest - factored if
  //     it is small (an assembly's most expensive part, at a hundred nodes), else smoothed: eight
  //     sweeps of its dense rows a cycle, and slow to converge.
  //   reaggregate_levels: a stress solve's multigrid level whose aggregates would hold fewer than
  //     two nodes on average (its couplings mostly under the strength threshold: a large irregular
  //     structure's second level) is aggregated again at half the threshold, up to three times;
  //     off: aggregated once - it barely coarsens, and its smoothed coarse level fills in (a
  //     damaged building's: 180 blocks a row, most of each assembly's and each cycle's work).
  //   shards_hold_together: a fragment a blast or a punch tears out whole has its faces with the
  //     rest torn, and keeps its own: a later cut breaks the shard as a piece; off: every face of
  //     its voxels is torn - a shard of loose voxels, all dust at the next cut or shot.
  //   cluster_cubes (OFF by default - a choice for the quality pass, not a fix to take silently):
  //     a structure of more than cluster_nodes fragments has them clustered in 1 m (2 m) cubes of
  //     its chunks, as was meant; off: the reference's cell key, which loses the cell along x -
  //     clusters a chunk (4 m) long in x: cracks along x only at chunk edges, a building's
  //     strength by its orientation. On, large structures fail differently (the tower's side
  //     blast brings it down: 7966 bonds broken, not 32) and a damaged building's solve costs
  //     some 2.5 times as much.
  //   fair_solve_order: the structures solving share stress_work from where it ran out the tick
  //     before - the first one it did not reach goes first - so a large structure slow to converge
  //     cannot hold the others back (a demolition's remnants waited 17 ticks to fall); off: by id
  //     every tick, the same structures first. (A tick that reached them all goes by id.)
  //   rigid.busy_hold, rigid.warm_to_step (RigidParams): busy mode decided once a tick, and held
  //     until a collapse is well under its thresholds; the solver's warm starts scaled to the
  //     substep's length; off: decided every substep, warm starts as they were.
  bool impact_penetration = true;
  bool restart_diverging_solves = true;
  bool jointed_keep_identity = true;
  bool spread_per_partner = true;
  bool design_in_place = true;
  bool patch_cut_structures = true;
  bool release_solvers = true;
  bool recheck_vacated = true;
  bool true_solve_work = true;
  bool rebuild_stale_only = true;
  bool coarsen_dense_levels = true;
  bool reaggregate_levels = true;
  bool shards_hold_together = true;
  bool cluster_cubes = false;  // (off: see above - a choice of the structural model's resolution)
  bool fair_solve_order = true;
  i32 evict_scan_ticks = 10;
  f64 fracture_energy = 1.0;       // x the materials' fracture energies (what impacts pay for cracks)
  f64 impact_wave_speed = 400.0;   // m/s: an impact loads a piece over its length / this (crushing slows the wave)
  i32 body_check_ticks = 12;       // steady contact: re-check every so many substeps
  i32 crumple_check_gap = 8;       // a piece crumpling (in place): collisions re-check it every so many substeps (else 2)
  i32 rollback_part_voxels = 500;  // a part at least this large coming apart re-solves the contact step
  i32 min_body_voxels = 16;        // smaller pieces (breaking off, or coming loose) turn to dust, not rigid pieces
  // blasts
  f64 blast_shatter = 1.7;         // shatter radius / crater radius: fragments come loose
  f64 blast_reach = 3.5;           // load radius / crater radius
  f64 blast_kinetic = 0.06;        // fraction of the blast energy given to the shattered pieces
  f64 blast_max_speed = 28.0;
  f64 max_event_radius = 16.0;     // m: larger carves and blasts are clamped to it
  // design pass (bake / first touch of streamed chunks): members above this self-weight
  // utilization are strengthened
  f64 design_utilization = 0.45;
  // junctions (bonds between grids, docs/GRIDS.md): a face bonds to another grid's voxels where
  // its samples (junction_samples^2 per face), pushed out by junction_reach voxels of the other
  // grid (whose surface is up to half of its voxel away where this grid displaced it), land in them
  i32 junction_samples = 3;
  f64 junction_reach = 0.5;
  // event budgets per tick (cosmetic events beyond them are dropped; piece events never are)
  i32 crack_events_per_tick = 24;  // cracks and dust
  i32 impact_events_per_tick = 6;  // heavy landings
  f64 impact_event_energy = 2e4;   // J dissipated by a landing to report it
  MemoryBudget memory{};
};

// Why a piece left the world (WorldEvent::PieceRemoved).
enum class PieceEnd : u8 {
  Split,       // it broke or changed shape (carved, crushed): its parts are new pieces (PieceAdded, parent = it)
  Culled,      // over max_bodies: the smallest sleeping pieces go first
  OutOfWorld,  // fell kill_depth below the world
  Removed,     // remove_piece(), load()
  Unloaded,    // asleep in chunks that left the resident area (streaming): rubble is not kept out of range
};

struct WorldEvent {
  enum class Kind : u8 {
    PieceAdded,    // id, parent (the piece it broke from; 0: the static world), pos (centre of mass), rot, vel, ang, voxels
    PieceRemoved,  // id, end, pos, rot (its last pose)
    Crack,         // pos, normal, strength (the bond's utilization when it broke, >= 1)
    Impact,        // pos, radius, strength (energy, J): blasts and heavy landings
    Dust,          // pos, vel, radius, voxels; strength 1: material crushed, 0: a shard too small to be a piece
    Forgotten,     // id: a region whose changes were forgotten (StreamConfig::archive_mb); pos: its centre; voxels: its chunks
    GridAdded,     // id: an oriented grid (a streamed one came, or load_delta made one); pos: its origin, rot
    GridRemoved,   // id: an oriented grid gone (removed, evicted with its home chunk, or by load)
    GridMoved,     // id: an oriented grid placed anew (set_grid_frame, load_delta); pos: its origin, rot (in the world)
    JointBroken,   // id: a joint that gave way (it is gone); pos: where; strength: the force it carried (N; 0: an end lost its hold)
    WheelDetached, // id: a wheel that came off (it is gone); parent: its carrier (the piece it hung from); pos, vel: its centre; normal: its axle; voxels: the wheel piece it became (its id; 0: none); strength: its force (N; 0: its mount was lost)
    PieceReshaped, // id: a piece whose voxels changed in place (crumpled, dented): same id, same pose; mesh it again
    ArticulationAdded,    // id: an articulation that came (back from the streaming archive, or with a loaded session): pos its first link; voxels its links
    ArticulationRemoved,  // id: an articulation gone; end: Removed (its host, load), Unloaded (archived out of range), OutOfWorld; pos its first link
  };
  Kind kind = Kind::Crack;
  PieceEnd end = PieceEnd::Split;
  i64 id = 0, parent = 0;
  V3 pos, vel, ang, normal{0, 0, 1};
  Quat rot;
  f64 radius = 0.0, strength = 0.0;
  i32 voxels = 0;
  i32 material = -1;  // (Dust) what it is made of (-1: unknown)
};

// A rigid piece's state. Its shape (voxels in the shape frame, phys/rigid.hpp) is placed at
// pos + rot (s - com): World::piece(id)->shape / ->com.
struct PieceState {
  i64 id = 0;
  V3 pos;       // centre of mass (world)
  Quat rot;     // shape frame -> world
  V3 vel, ang;  // linear, angular velocity (world)
  i32 voxels = 0;
  f64 mass = 0.0;
  bool asleep = false;
};

struct RayHit {
  bool hit = false;
  V3 pos, normal;
  f64 distance = 0.0;
  int material = -1;
  IVec3 voxel{0, 0, 0};  // the grid voxel hit (a static grid) or the shape voxel (a piece)
  i64 piece = 0;         // the rigid piece hit (0: a static grid)
  GridId grid = 0;       // the static grid hit (piece 0)
  i32 shape = 0;         // the piece's shape hit (piece != 0)
};

struct CollideResult {
  V3 move;  // the part of the requested move that is free
  bool on_ground = false;
  // (on_ground) what it stands on - a grid, or a piece (a lift's car, a turntable, a crate) - and
  // its velocity under the box's base (a grid's: zero). A controller riding it adds
  // ground_velocity x dt to its next move.
  GridId ground = 0;
  i64 ground_piece = 0;
  V3 ground_velocity;
};

// World::sweep: how far a box moves along a direction before it touches a grid or a piece.
struct SweepHit {
  bool hit = false;
  f64 t = 1.0;     // the fraction of the move that is free
  V3 normal;       // the touched surface's normal (against the move)
  GridId grid = 0;  // the grid touched (piece 0) ...
  i64 piece = 0;    // ... or the piece
  V3 velocity;     // the touched surface's velocity there (a piece's; a grid's: zero)
};

// One voxel write of World::set_voxels.
struct VoxelEdit {
  IVec3 p{0, 0, 0};
  Vox v = kAir;
};

enum EditFlags : u32 {
  kEditUntracked = 1u << 0,  // not part of the persistence delta (a level's movers, which its host rebuilds on load)
  kEditIsolated = 1u << 1,   // the written solid voxels bond to nothing (a level's movers: doors, lifts)
};

// Diagnostic fields per voxel (World::debug_field).
enum class DebugField : u8 {
  Utilization,  // the worst bond utilization of the voxel's structure node at its last judge (0..255 = 0..1)
  Fragment,     // a colour index of the voxel's fragment (1..255; 0 = none)
};

struct WorldStats {
  f64 tick_ms = 0.0, structural_ms = 0.0, event_ms = 0.0, rigid_ms = 0.0;
  f64 loads_ms = 0.0;    // (pieces' and blasts' loads on the structures)
  f64 systems_ms = 0.0;  // (WorldSystem::step of every system)
  f64 upkeep_ms = 0.0;   // (the rest: budgets, piece changes and announcements)
  i64 ticks = 0, events = 0;
  i64 voxels = 0, chunks = 0;
  f64 memory_mb = 0.0;
  // structures
  i32 structures = 0, solving = 0;        // registered / being solved now
  i64 solve_nodes = 0;                    // nodes of the structures being solved
  i64 extractions = 0, extracted_nodes = 0, solves = 0, pcg_iters = 0;
  i64 bonds_broken = 0, detached_voxels = 0, detached_pieces = 0;
  i64 solves_abandoned = 0;               // structures left alone after solve_restarts unconverged restarts
  f64 max_utilization = 0.0;              // of the last judged round
  // grids
  i32 grids = 0;                          // oriented grids
  // pieces
  i32 bodies = 0, awake = 0, contacts = 0;
  i32 substeps = 0;                          // the last tick's (1 busy; RigidParams::mixed_substeps)
  i64 body_checks = 0, body_splits = 0, impacts = 0;
  i64 impact_breaks = 0, steady_breaks = 0;  // bonds broken in pieces by collisions / by resting loads
  i64 pulverized_voxels = 0;                 // crushed to dust
  i64 chip_releases = 0;                     // pieces going on without the chips that broke off with their contacts
  i64 reshapes = 0, punches = 0;             // crumpling: pieces folded in place; walls a crumpling piece broke through
  i64 plastic_hinges = 0;                    // ductile sections that gave way in bending as hinges
  i64 mode_breaks[4] = {0, 0, 0, 0};         // pieces' bonds broken by mode (none, tension, crush, shear)
  // streaming
  i64 resident_chunks = 0, archived_chunks = 0, generated_total = 0, evicted_total = 0, budget_evicted = 0;
  f64 stream_ms = 0.0;
  f64 archive_used_mb = 0.0, archive_capacity_mb = 0.0;
  i64 forgotten_regions = 0, forgotten_chunks = 0;  // changes forgotten (the archive full, or forget_after_s)
  i64 archived_pieces = 0, forgotten_pieces = 0;    // pieces out of range, in the archive now; gone with their regions (totals)
  i64 archived_articulations = 0, forgotten_articulations = 0;  // (the same for articulations)
  // memory budgets (MemoryBudget): what they removed
  i64 culled_pieces = 0, dropped_structures = 0, dropped_fragment_caches = 0, dropped_events = 0;
  i64 released_solvers = 0;  // awake pieces' fracture solvers released over the pieces' budget (release_solvers)
  // design (bake)
  f64 design_max_utilization = 0.0;
  i64 strengthened_voxels = 0, floating_voxels = 0;
  f64 bake_ms = 0.0;
};

// Memory a world holds, by kind (bytes; container capacities and node overheads).
struct MemoryReport {
  i64 grid = 0;        // voxel chunks: voxels, broken faces, design classes
  i64 fragments = 0;   // fragment caches of chunks (derived from the grid, rebuilt on demand)
  i64 structures = 0;  // registered structures: graphs, matrices, preconditioners
  i64 pieces = 0;      // rigid pieces: shapes, collision samples, bond graphs, contacts
  i64 archive = 0;     // changes of chunks that are not resident (streaming)
  i64 caches = 0;      // warm starts, reference loads, resting loads of sleeping pieces
  i64 queues = 0;      // output the host has not taken yet (events, changed / evicted chunks)
  i64 systems = 0;     // the systems' own state (WorldSystem::memory_bytes)
  i32 chunks = 0, fragment_chunks = 0, structure_count = 0, piece_count = 0, archived_chunks = 0;
  i64 total() const { return grid + fragments + structures + pieces + archive + caches + queues + systems; }
};

class World;

// A system stepped with the world: fire, fluids, weather, ... (svx_env, or a host's own). It
// reads and changes the world through World's public API; the world tells it what it could not
// see coming (loads, streaming, voxel changes). The core knows no system; systems may know each
// other (through their host). Its calls must not throw: the core is built without exceptions,
// so one escaping into it is not contained (a host catching it finds the world mid-tick).
class WorldSystem {
 public:
  virtual ~WorldSystem() = default;
  virtual const char* name() const = 0;
  virtual void attach(World& w) { (void)w; }  // added to w (register layers here)
  virtual void on_load(World& w) { (void)w; }  // w loaded a new grid (its state went with the old one)
  // Streaming: chunks generated (their layers as the source made them) / evicted (the system's
  // state there goes: a chunk's persistent layers come back with it).
  virtual void on_generated(World& w, const std::vector<u64>& chunks) { (void)w, (void)chunks; }
  virtual void on_evicted(World& w, const std::vector<u64>& chunks) { (void)w, (void)chunks; }
  // Voxels changed in these chunks this tick (carves, detachments, edits, generation).
  virtual void on_voxels_changed(World& w, const std::vector<u64>& chunks) { (void)w, (void)chunks; }
  // Once per tick, before the mechanics: what the system drives is set here (the controls of the
  // articulations it moves, and the state its host changed in them since its step).
  virtual void pre_step(World& w, f64 dt) { (void)w, (void)dt; }
  virtual void step(World& w, f64 dt) = 0;  // once per tick, after the mechanics
  virtual i64 memory_bytes() const { return 0; }
  virtual u64 state_hash() const { return 0; }  // (determinism checks: mixed into session_hash)
};

// A force on a voxel of the static world (World::set_loads): N, at the voxel's centre.
struct VoxelLoad {
  IVec3 voxel{0, 0, 0};
  V3 force;
  GridId grid = kWorldGrid;  // (the voxel's grid)
};

// One layer value write (World::set_layer, set_piece_layer).
struct LayerEdit {
  IVec3 p{0, 0, 0};
  u8 v = 0;
};

class World {
 public:
  World();
  ~World();
  // (a moved-from World may only be destroyed or assigned to; its systems go with the world)
  World(World&&) noexcept;
  World& operator=(World&&) noexcept;
  World(const World&) = delete;
  World& operator=(const World&) = delete;

  void configure(const WorldConfig& c);
  const WorldConfig& config() const;
  // Materials (svx/material/material.hpp): the world's own table, made from the process's
  // (default_materials) when the world is made. Changed before load(): what the world builds
  // from it (fragments, structures, pieces) keeps the properties it was built with.
  const MaterialTable& materials() const;
  bool register_material(const Material& m, MaterialId* id);
  void set_material(MaterialId id, const Material& m);
  void set_params(const WorldParams& p);
  const WorldParams& params() const;

  // ---- content
  // Replaces the world with grid g (the pieces are removed: PieceRemoved events; the oriented
  // grids too: GridRemoved; its chunks are reported changed). Changes from here on are tracked
  // for save_delta().
  void load(VoxelGrid&& g);
  // Design pass (bake): removes floating source pieces, then solves every structure under its
  // own weight and strengthens members above design_utilization (so a level stands as built).
  // Returns true when the whole world was designed (streamed worlds: chunks are designed when
  // first touched).
  bool bake(f64* ms = nullptr);
  struct DesignReport {
    f64 max_utilization = 0.0;
    i64 strengthened_voxels = 0;
    i64 floating_voxels = 0;
    f64 max_utilization_after = 0.0;
    i64 structures = 0, nodes = 0;
  };
  const DesignReport& design_report() const;
  // Streaming: from now on the world is generated by src around the focus points (chunks
  // within load_radius are resident; beyond evict_radius they are dropped, their changes
  // archived). Call after load() of an empty grid of the source's voxel size.
  void enable_streaming(std::shared_ptr<const ChunkSource> src, const StreamConfig& sc);
  bool streaming() const;
  const ChunkSource* source() const;
  // Where the streamed world must be resident (e.g. the players); the first call also makes
  // the chunks around them resident at once. Ignored when not streaming.
  void set_focus(const V3& p) { set_focus(std::vector<V3>{p}); }
  void set_focus(const std::vector<V3>& points);
  // Generates the chunks overlapping the voxel box [lo, hi) now (e.g. before placing a player).
  void ensure_resident(const IVec3& lo, const IVec3& hi);
  bool chunk_resident(const IVec3& chunk) const;

  // Persistence: the changes since load() as a binary delta against the regenerable base
  // world (a level file, a generator, and the level's oriented grids); load_delta() applies one
  // after load() and the level's grids (false: malformed, nothing applied). Untracked edits are
  // not part of it.
  std::vector<u8> save_delta() const;
  bool load_delta(const std::vector<u8>& bytes);
  bool modified() const;

  const VoxelGrid& grid() const { return *grid_; }
  f64 voxel_size() const { return grid_->h; }

  // ---- grids (docs/GRIDS.md)
  // Adds an oriented grid: `voxels` in the grid's own coordinates, placed as `d` says (GridDesc:
  // frame, voxel size, priority, base). It is structure like the world grid's voxels: anchored
  // voxels are supports, free ones form fragments and structures, and faces that meet other
  // grids' voxels bond to them (junctions); where it overlaps other grids, the lower priority's
  // voxels there are removed. A grid of free voxels touching nothing falls as a piece (a rotated
  // object dropped in). Returns its id (0: refused - a frame, voxel size or voxels out of range,
  // or from inside a tick).
  GridId add_grid(const GridDesc& d, VoxelGrid&& voxels);
  GridId add_grid(const GridFrame& frame, VoxelGrid&& voxels, bool base = true) {
    GridDesc d;
    d.frame = frame;
    d.base = base;
    return add_grid(d, std::move(voxels));
  }
  // A grid of this session (not base) comes loose now, whole: all its free voxels one piece, at
  // once - no structure is solved for it (an assembly dropped in: the wheels and joints on its
  // voxels go with it). Returns the piece (0: none - not such a grid, nothing free, a tick).
  i64 loosen_grid(GridId id);
  // Removes an oriented grid (its voxels; pieces that broke off it stay). GridRemoved.
  bool remove_grid(GridId id);
  // Places a grid anew (it keeps its voxels, changes and design): what it was bonded to lets go,
  // it bonds to what it meets where it is now, and where it overlaps other grids the lower
  // priority's voxels are removed. Saved in deltas. GridMoved. (Grids stand still: what moves
  // is pieces - a machine is pieces on driven joints.)
  bool set_grid_frame(GridId id, const GridFrame& frame);

  // ---- joints (docs/MOTION.md)
  // A joint holds two things together: pieces, grids' voxels (their structures take its load),
  // the world. A hinge on a door, a rope from a crane's jib to a wrecking ball, a lift's car on
  // its slider, a chain. A hinge's or a slider's drive moves it (a machine: its drive's program
  // runs on the world's clock). An end on a voxel follows it: into the piece it breaks off in,
  // into the part of a piece it stays with; it lets go when the voxel is gone, and the joint gives
  // way beyond its breaking strength (JointBroken).
  JointId add_joint(const JointDesc& d);  // 0: refused (an anchor not there, from inside a tick)
  bool remove_joint(JointId id);
  bool set_joint_drive(JointId id, const JointDrive& drive);
  bool set_joint_limits(JointId id, bool on, f64 lower, f64 upper);
  bool joint(JointId id, JointState* out) const;  // false: none (broken, removed)
  // The pieces joined to a piece by joints that hold (ascending): the parts still on it.
  std::vector<i64> joined_pieces(i64 piece) const;
  std::vector<JointId> joints() const;            // ascending ids (the host's: an articulation's own are in its state)

  // ---- wheels (docs/MOTION.md §7)
  // A wheel on a sprung suspension with a tyre, hung from a body, its carrier (a piece, or a grid
  // of free voxels that becomes one), solved with the contacts: whatever rolls - a vehicle, a
  // trolley, a machine's undercarriage - is a carrier on wheels, driven through them. Its forces
  // load what it stands on. It comes off (WheelDetached, a wheel piece) when its mount voxel is
  // gone or its force passes its breaking strength.
  WheelId add_wheel(const WheelDesc& d);  // 0: refused (its mount not there, from inside a tick)
  bool remove_wheel(WheelId id);
  // The host's input for it (until changed): drive torque (N m; negative: backwards), brake
  // torque (N m), steer (rad, the axle turned about the suspension's axis). Wakes its carrier.
  bool set_wheel_input(WheelId id, f64 drive, f64 brake, f64 steer);
  bool wheel(WheelId id, WheelState* out) const;  // false: none (it came off, was removed)
  std::vector<WheelId> wheels() const;           // ascending ids
  // A piece's speed limit (m/s; 0: rigid.max_speed): a vehicle's is higher than the rubble's. Its
  // parts keep it when it breaks.
  bool set_piece_max_speed(i64 piece, f64 max_speed);

  // ---- articulations (svx/world/articulation.hpp, docs/MOTION.md §6)
  // Bodies of links (rigid bodies of no voxels, colliding as spheres) held by joints with limits
  // and muscles, pulled by targets: a character's body, a robot, a rag doll. Solved with
  // everything else (fine-stepped on their own while they touch no awake piece).
  ArticulationId add_articulation(const ArticulationDesc& d);  // 0: refused (malformed, out of range, from inside a tick)
  bool remove_articulation(ArticulationId id);                 // (ArticulationRemoved, Removed)
  std::vector<ArticulationId> articulations() const;           // ascending ids
  // Its drive (muscles, targets, forces for one tick, flags), changed in place by its host between
  // ticks; valid until the articulation goes (nullptr: none).
  ArticulationControl* articulation_control(ArticulationId id);
  bool articulation_state(ArticulationId id, ArticulationState* out) const;
  const std::vector<u8>* articulation_data(ArticulationId id) const;  // its host data (nullptr: none)
  bool set_articulation_data(ArticulationId id, std::vector<u8> data);
  // Immediate edits (they wake it): a link placed and set moving (a body taking over from an
  // animation with its momentum); a velocity change of a link (a shove); an impulse at a point.
  bool set_link(ArticulationId id, u16 link, const V3& pos, const Quat& rot, const V3& vel, const V3& ang);
  bool add_link_velocity(ArticulationId id, u16 link, const V3& dv, const V3& dw);
  bool apply_link_impulse(ArticulationId id, u16 link, const V3& point, const V3& impulse);
  // A link lost (a limb shot off): it touches nothing any more and keeps mass_scale of its mass.
  bool lose_link(ArticulationId id, u16 link, f64 mass_scale);
  bool wake_articulation(ArticulationId id);
  bool articulation_asleep(ArticulationId id) const;  // all its links asleep (false: awake, or none)
  // A link's body id (0: none): its joints to pieces (JointAnchor::Kind::Link), queries.
  i64 link_body(ArticulationId id, u16 link) const;
  std::vector<GridId> grids() const;                  // the oriented grids, ascending ids
  const VoxelGrid* grid(GridId id) const;             // kWorldGrid: grid(); nullptr: none
  bool grid_frame(GridId id, GridFrame* out) const;   // in the world now (false: none)
  i32 grid_priority(GridId id) const;                 // (kWorldGrid: 0)
  // The oriented grids in the world grid's voxels: a voxel whose centre lies in a solid voxel of
  // one. For systems of the world's lattice (water flows around a turned wall, smoke is held by
  // it): per world chunk, kChunkVox bits (voxel i: bit i & 7 of byte i >> 3), null where no grid
  // reaches; valid until the grids there change or the world ticks again, whichever is first.
  const u8* grid_solids(const IVec3& world_chunk) const;
  // What grid_solids(world_chunk) holds, as a stamp that changes when it does (0: none).
  u64 grid_solids_stamp(const IVec3& world_chunk) const;
  // The oriented grid with a solid voxel at a world point, and that voxel.
  bool grid_voxel_at(const V3& world, GridId* grid, IVec3* voxel) const;
  bool grid_solid(const IVec3& world_voxel) const {
    if (*oriented_ == 0) return false;  // (no oriented grids: nothing to look up)
    const u8* b = grid_solids(chunk_of(world_voxel));
    const i32 i = chunk_index(world_voxel);
    return b && ((b[i >> 3] >> (i & 7)) & 1);
  }
  // A grid's point (its coordinates, metres: voxel p's centre is h p) in the world, and back.
  V3 grid_to_world(GridId id, const V3& lattice) const;
  V3 world_to_grid(GridId id, const V3& world) const;

  // ---- commands
  // Queued: they take effect in the next tick, in call order.
  void carve(const V3& pos, f64 radius);              // a cut: removes the voxels in a sphere (not indestructible materials)
  // An impact of this energy (J: a bullet) in a sphere: removes the voxels whose penetration
  // resistance (Material::penetration, J/m^3) its energy density exceeds - it holes sheet metal
  // and brick, not a steel section or the bars in a column.
  void shoot(const V3& pos, f64 radius, f64 energy);
  void blast(const V3& pos, f64 radius, f64 energy);  // craters (as an impact of its energy), shatters around the crater, loads the structures near it (J)
  // Immediate. Writes voxels: structures there are extracted again, pieces near are woken
  // (voxels written into a piece push it out). Chunks not resident in a streamed world are
  // generated first. Returns the number of voxels changed.
  i32 set_voxels(const std::vector<VoxelEdit>& edits, u32 flags = 0);
  i32 set_voxels(GridId grid, const std::vector<VoxelEdit>& edits, u32 flags = 0);  // (in a grid's coordinates)
  bool apply_impulse(i64 piece, const V3& point, const V3& impulse);  // N s at a world point
  bool remove_piece(i64 piece);                                         // PieceRemoved (Removed)
  // A piece the host keeps: never culled over max_bodies or the pieces' memory budget (a joint's
  // pieces - a machine's parts, what hangs on it - are kept anyway). Its parts keep it when it
  // breaks.
  bool set_piece_keep(i64 piece, bool keep);

  void tick();  // one step of config().dt
  i64 ticks() const;
  // The world's clock: s of simulation since the level loaded (paused ticks do not count; a saved
  // session's is restored). Joint drives follow their programs by it.
  f64 time() const;

  // ---- output
  std::vector<WorldEvent> take_events();   // since the last call, in order
  std::vector<u64> take_changed_chunks();  // keys (key3) of the world grid's chunks whose voxels changed since the last call (sorted)
  std::vector<u64> take_evicted_chunks();  // keys of chunks no longer resident (streaming) since the last call
  // The oriented grids' chunks whose voxels changed since the last call (by grid, then chunk).
  std::vector<GridChunk> take_changed_grid_chunks();
  std::vector<PieceState> pieces() const;  // id order
  const Body* piece(i64 id) const;         // shapes, fragments and state (nullptr: gone)
  const RigidWorld& rigid() const;

  // ---- extensions: layers, loads, forces, systems (world_ext.cpp)
  // Layers (grid.hpp): per-voxel byte channels. Layer kDamageLayer is the core's: a voxel's
  // damage (0 intact .. 255 no strength left) takes the strength of every bond section it is in
  // (fire, corrosion, rot, ... write it). Pieces carry their voxels' layer values.
  static constexpr int kDamageLayer = 0;
  // Its index (the existing one for the same layer added before); -1: full (kMaxLayers), or a
  // different layer (persistence, binding) under that name.
  int add_layer(const LayerSpec& spec);
  int layer_index(const std::string& name) const { return grid_->layer_index(name); }
  u8 layer(int L, const IVec3& p) const { return grid_->layer(L, p); }
  u8 layer(GridId grid, int L, const IVec3& p) const;
  i32 set_layer(int L, const std::vector<LayerEdit>& edits);  // returns the values changed
  i32 set_layer(GridId grid, int L, const std::vector<LayerEdit>& edits);
  std::vector<u64> take_layer_changes(int L);
  std::vector<u64> take_layer_changes(GridId grid, int L);  // (an oriented grid's chunks)
  // A piece's layer values at its shape voxels (shape: its shapes' index, 0 the first).
  u8 piece_layer(i64 piece, int L, const IVec3& shape_voxel) const { return piece_layer(piece, 0, L, shape_voxel); }
  u8 piece_layer(i64 piece, i32 shape, int L, const IVec3& shape_voxel) const;
  i32 set_piece_layer(i64 piece, int L, const std::vector<LayerEdit>& shape_voxels) { return set_piece_layer(piece, 0, L, shape_voxels); }
  i32 set_piece_layer(i64 piece, i32 shape, int L, const std::vector<LayerEdit>& shape_voxels);
  // Removes voxels of a piece (burnt out, melted, ...): what is left is one or more new pieces
  // (PieceRemoved Split, PieceAdded with this one as parent). dust: a Dust event per piece.
  bool remove_piece_voxels(i64 piece, const std::vector<IVec3>& shape_voxels, bool dust) {
    return remove_piece_voxels(piece, 0, shape_voxels, dust);
  }
  bool remove_piece_voxels(i64 piece, i32 shape, const std::vector<IVec3>& shape_voxels, bool dust);
  // Loads on the static world, by group (a system's, a chunk's, ...): replaces the group's
  // loads; they stay until replaced (an empty list removes them). Structures under loads that
  // are new or change are solved again; the design pass designs for them.
  void set_loads(u64 group, std::vector<VoxelLoad> loads);
  // A force on a piece during the next tick (N at a world point: buoyancy, drag, wind). It
  // does not wake a sleeping piece: wake_piece does (a floating piece, a gust).
  void apply_force(i64 piece, const V3& point, const V3& force);
  void wake_piece(i64 piece);
  // Systems, stepped in the order they were added.
  void add_system(std::shared_ptr<WorldSystem> s);
  const std::vector<std::shared_ptr<WorldSystem>>& systems() const;

  // ---- queries (no state change)
  // A point commands and queries accept: finite, and within the voxel key range (kVoxelLimit).
  bool in_range(const V3& p) const;
  RayHit raycast(const V3& origin, const V3& dir, f64 max_dist) const;  // the grids' voxels and the pieces
  // Whether there are oriented grids. (Without them the queries - grid_solid, raycast, collide -
  // only read: a host may run them from many threads at once while the world does not tick.)
  bool has_oriented_grids() const { return *oriented_ > 0; }
  // Moves the box [min, max] by `move` (per axis, x then y then z) as far as the grids' and the
  // pieces' voxels let it: a character controller's sweep; stepping up ledges is the host's
  // business. Boxes and moves beyond 16 m are refused / clamped.
  CollideResult collide(const V3& min, const V3& max, const V3& move) const;
  // How far the box [min, max] moves along `move` (any direction) before it touches a grid's or
  // a piece's voxels, and the normal there: a controller slides along rotated walls with it.
  SweepHit sweep(const V3& min, const V3& max, const V3& move) const;
  // Whether the box [min, max] overlaps a grid's or a piece's voxels (touching is not overlapping).
  bool overlaps(const V3& min, const V3& max) const;
  // How far the box must rise to overlap nothing (0: it overlaps nothing; in steps of half the
  // world grid's voxel), or -1 when not within max_rise: a controller's feet pushed into by a
  // rising lift's car or a turning deck are lifted out before it moves on.
  f64 depenetrate(const V3& min, const V3& max, f64 max_rise) const;
  // Diagnostic field of the voxels of a resident chunk (kChunkVox values, Chunk::v order).
  // (Brings the chunk's fragments up to date: not const.)
  bool debug_field(const IVec3& chunk, DebugField field, std::vector<u8>* out) { return debug_field(kWorldGrid, chunk, field, out); }
  bool debug_field(GridId grid, const IVec3& chunk, DebugField field, std::vector<u8>* out);
  // Utilization of the structure holding a voxel (solved now, gravity and current loads): max
  // phi and the number of bonds at or over 1. -1 if the voxel is in no structure.
  f64 probe_utilization(const IVec3& voxel, i32* over = nullptr) { return probe_utilization(kWorldGrid, voxel, over); }
  f64 probe_utilization(GridId grid, const IVec3& voxel, i32* over = nullptr);
  void debug_voxel(const IVec3& p);  // prints a voxel's fragment, owner and neighbours (stdout)

  WorldStats stats() const;
  MemoryReport memory() const;
  u64 state_hash() const;    // voxels, broken bonds and junctions, the grids
  u64 session_hash() const;  // + the pieces' poses

 private:
  struct Impl;  // (core/src/world/world_impl.hpp: the world's machinery and state)
  std::unique_ptr<Impl> impl_;
  // (handles into impl_ - it never moves - for the reads hosts make per voxel, inline: the world
  // grid, and the count of oriented grids)
  const VoxelGrid* grid_ = nullptr;
  const i32* oriented_ = nullptr;
};

}  // namespace svx
