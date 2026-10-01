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
