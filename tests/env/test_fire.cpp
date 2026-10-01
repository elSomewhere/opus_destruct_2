// Fire (svx_env): burning, heat, and what they do to structures, on the core's extension points.
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
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
  // a voxel's layer values, wherever it is now: in the world, or on the piece it left with
  auto at_voxel = [](const World& w, int L, const IVec3& p) -> u8 {
    if (const u8 v = w.layer(L, p)) return v;
    u8 best = 0;
    for (const PieceState& ps : w.pieces()) best = std::max(best, w.piece_layer(ps.id, L, p));
    return best;
  };
  {
    // a warm spot (500 degC): weaker, still standing
    Fire f(cantilever(kSteel));
    f.fire->heat(f.w, at({2, 1, 25}), 0.3, 500.0);
    f.run(0.2);
    const u8 d = f.w.layer(World::kDamageLayer, {2, 1, 25});
    CHECK(d > 20);
    CHECK(d < 100);
    f.run(60.0);  // (cold again)
    CHECK(f.w.layer(f.fire->heat_layer(), {2, 1, 25}) < 30);
    CHECK(f.w.layer(World::kDamageLayer, {2, 1, 25}) >= d);  // (for good)
    CHECK(f.w.pieces().empty());
  }
  {
    // red hot (1000 degC) at the root: nothing left of the section, the beam comes down
    Fire f(cantilever(kSteel));
    f.fire->heat(f.w, at({1, 1, 25}), 0.4, 1000.0);
    f.run(0.2);
    CHECK(at_voxel(f.w, World::kDamageLayer, {1, 1, 25}) > 200);
    f.run(3.0);
    CHECK_FALSE(f.w.pieces().empty());
    CHECK(f.fire->stats().burning == 0);  // (steel does not burn)
  }
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

// ---- hardening (audit regressions)

TEST_CASE("fire: the budget keeps the fire, not the cold edge; commands stay within it") {
  FireConfig c;
  c.max_hot = 150;
  Fire f(plank_wall(), c);
  f.fire->ignite(f.w, at({12, 4, 12}), 3.0);  // (a sphere of hundreds of voxels)
  CHECK(f.fire->stats().hot == 0);  // (counted at the next step)
  f.run(0.2);
  CHECK(f.fire->stats().hot <= 150);
  CHECK(f.fire->stats().dropped > 0);
  f.run(10.0);
  CHECK(f.fire->stats().burning > 20);  // (burning voxels are not let go for cooler ones)
  CHECK(f.fire->stats().hot <= 150);
}

TEST_CASE("fire: bad settings are brought into range; a world without room for its layers has no fire") {
  FireConfig c;
  c.max_hot = -5;
  c.step_s = std::nan("");
  c.quench_c = 1e9;
  c.damage_quantum = 0;
  FireSystem fs(c);
  CHECK(fs.config().max_hot == 0);
  CHECK(fs.config().step_s == doctest::Approx(0.1));
  CHECK(fs.config().quench_c <= 1020.0);
  CHECK(fs.config().damage_quantum == 1);
  FireMaterial m;
  m.char_damage = -3.0;
  m.flame_c = -100.0;
  fs.set_material(MaterialId::Wood, m);
  CHECK(fs.fire_material(MaterialId::Wood).char_damage == 0.0);
  CHECK(fs.fire_material(MaterialId::Wood).flame_c == 0.0);
  // (all eight layers taken: attached, but inert - no crash)
  World w;
  for (int k = 0; k < 7; ++k) w.add_layer({"l" + std::to_string(k), true});
  auto fire = std::make_shared<FireSystem>();
  w.add_system(fire);
  CHECK_FALSE(fire->ok());
  w.load(plank_wall());
  fire->ignite(w, at({12, 4, 8}), 0.5);
  for (int t = 0; t < 30; ++t) w.tick();
  CHECK(fire->stats().burning == 0);
  // positions out of range do nothing
  Fire g(plank_wall());
  g.fire->ignite(g.w, {1e12, -1e12, 5.0}, 1.0);
  g.fire->heat(g.w, {std::nan(""), 0.0, 0.0}, 1.0, 500.0);
  g.fire->extinguish(g.w, {0.0, 0.0, 1e300}, 1.0);
  g.run(0.5);
  CHECK(g.fire->stats().hot == 0);
}

TEST_CASE("fire: the same session from a load, whatever came before it") {
  auto session = [](FireSystem& fire, World& w) {
    w.load(plank_wall());
    fire.ignite(w, at({12, 4, 8}), 0.3);
    for (int t = 0; t < 300; ++t) w.tick();
    return w.session_hash();
  };
  World a, b;
  auto fa = std::make_shared<FireSystem>(), fb = std::make_shared<FireSystem>();
  a.add_system(fa);
  b.add_system(fb);
  // b burns something else first
  b.load(cantilever(kWood));
  fb->ignite(b, at({1, 1, 23}), 0.3);
  for (int t = 0; t < 200; ++t) b.tick();
  CHECK(session(*fa, a) == session(*fb, b));
}

TEST_CASE("fire: heat a level comes with is tracked; glow and pieces' changes are reported") {
  VoxelGrid g = cantilever(kSteel);
  const int heat = g.add_layer({"heat", false, LayerBind::Solid});
  for (i32 x = 4; x < 8; ++x) g.set_layer(heat, {x, 1, 25}, 200);  // (800 degC)
  Fire f(std::move(g));
  f.run(0.2);
  CHECK(f.fire->stats().hot >= 4);  // (stepped: it cools)
  f.fire->take_glow_changes();
  f.run(20.0);
  CHECK_FALSE(f.fire->take_glow_changes().empty());  // (it stopped glowing)
  CHECK(f.w.layer(f.fire->heat_layer(), {5, 1, 25}) < 100);
}

TEST_CASE("fire: an extinguisher cools the sphere of a long piece, not all of it") {
  Fire f(cantilever(kWood));
  std::vector<VoxelEdit> cut;
  for (i32 y = 0; y < 3; ++y)
    for (i32 z = 24; z < 27; ++z) cut.push_back({{0, y, z}, kAir});
  f.w.set_voxels(cut);
  f.run(4.0);
  REQUIRE(f.w.pieces().size() == 1);
  const i64 id = f.w.pieces()[0].id;
  f.fire->heat(f.w, f.w.pieces()[0].pos, 3.0, 400.0);  // (the whole beam)
  const Body* b = f.w.piece(id);
  const V3 tip = b->to_world(V3{kH * 15, kH * 1, kH * 25});
  f.fire->extinguish(f.w, tip, 0.3);
  // (the tip is cool, the other end still hot)
  CHECK(f.w.piece_layer(id, f.fire->heat_layer(), {15, 1, 25}) <= 15);
  CHECK(f.w.piece_layer(id, f.fire->heat_layer(), {2, 1, 25}) >= 90);
}

TEST_CASE("fire: a turned timber wall (an oriented grid) burns as one of the world's, and sets the world's on fire") {
  // a plank wall 3 m x 3 m of a grid turned 30 degrees, on the rock plate; beside it, a plank wall
  // of the world grid its flames reach
  VoxelGrid g;
  g.h = kH;
  box(g, {-40, -40, -4}, {40, 40, 0}, kRock);
  box(g, {-12, 3, 0}, {12, 4, 24}, kWood);  // (the world's wall, 0.4 m from the grid's)
  g.compact();
  Fire f(std::move(g));
  VoxelGrid wall;
  wall.h = kH;
  box(wall, {-12, 0, 0}, {12, 1, 24}, kWood);
  wall.compact();
  const f64 t = 0.5 * 30.0 * 3.14159265358979323846 / 180.0;
  const GridId id = f.w.add_grid(GridFrame{V3{0.0, 0.0, 0.0}, Quat{0.0, 0.0, std::sin(t), std::cos(t)}}, std::move(wall));
  REQUIRE(id != 0);
  const i64 n0 = f.w.grid(id)->solid_count();
  // lit at its foot
  f.fire->ignite(f.w, f.w.grid_to_world(id, V3{0.0, 0.0, kH}), 0.3);
  f.run(20.0);
  const FireSystem::Stats s = f.fire->stats();
  i64 burnt = 0, charred_high = 0;
  const VoxelGrid& G = *f.w.grid(id);
  for (i32 x = -12; x < 12; ++x)
    for (i32 z = 0; z < 24; ++z) {
      if (G.layer(f.fire->burn_layer(), {x, 0, z})) ++burnt;
      if (z > 12 && G.layer(f.fire->burn_layer(), {x, 0, z})) ++charred_high;
    }
  i64 world_burnt = 0;
  for (i32 x = -12; x < 12; ++x)
    for (i32 z = 0; z < 24; ++z) world_burnt += f.w.layer(f.fire->burn_layer(), {x, 3, z}) ? 1 : 0;
  MESSAGE("turned timber wall after 20 s: " << burnt << " of " << n0 << " voxels burning or burnt (" << charred_high
                                             << " above 1.5 m), " << n0 - G.solid_count() << " gone; the world's wall beside it: "
                                             << world_burnt << " voxels caught; grid hot " << s.grid_hot << ", burning " << s.grid_burning);
  CHECK(burnt > 40);
  CHECK(charred_high > 0);  // (it climbed)
  CHECK(world_burnt > 0);   // (across the lattices)
  // and put out
  f.fire->extinguish(f.w, f.w.grid_to_world(id, V3{0.0, 0.0, 1.5}), 4.0);
  f.run(1.0);
  CHECK(f.fire->stats().grid_burning == 0);
}

TEST_CASE("fire: a burning floor of the world sets a turned crate of a grid on it alight") {
  VoxelGrid g;
  g.h = kH;
  box(g, {-40, -40, -4}, {40, 40, 0}, kRock);
  box(g, {-16, -16, 0}, {16, 16, 1}, kWood);  // (a timber floor)
  g.compact();
  Fire f(std::move(g));
  VoxelGrid crate;
  crate.h = kH;
  box(crate, {-4, -4, 0}, {4, 4, 8}, kWood);
  crate.compact();
  const f64 t = 0.5 * 20.0 * 3.14159265358979323846 / 180.0;
  const GridId id = f.w.add_grid(GridFrame{V3{0.0, 0.0, kH}, Quat{0.0, 0.0, std::sin(t), std::cos(t)}}, std::move(crate));
  REQUIRE(id != 0);
  f.fire->ignite(f.w, V3{1.2, 0.0, 0.0}, 0.4);  // (the floor beside it)
  f.run(15.0);
  i64 caught = 0;
  const VoxelGrid& G = *f.w.grid(id);
  for (i32 x = -4; x < 4; ++x)
    for (i32 y = -4; y < 4; ++y)
      for (i32 z = 0; z < 8; ++z) caught += G.layer(f.fire->burn_layer(), {x, y, z}) ? 1 : 0;
  MESSAGE("a crate of a turned grid on a burning floor: " << caught << " voxels caught");
  CHECK(caught > 0);
}

TEST_CASE("fire: a tree's leaves (decorative) burn away; they never held anything") {
  World w;
  Material leaves;
  leaves.name = "leaves";
  leaves.rho = 80.0;
  leaves.decorative = true;
  leaves.passable = true;
  MaterialId lid{};
  REQUIRE(w.register_material(leaves, &lid));
  auto fire = std::make_shared<FireSystem>();
  FireMaterial burns = fire->fire_material(MaterialId::Wood);
  burns.burn_s = 3.0;
  burns.ignition_c = 200.0;
  fire->set_material(lid, burns);
  FireMaterial post = fire->fire_material(MaterialId::Wood);
  post.combustible = false;  // (the trunk does not burn here: what burns is the crown)
  fire->set_material(MaterialId::Wood, post);
  w.add_system(fire);
  VoxelGrid g;
  g.h = kH;
  box(g, {-16, -16, -4}, {32, 32, 0}, kRock);
  box(g, {10, 10, 0}, {12, 12, 24}, kWood);                       // a trunk
  box(g, {6, 6, 24}, {16, 16, 30}, make_vox(lid, false));         // its crown
  g.compact();
  w.load(std::move(g));
  w.bake();
  auto leaves_left = [&] {
    i32 n = 0;
    for (i32 x = 6; x < 16; ++x)
      for (i32 y = 6; y < 16; ++y)
        for (i32 z = 24; z < 30; ++z) n += w.grid().get(x, y, z) == make_vox(lid, false);
    return n;
  };
  REQUIRE(leaves_left() == 600);
  fire->ignite(w, at({11, 11, 26}), 0.6);
  for (int t = 0; t < 30 * 60 && leaves_left() > 0; ++t) w.tick();
  CHECK(leaves_left() < 60);
  CHECK(fire->stats().burnt_out > 0);
  CHECK(w.pieces().empty());                         // (no structure lost: leaves held nothing)
  CHECK(w.grid().get(11, 11, 20) == make_vox(MaterialId::Wood, false));  // (the trunk stands)
}
