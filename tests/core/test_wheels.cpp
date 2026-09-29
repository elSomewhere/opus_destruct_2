// Wheels (docs/VEHICLES.md): a chassis on four cast wheels - it settles on its springs, drives,
// brakes, steers, loses its wheels in a hard landing, and replays bit for bit.
#include <algorithm>
#include <cmath>
#include <vector>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

constexpr f64 h = 0.125;
const Quat kId{0, 0, 0, 1};

void box(VoxelGrid& g, const IVec3& lo, const IVec3& hi, Vox v) {
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y) g.fill_column(x, y, lo[2], hi[2], v);
}

VoxelGrid ground(i32 half = 480) {
  VoxelGrid g;
  g.h = h;
  box(g, {-half, -half, -4}, {half, half, 0}, make_vox(MaterialId::Asphalt, true));
  g.lo = {-half, -half, -4};
  g.hi = {half, half, 64};
  g.compact();
  return g;
}

struct TestCar {
  GridId grid = 0;
  std::vector<WheelId> wheels;  // front left, front right, rear left, rear right
};

// A test chassis: a frame slab 4 m x 1.625 m x 0.25 m (car_frame, about 1.1 t) at `at` (its
// bottom's centre), on four wheels at its corners.
TestCar car(World& w, const V3& at, f64 break_force = 0.0, f64 yaw = 0.0) {
  VoxelGrid g;
  g.h = h;
  box(g, {-16, -6, 0}, {16, 7, 2}, make_vox(MaterialId::CarFrame, false));
  g.compact();
  const Quat q{0.0, 0.0, std::sin(0.5 * yaw), std::cos(0.5 * yaw)};
  TestCar c;
  c.grid = w.add_grid(GridFrame{at + V3{0, 0, 0.5 * h}, q}, std::move(g), false);
  REQUIRE(c.grid != 0);
  const f64 cy = std::cos(yaw), sy = std::sin(yaw);
  for (int k = 0; k < 4; ++k) {
    const f64 lx = k < 2 ? 1.5 : -1.5, ly = (k % 2 == 0) ? 0.75 : -0.75;
    WheelDesc d;
    d.mount.kind = JointAnchor::Kind::Grid;
    d.mount.id = c.grid;
    d.mount.point = at + V3{lx * cy - ly * sy, lx * sy + ly * cy, 0.0};
    d.down = V3{0, 0, -1};
    d.axle = V3{-sy, cy, 0};
    d.break_force = break_force;
    d.group = 1;
    d.tag = static_cast<u32>(k);
    const WheelId id = w.add_wheel(d);
    REQUIRE(id != 0);
    c.wheels.push_back(id);
  }
  return c;
}

i64 chassis(const World& w, const TestCar& c) {
  WheelState s;
  for (WheelId id : c.wheels)
    if (w.wheel(id, &s) && s.piece != 0) return s.piece;
  return 0;
}

void drive(World& w, const TestCar& c, f64 rear_torque, f64 brake, f64 steer) {
  for (size_t k = 0; k < c.wheels.size(); ++k)
    w.set_wheel_input(c.wheels[k], k >= 2 ? rear_torque : 0.0, brake, k < 2 ? steer : 0.0);
}

}  // namespace

TEST_CASE("wheels: a chassis settles on its springs at its static sag, and sleeps") {
  World w;
  w.load(ground());
  w.bake();
  const TestCar c = car(w, V3{0, 0, 0.75});
  for (int t = 0; t < 240; ++t) w.tick();
  const i64 id = chassis(w, c);
  REQUIRE(id != 0);
  const Body* b = w.piece(id);
  REQUIRE(b);
  f64 load = 0.0, comp = 0.0;
  for (WheelId wid : c.wheels) {
    WheelState s;
    REQUIRE(w.wheel(wid, &s));
    CHECK(s.contact);
    load += s.load;
    comp += s.length;
  }
  comp /= 4.0;
  const f64 weight = b->mass * 9.81;
  const f64 sag = weight / 4.0 / 35e3;
  MESSAGE("chassis " << b->mass << " kg: wheels carry " << load << " N (weight " << weight << "), mean length " << comp << " m (sag " << sag
                     << " -> " << 0.35 - sag << "), speed " << norm(b->v) << ", asleep " << b->asleep);
  CHECK(load == doctest::Approx(weight).epsilon(0.05));
  CHECK(comp == doctest::Approx(0.35 - sag).epsilon(0.1));
  CHECK(norm(b->v) < 0.05);
  // (on its wheels: the bottom of the chassis 0.6 m over the road)
  CHECK(b->x.z > 0.5);
  for (int t = 0; t < 240; ++t) w.tick();
  CHECK(w.piece(id)->asleep);
}

TEST_CASE("wheels: driven, a car accelerates; braked, it stops; steered, it turns") {
  World w;
  w.load(ground());
  w.bake();
  const TestCar c = car(w, V3{-40, 0, 0.7});
  for (int t = 0; t < 60; ++t) w.tick();
  const i64 id = chassis(w, c);
  REQUIRE(id != 0);
  // 2 x 700 N m on the rear wheels: about 4.2 kN on 1.1 t
  drive(w, c, 700.0, 0.0, 0.0);
  for (int t = 0; t < 180; ++t) w.tick();
  const Body* b = w.piece(id);
  REQUIRE(b);
  const f64 v3 = b->v.x;
  WheelState rl;
  REQUIRE(w.wheel(c.wheels[2], &rl));
  MESSAGE("after 3 s of throttle: " << v3 << " m/s (lateral " << b->v.y << "), rear wheel spin " << rl.spin << " rad/s (rolls at "
                                    << b->v.x / 0.33 << ")");
  CHECK(v3 > 8.0);
  CHECK(std::abs(b->v.y) < 0.2);
  CHECK(rl.spin * 0.33 == doctest::Approx(v3).epsilon(0.1));  // (rolling, a little slip)
  // steer left at speed: it turns left (yaw rate up)
  drive(w, c, 150.0, 0.0, 0.25);
  f64 yaw_rate = 0.0;
  for (int t = 0; t < 60; ++t) {
    w.tick();
    yaw_rate = w.piece(id)->w.z;
  }
  MESSAGE("steered 0.25 rad at " << norm(w.piece(id)->v) << " m/s: yaw rate " << yaw_rate << " rad/s");
  CHECK(yaw_rate > 0.3);
  // brake: it stops
  drive(w, c, 0.0, 2500.0, 0.0);
  for (int t = 0; t < 240; ++t) w.tick();
  MESSAGE("braked 4 s: " << norm(w.piece(id)->v) << " m/s");
  CHECK(norm(w.piece(id)->v) < 0.3);
}

TEST_CASE("wheels: a hard landing tears off wheels that are weaker than it, and they roll on as pieces") {
  World w;
  w.load(ground());
  w.bake();
  const TestCar c = car(w, V3{0, 0, 6.0}, 6000.0);
  i32 off = 0;
  i64 made = 0;
  for (int t = 0; t < 180; ++t) {
    w.tick();
    for (const WorldEvent& e : w.take_events())
      if (e.kind == WorldEvent::Kind::WheelDetached) {
        ++off;
        if (e.voxels > 0) made = e.voxels;
      }
  }
  MESSAGE("dropped from 6 m: " << off << " wheels came off (a wheel piece: " << made << "), " << w.wheels().size() << " left");
  CHECK(off >= 2);
  CHECK(made != 0);
  CHECK(w.piece(made) != nullptr);
}

TEST_CASE("wheels: a driven car is bit-identical on any thread count") {
  auto run = [](int threads) {
    set_num_threads(threads);
    World w;
    w.load(ground());
    w.bake();
    const TestCar a = car(w, V3{-20, -3, 0.7}), b = car(w, V3{-20, 3, 0.7}, 0.0, 0.3);
    for (int t = 0; t < 300; ++t) {
      drive(w, a, t < 150 ? 600.0 : 0.0, t < 150 ? 0.0 : 1500.0, t > 60 && t < 120 ? 0.2 : 0.0);
      drive(w, b, 400.0, 0.0, -0.15);
      w.tick();
    }
    return w.session_hash();
  };
  const u64 h1 = run(1), h4 = run(4);
  set_num_threads(0);
  CHECK(h1 == h4);
}

namespace {

// A flat streamed world of road, 64 x 64 chunks.
class Road final : public ChunkSource {
 public:
  bool generate(const IVec3& c, std::vector<Vox>& out) const override {
    out.assign(kChunkVox, kAir);
    if (c[2] != -1) return false;
    std::fill(out.begin(), out.end(), make_vox(MaterialId::Asphalt, true));
    return true;
  }
  IVec3 chunk_lo() const override { return {-32, -32, -1}; }
  IVec3 chunk_hi() const override { return {32, 32, 2}; }
};

}  // namespace

TEST_CASE("wheels: a saved session brings its car back on its wheels, driving on") {
  World w;
  w.load(ground());
  w.bake();
  const TestCar c = car(w, V3{-40, 0, 0.7});
  for (int t = 0; t < 60; ++t) w.tick();
  drive(w, c, 600.0, 0.0, 0.1);
  for (int t = 0; t < 90; ++t) w.tick();
  const std::vector<u8> delta = w.save_delta();
  // (the original goes on for a second)
  for (int t = 0; t < 60; ++t) w.tick();
  const Body* ba = w.piece(chassis(w, c));
  REQUIRE(ba);
  World b;
  b.load(ground());
  b.bake();
  REQUIRE(b.load_delta(delta));
  CHECK(b.wheels() == w.wheels());
  WheelState s;
  REQUIRE(b.wheel(c.wheels[3], &s));
  CHECK(s.group == 1);
  CHECK(s.tag == 3);
  CHECK(s.drive == 600.0);
  CHECK(s.steer == 0.0);
  for (int t = 0; t < 60; ++t) b.tick();
  const Body* bb = b.piece(chassis(b, c));
  REQUIRE(bb);
  MESSAGE("a second after the save: original at " << ba->x.x << ", " << ba->x.y << " (" << norm(ba->v) << " m/s); loaded at " << bb->x.x << ", "
                                                  << bb->x.y << " (" << norm(bb->v) << " m/s)");
  CHECK(norm(bb->x - ba->x) < 0.1);
  CHECK(norm(bb->v - ba->v) < 0.1);
}

TEST_CASE("wheels: a parked car goes out of range with its wheels, and comes back on them") {
  World w;
  VoxelGrid g;
  g.h = h;
  w.load(std::move(g));
  StreamConfig sc;
  sc.load_radius = 24.0;
  sc.evict_radius = 32.0;
  sc.chunks_per_tick = 400;
  w.enable_streaming(std::make_shared<Road>(), sc);
  w.set_focus(V3{0, 0, 0});
  for (int t = 0; t < 5; ++t) w.tick();
  const TestCar c = car(w, V3{2, 2, 0.7});
  for (int t = 0; t < 400; ++t) w.tick();  // (it settles and sleeps)
  const i64 id = chassis(w, c);
  REQUIRE(id != 0);
  REQUIRE(w.piece(id)->asleep);
  w.set_focus(V3{120, 0, 0});
  for (int t = 0; t < 30; ++t) w.tick();
  CHECK(w.piece(id) == nullptr);
  CHECK(w.wheels().empty());
  CHECK(w.stats().archived_pieces >= 1);
  w.set_focus(V3{0, 0, 0});
  for (int t = 0; t < 30; ++t) w.tick();
  REQUIRE(w.piece(id) != nullptr);
  CHECK(w.wheels().size() == 4);
  WheelState s;
  REQUIRE(w.wheel(c.wheels[0], &s));
  CHECK(s.piece == id);
  // (and it drives off)
  drive(w, c, 600.0, 0.0, 0.0);
  for (int t = 0; t < 120; ++t) w.tick();
  MESSAGE("back from the archive: drives at " << norm(w.piece(id)->v) << " m/s");
  CHECK(norm(w.piece(id)->v) > 4.0);
}
