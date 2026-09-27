// structvox — engine core (plan §B5 scheduling, §B7 failure / detachment, §B8 world, §B9).
//
// Owns the world voxel grid and turns player actions into structural events:
//   carve / blast  ->  physics window (Region) around the event, static baseline (warm-started
//   from the per-brick cache) -> event mutation (grid + window lattice) -> released residual
//   r = f_int_pre(u0) - f_int_post(u0) -> world-grid detachment -> severity triage:
//     low severity (bullet-class, nothing near failure): static settle, no bubble;
//     otherwise an event Bubble (telescoping composite dynamics) stepped under a per-tick
//     budget; its ruptures and detachments are mirrored into the grid as they happen.
//   When a bubble sleeps: settle projection + verification closure (static + DIF) of its
//   window, new baseline into the cache, persistent render offsets, damage overlay.
// Everything is deterministic: fixed tick, events processed in arrival order, bubbles stepped
// in id order; queries (raycast / collide) never change state.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "svx/bubble/bubble.hpp"
#include "svx/engine/debris.hpp"
#include "svx/mesh/mesher.hpp"
#include "svx/world/grid.hpp"
#include "svx/world/region.hpp"
#include "svx/world/streaming.hpp"

namespace svx {

class CommandLog;

// Defaults calibrated against the prototype's XPBD feel captures (docs/phase5/FEEL_SPEC.md),
// equal to the web front end's (damping 0.05 of critical at 5 Hz -> alpha = pi / s).
struct EngineParams {
  f64 compliance = 9.0;      // S_p (plan §B3)
  f64 amplification = 1.0;   // render amplification A of event-induced displacement
  f64 fragility = 1.0;       // game-law capacity multiplier F (< 1 weaker)
  f64 damping = 3.14159265358979;  // Rayleigh alpha (1/s)
  int debug_view = 0;        // 0 none, 1 damage / utilization, 2 bubble level
  bool paused = false;
};

struct EngineConfig {
  f64 dt = 1.0 / 60.0;
  i32 window = 32;           // physics window radius for rockets (voxels)
  i32 window_small = 10;     // window radius for bullet-class carves
  f64 merge_radius = 1.0;    // events of one tick within this distance (m) share a window
  i64 bake_max_cells = 3000000;  // bake the whole-world baseline at load up to this size
  // An event window whose cached baseline has kinks the law reads (residual over its f32 floor
  // and a bond off the rim over 0.75 of onset: tiles of a progressive bake meeting with a jump,
  // a stale cache) is solved statically first, in the background (settle_max_pcg), so the law
  // never reads a jump in the cache as strain. The cache heals as events store their states.
  bool resolve_windows = true;
  // A rocket's bubble spans the whole structure it hits (plan §B5: a composite over the anchored
  // structure, fine near the event and coarse far from it) when that structure has at most
  // structure_max_cells cells and lies within structure_max_chunks chunks: no pinned window rim,
  // so the far structure answers the event (a tower losing its columns tips over). It is found and
  // extracted by the bubble's background setup, from a snapshot taken before the event; the event
  // itself is applied to the world at once. Larger structures (a whole Doom level) keep a window.
  // 0: windows only.
  i64 structure_max_cells = 400000;
  i64 structure_max_chunks = 512;
  // Fine cells nomination may add to a bubble (plan §B6.1, above the cells its events asked for):
  // a window's, and a structure-spanning bubble's - whose steps then cost a few times a
  // window's, the far structure being aggregates (a tower losing its supports: ~10k nodes).
  i32 max_fine_cells = 12000;
  i32 structure_max_fine_cells = 4000;
  // Newton iterations of a structure-spanning bubble's step (plan §B2: linearly implicit, the
  // residual carried over): a third of the linear work of the windows' 3. (A tower losing its
  // supports falls 2 s later than with 3; the classification is the same.)
  int structure_newton_iters = 1;
  f64 R0 = 16.0;             // bubble fine radius (voxels), at least 2 x event radius
  // Error control of the bubbles (plan §B6, §B11): the nomination threshold (fraction of onset
  // an aggregate's bound may reach before it becomes fine), the marginal band of the re-solve
  // (§B6.3), and the near-field demand inflation (calibrated against outcomes: 1.0).
  f64 nominate_phi = 0.5;
  f64 marginal_eps = 0.05;
  f64 demand_inflation = 1.0;
  int max_level = 3;
  int max_bubbles = 4;
  int steps_per_tick = 2;    // bubble steps per tick (over all bubbles, id order)
  // A bubble is set up in four stages (event residual, composite, preconditioner, first step)
  // and joins the stepping at least this many ticks later (plan §B5: staggered first steps; the
  // event's carve shows at once), later when its counted work needs more (tick_work below): on
  // a background thread in one go, or without threads one stage per tick, so no tick carries a
  // whole setup (on one WASM thread each stage of a ~3k-node bubble takes 5-10 ms). After that
  // its steps run in the background, each committed after its work's span (one tick for most).
  // Deterministic whatever the threading; 0 = set up within the tick.
  // (4: one tick per stage; paced p99 with threads 2: 16 ms, 3: 12 ms, 4: 10 ms.)
  int spawn_latency_ticks = 4;
  bool pipeline_steps = true;  // false: each step is committed in the tick it is launched
  // small carves settle statically in the background, applied at the next tick (or before
  // anything that touches their window); false: within the event tick
  bool async_triage = true;
  i32 team_nodes = 10000;    // a lone bubble this large steps with the thread pool (else serial)
  i32 small_event = 16;      // carves removing <= this many voxels are triaged statically
  f64 design_utilization = 0.5;  // bake: strengthen members above this self-weight utilization
  // Bake with linear kinematics: self-weight rotations are ~1e-4..1e-2 rad, so the corotational
  // correction is second order, and events use f_int(u0) of whatever baseline they get (the
  // increment formulation stays exact); one linear solve instead of nonlinear iterations.
  bool bake_linear = true;
  bool idle_bake = true;     // idle ticks bake the next unbaked tile near the viewer (lazy baselines)
  int max_respawns = 8;      // dynamic cascade continuations per window
  int settle_max_pcg = 240;  // linear work of a settle projection (a sleeping bubble needs 0-60)
  // Budget manager (plan §B5): deterministic degradation from the active composite node count
  // (never wall-clock time), in the plan's order — coarser grading of new bubbles above half
  // the budget; a smaller fine radius and static triage of medium events above it; bubbles far
  // from the viewer stepped every other tick above it (slower simulated time, never dropped).
  i64 node_budget = 24000;
  f64 degraded_grading = 3.0;
  f64 degraded_R0 = 12.0;
  i32 degraded_small_event = 64;
  // Work per tick (plan §B5, last degrade: slower simulated time, never dropped work). Background
  // jobs - a bubble's setup or step, the settle projection of a finalizing bubble, a static
  // settle - count their work W in node-iterations as they run (base/work.hpp: PCG iterations
  // times nodes, multigrid builds, law sweeps: deterministic counts, never wall-clock time) and
  // are committed ceil(W / tick_work) ticks after their launch (at most max_span; at least their
  // minimum latency). A heavy bubble then steps at a fraction of the tick rate while the tick
  // goes on, instead of holding it; the simulation thread waits only when the machine is slower
  // than tick_work assumes. 0: every job commits at its minimum latency, however long it takes.
  i64 tick_work = 36000;
  i32 max_span = 120;
  // A bubble whose steps take several ticks of work keeps pace with the world by longer steps
  // (before slower simulated time, plan §B5): its step becomes k ticks, k a power of two up to
  // this, chosen from its last step's span (k doubles once a step spans 2k ticks, halves once it
  // spans k/2 or less; deterministic, from counted work). A structure-wide collapse then plays
  // at the world's pace, its motion committed every k ticks. 1: every step is one tick.
  i32 max_dt_multiplier = 8;
  // An event in a running bubble's window waits for that bubble: its step commits, its settle
  // projection runs in the background, and the event is processed once the bubble has been
  // finalized (false: the bubble is committed and settled within the event's tick).
  bool defer_takeovers = true;
  // Background verification (plan §B6): after a bubble settles, a full-fine static solve (law on
  // every bond) of the whole structures its window touched — not the window: a pinned rim would
  // be a phantom support; a failure the composite far field could not see continues as a
  // dynamic bubble (eventual consistency: "creak, then collapse").
  // A job starts once its window has been quiet (no event in it, no bubble or settle over it,
  // for verify_quiet_ticks - however busy the world is elsewhere) with a snapshot of the chunks
  // its structures may reach;
  // extraction and solve run on a background thread with their own thread team (native, WASM
  // with pthreads), or at the due tick. The result is applied at a tick fixed by the job's size
  // (verify_latency_ticks, longer for structures above verify_cells_per_tick per tick), so
  // replays are bit-identical whatever the threading and machine load.
  bool verify = true;
  i64 verify_max_cells = 400000;  // larger structures are verified around the window only
  int verify_max_iters = 60;
  int verify_max_pcg = 600;        // linear-solve budget of a job (near a mechanism the static
                                   // iteration chatters: undecided, resolved dynamically)
  // ... scaled down for large jobs to this many cell-iterations (at least verify_min_pcg), so a
  // job fits the time its due tick gives it on a loaded machine (~9M cell-iterations/s on the
  // three-thread team, plus ~140 ms per multigrid build at 71k cells: a 71k-cell job with 541
  // iterations kept the simulation thread waiting 4.7 s at its due tick, with 169 still 1.25 s)
  i64 verify_pcg_cells = 8000000;
  int verify_min_pcg = 120;
  int verify_latency_ticks = 120;  // 2 s (plan §B6: eventual consistency within 2 s)
  i64 verify_cells_per_tick = 600;  // larger jobs get proportionally longer (98k cells: 2.7 s)
  int verify_quiet_ticks = 60;     // start only after 1 s without events or bubbles in the window
  int verify_threads = 3;          // the background job's team (1 = serial)
  int verify_max_chunks = 512;     // snapshot bound (structures reaching beyond: window only)
  // Streamed worlds (plan §B8 "load paths cross boundaries"): chunks the structures reach that
  // are not resident are hydrated by the job - regenerated from the source and their archived
  // edits and designed - so a structure reaching into evicted chunks is checked whole (within
  // verify_max_chunks); a failure there streams its chunks in. false: verified around the window.
  bool verify_hydrate = true;
  bool verify_async = true;
  f64 dif = 1.5;             // dynamic increase factor of the verification closure
  bool corot = true;
  // Unilateral cracked contacts (plan §B7 / Phase 7): a rupture cracks a bond (the pieces
  // still touch, carry compression, friction and rocking) instead of cutting it; pieces detach
  // only once their cracks have opened for good.
  ContactParams contact{true, 0.6, 0.25};
  // Plate action of slabs and walls (Poisson stiffening, two-way capacity reserve).
  PlateParams plate{true, 4, 8, 0.2, 1.4};
  // chunks under running bubbles move by per-tick displacement fields on the GPU (take_fields)
  // instead of being re-meshed every tick
  bool gpu_displacement = false;
  // rigid debris (plan Phase 7): detached pieces fall, collide with the world and fade
  bool debris = true;
  int max_debris = 48;       // simulated pieces beyond this start fading (oldest first)
  DebrisParams debris_params{};
  // landings of debris on the structure become impact loads (plan §B7 virtual impact)
  f64 impact_min_impulse = 400.0;  // N s: lighter landings only raise effects
  f64 impact_duration = 0.05;      // s: equivalent static force J / duration for the triage
  i32 impact_window = 16;          // window radius (voxels)
  int impacts_per_tick = 1;        // structural impact loads processed per tick
  int impact_max_age = 30;         // ticks a queued impact load may wait before it is dropped
};

struct EngineEvent {
  enum class Kind : u8 { Detached, Crack, Impact, Bubble };
  Kind kind = Kind::Crack;
  i64 id = 0;
  std::array<f64, 3> pos{0, 0, 0}, vel{0, 0, 0}, ang{0, 0, 0}, normal{0, 0, 1};
  f64 radius = 0.0, strength = 0.0;
  i32 voxels = 0;
  int level = 0;
  bool rigid = false;  // detached pieces: simulated as rigid debris (poses via Engine::debris)
  ChunkMesh mesh;  // detached pieces, world coordinates
};

// Displacement of a running bubble for GPU vertex displacement (EngineConfig::gpu_displacement):
// a dense voxel grid over the moving cells, RGBA binary16 per voxel = (w dx, w dy, w dz, w) with
// w = 1 for solid voxels (0 for air) and d the amplified event displacement, x fastest. Sampled
// trilinearly at a mesh vertex (a voxel corner) and divided by w, it gives the displacement
// averaged over the solid voxels around the corner, as the CPU mesher does.
struct DisplacementField {
  i64 id = 0;                // the bubble
  IVec3 lo{0, 0, 0};         // first voxel of texel (0, 0, 0)
  std::array<i32, 3> size{0, 0, 0};
  i32 stride = 1;            // voxels per texel and axis (a large bubble's field is coarser)
  i64 version = 0;           // the bubble's commit it shows (unchanged: the same field)
  f64 max_disp = 0.0;        // metres (for culling bounds)
  std::vector<u16> rgba;
};

// What one activation of a mover does (a Doom line special's action on a sector plane). Rows are
// the solid part's thickness in voxel levels (0 .. z1 - z0).
struct MoverMove {
  enum class Type : u8 {
    To,      // go to `target` and stay (floors, ceilings, raising platforms, stairs, open / close doors)
    Return,  // go to `target`, wait, go back to `back` (doors, lifts, close-then-open doors)
    Cycle,   // go to `target`, wait, go to `back`, wait, ... (perpetual platforms, crushers)
    Stop,    // a cycling mover stops where it is; its Cycle move resumes it
  };
  Type type = Type::Return;
  i32 target = 0;
  i32 back = -1;         // Return / Cycle: -1 = the rows when the move started
  f64 speed = 1.0;       // voxel levels per second
  f64 wait = 0.0;        // s at `target` (Return), at each end (Cycle)
  bool reverse = false;  // Return: coming back, turn around at the player (doors)
};

// A moving part of the world (plan Phase 7 "doors and lifts", with Doom's floors, ceilings,
// platforms, crushers and stairs): columns whose voxel span [z0, z1) holds a solid part `rows`
// levels thick, hanging from z1 (Door, Ceiling) or standing on z0 (Lift, Floor). The solid part
// is anchored and bonded to nothing (the structure never leans on it). Activations run moves;
// deterministic: moves with the fixed tick. A growing ceiling never closes on the player (a
// door turns around, others wait); a rising floor carries the player up and waits while the
// player would not fit under what is above.
struct MoverDef {
  enum class Kind : u8 { Door, Lift, Floor, Ceiling };
  Kind kind = Kind::Door;
  std::vector<std::array<i32, 2>> cols;
  i32 z0 = 0, z1 = 0;
  i32 rows = -1;         // solid rows at the start (-1: the whole span: closed / raised)
  Vox vox = 0;           // voxel value of the solid part (made anchored)
  bool usable = true;    // "use" on the mover itself runs moves[0] (doors, lifts)
  // Door / Lift without `moves`: open / lower fully, wait, return (repeat) or stay open
  f64 speed = 1.5;       // span fractions per second
  f64 wait = 4.0;        // s open / lowered before returning (repeatable movers)
  bool repeat = true;    // false: stays open once used
  std::vector<MoverMove> moves;
  bool ceiling() const { return kind == Kind::Door || kind == Kind::Ceiling; }
};

// A resolver's answer: run move `move` of mover `mover`.
struct MoverTrigger {
  i32 mover = -1;
  i32 move = 0;
};

struct RayHit {
  bool hit = false;
  std::array<f64, 3> pos{0, 0, 0}, normal{0, 0, 0};
  f64 distance = 0.0;
  int material = -1;
  IVec3 voxel{0, 0, 0};
};

struct CollideResult {
  std::array<f64, 3> move{0, 0, 0};
  bool on_ground = false;
};

struct EngineStats {
  f64 tick_ms = 0.0, structural_ms = 0.0, event_ms = 0.0;
  i32 active_bubbles = 0, active_nodes = 0;
  i64 voxels = 0, chunks = 0;
  f64 memory_mb = 0.0;
  i64 events = 0, bubbles_spawned = 0, static_settles = 0, ruptures = 0, detached_voxels = 0;
  i64 cracks = 0;  // ruptures that became contacts (included in ruptures)
  i64 degraded_spawns = 0, degraded_triage = 0, skipped_steps = 0;  // budget manager
  i64 spanned_steps = 0, span_ticks = 0;  // steps given more than one tick, and their extra ticks
  i64 deferred_events = 0;               // events that waited for a bubble's finalize
  i64 merged_events = 0;                 // events merged into the running bubble of their structure
  i64 long_steps = 0;                    // bubble steps longer than a tick (max_dt_multiplier)
  i64 structure_bubbles = 0;             // bubbles whose setup extracted the whole structure
  i64 bubble_failures = 0;  // bubbles dropped after a numerical breakdown
  i64 ticks = 0;
  i64 unbaked_chunks = 0;
  f64 bake_ms = 0.0;
  i64 resident_chunks = 0, archived_chunks = 0, generated_total = 0, evicted_total = 0, budget_evicted = 0;
  f64 stream_ms = 0.0;
  // coarse connectivity of non-resident chunks (streamed worlds): summaries computed, chunks
  // made resident because a detached piece reached into them, time spent summarizing
  i64 coarse_summaries = 0, coarse_hydrated = 0;
  f64 coarse_ms = 0.0;
  i32 debris_bodies = 0;
  i64 debris_landings = 0, impact_loads = 0, impacts_dropped = 0;
  f64 debris_ms = 0.0;
  i64 verifications = 0, verify_failures = 0;
  f64 verify_ms = 0.0;
  // bubble step profile (cumulative)
  i64 bubble_steps = 0, step_pcg = 0, step_newton = 0, step_rebuilds = 0;
  f64 step_ms = 0.0, step_solve_ms = 0.0, step_mg_ms = 0.0, step_law_ms = 0.0, step_topo_ms = 0.0;
};

class Engine {
 public:
  Engine();
  ~Engine();
  Engine(Engine&&) noexcept;
  Engine& operator=(Engine&&) noexcept;
  void configure(const EngineConfig& c) { cfg_ = c; }
  const EngineConfig& config() const { return cfg_; }
  void set_params(const EngineParams& p);
  const EngineParams& params() const { return par_; }  // (compliance capped, see below)
  // Buckling margin of the compliance knob (plan §B3, §B10): S_p is capped at `cap` (e.g. a
  // map's doom::wall_slenderness p99 limit; 0 = none). Set after load and before the bake:
  // the baseline depends on it. load() clears it.
  void set_compliance_cap(f64 cap);
  f64 compliance_cap() const { return compliance_cap_; }

  void load(VoxelGrid&& g, const std::array<f64, 3>& spawn_pos, const std::array<f64, 3>& spawn_dir);
  // Static baseline of every structure (cached per brick; events then solve only increments).
  // Returns false if the world exceeds bake_max_cells: then chunks are baked progressively in
  // idle ticks, nearest to the viewer first (bake_tile), and events in unbaked regions solve
  // their own window.
  bool bake(f64* ms = nullptr, int* pcg_iters = nullptr);
  bool bake_tile();  // one chunk tile; false when everything is baked
  // Persistence: gameplay changes since load as a binary delta; load_delta applies one to the
  // freshly loaded base world (cached baselines around the touched chunks are re-baked).
  std::vector<u8> save_delta() const;
  bool load_delta(const std::vector<u8>& bytes);
  // Streaming (plan §B8, Phase 6): the world is generated chunk by chunk around the viewer and
  // evicted behind it; edits of evicted chunks are archived and restored exactly.
  void enable_streaming(std::unique_ptr<ChunkSource> src, const StreamConfig& sc);
  bool streaming() const { return source_ != nullptr; }
  i64 resident_chunks() const { return static_cast<i64>(generated_.size()); }
  const ChunkSource* source() const { return source_.get(); }
  bool modified() const { return !grid_.modified_chunks().empty(); }
  void set_viewer(const std::array<f64, 3>& pos);
  i64 unbaked_chunks() const { return static_cast<i64>(unbaked_.size()); }
  struct DesignReport {
    f64 max_utilization = 0.0;   // before strengthening (demand / onset)
    i64 strengthened_voxels = 0;
    i64 floating_voxels = 0;     // unsupported pieces of the source world, removed at bake
    f64 max_utilization_after = 0.0;  // after strengthening (< 1: nothing fails at rest)
    i64 unfixable_bonds = 0;     // still at or above onset with the strongest class
    // the most utilized bond before strengthening (diagnostics): its lower voxel, axis, material,
    // and the dominant component of its demand (0 N, 1-2 V, 3 T, 4-5 M) with that component's
    // share of onset
    IVec3 worst{0, 0, 0};
    int worst_axis = 0, worst_component = 0;
    MaterialId worst_mat = MaterialId::Concrete;
  };
  const DesignReport& design_report() const { return design_; }
  const VoxelGrid& grid() const { return grid_; }
  std::array<f64, 3> spawn_pos() const { return spawn_pos_; }
  std::array<f64, 3> spawn_dir() const { return spawn_dir_; }

  void carve(const std::array<f64, 3>& pos, f64 radius);
  void blast(const std::array<f64, 3>& pos, f64 radius, f64 energy);
  void tick();
  // Movers (doors, lifts, floors, ceilings). add_mover fills the solid part (MoverDef::rows) and
  // returns its id. activate_mover runs one of its moves: false if disabled, busy with another
  // move, or already where the move goes.
  i32 add_mover(const MoverDef& d);
  bool activate_mover(i32 id, i32 move = 0);
  i32 mover_at(const IVec3& voxel) const;  // mover whose span holds this voxel, or -1
  i32 mover_count() const { return static_cast<i32>(movers_.size()); }
  const MoverDef* mover_def(i32 id) const;
  i32 mover_rows(i32 id) const;            // solid rows now
  bool mover_busy(i32 id) const;           // running a move (a stopped cycle is not busy)
  f64 mover_position(i32 id) const;        // 0 closed / raised .. 1 open / lowered
  // The player's "use": a ray of `reach` metres; activates the usable mover at the hit voxel,
  // else the moves use_resolver returns for the hit face (e.g. Doom switch lines). Recorded.
  bool use(const std::array<f64, 3>& eye, const std::array<f64, 3>& dir, f64 reach = 2.0);
  std::function<std::vector<MoverTrigger>(const IVec3& voxel, int face)> use_resolver;
  // Walk-over triggers (e.g. Doom W1 / WR lines): called with the previous and the new viewer
  // position on every set_viewer (viewer updates are commands, so this replays exactly).
  std::function<std::vector<MoverTrigger>(const std::array<f64, 3>& from, const std::array<f64, 3>& to)>
      walk_resolver;
  // Gun-shot triggers (e.g. Doom G1 / GR lines): called with the point of every carve (hitscan
  // impacts are carves, which are commands, so this replays exactly).
  std::function<std::vector<MoverTrigger>(const std::array<f64, 3>& at)> shot_resolver;
  i64 ticks() const { return st_.ticks; }
  // Records every command (carve, blast, viewer, params) stamped with its tick into `log`
  // (nullptr: off). See engine/replay.hpp.
  void record_to(CommandLog* log) { log_ = log; }

  RayHit raycast(const std::array<f64, 3>& origin, const std::array<f64, 3>& dir, f64 max_dist) const;
  CollideResult collide(const std::array<f64, 3>& min, const std::array<f64, 3>& max,
                        const std::array<f64, 3>& move) const;

  // Meshes of every chunk changed since the last call plus chunks under active bubbles.
  std::vector<ChunkMesh> take_meshes(const MeshOptions& base);
  std::vector<u64> take_removed_chunks();
  // Far render tier of streamed worlds (plan Phase 6): coarse meshes of far tiles (chunk
  // field = tile coordinates, a tile being StreamConfig::far_tile chunks square, all heights)
  // and the tiles whose far mesh must go (they came near, or went out of range).
  std::vector<ChunkMesh> take_far_meshes();
  std::vector<std::array<i32, 2>> take_far_removed();
  // Displacement fields of the running bubbles (gpu_displacement): the complete current set.
  std::vector<DisplacementField> take_fields();
  std::vector<EngineEvent> take_events();
  // Rigid debris pieces (ids of their Detached events) with their current poses.
  const std::vector<DebrisBody>& debris() const { return debris_.bodies(); }
  EngineStats stats() const;
  // Diagnostics: the cached baseline around a voxel - relative equilibrium residual (interior
  // cells), the law's demand at it (max phi, bonds over 0.5 and 1), and its largest bond jump.
  struct BaselineCheck {
    i64 cells = 0, over_half = 0, over_one = 0;
    f64 residual = 0.0, max_phi = 0.0;
    IVec3 worst{0, 0, 0};
    int worst_axis = 0;
  };
  BaselineCheck check_baseline(const IVec3& center, i32 radius);
  // Diagnostics: the coarse field (built now) against the cached baselines (after a whole bake:
  // the reference), as the relative error of the vertical displacement over structural voxels
  // (percentiles of |dz| error / max |dz|) and the worst voxel.
  std::array<f64, 4> coarse_field_error(IVec3* worst);
  u64 state_hash() const;    // deterministic digest of voxels, bonds and damage (the world)
  u64 session_hash() const;  // state_hash plus transient state (debris poses, queued impacts)

 private:
  struct PendingEvent {
    bool blast = false;
    std::array<f64, 3> pos{0, 0, 0};
    f64 radius = 0.0, energy = 0.0;
    std::vector<std::array<f64, 4>> extra;  // merged carves of the same tick (pos, radius)
    bool impact = false;                    // a debris landing: impulse at pos, nothing removed
    std::array<f64, 3> impulse{0, 0, 0};
    i64 tick = 0;
  };
  struct Active;
  struct Verify;
  struct Spawn;   // an event's bubble to be: its window and loads
  struct StructureJob;  // the structure a bubble will span: its snapshot and the event to replay
  struct Merge;         // an event merged into a running bubble, as the world took it (voxels)
  struct Settle;  // a small carve's static settle running in the background

  LatticeOptions lattice_options() const;
  void process(const PendingEvent& e);
  // Debris landing on a running bubble's window: the impulse goes into that bubble.
  bool impact_into_bubble(const PendingEvent& e);
  void step_debris();
  void step_bubbles();
  // Mirrors a bubble step's cracks, breaks and detached pieces into the world.
  void commit_step(Active& a, const StepStats& s);
  // A bubble with a job in flight (its setup or a step): waits for (or runs) it and commits.
  void make_ready(Active& a);
  // Spawns an event's bubble (its setup runs in the background).
  void spawn_bubble(Spawn&& sp);
  // Applies a pending settle (waits for its solve): baseline, damage and offsets, or a bubble
  // when the window nears failure.
  void apply_settle(Settle& s);
  // Applies the pending settles due by now, and those whose windows reach within `reach` of
  // `at` (writes to the same cells keep their order).
  void apply_settles(bool due_only, const IVec3& at = {0, 0, 0}, f64 reach = -1.0);
  void launch_settle(std::unique_ptr<Settle> st);  // its solve in the background (counted work)
  bool settle_due(Settle& sl);
  // Schedules a bubble's setup job (composite, preconditioner, first step) on a background
  // thread; it joins the stepping spawn_latency_ticks later.
  void start_setup(Active& b, std::vector<std::function<void()>> stages, i32 est_nodes);
  void run_setup_stages();  // without threads: one setup stage per pending bubble and tick
  void verify_update();  // starts the oldest queued verification whose window is quiet / completes it
  // The chunks the structures around a window (centre c, radius wr) may reach: the window's and
  // one ring, then face-connected chunks that may hold structure, up to max_chunks (sorted keys).
  // complete: the search ended before the bound; structural_keys: the chunks holding structure.
  std::vector<u64> structure_chunks(const IVec3& c, i32 wr, i64 max_chunks, bool* complete,
                                    std::vector<u64>* structural_keys) const;
  // Queues a verification of the structures around a window (requeues: how often a moot or
  // stale job of this window was queued again; urgent: a stale job had found a failure — it
  // goes first and is never given up).
  void enqueue_verify(const IVec3& center, i32 radius, int respawns, int requeues, bool urgent = false,
                      std::vector<std::pair<u64, u32>> watch = {});
  // Settles a sleeping bubble; returns a continuation bubble if the window still fails.
  std::unique_ptr<Active> finalize(Active& a);
  // ... in two parts: the settle projection as the bubble's background job (due after its work
  // estimate), then the commit at the due tick (a taken-over bubble has no continuation).
  void start_finalize(Active& a);
  std::unique_ptr<Active> complete_finalize(Active& a);
  i64 span_of(i64 work) const;  // ticks a background job of this much work is given
  // Starts `fn` as the bubble's background job (counting its work); due after max(min_ticks,
  // span_of(work)) ticks (job_due).
  void launch_job(Active& a, std::function<void()> fn, i64 min_ticks, char kind);
  bool job_due(Active& a);
  // true: the event waits (a running bubble's window holds it; that bubble is taken over)
  bool defer_event(const PendingEvent& e);
  // The running bubble spanning the structure an event at voxel c hits (its reach holds c; one
  // whose cells are at c first), if any: the event merges into it instead of waiting.
  Active* merge_target(const PendingEvent& e, const IVec3& c);
  // The events merged into a bubble since its last job, as a mutation of its lattice (called on
  // the simulation thread with the bubble idle; its job applies it).
  Bubble::EventMutation take_merges(Active& a);
  void release_deferred();  // processes the deferred events no running bubble holds any more
  void mirror_islands(const std::vector<std::vector<IVec3>>& islands, Active* a, const std::vector<f64>* u,
                      const std::vector<f64>* v);
  void emit_detached(const std::vector<IVec3>& vox, const std::array<f64, 3>& vel, const std::array<f64, 3>& ang);
  std::vector<f64> window_baseline(Region& R, int* iters);
  // Removes voxels of the source world that cannot stand (pieces connected to no support) without
  // recording them as gameplay changes; returns their count.
  i64 remove_floating(const std::vector<IVec3>& vox);
  void store_baseline(const Region& R, const std::vector<f64>& u, const std::function<bool(i32)>& keep = {});
  // Strengthens members whose demand at u exceeds the design utilization (bonds whose cells
  // pass `inner`); updates design_.
  template <typename Inner>
  void design_pass(const Lattice& L, const std::vector<IVec3>& vox, const std::vector<f64>& u, Inner&& inner);

  EngineConfig cfg_;
  EngineParams par_;        // in effect
  EngineParams requested_;  // as set (the compliance before the cap)
  f64 compliance_cap_ = 0.0;
  VoxelGrid grid_;
  std::array<f64, 3> spawn_pos_{0, 0, 0}, spawn_dir_{1, 0, 0};
  std::vector<PendingEvent> queue_;
  std::vector<PendingEvent> deferred_;  // events waiting for the bubbles they hit (in order)
  std::vector<PendingEvent> impacts_;  // queued debris impact loads (strongest first each tick)
  DebrisSystem debris_;
  std::vector<std::unique_ptr<Active>> active_;
  std::vector<std::unique_ptr<Verify>> verify_;
  std::vector<std::unique_ptr<Verify>> verify_stale_;  // superseded, reaped once their thread ends
  std::vector<std::unique_ptr<Settle>> settles_;       // pending static settles (creation order)
  // bubbles of settles that neared failure, spawned at the next safe point of the tick (a settle
  // may be applied while the bubble list is being walked)
  std::vector<std::unique_ptr<Spawn>> spawns_;
  void spawn_pending();
  std::vector<EngineEvent> events_;
  std::vector<u64> removed_chunks_;
  std::vector<u64> animated_;  // chunks re-meshed every tick while a bubble moves them
  i64 next_id_ = 1;
  EngineStats st_;
  DesignReport design_;
  std::array<f64, 3> viewer_{0, 0, 0};
  bool viewer_set_ = false;  // a set_viewer since the load (walk triggers need a previous one)
  CommandLog* log_ = nullptr;
  std::vector<u64> unbaked_;  // chunks with structure but no baseline yet
  // A world too large to bake whole is solved on a coarse lattice first (4^3 voxels a cell, with
  // their solid extent as effective dimensions); tiles take their rims from its rigid motions, so
  // tiles baked alone agree across chunk borders (plan §B2: a baseline independent of the
  // partition; pinned at zero, a structure spanning tiles met itself with a jump there).
  struct CoarseField;
  std::unique_ptr<CoarseField> coarse_;
  void bake_chunk(const IVec3& cc);
  void build_coarse_field();
  f64 prof_commit_ms_ = 0.0;  // (SVX_TICK_PROFILE: step_bubbles' commits this tick)
  // movers
  struct Mover {
    MoverDef def;
    f64 level = 0.0;  // solid rows (continuous)
    i32 rows = 0;     // solid rows now (voxels)
    i32 move = -1;    // running move, -1 idle
    int leg = 0;      // 0 to target, 1 wait, 2 to back, 3 wait (Cycle)
    i32 back = 0;     // rows the running move returns to
    f64 timer = 0.0;
    bool stopped = false;   // a cycle stopped by a Stop move
    bool disabled = false;  // destroyed by an event: stays as it is
  };
  void step_movers();
  void set_mover_rows(Mover& m, i32 rows);
  bool blocks_player(const Mover& m, i32 rows) const;
  void disable_movers_at(const std::vector<IVec3>& removed);
  std::vector<Mover> movers_;
  std::unordered_map<u64, std::vector<i32>> mover_cols_;  // column (x, y) -> movers (floor / ceiling)
  // streaming
  int stream_update();  // returns the number of chunks generated this tick
  void ensure_chunks(const IVec3& vlo, const IVec3& vhi);  // generate chunks covering a voxel box
  bool generate_chunk(u64 key);
  void evict_chunk(u64 key);  // (archives its edits)
  void insert_generated(u64 key, bool any, std::vector<Vox>&& voxels);  // a generated chunk becomes resident
  void far_update();
  ChunkMesh far_mesh(i32 tx, i32 ty) const;
  std::unordered_set<u64> far_sent_;  // tiles with a far mesh in the front end
  std::vector<ChunkMesh> far_out_;
  std::vector<std::array<i32, 2>> far_removed_;
  std::shared_ptr<ChunkSource> source_;  // (shared with background verifications that hydrate)
  StreamConfig stream_{};
  std::unordered_set<u64> generated_;                   // resident (or known empty) chunks
  std::unordered_map<u64, i32> column_count_;           // generated chunks per (x, y) column
  // records of evicted modified chunks (immutable once made: background verifications share them)
  std::unordered_map<u64, std::shared_ptr<const std::vector<u8>>> archive_;
  // Coarse connectivity of the chunks that are not resident (plan §B4/§B8, world/coarse.hpp):
  // summaries of source chunk + archived edits, made when a detachment search first reaches a
  // chunk and dropped when it becomes resident or its record changes (trimmed between ticks).
  class StreamCoarse;
  std::unordered_map<u64, ChunkSummary> summaries_;
  // whether the grid holds the chunk's content (always, unless streamed; outside the world: air)
  bool chunk_resident(const IVec3& cc) const;
  // Detached islands of the world at `seeds`. In streamed worlds a piece reaching into chunks
  // that are not resident makes those chunks resident first (their voxels go with the piece).
  std::vector<std::vector<IVec3>> world_islands(const std::vector<IVec3>& seeds, bool supports_removed);
};

}  // namespace svx
