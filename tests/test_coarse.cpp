// Coarse connectivity of non-resident chunks (plan §B4 / §B8): detachment searches that cross
// the streaming boundary decide exactly what a fine search of the whole world decides.
#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "doctest.h"
#include "svx/engine/engine.hpp"
#include "svx/world/coarse.hpp"
#include "svx/world/region.hpp"
#include "svx/world/streaming.hpp"

using namespace svx;

namespace {

struct Lcg {
  u64 s;
  u32 next() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<u32>(s >> 33);
  }
  i32 range(i32 lo, i32 hi) { return lo + static_cast<i32>(next() % u32(hi - lo)); }  // [lo, hi)
};

// The rest of a full grid seen from a partial one: chunks in `resident` are the partial grid's,
// the others are summarized from the full grid.
class FullCoarse final : public CoarseWorld {
 public:
  FullCoarse(const VoxelGrid& full, const std::set<u64>& resident, const IVec3& clo, const IVec3& chi)
      : full_(full), resident_(resident), clo_(clo), chi_(chi) {}
  bool resident(const IVec3& cc) const override {
    for (int q = 0; q < 3; ++q)
      if (cc[q] < clo_[q] || cc[q] >= chi_[q]) return true;
    return resident_.count(key3(cc[0], cc[1], cc[2])) > 0;
  }
  const ChunkSummary* summary(const IVec3& cc) override {
    const u64 k = key3(cc[0], cc[1], cc[2]);
    auto it = cache_.find(k);
    if (it == cache_.end()) it = cache_.emplace(k, summarize_chunk(full_, cc)).first;
    return &it->second;
  }

 private:
  const VoxelGrid& full_;
  const std::set<u64>& resident_;
  IVec3 clo_, chi_;
  std::map<u64, ChunkSummary> cache_;
};

// Removes every solid voxel not connected to an anchored one (the search's invariant: before a
// change, everything stands).
void remove_unsupported(VoxelGrid& g, const IVec3& lo, const IVec3& hi) {
  std::set<u64> seen;
  std::vector<IVec3> queue;
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y)
      for (i32 z = lo[2]; z < hi[2]; ++z)
        if (vox_anchored(g.get(x, y, z))) {
          queue.push_back({x, y, z});
          seen.insert(key3(x, y, z));
        }
  for (size_t h = 0; h < queue.size(); ++h) {
    const IVec3 p = queue[h];
    for (int a = 0; a < 3; ++a)
      for (int s = -1; s <= 1; s += 2) {
        IVec3 q = p;
        q[a] += s;
        if (!g.bond(s > 0 ? p : q, a) || !seen.insert(key3(q[0], q[1], q[2])).second) continue;
        queue.push_back(q);
      }
  }
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y)
      for (i32 z = lo[2]; z < hi[2]; ++z)
        if (vox_solid(g.get(x, y, z)) && !seen.count(key3(x, y, z))) g.set(x, y, z, kAir);
}

}  // namespace

TEST_CASE("coarse: summaries hold a chunk's components, anchorage and face labels") {
  VoxelGrid g;
  const Vox c = make_vox(MaterialId::Concrete, false), r = make_vox(MaterialId::Rock, true);
  // two bars along x through chunk (0,0,0): one rests on rock inside the chunk, one does not;
  // the second has a broken bond at its + x end (the face label there is 0)
  for (i32 x = 0; x < kChunk; ++x) {
    g.set(x, 4, 4, c);
    g.set(x, 20, 20, c);
  }
  g.set(10, 4, 3, r);
  g.break_bond({kChunk - 1, 20, 20}, 0);
  g.crack_bond({5, 20, 20}, 0);  // cracked still connects
  const ChunkSummary S = summarize_chunk(g, {0, 0, 0});
  REQUIRE(S.components() == 2);
  const u16 a = S.label(0, 4, 4), b = S.label(0, 20, 20);  // - x face, (y, z)
  REQUIRE(a != 0);
  REQUIRE(b != 0);
  CHECK(a != b);
  CHECK(S.anchored[size_t(a - 1)] == 1);
  CHECK(S.anchored[size_t(b - 1)] == 0);
  CHECK(S.voxels[size_t(a - 1)] == u32(kChunk));
  CHECK(S.voxels[size_t(b - 1)] == u32(kChunk));
  CHECK(S.label(1, 4, 4) == a);   // + x face: bond to the next chunk intact
  CHECK(S.label(1, 20, 20) == 0);  // broken: no connection across
  CHECK(S.face[2].empty());        // - y face: all air
  CHECK(S.label(4, 10, 4) == 0);   // - z face at (x 10, y 4, z 0): air
  const ChunkSummary E = summarize_chunk(g, {5, 5, 5});
  CHECK(E.components() == 0);
  CHECK(E.bytes() > 0);
}

TEST_CASE("coarse: searches over non-resident summaries decide like the whole world (random)") {
  const IVec3 clo{0, 0, 0}, chi{4, 3, 2};
  const IVec3 lo{0, 0, 0}, hi{chi[0] * kChunk, chi[1] * kChunk, chi[2] * kChunk};
  Lcg rng{12345};
  VoxelGrid full;
  full.lo = lo;
  full.hi = hi;
  const Vox conc = make_vox(MaterialId::Concrete, false), rock = make_vox(MaterialId::Rock, true);
  // a frame of thin members grown from a few anchored blocks (each member starts on existing
  // structure), crossing chunk faces, with breaks and cracks: cuts split it into pieces, many
  // of them reaching other chunks
  std::vector<IVec3> solid;
  for (int k = 0; k < 6; ++k) {
    const IVec3 a{rng.range(lo[0], hi[0] - 6), rng.range(lo[1], hi[1] - 6), rng.range(lo[2], hi[2] - 6)};
    for (i32 x = a[0]; x < a[0] + 6; ++x)
      for (i32 y = a[1]; y < a[1] + 6; ++y)
        for (i32 z = a[2]; z < a[2] + 6; ++z) {
          full.set(x, y, z, rock);
          solid.push_back({x, y, z});
        }
  }
  for (int k = 0; k < 260; ++k) {
    const IVec3 s0 = solid[size_t(rng.range(0, static_cast<i32>(solid.size())))];
    const int ax = rng.range(0, 3), dir = rng.range(0, 2) ? 1 : -1;
    const i32 len = rng.range(16, 100), w = rng.range(1, 3);
    for (i32 t = 0; t < len; ++t)
      for (i32 u = 0; u < w; ++u)
        for (i32 v = 0; v < w; ++v) {
          IVec3 p = s0;
          p[ax] += dir * t;
          p[(ax + 1) % 3] += u;
          p[(ax + 2) % 3] += v;
          bool in = true;
          for (int q = 0; q < 3; ++q) in = in && p[q] >= lo[q] && p[q] < hi[q];
          if (!in || vox_solid(full.get(p))) continue;
          full.set(p, conc);
          solid.push_back(p);
        }
  }
  for (int k = 0; k < 300; ++k) {
    const IVec3 p{rng.range(lo[0], hi[0] - 1), rng.range(lo[1], hi[1] - 1), rng.range(lo[2], hi[2] - 1)};
    const int ax = rng.range(0, 3);
    if (rng.range(0, 3) == 0) full.crack_bond(p, ax);
    else full.break_bond(p, ax);
  }
  remove_unsupported(full, lo, hi);
  int differs_fine_only = 0, hydrated = 0, with_islands = 0;
  for (int trial = 0; trial < 120; ++trial) {
    // residency: about half the chunks
    std::set<u64> resident;
    std::vector<u64> keys;
    for (i32 x = clo[0]; x < chi[0]; ++x)
      for (i32 y = clo[1]; y < chi[1]; ++y)
        for (i32 z = clo[2]; z < chi[2]; ++z)
          if (rng.range(0, 2) == 0) {
            resident.insert(key3(x, y, z));
            keys.push_back(key3(x, y, z));
          }
    if (keys.empty()) continue;
    // a change inside a resident chunk: a ball of voxels removed (all its neighbours inside the
    // chunk too: every piece next to the change gets a seed, as the engine's seeds do)
    IVec3 c{0, 0, 0};
    bool found = false;
    for (int tries = 0; tries < 2000 && !found; ++tries) {
      const IVec3 cc = unkey3(keys[size_t(rng.range(0, static_cast<i32>(keys.size())))]);
      c = {cc[0] * kChunk + rng.range(6, kChunk - 6), cc[1] * kChunk + rng.range(6, kChunk - 6),
           cc[2] * kChunk + rng.range(6, kChunk - 6)};
      found = vox_solid(full.get(c)) && !vox_anchored(full.get(c));
    }
    if (!found) continue;
    const i32 rad = rng.range(2, 5);
    bool supports_removed = false;
    std::vector<IVec3> removed;
    for (i32 x = c[0] - rad; x <= c[0] + rad; ++x)
      for (i32 y = c[1] - rad; y <= c[1] + rad; ++y)
        for (i32 z = c[2] - rad; z <= c[2] + rad; ++z) {
          const i32 dx = x - c[0], dy = y - c[1], dz = z - c[2];
          if (dx * dx + dy * dy + dz * dz > rad * rad) continue;
          const Vox v = full.get(x, y, z);
          if (!vox_solid(v)) continue;
          supports_removed = supports_removed || vox_anchored(v);
          full.set(x, y, z, kAir);
          removed.push_back({x, y, z});
        }
    std::vector<IVec3> seeds;
    for (const IVec3& p : removed)
      for (int a = 0; a < 3; ++a)
        for (int s = -1; s <= 1; s += 2) {
          IVec3 q = p;
          q[a] += s;
          if (vox_solid(full.get(q))) seeds.push_back(q);
        }
    std::sort(seeds.begin(), seeds.end());
    seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());
    if (seeds.empty()) continue;
    // truth: the fine search over the whole world
    const auto truth = detached_islands_grid(full, seeds, supports_removed);
    // the partial world: resident chunks only, then non-resident summaries, hydrating the
    // chunks detached pieces reach into (as the engine does)
    std::vector<u64> rkeys(resident.begin(), resident.end());
    VoxelGrid part = full.snapshot(rkeys);
    const auto fine_only = detached_islands_grid(part, seeds, supports_removed);
    FullCoarse coarse(full, resident, clo, chi);
    std::vector<u64> far;
    ConnStats st;
    auto got = detached_islands_grid(part, seeds, supports_removed, &st, &coarse, &far);
    for (int round = 0; !far.empty() && round < 8; ++round) {
      for (u64 k : far) {
        REQUIRE(part.apply_record(full.chunk_record(k)));
        resident.insert(k);
        ++hydrated;
      }
      far.clear();
      got = detached_islands_grid(part, seeds, supports_removed, &st, &coarse, &far);
    }
    CHECK(far.empty());
    CHECK(got == truth);
    if (fine_only != truth) ++differs_fine_only;
    if (!truth.empty()) ++with_islands;
    // keep the invariant for the next trial: the detached pieces go
    for (const auto& isl : truth)
      for (const IVec3& p : isl) full.set(p, kAir);
  }
  MESSAGE("solid voxels " << full.solid_count() << ", trials with islands " << with_islands << ", where resident-only searches are wrong " << differs_fine_only
                                 << ", chunks hydrated " << hydrated);
  CHECK(with_islands > 5);
  CHECK(differs_fine_only > 0);  // the boundary matters in this world
  CHECK(hydrated > 0);           // and some pieces reached into non-resident chunks
}

namespace {

// A 32 m x 4 m x 4 m world: a steel bridge deck abutting anchored piers at both ends (its whole
// section bonded to them: cut in the middle, each half is a cantilever that stands), and beside it
// a steel cantilever from an anchored column in the middle to the far end.
class SpanSource final : public ChunkSource {
 public:
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    out.assign(kChunkVox, kAir);
    bool any = false;
    for (int i = 0; i < kChunkVox; ++i) {
      const IVec3 p{cc[0] * kChunk + i / (kChunk * kChunk), cc[1] * kChunk + (i / kChunk) % kChunk, cc[2] * kChunk + i % kChunk};
      out[size_t(i)] = at(p);
      any = any || out[size_t(i)] != kAir;
    }
    return any;
  }
  static Vox at(const IVec3& p) {
    // (steel: cut in the middle the halves are 15 m cantilevers, which plain concrete is not)
    const Vox rock = make_vox(MaterialId::Rock, true), conc = make_vox(MaterialId::Steel, false);
    const i32 x = p[0], y = p[1], z = p[2];
    if (y >= 12 && y < 16) {  // bridge: deck z [8, 16) between piers x < 8 and x >= 248 (to the deck's top)
      if (x < 8 || x >= 248) return z < 16 ? rock : kAir;
      if (z >= 8 && z < 16) return conc;
    }
    if (y >= 20 && y < 24) {  // cantilever: column x [128, 136), beam z [20, 28) to the end
      if (x >= 128 && x < 136 && z < 28) return rock;
      if (x >= 136 && z >= 20 && z < 28) return conc;
    }
    return kAir;
  }
  IVec3 chunk_lo() const override { return {0, 0, 0}; }
  IVec3 chunk_hi() const override { return {8, 1, 1}; }
  std::array<f64, 3> spawn_pos() const override { return {16.0, 2.0, 0.5}; }
  std::array<f64, 3> spawn_dir() const override { return {1.0, 0.0, 0.0}; }
};

}  // namespace

TEST_CASE("coarse: in a streamed world, structures anchored in evicted chunks stay connected, and pieces reaching them go whole") {
  Engine eng;
  // connectivity, not strength: the verification (which checks the halves as the 15 m cantilevers
  // they become - their interfaces to the rock piers do not hold them) is off here
  EngineConfig cfg = eng.config();
  cfg.verify = false;
  eng.configure(cfg);
  EngineParams p;
  p.fragility = 50.0;
  eng.set_params(p);
  auto src = std::make_unique<SpanSource>();
  const auto sp = src->spawn_pos();
  VoxelGrid g;
  eng.load(std::move(g), sp, src->spawn_dir());
  StreamConfig sc;
  sc.load_radius = 5.0;
  sc.evict_radius = 6.5;  // (chunk centres: 2 and 5 at 6.06 and 5.94 m, 1 and 6 at 10 m)
  sc.far_radius = 0.0;
  eng.enable_streaming(std::move(src), sc);
  eng.set_viewer(sp);
  for (int t = 0; t < 10; ++t) eng.tick();
  REQUIRE(eng.resident_chunks() == 4);  // chunks 2..5 (x 8..24 m); the piers and the tip are evicted
  const f64 h = 0.125;
  // cut the bridge deck in the middle: both halves hang from evicted piers
  eng.carve({16.0, 14 * h, 12 * h}, 0.7);
  for (int t = 0; t < 240; ++t) {
    eng.tick();
    (void)eng.take_events();
  }
  CHECK(!vox_solid(eng.grid().get(128, 14, 12)));
  CHECK(vox_solid(eng.grid().get(80, 14, 12)));   // x 10 m
  CHECK(vox_solid(eng.grid().get(176, 14, 12)));  // x 22 m
  CHECK(eng.stats().detached_voxels == 0);
  CHECK(eng.stats().coarse_summaries >= 2);
  CHECK(eng.stats().coarse_hydrated == 0);
  // cut the cantilever at its root: the rest, out to the evicted tip, detaches whole
  eng.carve({17.5, 22 * h, 24 * h}, 0.7);
  for (int t = 0; t < 240; ++t) {
    eng.tick();
    (void)eng.take_events();
  }
  CHECK(eng.stats().coarse_hydrated == 2);  // chunks 6 and 7 came in with the piece
  CHECK(eng.stats().detached_voxels > 3000);
  CHECK(!vox_solid(eng.grid().get(180, 22, 24)));
  // walk to the far end: the tip is gone there too, the deck is not
  eng.set_viewer({28.0, 2.0, 0.5});
  for (int t = 0; t < 20; ++t) eng.tick();
  CHECK(!vox_solid(eng.grid().get(240, 22, 24)));
  CHECK(vox_solid(eng.grid().get(240, 14, 12)));
}

namespace {

// A 32 m x 4 m x 4 m world: a steel beam (0.5 m wide, 0.25 m deep) cantilevers 18 m out of an
// anchored wall at x < 1 m and rests on an anchored prop at x 18 - 18.5 m. With the viewer at 16 m the wall and
// the beam's root are evicted; the prop is resident. Propped it stands; without the prop its root
// fails.
class ProppedSource final : public ChunkSource {
 public:
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    out.assign(kChunkVox, kAir);
    bool any = false;
    for (int i = 0; i < kChunkVox; ++i) {
      const IVec3 p{cc[0] * kChunk + i / (kChunk * kChunk), cc[1] * kChunk + (i / kChunk) % kChunk, cc[2] * kChunk + i % kChunk};
      out[size_t(i)] = at(p);
      any = any || out[size_t(i)] != kAir;
    }
    return any;
  }
  static Vox at(const IVec3& p) {
    // (supports of anchored steel: the beam is continuous with them, no rock-steel joint)
    const Vox rock = make_vox(MaterialId::Steel, true), steel = make_vox(MaterialId::Steel, false);
    const i32 x = p[0], y = p[1], z = p[2];
    if (y < 12 || y >= 16) return kAir;
    if (x < 8 && z < 28) return rock;                         // the wall
    if (x >= 144 && x < 148 && z < 20) return rock;          // the prop
    if (x >= 8 && x < 152 && z >= 20 && z < 22) return steel;  // the beam
    return kAir;
  }
  IVec3 chunk_lo() const override { return {0, 0, 0}; }
  IVec3 chunk_hi() const override { return {8, 1, 1}; }
  std::array<f64, 3> spawn_pos() const override { return {16.0, 2.0, 0.5}; }
  std::array<f64, 3> spawn_dir() const override { return {1.0, 0.0, 0.0}; }
};

struct PropRun {
  i64 verify_failures = 0, detached = 0, verifications = 0;
  bool tip_stands = true;
};

PropRun prop_run(bool hydrate, f64 fragility, bool take_prop = true) {
  Engine eng;
  EngineConfig cfg = eng.config();
  cfg.verify_hydrate = hydrate;
  eng.configure(cfg);
  EngineParams p;
  p.fragility = fragility;
  eng.set_params(p);
  auto src = std::make_unique<ProppedSource>();
  const auto sp = src->spawn_pos();
  eng.load(VoxelGrid{}, sp, src->spawn_dir());
  StreamConfig sc;
  sc.load_radius = 5.0;
  sc.evict_radius = 6.5;
  sc.far_radius = 0.0;
  eng.enable_streaming(std::move(src), sc);
  eng.set_viewer(sp);
  for (int t = 0; t < 200; ++t) {  // stream in and bake the resident part
    eng.tick();
    (void)eng.take_events();
  }
  REQUIRE(eng.resident_chunks() == 4);
  REQUIRE(vox_solid(eng.grid().get(146, 14, 21)));  // the beam over the prop
  // take the prop away under the beam: an 18 m cantilever from the evicted wall remains (or, as a
  // control, nick the beam's tip: a verification of the propped beam)
  const f64 h = 0.125;
  if (take_prop)
    for (i32 z = 12; z < 20; z += 3) eng.carve({146 * h, 14 * h, z * h}, 0.4);
  else
    eng.carve({151 * h, 14 * h, 21 * h}, 0.35);  // (more than a small event: a bubble, then its verification)
  for (int t = 0; t < 900; ++t) {
    eng.tick();
    (void)eng.take_events();
  }
  PropRun r;
  r.verify_failures = eng.stats().verify_failures;
  r.verifications = eng.stats().verifications;
  r.detached = eng.stats().detached_voxels;
  r.tip_stands = vox_solid(eng.grid().get(148, 14, 21));
  return r;
}

}  // namespace

TEST_CASE("coarse: a verification reaches into evicted chunks and catches a failure there") {
  const f64 frag = 1.0;
  const PropRun control = prop_run(true, frag, false);  // propped: verified whole, stands
  MESSAGE("propped (control): " << control.verifications << " verifications, " << control.verify_failures
                                << " failures, " << control.detached << " detached");
  CHECK(control.verifications >= 1);
  CHECK(control.verify_failures == 0);
  CHECK(control.tip_stands);
  const PropRun with = prop_run(true, frag);
  const PropRun without = prop_run(false, frag);
  MESSAGE("hydrated: " << with.verifications << " verifications, " << with.verify_failures << " failures, "
                       << with.detached << " detached, tip " << std::string(with.tip_stands ? "stands" : "gone")
                       << "; window only: " << without.verifications << " verifications, " << without.verify_failures
                       << " failures, " << without.detached << " detached, tip "
                       << std::string(without.tip_stands ? "stands" : "gone"));
  CHECK(with.verify_failures >= 1);
  CHECK_FALSE(with.tip_stands);
  CHECK(without.tip_stands);  // (the failure at the evicted root goes unseen)
}
