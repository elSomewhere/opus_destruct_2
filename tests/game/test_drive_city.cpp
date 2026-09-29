// The endless city (docs/VEHICLES.md): its chunks, its roads, and traffic driving them.
#include <cmath>
#include <map>
#include <vector>

#include "doctest.h"
#include "svx/game/drive_city.hpp"
#include "svx/game/game.hpp"

using namespace svx;

namespace {

constexpr f64 h = 0.125;

IVec3 voxel_at(const V3& p) { return {static_cast<i32>(std::floor(p.x / h + 0.5)), static_cast<i32>(std::floor(p.y / h + 0.5)), static_cast<i32>(std::floor(p.z / h + 0.5))}; }

// A voxel and its paint, from the source.
struct Probe {
  const ChunkSource& src;
  std::map<u64, std::pair<std::vector<Vox>, std::vector<u8>>> cache;
  std::pair<Vox, u8> at(const IVec3& p) {
    const IVec3 c = chunk_of(p);
    const u64 k = key3(c[0], c[1], c[2]);
    auto it = cache.find(k);
    if (it == cache.end()) {
      std::vector<Vox> v;
      std::vector<u8> paint;
      src.generate(c, v);
      if (!src.generate_layer(c, "paint", paint)) paint.assign(kChunkVox, 0);
      it = cache.emplace(k, std::make_pair(std::move(v), std::move(paint))).first;
    }
    const i32 i = chunk_index(p);
    return {it->second.first[size_t(i)], it->second.second[size_t(i)]};
  }
};

}  // namespace

TEST_CASE("drive city: its lanes run on asphalt, marked, between kerbs, and lead on through the junctions") {
  const auto src = make_drive_city(7);
  const RoadNetwork* roads = src->roads();
  REQUIRE(roads);
  Probe probe{*src, {}};
  std::vector<Lane> lanes;
  roads->lanes_in(V3{-60, -60, 0}, V3{60, 60, 0}, lanes);
  MESSAGE(lanes.size() << " lanes within 60 m of the origin");
  REQUIRE(lanes.size() > 20);
  i32 checked = 0, turns = 0;
  for (const Lane& l : lanes) {
    Lane back;
    REQUIRE(roads->lane(l.id, &back));
    CHECK(back.a.x == l.a.x);
    CHECK(back.b.y == l.b.y);
    const f64 len = norm(l.b - l.a);
    CHECK(len > 20.0);
    // (on asphalt all along)
    for (f64 u : {0.1, 0.5, 0.9}) {
      const V3 p = l.a + (l.b - l.a) * u;
      const auto [v, paint] = probe.at(voxel_at(p - V3{0, 0, 0.05}));
      CHECK(vox_mat(v) == MaterialId::Asphalt);
      CHECK_FALSE(vox_solid(probe.at(voxel_at(p + V3{0, 0, 0.1})).first));  // (and air over it)
      (void)paint;
    }
    // the lanes it leads on to start at its junction and go on its way, or turn
    std::vector<std::pair<u64, int>> next;
    roads->next(l.id, next);
    CHECK(!next.empty());
    const V3 d = (l.b - l.a) * (1.0 / len);
    for (const auto& [nid, turn] : next) {
      Lane n;
      REQUIRE(roads->lane(nid, &n));
      const V3 nd = normalized(n.b - n.a);
      CHECK(norm(n.a - l.b) < 30.0);
      const f64 cr = d.x * nd.y - d.y * nd.x;
      if (turn == 0) CHECK(dot(d, nd) > 0.99);
      if (turn == 1) CHECK(cr < -0.99);  // (right: clockwise)
      if (turn == -1) CHECK(cr > 0.99);
      turns += turn != 0 ? 1 : 0;
    }
    ++checked;
  }
  CHECK(turns > 0);
  // the markings: white and yellow lines on the roads near the origin
  std::map<int, i32> paints;
  for (i32 x = -300; x < 300; ++x)
    for (i32 y = -300; y < 300; ++y) {
      const auto [v, p] = probe.at({x, y, -1});
      if (vox_mat(v) == MaterialId::Asphalt) ++paints[p];
    }
  CHECK(paints[static_cast<int>(Paint::LineWhite)] > 10);
  CHECK(paints[static_cast<int>(Paint::LineYellow)] > 10);
  // parking along the streets, on asphalt
  std::vector<ParkingSpot> spots;
  roads->parking_in(V3{-120, -120, 0}, V3{120, 120, 0}, spots);
  CHECK(spots.size() > 10);
  for (const ParkingSpot& s : spots) CHECK(vox_mat(probe.at(voxel_at(s.pos - V3{0, 0, 0.05})).first) == MaterialId::Asphalt);
  // it goes on: 60 km out, a chunk of it is made like any other
  std::vector<Vox> far;
  CHECK(src->generate(IVec3{15000, -15000, -1}, far));
  CHECK(src->chunk_hi()[0] - src->chunk_lo()[0] > 60000);
}

TEST_CASE("drive city: streamed, its traffic drives the lanes around the viewer and parks at the kerbs") {
  Game game;
  std::shared_ptr<GameSource> src = make_drive_city(11);
  StreamConfig sc;
  sc.load_radius = 90.0;
  sc.evict_radius = 120.0;
  sc.chunks_per_tick = 400;
  game.load_streaming(src, h, sc);
  const V3 eye = src->spawn_pos();
  game.set_viewer(eye);
  for (int t = 0; t < 20; ++t) game.tick();  // (the ground around it)
  i32 moving = 0, total = 0, parked = 0, wrecks = 0, fallen = 0;
  for (int t = 0; t < 900; ++t) {
    game.tick();
    if (t % 300 != 299) continue;
    moving = total = parked = wrecks = fallen = 0;
    for (const VehicleView& v : game.vehicles()) {
      ++total;
      if (v.flags & VehicleView::kParked) ++parked;
      if (v.flags & VehicleView::kWreck) ++wrecks;
      if (!(v.flags & VehicleView::kParked) && std::abs(v.speed) > 2.0) ++moving;
      if (v.chassis && v.pos.z < -0.5) ++fallen;
    }
    MESSAGE("after " << (t + 1) / 60.0 << " s: " << total << " vehicles, " << parked << " parked, " << moving << " driving, " << wrecks
                     << " wrecks, " << fallen << " below the road");
  }
  CHECK(total > 8);
  CHECK(parked > 3);
  CHECK(moving > 3);
  CHECK(fallen == 0);
  CHECK(wrecks <= total / 3);
}
