// Host records (World::archive_host_record): a host's state archived with its region, back when the
// region is known again, forgotten with it, saved with the session; and the awake pieces' query.
#include <memory>
#include <vector>

#include "doctest.h"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

constexpr f64 kH = 0.125;
const Vox kRock = make_vox(MaterialId::Rock, true);

// A strip of rock ground, 80 chunks along x.
class Strip final : public ChunkSource {
 public:
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    if (cc[2] != -1) return false;
    out.assign(kChunkVox, kRock);
    return true;
  }
  IVec3 chunk_lo() const override { return {0, 0, -1}; }
  IVec3 chunk_hi() const override { return {80, 8, 1}; }
};

World streamed(const StreamConfig& sc, const V3& focus) {
  World w;
  VoxelGrid g;
  g.h = kH;
  w.load(std::move(g));
  w.enable_streaming(std::make_shared<Strip>(), sc);
  w.set_focus(focus);
  return w;
}

StreamConfig config() {
  StreamConfig sc;
  sc.load_radius = 16.0;
  sc.evict_radius = 24.0;
  sc.chunks_per_tick = 400;
  return sc;
}

}  // namespace

TEST_CASE("host records: archived with the region of their place, back when it is known again") {
  World w = streamed(config(), V3{8.0, 8.0, 0.0});
  for (int t = 0; t < 10; ++t) w.tick();
  const V3 at{12.0, 10.0, 0.2};
  const std::vector<u8> data{1, 2, 3, 4, 5};
  REQUIRE(w.archive_host_record(7, 42, at, data));
  CHECK(w.stats().archived_records == 1);
  // (its chunk is resident: it comes back at once - the host asked to keep it for a place it knows)
  w.tick();
  auto back = w.take_host_records(7);
  REQUIRE(back.size() == 1);
  CHECK(back[0].id == 42);
  CHECK(back[0].owner == 7);
  CHECK(back[0].data == data);
  CHECK(norm(back[0].at - at) == 0.0);
  CHECK(w.take_host_records(7).empty());
  CHECK(w.stats().archived_records == 0);

  // out of range: kept until the place is back
  for (int t = 0; t < 10; ++t) w.tick();
  w.set_focus(V3{300.0, 8.0, 0.0});
  for (int t = 0; t < 120; ++t) w.tick();
  REQUIRE_FALSE(w.chunk_resident({1, 1, 0}));
  REQUIRE(w.archive_host_record(7, 43, at, data));
  for (int t = 0; t < 30; ++t) w.tick();
  CHECK(w.take_host_records(7).empty());
  CHECK(w.take_host_records(8).empty());  // (another owner's: none)
  w.set_focus(V3{8.0, 8.0, 0.0});
  std::vector<HostRecord> later;
  for (int t = 0; t < 120 && later.empty(); ++t) {
    w.tick();
    later = w.take_host_records(7);
  }
  REQUIRE(later.size() == 1);
  CHECK(later[0].id == 43);
}

TEST_CASE("host records: forgotten with their region, saved with the session") {
  World w = streamed(config(), V3{8.0, 8.0, 0.0});
  for (int t = 0; t < 10; ++t) w.tick();
  w.set_focus(V3{300.0, 8.0, 0.0});
  for (int t = 0; t < 120; ++t) w.tick();
  REQUIRE(w.archive_host_record(3, 9, V3{12.0, 10.0, 0.2}, std::vector<u8>(64, 7)));
  // the session keeps it: a world loaded from it hands it back when the place comes back
  const std::vector<u8> delta = w.save_delta();
  World w2 = streamed(config(), V3{300.0, 8.0, 0.0});
  REQUIRE(w2.load_delta(delta));
  CHECK(w2.stats().archived_records == 1);
  w2.set_focus(V3{8.0, 8.0, 0.0});
  std::vector<HostRecord> back;
  for (int t = 0; t < 120 && back.empty(); ++t) {
    w2.tick();
    back = w2.take_host_records(3);
  }
  REQUIRE(back.size() == 1);
  CHECK(back[0].data == std::vector<u8>(64, 7));
  // not streaming: nothing to keep it in
  World flat;
  VoxelGrid g;
  g.h = kH;
  flat.load(std::move(g));
  CHECK_FALSE(flat.archive_host_record(3, 1, V3{}, {1}));
}

TEST_CASE("awake pieces: the pieces awake now, with their boxes, and none asleep or of an articulation") {
  World w;
  VoxelGrid g;
  g.h = kH;
  for (i32 x = -8; x < 8; ++x)
    for (i32 y = -8; y < 8; ++y) g.fill_column(x, y, -2, 0, kRock);
  g.compact();
  w.load(std::move(g));
  CHECK(w.awake_pieces().empty());
  std::vector<VoxelEdit> crate;
  for (i32 x = 0; x < 3; ++x)
    for (i32 y = 0; y < 3; ++y)
      for (i32 z = 8; z < 11; ++z) crate.push_back({{x, y, z}, make_vox(MaterialId::Wood, false)});
  w.set_voxels(kWorldGrid, crate);
  for (int t = 0; t < 3; ++t) w.tick();
  const auto awake = w.awake_pieces();
  REQUIRE(awake.size() == 1);
  CHECK(awake[0].lo.z < awake[0].hi.z);
  for (int t = 0; t < 600 && !w.awake_pieces().empty(); ++t) w.tick();
  CHECK(w.awake_pieces().empty());  // (at rest: asleep)
}
