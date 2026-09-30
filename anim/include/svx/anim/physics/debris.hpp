// svx_anim — gibs and blood: debris for the host to draw, on top of a CollisionWorld.
//
// - Gibs are rigid voxel chunks (a severed limb, a head, a piece of torso, a dropped weapon): a
//   VoxelPart in rest model space moved by a rigid transform (x -> pos + rot (x - pivot), the
//   pivot the part's rest centre of mass). They move as rigid bodies with the inertia of a solid
//   box (the part's cell bounds, ~1000 kg/m^3). Collision samples a few dozen surface cells as
//   spheres of half a voxel against the world (sequential impulses with restitution and Coulomb
//   friction, then positional correction), in substeps short enough that no sample point moves
//   more than a few centimetres in one (nothing tunnels at 20 m/s). Gibs sleep at rest and wake on
//   impulses or when what they rest on is gone.
// - Blood drops are ballistic particles (gravity, light drag). A drop that hits the world (swept
//   with a ray) becomes a stain on that surface - position, unit normal, size - and stains close
//   together on one surface merge and grow. The host draws drops as small cubes and stains as
//   decals or flat voxels.
//
// Sizes are radii (half-extents) in metres. Deterministic for a given seed and call sequence. The
// memory is bounded: at most max_gibs, max_drops and max_stains (the oldest make room).
//
// A character's pieces (Character's gib specs) become gibs here as the original game made them:
// spawn_gib and the helpers at the end.
#pragma once

#include <deque>
#include <memory>
#include <vector>

#include "svx/anim/characters/palette.hpp"
#include "svx/anim/physics/collision.hpp"

namespace svx::anim {

// The colour of blood (linear RGB).
inline constexpr Rgb kBlood = {0.22, 0.008, 0.008};

struct Gib {
  u32 id = 0;
  VoxelPart part;  // its cells in rest model space (a lattice of voxel_size)
  f64 voxel_size = 0.0;
  // the rigid transform: a rest-space point x is at pos + rot (x - pivot)
  V3 pos;
  Quat rot;
  V3 pivot;          // the rest-space centre of mass of the part's cells
  V3 vel, ang;       // world linear (m/s) and angular (rad/s) velocity
  f64 radius = 0.0;  // bounding radius about the pivot
  bool asleep = false;
  f64 age = 0.0;    // seconds since it was spawned
  f64 bleed = 0.0;  // blood drops a second it sheds while it moves (0: none; the host sets it)
  u64 user = 0;     // the host's (a mesh, a palette): never read here
};

struct BloodDrop {
  V3 pos, vel;
  f64 size = 0.0;  // radius (m)
  f64 age = 0.0;
  Rgb color{};  // linear RGB
};

struct BloodStain {
  V3 pos;
  V3 normal{0, 0, 1};  // unit surface normal (out of the surface)
  f64 size = 0.0;      // radius (m)
  f64 age = 0.0;
  Rgb color{};
};

struct GibSystemOptions {
  i32 max_gibs = 128, max_drops = 800, max_stains = 600;
  f64 gravity = 9.81;   // m/s^2
  f64 kill_z = -200.0;  // gibs below this height are removed (m)
  u32 seed = 0x61b5;    // (of the spray and the spin)
};

class GibSystem {
 public:
  explicit GibSystem(const CollisionWorld* collision, const GibSystemOptions& o = {});
  GibSystem(const GibSystem&) = delete;
  GibSystem& operator=(const GibSystem&) = delete;

  const CollisionWorld* collision = nullptr;
  // A gib is the system's until it goes (remove, the room a new one needs, below kill_z, clear):
  // hosts know theirs by id.
  std::vector<std::unique_ptr<Gib>> gibs;
  std::deque<BloodDrop> drops;    // (oldest first)
  std::deque<BloodStain> stains;  // (oldest first)
  i32 max_gibs = 128, max_drops = 800, max_stains = 600;
  f64 gravity = 9.81;
  f64 kill_z = -200.0;

  // Adds a gib whose part is where the bone's world transform puts it now (x -> bone_rot (x -
  // bone_rest_head) + bone_pos), moving at `vel` and spinning at `ang` (world). With max_gibs
  // already, the oldest sleeping gib goes first, else the oldest.
  Gib* spawn(VoxelPart part, f64 voxel_size, const V3& bone_pos, const Quat& bone_rot, const V3& bone_rest_head, const V3& vel,
             const V3& ang, u64 user = 0);
  void remove(const Gib* g);  // (the host let go of it)
  V3 world_point(const Gib& g, const V3& rest) const;  // the world position of a rest-space point of the gib
  // The skin matrix (16 floats, column-major) of the gib's part mesh, built in rest space.
  void write_skin(const Gib& g, f32* out) const;
  // A radial push (explosions): gibs and drops within `radius` of `center` get up to `speed` m/s
  // away from it (falling off linearly, a little upwards, and some spin); sleeping gibs wake.
  void impulse(const V3& center, f64 radius, f64 speed);
  void update(f64 dt);  // advances the gibs, the drops and the stains by dt seconds
  // Sprays `count` drops from `pos` along `dir` (any length) at up to `speed` m/s, scattered by
  // `spread` (0: a jet, 1: a hemisphere), in `color` (linear RGB).
  void spray(const V3& pos, const V3& dir, i32 count, f64 speed, f64 spread, const Rgb& color = kBlood);
  // fn(pos, size, color) per drop; fn(pos, normal, size, age, color) per stain.
  template <class F>
  void for_each_drop(F&& fn) const {
    for (const BloodDrop& d : drops) fn(d.pos, d.size, d.color);
  }
  template <class F>
  void for_each_stain(F&& fn) const {
    for (const BloodStain& s : stains) fn(s.pos, s.normal, s.size, s.age, s.color);
  }
  void clear();
  i64 memory_bytes() const;

 private:
  // A gib's physics (bodies_[i] is gibs[i]'s).
  struct Body {
    f64 mass = 0.0, inv_mass = 0.0;
    V3 inv_i;                 // inverse principal inertia in the rest frame (the box axes)
    std::vector<V3> samples;  // collision sample points: rest-space offsets from the pivot
    f64 sleep_timer = 0.0, support_timer = 0.0, bleed_acc = 0.0;
    f64 calm_v = 0.0, calm_w = 0.0;  // low-passed speed and spin (the sleep test)
  };
  struct Contact {
    V3 rw, n;  // the sample's offset (world) and the contact normal
  };

  std::vector<Body> bodies_;
  Rng rng_;
  u32 next_id_ = 1;
  std::vector<Contact> contacts_;  // (of a substep)

  void make_room();
  void step_gib(Gib& g, Body& b, f64 h);
  void check_support(Gib& g, Body& b, f64 dt);
  void add_drop(const V3& pos, const V3& vel, f64 size, const Rgb& color);
  void update_drops(f64 dt);
  V3 surface_normal(const V3& hit, const V3& dir) const;
  void add_stain(const V3& pos, const V3& normal, f64 size, const Rgb& color);
};

// ---- a character's pieces (character.hpp): what the original game made of them

struct GibSpec;
struct WoundResult;
struct BlastResult;
class Character;

// Blood drops a second a piece of a character sheds as it flies: a limb or a head shot or cut off,
// a piece of a body a blast tore apart (a dropped weapon: none).
constexpr f64 kSeveredBleed = 30.0, kBlastBleed = 40.0;

// A piece that came off a character (Character::wound, melee, blast, drop_weapon) as a gib: its
// part where its bone had it, moving and spinning as it came off, shedding `bleed` drops a second.
Gib* spawn_gib(GibSystem& gibs, const GibSpec& spec, f64 bleed, u64 user = 0);
// A round's wound (Character::wound, the hit at `point` along `dir`): blood sprays along the shot
// and back out of the wound, a drop in the colour of every fourth voxel carved out, and what the
// round severed flies off bleeding. Its gibs, in the order of r.gibs (a prop among them: the gun a
// severed hand let go of).
std::vector<Gib*> wound_gibs(GibSystem& gibs, const Character& c, const WoundResult& r, const V3& point, const V3& dir, u64 user = 0);
// A blast's (Character::blast at `center`): a body torn apart sprays blood up from its middle and
// its pieces fly off bleeding; a body only hurt, badly, sprays blood away from the blast. Its gibs,
// in the order of r.gibs. (Once every character had its blast, the original pushed what it
// reached: gibs.impulse(center, 4 radius, 11 strength).)
std::vector<Gib*> blast_gibs(GibSystem& gibs, const Character& c, const BlastResult& r, const V3& center, u64 user = 0);
// The held prop the dead let go of (Character::drop_weapon) as a gib, thrown 1.5 m/s further along
// `dir` (the killing blow's). Null without one.
Gib* drop_weapon_gib(GibSystem& gibs, Character& c, const V3& dir, u64 user = 0);

}  // namespace svx::anim
