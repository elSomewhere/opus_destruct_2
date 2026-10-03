// Layer storage (docs/CORE.md §5): one value over a whole chunk kept as one (a lake's water), and
// regenerable layers (LayerSpec::regenerable: a city's looks) whose base values come from the
// source when they are read and are stored only once play writes them.
#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "doctest.h"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

constexpr f64 kH = 0.125;
const Vox kRock = make_vox(MaterialId::Rock, true);
const Vox kStone = make_vox(MaterialId::Stone, false);

IVec3 local(i32 i) { return {i >> 10, (i >> 5) & 31, i & 31}; }

// Rock ground (chunk z 0, up to voxel 8), a stone wall on it (x 40..44, y 0..64, z 8..40) in
// chunks z 0 and 1; a lake's water filling chunk z 2 of the columns x < 0 (one value, 255) and
// the air of chunk z 1 there above voxel 50 (a surface: values over part of a chunk). The wall's
// voxels have a look: a pattern of their place.
u8 look(i32 x, i32 y, i32 z) { return static_cast<u8>(1 + ((x * 7 + y * 13 + z * 5) & 127)); }
bool wall(i32 x, i32 y, i32 z) { return x >= 40 && x < 44 && y >= 0 && y < 64 && z >= 8 && z < 40; }

class Lakeside final : public ChunkSource {
 public:
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    if (cc[2] > 1) return false;
    out.assign(kChunkVox, kAir);
    bool any = false;
    for (i32 i = 0; i < kChunkVox; ++i) {
      const IVec3 l = local(i);
      const i32 x = cc[0] * kChunk + l[0], y = cc[1] * kChunk + l[1], z = cc[2] * kChunk + l[2];
      if (z < 8) out[size_t(i)] = kRock;
      else if (wall(x, y, z)) out[size_t(i)] = kStone;
      any = any || out[size_t(i)] != kAir;
    }
    return any;
  }
  bool generate_layer(const IVec3& cc, const std::string& layer, std::vector<u8>& out) const override {
    if (layer == "water") {
      if (cc[0] >= 0 || cc[2] < 1 || cc[2] > 2) return false;
      out.assign(kChunkVox, 0);
      for (i32 i = 0; i < kChunkVox; ++i)
        if (cc[2] == 2 || (cc[2] == 1 && local(i)[2] >= 18)) out[size_t(i)] = 255;
      return true;
    }
    if (layer == "look") {
      ++look_calls;
      out.assign(kChunkVox, 0);
      bool any = false;
      for (i32 i = 0; i < kChunkVox; ++i) {
        const IVec3 l = local(i);
        const i32 x = cc[0] * kChunk + l[0], y = cc[1] * kChunk + l[1], z = cc[2] * kChunk + l[2];
        if (wall(x, y, z)) {
          out[size_t(i)] = look(x, y, z);
          any = true;
        }
      }
      return any;
    }
    return false;
  }
  IVec3 chunk_lo() const override { return {-4, -4, -1}; }
  IVec3 chunk_hi() const override { return {4, 4, 4}; }
  mutable std::atomic<i64> look_calls{0};
};

struct Setup {
  World w;
  int water = -1, look = -1;
};

void streamed(Setup& s, std::shared_ptr<const ChunkSource> src, const V3& focus = V3{2.0, 2.0, 1.0}) {
  VoxelGrid g;
  g.h = kH;
  s.w.load(std::move(g));
  s.water = s.w.add_layer({"water", true, LayerBind::Air});
  LayerSpec lk{"look", true, LayerBind::Solid};
  lk.regenerable = true;
  s.look = s.w.add_layer(lk);
  StreamConfig sc;
  sc.load_radius = 14.0;
  sc.evict_radius = 18.0;
  sc.chunks_per_tick = 4096;
  sc.archive_mb = 0.0;
  s.w.enable_streaming(std::move(src), sc);
  s.w.set_focus(focus);
  for (int t = 0; t < 3; ++t) s.w.tick();
}

}  // namespace

TEST_CASE("layers: one value over a whole chunk is kept as one, and reads as the array did") {
  Setup s;
  streamed(s, std::make_shared<Lakeside>());
  const Chunk* full = s.w.grid().chunk({-1, 0, 2});
  REQUIRE(full != nullptr);
  CHECK(full->layer[size_t(s.water)].uniform());
  CHECK(full->layer[size_t(s.water)].memory_bytes() == 0);
  CHECK(full->layer_count[size_t(s.water)] == kChunkVox);
  CHECK(s.w.layer(s.water, {-20, 5, 70}) == 255);
  const LayerValues& lv = full->layer[size_t(s.water)];
  i64 sum = 0;
  for (u8 v : lv) sum += v;
  CHECK(sum == 255LL * kChunkVox);
  const Chunk* part = s.w.grid().chunk({-1, 0, 1});
  REQUIRE(part != nullptr);
  CHECK_FALSE(part->layer[size_t(s.water)].uniform());
  CHECK(s.w.layer(s.water, {-20, 5, 50}) == 255);
  CHECK(s.w.layer(s.water, {-20, 5, 40}) == 0);
  CHECK_FALSE(s.w.modified());
  // written: an array now, the rest as it was
  REQUIRE(s.w.set_layer(s.water, {{{-20, 5, 70}, 100}}) == 1);
  CHECK_FALSE(full->layer[size_t(s.water)].uniform());
  CHECK(s.w.layer(s.water, {-20, 5, 70}) == 100);
  CHECK(s.w.layer(s.water, {-21, 5, 70}) == 255);
  // a saved session restores it; the hash is the same
  const std::vector<u8> delta = s.w.save_delta();
  Setup t;
  streamed(t, std::make_shared<Lakeside>());
  REQUIRE(t.w.load_delta(delta));
  CHECK(t.w.layer(t.water, {-20, 5, 70}) == 100);
  CHECK(t.w.layer(t.water, {-21, 5, 70}) == 255);
  CHECK(t.w.state_hash() == s.w.state_hash());
}

TEST_CASE("layers: a regenerable layer reads the source's values and stores none until play writes it") {
  auto src = std::make_shared<Lakeside>();
  Setup s;
  streamed(s, src);
  const Chunk* c = s.w.grid().chunk({1, 0, 0});
  REQUIRE(c != nullptr);
  CHECK(c->layer[size_t(s.look)].empty());
  CHECK_FALSE(c->layer[size_t(s.look)].own());
  CHECK(src->look_calls == 0);  // (nothing read them yet)
  for (i32 z = 8; z < 40; z += 3)
    for (i32 y = 0; y < 64; y += 7) CHECK(s.w.layer(s.look, {41, y, z}) == look(41, y, z));
  CHECK(src->look_calls > 0);
  CHECK(s.w.layer(s.look, {30, 5, 10}) == 0);  // (air)
  CHECK_FALSE(s.w.modified());
  // read from four threads at once: the same values (one after another where there are no threads:
  // WASM without pthreads)
  std::atomic<i64> wrong{0};
  auto reads = [&](int k) {
    for (i32 z = 8 + k; z < 40; z += 2)
      for (i32 y = 0; y < 64; ++y)
        if (s.w.layer(s.look, {42, y, z}) != look(42, y, z)) ++wrong;
  };
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
  for (int k = 0; k < 4; ++k) reads(k);
#else
  std::vector<std::thread> th;
  for (int k = 0; k < 4; ++k) th.emplace_back(reads, k);
  for (auto& t : th) t.join();
#endif
  CHECK(wrong == 0);
  // written in play: the chunk's own values now (the others kept), a change
  REQUIRE(s.w.set_layer(s.look, {{{41, 3, 20}, 200}}) == 1);
  CHECK(c->layer[size_t(s.look)].own());
  CHECK(s.w.layer(s.look, {41, 3, 20}) == 200);
  CHECK(s.w.layer(s.look, {41, 4, 20}) == look(41, 4, 20));
  CHECK(s.w.modified());
  // out of range and back: the written value comes back with the chunk
  s.w.set_focus(V3{-40.0, -40.0, 1.0});
  for (int t = 0; t < 20; ++t) s.w.tick();
  REQUIRE(s.w.grid().chunk({1, 0, 0}) == nullptr);
  s.w.set_focus(V3{2.0, 2.0, 1.0});
  for (int t = 0; t < 20; ++t) s.w.tick();
  CHECK(s.w.layer(s.look, {41, 3, 20}) == 200);
  CHECK(s.w.layer(s.look, {41, 4, 20}) == look(41, 4, 20));
  // a saved session restores it
  const std::vector<u8> delta = s.w.save_delta();
  Setup t;
  streamed(t, src);
  REQUIRE(t.w.load_delta(delta));
  CHECK(t.w.layer(t.look, {41, 3, 20}) == 200);
  CHECK(t.w.layer(t.look, {41, 4, 20}) == look(41, 4, 20));
  const Chunk* a = s.w.grid().chunk({1, 0, 0});
  const Chunk* b = t.w.grid().chunk({1, 0, 0});
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  CHECK(b->layer[size_t(t.look)].own());
  CHECK(std::equal(a->layer[size_t(s.look)].begin(), a->layer[size_t(s.look)].end(), b->layer[size_t(t.look)].begin()));
}

TEST_CASE("layers: a regenerable solid-bound value goes with its voxel, and is not the next voxel's") {
  Setup s;
  streamed(s, std::make_shared<Lakeside>());
  // a voxel cut out of the wall: its value is not read any more; a voxel placed there has none
  REQUIRE(s.w.set_voxels({{{42, 10, 30}, kAir}}) == 1);
  CHECK(s.w.grid().chunk({1, 0, 0})->layer[size_t(s.look)].own() == false);  // (a removal stores nothing)
  REQUIRE(s.w.set_voxels({{{42, 10, 30}, kStone}}) == 1);
  CHECK(s.w.layer(s.look, {42, 10, 30}) == 0);
  CHECK(s.w.layer(s.look, {42, 11, 30}) == look(42, 11, 30));
  // a piece cut off the wall's top carries its voxels' looks
  std::vector<VoxelEdit> cut;
  for (i32 x = 40; x < 44; ++x)
    for (i32 y = 0; y < 64; ++y) cut.push_back({{x, y, 34}, kAir});
  REQUIRE(s.w.set_voxels(cut) == 4 * 64);
  i64 piece = 0;
  for (int t = 0; t < 10 && piece == 0; ++t) {
    s.w.tick();
    for (const WorldEvent& e : s.w.take_events())
      if (e.kind == WorldEvent::Kind::PieceAdded) piece = e.id;
  }
  REQUIRE(piece != 0);
  const Body* b = s.w.piece(piece);
  REQUIRE(b != nullptr);
  const BodyShape& S = b->shapes[0];
  REQUIRE_FALSE(S.layer[size_t(s.look)].empty());
  i64 checked = 0, wrong = 0;
  for (i32 x = 40; x < 44; ++x)
    for (i32 y = 0; y < 64; ++y)
      for (i32 z = 35; z < 40; ++z) {
        const i32 i = S.index({x, y, z});
        if (i < 0 || !vox_solid(S.vox[size_t(i)])) continue;
        ++checked;
        wrong += S.layer[size_t(s.look)][size_t(i)] != look(x, y, z);
      }
  CHECK(checked > 1000);
  CHECK(wrong == 0);
}
