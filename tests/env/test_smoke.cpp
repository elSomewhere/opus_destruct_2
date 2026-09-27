// Smoke (svx_env): a sparse density field that rises, is held by ceilings and thins out.
#include <memory>
#include <vector>

#include "doctest.h"
#include "svx/env/env.hpp"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

constexpr f64 kH = 0.125;
const Vox kRock = make_vox(MaterialId::Rock, true);
const Vox kConc = make_vox(MaterialId::Concrete, false);
const Vox kWood = make_vox(MaterialId::Wood, false);

void box(VoxelGrid& g, const IVec3& lo, const IVec3& hi, Vox v) {
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y) g.fill_column(x, y, lo[2], hi[2], v);
}

// A rock plate, and a closed concrete room (6 x 6 x 3 m inside) on it.
VoxelGrid room() {
  VoxelGrid g;
  g.h = kH;
  box(g, {-16, -16, -4}, {80, 80, 0}, kRock);
  box(g, {0, 0, 0}, {52, 52, 28}, kConc);
  box(g, {2, 2, 0}, {50, 50, 26}, kAir);
  g.compact();
  return g;
}

V3 at(const IVec3& p) { return {kH * p[0], kH * p[1], kH * p[2]}; }

struct Setup {
  World w;
  Environment env;
  explicit Setup(VoxelGrid g, const EnvConfig& c = {}) {
    env.attach(w, c);
    w.load(std::move(g));
  }
  void run(f64 s) {
    const int n = static_cast<int>(s / w.config().dt + 0.5);
    for (int t = 0; t < n; ++t) w.tick();
  }
};

}  // namespace

TEST_CASE("smoke: rises in the open and thins out") {
  Setup s(room());
  SmokeSystem& sm = *s.env.smoke();
  sm.emit(at({70, 70, 4}), 4.0);
  s.run(0.2);
  CHECK(sm.density(s.w, at({70, 70, 4})) > 0.5);
  s.run(4.0);
  // it went up: more smoke 2-4 m up than at the source
  f64 low = 0.0, high = 0.0;
  for (const auto& c : sm.cells(s.w, 100000, 0.0f)) (c.pos.z < 1.0 ? low : high) += c.density;
  CHECK(high > low);
  s.run(120.0);
  CHECK(sm.stats().blocks == 0);  // (all gone)
  CHECK(sm.cells(s.w, 10, 0.0f).empty());
}

TEST_CASE("smoke: a fire in a closed room fills it from the ceiling down, and the walls hold it") {
  Setup s(room());
  std::vector<VoxelEdit> fuel;
  for (i32 x = 20; x < 30; ++x)
    for (i32 y = 20; y < 30; ++y) fuel.push_back({{x, y, 0}, kWood});
  s.w.set_voxels(fuel);
  s.env.fire()->ignite(s.w, at({25, 25, 1}), 0.6);
  s.run(15.0);
  REQUIRE(s.env.fire()->stats().burning > 0);
  SmokeSystem& sm = *s.env.smoke();
  const f64 ceiling = sm.density(s.w, at({10, 10, 23})), floor = sm.density(s.w, at({10, 10, 3}));
  CHECK(ceiling > 0.05);
  CHECK(ceiling > 2.0 * floor);
  // none outside (the room is closed)
  CHECK(sm.density(s.w, at({25, 25, 34})) == 0.0);
  CHECK(sm.density(s.w, at({60, 25, 10})) == 0.0);
  // an opening in the roof: it escapes up
  std::vector<VoxelEdit> hole;
  for (i32 x = 22; x < 30; ++x)
    for (i32 y = 22; y < 30; ++y) hole.push_back({{x, y, 26}, kAir}), hole.push_back({{x, y, 27}, kAir});
  s.w.set_voxels(hole);
  s.run(5.0);
  CHECK(sm.density(s.w, at({25, 25, 36})) > 0.01);
}

TEST_CASE("smoke: bounded by its budget, deterministic, gone with evicted chunks") {
  EnvConfig c;
  c.smoke_config.max_blocks = 6;
  Setup a(room(), c), b(room(), c);
  for (Setup* s : {&a, &b})
    for (int k = 0; k < 20; ++k) s->env.smoke()->emit_sphere(at({-10 + 8 * k, 70, 8}), 1.0, 3.0);
  a.run(3.0);
  b.run(3.0);
  CHECK(a.env.smoke()->stats().blocks <= 6);
  CHECK(a.env.smoke()->stats().dropped > 0);
  CHECK(a.env.smoke()->state_hash() == b.env.smoke()->state_hash());
  CHECK(a.w.session_hash() == b.w.session_hash());
  CHECK(a.env.smoke()->memory_bytes() < 6 * 8192 + 64 * 1024);
  CHECK(a.w.memory().systems >= a.env.smoke()->memory_bytes());
}

// ---- hardening (audit regressions)

TEST_CASE("smoke: a burning plank wall smokes (flames in closed cells find an open one); a point emit counts") {
  VoxelGrid g;
  g.h = kH;
  box(g, {-16, -16, -4}, {80, 80, 0}, kRock);
  box(g, {0, 4, 0}, {24, 5, 24}, kWood);  // (1 voxel thick: its cells are closed by the wall rule)
  g.compact();
  Setup s(std::move(g));
  s.env.fire()->ignite(s.w, at({12, 4, 8}), 0.3);
  s.run(6.0);
  REQUIRE(s.env.fire()->stats().burning > 5);
  CHECK(s.env.smoke()->stats().cells > 10);
  // a point emit, anywhere in its cell
  Setup t(room());
  t.env.smoke()->emit(at({70, 70, 5}), 2.0);  // (a voxel off the cell's centre)
  t.run(0.15);
  CHECK(t.env.smoke()->stats().cells > 0);
}

TEST_CASE("smoke: settings are brought into range; emits are bounded") {
  SmokeConfig c;
  c.lifetime = -1.0;
  c.wind = V3{1e9, std::nan(""), 0.0};
  c.max_blocks = -3;
  SmokeSystem sm(c);
  CHECK(sm.config().lifetime >= 0.5);
  CHECK(sm.config().wind.x <= 50.0);
  CHECK(sm.config().wind.y == 0.0);
  CHECK(sm.config().max_blocks == 0);
  EnvConfig e;
  e.smoke_config.max_blocks = 4;
  Setup s(room(), e);
  for (int k = 0; k < 5000; ++k) s.env.smoke()->emit_sphere(at({70, 70, 8}), 100.0, 1.0);  // (capped: radius, count)
  s.env.smoke()->emit({std::nan(""), 0, 0}, 1.0);
  s.run(0.15);
  CHECK(s.env.smoke()->stats().blocks <= 4);
  CHECK(s.env.smoke()->density(s.w, {1e300, 0, 0}) == 0.0);
}
