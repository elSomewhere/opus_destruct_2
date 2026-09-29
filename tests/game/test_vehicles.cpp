// Vehicles (docs/VEHICLES.md): their models, and vehicles in a game - driven by a player,
// crashed, driven by the traffic of a streamed city, saved and replayed.
#include <algorithm>
#include <array>
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

TEST_CASE("vehicles: a model's parts - doors, bonnet, boot, bumpers, cargo - are grids of their own beside its body, each held at its seam") {
  for (int k = 0; k < static_cast<int>(VehicleKind::Count); ++k) {
    const VehicleModel m = build_vehicle({static_cast<VehicleKind>(k), Paint::Red});
    std::array<int, size_t(PartKind::Count)> kinds{};
    VoxelGrid whole = m.voxels;  // (the body and its parts: the vehicle)
    for (const VehiclePart& P : m.parts) {
      ++kinds[size_t(P.kind)];
      const Census c = census(P.voxels);
      CHECK(c.voxels >= 12);
      CHECK(c.connected == c.voxels);  // (one piece)
      CHECK(P.voxels.h == kVehicleVoxel);
      CHECK(P.voxels.layer_index("paint") >= 0);
      // its voxels are its own (not the body's, nor another part's), and it meets the body where it
      // is held: a voxel of each within a voxel of its hinge (or its fixed joint)
      bool shared = false;
      for (const auto& [key, ch] : P.voxels.chunks()) {
        const IVec3 cc = unkey3(key);
        for (int i = 0; i < kChunkVox; ++i) {
          const Vox v = ch.uniform ? ch.value : ch.v[size_t(i)];
          if (!vox_solid(v)) continue;
          const IVec3 p{cc[0] * kChunk + i / (kChunk * kChunk), cc[1] * kChunk + (i / kChunk) % kChunk, cc[2] * kChunk + i % kChunk};
          shared = shared || vox_solid(whole.get(p));  // (the body's, or another part's)
          whole.set(p[0], p[1], p[2], v);
        }
      }
      CHECK_FALSE(shared);
      const IVec3 c0{static_cast<i32>(std::floor(P.hinge.x / kVehicleVoxel + 0.5)), static_cast<i32>(std::floor(P.hinge.y / kVehicleVoxel + 0.5)),
                     static_cast<i32>(std::floor(P.hinge.z / kVehicleVoxel + 0.5))};
      bool body = false, part = false;
      for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
          for (int dz = -1; dz <= 1; ++dz) {
            const IVec3 q{c0[0] + dx, c0[1] + dy, c0[2] + dz};
            body = body || vox_solid(m.voxels.get(q));
            part = part || vox_solid(P.voxels.get(q));
          }
      CHECK(body);
      CHECK(part);
      if (P.hinged) {
        // (it opens one way from shut, and a knock opens its latch before its hinge gives way)
        CHECK(P.lower <= 0.0);
        CHECK(P.upper >= 0.0);
        CHECK(P.upper - P.lower > 0.5);
        CHECK(P.latch > 0.0);
        CHECK(P.latch < P.break_torque);
      }
      CHECK(P.break_force > 0.0);
    }
    const Census all = census(whole);
    MESSAGE("kind " << k << ": " << m.parts.size() << " parts (" << kinds[size_t(PartKind::Door)] << " doors), " << all.voxels - census(m.voxels).voxels
                    << " of its " << all.voxels << " voxels");
    CHECK(all.connected == all.voxels);  // (the body and its parts are the whole vehicle)
    CHECK(kinds[size_t(PartKind::Door)] >= 2);
    CHECK(kinds[size_t(PartKind::Bumper)] >= 1);
    if (m.spec.kind == VehicleKind::Sedan) {
      CHECK(kinds[size_t(PartKind::Door)] == 4);
      CHECK(kinds[size_t(PartKind::Bonnet)] == 1);
      CHECK(kinds[size_t(PartKind::Boot)] == 1);
      CHECK(kinds[size_t(PartKind::Bumper)] == 2);
    }
    if (m.spec.kind == VehicleKind::Pickup) {
      CHECK(kinds[size_t(PartKind::Tailgate)] == 1);
      CHECK(kinds[size_t(PartKind::Cargo)] == 2);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Vehicles in a game

#include "svx/base/parallel.hpp"
#include "svx/game/game.hpp"
#include "svx/game/procgen.hpp"
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
  // (their parts on their latched hinges and mounts, as they were)
  for (const VehicleView& x : vs) {
    CHECK(x.parts0 > 0);
    CHECK(x.parts == x.parts0);
  }
  i32 latched = 0;
  for (JointId j : c.world().joints()) {
    JointState s;
    if (c.world().joint(j, &s) && s.latched) ++latched;
  }
  CHECK(latched > 0);
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

TEST_CASE("vehicles: a car's mesh is its size (its voxels are its grid's, not the world's)") {
  Game game;
  load_road(game);
  const u32 id = game.spawn_vehicle({VehicleKind::Sedan, Paint::Blue}, V3{40, 0, -0.0625}, 0.0);
  REQUIRE(id != 0);
  f64 lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9};
  i64 chassis = 0;
  for (int t = 0; t < 30 && chassis == 0; ++t) {
    game.tick();
    VehicleView v;
    if (game.vehicle(id, &v)) chassis = v.chassis;
    for (const GameEvent& e : game.take_events())
      if (e.kind == GameEvent::Kind::Detached && chassis != 0 && e.id == chassis)
        for (const MeshVertex& mv : e.mesh.vertices)
          for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], f64(mv.pos[a]));
            hi[a] = std::max(hi[a], f64(mv.pos[a]));
          }
  }
  VehicleView v;
  REQUIRE(game.vehicle(id, &v));
  REQUIRE(hi[0] > lo[0]);
  MESSAGE("its mesh: " << hi[0] - lo[0] << " x " << hi[1] - lo[1] << " x " << hi[2] - lo[2] << " m; its box " << 2 * v.half_extent.x << " x "
                       << 2 * v.half_extent.y << " x " << v.half_extent.z << " m");
  CHECK(std::abs((hi[0] - lo[0]) - 2 * v.half_extent.x) < 0.3);
  CHECK(std::abs((hi[1] - lo[1]) - 2 * v.half_extent.y) < 0.3);
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
  // (pushed at 60 km/h towards the wall 30 m ahead: its chassis and its parts)
  std::vector<i64> all = game.world().joined_pieces(chassis);
  all.push_back(chassis);
  for (i64 p : all) game.world().apply_impulse(p, game.world().piece(p)->x, V3{game.world().piece(p)->mass * 16.7, 0, 0});
  (void)game.take_events();
  i32 remeshed = 0, misplaced = 0;
  f64 peak = 0.0;
  for (int t = 0; t < 180; ++t) {
    game.tick();
    for (const GameEvent& e : game.take_events())
      if (e.kind == GameEvent::Kind::Remesh && e.id == chassis) {
        ++remeshed;
        // (its new mesh where the car is: within its box of its centre)
        V3 lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
        for (const MeshVertex& mv : e.mesh.vertices) {
          lo = V3{std::min(lo.x, f64(mv.pos[0])), std::min(lo.y, f64(mv.pos[1])), std::min(lo.z, f64(mv.pos[2]))};
          hi = V3{std::max(hi.x, f64(mv.pos[0])), std::max(hi.y, f64(mv.pos[1])), std::max(hi.z, f64(mv.pos[2]))};
        }
        const V3 mid = (lo + hi) * 0.5;
        if (e.mesh.vertices.empty() || norm(mid - e.pos) > 2.0 || hi.x - lo.x > 6.0) {
          ++misplaced;
          MESSAGE("remesh at " << e.pos.x << " " << e.pos.y << " " << e.pos.z << ": mesh " << e.mesh.vertices.size() << " vertices, box " << lo.x << ".." << hi.x
                               << " x " << lo.y << ".." << hi.y << " x " << lo.z << ".." << hi.z);
        }
      }
    REQUIRE(game.vehicle(id, &v));
    peak = std::max(peak, v.damage);
  }
  MESSAGE("into the wall at 60 km/h: remeshed " << remeshed << " times, damage " << v.damage << ", " << v.wheels << " wheels on, now "
                                                 << v.speed << " m/s");
  CHECK(v.chassis == chassis);
  CHECK(remeshed > 0);
  CHECK(misplaced == 0);
  CHECK(v.damage > 0.05);
  CHECK(std::abs(v.speed) < 3.0);
  // (its front bumper, first to meet the wall, came off it)
  MESSAGE(v.parts << " of its " << v.parts0 << " parts still on");
  CHECK(v.parts < v.parts0);
}

TEST_CASE("vehicles: its parts hold on while it is driven hard; a side impact tears the struck door off, the car keeps its id") {
  Game game;
  load_road(game);
  // a pickup - its cargo strapped in its bed - driven hard: floored, a handbrake turn at full lock,
  // an emergency stop
  const u32 id = game.spawn_vehicle({VehicleKind::Pickup, Paint::Red}, V3{-100, 0, -0.0625}, 0.0);
  REQUIRE(id != 0);
  for (int t = 0; t < 60; ++t) game.tick();
  VehicleView v;
  REQUIRE(game.vehicle(id, &v));
  CHECK(v.parts0 == static_cast<i32>(vehicle_model(VehicleKind::Pickup).parts.size()));
  CHECK(v.parts == v.parts0);
  CHECK(v.damage == 0.0);
  REQUIRE(game.enter_vehicle(id));
  f64 top = 0.0;
  for (int t = 0; t < 420; ++t) {
    VehicleInput in;
    if (t < 240) in.throttle = 1.0;
    else if (t < 300) in = VehicleInput{0.0, 0.0, 1.0, true};
    else in.brake = 1.0;
    game.drive(in);
    game.tick();
    REQUIRE(game.vehicle(id, &v));
    top = std::max(top, std::abs(v.speed));
  }
  MESSAGE("the pickup driven hard (" << top << " m/s at most): " << v.parts << " of its " << v.parts0 << " parts on, damage " << v.damage);
  CHECK(top > 15.0);
  CHECK(v.parts == v.parts0);
  CHECK(v.damage < 0.02);
  game.exit_vehicle();

  // a van at 40 km/h into the side of a parked saloon
  const u32 car = game.spawn_vehicle({VehicleKind::Sedan, Paint::Blue}, V3{60, 0, -0.0625}, 0.0);
  const u32 van = game.spawn_vehicle({VehicleKind::Van, Paint::White}, V3{60.3, -8, -0.0625}, 1.5707963);
  REQUIRE(car != 0);
  REQUIRE(van != 0);
  for (int t = 0; t < 60; ++t) game.tick();
  VehicleView a, b;
  REQUIRE(game.vehicle(car, &a));
  REQUIRE(game.vehicle(van, &b));
  const i64 chassis = a.chassis;
  REQUIRE(a.parts == a.parts0);
  const std::vector<i64> on = game.world().joined_pieces(chassis);
  REQUIRE(static_cast<i32>(on.size()) == a.parts0);
  std::vector<i64> pushed = game.world().joined_pieces(b.chassis);
  pushed.push_back(b.chassis);
  for (i64 p : pushed) game.world().apply_impulse(p, game.world().piece(p)->x, V3{0, game.world().piece(p)->mass * 11.1, 0});
  for (int t = 0; t < 180; ++t) game.tick();
  REQUIRE(game.vehicle(car, &a));
  const std::vector<i64> still = game.world().joined_pieces(chassis);
  i32 off = 0, lying = 0, popped = 0;
  for (i64 p : on)
    if (!std::binary_search(still.begin(), still.end(), p)) {
      ++off;
      if (const Body* pb = game.world().piece(p); pb && pb->x.y < a.pos.y) ++lying;  // (on the struck side, between the van and the car)
    }
  for (JointId j : game.world().joints()) {
    JointState s;
    if (game.world().joint(j, &s) && s.piece_a == chassis && s.type == JointType::Hinge && !s.latched) ++popped;
  }
  MESSAGE("struck at 40 km/h: " << off << " parts off (" << lying << " on the struck side), " << popped << " latches open, damage " << a.damage);
  CHECK(a.chassis == chassis);
  CHECK(a.parts == a.parts0 - off);
  CHECK(off >= 1);
  CHECK(lying >= 1);
  CHECK(a.damage > 0.1);

  // removed, a vehicle takes the parts still on it along (those come off stay: rubble)
  const size_t bodies = game.world().rigid().bodies.size();
  REQUIRE(game.remove_vehicle(car));
  CHECK(game.world().rigid().bodies.size() == bodies - 1 - still.size());
  for (i64 p : still) CHECK(game.world().piece(p) == nullptr);
}

TEST_CASE("vehicles: at 100 km/h a car does not pass through a thin loose slab (look-ahead contacts between pieces)") {
  Game game;
  load_road(game);
  // a concrete slab 12.5 cm thick, 4 m wide and 2 m high, loose, standing across the road 20 m on
  VoxelGrid s;
  s.h = 0.125;
  fill(s, {0, -16, 0}, {1, 16, 16}, make_vox(MaterialId::Rc, false));
  s.compact();
  GridDesc d;
  d.frame = GridFrame{V3{20.0, 0.0, 0.01}, Quat{0, 0, 0, 1}};
  d.voxel_size = 0.125;
  d.base = false;
  const GridId sg = game.world().add_grid(d, std::move(s));
  REQUIRE(game.world().loosen_grid(sg) != 0);
  const u32 id = game.spawn_vehicle({VehicleKind::Sedan, Paint::Red}, V3{0, 0, -0.0625}, 0.0);
  for (int t = 0; t < 60; ++t) game.tick();
  VehicleView v;
  REQUIRE(game.vehicle(id, &v));
  // (at 30 m/s it moves 12.5 cm - the slab's thickness, two of its own voxels - in a substep)
  std::vector<i64> all = game.world().joined_pieces(v.chassis);
  all.push_back(v.chassis);
  for (i64 p : all) game.world().apply_impulse(p, game.world().piece(p)->x, V3{game.world().piece(p)->mass * 30.0, 0, 0});
  f64 slowest = 1e9;
  for (int t = 0; t < 60; ++t) {
    game.tick();
    REQUIRE(game.vehicle(id, &v));
    if (v.pos.x > 14.0) slowest = std::min(slowest, v.speed);
  }
  // what is left of the slab (its grid's pieces): pushed ahead, or thrown up over the bonnet -
  // none of it behind the car's middle, where it would be had the car passed through
  f64 back = 1e9, ahead = -1e9;
  i32 pieces = 0;
  for (const PieceState& p : game.world().pieces()) {
    const Body* b = game.world().piece(p.id);
    if (!b || b->shapes.empty() || b->shapes.front().grid != sg) continue;
    ++pieces;
    back = std::min(back, p.pos.x);
    ahead = std::max(ahead, p.pos.x);
  }
  MESSAGE("into the slab at 108 km/h: slowed to " << slowest << " m/s, damage " << v.damage << ", " << v.parts << " of " << v.parts0
                                                   << " parts on; the slab in " << pieces << " pieces from x " << back << " to " << ahead
                                                   << " (the car's middle at " << v.pos.x << ")");
  CHECK(slowest < 20.0);
  CHECK(v.damage > 0.2);
  REQUIRE(pieces > 0);
  CHECK(back > v.pos.x);
  CHECK(ahead > 20.2);  // (pushed on)
}

TEST_CASE("vehicles: a car driven over a bridge loads its deck - solved again as it goes, a few times a second, not at every fragment") {
  ProcWorld w = make_procedural("bridge", 1);
  Game game;
  game.load(std::move(w.grid), w.spawn_pos, w.spawn_dir);
  add_grids(game.world(), std::move(w.grids));
  game.bake();
  // on the deck (x 1..21 m, y 2.5..5.5 m, its top 6.5 m up), driven along it
  const u32 id = game.spawn_vehicle({VehicleKind::Sedan, Paint::Red}, V3{3.5, 4.0, 6.5 - 0.0625}, 0.0);
  for (int t = 0; t < 60; ++t) game.tick();
  REQUIRE(game.enter_vehicle(id));
  const WorldStats s0 = game.world().stats();
  VehicleView v;
  int ticks = 0;
  for (; ticks < 600; ++ticks) {
    REQUIRE(game.vehicle(id, &v));
    if (v.pos.x > 11.0) break;
    game.drive({0.6, 0.0, 0.0, false});
    game.tick();
  }
  game.drive({0.0, 1.0, 0.0, false});
  for (int t = 0; t < 90; ++t) game.tick();
  ticks += 90;
  const WorldStats s1 = game.world().stats();
  REQUIRE(game.vehicle(id, &v));
  const i64 solves = s1.solves - s0.solves;
  MESSAGE("over the bridge in " << ticks << " ticks: " << solves << " solves of its structure, " << s1.impacts - s0.impacts << " impacts, "
                                << s1.bonds_broken - s0.bonds_broken << " bonds broken; the car at " << v.pos.x << " m, " << v.pos.z << " m up");
  CHECK(v.pos.z > 6.5);  // (still on the deck)
  CHECK(solves >= 5);    // (its load is followed ...)
  CHECK(solves * 4 <= ticks);  // (... at most every few ticks)
  CHECK(s1.bonds_broken == s0.bonds_broken);
}
