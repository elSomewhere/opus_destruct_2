// svx_anim — articulated rigid bodies, solved with extended position-based dynamics (XPBD, after
// Müller et al. 2020, "Detailed Rigid Body Simulation with Extended Position Based Dynamics"):
// many small substeps with one constraint pass each, which keeps long chains of light and heavy
// bodies (a hand on an arm on a chest) stiff and stable at any frame rate.
//
// - Bodies: mass, principal inertia (body frame), collision spheres.
// - Joints: a ball or a hinge with limits (an elliptical swing cone with its own reach in each of
//   four directions, a twist range, a hinge range) and a muscle: a drive towards a target
//   relative rotation with a stiffness (N m/rad), a damping (N m s/rad, on the relative angular
//   velocity), a torque limit, and a feed-forward torque.
// - Attachments pull a point of a body to a world point, and orienters turn a body towards a
//   world rotation: soft (compliance), force-limited, damped; the "assists" a controller uses to
//   hold a body up the way legs would, or to pin a planted foot.
// - Contacts: spheres against a CollisionWorld (found once per step with a margin, solved every
//   substep as planes) with static and dynamic friction, and sphere pairs between bodies of the
//   same system (arms against the trunk, leg against leg).
// - Sleep when everything has been slow for a while.
//
// This is a character's body in two ways. Stepped here (RigidSystem::step), it is the body on
// its own: the shallow path, a body that meets the world through its CollisionWorld and others
// through obstacles. Or it is the description and the mirror of a core articulation
// (core_binding.hpp): the core steps it with everything else - the deep path - and the bodies
// here carry what the core made of them back to the behaviours.
#pragma once

#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "svx/anim/math.hpp"
#include "svx/anim/physics/collision.hpp"

namespace svx::anim {

constexpr f64 kInf = std::numeric_limits<f64>::infinity();

// A collision sphere in a body's frame (centre relative to the centre of mass).
struct Sphere {
  V3 c;
  f64 r = 0.0;
};

class RigidBody {
 public:
  RigidBody(f64 mass, const V3& inertia, const V3& x, const Quat& q, std::vector<Sphere> spheres = {});

  // centre of mass and rotation (world)
  V3 x;
  Quat q;
  // linear and angular velocity (world)
  V3 v, w;
  // pose at the start of the substep
  V3 px;
  Quat pq;
  // velocities before the substep's constraints
  V3 v0, w0;
  f64 mass = 0.0, inv_mass = 0.0;
  V3 inv_i;  // principal inverse inertia in the body frame
  // gone (a limb shot off): it touches nothing and weighs next to nothing
  bool gone = false;
  std::vector<Sphere> spheres;
  // external force and torque (world) applied every substep of the next step, then cleared
  V3 force, torque;
  f64 friction = 0.8;  // (against the world)
  // touched the world during the last step, the last contact normal, and where (world)
  bool contact = false;
  V3 contact_normal{0, 0, 1};
  V3 contact_point;
  f64 impact = 0.0;  // largest contact impulse of the last step (N s): how hard it hit something
  // How hard obstacles (other bodies, debris, the player) pushed it sideways over the last step:
  // the horizontal impulse they gave it (N s; resting on them does not count).
  f64 bumped = 0.0;
  // Passes through other bodies (not the world): a limb that strikes, whose blow the host deals
  // (it is neither stopped by the body it hits nor one that body runs into).
  bool ghost = false;
  i32 index = -1;  // (in its system)
  // The body's long axis (body frame) and how fast a spin about it dies away (1/s): tissue resists
  // twisting a limb (a light limb would otherwise spin freely about its length).
  V3 long_axis{0, 0, 1};
  f64 twist_damping = 0.0;
  // World inverse inertia R diag(inv_i) R^T (symmetric: xx, yy, zz, xy, xz, yz), refreshed once per
  // substep (constraints within a substep use it as the body turns a little: the usual
  // approximation).
  f64 iw[6] = {0, 0, 0, 0, 0, 0};

  // The body is gone (a severed limb, now a gib): it keeps a token mass and touches nothing.
  void lose();
  void update_inertia();  // the world inverse inertia from the current rotation
  V3 inv_inertia_mul(const V3& v) const;  // I^-1 v (world, with the substep's inertia)
  f64 inv_mass_at(const V3& r, const V3& n) const;  // generalized inverse mass along unit n at offset r (world)
  f64 inv_mass_rot(const V3& n) const;              // ... of a rotation about unit n
  void apply_pos(const V3& p, const V3& r);         // a positional impulse p at offset r (world): moves and turns the body
  void apply_rot(const V3& l);                      // an angular positional impulse (world)
  void rotate(const V3& a);                         // q += 1/2 [a, 0] q (a small rotation vector), normalized
  void apply_impulse(const V3& j, const V3& r);     // velocity change: an impulse j (N s) at offset r (world)
  V3 point(const V3& local) const { return rotate_by(q, local) + x; }  // world position of a body-frame point
  V3 point_velocity(const V3& local) const;
  static V3 rotate_by(const Quat& q, const V3& v) { return svx::rotate(q, v); }
};

// Limits of a ball joint: how far B's twist axis may tilt towards +x, -x, +y, -y of A's joint frame.
struct SwingLimits {
  f64 x_pos = 0.0, x_neg = 0.0, y_pos = 0.0, y_neg = 0.0;
};

enum class JointKind : u8 { Ball, Hinge };

struct JointOptions {
  JointKind kind = JointKind::Ball;  // Ball: swing and twist limits; Hinge: about the joint frame's x axis
  V3 anchor_a, anchor_b;             // (the bodies' frames)
  // The joint frame in each body's frame (z: the twist axis, B's bone direction; x: the hinge
  // axis). At the rest pose both frames coincide in the world.
  Quat frame_a, frame_b;
  std::optional<SwingLimits> swing;
  std::optional<std::pair<f64, f64>> twist;  // range about z (ball)
  std::optional<std::pair<f64, f64>> hinge;  // angle range about x from A's z to B's z (hinge)
};

class Joint {
 public:
  Joint(RigidBody* a, RigidBody* b, const JointOptions& o);
  RigidBody* a;
  RigidBody* b;
  JointKind kind;
  V3 anchor_a, anchor_b;
  Quat frame_a, frame_b;
  std::optional<SwingLimits> swing;
  std::optional<std::pair<f64, f64>> twist;
  std::optional<std::pair<f64, f64>> hinge;
  Quat target;  // drive target: B's rotation relative to A (body frames: qB = qA target)
  f64 stiffness = 0.0;  // drive stiffness (N m/rad; 0: no drive)
  f64 damping = 0.0;    // damping torque per unit relative angular velocity (N m s/rad), relative to target_vel
  // The drive target's relative angular velocity (B relative to A, in A's frame, rad/s): the
  // damping works towards it, so a joint follows a moving target instead of dragging on it.
  V3 target_vel;
  f64 max_torque = kInf;  // largest drive torque (N m)
  // The inertia the joint really moves (B's whole limb, kg m^2; 0: just the two bodies). The
  // damping slows the relative spin at the rate it would slow that limb: applied between two
  // light bodies alone it would lock them together within a substep.
  f64 eff_inertia = 0.0;
  V3 feed;  // feed-forward torque on B (world, N m; A gets the reaction), e.g. gravity compensation
};

// Pulls a body point to a world point (soft, force-limited, damped).
class Attachment {
 public:
  Attachment(RigidBody* body_, const V3& local_) : body(body_), local(local_) {}
  RigidBody* body;
  V3 local;  // the point in the body's frame
  V3 target;
  V3 target_vel;  // velocity of the target (the damping works relative to it)
  RigidBody* reference = nullptr;  // optional physical anchor; receives the opposite force
  V3 reference_local;
  f64 stiffness = kInf;  // N/m (kInf: rigid)
  f64 max_force = kInf;  // N
  f64 damping = 0.0;     // damping force per unit velocity of the point relative to the target's (N s/m)
  bool axes[3] = {true, true, true};  // which world axes it acts on
  bool enabled = false;
  V3 applied;  // force it applied in the last substep (N, world)
};

// Turns a body towards a world rotation (soft, torque-limited, damped).
class Orienter {
 public:
  explicit Orienter(RigidBody* body_) : body(body_) {}
  RigidBody* body;
  Quat target;
  f64 stiffness = kInf;
  f64 max_torque = kInf;
  f64 damping = 0.0;  // damping torque per unit angular velocity (N m s/rad)
  bool tilt_only = false;  // only tilt (the body's `up` axis towards the target's), leaving the heading free
  V3 up{0, 0, 1};          // the body-frame axis tilted towards the target's (tilt_only)
  bool enabled = false;
};

// A push this system gave an obstacle's body (world impulse at a point), to hand on.
struct Reaction {
  i32 owner = -1, part = -1;
  V3 j, at;
};

// A pair of spheres of two bodies that must not overlap.
struct SpherePair {
  RigidBody* a = nullptr;
  i32 sa = 0;
  RigidBody* b = nullptr;
  i32 sb = 0;
};

struct RigidSystemOptions {
  f64 gravity = 9.81;
  f64 max_substep = 1.0 / 480.0;  // longest substep (s)
  f64 margin = 0.02;              // collision margin (m) added to every sphere when looking for contacts
};

class RigidSystem {
 public:
  explicit RigidSystem(const CollisionWorld* collision, const RigidSystemOptions& o = {});
  RigidSystem(const RigidSystem&) = delete;
  RigidSystem& operator=(const RigidSystem&) = delete;

  std::vector<std::unique_ptr<RigidBody>> bodies;
  std::vector<std::unique_ptr<Joint>> joints;
  std::vector<std::unique_ptr<Attachment>> attachments;
  std::vector<std::unique_ptr<Orienter>> orienters;
  std::vector<SpherePair> pairs;  // sphere pairs of bodies that must not overlap (checked with a margin once per step)
  const CollisionWorld* collision = nullptr;
  std::vector<Obstacle> obstacles;  // other things to collide with this step (spheres, world; the host fills it)
  std::vector<Reaction> reactions;  // the pushes the last step gave obstacles that belong to bodies (to apply to them)
  f64 gravity = 9.81;
  f64 max_substep = 1.0 / 480.0;
  f64 margin = 0.02;
  f64 pair_speed = 1.5;       // how fast overlapping parts of the same system are pushed apart at most (m/s)
  bool pairs_enabled = true;  // parts of the same system keep apart (off: a body that has come to rest)
  // The fastest any body may spin (rad/s): a limp body's parts turn no faster than a limb flung
  // loose does (an impact on a light part does not set it whirling).
  f64 spin_cap = 80.0;
  // the most the constraints change a body's velocity in one substep (m/s, rad/s)
  f64 max_dv = 1.2, max_dw = 12.0;
  f64 linear_drag = 0.98, angular_drag = 0.9;  // air drag: velocity kept per second
  bool asleep = false;
  // Stepped elsewhere (a core articulation: core_binding.hpp): step() does nothing, and try_sleep
  // lets the system sleep (the core puts it to sleep when it rests) rather than putting it to sleep.
  bool external = false;
  bool may_sleep = false;  // (external) try_sleep was asked since the binding last took it
  f64 still = 0.0;  // seconds nothing has moved further than still_distance (m)
  f64 still_distance = 0.02;
  f64 last_speed = 0.0;  // the fastest body's speed over the last step (m/s)
  f64 substep = 1.0 / 480.0;  // (of the last step, s)

  RigidBody* add(std::unique_ptr<RigidBody> b);
  Joint* add_joint(std::unique_ptr<Joint> j);
  Attachment* attach(RigidBody* body, const V3& local);
  Orienter* orienter(RigidBody* body);
  void wake();
  void step(f64 dt);  // advances the system by dt (split into substeps)
  // Puts the system to sleep if it has been still for `after` seconds (callers decide when it may).
  bool try_sleep(f64 after = 0.6);
  // The fastest body's speed and the stillness, from where the bodies were before a step of dt
  // (`start`, per body): the end of step(), or of a core tick.
  void note_motion(f64 dt, const std::vector<V3>& start);
  i64 memory_bytes() const;

 private:
  struct WorldContact {
    RigidBody* body = nullptr;
    V3 c;  // sphere centre (body frame) and radius
    f64 r = 0.0;
    V3 n{0, 0, 1};  // plane n . p = d (the surface), n towards the body
    f64 d = 0.0;
    f64 lambda = 0.0;  // normal impulse of the last substep (0: not touching)
    bool obstacle = false;  // (against an obstacle, not the world)
    // the obstacle's owner (its reaction; -1: none), its velocity, and the momentum this contact
    // took from us (N s)
    i32 other_owner = -1, other_part = -1;
    V3 other_v;
    f64 took = 0.0;
  };
  struct LimitHit {
    RigidBody* a = nullptr;
    RigidBody* b = nullptr;
    V3 n;
  };

  std::vector<V3> still_at_;
  bool anchored_ = false;
  std::vector<SpherePair> near_pairs_;
  std::vector<WorldContact> contacts_;
  size_t contact_count_ = 0;
  std::vector<V3> frame_start_;
  std::vector<LimitHit> limit_hits_;
  size_t limit_count_ = 0;

  void find_contacts(f64 dt);
  void find_obstacles(f64 dt);
  void find_pairs(f64 dt);
  WorldContact& add_contact(RigidBody* b, const Sphere& s, const V3& n, const V3& c, f64 rr);
  void substep_once(f64 h);
  void solve_joint(Joint& j, f64 h);
  void align(RigidBody* A, RigidBody* B, const V3& ax, const V3& bx);
  void limit_angle(RigidBody* A, RigidBody* B, const V3& n, const V3& n1, const V3& n2, f64 lo, f64 hi);
  void limit_hit(RigidBody* A, RigidBody* B, const V3& n, f64 sign);
  void limit_velocity();
  void turn(RigidBody* A, RigidBody* B, const V3& n, f64 angle, f64 compliance, f64 max_impulse);
  void damp_joint(Joint& j, f64 h);
  void solve_attachment(Attachment& a, f64 h);
  void damp_attachment(Attachment& a, f64 h);
  void solve_orienter(Orienter& o, f64 h);
  void damp_orienter(Orienter& o, f64 h);
  void solve_pair(const SpherePair& p);
  void solve_contact(WorldContact& k);
  void contact_velocity(WorldContact& k, f64 h);
};

}  // namespace svx::anim
