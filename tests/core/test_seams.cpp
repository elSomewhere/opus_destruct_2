// Seams and resting objects (ChunkSource::generate_seams, World::loosen, docs/CORE.md §3): faces
// a source draws unbonded are base state; an object with seams round it is a free component that
// rests where it stands until something moves it. And sparse broken faces: a chunk's few broken
// faces are a sorted list, many an array.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

#include "doctest.h"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

constexpr f64 kH = 0.125;
const Vox kRock = make_vox(MaterialId::Rock, true);
const Vox kConcrete = make_vox(MaterialId::Concrete, false);
const Vox kWood = make_vox(MaterialId::Wood, false);

// The bench of the plan's prop experiments (12 x 4 x 4 voxels of wood) standing on anchored rock.
bool bench(i32 x, i32 y, i32 z) { return x >= 40 && x < 52 && y >= 40 && y < 44 && z >= 8 && z < 12; }
// A wooden post on the same floor, bonded to it (no seams): it stands.
bool post(i32 x, i32 y, i32 z) { return x >= 8 && x < 10 && y >= 8 && y < 10 && z >= 8 && z < 24; }
Vox scene(i32 x, i32 y, i32 z) {
  if (z < 8) return kRock;
  if (bench(x, y, z) || post(x, y, z)) return kWood;
  return kAir;
}
V3 centre(i32 x, i32 y, i32 z) { return V3{x * kH, y * kH, z * kH}; }  // (voxel p is centred at p h)
IVec3 local(i32 i) { return {i >> 10, (i >> 5) & 31, i & 31}; }  // (Chunk::v order)

// Rock up to z = 8 voxels over the extent, the bench and the post; the bench drawn with seams on
// every face between it and anything else (seams = false: none, the bench bonds to the floor).
class BenchSource final : public ChunkSource {
 public:
  explicit BenchSource(bool seams = true) : seams_(seams) {}
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    if (cc[2] != 0) return false;
    out.assign(kChunkVox, kAir);
    for (i32 i = 0; i < kChunkVox; ++i) {
      const IVec3 l = local(i);
      out[size_t(i)] = scene(cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], l[2]);
    }
    return true;
  }
  bool generate_seams(const IVec3& cc, std::vector<u8>& out) const override {
    ++seam_calls;
    if (!seams_ || cc[2] != 0) return false;
    out.assign(kChunkVox, 0);
    bool any = false;
    for (i32 i = 0; i < kChunkVox; ++i) {
      const IVec3 l = local(i);
      const IVec3 p{cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], l[2]};
      if (!vox_solid(scene(p[0], p[1], p[2]))) continue;
      for (int a = 0; a < 3; ++a) {
        IVec3 q = p;
        q[a] += 1;
        if (vox_solid(scene(q[0], q[1], q[2])) && bench(p[0], p[1], p[2]) != bench(q[0], q[1], q[2])) {
          out[size_t(i)] = static_cast<u8>(out[size_t(i)] | (1u << a));
          any = true;
        }
      }
    }
    return any;
  }
  IVec3 chunk_lo() const override { return {-6, -6, -2}; }
  IVec3 chunk_hi() const override { return {6, 6, 2}; }
  void column_range(i32, i32, i32* z_lo, i32* z_hi, Vox* below) const override {
    *z_lo = 0;
    *z_hi = 1;
    *below = kRock;
  }
  bool seams_;
  mutable std::atomic<i64> seam_calls{0};
};

World streamed(std::shared_ptr<const ChunkSource> src, const V3& focus = V3{5.0, 5.0, 1.0}) {
  World w;
  VoxelGrid g;
  g.h = kH;
  w.load(std::move(g));
  StreamConfig sc;
  sc.load_radius = 14.0;
  sc.evict_radius = 20.0;
  sc.chunks_per_tick = 4096;
  sc.archive_mb = 0.0;
  w.enable_streaming(std::move(src), sc);
  w.set_focus(focus);
  for (int t = 0; t < 3; ++t) w.tick();
  return w;
}

i32 bench_left(const World& w) {
  i32 n = 0;
  for (i32 x = 40; x < 52; ++x)
    for (i32 y = 40; y < 44; ++y)
      for (i32 z = 8; z < 12; ++z) n += w.grid().get(x, y, z) == kWood;
  return n;
}

i64 piece_voxels(const World& w, i64 id) {
  for (const PieceState& p : w.pieces())
    if (p.id == id) return p.voxels;
  return 0;
}

V3 piece_pos(const World& w, i64 id) {
  for (const PieceState& p : w.pieces())
    if (p.id == id) return p.pos;
  return V3{};
}

}  // namespace

TEST_CASE("seams: a chunk's few broken faces are a sorted list, many a byte per voxel; both read the same") {
  VoxelGrid g;
  g.h = kH;
  for (i32 x = 0; x < 32; ++x)
    for (i32 y = 0; y < 32; ++y) g.fill_column(x, y, 0, 32, kWood);
  const Chunk* c = g.chunk({0, 0, 0});
  REQUIRE(c != nullptr);
  // a hundred faces: a list
  for (i32 i = 0; i < 100; ++i) g.break_bond({i % 32, (i / 32) * 3, 5}, i % 3);
  CHECK(c->broken.empty());
  CHECK(c->broken_few.size() == 100);
  CHECK(g.broken({7, 0, 5}, 7 % 3));
  CHECK_FALSE(g.broken({7, 0, 5}, (7 + 1) % 3));
  CHECK_FALSE(g.broken({7, 1, 5}, 7 % 3));
  // the same face again: nothing new
  g.break_bond({7, 0, 5}, 7 % 3);
  CHECK(c->broken_few.size() == 100);
  // its record carries them as before, and comes back as a list
  VoxelGrid g2;
  g2.h = kH;
  REQUIRE(g2.apply_record(g.chunk_record(key3(0, 0, 0))));
  CHECK(g2.chunk({0, 0, 0})->broken_few == c->broken_few);
  // past kBrokenListMax voxels with one: an array
  for (i32 i = 0; i < 3000; ++i) g.break_bond({i % 32, (i / 32) % 32, 20 + (i / 1024)}, 0);
  CHECK(c->broken.size() == size_t(kChunkVox));
  CHECK(c->broken_few.empty());
  CHECK(g.broken({7, 0, 5}, 7 % 3));
  CHECK(g.broken({31, 31, 20}, 0));
  CHECK_FALSE(g.broken({31, 31, 20}, 1));
  // the record of the array: the same faces, an array again
  VoxelGrid g3;
  g3.h = kH;
  REQUIRE(g3.apply_record(g.chunk_record(key3(0, 0, 0))));
  CHECK(g3.chunk({0, 0, 0})->broken == c->broken);
  for (i32 i = 0; i < kChunkVox; i += 97) CHECK(g3.chunk({0, 0, 0})->broken_at(i) == c->broken_at(i));
}

TEST_CASE("seams: a source's seams are base state - the bench rests, nothing is changed, a shot makes it one piece") {
  auto src = std::make_shared<BenchSource>();
  World w = streamed(src);
  CHECK(src->seam_calls > 0);
  REQUIRE(bench_left(w) == 192);
  CHECK(w.grid().broken({45, 41, 7}, 2));   // (the floor's face under the bench: a seam)
  CHECK_FALSE(w.grid().broken({51, 41, 9}, 0));  // (no voxel beyond: a seam to air is not stored)
  CHECK_FALSE(w.grid().broken({45, 41, 9}, 2));  // (inside the bench: bonded)
  for (int t = 0; t < 60; ++t) w.tick();
  CHECK(w.pieces().empty());
  CHECK(bench_left(w) == 192);
  CHECK_FALSE(w.modified());  // (seams are not a change)
  // the plan's shot (0.1 m, 50 J): the bench comes loose whole, less what the shot took
  w.shoot(centre(46, 41, 11), 0.1, 50.0);
  int added = 0;
  for (int t = 0; t < 30; ++t) {
    w.tick();
    for (const WorldEvent& e : w.take_events()) added += e.kind == WorldEvent::Kind::PieceAdded;
  }
  CHECK(added == 1);
  REQUIRE(w.pieces().size() == 1);
  const i64 n = piece_voxels(w, w.pieces()[0].id);
  CHECK(n >= 185);
  CHECK(n <= 192);
  CHECK(bench_left(w) == 0);
  CHECK(w.grid().get(8, 8, 20) == kWood);  // (the post stands)
}

TEST_CASE("seams: without seams the same shot leaves the bench standing on its floor") {
  World w = streamed(std::make_shared<BenchSource>(false));
  w.shoot(centre(46, 41, 11), 0.1, 50.0);
  for (int t = 0; t < 30; ++t) w.tick();
  CHECK(w.pieces().empty());
  CHECK(bench_left(w) >= 185);
}

TEST_CASE("seams: isolated edits tear every face - the same shot turns such a bench to dust") {
  World w = streamed(std::make_shared<BenchSource>(false));
  // the bench written again with kEditIsolated (its faces torn, to the floor and inside it)
  std::vector<VoxelEdit> e;
  for (i32 x = 40; x < 52; ++x)
    for (i32 y = 40; y < 44; ++y)
      for (i32 z = 8; z < 12; ++z) e.push_back({{x, y, z}, kWood});
  w.set_voxels(e, kEditIsolated);
  CHECK_FALSE(w.grid().bond({43, 41, 9}, 0));
  w.shoot(centre(46, 41, 11), 0.1, 50.0);
  int pieces = 0;
  for (int t = 0; t < 30; ++t) {
    w.tick();
    for (const WorldEvent& ev : w.take_events()) pieces += ev.kind == WorldEvent::Kind::PieceAdded;
  }
  CHECK(pieces == 0);
  CHECK(bench_left(w) == 0);
}

TEST_CASE("seams: a block dropped on a resting bench knocks it loose as a piece of its own") {
  World w = streamed(std::make_shared<BenchSource>());
  std::vector<VoxelEdit> block;
  for (i32 x = 43; x < 49; ++x)
    for (i32 y = 39; y < 45; ++y)
      for (i32 z = 30; z < 36; ++z) block.push_back({{x, y, z}, kConcrete});
  REQUIRE(w.set_voxels(block) == 216);
  for (int t = 0; t < 120; ++t) w.tick();
  CHECK(bench_left(w) == 0);
  bool found = false;
  for (const PieceState& p : w.pieces()) found = found || piece_voxels(w, p.id) == 192;
  CHECK(found);
}

TEST_CASE("seams: loosen() pushes a resting object off as a piece; what stands stays") {
  World w = streamed(std::make_shared<BenchSource>());
  CHECK(w.loosen(kWorldGrid, {8, 8, 12}, V3{0, 100.0, 0}) == 0);   // (the post: bonded to the floor)
  CHECK(w.loosen(kWorldGrid, {45, 41, 4}, V3{0, 100.0, 0}) == 0);  // (the floor: anchored)
  CHECK(w.loosen(kWorldGrid, {45, 41, 20}, V3{0, 100.0, 0}) == 0); // (air)
  const i64 id = w.loosen(kWorldGrid, {45, 41, 10}, V3{0, 400.0, 0});
  REQUIRE(id != 0);
  CHECK(bench_left(w) == 0);
  w.tick();  // (announced)
  REQUIRE(piece_voxels(w, id) == 192);
  const f64 y0 = piece_pos(w, id).y;
  CHECK(y0 > 41.5 * kH);  // (it has moved a tick's way already)
  for (int t = 0; t < 20; ++t) w.tick();
  REQUIRE(piece_voxels(w, id) == 192);
  CHECK(piece_pos(w, id).y > y0 + 0.1);  // (pushed along +y)
  CHECK(w.grid().get(8, 8, 20) == kWood);
}

// A heavy ball (an articulation of one link: 30 kg, 294 N) set down on the bench: it presses on
// it, far below what an impact must load a structure with to be looked at (4 load_trigger_abs).
ArticulationId ball_on_bench(World& w) {
  ArticulationDesc d;
  LinkDesc L;
  L.mass = 30.0;
  const f64 r = 0.15;
  L.inertia = V3{0.4 * L.mass * r * r, 0.4 * L.mass * r * r, 0.4 * L.mass * r * r};
  L.pos = V3{46 * kH, 42 * kH, 11.5 * kH + r + 0.0005};  // (on its top face)
  L.spheres = {{V3{}, r}};
  d.links.push_back(L);
  return w.add_articulation(d);
}

// (ticks until the bench came loose, the ball kept awake - a character's hand never sleeps; -1:
// not in n)
int ticks_to_loose(World& w, ArticulationId ball, int n) {
  for (int t = 0; t < n; ++t) {
    w.wake_articulation(ball);
    w.tick();
    if (bench_left(w) == 0) return t + 1;
  }
  return -1;
}

TEST_CASE("seams: a link pressing on a resting object makes it a piece at once") {
  World w = streamed(std::make_shared<BenchSource>());
  const ArticulationId ball = ball_on_bench(w);
  REQUIRE(ball != 0);
  const int t = ticks_to_loose(w, ball, 60);
  CHECK(t >= 1);
  CHECK(t <= 3);
  i64 n = 0;
  for (const PieceState& p : w.pieces()) n = std::max<i64>(n, p.voxels);
  CHECK(n == 192);
}

TEST_CASE("seams: with link_loosen_force 0 an awake link's weight leaves a resting object where it is") {
  // (a body falling asleep on it would look at it: it leaves its contacts' loads on what it rests on)
  World w = streamed(std::make_shared<BenchSource>());
  WorldConfig c = w.config();
  c.link_loosen_force = 0.0;
  w.configure(c);
  const ArticulationId ball = ball_on_bench(w);
  REQUIRE(ball != 0);
  CHECK(ticks_to_loose(w, ball, 120) == -1);
  CHECK(w.pieces().empty());
}

TEST_CASE("seams: a link on a bench of no seams looks at nothing (a world without seams)") {
  World w = streamed(std::make_shared<BenchSource>(false));
  const ArticulationId ball = ball_on_bench(w);
  REQUIRE(ball != 0);
  CHECK(ticks_to_loose(w, ball, 60) == -1);
  CHECK(w.pieces().empty());
}

TEST_CASE("seams: a seamed chunk goes out of range and comes back as it was generated (not a change, still resting)") {
  World w = streamed(std::make_shared<BenchSource>());
  w.set_focus(V3{-60.0, -60.0, 1.0});
  for (int t = 0; t < 20; ++t) w.tick();
  REQUIRE(w.grid().chunk({1, 1, 0}) == nullptr);
  w.set_focus(V3{5.0, 5.0, 1.0});
  for (int t = 0; t < 20; ++t) w.tick();
  CHECK(bench_left(w) == 192);
  CHECK(w.grid().broken({45, 41, 7}, 2));
  CHECK_FALSE(w.modified());
  CHECK(w.pieces().empty());
  // a change there (a corner of the floor carved) goes out and comes back with the seams
  w.set_voxels({{{20, 20, 7}, kAir}});
  CHECK(w.modified());
  w.set_focus(V3{-60.0, -60.0, 1.0});
  for (int t = 0; t < 20; ++t) w.tick();
  w.set_focus(V3{5.0, 5.0, 1.0});
  for (int t = 0; t < 20; ++t) w.tick();
  CHECK(w.grid().get(20, 20, 7) == kAir);
  CHECK(w.grid().broken({45, 41, 7}, 2));
  CHECK(bench_left(w) == 192);
  CHECK(w.pieces().empty());
}

TEST_CASE("seams: a saved session with a seamed source loads to the same state") {
  auto src = std::make_shared<BenchSource>();
  World a = streamed(src);
  a.shoot(centre(46, 41, 11), 0.1, 50.0);
  a.set_voxels({{{20, 20, 7}, kAir}});
  for (int t = 0; t < 30; ++t) a.tick();
  const std::vector<u8> delta = a.save_delta();
  World b = streamed(src);
  REQUIRE(b.load_delta(delta));
  CHECK(b.grid().get(20, 20, 7) == kAir);
  CHECK(b.grid().broken({45, 41, 7}, 2));
  CHECK(bench_left(b) == bench_left(a));
}

TEST_CASE("seams: bake keeps a seamed object resting on a floor and removes what floats") {
  VoxelGrid g;
  g.h = kH;
  for (i32 x = 0; x < 64; ++x)
    for (i32 y = 0; y < 64; ++y) g.fill_column(x, y, 0, 8, kRock);
  // the bench, seamed to the floor; a block of concrete floating above it, unseamed; a seamed
  // crate in the air (seams, but nothing beneath)
  for (i32 x = 40; x < 52; ++x)
    for (i32 y = 40; y < 44; ++y) g.fill_column(x, y, 8, 12, kWood);
  for (i32 x = 40; x < 52; ++x)
    for (i32 y = 40; y < 44; ++y) g.break_bond({x, y, 7}, 2);
  for (i32 x = 10; x < 14; ++x)
    for (i32 y = 10; y < 14; ++y) g.fill_column(x, y, 30, 34, kConcrete);
  for (i32 x = 20; x < 24; ++x)
    for (i32 y = 20; y < 24; ++y) g.fill_column(x, y, 30, 34, kWood);
  for (i32 x = 20; x < 24; ++x)
    for (i32 y = 20; y < 24; ++y) g.break_bond({x, y, 29}, 2);
  g.compact();
  g.lo = {0, 0, 0};
  g.hi = {64, 64, 64};
  World w;
  w.load(std::move(g));
  w.bake();
  CHECK(bench_left(w) == 192);
  CHECK(w.grid().get(11, 11, 31) == kAir);
  CHECK(w.grid().get(21, 21, 31) == kAir);
  CHECK(w.design_report().floating_voxels == 128);
  for (int t = 0; t < 30; ++t) w.tick();
  CHECK(w.pieces().empty());
  CHECK(bench_left(w) == 192);
  // the resting bench comes loose when pushed
  const i64 id = w.loosen(kWorldGrid, {45, 41, 10}, V3{0, 0, 10.0});
  CHECK(id != 0);
}

TEST_CASE("seams: a grid keeps the broken faces it is added with") {
  World w;
  VoxelGrid ground;
  ground.h = kH;
  for (i32 x = -16; x < 16; ++x)
    for (i32 y = -16; y < 16; ++y) ground.fill_column(x, y, -8, 0, kRock);
  ground.compact();
  ground.lo = {-16, -16, -8};
  ground.hi = {16, 16, 32};
  w.load(std::move(ground));
  w.bake();
  VoxelGrid g;
  for (i32 x = 0; x < 4; ++x)
    for (i32 y = 0; y < 4; ++y) g.fill_column(x, y, 0, 4, kRock);
  g.break_bond({1, 1, 1}, 0);
  g.break_bond({2, 1, 1}, 2);
  const GridId id = w.add_grid(GridFrame{V3{0.0, 0.0, 1.0}, Quat{0, 0, 0.3826834323650898, 0.9238795325112867}}, std::move(g));
  REQUIRE(id != 0);
  REQUIRE(w.grid(id) != nullptr);
  CHECK(w.grid(id)->broken({1, 1, 1}, 0));
  CHECK(w.grid(id)->broken({2, 1, 1}, 2));
  CHECK_FALSE(w.grid(id)->broken({1, 1, 1}, 1));
}
