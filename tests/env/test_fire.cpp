// Fire (svx_env): burning, heat, and what they do to structures, on the core's extension points.
#include <memory>
#include <vector>

#include "doctest.h"
#include "svx/env/fire.hpp"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

constexpr f64 kH = 0.125;
const Vox kRock = make_vox(MaterialId::Rock, true);
const Vox kWood = make_vox(MaterialId::Wood, false);
const Vox kSteel = make_vox(MaterialId::Steel, false);

void box(VoxelGrid& g, const IVec3& lo, const IVec3& hi, Vox v) {
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y) g.fill_column(x, y, lo[2], hi[2], v);
}

// A rock plate and wall, a beam (3 x 3 voxels, 2 m) out of the wall.
VoxelGrid cantilever(Vox beam) {
  VoxelGrid g;
  g.h = kH;
  box(g, {-8, -8, -4}, {40, 16, 0}, kRock);
  box(g, {-8, -8, 0}, {0, 16, 40}, kRock);
  box(g, {0, 0, 24}, {16, 3, 27}, beam);
  g.compact();
  return g;
}

// A wooden wall (1 voxel thick, 3 m x 3 m) on the plate.
VoxelGrid plank_wall() {
  VoxelGrid g;
  g.h = kH;
  box(g, {-8, -8, -4}, {40, 16, 0}, kRock);
  box(g, {0, 4, 0}, {24, 5, 24}, kWood);
  g.compact();
  return g;
}

struct Fire {
  World w;
  std::shared_ptr<FireSystem> fire = std::make_shared<FireSystem>();
  explicit Fire(VoxelGrid g, const FireConfig& c = {}) {
    fire->configure(c);
    w.add_system(fire);
    w.load(std::move(g));
    w.bake();
  }
  void run(f64 seconds) {
    const int n = static_cast<int>(seconds / w.config().dt + 0.5);
    for (int t = 0; t < n; ++t) w.tick();
  }
  i32 solid(const IVec3& lo, const IVec3& hi) const {
    i32 n = 0;
    for (i32 x = lo[0]; x < hi[0]; ++x)
      for (i32 y = lo[1]; y < hi[1]; ++y)
        for (i32 z = lo[2]; z < hi[2]; ++z) n += vox_solid(w.grid().get(x, y, z)) ? 1 : 0;
    return n;
  }
};

V3 at(const IVec3& p) { return {kH * p[0], kH * p[1], kH * p[2]}; }

}  // namespace

TEST_CASE("fire: a wooden beam burns at its root, chars and falls") {
  Fire f(cantilever(kWood));
  FireMaterial wood = f.fire->fire_material(MaterialId::Wood);
  wood.burn_s = 12.0;  // (a quick fire: the test's time)
  f.fire->set_material(MaterialId::Wood, wood);
  f.run(1.0);
  REQUIRE(f.w.pieces().empty());
  f.fire->ignite(f.w, at({1, 1, 23}), 0.3);
  f.run(1.0);
  CHECK(f.fire->stats().burning > 0);
  CHECK_FALSE(f.fire->flames().empty());
  CHECK(f.w.layer(World::kDamageLayer, {1, 1, 24}) > 0);  // (charring)
  bool fell = false;
  for (int s = 0; s < 120 && !fell; ++s) {
    f.run(1.0);
    fell = !f.w.pieces().empty();
  }
  CHECK(fell);
  CHECK(f.fire->stats().burnt_out > 0);
  CHECK(f.fire->stats().ignited > 0);
}

TEST_CASE("fire: flames climb a wall, hardly descend, and burn out") {
  Fire f(plank_wall());
  f.fire->ignite(f.w, at({12, 4, 8}), 0.2);
  f.run(10.0);
  i32 up = 0, down = 0;
  for (i32 x = 0; x < 24; ++x)
    for (i32 z = 0; z < 24; ++z) {
      const u8 b = f.w.layer(f.fire->burn_layer(), {x, 4, z});
      if (!b && vox_solid(f.w.grid().get(x, 4, z))) continue;
      if (z > 9) ++up;
      if (z < 7) ++down;
    }
  CHECK(up > 3 * std::max(1, down));
  // nothing is left burning forever: the flames die when the fuel is gone
  for (int s = 0; s < 400 && f.fire->stats().hot > 0; ++s) f.run(1.0);
  CHECK(f.fire->stats().hot == 0);
  CHECK(f.fire->stats().burnt_out > 24 * 16 / 2);
  CHECK(f.solid({0, 4, 8}, {24, 5, 24}) < 24 * 16 / 4);
}

TEST_CASE("fire: steel weakens with heat, for good") {
  Fire f(cantilever(kSteel));
  f.fire->heat(f.w, at({2, 1, 25}), 0.3, 1000.0);
  f.run(0.2);
  const u8 d = f.w.layer(World::kDamageLayer, {2, 1, 25});
  CHECK(d > 150);
  CHECK(f.w.layer(f.fire->heat_layer(), {2, 1, 25}) > 0);
  f.run(60.0);  // (cold again)
  CHECK(f.w.layer(f.fire->heat_layer(), {2, 1, 25}) < 30);
  CHECK(f.w.layer(World::kDamageLayer, {2, 1, 25}) >= d);
  CHECK(f.fire->stats().burning == 0);  // (steel does not burn)
}

TEST_CASE("fire: water quenches, an extinguisher puts it out") {
  {
    Fire f(plank_wall());
    const int water = f.w.add_layer({"water", true});
    std::vector<LayerEdit> wet;
    for (i32 x = 8; x < 16; ++x)
      for (i32 z = 4; z < 12; ++z) wet.push_back({{x, 3, z}, 200});  // (a wet film in front of the planks)
    f.w.set_layer(water, wet);
    f.fire->ignite(f.w, at({12, 4, 8}), 0.2);
    f.run(3.0);
    CHECK(f.fire->stats().burning == 0);
    CHECK(f.fire->stats().burnt_out == 0);
  }
  {
    Fire f(plank_wall());
    f.fire->ignite(f.w, at({12, 4, 8}), 0.2);
    f.run(5.0);
    REQUIRE(f.fire->stats().burning > 0);
    f.fire->extinguish(f.w, at({12, 4, 12}), 3.0);
    f.run(0.2);
    CHECK(f.fire->stats().burning == 0);
    f.run(10.0);
    CHECK(f.fire->stats().burning == 0);
  }
}

TEST_CASE("fire: pieces burn, and heat the world they lie on") {
  Fire f(cantilever(kWood));
  FireMaterial wood = f.fire->fire_material(MaterialId::Wood);
  wood.burn_s = 8.0;
  f.fire->set_material(MaterialId::Wood, wood);
  // the beam comes off its wall and lands on the plate
  std::vector<VoxelEdit> cut;
  for (i32 y = 0; y < 3; ++y)
    for (i32 z = 24; z < 27; ++z) cut.push_back({{0, y, z}, kAir});
  f.w.set_voxels(cut);
  f.run(4.0);
  REQUIRE(f.w.pieces().size() == 1);
  const i64 id = f.w.pieces()[0].id;
  const i32 before = f.w.pieces()[0].voxels;
  f.fire->ignite(f.w, f.w.pieces()[0].pos, 0.4);
  f.run(2.0);
  CHECK(f.fire->stats().burning_pieces > 0);
  bool piece_flame = false;
  for (const auto& fl : f.fire->flames()) piece_flame |= fl.piece == id;
  CHECK(piece_flame);
  f.run(30.0);
  i32 after = 0;
  for (const PieceState& ps : f.w.pieces()) after += ps.voxels;
  CHECK(after < before);
}

TEST_CASE("fire: deterministic, and within its budget") {
  FireConfig c;
  c.max_hot = 300;
  Fire a(plank_wall(), c), b(plank_wall(), c);
  for (Fire* f : {&a, &b}) f->fire->ignite(f->w, at({12, 4, 8}), 0.3);
  for (int s = 0; s < 20; ++s) {
    a.run(0.5);
    b.run(0.5);
    CHECK(a.fire->stats().hot <= 300);
  }
  CHECK(a.fire->stats().dropped > 0);
  CHECK(a.fire->state_hash() == b.fire->state_hash());
  CHECK(a.w.state_hash() == b.w.state_hash());
}

namespace {

// Rock ground, a wooden hut (walls and a roof) in chunk (0, 0, 0).
class Woods final : public ChunkSource {
 public:
  bool generate(const IVec3& c, std::vector<Vox>& out) const override {
    out.assign(kChunkVox, kAir);
    if (c[2] == -1) {
      std::fill(out.begin(), out.end(), kRock);
      return true;
    }
    if (c[2] != 0 || c[0] != 0 || c[1] != 0) return false;
    for (i32 x = 4; x < 20; ++x)
      for (i32 y = 4; y < 20; ++y)
        for (i32 z = 0; z < 16; ++z) {
          const bool wall = x == 4 || x == 19 || y == 4 || y == 19;
          if (wall || z == 15) out[size_t(chunk_index({x, y, z}))] = kWood;
        }
    return true;
  }
  IVec3 chunk_lo() const override { return {-16, -16, -1}; }
  IVec3 chunk_hi() const override { return {16, 16, 2}; }
};

}  // namespace

TEST_CASE("fire: streamed: evicted chunks take their heat with them, the burns stay") {
  auto fire = std::make_shared<FireSystem>();
  World w;
  w.add_system(fire);
  VoxelGrid g;
  g.h = kH;
  w.load(std::move(g));
  StreamConfig sc;
  sc.load_radius = 12.0;
  sc.evict_radius = 16.0;
  sc.chunks_per_tick = 400;
  w.enable_streaming(std::make_shared<Woods>(), sc);
  w.set_focus(V3{0, 0, 0});
  for (int t = 0; t < 10; ++t) w.tick();
  REQUIRE(w.grid().get(4, 10, 5) == kWood);
  fire->ignite(w, at({4, 10, 5}), 0.3);
  for (int t = 0; t < 600; ++t) w.tick();
  CHECK(fire->stats().burning > 0);
  i32 burnt = 0;
  for (i32 z = 0; z < 16; ++z)
    for (i32 y = 4; y < 20; ++y) burnt += w.layer(fire->burn_layer(), {4, y, z}) > 0 ? 1 : 0;
  REQUIRE(burnt > 0);
  w.set_focus(V3{80, 0, 0});
  for (int t = 0; t < 10; ++t) w.tick();
  CHECK_FALSE(w.chunk_resident({0, 0, 0}));
  CHECK(fire->stats().hot == 0);
  CHECK(fire->stats().burning == 0);
  w.set_focus(V3{0, 0, 0});
  for (int t = 0; t < 10; ++t) w.tick();
  REQUIRE(w.chunk_resident({0, 0, 0}));
  i32 again = 0;
  for (i32 z = 0; z < 16; ++z)
    for (i32 y = 4; y < 20; ++y) again += w.layer(fire->burn_layer(), {4, y, z}) > 0 ? 1 : 0;
  CHECK(again > 0);  // (the burns came back with the chunk; the fire did not)
  CHECK(w.layer(fire->heat_layer(), {4, 10, 5}) == 0);
  CHECK(w.memory().systems > 0);
}
