// svx_anim tests: a place for characters, on either path. The shallow path runs the bodies on
// their own (as the original did) against a CollisionWorld; the deep path makes the same ground
// the voxels of a core World and runs the bodies as its articulations, ticking the world between
// their drives and their update (as a CharacterSystem does).
#pragma once

#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "svx/anim/character.hpp"
#include "svx/anim/characters/humans.hpp"
#include "svx/anim/physics/world_collision.hpp"
#include "svx/world/world.hpp"

namespace scene {

using namespace svx;
using namespace svx::anim;

constexpr f64 DT = 1.0 / 60.0;
constexpr f64 kH = 0.125;

enum class Path { Shallow, Deep };
inline const char* path_name(Path p) { return p == Path::Shallow ? "shallow" : "deep"; }

// The ground: voxels solid where `solid` says (voxel (i, j, k) centred at kH (i, j, k)), within
// [lo, hi); or flat (null solid: the plane z = 0 on the shallow path, voxels k <= 0 - their top at
// kH / 2 - on the deep one: -16..16 m across, -16..32 m along y, the way the characters face).
using Solid = std::function<bool(i32, i32, i32)>;

class Scene {
 public:
  explicit Scene(Path p, Solid solid = nullptr, IVec3 lo = {-128, -128, -4}, IVec3 hi = {128, 256, 40}) : path(p) {
    if (path == Path::Shallow) {
      if (solid) col = std::make_unique<VoxelCollision>(kH, solid);
      else col = std::make_unique<FlatGround>(0.0);
      ground = 0.0;
      return;
    }
    world = std::make_unique<World>();
    {
      WorldConfig cfg;
      if (const char* v = std::getenv("SVX_LINK_IT")) cfg.rigid.link_iterations = std::atoi(v);
      if (const char* v = std::getenv("SVX_LINK_PIT")) cfg.rigid.link_position_iterations = std::atoi(v);
      if (const char* v = std::getenv("SVX_LINK_SUB")) cfg.rigid.link_substeps = std::atoi(v);
      if (const char* v = std::getenv("SVX_LINK_WARM")) cfg.rigid.link_warm = std::atof(v);
      world->configure(cfg);
    }
    VoxelGrid g;
    g.h = kH;
    const Vox rock = make_vox(MaterialId::Rock, true);
    for (i32 x = lo[0]; x < hi[0]; ++x)
      for (i32 y = lo[1]; y < hi[1]; ++y) {
        if (!solid) {
          g.fill_column(x, y, lo[2], 1, rock);
          continue;
        }
        for (i32 z = lo[2]; z < hi[2]; ++z)
          if (solid(x, y, z)) g.set(x, y, z, rock);
      }
    g.compact();
    g.lo = lo;
    g.hi = {hi[0], hi[1], hi[2] + 32};
    world->load(std::move(g));
    col = std::make_unique<WorldCollision>(*world);
    ground = solid ? 0.0 : 0.5 * kH;
  }

  Path path;
  std::unique_ptr<World> world;
  std::unique_ptr<CollisionWorld> col;
  f64 ground = 0.0;  // (flat) the ground's height
  std::vector<std::unique_ptr<Character>> chars;

  Character& add(const HumanVariant& v, f64 seed, f64 yaw, const V3& at, PropPtr weapon = nullptr) {
    CharacterOptions o;
    o.model = v.model;
    o.palette = v.palette;
    o.collision = col.get();
    o.weapon = std::move(weapon);
    o.seed = seed;
    o.backend = path == Path::Deep ? BodyBackend::Deep : BodyBackend::Shallow;
    o.world = world.get();
    chars.push_back(std::make_unique<Character>(o));
    Character& c = *chars.back();
    c.place(at, yaw);
    return c;
  }
  Character& civilian(f64 seed = 3.0, f64 yaw = 1.5707963267948966, V3 at = V3{}, bool on_ground = true) {
    if (on_ground) at.z += ground;
    return add(make_civilian(static_cast<i32>(seed)), seed, yaw, at);
  }
  Character& soldier() { return add(make_soldier(4), 4.0, 1.5707963267948966, V3{0, 0, ground}, prop_archetype("rifle")); }

  // One frame: `host` does what a game does before the characters' frame (their roots, their
  // obstacles), then the characters' frames - on the deep path in two halves about the world's
  // tick (the plans and drives, then what the tick made of the bodies).
  void frame(const std::vector<Character*>& cs, const std::function<void()>& host = nullptr) {
    if (host) host();
    for (Character* c : cs) c->begin(DT);
    if (path == Path::Deep) world->tick();
    for (Character* c : cs) {
      c->end();
      for (const V3& p : c->pose.p) REQUIRE((std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)));
    }
  }
};

struct Host {
  V3 pos;
  f64 yaw = 0.0;
  f64 v = 0.0;  // the walking speed it has built up
};

inline Host host_of(const Character& c) { return Host{c.motion.root_pos, c.motion.root_yaw, 0.0}; }

// Runs a character as a game would: the host walks it along its facing at `speed` (m/s,
// accelerating over half a second) and follows the body's root motion while the body leads.
inline Host run(Scene& s, Character& c, f64 seconds, f64 speed = 0.0, const std::function<void(Character&, f64, Host&)>& each = nullptr,
                std::optional<Host> start = std::nullopt) {
  Host h = start ? *start : host_of(c);
  f64 v = h.v;
  const i32 n = static_cast<i32>(std::floor(seconds * 60.0 + 0.5));
  for (i32 i = 0; i < n; ++i) {
    s.frame({&c}, [&] {
      const V3 rm = c.take_root_motion();
      if (c.controlled()) {
        h.pos = h.pos + rm;
        h.yaw = c.motion.root_yaw;
        v = 0.0;
      } else {
        v = std::min(speed, v + (speed / 0.5) * DT);
        h.pos.x += std::cos(h.yaw) * v * DT;
        h.pos.y += std::sin(h.yaw) * v * DT;
      }
      c.set_root(h.pos, h.yaw);
    });
    h.v = v;
    if (each) each(c, (i + 1) * DT, h);
  }
  return h;
}

inline V3 chest_up(const Character& c) { return rotate(c.pose.q[H::chest], V3{0, 0, 1}); }

}  // namespace scene
