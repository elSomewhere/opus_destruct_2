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
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "svx/env/env.hpp"
#include "svx/game/movers.hpp"
#include "svx/game/source.hpp"
#include "svx/game/vehicles.hpp"
#include "svx/mesh/mesher.hpp"
#include "svx/world/world.hpp"

namespace svx {

class CommandLog;
class Pedestrians;
namespace anim {
class CharacterSystem;
class ModelMesher;
}  // namespace anim

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
  i32 material = -1;  // Dust: its material (-1: unknown) - a car's glass shattering, a wall's brick dust
  ChunkMesh mesh;  // Detached: the piece's mesh in world coordinates at pos (drawn at its PiecePose)
  // Detached, Remesh: the piece's voxels at pos, for a front end's collision (a player stands on
  // a lift's car, rides a turntable, climbs rubble): piece_occupancy's layout.
  std::vector<u8> occupancy;
};

// A piece's voxels for a front end's collision, at its pose now (little-endian): u32 its shapes,
// then per shape its lattice in the world - origin xyz, rotation xyzw, voxel size (7 + 1 f64:
// lattice point h p is at origin + rot (h p)) - its voxel box - lo xyz, dims xyz (6 i32) - and a
// bit per cell of the box (cell ((x - lo) dy + (y - lo)) dz + (z - lo): bit i & 7 of byte i >> 3).
std::vector<u8> piece_occupancy(const Body& b);

// A piece's pose for the front end: its mesh (sent in world coordinates) is drawn at
// pos + rot (p - the centre it was sent at); it moves at vel (its centre) and turns at ang.
struct PiecePose {
  i64 id = 0;
  V3 pos;
  Quat rot;
  f64 opacity = 1.0;
  V3 vel, ang;
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

// An oriented grid's place for the front end (docs/GRIDS.md): its frame in the world and voxel
// size. Its chunk meshes and occupancy are in its lattice.
struct GridView {
  GridId id = 0;
  V3 origin;
  Quat rot;
  f64 voxel_size = 0.0;
};

// Something a level drops into its world when play starts (after the bake and a saved session's
// changes): a free object of this session, a piece from the first tick (crates on a turntable).
struct Drop {
  GridDesc desc;
  VoxelGrid voxels;
};

// A vehicle for the front end (docs/VEHICLES.md): its chassis (a piece, drawn from its poses),
// what drives it, and where its driver sits.
struct VehicleView {
  u32 id = 0;
  i64 chassis = 0;              // its piece (0: not a piece yet)
  VehicleKind kind = VehicleKind::Sedan;
  Paint paint = Paint::None;
  V3 pos, vel;                  // its chassis' centre of mass
  Quat rot;                     // its frame (x forward, y left, z up) in the world
  f64 speed = 0.0;              // m/s along its forward
  f64 rpm = 0.0;
  int gear = 0;                 // -1 reverse, 0 neutral, 1..
  VehicleInput input;           // what drives it now
  u8 flags = 0;                 // kPlayer, kNpc, kParked, kWreck
  V3 seat;                      // the driver's seat (world)
  V3 origin;                    // its frame's origin (world): on the ground under the middle between its axles
  V3 half_extent;               // m: its box about its frame's origin (x, y), from the ground (z)
  i32 wheels = 0;               // wheels still on
  i32 parts = 0, parts0 = 0;    // its parts still on (doors, bonnet, bumpers, ...), of those it was built with
  f64 damage = 0.0;             // 0 .. 1: how much of it is crumpled or gone
  f64 redline = 0.0;            // its engine's (rpm)
  static constexpr u8 kPlayer = 1, kNpc = 2, kParked = 4, kWreck = 8;
};

// A wheel for the renderer: drawn at its centre, turned (x the way it rolls, y its axle) and spun.
struct WheelView {
  u32 vehicle = 0;
  WheelId id = 0;
  V3 centre;
  Quat rot;
  f64 radius = 0.0, width = 0.0;
  bool contact = false;
  f64 slip = 0.0;               // m/s: the tyre sliding (skids, smoke)
  int material = -1;            // what it stands on
  f64 compression = 0.0;
};

// Traffic (a streamed city with roads, GameSource::roads): cars driving its lanes around the
// viewer, parked cars at its kerbs. Cars out of range that nobody touched go (and come again);
// wrecks stay (the world keeps them in its archive).
struct TrafficConfig {
  bool enabled = true;
  i32 cars = 14;                // driving within range of the viewer
  i32 parked = 18;              // parked within range
  f64 near_radius = 45.0;       // m: not spawned nearer (out of sight)
  f64 radius = 110.0;           // m: spawned within, removed beyond (untouched)
  f64 speed_scale = 1.0;        // x the roads' limits
};

// Pedestrians (a streamed city with walkways, RoadNetwork::walks_in; docs/ANIM.md): people on its
// sidewalks around the viewer, svx_anim characters in the world - near the viewer and near moving
// pieces their bodies are articulations of it (a car that hits one hits a body). The living out
// of range go and others come; the dead stay while in range.
struct PedestrianConfig {
  bool enabled = true;
  i32 count = 24;            // about the viewer
  f64 near_radius = 30.0;    // m: not spawned nearer (out of sight)
  f64 radius = 70.0;         // m: spawned within, the living removed beyond
  i32 bodies = 2;            // 0: deep (every physical body an articulation of the world), 1: shallow (their own), 2: hybrid
  i32 max_deep = 24;         // (hybrid) the most deep bodies: the nearest
};

// A character for the front end (docs/ANIM.md): its mesh (take_character_meshes) drawn with its
// palette (take_character_palettes) and skin matrices - rigid skinning: a vertex at
// skin[bone] x its rest position - and its held prop's mesh with the prop's matrix. A gib (a piece
// of one - a limb shot off, what a blast tore apart: kGib) is drawn the same way with one matrix.
constexpr i32 kCharacterBones = 23;
struct CharacterView {
  u32 id = 0;
  u32 mesh = 0, prop_mesh = 0;  // (0: none)
  u32 palette = 0;
  u8 flags = 0;
  V3 centre;                    // its bounding sphere (world)
  f64 radius = 1.0;
  f64 flash = 0.0;              // 0..1: a hit's flash (a tint)
  f64 health = 1.0;             // 0..1 of its full health
  const f32* skin = nullptr;    // `bones` x 16 floats (column-major 4x4), until the next tick
  i32 bones = kCharacterBones;  // (a gib: 1)
  std::array<f32, 16> prop{};   // the prop's matrix
  static constexpr u8 kAlive = 1, kDeep = 2, kPhysical = 4, kAsleep = 8, kDown = 16, kGib = 32;
};

// A character mesh (the svx_anim character vertex format: 20 bytes a vertex, svx/anim/voxel/mesh.hpp).
struct CharacterMeshData {
  u32 id = 0;
  std::vector<u8> vertices;
  std::vector<u32> indices;
  i32 vertex_count = 0;
};

// A character palette: its 16 slots' colours (linear rgb).
struct CharacterPalette {
  u32 id = 0;
  std::array<f32, 48> rgb{};
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
  bool load_delta(const std::vector<u8>& bytes);  // (movers keep their state; a played session's drops are not dropped again)
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

  // Drops: objects dropped in when play starts (the first tick). (A level's machines are pieces
  // on driven joints: World::add_joint.)
  void add_drop(Drop&& d) { drops_.push_back(std::move(d)); }

  // Vehicles (game/src/vehicles.cpp; docs/VEHICLES.md). A vehicle's id is its wheels' group;
  // its model and flags (WheelTag: npc, parked) ride on its wheels' tags. Spawning, removing,
  // getting in and out, the player's controls and shots are commands (logged).
  u32 spawn_vehicle(const VehicleSpec& spec, const V3& pos, f64 yaw, u8 flags = 0);  // its id (0: refused)
  bool remove_vehicle(u32 id);
  bool enter_vehicle(u32 id);  // the player drives it
  void exit_vehicle();
  u32 player_vehicle() const { return player_vehicle_; }
  void drive(const VehicleInput& in);  // the player's vehicle's controls (until changed)
  u32 vehicle_near(const V3& pos, f64 reach) const;  // (0: none)
  std::vector<VehicleView> vehicles() const;  // id order
  std::vector<WheelView> wheel_views() const;
  bool vehicle(u32 id, VehicleView* out) const;
  // A bullet: an impact of this energy (J) removes what its energy density penetrates.
  void shoot(const V3& pos, f64 radius, f64 energy);
  void set_traffic(const TrafficConfig& c);  // (logged)
  const TrafficConfig& traffic() const { return traffic_; }
  int paint_layer() const { return paint_layer_; }

  // Pedestrians (game/src/pedestrians.cpp). A shot or a blast frightens them; a car coming at
  // them makes them jump aside; one that hits them hurts them.
  void set_pedestrians(const PedestrianConfig& c);  // (logged)
  const PedestrianConfig& pedestrians() const { return peds_; }
  // The characters (null until people come: a world with walkways, pedestrians enabled).
  anim::CharacterSystem* characters() { return chars_.get(); }
  const anim::CharacterSystem* characters() const { return chars_.get(); }
  std::vector<CharacterView> character_views() const;
  // A shot's line against the world and the characters: the nearest hit (character: whose body,
  // and the bone; 0: the world's voxels or a piece).
  struct ShotHit {
    bool hit = false;
    V3 pos, normal;
    f64 distance = 0.0;
    i32 material = -1;
    u32 character = 0;
    i32 bone = -1;
    u8 slot = 0;  // (a character) the palette slot of the voxel hit
  };
  ShotHit raycast_shot(const V3& origin, const V3& dir, f64 max_dist) const;
  // A round into a character (logged): where raycast_shot found its body, fired from the viewer
  // (a host's round comes a tick or more after its ray: a body that moved since is found along
  // the same line at the bone nearest that point); `energy` (J) and `radius` as shoot's (the
  // wound's hole some 0.3 of it). False: no such character, or no body there now.
  bool wound_character(u32 id, const V3& pos, f64 radius, f64 energy);
  // Character meshes and palettes the front end has not had, and meshes no character draws any more.
  std::vector<CharacterMeshData> take_character_meshes();
  std::vector<u32> take_removed_character_meshes();
  std::vector<CharacterPalette> take_character_palettes();
  // Blood now: drops (x, y, z, radius, r, g, b each) and stains on the surfaces (x, y, z, normal
  // xyz, radius, age each), floats.
  void blood(std::vector<f32>* drops, std::vector<f32>* stains) const;

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
  // The grids whose place changed since the last call (they came, or were moved), and the grids
  // gone.
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
  void set_mover_rows(Mover& m, i32 rows);
  bool blocks_player(const Mover& m, i32 rows) const;
  void check_movers_hit();  // (after a tick: carves and blasts destroy the movers they hollowed)
  void drain_world_events();
  GridView grid_view(GridId id) const;
  // (fresh: a new piece - its shapes' meshes may be ones made before for the same voxels: a
  // vehicle of a kind dropped in again; body: its body's paint, the same mesh re-tinted)
  ChunkMesh piece_mesh(const Body& b, bool fresh = false, Paint body = Paint::None) const;
  const Body* player_car() const;  // (the chassis of the player's vehicle, if they drive one)
  ChunkMesh shape_mesh(const Body& b, size_t shape, bool fresh = false, Paint body = Paint::None) const;
  struct ShapeMeshMemo {
    IVec3 lo{0, 0, 0}, dim{0, 0, 0};
    f64 h = 0.0;
    std::vector<Vox> vox;
    std::vector<u8> paint;
    ChunkMesh mesh;  // (in its lattice)
  };
  mutable std::unique_ptr<std::mutex> shape_memo_mu_ = std::make_unique<std::mutex>();
  mutable std::unordered_map<u64, ShapeMeshMemo> shape_memo_;
  void far_update();
  ChunkMesh far_mesh(i32 tx, i32 ty) const;

  // (the game's materials are in the process's table before its world is made from it)
  struct MaterialsFirst {
    MaterialsFirst();
  } materials_first_;
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

  // vehicles (game/src/vehicles.cpp, traffic.cpp)
  struct Vehicle {
    u32 id = 0;
    VehicleSpec spec;
    u8 flags = 0;                   // WheelTag flags
    std::vector<WheelId> wheels;    // by slot (0: come off)
    i64 chassis = 0;
    i32 voxels0 = 0;                // its voxels when it was whole
    VehicleInput input;
    int gear = 1;
    f64 rpm = 0.0, shift = 0.0, steer = 0.0;
    bool wreck = false;
    // a driver's (traffic): the lane it drives and the one it takes on
    u64 lane = 0, next = 0;
    f64 stuck = 0.0;                // s it has not moved while it meant to
    f64 hit_speed = 0.0;            // (its speed last tick: a sudden change is a collision)
    bool touched = false;           // (a parked car or a driver's that something moved or hit)
    V3 home;                        // (a parked car's place)
    i32 reshapes = 0;               // (its chassis crumpled: PieceReshaped)
    u64 spot = 0;                   // (a parked car's kerbside place)
    // (its damage, measured when its chassis last changed: the piece, its voxels, its folds)
    mutable f64 damage = 0.0;
    mutable i64 damage_chassis = 0;
    mutable i32 damage_count = -1, damage_reshapes = -1, damage_parts = -1;
  };
  f64 body_damage(const Vehicle& v, const Body& b, i32 part_voxels) const;
  std::vector<i64> vehicle_parts(const Vehicle& v) const;  // (its parts' pieces still on it)
  void remove_vehicle_bodies(const Vehicle& v);           // (its wheels, parts and chassis)
  u32 spawn_vehicle_internal(const VehicleSpec& spec, const V3& pos, f64 yaw, u8 flags);  // (not logged: traffic)
  void vehicles_before_tick();      // controls -> wheel inputs, drag, anti-roll
  void vehicles_after_tick();       // the registry from the wheels, drivetrains, wrecks, traffic
  void sync_vehicles();             // (records for wheels' groups: loads, the archive)
  void drive_vehicle(Vehicle& v, const Body& b, f64 dt);
  void step_traffic();
  void steer_driver(Vehicle& v, const Body& b);
  std::map<u32, Vehicle> vehicles_;
  u32 next_vehicle_ = 1;
  u32 player_vehicle_ = 0;
  VehicleInput player_input_;
  int paint_layer_ = -1;
  TrafficConfig traffic_;
  f64 traffic_clock_ = 0.0;
  std::set<u64> parked_spots_;      // (kerbside places with a parked car now, or one that is out of range)

  // pedestrians (game/src/pedestrians.cpp)
  friend class Pedestrians;
  PedestrianConfig peds_;
  std::shared_ptr<anim::CharacterSystem> chars_;
  std::unique_ptr<Pedestrians> people_;
  Pedestrians* people() const;
  void pedestrians_before_tick();
  void pedestrians_after_tick();
  void noise(const V3& pos, f64 radius, int kind);  // (what people hear: a shot, a blast, a crash)

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
