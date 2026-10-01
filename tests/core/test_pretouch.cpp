// Pre-touch (WorldConfig::pretouch_radius, docs/CORE.md §3 Streaming): the undesigned structures
// near the focus are designed before anything touches them, nearest first, a tick's work at most;
// what stands on nothing is left as it rests. Off by default.
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

IVec3 local(i32 i) { return {i >> 10, (i >> 5) & 31, i & 31}; }

// Rock up to z = 8; a concrete cantilever - a column (x 40..44, y 40..44, z 8..48) and a beam
// from it 4.6 m out at its top (x 44..81, z 44..48) - overloaded under its own weight until it is
// designed; and a wooden crate resting on the rock with seams round it (x 8..12, y 8..12, z 8..12).
bool crate(i32 x, i32 y, i32 z) { return x >= 8 && x < 12 && y >= 8 && y < 12 && z >= 8 && z < 12; }
Vox scene(i32 x, i32 y, i32 z) {
  if (z < 8) return kRock;
  if (x >= 40 && x < 44 && y >= 40 && y < 44 && z < 48) return kConcrete;
  if (x >= 44 && x < 81 && y >= 40 && y < 44 && z >= 44 && z < 48) return kConcrete;
  if (crate(x, y, z)) return kWood;
  return kAir;
}

class Yard final : public ChunkSource {
 public:
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    if (cc[2] < 0 || cc[2] > 1) return false;
    out.assign(kChunkVox, kAir);
    for (i32 i = 0; i < kChunkVox; ++i) {
      const IVec3 l = local(i);
      out[size_t(i)] = scene(cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]);
    }
    return true;
  }
  bool generate_seams(const IVec3& cc, std::vector<u8>& out) const override {
    out.assign(kChunkVox, 0);
    bool any = false;
    for (i32 i = 0; i < kChunkVox; ++i) {
      const IVec3 l = local(i);
      const IVec3 p{cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]};
      for (int a = 0; a < 3; ++a) {
        IVec3 q = p;
        q[a] += 1;
        if (crate(p[0], p[1], p[2]) != crate(q[0], q[1], q[2]) && vox_solid(scene(p[0], p[1], p[2])) && vox_solid(scene(q[0], q[1], q[2]))) {
          out[size_t(i)] = static_cast<u8>(out[size_t(i)] | (1u << a));
          any = true;
        }
      }
    }
    return any;
  }
  IVec3 chunk_lo() const override { return {-6, -6, -1}; }
  IVec3 chunk_hi() const override { return {6, 6, 3}; }
};

World yard(f64 pretouch) {
  World w;
  WorldConfig c = w.config();
  c.pretouch_radius = pretouch;
  w.configure(c);
  VoxelGrid g;
  g.h = kH;
  w.load(std::move(g));
  StreamConfig sc;
  sc.load_radius = 14.0;
  sc.evict_radius = 18.0;
  sc.chunks_per_tick = 4096;
  sc.archive_mb = 0.0;
  w.enable_streaming(std::make_shared<Yard>(), sc);
  w.set_focus(V3{3.0, 3.0, 1.0});
  return w;
}

}  // namespace

TEST_CASE("pretouch: off by default - nothing is extracted until something touches it") {
  World w = yard(0.0);
  for (int t = 0; t < 30; ++t) w.tick();
  CHECK(w.stats().extractions == 0);
  CHECK(w.stats().strengthened_voxels == 0);
  // the first shot designs it in its tick
  w.shoot(V3{70 * kH, 41 * kH, 46 * kH}, 0.1, 50.0);
  w.tick();
  CHECK(w.stats().extractions > 0);
  CHECK(w.stats().strengthened_voxels > 0);
}

TEST_CASE("pretouch: the structures near the focus are designed before anything touches them; a resting crate is left") {
  World w = yard(48.0);
  i64 pieces = 0;
  for (int t = 0; t < 30; ++t) {
    w.tick();
    for (const WorldEvent& e : w.take_events()) pieces += e.kind == WorldEvent::Kind::PieceAdded;
  }
  CHECK(w.stats().extractions > 0);
  CHECK(w.stats().strengthened_voxels > 0);
  CHECK(pieces == 0);  // (the cantilever stands, designed; the crate rests)
  CHECK(w.grid().get(10, 10, 10) == kWood);
  CHECK(w.grid().get(78, 41, 46) == kConcrete);
  // a shot at the beam finds it designed: nothing is designed in its tick
  const i64 strengthened = w.stats().strengthened_voxels;
  w.shoot(V3{70 * kH, 41 * kH, 46 * kH}, 0.1, 50.0);
  w.tick();
  CHECK(w.stats().strengthened_voxels == strengthened);
}

TEST_CASE("pretouch: the same commands give the same world") {
  World a = yard(48.0), b = yard(48.0);
  for (int t = 0; t < 40; ++t) {
    if (t == 20) {
      a.shoot(V3{60 * kH, 41 * kH, 46 * kH}, 0.2, 400.0);
      b.shoot(V3{60 * kH, 41 * kH, 46 * kH}, 0.2, 400.0);
    }
    a.tick();
    b.tick();
  }
  CHECK(a.session_hash() == b.session_hash());
  CHECK(a.stats().extractions == b.stats().extractions);
}
