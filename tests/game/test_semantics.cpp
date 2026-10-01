// What a generated world means (svx/game/semantics.hpp): a source's semantics reach the game and
// the C ABI.
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "svx/game/api/svx_api.h"
#include "svx/game/game.hpp"

using namespace svx;

namespace {

class OneHouse final : public WorldSemantics {
 public:
  void buildings_in(const V3& lo, const V3& hi, std::vector<BuildingInfo>& out) const override {
    if (lo.x > 10.0 || hi.x < 0.0 || lo.y > 8.0 || hi.y < 0.0) return;
    BuildingInfo b;
    b.id = "C0_0/b0/l1/B";
    b.program = "residential";
    b.footprint = {{0, 0, 0}, {10, 0, 0}, {10, 8, 0}, {0, 8, 0}};
    b.floors = {0.0, 3.0, 6.0};
    b.entrances = {{V3{5, 0, 0}, V3{0, -1, 0}, "C0_0/r3"}};
    out.push_back(b);
  }
  void furniture_in(const V3& lo, const V3& hi, std::vector<FurnitureInfo>& out) const override {
    (void)lo, (void)hi;
    FurnitureInfo f;
    f.id = (u64{1} << 51) + 7;
    f.prefab = "sofa";
    f.pos = V3{2.5, 3.0, 0.0};
    f.yaw = 1.5;
    f.building = "C0_0/b0/l1/B";
    f.uses = {{Affordance::Sit, V3{2.5, 2.4, 0.0}, -1.5}, {Affordance::Sleep, V3{2.5, 2.4, 0.0}, -1.5}};
    out.push_back(f);
  }
  ZoneInfo zone_at(const V3& p) const override { return p.x < 100.0 ? ZoneInfo{"suburb", "T0", "nordic"} : ZoneInfo{}; }
};

class SemanticSource final : public GameSource {
 public:
  IVec3 chunk_lo() const override { return {-1, -1, -1}; }
  IVec3 chunk_hi() const override { return {1, 1, 1}; }
  bool generate(const IVec3&, std::vector<Vox>& out) const override {
    out.assign(kChunkVox, kAir);
    return false;
  }
  V3 spawn_pos() const override { return {0.0, 0.0, 0.0}; }
  V3 spawn_dir() const override { return {1, 0, 0}; }
  const WorldSemantics* semantics() const override { return &s_; }
  OneHouse s_;
};

}  // namespace

TEST_CASE("semantics: a source's semantics are the game's; a world without them has none") {
  Game g;
  CHECK(g.semantics() == nullptr);
  g.load_streaming(std::make_shared<SemanticSource>(), 0.125, StreamConfig{});
  REQUIRE(g.semantics() != nullptr);
  std::vector<BuildingInfo> bs;
  g.semantics()->buildings_in(V3{-5, -5, -5}, V3{5, 5, 5}, bs);
  REQUIRE(bs.size() == 1);
  CHECK(bs[0].entrances.size() == 1);
  CHECK(g.semantics()->zone_at(V3{1, 1, 0}).district == "suburb");
}

TEST_CASE("semantics: the C ABI gives them as JSON") {
  svx_engine* e = svx_create(0.125);
  CHECK(std::string(svx_buildings_in(e, -5, -5, -5, 5, 5, 5)) == "[]");
  CHECK(std::string(svx_zone_at(e, 0, 0, 0)) == R"({"district":"","settlement":"","flavor":""})");
  svx_destroy(e);
  // (a game with a semantic source behind the C ABI: the same text its records make)
  Game g;
  g.load_streaming(std::make_shared<SemanticSource>(), 0.125, StreamConfig{});
  std::vector<FurnitureInfo> fs;
  g.semantics()->furniture_in(V3{0, 0, 0}, V3{9, 9, 9}, fs);
  REQUIRE(fs.size() == 1);
  CHECK(fs[0].uses.size() == 2);
}
