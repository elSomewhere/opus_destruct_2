// svx_city tests — what rasterizing trees costs (not run by default: svx_city_tests --no-skip
// -tc='*bench*'): forest-sized trees of every kind with summer, autumn and winter looks,
// rasterized into every chunk they touch at LODs 0, 1 and 2; nanoseconds per voxel written.
#include <doctest.h>

#include <chrono>
#include <cstdio>

#include "core/hash.hpp"
#include "records.hpp"
#include "tree_records.hpp"

using namespace svx::city;

namespace {

struct Cost {
  double ns = 0;
  double written = 0;
  double chunks = 0;
};

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

Cost raster_all(const Tree& t, int lod) {
  const Box3 bb = tree_bounds(t);
  const double S = 32 << lod;
  const double s = 1 << lod;
  Cost c;
  for (double cz = std::floor((bb.z0 - s) / S); cz <= std::floor((bb.z1 + s) / S); cz += 1)
    for (double cy = std::floor((bb.y0 - s) / S); cy <= std::floor((bb.y1 + s) / S); cy += 1)
      for (double cx = std::floor((bb.x0 - s) / S); cx <= std::floor((bb.x1 + s) / S); cx += 1) {
        ChunkBuffer ch(lod, cx, cy, cz);
        const auto t0 = std::chrono::steady_clock::now();
        rasterize_tree(ch, t);
        const auto t1 = std::chrono::steady_clock::now();
        c.ns += std::chrono::duration<double, std::nano>(t1 - t0).count();
        c.written += ch.count_non_air();
        c.chunks += 1;
      }
  return c;
}

}  // namespace

TEST_CASE("city trees: bench rasterizing trees" * doctest::skip()) {
  const std::vector<Season> seasons = trec::seasons();
  const Season& summer = seasons[1];
  const Season& autumn = seasons[2];
  const Season& winter = seasons[3];
  for (const int lod : {0, 1, 2}) {
    Cost all;
    std::printf("LOD %d\n", lod);
    for (const TreeKindSpec& k : tree_kinds()) {
      rec::Samples r(5 + static_cast<uint32_t>(k.kind));
      Cost c;
      for (int i = 0; i < 24; ++i) {
        Tree t = forest_tree(r, k, 1000 + i * 37, -2000 + i * 53);
        const Season& S = i % 3 == 0 ? summer : i % 3 == 1 ? autumn : winter;
        t.look = S.tree_look(k.name, t.seed, 0.45);
        tree_model(t);  // (made with the tree, as its emitter does: tree_bounds)
        const Cost one = raster_all(t, lod);
        c.ns += one.ns;
        c.written += one.written;
        c.chunks += one.chunks;
      }
      std::printf("  %-10s %8.0f voxels/tree %6.1f chunks/tree %7.1f us/tree %6.2f ns/voxel\n", std::string(k.name).c_str(), c.written / 24, c.chunks / 24,
                  c.ns / 24 / 1000, c.ns / js::max(1, c.written));
      all.ns += c.ns;
      all.written += c.written;
      all.chunks += c.chunks;
    }
    std::printf("  all        %8.0f voxels %8.0f chunks %9.1f ms  %6.2f ns/voxel written, %6.1f us/chunk\n", all.written, all.chunks, all.ns / 1e6,
                all.ns / all.written, all.ns / all.chunks / 1000);
  }
}
