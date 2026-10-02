// svx_anim — characters in a core World: a WorldSystem that owns them and steps them with the
// world's ticks (docs/ANIM.md).
//
// Every tick, a frame of every character in two halves about the mechanics: before them (pre_step)
// the level of detail, the plans and the drives (the deep bodies' - and whatever the host did to
// them since - to their articulations; the shallow bodies step on their own there); after them
// (step) what the tick made of the deep bodies. The host sets the characters' roots and inputs
// between ticks.
//
// Where each body is simulated is the system's level of detail, by the distance to the focus
// points (the player, the camera) and what is near it:
//  - deep: an articulation of the world (every contact with the world real and two-way),
//  - shallow: its own XPBD body, meeting the world through its collision and the other bodies
//    through obstacles,
//  - plan only: no physics at all (far, calm).
// A character near an awake piece (a car coming, debris flying) goes deep, whatever its distance:
// what can touch the world's bodies is one of them. Bodies that need physics (hit, falling, dying)
// always get it.
//
// Streaming: a character whose ground goes out of range goes with it (the host's population makes
// the living again as the player comes back). The dead are kept by the world: a dead body is one
// of its articulations (asleep once at rest, it costs nothing), archived with its region and given
// back with it - the system then makes the character again (the host's `restore`: who it was, from
// what it recorded in the articulation) and adopts the body where it lies.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "svx/anim/character.hpp"
#include "svx/anim/physics/world_collision.hpp"
#include "svx/world/world.hpp"

namespace svx::anim {

using CharacterId = u32;

enum class BodyPolicy : u8 {
  Deep,     // every physical body an articulation of the world
  Shallow,  // every physical body its own (XPBD)
  Hybrid,   // deep near the focus points and near awake pieces, shallow further, plan only far
};

struct CharacterSystemConfig {
  BodyPolicy policy = BodyPolicy::Hybrid;
  f64 deep_radius = 14.0;     // (hybrid) within this of a focus point: deep
  f64 physics_radius = 45.0;  // within this: physical even while calm; beyond: on the plan alone (calm)
  i32 max_deep = 24;          // (hybrid) the most deep bodies (the nearest)
  f64 piece_radius = 3.5;     // a piece awake within this of a body: deep (0: never for that)
  f64 hysteresis = 2.0;       // (m) a body changes path only this far past a radius
  f64 despawn_margin = 8.0;   // (m) a character whose ground is gone from within this goes (streaming)
  bool parallel = true;       // update the characters on the world's threads
};

struct CharacterDesc {
  ModelPtr model;
  Palette palette{};
  PropPtr weapon;
  std::vector<LoadoutEntry> loadout;
  WieldProfile wield;
  f64 health = 100.0;
  f64 seed = 1.0;
  f64 mass = 0.0;
  V3 pos;
  f64 yaw = 0.0;
  u32 kind = 0;  // (the host's: a pedestrian, a soldier)
  std::vector<u8> data;  // (the host's: who the character is, for `restore` - its look)
};

struct CharacterStats {
  i32 characters = 0, deep = 0, shallow = 0, plan_only = 0, asleep = 0;
  f64 pre_ms = 0.0, step_ms = 0.0;  // (last tick)
};

class CharacterSystem final : public WorldSystem {
 public:
  explicit CharacterSystem(const CharacterSystemConfig& cfg = {});
  ~CharacterSystem() override;

  const char* name() const override { return "characters"; }
  void attach(World& w) override;
  void on_load(World& w) override;
  void on_evicted(World& w, const std::vector<u64>& chunks) override;
  void pre_step(World& w, f64 dt) override;
  void step(World& w, f64 dt) override;
  i64 memory_bytes() const override;
  u64 state_hash() const override;

  std::shared_ptr<PropRegistry> props = std::make_shared<PropRegistry>();
  CharacterSystemConfig config;
  // The focus points of the level of detail (the player, the camera), set by the host each frame.
  std::vector<V3> focus;
  // A body of one of the system's characters the world gave back (its region came back into
  // range, a session loaded): the character it was - the host fills in its model, palette, weapon
  // from its kind and data (false: none; the body stays in the world, no one's).
  std::function<bool(u32 kind, const std::vector<u8>& data, CharacterDesc* out)> restore;

  // The world it is in moved (its host's): the characters follow it. (Its hooks do this
  // themselves; a host spawning between a move and the next tick calls it.)
  void rebind(World& w);
  CharacterId spawn(const CharacterDesc& d);  // 0: refused
  bool despawn(CharacterId id);
  Character* get(CharacterId id);
  const Character* get(CharacterId id) const;
  u32 kind_of(CharacterId id) const;
  std::vector<CharacterId> ids() const;  // ascending
  CharacterStats stats() const { return stats_; }
  const CollisionWorld* collision() const { return collision_.get(); }

  // The nearest character voxel a ray hits (a bullet), within max_dist.
  struct Hit {
    CharacterId id = 0;
    CharacterHit hit;
  };
  std::optional<Hit> raycast(const V3& origin, const V3& dir, f64 max_dist) const;

 private:
  struct Entry {
    CharacterId id = 0;
    u32 kind = 0;
    std::vector<u8> data;
    std::unique_ptr<Character> c;
    ArticulationId recorded = 0;  // (the articulation its record was written to, and ...)
    bool recorded_alive = true;   // (... whether it was alive then, ...)
    u32 recorded_geometry = 0;    // (... and its model's version)
  };
  std::vector<ArticulationId> strangers_;  // (the world's articulations that are not the system's: ascending)
  i64 scan_tick_ = -1;
  World* world_ = nullptr;
  std::unique_ptr<WorldCollision> collision_;
  std::vector<Entry> chars_;  // ascending ids
  CharacterId next_ = 1;
  CharacterStats stats_;
  Entry* entry(CharacterId id);
  const Entry* entry(CharacterId id) const;
  void level_of_detail(World& w);
  void record(World& w);      // (what each bound body is, in its articulation's host data)
  void take_back(World& w);   // (bodies the world gave back: characters again)
};

}  // namespace svx::anim
