// Fragments (docs/V2_DESIGN.md §1): a deterministic partition of each chunk's free voxels into
// connected pre-scored pieces.
#include <vector>

#include "doctest.h"
#include "svx/frag/fragments.hpp"

using namespace svx;

namespace {

VoxelGrid block_grid(MaterialId m) {
  VoxelGrid g;
  g.h = 0.125;
  for (i32 x = 0; x < kChunk; ++x)
    for (i32 y = 0; y < kChunk; ++y) {
      g.fill_column(x, y, 0, 1, make_vox(MaterialId::Rock, true));
      g.fill_column(x, y, 1, kChunk, make_vox(m, false));
    }
  g.compact();
  return g;
}

// every fragment's voxels are 6-connected among themselves
bool connected(const FragChunk& fc, i32 f) {
  std::vector<u8> seen(kChunkVox, 0);
  std::vector<i32> stack{fc.vox[size_t(fc.vox_start[size_t(f)])]};
  seen[size_t(stack[0])] = 1;
  i32 reached = 0;
  while (!stack.empty()) {
    const i32 i = stack.back();
    stack.pop_back();
    ++reached;
    const i32 x = i / (kChunk * kChunk), y = (i / kChunk) % kChunk, z = i % kChunk;
    const i32 nb[6][3] = {{x - 1, y, z}, {x + 1, y, z}, {x, y - 1, z}, {x, y + 1, z}, {x, y, z - 1}, {x, y, z + 1}};
    for (const auto& q : nb) {
      if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[0] >= kChunk || q[1] >= kChunk || q[2] >= kChunk) continue;
      const i32 j = (q[0] * kChunk + q[1]) * kChunk + q[2];
      if (seen[size_t(j)] || fc.at(j) != f) continue;
      seen[size_t(j)] = 1;
      stack.push_back(j);
    }
  }
  return reached == fc.frags[size_t(f)].count;
}

}  // namespace

TEST_CASE("fragments: every free voxel is in exactly one connected fragment, anchored ones in none") {
  const VoxelGrid g = block_grid(MaterialId::Rc);
  const FragChunk fc = fragment_chunk(g, {0, 0, 0});
  i64 total = 0;
  for (size_t f = 0; f < fc.frags.size(); ++f) {
    total += fc.frags[f].count;
    CHECK(connected(fc, static_cast<i32>(f)));
    CHECK(fc.frags[f].mass > 0.0);
  }
  CHECK(total == kChunk * kChunk * (kChunk - 1));
  for (i32 x = 0; x < kChunk; ++x)
    for (i32 y = 0; y < kChunk; ++y) CHECK(fc.at((x * kChunk + y) * kChunk) == -1);  // the anchored layer
  // rubble-sized pieces: tens of voxels on average for reinforced concrete
  const f64 mean = static_cast<f64>(total) / fc.frags.size();
  MESSAGE(fc.frags.size() << " fragments, mean " << mean << " voxels");
  CHECK(mean > 20.0);
  CHECK(mean < 90.0);
  // deterministic: the same content gives the same partition
  const FragChunk again = fragment_chunk(g, {0, 0, 0});
  CHECK(again.id == fc.id);
}

TEST_CASE("fragments: broken faces split fragments; mass properties add up") {
  VoxelGrid g = block_grid(MaterialId::Concrete);
  // break every face across the plane x = 15 | 16
  for (i32 y = 0; y < kChunk; ++y)
    for (i32 z = 1; z < kChunk; ++z) g.break_bond({15, y, z}, 0);
  const FragChunk fc = fragment_chunk(g, {0, 0, 0});
  for (size_t f = 0; f < fc.frags.size(); ++f) {
    const FragInfo& fi = fc.frags[f];
    CHECK((fi.hi[0] <= 15 || fi.lo[0] >= 16));  // no fragment straddles the cut
  }
  // mass and centre of mass of all fragments equal the block's
  f64 m = 0.0;
  V3 c;
  for (const FragInfo& fi : fc.frags) {
    m += fi.mass;
    c += fi.com * fi.mass;
  }
  const f64 h = g.h;
  const f64 expect = material(MaterialId::Concrete).rho * h * h * h * kChunk * kChunk * (kChunk - 1);
  CHECK(m == doctest::Approx(expect).epsilon(1e-9));
  c *= 1.0 / m;
  CHECK(c.x == doctest::Approx(h * 15.5).epsilon(1e-9));
  CHECK(c.z == doctest::Approx(h * 16.0).epsilon(1e-9));
}

TEST_CASE("fragments: materials never mix within a fragment") {
  VoxelGrid g = block_grid(MaterialId::Rc);
  for (i32 x = 0; x < kChunk; ++x)
    for (i32 y = 0; y < kChunk; ++y) g.fill_column(x, y, 16, kChunk, make_vox(MaterialId::Masonry, false));
  g.compact();
  const FragChunk fc = fragment_chunk(g, {0, 0, 0});
  for (size_t f = 0; f < fc.frags.size(); ++f)
    for (i32 k = fc.vox_start[f]; k < fc.vox_start[f + 1]; ++k) {
      const i32 i = fc.vox[size_t(k)];
      const i32 z = i % kChunk;
      CHECK((z >= 16) == (fc.frags[f].mat == MaterialId::Masonry));
    }
}
