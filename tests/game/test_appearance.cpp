// The appearance table (svx/game/appearance.hpp): a source's looks reach the meshes as texture
// ids 0xC000 + i - the world's chunks, its oriented grids' and the pieces that come off them.
#include <cmath>
#include <memory>
#include <set>
#include <vector>

#include "doctest.h"
#include "svx/game/appearance.hpp"
#include "svx/game/game.hpp"

using namespace svx;

namespace {

constexpr f64 kH = 0.125;

// Voxel i of chunk cc (Chunk::v order: (x * 32 + y) * 32 + z).
IVec3 voxel_of(const IVec3& cc, i32 i) {
  return {cc[0] * kChunk + (i >> (2 * kChunkBits)), cc[1] * kChunk + ((i >> kChunkBits) & (kChunk - 1)), cc[2] * kChunk + (i & (kChunk - 1))};
}

Appearance shade(f32 v, f32 opacity = 1.0f) {
  Appearance a;
  a.rgb[0] = a.rgb[1] = a.rgb[2] = v;
  a.opacity = opacity;
  return a;
}

// Rock below z 0 (anchored), a concrete pillar 1 m square and 3 m high on it at the origin; looks:
// the rock's 1, the pillar's 1 + (z / 8) % 2 - and the rows of x = 7 none (0). One chunk column.
class LookSource final : public GameSource {
 public:
  LookSource() {
    auto t = std::make_shared<AppearanceTable>();
    rock_ = t->set(static_cast<int>(MaterialId::Rock), 1, shade(0.2f));
    lo_ = t->set(static_cast<int>(MaterialId::Concrete), 1, shade(0.4f));
    hi_ = t->set(static_cast<int>(MaterialId::Concrete), 2, shade(0.6f, 0.3f));
    table_ = std::move(t);
  }
  int rock_ = -1, lo_ = -1, hi_ = -1;
  std::shared_ptr<const AppearanceTable> table_;

  static Vox at(const IVec3& p) {
    if (p[0] < -32 || p[0] >= 32 || p[1] < -32 || p[1] >= 32) return kAir;
    if (p[2] < 0 && p[2] >= -8) return make_vox(MaterialId::Rock, true);
    if (p[2] >= 0 && p[2] < 24 && p[0] >= 0 && p[0] < 8 && p[1] >= 0 && p[1] < 8) return make_vox(MaterialId::Concrete, false);
    return kAir;
  }
  static u8 look(const IVec3& p) {
    const Vox v = at(p);
    if (v == kAir || p[0] == 7) return 0;
    if (vox_mat(v) == MaterialId::Rock) return 1;
    return static_cast<u8>(1 + (p[2] / 8) % 2);
  }

  IVec3 chunk_lo() const override { return {-1, -1, -1}; }
  IVec3 chunk_hi() const override { return {1, 1, 1}; }
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    out.assign(kChunkVox, kAir);
    bool any = false;
    for (i32 i = 0; i < kChunkVox; ++i) {
      out[size_t(i)] = at(voxel_of(cc, i));
      any = any || out[size_t(i)] != kAir;
    }
    return any;
  }
  bool generate_layer(const IVec3& cc, const std::string& layer, std::vector<u8>& out) const override {
    if (layer != "look") return false;
    out.assign(kChunkVox, 0);
    bool any = false;
    for (i32 i = 0; i < kChunkVox; ++i) {
      out[size_t(i)] = look(voxel_of(cc, i));
      any = any || out[size_t(i)] != 0;
    }
    return any;
  }
  V3 spawn_pos() const override { return {-2.0, -2.0, 0.0}; }
  V3 spawn_dir() const override { return {1, 0, 0}; }
  std::shared_ptr<const AppearanceTable> appearances() const override { return table_; }
};

std::set<u16> textures_of(const std::vector<MeshVertex>& vs) {
  std::set<u16> out;
  for (const MeshVertex& v : vs) out.insert(v.texture);
  return out;
}

}  // namespace

TEST_CASE("appearance: a table maps (material, look) to its appearances, look 0 to none unless given") {
  AppearanceTable t;
  CHECK(t.find(1, 0) == -1);
  const int a = t.set(1, 3, shade(0.5f));
  const int b = t.set(1, 4, shade(0.25f, 0.5f));
  CHECK(a == 0);
  CHECK(b == 1);
  CHECK(t.set(1, 3, shade(0.75f)) == 0);  // (the same pair keeps its index)
  CHECK(t.size() == 2);
  CHECK(t[0].rgb[0] == doctest::Approx(0.75f));
  CHECK(t.find(1, 3) == 0);
  CHECK(t.find(1, 0) == -1);
  CHECK(t.find(2, 3) == -1);
  CHECK(t.find(200, 3) == -1);
  CHECK(t.set(-1, 0, shade(0.1f)) == -1);
  CHECK(t.set(kMaxMaterials, 0, shade(0.1f)) == -1);
  CHECK(t.texture(1, 4, 0xFF01) == kAppearanceTexture + 1);
  CHECK(t.texture(1, 5, 0xFF01) == 0xFF01);
  const std::vector<f32> p = t.packed();
  REQUIRE(p.size() == 2 * kAppearanceFloats);
  CHECK(p[kAppearanceFloats + 3] == doctest::Approx(0.5f));
  CHECK(srgb_to_linear(0) == 0.0f);
  CHECK(srgb_to_linear(255) == doctest::Approx(1.0f));
  CHECK(srgb_to_linear(128) == doctest::Approx(0.2158605f).epsilon(1e-5));
}

TEST_CASE("appearance: a streamed source's looks reach its chunk meshes and the pieces that come off them") {
  auto src = std::make_shared<LookSource>();
  Game g;
  g.load_streaming(src, kH, StreamConfig{});
  REQUIRE(g.appearances() != nullptr);
  REQUIRE(g.look_layer() >= 0);
  CHECK(g.world().layer_index("look") == g.look_layer());
  g.set_viewer({-2.0, -2.0, 1.6});
  std::set<u16> seen;
  for (int t = 0; t < 30; ++t) {
    g.tick();
    for (const ChunkMesh& m : g.take_meshes({})) {
      const std::set<u16> s = textures_of(m.vertices);
      seen.insert(s.begin(), s.end());
    }
  }
  CHECK(seen.count(static_cast<u16>(kAppearanceTexture + src->rock_)) == 1);
  CHECK(seen.count(static_cast<u16>(kAppearanceTexture + src->lo_)) == 1);
  CHECK(seen.count(static_cast<u16>(kAppearanceTexture + src->hi_)) == 1);
  // (no look: the material's colour)
  CHECK(seen.count(static_cast<u16>(0xFF00 + static_cast<u16>(MaterialId::Concrete))) == 1);
  CHECK(seen.count(static_cast<u16>(0xFF00 + static_cast<u16>(MaterialId::Rock))) == 1);
  // the look layer is not stored: regenerated on demand
  const Chunk* c = g.world().grid().chunk(IVec3{0, 0, 0});
  REQUIRE(c != nullptr);
  CHECK(c->layer[size_t(g.look_layer())].own() == false);
  CHECK(g.world().grid().layer(g.look_layer(), IVec3{1, 1, 9}) == 2);

  // cut the pillar at its foot: the piece keeps its looks
  g.carve({0.5, 0.5, 0.0625}, 0.75);
  bool detached = false;
  std::set<u16> piece;
  for (int t = 0; t < 120 && !detached; ++t) {
    g.tick();
    for (const GameEvent& e : g.take_events()) {
      if (e.kind != GameEvent::Kind::Detached || e.voxels < 200) continue;
      detached = true;
      const std::set<u16> s = textures_of(e.mesh.vertices);
      piece.insert(s.begin(), s.end());
    }
    (void)g.take_meshes({});
  }
  REQUIRE(detached);
  CHECK(piece.count(static_cast<u16>(kAppearanceTexture + src->lo_)) == 1);
  CHECK(piece.count(static_cast<u16>(kAppearanceTexture + src->hi_)) == 1);
  CHECK(piece.count(static_cast<u16>(0xFF00 + static_cast<u16>(MaterialId::Concrete))) == 1);
}

TEST_CASE("appearance: a new load drops the table, and a world without one meshes as before") {
  Game g;
  g.load_streaming(std::make_shared<LookSource>(), kH, StreamConfig{});
  REQUIRE(g.appearances() != nullptr);
  VoxelGrid w;
  w.h = kH;
  for (i32 x = 0; x < 8; ++x)
    for (i32 y = 0; y < 8; ++y) w.fill_column(x, y, 0, 8, make_vox(MaterialId::Concrete, true));
  w.compact();
  g.load(std::move(w), V3{0, 0, 2}, V3{1, 0, 0});
  CHECK(g.appearances() == nullptr);
  CHECK(g.look_layer() == -1);  // (the world keeps its layers across loads - as the environment's - unused here)
  g.tick();
  std::set<u16> seen;
  for (const ChunkMesh& m : g.take_meshes({})) {
    const std::set<u16> s = textures_of(m.vertices);
    seen.insert(s.begin(), s.end());
  }
  CHECK(seen == std::set<u16>{static_cast<u16>(0xFF00 + static_cast<u16>(MaterialId::Concrete))});
}

TEST_CASE("appearance: an oriented grid's looks come from its own look layer") {
  auto src = std::make_shared<LookSource>();
  Game g;
  g.load_streaming(src, kH, StreamConfig{});
  g.set_viewer({-2.0, -2.0, 1.6});
  for (int t = 0; t < 10; ++t) g.tick();
  (void)g.take_meshes({});
  // a turned concrete block (as a source's generate_grid would make it): look 2 but its top layer
  VoxelGrid b;
  b.h = kH;
  const int L = b.add_layer({"look", true, LayerBind::Solid});
  for (i32 x = 0; x < 8; ++x)
    for (i32 y = 0; y < 8; ++y)
      for (i32 z = 0; z < 8; ++z) {
        b.set(IVec3{x, y, z}, make_vox(MaterialId::Concrete, false));
        if (z < 7) b.set_layer(L, IVec3{x, y, z}, 2);
      }
  b.compact();
  const Quat rot{0.0, 0.0, std::sin(0.3), std::cos(0.3)};
  const GridId id = g.world().add_grid(GridFrame{V3{-3.0, 2.0, 0.5}, rot}, std::move(b));
  REQUIRE(id != 0);
  std::set<u16> seen;
  for (int t = 0; t < 3; ++t) {
    g.tick();
    for (const ChunkMesh& m : g.take_meshes({}))
      if (m.grid == id) {
        const std::set<u16> s = textures_of(m.vertices);
        seen.insert(s.begin(), s.end());
      }
  }
  CHECK(seen.count(static_cast<u16>(kAppearanceTexture + src->hi_)) == 1);
  CHECK(seen.count(static_cast<u16>(0xFF00 + static_cast<u16>(MaterialId::Concrete))) == 1);
}
