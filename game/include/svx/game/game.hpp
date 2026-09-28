// structvox game — the prototype game harness around the physics core (docs/CORE.md: layers).
//
// A Game owns a World (svx/world/world.hpp) and adds what the prototype game needs on top of it:
//   - a viewer (the player's eye): the world's streaming focus, walk-over triggers, movers that
//     wait for the player;
//   - movers (doors, lifts, ...) and the level's triggers (use, walk-over and shot resolvers);
//   - output for the renderer: chunk meshes (with debug views), piece meshes (sent once, in world
//     coordinates) and poses, fading of culled pieces, the far render tier of streamed levels;
//   - a command log (record / replay / lockstep);
//   - the environment (svx_env: fire, smoke, ...) and its output for the renderer (flames,
//     charring, smoke); blasts raise dust into the smoke.
// The core never sees any of it: a different game (or tool) builds its own harness on World.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "svx/env/env.hpp"
#include "svx/game/movers.hpp"
#include "svx/game/source.hpp"
#include "svx/mesh/mesher.hpp"
#include "svx/world/world.hpp"

namespace svx {

class CommandLog;

struct GameParams {
  f64 fragility = 1.0;  // WorldParams
  f64 impact = 1.0;
  f64 dif = 1.5;
  int debug_view = 0;   // 0 none, 1 utilization (last judged state), 2 fragments
  bool paused = false;
};

// An event for the front end (the web worker protocol, docs/API.md).
struct GameEvent {
  // Remesh: a piece's new mesh (its charring or glow changed), in world coordinates at its pose
  // now: drawn from then on at the poses that follow (as a Detached event's).
  enum class Kind : u8 { Detached, Crack, Impact, Dust, Splash, Remesh };
  Kind kind = Kind::Crack;
  i64 id = 0;
  V3 pos, vel, ang, normal{0, 0, 1};
  f64 radius = 0.0, strength = 0.0;  // Crack: utilization; Impact: energy (J); Dust: 1 crushed, 0 a shard; Splash: kg m/s
  i32 voxels = 0;
  ChunkMesh mesh;  // Detached: the piece's mesh in world coordinates at pos (drawn at its PiecePose)
};

// A piece's pose for the front end: its mesh (sent in world coordinates) is drawn at
// pos + rot (p - the centre it was sent at).
struct PiecePose {
  i64 id = 0;
  V3 pos;
  Quat rot;
  f64 opacity = 1.0;
};

struct GameStats : WorldStats {
  f64 mesh_ms = 0.0;
  i32 movers = 0;
  // environment
  i32 fire_hot = 0, fire_burning = 0;
  i32 smoke_cells = 0, smoke_blocks = 0;
  i32 water_active = 0, water_loads = 0, floating = 0;
  f64 env_ms = 0.0;
};

// An oriented grid's place for the front end (docs/GRIDS.md): its frame in the world, voxel
// size, and the velocity field it moves with (a kinematic body's: v + w x (X - c); zero for the
// static world's). Its chunk meshes and occupancy are in its lattice.
struct GridView {
  GridId id = 0;
  V3 origin;
  Quat rot;
  f64 voxel_size = 0.0;
  KinematicId body = 0;
  V3 vel, ang, centre;
};

// A machine's motion: how the game drives a kinematic body of the level every tick (World::
// drive_kinematic), a function of time (the same in every session and replay).
struct MachineDrive {
  enum class Kind : u8 {
    Oscillate,  // along the axis, from its rest pose out by amplitude (m) and back, eased
    Spin,       // about the axis (through its origin) at amplitude (rad/s)
    Swing,      // about the axis, from its rest pose by amplitude (rad) and back, eased
  };
  Kind kind = Kind::Spin;
  V3 axis{0, 0, 1};     // (world)
  f64 amplitude = 0.0;
  f64 period = 10.0;    // s (oscillate, swing: there and back)
  f64 phase = 0.0;      // s
};

// Something a level drops into its world when play starts (after the bake and a saved session's
// changes): a free object of this session, a piece from the first tick (crates on a turntable).
struct Drop {
  GridDesc desc;
  VoxelGrid voxels;
};

// A joint for the renderer: its ends in the world (a rope is drawn between them).
struct JointView {
  JointId id = 0;
  JointType type = JointType::Ball;
  V3 a, b;
};

// A flame for the renderer: a burning voxel (world position, degC).
struct FlamePoint {
  V3 pos;
  f32 heat = 0.0f;
};

// Smoke for the renderer: a cell of the smoke field (its centre, density: about 1 is thick).
struct SmokePoint {
  V3 pos;
  f32 density = 0.0f;
};

class Game {
 public:
  Game();
  ~Game();
  Game(Game&&);
  Game& operator=(Game&&);
  Game(const Game&) = delete;
  Game& operator=(const Game&) = delete;

  World& world() { return world_; }
  const World& world() const { return world_; }
  const VoxelGrid& grid() const { return world_.grid(); }

  void configure(const WorldConfig& c) { world_.configure(c); }
  const WorldConfig& config() const { return world_.config(); }
  void set_params(const GameParams& p);
  const GameParams& params() const { return par_; }
  f64 fade_time = 1.0;  // s: culled pieces fade out
  i32 max_events = 4096;  // events (with piece meshes) not taken: beyond, the oldest go

  // Levels. load() replaces everything (movers too); load_streaming() then streams a level
  // from src around the viewer (the grid: empty, of the source's voxel size).
  void load(VoxelGrid&& g, const V3& spawn_pos, const V3& spawn_dir);
  void load_streaming(std::shared_ptr<const GameSource> src, f64 h, const StreamConfig& sc = {}, const FarConfig& far = {});
  bool bake(f64* ms = nullptr) { return world_.bake(ms); }
  std::vector<u8> save_delta() const { return world_.save_delta(); }
  bool load_delta(const std::vector<u8>& bytes);  // (movers keep their state)
  V3 spawn_pos() const { return spawn_pos_; }
  V3 spawn_dir() const { return spawn_dir_; }

  // Player commands (logged when recording).
  void set_viewer(const V3& eye);
  void carve(const V3& pos, f64 radius);
  void blast(const V3& pos, f64 radius, f64 energy);
  bool use(const V3& eye, const V3& dir, f64 reach = 2.0);
  void ignite(const V3& pos, f64 radius);      // sets fire to what burns in the sphere
  void extinguish(const V3& pos, f64 radius);  // puts out and cools the sphere
  void pour(const V3& pos, f64 radius);        // fills the air in the sphere with water
  void heat(const V3& pos, f64 radius, f64 celsius);  // brings solids in the sphere to (at least) this
  void drain(const V3& pos, f64 radius);             // removes the water in the sphere
  // Settings, logged like the other commands (a session replays with the same ones): the
  // environment's (env_param_*, svx/env/env.hpp) and the world's (tunable_*,
  // svx/world/tunables.hpp), by index or name. False: unknown (or its system is not there).
  bool set_env(i32 index, f64 value);
  bool set_env(const char* name, f64 value) { return set_env(env_param_index(name), value); }
  bool set_tunable(i32 index, f64 value);
  bool set_tunable(const char* name, f64 value);
  void tick();
  i64 ticks() const { return world_.ticks(); }
  void record_to(CommandLog* log) { log_ = log; }

  // Machines: kinematic bodies of the level the game drives (a lift, a turntable, a drawbridge,
  // a crane's jib); their pose is a function of the ticks. Drops: objects dropped in when play
  // starts (the first tick).
  bool add_machine(KinematicId body, const MachineDrive& drive);
  void add_drop(Drop&& d) { drops_.push_back(std::move(d)); }
  i32 machine_count() const { return static_cast<i32>(machines_.size()); }

  // Movers (game/src/movers.cpp).
  i32 add_mover(const MoverDef& d);
  bool activate_mover(i32 id, i32 move = 0);
  i32 mover_at(const IVec3& voxel) const;
  i32 mover_count() const { return static_cast<i32>(movers_.size()); }
  const MoverDef* mover_def(i32 id) const;
  i32 mover_rows(i32 id) const;
  bool mover_busy(i32 id) const;
  f64 mover_position(i32 id) const;
  // Level triggers: which moves a use (the voxel and face hit), a walk (the viewer's move) or a
  // shot (a carve's centre) starts.
  std::function<std::vector<MoverTrigger>(const IVec3& voxel, int face)> use_resolver;
  std::function<std::vector<MoverTrigger>(const V3& from, const V3& to)> walk_resolver;
  std::function<std::vector<MoverTrigger>(const V3& at)> shot_resolver;

  // Output for the front end.
  // (the oriented grids' chunks among them: ChunkMesh::grid their id, their vertices in the
  // grid's lattice (metres), placed in the world by its view: take_grid_views)
  std::vector<ChunkMesh> take_meshes(const MeshOptions& base);
  std::vector<u64> take_removed_chunks();
  // The oriented grids' chunks whose meshes go (emptied, or their grid removed).
  std::vector<GridChunk> take_removed_grid_chunks();
  // The player's collision occupancy of a world chunk (kChunkVox / 8 bytes of bits; returns 0 no
  // chunk, 1 all solid, 2 mixed), and of an oriented grid's chunk in its lattice: a front end
  // sweeps the player against both exactly (World::collide's rules).
  int chunk_occupancy(const IVec3& chunk, u8* bits) const;
  int grid_chunk_occupancy(GridId grid, const IVec3& chunk, u8* bits) const;
  // The grids whose place changed since the last call (they came, were moved, their kinematic
  // body moves), and the grids gone.
  std::vector<GridView> take_grid_views();
  std::vector<GridId> take_removed_grids();
  // The joints now (to be drawn).
  std::vector<JointView> joint_views() const;
  std::vector<ChunkMesh> take_far_meshes();
  std::vector<std::array<i32, 2>> take_far_removed();
  std::vector<GameEvent> take_events();
  std::vector<PiecePose> pieces() const;
  const FarConfig& far_config() const { return far_; }
  // The flames burning now (at most max: an even sample of them).
  std::vector<FlamePoint> flames(i32 max) const;
  // The smoke (at most max cells: the densest).
  std::vector<SmokePoint> smoke(i32 max) const;
  // Water surfaces of chunks whose water changed (meshed again at most every water_remesh_s;
  // mesh_water, texture kWaterTexture), and chunks whose water is gone or evicted.
  std::vector<ChunkMesh> take_water_meshes();
  std::vector<u64> take_water_removed();
  f64 water_remesh_s = 0.1;
  // (read only: its changes go through the commands above, which are logged)
  const Environment& env() const { return env_; }
  // Whether the chunk's last mesh (take_meshes) came from a change of its decoration only
  // (charring, glow): its voxels, and so its occupancy, are as before.
  bool decoration_only(u64 chunk) const { return decor_only_.count(chunk) > 0; }
  f64 char_remesh_s = 1.0;  // s: charring chunks are meshed again at most this often

  GameStats stats() const;
  u64 session_hash() const;  // the world's + the movers'

 private:
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
  void step_machines();
  void set_mover_rows(Mover& m, i32 rows);
  bool blocks_player(const Mover& m, i32 rows) const;
  void check_movers_hit();  // (after a tick: carves and blasts destroy the movers they hollowed)
  void drain_world_events();
  GridView grid_view(GridId id) const;
  ChunkMesh piece_mesh(const Body& b) const;
  ChunkMesh shape_mesh(const Body& b, size_t shape) const;
  void far_update();
  ChunkMesh far_mesh(i32 tx, i32 ty) const;

  World world_;
  Environment env_;
  GameParams par_;
  V3 spawn_pos_{0, 0, 0}, spawn_dir_{1, 0, 0};
  V3 viewer_{0, 0, 0};
  bool viewer_set_ = false;
  CommandLog* log_ = nullptr;
  MeshOptions mesh_base_;                    // (texture provider for piece meshes)
  std::vector<GameEvent> events_;
  std::unordered_set<u64> remesh_;           // chunks to mesh again (debug view changes, charring)
  std::unordered_set<u64> decor_only_;       // (of the last take_meshes: decoration changes only)
  std::unordered_set<u64> meshed_;           // (chunks meshed since their removal was queued)
  std::vector<i64> piece_remesh_;            // pieces whose charring or glow changed
  std::unordered_set<u64> charred_;          // chunks whose charring changed (meshed again every char_remesh_s)
  f64 char_clock_ = 0.0;
  std::unordered_set<u64> wet_dirty_;        // chunks whose water changed (meshed again every water_remesh_s)
  std::unordered_set<u64> wet_sent_;         // chunks with a water mesh at the front end
  std::vector<u64> wet_removed_;
  f64 wet_clock_ = 0.0;
  std::vector<u64> removed_chunks_;
  f64 mesh_ms_ = 0.0;
  // oriented grids: the chunks with a mesh at the front end, the ones to drop; the grids' places
  // the front end has, and the grids it is to drop
  std::set<std::pair<GridId, u64>> grid_meshed_;
  std::vector<GridChunk> removed_grid_chunks_;
  std::set<std::pair<GridId, u64>> grid_charred_, grid_remesh_;  // (the grids' charring and glow: as charred_, remesh_)
  std::unordered_map<GridId, GridView> grid_sent_;
  std::vector<GridId> removed_grids_;

  struct View {                              // a piece's mesh frame: sent at (x0, q0)
    V3 x0;
    Quat q0;
  };
  std::unordered_map<i64, View> views_;
  struct Fading {
    i64 id;
    f64 t;
    V3 pos;
    Quat rot;
  };
  std::vector<Fading> fading_;

  struct Machine {
    KinematicId body = 0;
    MachineDrive drive;
    Pose rest;
  };
  std::vector<Machine> machines_;
  std::vector<Drop> drops_;
  std::vector<Mover> movers_;
  std::unordered_map<u64, std::vector<i32>> mover_cols_;
  std::vector<std::pair<V3, f64>> shots_;  // carves and blasts of this tick (centre, reach)

  // streamed levels: far tier
  std::shared_ptr<const GameSource> source_;
  StreamConfig stream_{};
  FarConfig far_{};
  std::unordered_set<u64> far_sent_;
  std::vector<ChunkMesh> far_out_;
  std::vector<std::array<i32, 2>> far_removed_;
};

}  // namespace svx
