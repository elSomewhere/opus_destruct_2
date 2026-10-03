// svx_anim — a body in the core's world (the deep path): the bodies, joints and assists of a
// RigidSystem as a core articulation (svx/world/articulation.hpp), stepped by the World with
// everything else - standing on its structures and loading them, knocked by what hits it, pushing
// what it meets, colliding with the other bodies as they collide with each other.
//
// The RigidSystem stays what the behaviours read and write: a mirror of the articulation.
// Before a tick (push) the muscles, assists, forces and flags go to the articulation's control,
// and whatever the host did to the bodies since the last pull (a blow, a shove, a teleport, a
// limb lost) to its links; after a tick (pull) where the links are and what they felt comes back.
// Bound, the system is external: it is not stepped on its own.
#pragma once

#include <vector>

#include "svx/anim/physics/rigid.hpp"
#include "svx/world/world.hpp"

namespace svx::anim {

class CoreBinding {
 public:
  CoreBinding() = default;
  CoreBinding(const CoreBinding&) = delete;
  CoreBinding& operator=(const CoreBinding&) = delete;

  // Makes the articulation from the system as it is now (its bodies where they are, moving as
  // they move) and binds the system to it (external). False: refused (see World::add_articulation).
  bool bind(World& w, RigidSystem& s, u32 group = 0, u32 tag = 0, std::vector<u8> data = {});
  // Removes the articulation; the system keeps the bodies' last state and is its own again.
  void unbind(World& w);
  // Lets go of the articulation without removing it (the world keeps it, unbound).
  void release();
  // Binds to an articulation that is already in the world (a session loaded, one back from the
  // streaming archive): the system's layout must be the one it was made from.
  bool adopt(World& w, RigidSystem& s, ArticulationId id);
  bool bound() const { return id_ != 0; }
  ArticulationId id() const { return id_; }
  RigidSystem* system() const { return sys_; }

  // Before a tick (WorldSystem::pre_step, or between ticks): drives and changes to the core.
  void push(World& w);
  // After a tick: the links into the bodies (false: the articulation is gone - fallen out of the
  // world, removed; the binding is dropped and the system keeps its last state).
  bool pull(World& w, f64 dt);

  // The articulation desc a system makes (its bodies now).
  static ArticulationDesc desc_of(const RigidSystem& s);

 private:
  RigidSystem* sys_ = nullptr;
  ArticulationId id_ = 0;
  // the bodies at the last pull (what differs at a push, the host changed) and before the tick
  std::vector<V3> x_, v_, w_, start_;
  std::vector<Quat> q_;
  std::vector<u8> gone_;
  bool core_asleep_ = false;
  void snapshot();
};

}  // namespace svx::anim
