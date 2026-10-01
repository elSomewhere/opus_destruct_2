#include "svx/game/columns.hpp"

#include <algorithm>

namespace svx {

namespace {

struct UF {
  std::vector<i32> p;
  explicit UF(size_t n) : p(n) {
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<i32>(i);
  }
  i32 find(i32 x) {
    while (p[x] != x) {
      p[x] = p[p[x]];
      x = p[x];
    }
    return x;
  }
  void unite(i32 a, i32 b) {
    a = find(a);
    b = find(b);
    if (a == b) return;
    if (a < b)
      p[b] = a;
    else
      p[a] = b;
  }
};

}  // namespace

i64 ColumnGrid::count(RunKind k) const {
  i64 c = 0;
  for (const Run& r : runs)
    if (r.kind == k) c += r.z1 - r.z0;
  return c;
}

RunComponents run_components(const ColumnGrid& g) {
  RunComponents out;
  const size_t nr = g.runs.size();
  UF uf(nr);
  std::vector<i64> anchor_bonds(nr, 0);
  auto overlap = [](const Run& a, const Run& b) { return std::min(a.z1, b.z1) - std::max(a.z0, b.z0); };
  const int dx[4] = {1, -1, 0, 0};
  const int dy[4] = {0, 0, 1, -1};
  for (i32 y = 0; y < g.ny; ++y)
    for (i32 x = 0; x < g.nx; ++x) {
      const i32 c = g.column(x, y);
      const u32 b0 = g.col_start[c], b1 = g.col_start[c + 1];
      // vertical contacts inside the column (adjacent runs)
      for (u32 r = b0; r + 1 < b1; ++r) {
        const Run& lo = g.runs[r];
        const Run& hi = g.runs[r + 1];
        if (lo.z1 != hi.z0) continue;
        if (lo.kind == RunKind::Structural && hi.kind == RunKind::Structural)
          uf.unite(static_cast<i32>(r), static_cast<i32>(r + 1));
        else if (lo.kind == RunKind::Structural && hi.kind == RunKind::Anchored)
          anchor_bonds[r] += 1;
        else if (lo.kind == RunKind::Anchored && hi.kind == RunKind::Structural)
          anchor_bonds[r + 1] += 1;
      }
      // horizontal contacts
      for (int d = 0; d < 4; ++d) {
        const i32 xn = x + dx[d], yn = y + dy[d];
        if (xn < 0 || yn < 0 || xn >= g.nx || yn >= g.ny) continue;
        const i32 cn = g.column(xn, yn);
        u32 i = b0, j = g.col_start[cn];
        const u32 j1 = g.col_start[cn + 1];
        while (i < b1 && j < j1) {
          const Run& a = g.runs[i];
          const Run& b = g.runs[j];
          const i32 ov = overlap(a, b);
          if (ov > 0 && a.kind == RunKind::Structural) {
            if (b.kind == RunKind::Structural) {
              if (d == 0 || d == 2) uf.unite(static_cast<i32>(i), static_cast<i32>(j));
            } else {
              anchor_bonds[i] += ov;
            }
          }
          if (a.z1 < b.z1)
            ++i;
          else
            ++j;
        }
      }
    }
  out.run_comp.assign(nr, -1);
  std::vector<i32> root_comp(nr, -1);
  for (i32 y = 0; y < g.ny; ++y)
    for (i32 x = 0; x < g.nx; ++x) {
      const i32 c = g.column(x, y);
      for (u32 r = g.col_start[c]; r < g.col_start[c + 1]; ++r) {
        const Run& run = g.runs[r];
        if (run.kind != RunKind::Structural) continue;
        const i32 root = uf.find(static_cast<i32>(r));
        if (root_comp[root] < 0) {
          root_comp[root] = static_cast<i32>(out.comps.size());
          RunComponents::Comp cc;
          cc.bbox = {x, y, run.z0, x, y, run.z1 - 1};
          cc.first_run = static_cast<i32>(r);
          out.comps.push_back(cc);
        }
        const i32 id = root_comp[root];
        out.run_comp[r] = id;
        RunComponents::Comp& cc = out.comps[id];
        cc.voxels += run.z1 - run.z0;
        cc.anchor_bonds += anchor_bonds[r];
        cc.bbox[0] = std::min(cc.bbox[0], x);
        cc.bbox[1] = std::min(cc.bbox[1], y);
        cc.bbox[2] = std::min(cc.bbox[2], run.z0);
        cc.bbox[3] = std::max(cc.bbox[3], x);
        cc.bbox[4] = std::max(cc.bbox[4], y);
        cc.bbox[5] = std::max(cc.bbox[5], run.z1 - 1);
      }
    }
  return out;
}


}  // namespace svx
