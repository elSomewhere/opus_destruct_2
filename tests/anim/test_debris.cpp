// svx_anim gibs and blood (the TypeScript test/debris.test.ts, and more): gibs dropped or thrown come
// to rest and sleep, never deep in solid voxels; blood stains the ground and the walls it hits;
// skins and blasts; gibs that lose their support; gibs on the voxels of a core World; a
// character's pieces as the original game spawned them; the caps that bound the memory;
// determinism.
#include <algorithm>
#include <cmath>
#include <string>

#include "doctest.h"
#include "scene.hpp"
#include "svx/anim/characters/props.hpp"
#include "svx/anim/physics/debris.hpp"

using namespace scene;

namespace {

const Path kPaths[] = {Path::Shallow, Path::Deep};

const VoxelModel& soldier_model() {
  static const ModelPtr m = make_soldier(1).model;
  return *m;
}
f64 vs() { return soldier_model().voxel_size; }
const VoxelPart& part_of(i32 bone) {
  const VoxelModel& m = soldier_model();
  return m.parts[size_t(m.part_of_bone[size_t(bone)])];
}
const V3& rest_head(i32 bone) { return soldier_model().skeleton->rest_head[size_t(bone)]; }

// The world centres of every solid cell of a gib.
std::vector<V3> cell_centres(const GibSystem& sys, const Gib& g) {
  const VoxelPart& p = g.part;
  const f64 s = g.voxel_size;
  std::vector<V3> out;
  for (i32 z = 0; z < p.dims[2]; ++z)
    for (i32 y = 0; y < p.dims[1]; ++y)
      for (i32 x = 0; x < p.dims[0]; ++x) {
        if (p.cells[size_t(p.index(x, y, z))] == 0) continue;
        out.push_back(sys.world_point(g, V3{(p.origin[0] + x + 0.5) * s, (p.origin[1] + y + 0.5) * s, (p.origin[2] + z + 0.5) * s}));
      }
  return out;
}

// The lowest bottom of a gib's cells (a cell's centre less half a voxel).
f64 lowest_bottom(const GibSystem& sys, const Gib& g) {
  f64 low = 1e300;
  for (const V3& p : cell_centres(sys, g)) low = std::min(low, p.z - g.voxel_size / 2.0);
  return low;
}

// A point through a column-major skin matrix (16 floats).
V3 transform_point(const f32* m, const V3& p) {
  return V3{m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13], m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}

// How a heap of gibs settles over `frames`: when the last one of more than 20 voxels fell asleep
// (-1: never), how many are awake at the end, and how far any moved over the last second. (The
// smallest fragments, a dozen voxels or so, may jitter in place without ever falling asleep, as the
// original's do - its GibSystem, given the same pieces, does the same bit for bit: at rest, but not
// asleep.)
struct Settled {
  f64 slept = -1.0;
  i32 awake = 0;
  f64 moved = 0.0;
};

Settled settle(GibSystem& gibs, i32 frames) {
  Settled r;
  std::vector<std::pair<u32, V3>> before;
  for (i32 i = 0; i < frames; ++i) {
    if (i == frames - 60)
      for (const auto& g : gibs.gibs) before.push_back({g->id, g->pos});
    gibs.update(DT);
    bool asleep = true;
    for (const auto& g : gibs.gibs) asleep = asleep && (g->asleep || g->part.count <= 20);
    if (asleep && r.slept < 0.0) r.slept = (i + 1) / 60.0;
  }
  for (const auto& g : gibs.gibs) {
    if (!g->asleep) ++r.awake;
    for (const auto& [id, p] : before)
      if (id == g->id) r.moved = std::max(r.moved, vdist(g->pos, p));
  }
  return r;
}

}  // namespace

TEST_CASE("anim debris: a gib dropped with spin comes to rest on the ground and sleeps") {
  const FlatGround ground(0.0);
  GibSystem sys(&ground);
  const f64 s = vs();
  const i32 bone = H::forearmL;
  Gib* g = sys.spawn(part_of(bone), s, V3{0.3, -0.2, 2}, qaxis(vnorm(V3{0.3, 1, 0.2}), 1.1), rest_head(bone), V3{0.5, 0, 0}, V3{3, -2, 5});
  f64 slept = -1.0;
  for (i32 i = 0; i < 240; ++i) {
    sys.update(DT);
    if (g->asleep && slept < 0.0) slept = (i + 1) / 60.0;
  }
  MESSAGE("a forearm dropped from 2 m sleeps after " << slept << " s");
  CHECK_MESSAGE((slept > 0.0 && slept <= 4.0), "asleep after " << slept << " s");
  const f64 lowest = lowest_bottom(sys, *g);
  CHECK_MESSAGE(lowest >= -s, "the lowest cell's bottom at " << lowest);
  CHECK_MESSAGE(lowest < 0.05, "resting on the ground, the lowest at " << lowest);
  CHECK_MESSAGE(hypot2(g->pos.x, g->pos.y) < 3.0, "stayed near where it fell");
}

TEST_CASE("anim debris: a gib thrown at 15 m/s into a raised block does not end inside solid voxels") {
  const f64 h = 0.125;
  const auto solid = [](i32 i, i32 j, i32 k) { return k < 0 || (i >= 8 && i < 12 && j >= -24 && j < 24 && k < 8); };
  const VoxelCollision world(h, solid);
  GibSystem sys(&world);
  const f64 s = vs();
  const auto idx = [&](f64 v) { return static_cast<i32>(std::floor(v / h + 0.5)); };
  for (i32 bone : {H::head, H::thighR, H::chest}) {
    CAPTURE(bone);
    Gib* g = sys.spawn(part_of(bone), s, V3{0, 0, 0.35}, Quat{}, rest_head(bone), V3{15, 0.4, 0.5}, V3{0, 4, 1});
    // (the chunk starts with its pivot at bone_pos + (pivot - head); lift it clear of the ground)
    const f64 low = lowest_bottom(sys, *g);
    if (low < 0.05) g->pos.z += 0.05 - low;
    // a cell is deep in solid when its centre and the six points half a voxel around it all are (a
    // cell between collision samples may rest up to half a voxel into a surface)
    const auto deep = [&](const V3& p) {
      static const i32 around[7][3] = {{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
      for (const auto& d : around)
        if (!solid(idx(p.x + (d[0] * s) / 2.0), idx(p.y + (d[1] * s) / 2.0), idx(p.z + (d[2] * s) / 2.0))) return false;
      return true;
    };
    i32 deep_cells = 0;
    std::string first;
    for (i32 i = 0; i < 180; ++i) {
      sys.update(DT);
      for (const V3& p : cell_centres(sys, *g)) {
        if (!deep(p) || deep_cells++ > 0) continue;
        first = "a cell deep in solid at " + std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z) + " (t " +
                std::to_string((i + 1) / 60.0) + ")";
      }
    }
    CHECK_MESSAGE(deep_cells == 0, first);
    // and at the end nothing is below the ground's surface by more than half a voxel
    f64 lowest = 1e300;
    for (const V3& p : cell_centres(sys, *g)) lowest = std::min(lowest, p.z);
    CHECK_MESSAGE(lowest > -h / 2.0 - s / 2.0, "the lowest cell centre at " << lowest);
    CHECK_MESSAGE(g->pos.x < 0.9375, "stopped by the wall, at " << g->pos.x << " " << g->pos.y << " " << g->pos.z);
  }
}

TEST_CASE("anim debris: blood spray leaves stains on the ground with upward normals") {
  const FlatGround ground(0.0);
  GibSystemOptions o;
  o.seed = 3;
  GibSystem sys(&ground, o);
  sys.spray(V3{0, 0, 1.2}, V3{1, 0, 0.3}, 60, 4.0, 0.5);
  CHECK(sys.drops.size() == 60);
  for (i32 i = 0; i < 180; ++i) sys.update(DT);
  CHECK_MESSAGE(sys.drops.empty(), "every drop landed or expired");
  CHECK_MESSAGE(sys.stains.size() > 5, sys.stains.size() << " stains");
  sys.for_each_stain([](const V3& p, const V3& n, f64 size, f64, const Rgb&) {
    CHECK_MESSAGE(std::abs(p.z) < 1e-6, "a stain at height " << p.z);
    CHECK_MESSAGE(n.z > 0.999, "a stain's normal " << n.x << " " << n.y << " " << n.z);
    CHECK((size > 0.0 && size < 0.4));
  });
}

TEST_CASE("anim debris: the skin maps the pivot to the gib's position; impulses wake sleeping gibs") {
  const FlatGround ground(0.0);
  GibSystem sys(&ground);
  const i32 bone = H::head;
  Gib* g = sys.spawn(part_of(bone), vs(), V3{1, 2, 0.5}, qaxis(V3{0, 0, 1}, 0.7), rest_head(bone), V3{}, V3{});
  f32 m[32] = {};
  sys.write_skin(*g, m + 16);
  const V3 p = transform_point(m + 16, g->pivot);
  for (int a = 0; a < 3; ++a) CHECK(std::abs(p[a] - g->pos[a]) < 1e-5);
  for (i32 i = 0; i < 300; ++i) sys.update(DT);
  CHECK_MESSAGE(g->asleep, "the head came to rest");
  sys.impulse(V3{g->pos.x - 0.5, g->pos.y, 0}, 3.0, 8.0);
  CHECK_MESSAGE((!g->asleep && g->vel.x > 0.0), "the blast woke and pushed it");
  sys.update(DT);
  CHECK(g->pos.z > 0.0);
}

TEST_CASE("anim debris: blood hitting a voxel wall stains it with the wall's normal") {
  const f64 h = 0.125;
  const VoxelCollision world(h, [](i32 i, i32, i32 k) { return k < 0 || i >= 8; });
  GibSystemOptions o;
  o.seed = 9;
  GibSystem sys(&world, o);
  sys.spray(V3{0, 0, 1}, V3{1, 0, 0}, 40, 9.0, 0.08);
  for (i32 i = 0; i < 120; ++i) sys.update(DT);
  std::vector<const BloodStain*> wall;
  for (const BloodStain& st : sys.stains)
    if (st.normal.x < -0.99) wall.push_back(&st);
  CHECK_MESSAGE(wall.size() > 3, wall.size() << " wall stains of " << sys.stains.size());
  for (const BloodStain* st : wall) CHECK_MESSAGE(std::abs(st->pos.x - 7.5 * h) < 0.01, "a wall stain at x " << st->pos.x);
}

// ---- the port's own ------------------------------------------------------------------------------

TEST_CASE("anim debris: a sleeping gib whose support is blasted away wakes, falls and rests again below") {
  const f64 h = 0.125, s = vs();
  bool block = true;  // (a block 1 m high, its top at 7.5 h)
  const VoxelCollision world(h, [&](i32 i, i32 j, i32 k) { return k < 0 || (block && k < 8 && i >= -4 && i < 4 && j >= -4 && j < 4); });
  GibSystem sys(&world);
  Gib* g = sys.spawn(part_of(H::head), s, V3{0, 0, 1.3}, Quat{}, rest_head(H::head), V3{}, V3{});
  for (i32 i = 0; i < 180; ++i) sys.update(DT);
  REQUIRE(g->asleep);
  const f64 on_block = lowest_bottom(sys, *g) - 7.5 * h;
  CHECK_MESSAGE((on_block >= -s && on_block < 0.05), "on the block, the lowest " << on_block << " m off its top");
  block = false;
  i32 woke = -1;
  for (i32 i = 0; i < 240; ++i) {
    sys.update(DT);
    if (woke < 0 && !g->asleep) woke = i + 1;
  }
  // (the support is looked for four times a second)
  CHECK_MESSAGE((woke > 0 && woke <= 16), "woke " << woke << " frames after the block went");
  CHECK(g->asleep);
  const f64 on_ground = lowest_bottom(sys, *g) + h / 2.0;
  CHECK_MESSAGE((on_ground >= -s && on_ground < 0.05), "on the ground, the lowest " << on_ground << " m off it");
}

TEST_CASE("anim debris: gibs come to rest on the voxels of a core World (WorldCollision) and sleep; blood stains them") {
  Scene s(Path::Deep);  // (flat voxel ground: its top at s.ground)
  GibSystem sys(s.col.get());
  const f64 vox = vs();
  const i32 bones[2] = {H::forearmL, H::head};
  Gib* gs[2] = {};
  for (i32 k = 0; k < 2; ++k)
    gs[k] = sys.spawn(part_of(bones[k]), vox, V3{0.3 + k, -0.2, 2}, qaxis(vnorm(V3{0.3, 1, 0.2}), 1.1), rest_head(bones[k]), V3{0.5, 0, 0}, V3{3, -2, 5});
  sys.spray(V3{0, 1, 1.2}, V3{1, 0, 0.3}, 40, 4.0, 0.5);
  f64 slept[2] = {-1.0, -1.0};
  for (i32 i = 0; i < 240; ++i) {
    sys.update(DT);
    for (i32 k = 0; k < 2; ++k)
      if (gs[k]->asleep && slept[k] < 0.0) slept[k] = (i + 1) / 60.0;
  }
  for (i32 k = 0; k < 2; ++k) {
    CAPTURE(bones[k]);
    const f64 lowest = lowest_bottom(sys, *gs[k]) - s.ground;
    MESSAGE("on the voxels of a World: bone " << bones[k] << " sleeps after " << slept[k] << " s, its lowest cell " << lowest * 1000.0
                                              << " mm off the ground");
    CHECK_MESSAGE((slept[k] > 0.0 && slept[k] <= 4.0), "asleep after " << slept[k] << " s");
    CHECK_MESSAGE(lowest >= -vox, "the lowest cell's bottom " << lowest << " m off the ground");
    CHECK_MESSAGE(lowest < 0.05, "resting on the ground, the lowest " << lowest << " m off it");
  }
  CHECK(sys.drops.empty());
  CHECK(sys.stains.size() > 5);
  for (const BloodStain& st : sys.stains) {
    CHECK_MESSAGE(std::abs(st.pos.z - s.ground) < 1e-6, "a stain at height " << st.pos.z);
    CHECK(st.normal.z > 0.999);
  }
}

TEST_CASE("anim debris: a nearby blast peppers and throws a coherent body, with bounded blood") {
  for (Path path : kPaths) {
    Scene s(path);
    auto& c = s.add(make_soldier(4), 1, kPi / 2, {0, 0, s.ground}, make_rifle());
    for (int i = 0; i < 30; ++i) s.frame({&c});
    const int before = c.model->voxel_count();
    const V3 centre = c.pose.p[H::pelvis] + V3{.3, .5, -.5};
    const auto result = c.blast(centre, 1, 1);
    CHECK_FALSE(result.gibbed);
    CHECK(c.model->voxel_count() > before / 2);
    CHECK(c.model->voxel_count() < before);
    CHECK_FALSE(c.effects.drops.empty());
    for (int i = 0; i < 180; ++i) s.frame({&c});
    CHECK(c.effects.drops.size() <= size_t(c.effects.max_drops));
    CHECK(c.effects.stains.size() <= size_t(c.effects.max_stains));
    for (const auto& p : c.pose.p) CHECK(std::isfinite(norm(p)));
  }
}

TEST_CASE("anim debris: a cut that severs a forearm sprays blood; the forearm flies off bleeding and comes to rest") {
  Scene s(Path::Shallow);
  Character& c = s.add(make_soldier(4), 1.0, kPi / 2.0, V3{0, 0, s.ground}, make_rifle());
  for (i32 i = 0; i < 30; ++i) s.frame({&c});
  GibSystem gibs(s.col.get());
  std::vector<Gib*> pieces;
  i32 shots = 1;
  const int bone = H::forearmL;
  const V3 centre = vlerp(c.pose.p[bone], c.pose.tail(bone), .55), axis = vnorm(c.pose.tail(bone) - c.pose.p[bone]);
  DamageDescriptor cut;
  cut.kind = DamageKind::Edge;
  cut.mass = 3;
  cut.speed = 36;
  cut.direction = vnorm(cross(axis, V3{0, 0, 1}), V3{0, -1, 0});
  cut.point = centre - cut.direction * .12;
  const V3 edge = vnorm(cross(axis, cut.direction));
  cut.edge_a = centre - edge * .2;
  cut.edge_b = centre + edge * .2;
  cut.swept_length = .4;
  cut.bone = bone;
  auto result = c.damage(cut);
  pieces = wound_gibs(gibs, c, result, cut.point, cut.direction);
  CHECK(pieces.size() == result.gibs.size());
  REQUIRE(!pieces.empty());
  bool hand = false;
  for (Gib* g : pieces) {
    CHECK(g->bleed == kSeveredBleed);
    hand = hand || g->part.bone == H::handL;
  }
  CHECK(hand);
  const Settled rest = settle(gibs, 60 * 8);
  std::string sizes;
  for (Gib* g : pieces) sizes += " " + std::to_string(g->part.count);
  MESSAGE(shots << " shots severed " << pieces.size() << " pieces (voxels:" << sizes << "); all but the smallest asleep after " << rest.slept << " s, "
                << rest.awake << " awake after 8 s (moved " << rest.moved * 1000.0 << " mm in the last second); " << gibs.stains.size() << " stains");
  CHECK_MESSAGE((rest.slept > 0.0 && rest.slept <= 6.0), "all but the smallest asleep after " << rest.slept << " s");
  CHECK(rest.moved < 0.01);
  for (const auto& g : gibs.gibs) {
    const f64 lowest = lowest_bottom(gibs, *g);
    CHECK((lowest >= -g->voxel_size && lowest < 0.05));
  }
  CHECK(!gibs.stains.empty());
}

TEST_CASE("anim debris: the caps bound the memory - the oldest sleeping gib makes room first, then the oldest; drops and stains the oldest") {
  const FlatGround ground(0.0);
  GibSystemOptions o;
  o.max_gibs = 3;
  o.max_drops = 50;
  o.max_stains = 20;
  GibSystem sys(&ground, o);
  const f64 s = vs();
  auto spawn = [&](const V3& at) { return sys.spawn(part_of(H::handL), s, at, Quat{}, rest_head(H::handL), V3{}, V3{})->id; };
  auto ids = [&] {
    std::vector<u32> v;
    for (const auto& g : sys.gibs) v.push_back(g->id);
    return v;
  };
  // two falling from high up, one landing at once (it sleeps first)
  const u32 a = spawn(V3{0, 0, 100}), b = spawn(V3{1, 0, 0.2}), c = spawn(V3{2, 0, 100});
  CHECK(ids() == std::vector<u32>{a, b, c});
  for (i32 i = 0; i < 120; ++i) sys.update(DT);
  REQUIRE(sys.gibs[1]->asleep);
  REQUIRE(!sys.gibs[0]->asleep);
  REQUIRE(!sys.gibs[2]->asleep);
  const u32 d = spawn(V3{3, 0, 100});
  CHECK(ids() == std::vector<u32>{a, c, d});  // (the sleeping one went)
  const u32 e = spawn(V3{4, 0, 100});
  CHECK(ids() == std::vector<u32>{c, d, e});  // (none asleep: the oldest went)
  sys.remove(sys.gibs[1].get());
  CHECK(ids() == std::vector<u32>{c, e});
  // drops: the newest max_drops
  sys.spray(V3{0, 0, 50}, V3{0, 0, 1}, 200, 1.0, 1.0);
  CHECK(sys.drops.size() == 50);
  // stains: one drop straight down every metre (nothing merges), the newest max_stains stay
  for (i32 k = 0; k < 100; ++k) sys.spray(V3{10.0 + k, 0, 0.3}, V3{0, 0, -1}, 1, 1.0, 0.0);
  for (i32 i = 0; i < 60; ++i) sys.update(DT);
  CHECK(sys.stains.size() == 20);
  CHECK(sys.stains.front().pos.x > 10.0 + 79.5);
  // bounded: a thousand more gibs and sprays leave the memory where the caps hold it
  GibSystem big(&ground);
  for (i32 k = 0; k < 128; ++k) big.spawn(part_of(H::chest), s, V3{k * 1.0, 0, 5}, Quat{}, rest_head(H::chest), V3{}, V3{});
  for (i32 k = 0; k < 20; ++k) big.spray(V3{0, 0, 1}, V3{0, 0, 1}, 100, 2.0, 1.0);
  const i64 full = big.memory_bytes();
  for (i32 k = 0; k < 1000; ++k) big.spawn(part_of(H::chest), s, V3{k * 1.0, 0, 5}, Quat{}, rest_head(H::chest), V3{}, V3{});
  for (i32 k = 0; k < 200; ++k) big.spray(V3{0, 0, 1}, V3{0, 0, 1}, 100, 2.0, 1.0);
  CHECK(big.gibs.size() == 128);
  CHECK(big.drops.size() == 800);
  MESSAGE("128 chest gibs and 800 drops: " << full / 1024 << " KiB; after a thousand more gibs and 20000 drops: " << big.memory_bytes() / 1024 << " KiB");
  CHECK(big.memory_bytes() <= full);
  big.clear();
  CHECK(big.memory_bytes() < full / 4);
}

TEST_CASE("anim debris: gibs and drops that fall out of the world go; drops expire") {
  const VoxelCollision nothing(0.125, [](i32, i32, i32) { return false; });
  GibSystemOptions o;
  o.kill_z = -5.0;
  GibSystem sys(&nothing, o);
  sys.spawn(part_of(H::head), vs(), V3{0, 0, 0}, Quat{}, rest_head(H::head), V3{}, V3{});
  sys.spray(V3{0, 0, 0}, V3{0, 0, 1}, 20, 2.0, 0.3);
  i32 gone = -1, drops_gone = -1;
  for (i32 i = 0; i < 120; ++i) {
    sys.update(DT);
    if (gone < 0 && sys.gibs.empty()) gone = i + 1;
    if (drops_gone < 0 && sys.drops.empty()) drops_gone = i + 1;
  }
  // (5 m down in about a second; the drops, thrown up first, a little later)
  CHECK_MESSAGE((gone > 55 && gone <= 62), "gone after " << gone << " frames");
  CHECK_MESSAGE((drops_gone > gone && drops_gone < 120), "the drops gone after " << drops_gone << " frames");
  // drops that land nowhere last 4 s
  GibSystem open(&nothing);
  open.spray(V3{0, 0, 0}, V3{0, 0, 1}, 20, 2.0, 0.3);
  for (i32 i = 0; i < 235; ++i) open.update(DT);
  CHECK(open.drops.size() == 20);
  for (i32 i = 0; i < 10; ++i) open.update(DT);
  CHECK(open.drops.empty());
}

TEST_CASE("anim debris: deterministic (the same seed and calls, the same gibs and blood, bit for bit)") {
  auto run = [](u32 seed) {
    const FlatGround ground(0.0);
    GibSystemOptions o;
    o.seed = seed;
    GibSystem sys(&ground, o);
    const f64 s = vs();
    const i32 bones[4] = {H::head, H::forearmL, H::thighR, H::chest};
    for (i32 k = 0; k < 4; ++k) {
      Gib* g = sys.spawn(part_of(bones[k]), s, V3{0.2 * k, 0, 1.0 + k}, qaxis(vnorm(V3{1, 0.3, k * 0.2}), 0.4 * k), rest_head(bones[k]), V3{1, 0.5 * k, 2},
                         V3{2, -k * 1.0, 3});
      g->bleed = 20.0;
    }
    sys.spray(V3{0, 0, 1}, V3{1, 1, 0}, 50, 5.0, 0.7);
    std::vector<f64> trace;
    for (i32 i = 0; i < 240; ++i) {
      sys.update(DT);
      if (i == 90) sys.impulse(V3{0.5, 0, 0}, 2.0, 6.0);
      for (const auto& g : sys.gibs) trace.insert(trace.end(), {g->pos.x, g->pos.y, g->pos.z, g->rot.x, g->rot.w});
    }
    for (const BloodStain& st : sys.stains) trace.insert(trace.end(), {st.pos.x, st.pos.y, st.size});
    return trace;
  };
  const std::vector<f64> a = run(5), b = run(5), c = run(6);
  CHECK(a == b);
  CHECK(a != c);  // (another seed: other spins and sprays)
}
