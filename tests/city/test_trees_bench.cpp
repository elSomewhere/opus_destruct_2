// svx_city tests — what rasterizing trees costs (not run by default: svx_city_tests --no-skip
// -tc='*bench*'). Each time is the best of several runs (the machine may be busy):
//   - forest chunks: the chunks of a wood (a tree per 5 m lattice cell, as nature/forest's, of a
//     temperate and of a boreal mix, summer, autumn and winter looks), each with every tree that
//     reaches into it, at LODs 0, 1 and 2 - per chunk and per voxel written;
//   - per kind: forest-sized trees of every kind rasterized alone into every chunk they touch.
// ("profile" runs the forest chunks at LOD 0 once, for a profiler.)
#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "core/hash.hpp"
#include "records.hpp"
#include "tree_records.hpp"

using namespace svx::city;

namespace {

constexpr int kReps = 5;

// A forest's tree of a kind (forest.js computeTree's sizes: the kind's ranges, crowded or open).
Tree forest_tree(rec::Samples& r, const TreeKindSpec& k, double x, double y) {
  Tree t;
  t.x = x;
  t.y = y;
  t.z = 64;
  t.kind = k.kind;
  const double a = r();
  const double b = r();
  t.h = js::max(3, js::round(js::round((k.h[0] + (k.h[1] - k.h[0]) * a) * 8) * (0.9 + 0.2 * r())));
  t.r = js::round((k.r[0] + (k.r[1] - k.r[0]) * b) * 8) * (1.05 + 0.2 * r());
  t.seed = hash32(x, y, 311);
  t.open = r() < 0.3;
  return t;
}

// The best time (ns) of rasterizing trees into a fresh chunk, and the voxels they write.
struct Run {
  double ns = 0;
  int written = 0;
};
Run best_of(int lod, double cx, double cy, double cz, const std::vector<const Tree*>& trees) {
  Run best;
  best.ns = 1e300;
  for (int rep = 0; rep < kReps; ++rep) {
    ChunkBuffer ch(lod, cx, cy, cz);
    const auto t0 = std::chrono::steady_clock::now();
    for (const Tree* t : trees) rasterize_tree(ch, *t);
    const auto t1 = std::chrono::steady_clock::now();
    best.ns = std::min(best.ns, std::chrono::duration<double, std::nano>(t1 - t0).count());
    best.written = ch.count_non_air();
  }
  return best;
}

bool touches(const Box3& b, int lod, double cx, double cy, double cz) {
  const ChunkBuffer probe(lod, cx, cy, cz);
  return probe.touches(b.x0, b.y0, b.z0, b.x1, b.y1, b.z1);
}

// A wood of 16 x 16 lattice cells (40 voxels) of a temperate (0) or boreal (1) mix of kinds, in
// summer (0), autumn (1) or winter (2).
std::vector<Tree> wood(int mix, int season, const std::vector<Season>& seasons) {
  static const std::vector<std::vector<std::pair<const char*, double>>> kMixes = {
      {{"oak", 4}, {"maple", 3}, {"birch", 2}, {"pine", 0.6}, {"spruce", 0.8}, {"rowan", 0.4}, {"aspen", 0.45}, {"alder", 0.35}},
      {{"spruce", 5}, {"pine", 4}, {"birch", 2.5}, {"rowan", 0.4}, {"aspen", 0.45}, {"larch", 0.5}, {"alder", 0.25}},
  };
  const auto& mx = kMixes[static_cast<size_t>(mix)];
  const Season& look = seasons[static_cast<size_t>(1 + season)];
  rec::Samples r(41 + static_cast<uint32_t>(mix * 3 + season));
  std::vector<Tree> trees;
  trees.reserve(256);
  for (int gy = 0; gy < 16; ++gy)
    for (int gx = 0; gx < 16; ++gx) {
      double total = 0;
      for (const auto& kw : mx) total += kw.second;
      double acc = r() * total;
      const char* kind = mx.back().first;
      for (const auto& kw : mx) {
        acc -= kw.second;
        if (acc < 0) {
          kind = kw.first;
          break;
        }
      }
      const double x = gx * 40 + std::floor(r() * 40);
      const double y = gy * 40 + std::floor(r() * 40);
      Tree t = forest_tree(r, *tree_kind_spec(kind), x, y);
      t.look = look.tree_look(kind, t.seed, 0.45);
      t.bb = tree_bounds(t);
      trees.push_back(t);
    }
  return trees;
}

// Every chunk over the wood's middle (whole trees round them) at a LOD, from the ground up, with
// the trees that reach into it.
template <class F>
void wood_chunks(const std::vector<Tree>& trees, int lod, F f) {
  const double S = 32 << lod;
  for (double cz = std::floor(56 / S); cz <= std::floor(300 / S); cz += 1)
    for (double cy = std::floor(160 / S); cy < std::floor(480 / S); cy += 1)
      for (double cx = std::floor(160 / S); cx < std::floor(480 / S); cx += 1) {
        std::vector<const Tree*> in;
        for (const Tree& t : trees)
          if (touches(t.bb, lod, cx, cy, cz)) in.push_back(&t);
        if (!in.empty()) f(cx, cy, cz, in);
      }
}

}  // namespace

TEST_CASE("city trees: profile forest chunks once" * doctest::skip()) {
  const std::vector<Season> seasons = trec::seasons();
  int written = 0;
  for (int mix = 0; mix < 2; ++mix)
    for (const int season : {0, 2}) {
      const std::vector<Tree> trees = wood(mix, season, seasons);
      wood_chunks(trees, 0, [&](double cx, double cy, double cz, const std::vector<const Tree*>& in) {
        ChunkBuffer ch(0, cx, cy, cz);
        for (const Tree* t : in) rasterize_tree(ch, *t);
        written += ch.count_non_air();
      });
    }
  CHECK(written > 0);
}

TEST_CASE("city trees: bench rasterizing trees" * doctest::skip()) {
  const std::vector<Season> seasons = trec::seasons();
  const char* mix_names[2] = {"temperate", "boreal"};
  const char* season_names[3] = {"summer", "autumn", "winter"};
  for (int mix = 0; mix < 2; ++mix)
    for (int season = 0; season < 3; ++season) {
      const std::vector<Tree> trees = wood(mix, season, seasons);
      for (const int lod : {0, 1, 2}) {
        double ns = 0, written = 0, chunks = 0, pairs = 0;
        wood_chunks(trees, lod, [&](double cx, double cy, double cz, const std::vector<const Tree*>& in) {
          const Run run = best_of(lod, cx, cy, cz, in);
          ns += run.ns;
          written += run.written;
          chunks += 1;
          pairs += static_cast<double>(in.size());
        });
        std::printf("forest %-9s %-6s LOD %d: %5.0f chunks, %4.1f trees/chunk, %7.0f voxels/chunk, %7.1f us/chunk, %6.1f ns/voxel written\n", mix_names[mix],
                    season_names[season], lod, chunks, pairs / chunks, written / chunks, ns / chunks / 1000, ns / written);
      }
    }
  // per kind, alone
  for (const int lod : {0, 1, 2}) {
    double all_ns = 0, all_written = 0;
    std::printf("per kind, LOD %d (summer, autumn, winter looks)\n", lod);
    for (const TreeKindSpec& k : tree_kinds()) {
      rec::Samples r(5 + static_cast<uint32_t>(k.kind));
      double ns = 0, written = 0, chunks = 0;
      for (int i = 0; i < 12; ++i) {
        Tree t = forest_tree(r, k, 1000 + i * 37, -2000 + i * 53);
        t.look = seasons[static_cast<size_t>(1 + i % 3)].tree_look(k.name, t.seed, 0.45);
        const Box3 bb = tree_bounds(t);  // (the model made with the tree, as its emitter does)
        const double S = 32 << lod;
        const double s = 1 << lod;
        const std::vector<const Tree*> one = {&t};
        for (double cz = std::floor((bb.z0 - s) / S); cz <= std::floor((bb.z1 + s) / S); cz += 1)
          for (double cy = std::floor((bb.y0 - s) / S); cy <= std::floor((bb.y1 + s) / S); cy += 1)
            for (double cx = std::floor((bb.x0 - s) / S); cx <= std::floor((bb.x1 + s) / S); cx += 1) {
              const Run run = best_of(lod, cx, cy, cz, one);
              ns += run.ns;
              written += run.written;
              chunks += 1;
            }
      }
      std::printf("  %-10s %8.0f voxels/tree %6.1f chunks/tree %8.1f us/tree %7.2f ns/voxel\n", std::string(k.name).c_str(), written / 12, chunks / 12, ns / 12 / 1000,
                  ns / js::max(1, written));
      all_ns += ns;
      all_written += written;
    }
    std::printf("  all kinds: %.2f ns/voxel written\n", all_ns / all_written);
  }
}
