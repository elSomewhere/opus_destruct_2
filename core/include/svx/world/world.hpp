// structvox — the destruction world: the public API of the physics core (docs/CORE.md).
//
// A World owns voxel grids (world/grid.hpp) and the rigid pieces that broke off them, and runs
// the fragment-graph mechanics:
//   - Grids: the world grid (id 0: its axes the world's; loaded or streamed) and oriented grids,
//     each a voxel lattice of the same voxel size placed with a frame of its own (a building at an
//     angle, a diagonal brace, a tilted slab: docs/GRIDS.md). Where the voxels of two grids meet,
//     they are bonded (junctions), so structures and pieces span grids.
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
#include <functional>
#include <map>
#include <memory>
#include <string>
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
struct VoxelAt;
struct JSample;
class ChangeArchive;
}

// Grids (docs/GRIDS.md): the world grid is kWorldGrid; oriented grids have ids from 1.
using GridId = u32;
constexpr GridId kWorldGrid = 0;

// Where a grid is: its voxel p (integer coordinates in the grid) is centred at
// origin + rot (h p) in the world.
struct GridFrame {
  V3 origin;
  Quat rot;
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
  f64 piece_mb = 256.0;           // rigid pieces: beyond (as beyond max_bodies) the smallest are culled, sleeping first
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
  // junctions (bonds between grids, docs/GRIDS.md): a face bonds to another grid's voxels where
  // its samples (junction_samples^2 per face), pushed out by junction_reach voxels, land in them
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
  IVec3 voxel{0, 0, 0};  // the grid voxel hit (a static grid) or the shape voxel (a piece)
  i64 piece = 0;         // the rigid piece hit (0: a static grid)
  GridId grid = 0;       // the static grid hit (piece 0)
  i32 shape = 0;         // the piece's shape hit (piece != 0)
};

struct CollideResult {
  V3 move;  // the part of the requested move that is free
  bool on_ground = false;
};

// World::sweep: how far a box moves along a direction before it touches a static grid.
struct SweepHit {
  bool hit = false;
  f64 t = 1.0;     // the fraction of the move that is free
  V3 normal;       // the touched surface's normal (against the move)
  GridId grid = 0;
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
  i64 body_checks = 0, body_splits = 0, impacts = 0;
  i64 impact_breaks = 0, steady_breaks = 0;  // bonds broken in pieces by collisions / by resting loads
  i64 pulverized_voxels = 0;                 // crushed to dust
  i64 chip_releases = 0;                     // pieces going on without the chips that broke off with their contacts
  i64 mode_breaks[4] = {0, 0, 0, 0};         // pieces' bonds broken by mode (none, tension, crush, shear)
  // streaming
  i64 resident_chunks = 0, archived_chunks = 0, generated_total = 0, evicted_total = 0, budget_evicted = 0;
  f64 stream_ms = 0.0;
  f64 archive_used_mb = 0.0, archive_capacity_mb = 0.0;
  i64 forgotten_regions = 0, forgotten_chunks = 0;  // changes forgotten (the archive full, or forget_after_s)
  // memory budgets (MemoryBudget): what they removed
  i64 culled_pieces = 0, dropped_structures = 0, dropped_fragment_caches = 0, dropped_events = 0;
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
// other (through their host).
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
  World(World&&) noexcept;
  World& operator=(World&&) noexcept;
  World(const World&) = delete;
  World& operator=(const World&) = delete;

  void configure(const WorldConfig& c);
  const WorldConfig& config() const { return cfg_; }
  void set_params(const WorldParams& p);
  const WorldParams& params() const { return par_; }

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
  // world (a level file, a generator, and the level's oriented grids); load_delta() applies one
  // after load() and the level's grids (false: malformed, nothing applied). Untracked edits are
  // not part of it.
  std::vector<u8> save_delta() const;
  bool load_delta(const std::vector<u8>& bytes);
  bool modified() const;

  const VoxelGrid& grid() const { return grid_; }
  f64 voxel_size() const { return grid_.h; }

  // ---- grids (docs/GRIDS.md)
  // Adds an oriented grid: `voxels` in the grid's own coordinates (its voxel size is the
  // world's), placed at `frame`. It is structure like the world grid's voxels: anchored voxels
  // are supports, free ones form fragments and structures, and faces that meet other grids'
  // voxels bond to them (junctions). A grid of free voxels touching nothing falls as a piece (a
  // rotated object dropped in). base: part of the level (not saved in deltas: a host that loads
  // the level adds its grids again, in the same order, before load_delta; their changes are
  // saved like the world grid's); else a change of this session, saved whole. Returns its id
  // (0: refused - a frame or voxels out of range, or from inside a tick).
  GridId add_grid(const GridFrame& frame, VoxelGrid&& voxels, bool base = true);
  // Removes an oriented grid (its voxels; pieces that broke off it stay). GridRemoved.
  bool remove_grid(GridId id);
  std::vector<GridId> grids() const;                  // the oriented grids, ascending ids
  const VoxelGrid* grid(GridId id) const;             // kWorldGrid: grid(); nullptr: none
  bool grid_frame(GridId id, GridFrame* out) const;   // false: none
  // A grid's point (its coordinates, metres: voxel p's centre is h p) in the world, and back.
  V3 grid_to_world(GridId id, const V3& lattice) const;
  V3 world_to_grid(GridId id, const V3& world) const;

  // ---- commands
  // Queued: they take effect in the next tick, in call order.
  void carve(const V3& pos, f64 radius);              // removes the voxels in a sphere (not indestructible materials)
  void blast(const V3& pos, f64 radius, f64 energy);  // carves, shatters around the crater, loads the structures near it (J)
  // Immediate. Writes voxels: structures there are extracted again, pieces near are woken
  // (voxels written into a piece push it out). Chunks not resident in a streamed world are
  // generated first. Returns the number of voxels changed.
  i32 set_voxels(const std::vector<VoxelEdit>& edits, u32 flags = 0);
  i32 set_voxels(GridId grid, const std::vector<VoxelEdit>& edits, u32 flags = 0);  // (in a grid's coordinates)
  bool apply_impulse(i64 piece, const V3& point, const V3& impulse);  // N s at a world point
  bool remove_piece(i64 piece);                                         // PieceRemoved (Removed)

  void tick();  // one step of config().dt
  i64 ticks() const { return st_.ticks; }

  // ---- output
  std::vector<WorldEvent> take_events();   // since the last call, in order
  std::vector<u64> take_changed_chunks();  // keys (key3) of the world grid's chunks whose voxels changed since the last call (sorted)
  std::vector<u64> take_evicted_chunks();  // keys of chunks no longer resident (streaming) since the last call
  // The oriented grids' chunks whose voxels changed since the last call (by grid, then chunk).
  std::vector<GridChunk> take_changed_grid_chunks();
  std::vector<PieceState> pieces() const;  // id order
  const Body* piece(i64 id) const;         // shapes, fragments and state (nullptr: gone)
  const RigidWorld& rigid() const { return rigid_; }

  // ---- extensions: layers, loads, forces, systems (world_ext.cpp)
  // Layers (grid.hpp): per-voxel byte channels. Layer kDamageLayer is the core's: a voxel's
  // damage (0 intact .. 255 no strength left) takes the strength of every bond section it is in
  // (fire, corrosion, rot, ... write it). Pieces carry their voxels' layer values.
  static constexpr int kDamageLayer = 0;
  // Its index (the existing one for the same layer added before); -1: full (kMaxLayers), or a
  // different layer (persistence, binding) under that name.
  int add_layer(const LayerSpec& spec);
  int layer_index(const std::string& name) const { return grid_.layer_index(name); }
  u8 layer(int L, const IVec3& p) const { return grid_.layer(L, p); }
  u8 layer(GridId grid, int L, const IVec3& p) const;
  i32 set_layer(int L, const std::vector<LayerEdit>& edits);  // returns the values changed
  i32 set_layer(GridId grid, int L, const std::vector<LayerEdit>& edits);
  std::vector<u64> take_layer_changes(int L) { return grid_.take_layer_dirty(L); }
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
  const std::vector<std::shared_ptr<WorldSystem>>& systems() const { return systems_; }

  // ---- queries (no state change)
  // A point commands and queries accept: finite, and within the voxel key range (kVoxelLimit).
  bool in_range(const V3& p) const;
  RayHit raycast(const V3& origin, const V3& dir, f64 max_dist) const;  // the grids' voxels and the pieces
  // Moves the box [min, max] by `move` (per axis, x then y then z) as far as the grids' voxels
  // let it (not the pieces): a character controller's sweep; stepping up ledges is the host's
  // business. Boxes and moves beyond 16 m are refused / clamped.
  CollideResult collide(const V3& min, const V3& max, const V3& move) const;
  // How far the box [min, max] moves along `move` (any direction) before it touches a grid's
  // voxels, and the normal there: a controller slides along rotated walls with it.
  SweepHit sweep(const V3& min, const V3& max, const V3& move) const;
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
  struct Structure;
  struct GridState;
  struct JunctionScratch;
  struct PendingEvent {
    bool blast = false;
    V3 pos;
    f64 radius = 0.0, energy = 0.0;
  };
  // Internally a grid is its slot (grids_ index; the world grid's is 0), not its id.
  // A voxel of a grid.
  struct GVox {
    IVec3 p{0, 0, 0};
    u16 grid = 0;
  };
  // a fragment's identity: grid, chunk and its first voxel (stable while that voxel stays)
  struct FragKey {
    u64 chunk = 0;
    i32 idx = -1;  // index in the chunk's FragChunk (valid for the chunk's current fragments)
    u16 grid = 0;
  };
  // a chunk of a grid, ordered by grid then chunk (the world grid's first, in key order)
  struct GKey {
    u16 grid = 0;
    u64 chunk = 0;
    bool operator==(const GKey& o) const { return grid == o.grid && chunk == o.chunk; }
    bool operator<(const GKey& o) const { return grid < o.grid || (grid == o.grid && chunk < o.chunk); }
  };
  struct GKeyHash {
    size_t operator()(const GKey& k) const {
      u64 x = k.chunk ^ (static_cast<u64>(k.grid) * 0x9E3779B97F4A7C15ull);
      x = (x ^ (x >> 31)) * 0xBF58476D1CE4E5B9ull;
      return static_cast<size_t>(x ^ (x >> 29));
    }
  };

  // ---- grids (world_grids.cpp)
  VoxelGrid& vg(u16 g);
  const VoxelGrid& vg(u16 g) const;
  GridState& gs(u16 g);
  const GridState& gs(u16 g) const;
  bool live(u16 g) const { return g < grids_.size() && grids_[g] != nullptr; }
  i32 slot_of(GridId id) const;              // -1: none
  GridId id_of(u16 g) const;
  const LatticeXf& xf_of(u16 g) const;       // lattice -> world
  V3 voxel_centre(const GVox& v) const;      // in the world
  u64 frag_ident_of(const FragKey& f, i32 first) const;  // (identities: the world grid's as they always were)
  u64 residency_key(u16 g, u64 chunk) const; // (warm starts, reference loads: dropped with their chunk or grid)
  bool resident_key(u64 k) const;
  void refresh_grid_box(u16 g);              // (after its chunks changed)
  void grids_changed();                      // (added, removed, grown: junction candidates are found again)
  // Other grids whose voxels a junction sample of this chunk may reach (the world grid first).
  const std::vector<u16>& near_grids(u16 g, u64 chunk);
  // Which side of a node of grid g a junction sample bonds it on (0..5: the axis and sign, in g's
  // lattice, of the normal out of the node): a node's junction supports are one bond per side
  // and grid, as its supports in its own grid are one per face direction.
  int junction_side(u16 g, const world_detail::JSample& j, bool fwd) const;
  std::vector<StaticGrid> static_grids() const;  // (the rigid bodies' static world)
  void remove_grid_slot(u16 g, bool event);
  void tear_voxel(const GVox& v);            // (its faces bond to nothing any more: intra and junctions)
  // Junction samples (world_grids.cpp): of a chunk's faces into other grids, cached per extraction.
  const std::vector<world_detail::JSample>& junction_fwd(JunctionScratch& js, u16 g, u64 chunk);
  // The samples of other grids' faces that land in fragment f.
  const std::vector<world_detail::JSample>* junction_rev(JunctionScratch& js, const FragKey& f);
  // Calls fn(sample, fwd) for fragment f's junction samples: its faces' into other grids (fwd),
  // and other grids' faces' landing in it.
  template <class Fn>
  void each_junction(JunctionScratch& js, const FragKey& f, Fn&& fn);

  // ---- fragments
  FragChunk& frag_chunk(u16 g, const IVec3& cc);  // (re)builds when stale
  FragChunk& adopt_fragments(u16 g, u64 key, FragChunk&& nf);  // (a chunk's new fragments into the cache)
  void prefragment(u16 g, const IVec3& seed_chunk, f64 max_radius);  // (the chunks a walk can reach, in parallel)
  FragChunk* frag_chunk_if(u16 g, u64 key);  // current or nullptr (no rebuild)
  FragChunk* frag_chunk_if(const FragKey& f) { return frag_chunk_if(f.grid, f.chunk); }
  bool frag_at(const GVox& v, FragKey* out);  // the free fragment holding voxel v
  i64 owner_of(const FragKey& f) const;      // structure id holding it (0: none)
  world_detail::VoxelAt voxel_at(const GVox& v) const;                  // (sections: the grid's voxel)
  world_detail::VoxelAt piece_voxel_at(const Body& b, i32 shape, const IVec3& p) const;  // (a piece's shape voxel)
  void voxels_of(const FragKey& f, std::vector<IVec3>& out);  // (its grid's coordinates)
  u8 frag_class(const FragKey& f);           // weakest design class of its voxels
  V3 frag_com(const FragKey& f);             // its centre of mass in the world
  // owners of a grid's chunk: stale, `changed` (default: that chunk) to patch
  void mark_owners_stale(u16 g, u64 chunk_key, GKey changed = GKey{0xFFFF, 0});

  // ---- structures (world.cpp)
  Structure* structure(i64 id);
  void process(const PendingEvent& e);
  void carve_world(const V3& c, f64 r, std::vector<GVox>* removed);
  void blast_world(const PendingEvent& e);
  void seed_near(const std::vector<GVox>& removed);
  void support_changed(const GVox& v, std::vector<GKey>* chunks);  // (before an anchored voxel goes / after one comes)
  void prune_caches();
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
  void design_node(const Structure& s, i32 node, u8 cls, i64* strengthened);  // (its voxels to class cls)
  void refresh_structures();                 // seeds and stale structures -> (re)extracted
  void step_structures();                    // solves within the work budget, judging
  void judge(Structure& s);
  void break_structure_bond(Structure& s, i32 b);  // (its faces and junction samples, in the grids)
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
  i32 world_set_voxels(u16 g, const std::vector<VoxelEdit>& edits, u32 flags);

  // ---- pieces (world_pieces.cpp)
  // A body from world fragments (their voxels leave their grids), with a velocity field.
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
    f64 spent = 0.0;                        // J: what an impact's cracks cost
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
  // carried: this substep's contact impulses on b (chips that broke off with most of them take
  // them along, but for what crushing them cost: the rest of the piece goes on without); spent:
  // the fracture energy of the step's cracks.
  struct Carried {
    i32 frag = -1;
    V3 J, p;         // (impulse on b, world point)
    f64 e = 0.0;     // (the kinetic energy it took out of the collision, b's share)
  };
  bool split_body(Body& b, bool use_pre, bool force_replace, const std::vector<Carried>* carried = nullptr, f64 spent = 0.0);
  void wake_around(const Body& b);  // (the sleepers touching b's box)
  // A voxel of a body: its shape and cell.
  struct SVox {
    u16 shape = 0;
    i32 cell = 0;
  };
  std::unique_ptr<Body> sub_body(const Body& parent, const std::vector<SVox>& voxels, bool use_pre);
  void carve_bodies(const V3& c, f64 r);
  void blast_bodies(const PendingEvent& e);
  void flush_body_changes();                 // pending additions / retirements into the world
  void announce_bodies();                    // PieceAdded events for new pieces
  void remove_bodies(std::vector<i64> ids, PieceEnd end);  // PieceRemoved events (announced pieces)
  void limit_bodies();
  static i64 body_bytes(const Body& b);

  // ---- streaming (world_io.cpp)
  int stream_update();
  void ensure_chunks(const IVec3& vlo, const IVec3& vhi);
  bool generate_chunk(u64 key);
  void evict_chunk(u64 key);
  void insert_generated(u64 key, bool any, std::vector<Vox>&& voxels);
  f64 focus_distance(const IVec3& chunk) const;  // horizontal, m (to the nearest focus point)
  void generate_grids(u64 key);              // (the source's grids at home in a chunk just generated)
  void evict_grid(u16 g, u64 home_region);   // (a streamed grid out of range: its changes archived)
  // add_grid's work: id 0 = the next; home: the world chunk a streamed grid came with (~0: none);
  // seed: extract its structures at the next tick (a grid added after the design pass)
  GridId add_grid_impl(const GridFrame& frame, VoxelGrid&& voxels, bool base, GridId id, u64 home, bool seed);
  // A grid's changes as saved (its id, whether a level's, its frame, its chunk records: the
  // changed ones, or all of them for a grid of this session), and applying one to a grid.
  std::vector<u8> grid_entry(u16 g) const;
  bool apply_grid_entry(u16 g, const std::vector<u8>& entry, std::vector<u64>* touched);
  SweepHit sweep_grids(const V3& mn, const V3& mx, const V3& mv, bool world_grid) const;

  WorldConfig cfg_;
  WorldParams par_;
  VoxelGrid grid_;                           // the world grid (slot 0)
  std::vector<std::unique_ptr<GridState>> grids_;  // by slot (nullptr: free); slot 0: the world grid's state
  std::unordered_map<GridId, u16> slots_;    // oriented grid id -> slot
  GridId next_grid_ = 1;
  i32 oriented_ = 0;                         // oriented grids there are (none: no junctions anywhere)
  u64 grid_epoch_ = 1;                       // (junction candidates: found again when it changes)
  std::vector<GridChunk> grid_dirty_;        // (oriented grids' changed chunks the host has not taken)
  std::vector<GridId> removed_base_;         // base grids removed since load (saved in deltas)
  std::vector<PendingEvent> queue_;
  std::vector<WorldEvent> events_;
  std::vector<u64> evicted_chunks_;
  i64 next_id_ = 1;
  WorldStats st_;
  DesignReport design_;
  std::vector<V3> focus_;
  bool focus_set_ = false;

  FragChunk empty_frags_;                    // (frag_chunk of a chunk that is not there)
  std::vector<std::unique_ptr<Structure>> structures_;  // ascending id
  std::vector<GVox> seeds_;                  // voxels whose structures must be (re)extracted
  i64 fresh_from_ = INT64_MAX;               // (during a refresh: the first id made in it)
  // (both keep the chunk of the node they belong to: dropped with it when it leaves)
  struct WarmStart {
    u64 chunk = 0;
    std::array<f32, 6> u{};
  };
  struct Judged {
    u64 chunk = 0;
    BondLoad load;
  };
  std::unordered_map<u64, WarmStart> warm_u_;  // fragment identity -> last displacement
  std::unordered_map<u64, Judged> judged_;     // bond identity -> load at its last judged state
  u64 node_chunk(const Structure& s, i32 node) const;
  bool designed_all_ = false;
  bool in_tick_ = false;  // (a system calling tick / load from inside a tick is refused)
  struct DeadLoad {
    GVox vox;
    V3 p, F;
  };
  std::unordered_map<i64, std::vector<DeadLoad>> dead_loads_;  // sleeping body -> its resting forces
  struct BlastLoad {
    i32 first;                               // the fragment's identity (grid, chunk, first voxel)
    u64 chunk;
    u16 grid;
    V3 F, at;                                // (at: the fragment's centre of mass)
  };
  std::vector<BlastLoad> blast_loads_;
  std::vector<std::unique_ptr<Body>> pending_add_;
  std::vector<i64> pending_retire_;
  i32 crack_budget_ = 0;
  i32 impact_budget_ = 0;
  bool rollback_ = false;  // (fracture hook) a part of some size came apart: the contact step is solved again
  i32 ensuring_ = 0;  // (nesting of first-touch chunk generation)
  i64 impact_count_tick_ = 0;

  RigidWorld rigid_;
  std::vector<StaticGrid> statics_;          // (the rigid bodies' static world of this tick)

  // streaming
  std::shared_ptr<const ChunkSource> source_;
  StreamConfig stream_{};
  std::unordered_set<u64> generated_;
  std::unordered_map<u64, i32> column_count_;
  std::unique_ptr<world_detail::ChangeArchive> archive_;
  std::unordered_map<u64, i32> region_resident_;  // region -> resident chunks
  std::unordered_map<u64, std::vector<u16>> home_grids_;  // (streamed) chunk key -> the grids at home in it
  u64 region_of(u64 chunk_key) const;
  void unload_sleepers(const std::vector<u64>& chunks,
                       const std::function<void(const Body&, const std::function<void(u64)>&)>& chunks_of);
  // Keeps a chunk's changes (the archive; a full bounded archive forgets old regions first).
  void archive_record(u64 key, const std::vector<u8>& rec, u64 region);
  bool forget_regions(size_t need, u64 keep);  // (need bytes free; `keep`: never this region)
  void forget_region(u64 region);
  void forget_stale_regions();

  // extensions
  std::vector<LayerSpec> layer_specs_;
  std::vector<std::shared_ptr<WorldSystem>> systems_;
  std::map<u64, std::vector<VoxelLoad>> loads_;  // group -> loads (ordered: sums in the same order everywhere)
  std::unordered_set<u64> host_dirty_;            // chunks changed since the host last took them
  bool host_dirty_all_ = false;
  std::vector<u64> sys_changed_, sys_generated_, sys_evicted_;  // (for the systems' next step)
  void refresh_strengths(const std::vector<GVox>& voxels);  // (their damage changed)
  i32 world_set_layer(u16 g, int L, const std::vector<LayerEdit>& edits);
  void finish_tick_changes();                              // voxel changes: to the systems and the host
  void step_systems(bool step = true);  // (notifications; and the steps unless paused)
  void add_external_loads();                               // (finish_loads)
  void add_loads_to(const Structure& s, std::vector<f64>& F) const;  // (design solves)

  // memory budgets (MemoryBudget)
  void enforce_budgets();
  void trim_fragment_caches();
  void trim_structures();
  void trim_output();
  i64 structure_bytes(const Structure& s) const;
};

}  // namespace svx
