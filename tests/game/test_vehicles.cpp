// Vehicles (docs/VEHICLES.md): their models, and vehicles in a game - driven by a player,
// crashed, driven by the traffic of a streamed city, saved and replayed.
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

#include "doctest.h"
#include "svx/game/vehicles.hpp"
#include "svx/material/material.hpp"

using namespace svx;

namespace {

// The voxels of a grid, and how many of them hold together with the first (over faces).
struct Census {
  i64 voxels = 0, connected = 0;
  f64 mass = 0.0;
};
Census census(const VoxelGrid& g) {
  Census c;
  std::vector<IVec3> all;
  for (const auto& [k, ch] : g.chunks()) {
    if (ch.uniform && !vox_solid(ch.value)) continue;
    const IVec3 cc = unkey3(k);
    for (int i = 0; i < kChunkVox; ++i) {
      const Vox v = ch.uniform ? ch.value : ch.v[size_t(i)];
      if (!vox_solid(v)) continue;
      all.push_back({cc[0] * kChunk + i / (kChunk * kChunk), cc[1] * kChunk + (i / kChunk) % kChunk, cc[2] * kChunk + i % kChunk});
      c.mass += default_materials()[vox_mat(v)].rho * g.h * g.h * g.h;
    }
  }
  c.voxels = static_cast<i64>(all.size());
  if (all.empty()) return c;
  std::vector<IVec3> stack{all.front()};
  std::unordered_set<u64> set{key3(all.front()[0], all.front()[1], all.front()[2])};
  while (!stack.empty()) {
    const IVec3 p = stack.back();
    stack.pop_back();
    ++c.connected;
    for (int a = 0; a < 3; ++a)
      for (int s = -1; s <= 1; s += 2) {
        IVec3 q = p;
        q[a] += s;
        if (!vox_solid(g.get(q))) continue;
        if (!set.insert(key3(q[0], q[1], q[2])).second) continue;
        stack.push_back(q);
      }
  }
  return c;
}

}  // namespace

TEST_CASE("vehicles: every model is one body of a car's mass, with its wheels' struts under their mounts") {
  for (int k = 0; k < static_cast<int>(VehicleKind::Count); ++k) {
    const VehicleModel m = build_vehicle({static_cast<VehicleKind>(k), Paint::Red});
    const Census c = census(m.voxels);
    MESSAGE("kind " << k << ": " << c.voxels << " voxels, " << c.mass << " kg, " << m.wheels.size() << " wheels");
    CHECK(c.voxels > 3000);
    CHECK(c.connected == c.voxels);  // (it holds together)
    CHECK(m.voxels.h == kVehicleVoxel);
    CHECK(m.voxels.layer_index("paint") >= 0);
    CHECK(m.wheels.size() >= 4);
    for (const WheelSlot& w : m.wheels) {
      // (a voxel of the model within a voxel of the mount)
      const IVec3 p{static_cast<i32>(std::floor(w.mount.x / kVehicleVoxel + 0.5)), static_cast<i32>(std::floor(w.mount.y / kVehicleVoxel + 0.5)),
                    static_cast<i32>(std::floor(w.mount.z / kVehicleVoxel + 0.5))};
      bool near = false;
      for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
          for (int dz = -1; dz <= 1; ++dz) near = near || vox_solid(m.voxels.get({p[0] + dx, p[1] + dy, p[2] + dz}));
      CHECK(near);
      CHECK(w.rest > w.travel);
      CHECK(w.mount.z - w.rest < w.radius);  // (at full droop its wheel reaches below the ground)
    }
    if (m.spec.kind == VehicleKind::Truck) {
      CHECK(c.mass > 2500.0);
    } else {
      CHECK(c.mass > 700.0);
      CHECK(c.mass < 3200.0);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Vehicles in a game

#include "svx/base/parallel.hpp"
#include "svx/game/game.hpp"
#include "svx/game/replay.hpp"

namespace {

void fill(VoxelGrid& g, const IVec3& lo, const IVec3& hi, Vox v) {
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y) g.fill_column(x, y, lo[2], hi[2], v);
}

// A flat road 240 m x 40 m (asphalt, anchored), and a concrete wall across its far end.
void load_road(Game& game, bool wall = false) {
  VoxelGrid g;
  g.h = 0.125;
  fill(g, {-960, -160, -4}, {960, 160, 0}, make_vox(MaterialId::Asphalt, true));
  if (wall) {
    fill(g, {600, -40, -4}, {606, 40, 0}, make_vox(MaterialId::Rc, true));
    fill(g, {601, -40, 0}, {605, 40, 24}, make_vox(MaterialId::Rc, false));
  }
  g.lo = {-960, -160, -4};
  g.hi = {960, 160, 64};
  g.compact();
  game.load(std::move(g), V3{0, 0, 0}, V3{1, 0, 0});
  game.bake();
}

}  // namespace

TEST_CASE("vehicles: a car settles on its wheels; driven it shifts up to speed, steers, brakes and reverses") {
  Game game;
  load_road(game);
  const u32 id = game.spawn_vehicle({VehicleKind::Sedan, Paint::Blue}, V3{-100, 0, -0.0625}, 0.0);
  REQUIRE(id != 0);
  for (int t = 0; t < 120; ++t) game.tick();
  VehicleView v;
  REQUIRE(game.vehicle(id, &v));
  CHECK(v.chassis != 0);
  CHECK(v.wheels == 4);
  CHECK(v.paint == Paint::Blue);
  i32 contact = 0;
  for (const WheelView& w : game.wheel_views()) contact += w.vehicle == id && w.contact ? 1 : 0;
  CHECK(contact == 4);
  const Body* b = game.world().piece(v.chassis);
  REQUIRE(b);
  MESSAGE("sedan: " << b->mass << " kg, centre of mass " << b->x.z << " m up, " << norm(b->v) << " m/s at rest");
  CHECK(norm(b->v) < 0.1);
  // the player gets in and floors it
  REQUIRE(game.enter_vehicle(id));
  CHECK(game.player_vehicle() == id);
  VehicleInput in;
  in.throttle = 1.0;
  game.drive(in);
  int top_gear = 0;
  f64 top_rpm = 0.0;
  for (int t = 0; t < 360; ++t) {
    game.tick();
    REQUIRE(game.vehicle(id, &v));
    top_gear = std::max(top_gear, v.gear);
    top_rpm = std::max(top_rpm, v.rpm);
  }
  MESSAGE("after 6 s of throttle: " << v.speed << " m/s (" << v.speed * 3.6 << " km/h), gear " << v.gear << " (top " << top_gear << "), rpm "
                                    << v.rpm << " (top " << top_rpm << ")");
  CHECK(v.speed > 16.0);
  CHECK(top_gear >= 2);
  CHECK(top_rpm <= vehicle_model(VehicleKind::Sedan).tuning.redline * 1.02 + 1.0);
  CHECK(std::abs(v.vel.y) < 1.0);
  // steering left at speed: it turns left
  in.throttle = 0.3;
  in.steer = 0.5;
  game.drive(in);
  f64 yaw_rate = 0.0;
  for (int t = 0; t < 60; ++t) {
    game.tick();
    REQUIRE(game.vehicle(id, &v));
    yaw_rate = game.world().piece(v.chassis)->w.z;
  }
  MESSAGE("steering left at " << v.speed << " m/s: yaw rate " << yaw_rate << " rad/s");
  CHECK(yaw_rate > 0.15);
  // braking: it stops
  in = VehicleInput{};
  in.brake = 1.0;
  game.drive(in);
  for (int t = 0; t < 300; ++t) game.tick();
  REQUIRE(game.vehicle(id, &v));
  MESSAGE("braked 5 s: " << v.speed << " m/s");
  CHECK(std::abs(v.speed) < 0.3);
  // reversing: throttle backwards from standstill
  in = VehicleInput{};
  in.throttle = -1.0;
  game.drive(in);
  for (int t = 0; t < 120; ++t) game.tick();
  REQUIRE(game.vehicle(id, &v));
  MESSAGE("2 s in reverse: " << v.speed << " m/s, gear " << v.gear);
  CHECK(v.speed < -2.0);
  CHECK(v.gear == -1);
  // out: the handbrake holds it
  game.exit_vehicle();
  CHECK(game.player_vehicle() == 0);
  for (int t = 0; t < 180; ++t) game.tick();
  REQUIRE(game.vehicle(id, &v));
  CHECK(std::abs(v.speed) < 0.5);
}

TEST_CASE("vehicles: every kind drives off, and none of them tips over in a hard turn at town speed") {
  for (int k = 0; k < static_cast<int>(VehicleKind::Count); ++k) {
    Game game;
    load_road(game);
    const u32 id = game.spawn_vehicle({static_cast<VehicleKind>(k), Paint::White}, V3{-100, 0, -0.0625}, 0.0);
    REQUIRE(id != 0);
    for (int t = 0; t < 90; ++t) game.tick();
    REQUIRE(game.enter_vehicle(id));
    VehicleInput in;
    in.throttle = 1.0;
    game.drive(in);
    VehicleView v;
    for (int t = 0; t < 240; ++t) game.tick();
    REQUIRE(game.vehicle(id, &v));
    const f64 v4 = v.speed;
    in.throttle = 0.2;
    in.steer = 1.0;
    game.drive(in);
    f64 lowest_up = 1.0;
    for (int t = 0; t < 180; ++t) {
      game.tick();
      REQUIRE(game.vehicle(id, &v));
      lowest_up = std::min(lowest_up, rotate(v.rot, V3{0, 0, 1}).z);
    }
    MESSAGE("kind " << k << ": " << v4 << " m/s after 4 s; full lock at speed: its up axis at least " << lowest_up << " up");
    CHECK(v4 > (k == static_cast<int>(VehicleKind::Truck) ? 5.0 : 8.0));
    CHECK(lowest_up > 0.8);
    CHECK(v.wheels == static_cast<i32>(vehicle_model(static_cast<VehicleKind>(k)).wheels.size()));
  }
}

TEST_CASE("vehicles: a player's drive replays bit for bit, and a saved session brings its car back to drive on") {
  auto play = [](Game& game, CommandLog* log) {
    load_road(game);
    game.record_to(log);
    const u32 id = game.spawn_vehicle({VehicleKind::Compact, Paint::Green}, V3{-100, 2, -0.0625}, 0.1);
    game.spawn_vehicle({VehicleKind::Van, Paint::White}, V3{-60, -4, -0.0625}, 0.0);
    for (int t = 0; t < 60; ++t) game.tick();
    game.enter_vehicle(id);
    for (int t = 0; t < 240; ++t) {
      VehicleInput in;
      in.throttle = t < 150 ? 1.0 : 0.0;
      in.brake = t >= 180 ? 0.7 : 0.0;
      in.steer = (t / 40) % 2 == 0 ? 0.3 : -0.3;
      game.drive(in);
      game.tick();
    }
    return game.session_hash();
  };
  set_num_threads(1);
  Game a;
  CommandLog log;
  const u64 ha = play(a, &log);
  // the log replayed on another thread count
  set_num_threads(4);
  Game b;
  load_road(b);
  replay(b, log, a.ticks(), [](i64) {});
  set_num_threads(0);
  CHECK(b.session_hash() == ha);
  CHECK(log.commands().size() > 5);
  // saved and loaded: the vehicles are there, drivable
  const std::vector<u8> delta = a.save_delta();
  Game c;
  load_road(c);
  REQUIRE(c.load_delta(delta));
  const std::vector<VehicleView> vs = c.vehicles();
  REQUIRE(vs.size() == 2);
  CHECK(vs[0].kind == VehicleKind::Compact);
  CHECK(vs[0].paint == Paint::Green);
  CHECK(vs[1].kind == VehicleKind::Van);
  REQUIRE(c.enter_vehicle(vs[1].id));
  VehicleInput in;
  in.throttle = 1.0;
  c.drive(in);
  for (int t = 0; t < 180; ++t) c.tick();
  VehicleView v;
  REQUIRE(c.vehicle(vs[1].id, &v));
  MESSAGE("the loaded van, driven 3 s: " << v.speed << " m/s");
  CHECK(v.speed > 6.0);
}

TEST_CASE("vehicles: driven into a wall its front crumples - it keeps its id, is meshed again, and shows damage") {
  Game game;
  load_road(game, true);
  const u32 id = game.spawn_vehicle({VehicleKind::Sedan, Paint::Red}, V3{40, 0, -0.0625}, 0.0);
  REQUIRE(id != 0);
  for (int t = 0; t < 60; ++t) game.tick();
  VehicleView v;
  REQUIRE(game.vehicle(id, &v));
  const i64 chassis = v.chassis;
  REQUIRE(chassis != 0);
  // (pushed at 60 km/h towards the wall 30 m ahead)
  game.world().apply_impulse(chassis, game.world().piece(chassis)->x, V3{game.world().piece(chassis)->mass * 16.7, 0, 0});
  (void)game.take_events();
  i32 remeshed = 0;
  f64 peak = 0.0;
  for (int t = 0; t < 180; ++t) {
    game.tick();
    for (const GameEvent& e : game.take_events())
      if (e.kind == GameEvent::Kind::Remesh && e.id == chassis) ++remeshed;
    REQUIRE(game.vehicle(id, &v));
    peak = std::max(peak, v.damage);
  }
  MESSAGE("into the wall at 60 km/h: remeshed " << remeshed << " times, damage " << v.damage << ", " << v.wheels << " wheels on, now "
                                                 << v.speed << " m/s");
  CHECK(v.chassis == chassis);
  CHECK(remeshed > 0);
  CHECK(v.damage > 0.05);
  CHECK(std::abs(v.speed) < 3.0);
}
