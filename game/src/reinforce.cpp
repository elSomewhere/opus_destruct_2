#include "svx/game/reinforce.hpp"

#include <algorithm>

namespace svx {

i64 reinforce(VoxelGrid& g, const IVec3& lo, const IVec3& hi, i32 spacing) {
  spacing = std::max(2, spacing);
  const Vox bar = make_vox(MaterialId::Rebar, false);
  i64 n = 0;
  for (int a = 0; a < 3; ++a) {
    const int b = (a + 1) % 3, c = (a + 2) % 3;
    const i32 len = hi[a] - lo[a], wb = hi[b] - lo[b], wc = hi[c] - lo[c];
    if (len < 2 * spacing || wb < 3 || wc < 3) continue;
    // bar positions across: one voxel in from each face, then every `spacing` (a thin side
    // gets its middle)
    auto across = [&](i32 l, i32 w) {
      std::vector<i32> at;
      if (w < 3 + spacing) {
        at.push_back(l + w / 2);
      } else {
        for (i32 u = l + 1; u <= l + w - 2; u += spacing) at.push_back(u);
        if (at.back() != l + w - 2) at.push_back(l + w - 2);
      }
      return at;
    };
    for (i32 u : across(lo[b], wb))
      for (i32 v : across(lo[c], wc))
        for (i32 t = lo[a]; t < hi[a]; ++t) {
          IVec3 p;
          p[a] = t;
          p[b] = u;
          p[c] = v;
          const Vox cur = g.get(p);
          if (!vox_free(cur) || cur == bar) continue;
          g.set(p, bar);
          ++n;
        }
  }
  return n;
}

}  // namespace svx
