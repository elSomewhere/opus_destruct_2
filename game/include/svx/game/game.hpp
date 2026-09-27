// structvox game — the prototype game harness around the physics core (docs/CORE.md: layers).
//
// A Game owns a World (svx/world/world.hpp) and adds what the prototype game needs on top of it:
//   - a viewer (the player's eye): the world's streaming focus, walk-over triggers, movers that
//     wait for the player;
//   - movers (doors, lifts, ...) and the level's triggers (use, walk-over and shot resolvers);
//   - output for the renderer: chunk meshes (with debug views), piece meshes (sent once, in world
//     coordinates) and poses, fading of culled pieces, the far render tier of streamed levels;
//   - a command log (record / replay / lockstep).
// The core never sees any of it: a different game (or tool) builds its own harness on World.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
  enum class Kind : u8 { Detached, Crack, Impact, Dust };
  Kind kind = Kind::Crack;
  i64 id = 0;
  V3 pos, vel, ang, normal{0, 0, 1};
  f64 radius = 0.0, strength = 0.0;  // Crack: utilization; Impact: energy (J); Dust: 1 crushed, 0 a shard
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
  void tick();
  i64 ticks() const { return world_.ticks(); }
  void record_to(CommandLog* log) { log_ = log; }

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
  std::vector<ChunkMesh> take_meshes(const MeshOptions& base);
  std::vector<u64> take_removed_chunks();
  std::vector<ChunkMesh> take_far_meshes();
  std::vector<std::array<i32, 2>> take_far_removed();
  std::vector<GameEvent> take_events();
  std::vector<PiecePose> pieces() const;
  const FarConfig& far_config() const { return far_; }

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
  void set_mover_rows(Mover& m, i32 rows);
  bool blocks_player(const Mover& m, i32 rows) const;
  void check_movers_hit();  // (after a tick: carves and blasts destroy the movers they hollowed)
  void drain_world_events();
  ChunkMesh piece_mesh(const Body& b) const;
  void far_update();
  ChunkMesh far_mesh(i32 tx, i32 ty) const;

  World world_;
  GameParams par_;
  V3 spawn_pos_{0, 0, 0}, spawn_dir_{1, 0, 0};
  V3 viewer_{0, 0, 0};
  bool viewer_set_ = false;
  CommandLog* log_ = nullptr;
  MeshOptions mesh_base_;                    // (texture provider for piece meshes)
  std::vector<GameEvent> events_;
  std::vector<u64> remesh_;                  // chunks to mesh again (debug view changes)
  std::vector<u64> removed_chunks_;
  f64 mesh_ms_ = 0.0;

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
