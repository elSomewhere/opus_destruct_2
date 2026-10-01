// Entities a streamed source places (GameSource::spawns_in): the game makes their vehicles as it
// makes parked cars - near the viewer, out of sight - and they go when out of range untouched, and
// come back where they were.
#include <cmath>
#include <memory>
#include <vector>

#include "doctest.h"
#include "svx/game/game.hpp"

using namespace svx;

namespace {

constexpr f64 kH = 0.125;

IVec3 voxel_of(const IVec3& cc, i32 i) {
  return {cc[0] * kChunk + (i >> (2 * kChunkBits)), cc[1] * kChunk + ((i >> kChunkBits) & (kChunk - 1)), cc[2] * kChunk + (i & (kChunk - 1))};
}

// Flat anchored rock below z 0 over 320 m square, a fire engine and an ambulance at their stations.
class StationSource final : public GameSource {
 public:
  IVec3 chunk_lo() const override { return {-40, -40, -1}; }
  IVec3 chunk_hi() const override { return {40, 40, 1}; }
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    out.assign(kChunkVox, kAir);
    if (cc[2] >= 0) return false;
    for (i32 i = 0; i < kChunkVox; ++i)
      if (voxel_of(cc, i)[2] >= -8) out[size_t(i)] = make_vox(MaterialId::Rock, true);
    return true;
  }
  V3 spawn_pos() const override { return {0.0, 0.0, 0.0}; }
  V3 spawn_dir() const override { return {1, 0, 0}; }
  void spawns_in(const V3& lo, const V3& hi, std::vector<SpawnRecord>& out) const override {
    for (const SpawnRecord& r : records)
      if (r.pos.x >= lo.x && r.pos.x <= hi.x && r.pos.y >= lo.y && r.pos.y <= hi.y) out.push_back(r);
  }
  std::vector<SpawnRecord> records{{11, "fire_truck", V3{60.0, 0.0, 0.0}, 0.0, "station"},
                                   {12, "ambulance", V3{0.0, 70.0, 0.0}, 1.5707963267948966, "hospital"}};
};

StreamConfig stream() {
  StreamConfig sc;
  sc.chunks_per_tick = 64;
  return sc;
}

std::vector<VehicleView> run(Game& g, f64 seconds) {
  for (int t = 0; t < static_cast<int>(seconds * 60.0); ++t) {
    g.tick();
    (void)g.take_events();
    (void)g.take_meshes({});
  }
  return g.vehicles();
}

}  // namespace

TEST_CASE("spawns: a source's entities come as vehicles of their kind where they stand, and come back") {
  Game g;
  g.load_streaming(std::make_shared<StationSource>(), kH, stream());
  g.set_viewer({0.0, 0.0, 1.6});
  std::vector<VehicleView> vs = run(g, 6.0);
  REQUIRE(vs.size() == 2);
  for (const VehicleView& v : vs) {
    CHECK((v.flags & WheelTag::kTagParked) != 0);
    if (v.kind == VehicleKind::Truck) {
      CHECK(v.paint == Paint::Red);
      CHECK(std::abs(v.pos.x - 60.0) < 1.5);
    } else {
      CHECK(v.kind == VehicleKind::Van);
      CHECK(v.paint == Paint::White);
      CHECK(std::abs(v.pos.y - 70.0) < 1.5);
    }
  }
  // far away: gone; back: there again, where they were
  g.set_viewer({0.0, -150.0, 1.6});
  CHECK(run(g, 3.0).empty());
  g.set_viewer({0.0, 0.0, 1.6});
  vs = run(g, 6.0);
  REQUIRE(vs.size() == 2);
  for (const VehicleView& v : vs)
    CHECK((v.kind == VehicleKind::Truck ? std::abs(v.pos.x - 60.0) : std::abs(v.pos.y - 70.0)) < 1.5);
}

TEST_CASE("spawns: none with the traffic off") {
  Game g;
  g.load_streaming(std::make_shared<StationSource>(), kH, stream());
  TrafficConfig t = g.traffic();
  t.enabled = false;
  g.set_traffic(t);
  g.set_viewer({0.0, 0.0, 1.6});
  CHECK(run(g, 4.0).empty());
}

namespace {

// Rock below z 0, and below a lake's level (z -1: water from -8 to -1) east of x 0 (the far tier's
// coarse view of it).
class LakeSource final : public GameSource {
 public:
  IVec3 chunk_lo() const override { return {-512, -512, -1}; }
  IVec3 chunk_hi() const override { return {512, 512, 1}; }
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    out.assign(kChunkVox, kAir);
    if (cc[2] >= 0) return false;
    for (i32 i = 0; i < kChunkVox; ++i) {
      const IVec3 p = voxel_of(cc, i);
      if (p[2] < (p[0] >= 0 ? -8 : 0)) out[size_t(i)] = make_vox(MaterialId::Rock, true);
    }
    return true;
  }
  V3 spawn_pos() const override { return {0.0, 0.0, 0.0}; }
  V3 spawn_dir() const override { return {1, 0, 0}; }
  bool coarse(const IVec3& lo, const IVec3& n, i32 factor, std::vector<Vox>& out) const override {
    out.assign(size_t(n[0]) * size_t(n[1]) * size_t(n[2]), kAir);
    for (i32 x = 0; x < n[0]; ++x)
      for (i32 y = 0; y < n[1]; ++y)
        for (i32 z = 0; z < n[2]; ++z) {
          const i32 wx = lo[0] + x * factor, wz = lo[2] + z * factor;
          if (wz < (wx >= 0 ? -8 : 0)) out[(size_t(x) * size_t(n[1]) + size_t(y)) * size_t(n[2]) + size_t(z)] = make_vox(MaterialId::Rock, true);
        }
    return true;
  }
  bool coarse_water(const IVec3& lo, const IVec3& n, i32 factor, std::vector<i32>& out) const override {
    out.assign(size_t(n[0]) * size_t(n[1]), kNoWater);
    bool any = false;
    for (i32 x = 0; x < n[0]; ++x)
      if (lo[0] + x * factor >= 0) {
        for (i32 y = 0; y < n[1]; ++y) out[size_t(x) * size_t(n[1]) + size_t(y)] = -1;
        any = true;
      }
    return any;
  }
};

}  // namespace

TEST_CASE("far tier: a source's open water is a flat surface at its level in the far tiles") {
  Game g;
  FarConfig far;
  far.radius = 400.0;
  far.tiles_per_tick = 64;
  g.load_streaming(std::make_shared<LakeSource>(), kH, StreamConfig{}, far);
  g.set_viewer({0.0, 0.0, 1.6});
  i32 water = 0, land = 0, tiles = 0;
  bool level = true, west = false;
  for (int t = 0; t < 30; ++t) {
    g.tick();
    for (const ChunkMesh& m : g.take_far_meshes()) {
      ++tiles;
      for (const MeshVertex& v : m.vertices) {
        if (v.texture == kFarWaterTexture) {
          ++water;
          level = level && std::abs(v.pos[2] - static_cast<f32>(kH * -0.5)) < 1e-4f && v.normal[2] == 127;
          west = west || v.pos[0] < -kH;
        } else {
          ++land;
        }
      }
    }
    (void)g.take_meshes({});
  }
  MESSAGE(tiles << " far tiles: " << water << " water vertices, " << land << " others");
  CHECK(tiles > 0);
  CHECK(water > 0);
  CHECK(level);
  CHECK_FALSE(west);  // (none over the land west of x 0)
}
