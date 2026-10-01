// A district of the reference city in the engine (docs/CITY.md): the export's dump
// (tools/procgen_ref/district.mjs) behind a DistrictSource, streamed around its focus and ticked.
// The checks of voxel_city's own harness (scripts/svx-harness/harness.cpp) - the materials' ids,
// the district streaming in, every part a grid at its exported frame, the district standing under
// its own weight (touched decks too), the lanes on the road's surface - and the costs a merge
// cares about: a first touch (a shot, a blast) at a building.
//
//   svx_district_check FILE [--props structure|isolated|none] [--flora] [--threads N]
//                           [--load-radius M] [--touch shoot|blast|none] [--tune NAME=VALUE ...]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "svx/base/parallel.hpp"
#include "svx/game/materials.hpp"
#include "svx/procgen/district.hpp"
#include "svx/world/tunables.hpp"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

// Writes a district's isolated props as the export means them (kEditIsolated, after generation).
class PropWriter : public WorldSystem {
 public:
  explicit PropWriter(std::shared_ptr<const District> d) : d_(std::move(d)) {}
  const char* name() const override { return "district_props"; }
  void on_generated(World& w, const std::vector<u64>& chunks) override {
    for (u64 k : chunks) {
      auto e = district_props(*d_, unkey3(k));
      if (!e.empty()) pending_.insert(pending_.end(), e.begin(), e.end());
    }
  }
  void step(World& w, f64) override {
    if (pending_.empty()) return;
    w.set_voxels(pending_, kEditIsolated);
    pending_.clear();
  }

 private:
  std::shared_ptr<const District> d_;
  std::vector<VoxelEdit> pending_;
};

f64 now_ms() {
  using namespace std::chrono;
  return duration<f64, std::milli>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: svx_district_check FILE [--props structure|isolated|none] [--flora] [--threads N] [--load-radius M] [--touch shoot|blast|none] [--tune N=V]\n");
    return 2;
  }
  DistrictOptions opts;
  int threads = 4;
  f64 load_radius = 40.0;
  std::string touch = "shoot";
  std::vector<std::pair<std::string, f64>> tunes;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--props") {
      const std::string m = next();
      opts.props = m == "isolated" ? DistrictOptions::Props::Isolated : m == "none" ? DistrictOptions::Props::None : DistrictOptions::Props::Structure;
    } else if (a == "--flora") {
      opts.flora = true;
    } else if (a == "--threads") {
      threads = std::atoi(next().c_str());
    } else if (a == "--load-radius") {
      load_radius = std::atof(next().c_str());
    } else if (a == "--touch") {
      touch = next();
    } else if (a == "--tune") {
      const std::string t = next();
      const size_t eq = t.find('=');
      if (eq != std::string::npos) tunes.push_back({t.substr(0, eq), std::atof(t.c_str() + eq + 1)});
    }
  }
  auto d = std::make_shared<District>();
  if (!read_district(argv[1], d.get())) {
    std::fprintf(stderr, "svx_district_check: cannot read %s\n", argv[1]);
    return 2;
  }
  set_num_threads(threads);
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
  };
  // 1. materials: the game's (12..20), then the city's own at the ids the export gave them
  register_game_materials();
  bool ids = true;
  for (const District::Mat& m : d->mats) {
    MaterialId id{};
    // (plants - the export's foliage class - are decorative and passable: plan §8.3)
    Material mm = m.m;
    if (mm.name == "foliage") mm.decorative = mm.passable = true;
    const bool ok = register_material(mm, &id);
    if (!ok || static_cast<int>(id) != m.id) {
      std::printf("  %s: registered %d, the export says %d\n", m.m.name.c_str(), ok ? static_cast<int>(id) : -1, m.id);
      ids = false;
    }
  }
  check(ids, "the city's materials get the ids the export gave them (after the game's)");

  // 2. the world, streamed from the district around its focus
  World w;
  for (const auto& [n, v] : tunes)
    if (!set_tunable(w, n.c_str(), v)) std::fprintf(stderr, "unknown tunable %s\n", n.c_str());
  VoxelGrid g0;
  g0.h = d->h;
  w.load(std::move(g0));
  w.add_layer(LayerSpec{"look", true, LayerBind::Solid});
  w.add_layer(LayerSpec{"water", true, LayerBind::Air});
  if (opts.props == DistrictOptions::Props::Isolated) w.add_system(std::make_shared<PropWriter>(d));
  StreamConfig sc;
  sc.load_radius = load_radius;
  sc.evict_radius = load_radius + 20.0;
  sc.chunks_per_tick = 4096;
  sc.archive_mb = 16.0;
  auto src = std::make_shared<DistrictSource>(d, opts);
  w.enable_streaming(src, sc);
  const f64 t0 = now_ms();
  w.set_focus(d->focus);
  const f64 stream_ms = now_ms() - t0;
  int pieces = 0;
  f64 worst = 0;
  for (int t = 0; t < 240; ++t) {
    const f64 a = now_ms();
    w.tick();
    worst = std::max(worst, now_ms() - a);
    for (const WorldEvent& e : w.take_events())
      if (e.kind == WorldEvent::Kind::PieceAdded) ++pieces;
    w.take_changed_chunks();
  }
  const WorldStats s = w.stats();
  const MemoryReport mem = w.memory();
  std::printf("  streamed in %.0f ms; resident chunks %lld, voxels %lld, grids %d, structures %d, bodies %d (awake %d), pieces made %d, detached voxels %lld; worst tick %.1f ms; grid memory %.1f MB (%.1f KB a chunk)\n",
              stream_ms, static_cast<long long>(s.resident_chunks), static_cast<long long>(s.voxels), s.grids, s.structures, s.bodies, s.awake, pieces,
              static_cast<long long>(s.detached_voxels), worst, mem.grid / 1048576.0, mem.chunks ? mem.grid / 1024.0 / mem.chunks : 0.0);
  check(s.resident_chunks > 50, "the district streams in");

  // 3. the grids, where the export put them
  int placed = 0, expected = 0, frames = 0;
  for (const District::Grid& r : d->grids) {
    if (!w.chunk_resident(r.home)) continue;
    ++expected;
    GridFrame f;
    if (!w.grid_frame(r.g.id, &f)) continue;
    ++placed;
    const f64 dq = std::fabs(f.rot.x - r.g.rot.x) + std::fabs(f.rot.y - r.g.rot.y) + std::fabs(f.rot.z - r.g.rot.z) + std::fabs(f.rot.w - r.g.rot.w);
    const f64 dp = std::fabs(f.origin.x - r.g.origin.x) + std::fabs(f.origin.y - r.g.origin.y) + std::fabs(f.origin.z - r.g.origin.z);
    if (dq < 1e-12 && dp < 1e-9) ++frames;
  }
  std::printf("  grids at home in resident chunks %d, in the world %d, frames as exported %d\n", expected, placed, frames);
  check(placed == expected && frames == placed, "every part is a grid of the world, at its exported frame");

  // 4. it stands
  check(opts.props == DistrictOptions::Props::Isolated || (pieces == 0 && s.detached_voxels == 0),
        "the district stands under its own weight (no piece comes down)");
  if (!d->touches.empty()) {
    std::vector<VoxelLoad> loads;
    for (const V3& p : d->touches) {
      VoxelLoad l;
      l.voxel = IVec3{static_cast<i32>(std::floor(p.x / d->h + 0.5)), static_cast<i32>(std::floor(p.y / d->h + 0.5)), static_cast<i32>(std::floor(p.z / d->h + 0.5))};
      l.force = V3{0, 0, -15000.0};
      loads.push_back(l);
    }
    w.set_loads(1, loads);
    int after = 0;
    for (int t = 0; t < 240; ++t) {
      w.tick();
      for (const WorldEvent& e : w.take_events())
        if (e.kind == WorldEvent::Kind::PieceAdded) ++after;
      w.take_changed_chunks();
    }
    const WorldStats s2 = w.stats();
    std::printf("  touched %zu deck points: structures %d, design utilization %.2f, pieces made %d\n", d->touches.size(), s2.structures, s2.design_max_utilization, after);
    check(s2.structures > 0 && after == 0, "touched, its structures stand (designed under their own weight)");
    w.set_loads(1, {});
  }

  // 5. lanes on the road's surface
  std::set<u64> columns;
  for (const auto& [k, v] : d->chunks) {
    (void)v;
    const IVec3 c = unkey3(k);
    columns.insert(key3(c[0], c[1], 0));
  }
  int hit = 0, tried = 0;
  for (const auto& [a, b] : d->lanes) {
    const f64 len = std::hypot(b.x - a.x, b.y - a.y);
    for (f64 t = 0.5; t < len - 0.5; t += 2.0) {
      const f64 u = t / len;
      const V3 m{a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u};
      const IVec3 c{static_cast<i32>(std::floor((m.x / d->h + 0.5) / kChunk)), static_cast<i32>(std::floor((m.y / d->h + 0.5) / kChunk)),
                    static_cast<i32>(std::floor((m.z / d->h + 0.5) / kChunk))};
      if (!columns.count(key3(c[0], c[1], 0)) || !w.chunk_resident(c)) continue;
      ++tried;
      const RayHit h = w.raycast(V3{m.x, m.y, m.z + 1.0}, V3{0, 0, -1}, 3.0);
      if (h.hit && std::fabs(h.pos.z - m.z) < 0.3) ++hit;
    }
  }
  std::printf("  lane points checked %d, on a surface %d\n", tried, hit);
  if (!d->lanes.empty()) check(tried == 0 || hit >= tried * 9 / 10, "the lanes run on the road's surface");

  // 6. a first touch: at the 4 m column with the most free voxels within 30 m of the focus, a free
  // voxel 2 m above its lowest free voxel is shot (0.25 m, 2 kJ) or blasted (1.2 m, 400 kJ); the
  // tick that follows is timed (the structure is walked, its multigrid built, it is designed)
  if (touch != "none") {
    const i32 fx = static_cast<i32>(std::floor(d->focus.x / d->h + 0.5)), fy = static_cast<i32>(std::floor(d->focus.y / d->h + 0.5));
    i64 best = 0;
    IVec3 target{0, 0, 0};
    const i32 r = static_cast<i32>(30.0 / d->h);
    for (i32 cx = (fx - r) >> kChunkBits; cx <= (fx + r) >> kChunkBits; ++cx)
      for (i32 cy = (fy - r) >> kChunkBits; cy <= (fy + r) >> kChunkBits; ++cy) {
        i64 n = 0;
        i32 zmin = 1 << 30;
        for (const auto& [k, ch] : w.grid().chunks()) {
          const IVec3 c = unkey3(k);
          if (c[0] != cx || c[1] != cy || ch.uniform) continue;
          for (i32 i = 0; i < kChunkVox; ++i)
            if (vox_free(ch.v[static_cast<size_t>(i)])) {
              ++n;
              zmin = std::min(zmin, c[2] * kChunk + (i & 31));
            }
        }
        if (n > best) {
          best = n;
          target = IVec3{cx * kChunk + 16, cy * kChunk + 16, zmin + 16};
        }
      }
    if (best > 0) {
      // the free voxel nearest the target
      IVec3 hitv = target;
      for (i32 dz = 0; dz < 24; ++dz) {
        bool found = false;
        for (i32 dx = -8; dx <= 8 && !found; ++dx)
          for (i32 dy = -8; dy <= 8 && !found; ++dy)
            if (vox_free(w.grid().get(target[0] + dx, target[1] + dy, target[2] + dz))) {
              hitv = IVec3{target[0] + dx, target[1] + dy, target[2] + dz};
              found = true;
            }
        if (found) break;
      }
      const V3 p{hitv[0] * d->h, hitv[1] * d->h, hitv[2] * d->h};
      if (touch == "blast")
        w.blast(p, 1.2, 4e5);
      else
        w.shoot(p, 0.25, 2000.0);
      const WorldStats before = w.stats();
      const f64 a = now_ms();
      w.tick();
      const f64 ms = now_ms() - a;
      const WorldStats after = w.stats();
      std::printf("  first touch (%s at %.1f %.1f %.1f): tick %.0f ms, extracted nodes %lld, strengthened voxels %lld\n", touch.c_str(), p.x, p.y, p.z, ms,
                  static_cast<long long>(after.extracted_nodes - before.extracted_nodes), static_cast<long long>(after.strengthened_voxels - before.strengthened_voxels));
      f64 later = 0;
      for (int t = 0; t < 30; ++t) {
        const f64 b = now_ms();
        w.tick();
        later = std::max(later, now_ms() - b);
        w.take_events();
        w.take_changed_chunks();
      }
      std::printf("  the next 30 ticks: worst %.0f ms\n", later);
    }
  }
  std::printf("%s\n", fails ? "FAILED" : "all checks passed");
  return fails ? 1 : 0;
}
