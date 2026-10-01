// structvox game — pedestrians: what is behind Game's people (game.hpp: PedestrianConfig).
//
// A walker is a character's host: it moves the character's root the way the original's actor
// world did (steering with weight, keeping apart from the others, a box swept against the world
// with a step up kerbs, the body's own moves while it leads) and minds it the way the original's
// civilians were minded, as much of it as a city street asks: walking from street corner to
// street corner, waiting at the kerb for the lights, crossing on the zebra, stopping for a
// while, jogging; frightened by shots, blasts, crashes and screams, jumping out of the way of a
// car coming at it, running, cowering; hurt by what hits its body (a car), knocked down, up and
// away - or dead.
#pragma once

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "svx/anim/characters/humans.hpp"
#include "svx/anim/physics/debris.hpp"
#include "svx/anim/system.hpp"
#include "svx/anim/voxel/mesh.hpp"
#include "svx/game/game.hpp"

namespace svx {

class Pedestrians {
 public:
  enum class Mind : u8 { Walk, Wait, Cross, Flee, Cower, Dodge, Talk };
  // (what people hear)
  enum NoiseKind : int { kShot = 0, kImpact = 1, kExplosion = 2, kDeath = 3, kScream = 4, kCrash = 5 };

  struct Walker {
    u32 id = 0;          // its character
    u64 rng = 0;         // (its own draws)
    V3 pos, vel;         // the root: its feet (world), their velocity
    f64 yaw = 0.0, yaw_rate = 0.0;
    bool on_ground = true;
    Mind mind = Mind::Walk;
    f64 timer = 0.0, think = 0.0, stuck = 0.0;
    f64 fear = 0.0, bravery = 1.0, pace = 1.3;
    bool jogger = false;
    f64 speed = 0.0;     // the pace it wants now (m/s)
    V3 threat;           // where the last fright came from
    u64 walk = 0;        // the walkway it is on ...
    int toward = 1;      // ... and the end it heads for (0: a, 1: b)
    std::vector<V3> path;  // the way left along it
    V3 dodge;            // (Dodge) the way it jumps
    u32 partner = 0;     // (Talk) who with; the one who stopped to talk walks up to the other
    bool lead = false, speaking = false;
    f64 turn = 0.0;      // (Talk) s until the other speaks
    f64 screamed = -99.0;
    bool touched = false;  // (hurt, knocked about: it stays while in range)
    bool alive = true;
    f64 dead_for = 0.0;
  };
  struct Noise {
    V3 pos;
    f64 radius = 0.0;
    int kind = kShot;
    u32 source = 0;  // (the walker who made it: it does not frighten itself)
  };

  explicit Pedestrians(Game& g);
  ~Pedestrians();
  void rebind(Game& g) { g_ = &g; }  // (the game moved)

  void before_tick();  // minds and moves (the roots and inputs for the characters' frames)
  void after_tick();   // blows, deaths, the population, the front end's meshes
  void noise(const V3& pos, f64 radius, int kind, u32 source = 0) { noises_.push_back(Noise{pos, radius, kind, source}); }
  void blast(const V3& pos, f64 radius, f64 energy);  // (Game::blast: before the world's)
  bool wound(u32 id, const V3& from, const V3& pos, f64 radius, f64 energy);  // (Game::wound_character)
  void clear();        // (a new level)

  std::vector<CharacterView> views() const;
  std::vector<CharacterMeshData> take_meshes();
  std::vector<u32> take_removed_meshes();
  std::vector<CharacterPalette> take_palettes();
  void blood(std::vector<f32>* drops, std::vector<f32>* stains) const;
  i64 memory_bytes() const;
  i32 walkers() const { return static_cast<i32>(walkers_.size()); }

 private:
  Game* g_;
  std::map<u32, Walker> walkers_;  // by character
  std::vector<Noise> noises_, heard_;
  f64 clock_ = 0.0;        // (the population: every half second)
  f64 time_ = 0.0;
  // the looks people are made from (a few models shared by many, until one is wounded)
  std::vector<anim::HumanVariant> looks_;
  // the front end's meshes (by model and its geometry version) and palettes
  struct MeshEntry {
    anim::ModelPtr model;  // (kept while its mesh is drawn)
    u32 id = 0;
    f64 unused = 0.0;      // s no character has drawn it
    bool sent = false;     // (meshed for the front end: made when it asks)
  };
  std::map<std::pair<const anim::VoxelModel*, u32>, MeshEntry> meshes_;
  std::map<u32, u32> mesh_of_;     // character -> its mesh
  std::map<u64, u32> palettes_;    // (palette digest) -> id
  std::map<u32, u32> palette_of_;  // character -> its palette
  u32 next_mesh_ = 1, next_palette_ = 1;
  std::vector<u32> removed_out_;
  std::vector<CharacterPalette> palettes_out_;
  std::unique_ptr<anim::ModelMesher> mesher_;
  // gibs and blood (over the characters' collision), and the gibs' meshes and matrices
  std::unique_ptr<anim::GibSystem> gibs_;
  struct GibEntry {
    u32 mesh = 0;
    bool sent = false;
    std::array<f32, 16> skin{};
  };
  std::map<u32, GibEntry> gib_meshes_;  // gib id -> its mesh
  anim::GibSystem* gibs();
  u64 palette_id(const anim::Palette& p);

  anim::CharacterSystem& chars();
  const RoadNetwork* roads() const;
  V3 focus() const;  // (the player's car, or the viewer)
  f64 rnd(Walker& w, f64 a, f64 b);
  bool chance(Walker& w, f64 p) { return rnd(w, 0.0, 1.0) < p; }

  // the mind
  void think(Walker& w, anim::Character& c, f64 dt);
  void hear(Walker& w, const Noise& n);
  void watch_traffic(Walker& w, anim::Character& c);
  void choose(Walker& w);
  void flee(Walker& w);
  void meet(Walker& w);                 // (a calm walker passing another: now and then they stop and talk)
  void part(Walker& w, bool afraid);    // (a talk over, for both)
  void on_walk(Walker& w, u64 walk, int toward, bool from_start);
  void at_corner(Walker& w, bool fleeing);
  // the body
  void move(Walker& w, anim::Character& c, f64 dt);
  // the population
  void make_looks();
  void populate();
  void blows(Walker& w, anim::Character& c);
  void output();
};

}  // namespace svx
