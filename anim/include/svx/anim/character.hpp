// svx_anim — a voxel character: the high-level object a host drives. It ties together
//  - the voxel model (shared between characters until the first wound, then copied: damage is per
//    character; `geometry_version` tells the host to re-mesh),
//  - the motion plan (motion/plan.hpp): what the character means to do, from the host's inputs,
//  - the physical body (body/humanoid.hpp): rigid bodies and muscles that carry the plan out,
//  - the behaviours (behaviour/controller.hpp): the motor intelligence between them (balance,
//    stagger, bracing, flinching, holding wounds, falling, getting up, dying),
//  - the held prop (in the physical hand; dropped on death),
//  - health, hit zones, wounds (voxels carved out, flesh and bone inside), severed limbs and
//    heads, and gibbing by blasts (the pieces come back as gib specs for a GibSystem).
//
// The host moves it (set_root, with its own collision) while the body follows its plan; when the
// body leads (knocked off its feet, staggering, down, getting up) the host follows it instead
// (`controlled`, `take_root_motion`). It sets `motion.input`, calls update, and draws `skin`.
//
// Where the body is simulated:
//  - Shallow: on its own (the body's XPBD system, stepped in update), meeting the world through
//    the CollisionWorld and other bodies through obstacles (set_obstacles, gather_obstacles).
//  - Deep: as an articulation of a core World, stepped with everything else in the world's tick
//    (standing on its structures and loading them, hit by what hits it, pushing what it meets).
//    Its frame comes in two halves about the world's tick (a CharacterSystem calls them): begin
//    before it (the plan, the drives, pushed to the core), end after it (what the tick made of the
//    body) - the order the shallow path keeps within update.
// The physics costs time, so a calm character may run on its plan alone (`physics = false`, the
// host's level of detail); anything that needs the body (a hit, a push, a fall, death) wakes it.
// A body can move between the two paths at any frame (set_backend): its state goes with it.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "svx/anim/behaviour/controller.hpp"
#include "svx/anim/characters/props.hpp"
#include "svx/anim/physics/core_binding.hpp"
#include "svx/anim/voxel/damage.hpp"

namespace svx::anim {

enum class BodyBackend : u8 { Shallow, Deep };

// A piece that came off a character: hand it to a GibSystem.
struct GibSpec {
  VoxelPart part;
  f64 voxel_size = 0.0;
  // its bone's world transform at the moment (x -> bone_rot (x - bone_rest_head) + bone_pos)
  V3 bone_pos;
  Quat bone_rot;
  V3 bone_rest_head;
  V3 vel, ang;
  bool prop = false;  // the piece is the held prop (drawn with the prop's palette slots)
};

struct WoundResult {
  bool killed = false;  // the shot killed the character now
  bool headshot = false;
  f64 damage = 0.0;                   // dealt (after hit-zone multipliers)
  std::vector<RemovedVoxel> removed;  // voxels carved out (blood and flesh bits for the host's effects)
  std::vector<GibSpec> gibs;          // pieces that came off (severed limbs, a head)
  Zone zone = Zone::Chest;            // (melee)
};

struct BlastResult {
  f64 damage = 0.0;
  bool killed = false, gibbed = false;
  std::vector<GibSpec> gibs;
};

struct CharacterOptions {
  ModelPtr model;
  Palette palette{};
  const CollisionWorld* collision = nullptr;
  PropPtr weapon;
  f64 health = 100.0;
  f64 seed = 1.0;
  f64 mass = 0.0;  // total mass (kg); 0: by the rig's height
  BodyBackend backend = BodyBackend::Shallow;
  World* world = nullptr;  // (deep: the world its body is an articulation of)
  // (deep: the articulation's host data - what the character is to its host; see CoreBinding)
  u32 group = 0, tag = 0;
};

class Character {
 public:
  explicit Character(const CharacterOptions& o);
  Character(const Character&) = delete;
  Character& operator=(const Character&) = delete;
  ~Character();

  ModelPtr model;  // the model drawn: shared until the first wound, then this character's own copy
  Palette palette;
  MotionPlan motion;       // what the character means to do (the host's inputs, actions, stances)
  HumanoidBody body;       // what it is made of
  Behaviours behaviours;   // how it carries itself (balance, reflexes, injuries, falls, death)
  WorldPose pose;          // the pose shown (the body's, or the plan's while the physics rests) ...
  WorldPose prev_pose;     // ... and the frame before
  PropPtr weapon;
  f64 health = 100.0;
  f64 max_health = 100.0;
  u32 geometry_version = 0;  // bumped when `model` changes (a copy was made or voxels were carved): re-mesh
  bool owns_model = false;   // true once `model` is this character's own copy
  std::vector<f32> skin;     // skin matrices of the current pose (16 floats per bone)
  f64 flash = 0.0;           // hit flash 0..1 (the host tints the character)
  bool knocked_out = false;  // beaten unconscious (a melee knockout): down until it comes to
  f64 dead_time = 0.0;       // seconds since death
  bool physics = true;       // physics even while calm (the host's level of detail; bodies that need it always get it)
  V3 weapon_pos;             // the held prop's transform (world)
  Quat weapon_rot;

  bool alive() const { return behaviours.alive; }
  bool controlled() const { return behaviours.leading(); }  // the body leads (staggering, falling, down, getting up, dead): the host follows its root
  bool gun_hand_lost() const { return behaviours.lost[weapon && weapon->kind == PropKind::Knife && motion.weapon_hand == H::handL ? B::handL : B::handR]; }
  bool writhing() const { return behaviours.writhing; }
  bool down() const;    // down on the ground (knocked down or out), or getting up
  bool asleep() const;  // the body rests (a corpse that stopped moving)

  // ---- where the body is simulated
  BodyBackend backend() const { return backend_; }
  bool bound() const { return binding_.bound(); }  // (deep: its articulation is in the world)
  ArticulationId articulation() const { return binding_.id(); }
  // Moves the body to the other path (its state goes with it). Deep needs a world.
  bool set_backend(BodyBackend b, World* world = nullptr);
  // (deep) Its world moved (the host's): the same world at a new address.
  void rebind_world(World* w) { world_ = w; }
  // (deep) The articulation came back (a session loaded, the streaming archive): adopt it.
  bool adopt(ArticulationId id);
  // (deep) Its articulation is gone from the world (removed, archived, out of the world): the body
  // is its own again, where it last was.
  void unbound();

  // ---- the host's side
  void place(const V3& pos, f64 yaw);           // feet on the ground at pos, facing yaw
  void set_root(const V3& pos, f64 yaw);        // the host's root this frame (ignored while the body leads: see take_root_motion)
  std::vector<AnimEvent> take_events();         // the motion's events since the last call, the limb positions from the body
  V3 take_root_motion();                        // how far the body moved the root since the last call
  void fire();                                  // a shot fired (recoil)
  // The frame: the plan, the body carrying it out, the pose. A body bound to a world moves with its
  // ticks: begin(dt) before one (the plan and the drives, to the core), end() after it (what the
  // tick made of the body); update(dt) is both - a frame the world stood still for. Shallow or
  // plan-only, begin does it all (end: nothing).
  void update(f64 dt) {
    begin(dt);
    end();
  }
  void begin(f64 dt) {
    if (!begin_start(dt)) return;
    begin_body();
    begin_push();
  }
  void end();
  // begin in three: its start (the timers, the physics on or off - which may add or remove its
  // articulation: characters one at a time; false: nothing more this frame), its body (the plan,
  // the body's own step or the drives: characters side by side, reading the world only), the
  // push (the drives to the world: one at a time).
  bool begin_start(f64 dt);
  void begin_body();
  void begin_push();

  // ---- senses and blows
  void perceive(const Perception& p);
  // A blow on the body (world): `point` where it lands, `dir` the direction it travels, `force` (1 ~
  // a rifle round or a punch), its kind and, if known, the bone. Returns the zone.
  Zone hit_at(const HitInfo& info);
  void push(const V3& dir, f64 strength);  // thrown off balance: strength ~0.5 (a jolt) .. 3 (thrown)
  void trip();
  void knock_out(f64 seconds);
  void add_injury(i32 bone, f64 severity);  // an old wound (no blow now)
  // The body's collision spheres (world), appended: what other bodies bump into and trip over.
  void collision_spheres(std::vector<Obstacle>& out, i32 owner) const;
  // What is about this frame (the bodies of others near by, the dead, debris): the body collides
  // with them (shallow) and its steps clear what they see of them.
  void set_obstacles(const std::vector<Obstacle>& list);
  // Another body's push on one of this body's parts (an impulse, N s, at a world point).
  void pushed_at(i32 part, const V3& j, const V3& at);
  void impulse(const V3& point, const V3& dv, f64 carry = 0.0);  // a push at `point` (velocity change dv), alive or dead
  // A radial push from `center`: the body gains up to `speed` away from it.
  void blast_push(const V3& center, f64 radius, f64 speed);

  // ---- damage
  V3 bounds_center() const;
  f64 bounds_radius() const;
  std::optional<CharacterHit> raycast(const V3& origin, const V3& dir, f64 max_dist) const;
  // A bullet (or blade) wound at `hit` travelling along `dir`: carves a hole of `radius`, deals
  // `damage` times the zone multiplier, and may sever limbs or the head. The optional
  // transferred impulse is in N s on both living and dead bodies; negative uses the bullet default.
  WoundResult wound(const CharacterHit& hit, const V3& dir, f64 damage, f64 radius = 0.045, f64 impulse_ns = -1.0);
  // A melee blow landing at `point` travelling along `dir`: a fist or a foot (Blunt, force ~1 a
  // punch, ~1.8 a kick) or a blade (a slice of voxels is cut out, it bleeds).
  WoundResult melee(const V3& point, const V3& dir, HitKind kind, f64 force = 1.0);
  // Death: the muscles fade over `collapse` seconds; a killing blow at `point` (velocity change
  // `dv`) sends the body its way.
  void die(const V3* point = nullptr, const V3* dv = nullptr, f64 collapse = 0.6);
  std::optional<GibSpec> drop_weapon();  // the held prop as a gib (on death); the character lets go of it
  // A blast at `center` (radius of full effect, m; strength 1 ~ a rocket).
  BlastResult blast(const V3& center, f64 radius, f64 strength = 1.0);
  // The damage it took, to keep with its body (a corpse the world archives comes back as it was):
  // the limbs lost and the model's cells gone (encode_damage). Empty while whole.
  std::vector<u8> damage_record() const;
  // Made again with the damage it had (a character just made from the same model): the holes, the
  // limbs gone and the body without their use, a gun hand's prop let go. False if the record does
  // not fit the model (nothing changes).
  bool restore_damage(std::span<const u8> record);

  // ---- where things are
  V3 muzzle() const;
  V3 prop_point(const V3& p) const;
  void write_prop_skin(f32* out) const;
  V3 eyes() const;
  V3 limb_pos(Limb limb) const;
  i32 nearest_bone(const V3& p) const;
  i64 memory_bytes() const;

 private:
  BodyBackend backend_ = BodyBackend::Shallow;
  World* world_ = nullptr;
  CoreBinding binding_;
  u32 group_ = 0, tag_ = 0;
  std::vector<i32> part_full_;  // each model part's voxel count when whole (a limb mostly shot away is lost)
  ModelPtr whole_;              // the model it was made with (shared: its damage is told against it)
  f64 pain_ = 0.0;
  f64 firing_ = 0.0;
  f64 last_dt_ = 1.0 / 60.0;
  f64 calm_for_ = 0.0;
  f64 switch_blend_ = 1.0;  // blend from the last shown pose after a switch between physics and plan (1: done)
  WorldPose switch_from_;
  bool placed_ = false;
  bool pending_post_ = false;  // (deep: begin pushed the drives; end takes what the tick made of them)
  f64 frame_dt_ = 0.0;         // (the frame begun)
  Rng rng_;                    // (what the original left to Math.random: a character's own)

  void wake();
  void rest();
  bool bind_body();
  void finish_frame();
  void limit_turns(f64 max);
  void place_weapon();
  void own_model();
  V3 rest_dir(i32 b, const V3& dir) const;
  std::vector<GibSpec> sever_after_damage(i32 bone, const V3& dir);
  GibSpec gib_spec(VoxelPart p, const V3& dir, f64 speed);
  f64 random() { return rng_.next(); }
};

// Lets characters collide with each other this frame (the shallow path; deep bodies collide in the
// core): every simulated body gets the spheres of the bodies (alive or dead) within `reach` metres
// as obstacles, plus `extra` (debris) near it; the pushes of the last frame are handed on first.
// Hosts call it once a frame before updating the characters.
void gather_obstacles(const std::vector<Character*>& chars, f64 reach = 2.2, const std::vector<Obstacle>& extra = {});

}  // namespace svx::anim
