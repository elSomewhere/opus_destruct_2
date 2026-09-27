// structvox v2 — engine core (docs/V2_DESIGN.md).
//
// Owns the world voxel grid and the rigid bodies, and runs the fragment-graph mechanics:
//   - The world's free voxels are grouped into fragments (frag/fragments.hpp, cached per chunk).
//   - A static structure is a connected set of world fragments that reaches a support. It is
//     extracted from the grid when something changes it (an event, a detachment, new loads) and
//     its equilibrium K u = f (gravity, contact forces of bodies, blast loads) is solved over
//     ticks under a work budget. Converged, its bonds are judged; the worst overloaded ones break
//     (a dynamic increase factor on sudden changes), pieces that lose their supports leave the
//     grid as rigid bodies, and the structure is solved again: cascades unfold over ticks.
//   - Rigid bodies keep their fragments and bonds. After each contact solve their stress is
//     solved under contact forces and inertia (fracture hook of phys/rigid.hpp); a body whose
//     bonds break splits, and the substep is solved again with the pieces.
//   - Contact forces of bodies on the world load the structures under them (impacts and dead
//     loads): falling floors can break the floors they land on.
// Deterministic: fixed tick, events in arrival order, structures and bodies in id order, work
// budgets counted in solver operations (never wall-clock time).
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "svx/base/vec.hpp"
#include "svx/frag/fragments.hpp"
#include "svx/mesh/mesher.hpp"
#include "svx/phys/rigid.hpp"
#include "svx/stress/stress.hpp"
#include "svx/world/grid.hpp"
#include "svx/world/streaming.hpp"

namespace svx {

class CommandLog;
namespace engine_detail {
struct SecAcc;
}
using engine_detail_SecAcc = engine_detail::SecAcc;

// Runtime knobs (docs/V2_DESIGN.md §6).
struct EngineParams {
  f64 fragility = 1.0;    // divides every bond strength (> 1: more collapse)
  f64 impact = 1.0;       // contact force = impact x impulse / dt (impact severity)
  f64 dif = 1.5;          // dynamic increase factor of sudden load changes (1 = static)
  int debug_view = 0;     // 0 none, 1 utilization (last judged state), 2 fragments
  bool paused = false;
};

struct EngineConfig {
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
  f64 structure_max_radius = 60.0; // m from the seed (beyond: held fixed)
  i32 max_breaks_per_round = 256;
  f64 break_band = 0.85;           // a round breaks the bonds with phi >= max(1, band x max phi)
  i32 max_rounds = 400;            // break rounds of one structure before it is left alone
  i32 idle_drop_ticks = 1800;      // idle structures without loads are dropped after this
  f64 load_trigger = 0.25;         // re-solve when a node's external load changes by this x its weight ...
  f64 load_trigger_abs = 800.0;    // ... plus this (N)
  f64 dead_load_ema = 0.25;        // smoothing of resting contact loads per tick
  // bodies
  i32 max_bodies = 3000;           // beyond: the smallest old sleeping pieces fade out
  i32 body_stress_maxit = 60;      // PCG iterations of a body's stress solve
  f64 body_stress_rtol = 1e-2;
  f64 body_trigger = 1.8;          // contact force sum over weight that triggers a body's stress check
  f64 body_impact_dv = 0.35;       // ... together with a velocity change of this much per substep (m/s)
  f64 small_impact_dv = 2.5;       // pieces lighter than small_piece_mass need up to this velocity change
  f64 small_piece_mass = 1500.0;   // kg
  i32 min_fracture_frags = 8;      // smaller pieces never break further (the smallest rubble)
  i32 body_check_ticks = 12;       // steady contact: re-check every so many substeps
  i32 min_body_voxels = 1;
  f64 fade_time = 1.0;
  // blasts
  f64 blast_shatter = 1.7;         // shatter radius / crater radius: fragments come loose
  f64 blast_reach = 3.5;           // load radius / crater radius
  f64 blast_kinetic = 0.06;        // fraction of the blast energy given to the shattered pieces
  f64 blast_max_speed = 28.0;
  // design pass (bake): members above this self-weight utilization are strengthened
  f64 design_utilization = 0.45;
  i32 crack_events_per_tick = 24;
};

struct EngineEvent {
  enum class Kind : u8 { Detached, Crack, Impact, Bubble };
  Kind kind = Kind::Crack;
  i64 id = 0;
  std::array<f64, 3> pos{0, 0, 0}, vel{0, 0, 0}, ang{0, 0, 0}, normal{0, 0, 1};
  f64 radius = 0.0, strength = 0.0;
  i32 voxels = 0;
  int level = 0;
  bool rigid = false;  // detached pieces: a rigid body (pose via Engine::pieces)
  ChunkMesh mesh;      // detached pieces, world coordinates at announcement
};

// A rigid piece's pose for the front end: its mesh (sent in world coordinates with pos = its
// centre of mass then) is drawn at pos + rot (p - announced centre).
struct PiecePose {
  i64 id = 0;
  V3 pos;
  Quat rot;
  f64 opacity = 1.0;
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

// A moving part of the world (Doom's doors, lifts, floors, platforms, crushers, stairs): columns
// whose voxel span [z0, z1) holds a solid part `rows` levels thick, hanging from z1 (Door,
// Ceiling) or standing on z0 (Lift, Floor). The solid part is anchored and bonded to nothing.
struct MoverDef {
  enum class Kind : u8 { Door, Lift, Floor, Ceiling };
  Kind kind = Kind::Door;
  std::vector<std::array<i32, 2>> cols;
  i32 z0 = 0, z1 = 0;
  i32 rows = -1;         // solid rows at the start (-1: the whole span: closed / raised)
  Vox vox = 0;           // voxel value of the solid part (made anchored)
  bool usable = true;    // "use" on the mover itself runs moves[0] (doors, lifts)
  f64 speed = 1.5;       // span fractions per second
  f64 wait = 4.0;        // s open / lowered before returning (repeatable movers)
  bool repeat = true;    // false: stays open once used
  std::vector<MoverMove> moves;
  bool ceiling() const { return kind == Kind::Door || kind == Kind::Ceiling; }
};

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
  i64 body = 0;  // the rigid piece hit (0: the world)
};

struct CollideResult {
  std::array<f64, 3> move{0, 0, 0};
  bool on_ground = false;
};

struct EngineStats {
  f64 tick_ms = 0.0, structural_ms = 0.0, event_ms = 0.0, rigid_ms = 0.0, mesh_ms = 0.0;
  i64 ticks = 0, events = 0;
  i64 voxels = 0, chunks = 0;
  f64 memory_mb = 0.0;
  // structures
  i32 structures = 0, solving = 0;        // registered / being solved now
  i64 solve_nodes = 0;                    // nodes of the structures being solved
  i64 extractions = 0, extracted_nodes = 0, solves = 0, pcg_iters = 0;
  i64 bonds_broken = 0, detached_voxels = 0, detached_pieces = 0;
  f64 max_utilization = 0.0;              // of the last judged round
  // bodies
  i32 bodies = 0, awake = 0, contacts = 0;
  i64 body_checks = 0, body_splits = 0, impacts = 0;
  // world
  i64 resident_chunks = 0, archived_chunks = 0, generated_total = 0, evicted_total = 0, budget_evicted = 0;
  f64 stream_ms = 0.0;
  i32 movers = 0;
  // design (bake)
  f64 design_max_utilization = 0.0;
  i64 strengthened_voxels = 0, floating_voxels = 0;
  f64 bake_ms = 0.0;
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
  const EngineParams& params() const { return par_; }

  void load(VoxelGrid&& g, const std::array<f64, 3>& spawn_pos, const std::array<f64, 3>& spawn_dir);
  // Design pass (bake): removes floating source pieces, then solves every structure under its
  // own weight and strengthens members above design_utilization. Returns true when the whole
  // world was designed (streamed worlds: their generators are designed by construction).
  bool bake(f64* ms = nullptr);
  struct DesignReport {
    f64 max_utilization = 0.0;
    i64 strengthened_voxels = 0;
    i64 floating_voxels = 0;
    f64 max_utilization_after = 0.0;
    i64 structures = 0, nodes = 0;
  };
  const DesignReport& design_report() const { return design_; }

  std::vector<u8> save_delta() const;
  bool load_delta(const std::vector<u8>& bytes);
  void enable_streaming(std::unique_ptr<ChunkSource> src, const StreamConfig& sc);
  bool streaming() const { return source_ != nullptr; }
  i64 resident_chunks() const { return static_cast<i64>(generated_.size()); }
  const ChunkSource* source() const { return source_.get(); }
  bool modified() const { return !grid_.modified_chunks().empty(); }
  void set_viewer(const std::array<f64, 3>& pos);
  const VoxelGrid& grid() const { return grid_; }
  std::array<f64, 3> spawn_pos() const { return spawn_pos_; }
  std::array<f64, 3> spawn_dir() const { return spawn_dir_; }

  void carve(const std::array<f64, 3>& pos, f64 radius);
  void blast(const std::array<f64, 3>& pos, f64 radius, f64 energy);
  void tick();

  // Movers (engine/movers.cpp).
  i32 add_mover(const MoverDef& d);
  bool activate_mover(i32 id, i32 move = 0);
  i32 mover_at(const IVec3& voxel) const;
  i32 mover_count() const { return static_cast<i32>(movers_.size()); }
  const MoverDef* mover_def(i32 id) const;
  i32 mover_rows(i32 id) const;
  bool mover_busy(i32 id) const;
  f64 mover_position(i32 id) const;
  bool use(const std::array<f64, 3>& eye, const std::array<f64, 3>& dir, f64 reach = 2.0);
  std::function<std::vector<MoverTrigger>(const IVec3& voxel, int face)> use_resolver;
  std::function<std::vector<MoverTrigger>(const std::array<f64, 3>& from, const std::array<f64, 3>& to)>
      walk_resolver;
  std::function<std::vector<MoverTrigger>(const std::array<f64, 3>& at)> shot_resolver;
  i64 ticks() const { return st_.ticks; }
  void record_to(CommandLog* log) { log_ = log; }

  // Queries (never change state). Rays hit the world and the rigid pieces.
  RayHit raycast(const std::array<f64, 3>& origin, const std::array<f64, 3>& dir, f64 max_dist) const;
  CollideResult collide(const std::array<f64, 3>& min, const std::array<f64, 3>& max,
                        const std::array<f64, 3>& move) const;

  // Output for the front end.
  std::vector<ChunkMesh> take_meshes(const MeshOptions& base);
  std::vector<u64> take_removed_chunks();
  std::vector<ChunkMesh> take_far_meshes();
  std::vector<std::array<i32, 2>> take_far_removed();
  std::vector<EngineEvent> take_events();
  std::vector<PiecePose> pieces() const;
  const RigidWorld& rigid() const { return rigid_; }
  EngineStats stats() const;
  u64 state_hash() const;
  u64 session_hash() const;

  // Diagnostics / tests: utilization of the structure holding a voxel (solved now, gravity and
  // current loads): max phi and the number of bonds at or over 1. -1 if the voxel is no structure.
  f64 probe_utilization(const IVec3& voxel, i32* over = nullptr);

 private:
  struct Structure;
  struct PendingEvent {
    bool blast = false;
    V3 pos;
    f64 radius = 0.0, energy = 0.0;
  };
  // a fragment's identity: chunk and its first voxel (stable while that voxel stays)
  struct FragKey {
    u64 chunk = 0;
    i32 idx = -1;  // index in the chunk's FragChunk (valid for the chunk's current fragments)
  };

  // ---- fragments
  FragChunk& frag_chunk(const IVec3& cc);   // (re)builds when stale
  FragChunk* frag_chunk_if(u64 key);         // current or nullptr (no rebuild)
  bool frag_at(const IVec3& p, FragKey* out); // the free fragment holding voxel p
  i64 owner_of(const FragKey& f) const;      // structure id holding it (0: none)
  void voxels_of(const FragKey& f, std::vector<IVec3>& out);
  u8 frag_class(const FragKey& f);           // weakest design class of its voxels
  void mark_owners_stale(u64 chunk_key);

  // ---- structures (engine.cpp)
  Structure* structure(i64 id);
  void process(const PendingEvent& e);
  void carve_world(const V3& c, f64 r, std::vector<IVec3>* removed);
  void blast_world(const PendingEvent& e);
  void seed_near(const std::vector<IVec3>& removed);
  // Extracts the structure holding fragment f (bounded: max_nodes / max_radius, 0 = config): a
  // new Structure, or nullptr after detaching it (it reaches no support).
  // detach_free false (bake): a piece reaching no support is removed from the source world.
  Structure* extract(const FragKey& f, i32 max_nodes = 0, f64 max_radius = 0.0, bool detach_free = true);
  void refresh_structures();                 // seeds and stale structures -> (re)extracted
  void step_structures();                    // solves within the work budget, judging
  void judge(Structure& s);
  void detach_unsupported(Structure& s);
  void drop_structure(i64 id);
  // Updates a structure whose chunks were re-fragmented: nodes there retire, the new fragments
  // join with their bonds; the solver keeps its preconditioner. False: re-extract instead.
  bool patch_structure(Structure& s);
  // Appends the nodes of fragments `frags` (clusters of `cell` voxels, 0: one per fragment) and the
  // bonds of `fine` (endpoints: fragment indices, kExisting + existing node, < 0 supports); the
  // fragments become the structure's (the ids of structures they belonged to go to superseded).
  void append_nodes(Structure& s, const std::vector<FragKey>& frags, const std::vector<engine_detail_SecAcc>& fine, i32 cell,
                    std::vector<i64>* superseded);
  i32 cluster_cell(i64 fragments) const;
  void retire_structure_nodes(Structure& s, const std::vector<i32>& list);
  void structure_loads(f64 dt_sub);          // contact forces of bodies on world fragments
  void finish_loads(int substeps);           // smoothing, dead loads, load triggers
  void apply_blast_loads();
  void crack_event(const V3& p, const V3& n, f64 phi);
  std::vector<f64> load_vector(const Structure& s) const;

  // ---- bodies (engine_bodies.cpp)
  // A body from world fragments (their voxels leave the grid), with a velocity field.
  Body* make_body_from_world(const std::vector<FragKey>& frags, const V3& v, const V3& w);
  bool fracture_hook(f64 dt);                // rigid substep hook: body stress, splits
  struct PointForce {
    i32 frag;                                // body fragment
    V3 F, p;                                 // world force and point
  };
  // Stress of body b under forces (and its inertia); returns the bonds to break (worst first).
  std::vector<i32> body_stress(Body& b, const std::vector<PointForce>& forces, bool inertia);
  void rebuild_body_graph(Body& b);
  void refragment_body(Body& b);
  // Splits b into its components (pieces take the velocity field of the pre-solve velocities if
  // use_pre); returns true if it split or changed shape (b is then retired: pending_retire_).
  bool split_body(Body& b, bool use_pre, bool force_replace);
  std::unique_ptr<Body> sub_body(const Body& parent, const std::vector<i32>& voxels, const std::vector<i32>& frag_map,
                                 bool use_pre);
  void carve_bodies(const V3& c, f64 r);
  void blast_bodies(const PendingEvent& e);
  void flush_body_changes();                 // pending additions / retirements into the world
  void announce_bodies();                    // Detached events for new pieces
  void limit_bodies();
  ChunkMesh body_mesh(const Body& b) const;

  // ---- world / streaming / output (engine_world.cpp)
  int stream_update();
  void ensure_chunks(const IVec3& vlo, const IVec3& vhi);
  bool generate_chunk(u64 key);
  void evict_chunk(u64 key);
  void insert_generated(u64 key, bool any, std::vector<Vox>&& voxels);
  bool chunk_resident(const IVec3& cc) const;
  void far_update();
  ChunkMesh far_mesh(i32 tx, i32 ty) const;

  // ---- movers (movers.cpp)
  struct Mover {
    MoverDef def;
    f64 level = 0.0;
    i32 rows = 0;
    i32 move = -1;
    int leg = 0;
    i32 back = 0;
    f64 timer = 0.0;
    bool stopped = false;
    bool disabled = false;
  };
  void step_movers();
  void set_mover_rows(Mover& m, i32 rows);
  bool blocks_player(const Mover& m, i32 rows) const;
  void disable_movers_at(const std::vector<IVec3>& removed);

  EngineConfig cfg_;
  EngineParams par_;
  VoxelGrid grid_;
  std::array<f64, 3> spawn_pos_{0, 0, 0}, spawn_dir_{1, 0, 0};
  std::vector<PendingEvent> queue_;
  std::vector<EngineEvent> events_;
  std::vector<u64> removed_chunks_;
  i64 next_id_ = 1;
  EngineStats st_;
  DesignReport design_;
  std::array<f64, 3> viewer_{0, 0, 0};
  bool viewer_set_ = false;
  CommandLog* log_ = nullptr;
  MeshOptions mesh_base_;                    // (texture / light providers for piece meshes)

  std::unordered_map<u64, FragChunk> frags_;
  std::unordered_map<u64, std::vector<i64>> owner_;  // chunk -> structure id per fragment (0 none)
  std::vector<std::unique_ptr<Structure>> structures_;  // ascending id
  std::vector<IVec3> seeds_;                 // voxels whose structures must be (re)extracted
  std::unordered_map<u64, std::array<f32, 6>> warm_u_;  // fragment identity -> last displacement
  std::unordered_map<u64, BondLoad> judged_;  // bond identity -> load at its last judged state
  bool designed_all_ = false;
  struct DeadLoad {
    IVec3 vox;
    V3 p, F;
  };
  std::unordered_map<i64, std::vector<DeadLoad>> dead_loads_;  // sleeping body -> its resting forces
  struct BlastLoad {
    i32 first;                               // the fragment's identity (chunk, first voxel)
    u64 chunk;
    V3 F;
  };
  std::vector<BlastLoad> blast_loads_;
  std::vector<std::unique_ptr<Body>> pending_add_;
  std::vector<i64> pending_retire_;
  i32 crack_budget_ = 0;
  i64 impact_count_tick_ = 0;

  RigidWorld rigid_;
  std::vector<i64> retired_;                 // bodies removed this tick (front end: gone)
  struct Fading {
    i64 id;
    f64 t;
    V3 pos;
    Quat rot;
  };
  std::vector<Fading> fading_;

  std::vector<Mover> movers_;
  std::unordered_map<u64, std::vector<i32>> mover_cols_;

  // streaming
  std::shared_ptr<ChunkSource> source_;
  StreamConfig stream_{};
  std::unordered_set<u64> generated_;
  std::unordered_map<u64, i32> column_count_;
  std::unordered_map<u64, std::shared_ptr<const std::vector<u8>>> archive_;
  std::unordered_set<u64> far_sent_;
  std::vector<ChunkMesh> far_out_;
  std::vector<std::array<i32, 2>> far_removed_;
};

}  // namespace svx
