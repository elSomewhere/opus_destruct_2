// structvox — the destruction world: the public API of the physics core (docs/CORE.md).
//
// A World owns a voxel grid (world/grid.hpp) and the rigid pieces that broke off it, and runs
// the fragment-graph mechanics:
//   - The free (non-anchored) voxels are grouped into fragments (frag/fragments.hpp): pre-scored
//     rubble pieces. Anchored voxels are supports (bedrock, foundations, kinematic parts).
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

#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "svx/base/vec.hpp"
#include "svx/frag/fragments.hpp"
#include "svx/phys/rigid.hpp"
#include "svx/stress/stress.hpp"
#include "svx/world/grid.hpp"
#include "svx/world/source.hpp"

namespace svx {

namespace world_detail {
struct SecAcc;
}

// Runtime knobs: may change between ticks.
struct WorldParams {
  f64 fragility = 1.0;  // divides every bond strength (> 1: more collapse)
  f64 impact = 1.0;     // scales contact loads (impulse / impact duration): impact severity
  f64 dif = 1.5;        // dynamic increase factor of sudden load changes (1 = static)
  bool paused = false;  // tick() streams only (no commands, no motion)
  bool debug_fields = false;  // judged structures report their chunks changed (debug_field views refresh)
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
  i32 idle_drop_ticks = 1800;      // idle structures without loads are dropped after this
  f64 load_trigger = 0.25;         // re-solve when a node's external load changes by this x its weight ...
  f64 load_trigger_abs = 800.0;    // ... plus this (N)
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
  bool spread_contacts = true;     // stress checks share a piece's contact force over its contacts (least squares)
  f64 fracture_energy = 1.0;       // x the materials' fracture energies (what impacts pay for cracks)
  f64 impact_wave_speed = 400.0;   // m/s: an impact loads a piece over its length / this (crushing slows the wave)
  i32 body_check_ticks = 12;       // steady contact: re-check every so many substeps
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
  // event budgets per tick (cosmetic events beyond them are dropped; piece events never are)
  i32 crack_events_per_tick = 24;  // cracks and dust
  i32 impact_events_per_tick = 6;  // heavy landings
  f64 impact_event_energy = 2e4;   // J dissipated by a landing to report it
};

// Why a piece left the world (WorldEvent::PieceRemoved).
enum class PieceEnd : u8 {
  Split,       // it broke or changed shape (carved, crushed): its parts are new pieces (PieceAdded, parent = it)
  Culled,      // over max_bodies: the smallest sleeping pieces go first
  OutOfWorld,  // fell kill_depth below the world
  Removed,     // remove_piece(), load()
};

struct WorldEvent {
  enum class Kind : u8 {
    PieceAdded,    // id, parent (the piece it broke from; 0: the static world), pos (centre of mass), rot, vel, ang, voxels
    PieceRemoved,  // id, end, pos, rot (its last pose)
    Crack,         // pos, normal, strength (the bond's utilization when it broke, >= 1)
    Impact,        // pos, radius, strength (energy, J): blasts and heavy landings
    Dust,          // pos, vel, radius, voxels; strength 1: material crushed, 0: a shard too small to be a piece
  };
  Kind kind = Kind::Crack;
  PieceEnd end = PieceEnd::Split;
  i64 id = 0, parent = 0;
  V3 pos, vel, ang, normal{0, 0, 1};
  Quat rot;
  f64 radius = 0.0, strength = 0.0;
  i32 voxels = 0;
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
  IVec3 voxel{0, 0, 0};  // the grid voxel hit (the world) or the shape voxel (a piece)
  i64 piece = 0;         // the rigid piece hit (0: the world)
};

struct CollideResult {
  V3 move;  // the part of the requested move that is free
  bool on_ground = false;
};

// One voxel write of World::set_voxels.
struct VoxelEdit {
  IVec3 p{0, 0, 0};
  Vox v = kAir;
};

enum EditFlags : u32 {
  kEditUntracked = 1u << 0,  // not part of the persistence delta (kinematic parts a host rebuilds on load)
  kEditIsolated = 1u << 1,   // the written solid voxels bond to nothing (doors, lifts: kinematic parts)
};

// Diagnostic fields per voxel (World::debug_field).
enum class DebugField : u8 {
  Utilization,  // the worst bond utilization of the voxel's structure node at its last judge (0..255 = 0..1)
  Fragment,     // a colour index of the voxel's fragment (1..255; 0 = none)
};

struct WorldStats {
  f64 tick_ms = 0.0, structural_ms = 0.0, event_ms = 0.0, rigid_ms = 0.0;
  i64 ticks = 0, events = 0;
  i64 voxels = 0, chunks = 0;
  f64 memory_mb = 0.0;
  // structures
  i32 structures = 0, solving = 0;        // registered / being solved now
  i64 solve_nodes = 0;                    // nodes of the structures being solved
  i64 extractions = 0, extracted_nodes = 0, solves = 0, pcg_iters = 0;
  i64 bonds_broken = 0, detached_voxels = 0, detached_pieces = 0;
  f64 max_utilization = 0.0;              // of the last judged round
  // pieces
  i32 bodies = 0, awake = 0, contacts = 0;
  i64 body_checks = 0, body_splits = 0, impacts = 0;
  i64 impact_breaks = 0, steady_breaks = 0;  // bonds broken in pieces by collisions / by resting loads
  i64 pulverized_voxels = 0;                 // crushed to dust
  i64 mode_breaks[4] = {0, 0, 0, 0};         // pieces' bonds broken by mode (none, tension, crush, shear)
  // streaming
  i64 resident_chunks = 0, archived_chunks = 0, generated_total = 0, evicted_total = 0, budget_evicted = 0;
  f64 stream_ms = 0.0;
  // design (bake)
  f64 design_max_utilization = 0.0;
  i64 strengthened_voxels = 0, floating_voxels = 0;
  f64 bake_ms = 0.0;
};

class World {
 public:
  World();
  ~World();
  World(World&&) noexcept;
  World& operator=(World&&) noexcept;
  World(const World&) = delete;
  World& operator=(const World&) = delete;

  void configure(const WorldConfig& c);
  const WorldConfig& config() const { return cfg_; }
  void set_params(const WorldParams& p);
  const WorldParams& params() const { return par_; }

  // ---- content
  // Replaces the world with grid g (the pieces are removed: PieceRemoved events; its chunks are
  // reported changed). Changes from here on are tracked for save_delta().
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
  const DesignReport& design_report() const { return design_; }
  // Streaming: from now on the world is generated by src around the focus points (chunks
  // within load_radius are resident; beyond evict_radius they are dropped, their changes
  // archived). Call after load() of an empty grid of the source's voxel size.
  void enable_streaming(std::shared_ptr<const ChunkSource> src, const StreamConfig& sc);
  bool streaming() const { return source_ != nullptr; }
  const ChunkSource* source() const { return source_.get(); }
  // Where the streamed world must be resident (e.g. the players); the first call also makes
  // the chunks around them resident at once. Ignored when not streaming.
  void set_focus(const V3& p) { set_focus(std::vector<V3>{p}); }
  void set_focus(const std::vector<V3>& points);
  // Generates the chunks overlapping the voxel box [lo, hi) now (e.g. before placing a player).
  void ensure_resident(const IVec3& lo, const IVec3& hi);
  bool chunk_resident(const IVec3& chunk) const;

  // Persistence: the changes since load() as a binary delta against the regenerable base
  // world (a level file, a generator); load_delta() applies one after load() (false: malformed,
  // nothing applied). Untracked edits are not part of it.
  std::vector<u8> save_delta() const;
  bool load_delta(const std::vector<u8>& bytes);
  bool modified() const;

  const VoxelGrid& grid() const { return grid_; }
  f64 voxel_size() const { return grid_.h; }

  // ---- commands
  // Queued: they take effect in the next tick, in call order.
  void carve(const V3& pos, f64 radius);              // removes the voxels in a sphere (not indestructible materials)
  void blast(const V3& pos, f64 radius, f64 energy);  // carves, shatters around the crater, loads the structures near it (J)
  // Immediate. Writes voxels: structures there are extracted again, pieces near are woken
  // (voxels written into a piece push it out). Chunks not resident in a streamed world are
  // generated first. Returns the number of voxels changed.
  i32 set_voxels(const std::vector<VoxelEdit>& edits, u32 flags = 0);
  bool apply_impulse(i64 piece, const V3& point, const V3& impulse);  // N s at a world point
  bool remove_piece(i64 piece);                                         // PieceRemoved (Removed)

  void tick();  // one step of config().dt
  i64 ticks() const { return st_.ticks; }

  // ---- output
  std::vector<WorldEvent> take_events();   // since the last call, in order
  std::vector<u64> take_changed_chunks();  // keys (key3) of chunks whose voxels changed since the last call (sorted)
  std::vector<u64> take_evicted_chunks();  // keys of chunks no longer resident (streaming) since the last call
  std::vector<PieceState> pieces() const;  // id order
  const Body* piece(i64 id) const;         // shape, fragments and state (nullptr: gone)
  const RigidWorld& rigid() const { return rigid_; }

  // ---- queries (no state change)
  RayHit raycast(const V3& origin, const V3& dir, f64 max_dist) const;  // the world's voxels and the pieces
  // Moves the box [min, max] by `move` (per axis, x then y then z) as far as the world's voxels
  // let it (not the pieces): a character controller's sweep; stepping up ledges is the host's
  // business. Boxes and moves beyond 16 m are refused / clamped.
  CollideResult collide(const V3& min, const V3& max, const V3& move) const;
  // Diagnostic field of the voxels of a resident chunk (kChunkVox values, Chunk::v order).
  // (Brings the chunk's fragments up to date: not const.)
  bool debug_field(const IVec3& chunk, DebugField field, std::vector<u8>* out);
  // Utilization of the structure holding a voxel (solved now, gravity and current loads): max
  // phi and the number of bonds at or over 1. -1 if the voxel is in no structure.
  f64 probe_utilization(const IVec3& voxel, i32* over = nullptr);
  void debug_voxel(const IVec3& p);  // prints a voxel's fragment, owner and neighbours (stdout)

  WorldStats stats() const;
  u64 state_hash() const;    // voxels and broken bonds
  u64 session_hash() const;  // + the pieces' poses

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
  void mark_owners_stale(u64 chunk_key, u64 changed = ~0ull);  // owners of chunk_key: stale, `changed` (default: chunk_key) to patch

  // ---- structures (world.cpp)
  Structure* structure(i64 id);
  void process(const PendingEvent& e);
  void carve_world(const V3& c, f64 r, std::vector<IVec3>* removed);
  void blast_world(const PendingEvent& e);
  void seed_near(const std::vector<IVec3>& removed);
  void support_changed(const IVec3& p, std::vector<u64>* chunks);  // (before an anchored voxel goes / after one comes)
  // Extracts the structure holding fragment f (bounded: max_nodes / max_radius, 0 = config): a
  // new Structure, or nullptr after detaching it (it reaches no support).
  // detach_free false (bake): a piece reaching no support is removed from the source world.
  Structure* extract(const FragKey& f, i32 max_nodes = 0, f64 max_radius = 0.0, bool detach_free = true);
  // Streamed worlds are designed on first touch: a structure reaching chunks generated since
  // (not yet designed) is solved under its own weight and its overloaded members strengthened
  // before anything happens to it (an event designs what it will hit before it hits).
  void design_structure(Structure& s, bool dry = false);  // dry: only report
  bool touches_undesigned(const Structure& s) const;
  bool pristine(const Structure& s) const;  // no broken bond, no changed chunk
  void design_near(const V3& c, f64 r);
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
  void append_nodes(Structure& s, const std::vector<FragKey>& frags, const std::vector<world_detail::SecAcc>& fine,
                    i32 cell, std::vector<i64>* superseded);
  i32 cluster_cell(i64 fragments, i32 limit = 0) const;  // (limit: 0 = cluster_nodes)
  void retire_structure_nodes(Structure& s, const std::vector<i32>& list);
  void reseed(const Structure& s);  // seeds for extracting a dropped structure's fragments again
  void structure_loads(f64 dt_sub);          // contact forces of bodies on world fragments
  void finish_loads(int substeps);           // smoothing, dead loads, load triggers
  void apply_blast_loads();
  void crack_event(const V3& p, const V3& n, f64 phi);
  void dust_event(const V3& p, const V3& v, i32 voxels, bool crushed);
  std::vector<f64> load_vector(const Structure& s) const;

  // ---- pieces (world_pieces.cpp)
  // A body from world fragments (their voxels leave the grid), with a velocity field.
  Body* make_body_from_world(const std::vector<FragKey>& frags, const V3& v, const V3& w);
  int fracture_hook(f64 dt);                 // rigid substep hook: body stress, splits (0 none, 1 bodies changed, 2 solve again)
  struct PointForce {
    i32 frag;                                // body fragment
    V3 F, p;                                 // world force and point
  };
  // Stress of body b under forces (and its inertia); returns the bonds to break (worst first).
  // energy >= 0: the cracks may cost at most this much (J): an impact pays for its fractures.
  // crushed: (optional) the broken bonds that failed by crushing.
  std::vector<i32> body_stress(Body& b, const std::vector<PointForce>& forces, bool inertia, f64 energy = -1.0,
                               std::vector<i32>* crushed = nullptr);
  // The check itself touches only the piece (pieces are checked concurrently); what it did for
  // the stats and the events is applied afterwards, in body order.
  struct StressOut {
    std::vector<i32> broken, crushed;
    std::vector<std::pair<V3, V3>> cracks;  // world position, normal
    i64 checks = 0, pcg_iters = 0, impact_breaks = 0, steady_breaks = 0;
    i64 modes[4] = {0, 0, 0, 0};
    f64 assemble_ms = 0.0, solve_ms = 0.0;  // (profiling)
  };
  void body_stress_run(Body& b, const std::vector<PointForce>& forces, bool inertia, f64 energy, StressOut& o);
  void apply_stress_out(const StressOut& o);
  // Crushed material turns to gravel and dust: the fragments on the lighter side of crushed
  // bonds leave the piece (dust events). Returns whether the shape changed.
  bool pulverize(Body& b, const std::vector<i32>& crushed);
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
  void announce_bodies();                    // PieceAdded events for new pieces
  void remove_bodies(std::vector<i64> ids, PieceEnd end);  // PieceRemoved events (announced pieces)
  void limit_bodies();

  // ---- streaming (world_io.cpp)
  int stream_update();
  void ensure_chunks(const IVec3& vlo, const IVec3& vhi);
  bool generate_chunk(u64 key);
  void evict_chunk(u64 key);
  void insert_generated(u64 key, bool any, std::vector<Vox>&& voxels);
  f64 focus_distance(const IVec3& chunk) const;  // horizontal, m (to the nearest focus point)

  WorldConfig cfg_;
  WorldParams par_;
  VoxelGrid grid_;
  std::vector<PendingEvent> queue_;
  std::vector<WorldEvent> events_;
  std::vector<u64> evicted_chunks_;
  i64 next_id_ = 1;
  WorldStats st_;
  DesignReport design_;
  std::vector<V3> focus_;
  bool focus_set_ = false;

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
  i32 impact_budget_ = 0;
  bool rollback_ = false;  // (fracture hook) a part of some size came apart: the contact step is solved again
  std::unordered_set<u64> undesigned_;  // streamed chunks generated and not designed yet
  i32 ensuring_ = 0;  // (nesting of first-touch chunk generation)
  i64 impact_count_tick_ = 0;

  RigidWorld rigid_;

  // streaming
  std::shared_ptr<const ChunkSource> source_;
  StreamConfig stream_{};
  std::unordered_set<u64> generated_;
  std::unordered_map<u64, i32> column_count_;
  std::unordered_map<u64, std::shared_ptr<const std::vector<u8>>> archive_;
};

}  // namespace svx
