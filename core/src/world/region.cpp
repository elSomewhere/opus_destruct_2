#include "svx/world/region.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <unordered_set>

namespace svx {

namespace {

// The grid's chunks over a box, looked up once: voxel reads inside the box cost an index
// instead of a hash lookup (same results as the grid's accessors; outside the box it defers).
class GridView {
 public:
  GridView(const VoxelGrid& g, const IVec3& lo, const IVec3& hi) : g_(g) {
    c0_ = chunk_of(lo);
    const IVec3 c1 = chunk_of({hi[0] - 1, hi[1] - 1, hi[2] - 1});
    for (int q = 0; q < 3; ++q) n_[q] = std::max(0, c1[q] - c0_[q] + 1);
    table_.assign(size_t(n_[0]) * size_t(n_[1]) * size_t(n_[2]), nullptr);
    for (i32 x = 0; x < n_[0]; ++x)
      for (i32 y = 0; y < n_[1]; ++y)
        for (i32 z = 0; z < n_[2]; ++z)
          table_[(size_t(x) * size_t(n_[1]) + size_t(y)) * size_t(n_[2]) + size_t(z)] =
              g.chunk({c0_[0] + x, c0_[1] + y, c0_[2] + z});
  }
  const Chunk* chunk_at(const IVec3& p) const {
    const IVec3 cc = chunk_of(p);
    const i32 x = cc[0] - c0_[0], y = cc[1] - c0_[1], z = cc[2] - c0_[2];
    if (x < 0 || y < 0 || z < 0 || x >= n_[0] || y >= n_[1] || z >= n_[2]) return g_.chunk(cc);
    return table_[(size_t(x) * size_t(n_[1]) + size_t(y)) * size_t(n_[2]) + size_t(z)];
  }
  Vox get(const IVec3& p) const {
    const Chunk* c = chunk_at(p);
    if (!c) return kAir;
    return c->uniform ? c->value : c->v[chunk_index(p)];
  }
  bool broken(const IVec3& p, int axis) const {
    const Chunk* c = chunk_at(p);
    if (!c || c->broken.empty()) return false;
    return (c->broken[chunk_index(p)] >> axis) & 1;
  }
  bool cracked(const IVec3& p, int axis) const {
    const Chunk* c = chunk_at(p);
    if (!c || c->broken.empty()) return false;
    return (c->broken[chunk_index(p)] >> (3 + axis)) & 1;
  }
  u8 strength(const IVec3& p) const {
    const Chunk* c = chunk_at(p);
    if (!c || c->strength.empty()) return 0;
    return c->strength[chunk_index(p)];
  }
  f32 damage(const IVec3& p, int axis) const { return g_.damage(p, axis); }

 private:
  const VoxelGrid& g_;
  IVec3 c0_{0, 0, 0};
  IVec3 n_{0, 0, 0};
  std::vector<const Chunk*> table_;
};

template <typename Inside, typename Each>
Region extract_impl(const VoxelGrid& g, const IVec3& vlo, const IVec3& vhi, Each&& each, Inside&& inside,
                    const LatticeOptions& lo, size_t expect);

template <typename Inside>
Region extract_impl(const VoxelGrid& g, const IVec3& blo, const IVec3& bhi, Inside&& inside, const LatticeOptions& lo) {
  auto each = [&](auto&& f) {
    for (i32 x = blo[0]; x < bhi[0]; ++x)
      for (i32 y = blo[1]; y < bhi[1]; ++y)
        for (i32 z = blo[2]; z < bhi[2]; ++z) f(IVec3{x, y, z});
  };
  return extract_impl(g, blo, bhi, each, inside, lo, 0);
}

}  // namespace

Region extract_region(const VoxelGrid& g, const IVec3& center, i32 radius, const LatticeOptions& lo) {
  const i64 r2 = i64(radius) * radius;
  auto inside = [&](const IVec3& p) {
    const i64 dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
    return dx * dx + dy * dy + dz * dz <= r2;
  };
  auto each = [&](auto&& f) {
    for (i32 x = center[0] - radius; x <= center[0] + radius; ++x)
      for (i32 y = center[1] - radius; y <= center[1] + radius; ++y)
        for (i32 z = center[2] - radius; z <= center[2] + radius; ++z) f(IVec3{x, y, z});
  };
  // (room for a quarter of the ball's volume: the cells of a typical window without growing)
  const size_t expect = static_cast<size_t>(i64(radius) * radius * radius);
  Region R = extract_impl(g, {center[0] - radius, center[1] - radius, center[2] - radius},
                          {center[0] + radius + 1, center[1] + radius + 1, center[2] + radius + 1}, each, inside, lo,
                          expect);
  R.center = center;
  R.radius = radius;
  return R;
}

Region extract_box(const VoxelGrid& g, const IVec3& blo, const IVec3& bhi, const LatticeOptions& lo) {
  auto inside = [&](const IVec3& p) {
    return p[0] >= blo[0] && p[1] >= blo[1] && p[2] >= blo[2] && p[0] < bhi[0] && p[1] < bhi[1] && p[2] < bhi[2];
  };
  Region R = extract_impl(g, blo, bhi, inside, lo);
  R.center = {(blo[0] + bhi[0]) / 2, (blo[1] + bhi[1]) / 2, (blo[2] + bhi[2]) / 2};
  R.radius = std::max({bhi[0] - blo[0], bhi[1] - blo[1], bhi[2] - blo[2]}) / 2;
  return R;
}

Region extract_set(const VoxelGrid& g, std::span<const IVec3> set, const LatticeOptions& lo) {
  std::unordered_set<u64> members;
  members.reserve(set.size() * 2);
  IVec3 mn{INT32_MAX, INT32_MAX, INT32_MAX}, mx{INT32_MIN, INT32_MIN, INT32_MIN};
  for (const IVec3& p : set) {
    members.insert(key3(p[0], p[1], p[2]));
    for (int q = 0; q < 3; ++q) {
      mn[q] = std::min(mn[q], p[q]);
      mx[q] = std::max(mx[q], p[q]);
    }
  }
  auto inside = [&](const IVec3& p) { return members.count(key3(p[0], p[1], p[2])) > 0; };
  auto each = [&](auto&& f) {
    for (const IVec3& p : set) f(p);
  };
  Region R = set.empty() ? extract_impl(g, IVec3{0, 0, 0}, IVec3{1, 1, 1}, each, inside, lo, 0)
                         : extract_impl(g, mn, IVec3{mx[0] + 1, mx[1] + 1, mx[2] + 1}, each, inside, lo, set.size());
  if (!set.empty()) {
    R.center = {(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2};
    R.radius = std::max({mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]}) / 2 + 1;
  }
  return R;
}

std::vector<IVec3> structures_of(const VoxelGrid& g, std::span<const IVec3> seeds, i64 max_cells, bool* truncated,
                                 const std::function<bool(const IVec3& cc)>& hydrate) {
  std::vector<IVec3> out;
  std::unordered_set<u64> seen;
  if (truncated) *truncated = false;
  for (const IVec3& s : seeds) {
    const Vox v = g.get(s);
    if (!vox_solid(v) || vox_anchored(v)) continue;
    if (seen.insert(key3(s[0], s[1], s[2])).second) out.push_back(s);
  }
  for (size_t h = 0; h < out.size(); ++h) {
    if (static_cast<i64>(out.size()) >= max_cells) {
      if (truncated) *truncated = true;
      break;
    }
    const IVec3 p = out[h];
    for (int a = 0; a < 3; ++a)
      for (int sg = -1; sg <= 1; sg += 2) {
        IVec3 q = p;
        q[a] += sg;
        if (!g.known(chunk_of(q)) && !(hydrate && hydrate(chunk_of(q)))) {  // beyond a snapshot: the rest is unknown
          if (truncated) *truncated = true;
          return out;
        }
        const Vox w = g.get(q);
        if (!vox_solid(w) || vox_anchored(w)) continue;
        if (g.broken(sg > 0 ? p : q, a)) continue;
        if (seen.insert(key3(q[0], q[1], q[2])).second) out.push_back(q);
      }
  }
  return out;
}

namespace {

template <typename Inside, typename Each>
Region extract_impl(const VoxelGrid& g, const IVec3& vlo, const IVec3& vhi, Each&& each, Inside&& inside,
                    const LatticeOptions& lo, size_t expect) {
  static const bool prof = std::getenv("SVX_EXTRACT_PROFILE") != nullptr;  // (diagnostics)
  auto t_last = std::chrono::steady_clock::now();
  auto lap = [&](const char* what) {
    if (!prof) return;
    const auto now = std::chrono::steady_clock::now();
    std::printf("      [extract] %-8s %6.2f ms\n", what, std::chrono::duration<double, std::milli>(now - t_last).count());
    t_last = now;
  };
  // (the view covers the box plus one voxel: rim and rock neighbours)
  const GridView view(g, {vlo[0] - 1, vlo[1] - 1, vlo[2] - 1}, {vhi[0] + 1, vhi[1] + 1, vhi[2] + 1});
  lap("view");
  Region R;
  std::vector<CellIn> cells;
  std::vector<IVec3> vox;
  if (expect > 0) {
    R.index.reserve(expect + expect / 4);
    cells.reserve(expect + expect / 4);
    vox.reserve(expect + expect / 4);
  }
  auto add = [&](const IVec3& p, Vox v, bool pinned) {
    if (!R.index.insert(p, static_cast<i32>(cells.size()))) return;
    CellIn c;
    c.p = p;
    c.mat = vox_mat(v);
    c.anchored = vox_anchored(v) || pinned;
    c.strength = view.strength(p);
    cells.push_back(c);
    vox.push_back(p);
    R.rim.push_back(pinned && !vox_anchored(v) ? 1 : 0);
  };
  // structural and anchored voxels of the window
  each([&](const IVec3& p) {
    if (!inside(p)) return;
    const Vox v = view.get(p);
    if (!vox_solid(v)) return;
    bool pinned = false;
    if (!vox_anchored(v))
      for (int a = 0; a < 3 && !pinned; ++a)
        for (int s = -1; s <= 1; s += 2) {
          IVec3 q = p;
          q[a] += s;
          if (inside(q)) continue;
          const Vox w = view.get(q);
          if (!vox_solid(w) || vox_anchored(w)) continue;
          // a structural bond leaves the window
          const IVec3 lo_p = s > 0 ? p : q;
          if (!view.broken(lo_p, a)) {
            pinned = true;
            break;
          }
        }
    add(p, v, pinned);
  });
  lap("cells");
  // rock bonded to the window from outside
  const size_t ncore = cells.size();
  for (size_t k = 0; k < ncore; ++k) {
    if (cells[k].anchored) continue;
    const IVec3 p = vox[k];
    for (int a = 0; a < 3; ++a)
      for (int s = -1; s <= 1; s += 2) {
        IVec3 q = p;
        q[a] += s;
        if (inside(q)) continue;
        const Vox w = view.get(q);
        if (vox_solid(w) && vox_anchored(w)) add(q, w, false);
      }
  }
  lap("rock");
  // plate classes from the world (as a whole-world lattice classifies them): in the window's
  // own lattice the runs would end at its boundary and at its pinned rim
  if (lo.plate.enabled) {
    const i32 tmax = lo.plate.max_thickness, wmin = lo.plate.min_width;
    constexpr int kStride[3] = {kChunk * kChunk, kChunk, 1};  // (chunk_index steps per axis)
    auto run = [&](const IVec3& p, MaterialId m, int a, i32 cap) {
      i32 len = 1;
      for (int sg = -1; sg <= 1 && len < cap; sg += 2) {
        IVec3 q = p;
        const Chunk* ch = view.chunk_at(q);
        int idx = chunk_index(q);
        for (;;) {  // (within a chunk by index; a chunk lookup only when the run leaves it)
          const int local = q[a] & (kChunk - 1);
          q[a] += sg;
          if (local == (sg > 0 ? kChunk - 1 : 0)) {
            ch = view.chunk_at(q);
            idx = chunk_index(q);
          } else {
            idx += sg * kStride[a];
          }
          const Vox w = !ch ? kAir : (ch->uniform ? ch->value : ch->v[size_t(idx)]);
          if (!vox_solid(w) || vox_anchored(w) || vox_mat(w) != m) break;
          if (++len >= cap) break;
        }
      }
      return len;
    };
    for (size_t k = 0; k < cells.size(); ++k) {
      CellIn& c = cells[k];
      c.plate = 0;
      if (vox_anchored(view.get(vox[k]))) continue;  // (rock)
      for (int n = 0; n < 3; ++n) {
        if (run(vox[k], c.mat, n, tmax + 1) > tmax) continue;
        if (run(vox[k], c.mat, (n + 1) % 3, wmin) < wmin || run(vox[k], c.mat, (n + 2) % 3, wmin) < wmin) continue;
        c.plate = static_cast<u8>(1 + n);
        break;
      }
    }
  }
  lap("plates");
  R.L = build_lattice(cells, lo);
  lap("lattice");
  R.vox = std::move(vox);
  Lattice& L = R.L;
  L.enable_damage();
  for (i32 i = 0; i < L.n; ++i) {
    for (int a = 0; a < 3; ++a) {
      const i32 j = L.nbr[a][i];
      if (j < 0) continue;
      if (view.broken(R.vox[i], a)) {
        L.break_bond(i, a);
        continue;
      }
      if (view.cracked(R.vox[i], a)) {
        L.crack_bond(i, a);  // contact state from the next law sweep
        continue;
      }
      L.dmg[a][i] = view.damage(R.vox[i], a);
    }
    if (R.rim[i]) ++R.rim_count;
  }
  lap("overlays");
  return R;
}

}  // namespace

std::vector<std::vector<IVec3>> detached_islands_grid(const VoxelGrid& g, std::span<const IVec3> seeds,
                                                      bool supports_removed, ConnStats* stats, CoarseWorld* coarse,
                                                      std::vector<u64>* nonresident) {
  // One search per seed, advanced in lockstep (one node each per round), merged when they
  // meet; a search is supported once it reaches an anchored voxel, an island once exhausted.
  // Nodes are resident voxels and, in streamed worlds, components of non-resident chunks.
  struct Search {
    std::vector<IVec3> members, queue;
    std::vector<u64> cmembers, cqueue;  // component nodes: (slot << 16) | component
    size_t head = 0, chead = 0;
    bool supported = false, exhausted = false;
  };
  std::unordered_map<u64, i32> owner, cowner;
  std::vector<Search> S;
  std::vector<i32> sp;
  // non-resident chunks met, in order of first use (summaries are held by `coarse`)
  constexpr size_t kMaxCoarseChunks = 4096;
  std::unordered_map<u64, i32> slot_of;
  std::vector<std::pair<IVec3, const ChunkSummary*>> slots;
  auto slot_for = [&](const IVec3& cc) -> i32 {
    const u64 k = key3(cc[0], cc[1], cc[2]);
    const auto it = slot_of.find(k);
    if (it != slot_of.end()) return it->second;
    const ChunkSummary* cs = slots.size() < kMaxCoarseChunks ? coarse->summary(cc) : nullptr;
    const i32 s = cs ? static_cast<i32>(slots.size()) : -1;
    if (cs) slots.push_back({cc, cs});
    slot_of.emplace(k, s);
    return s;
  };
  auto find = [&](i32 x) {
    while (sp[x] != x) {
      sp[x] = sp[sp[x]];
      x = sp[x];
    }
    return x;
  };
  auto merge = [&](i32 a, i32 b) {
    if (a == b) return a;
    if (S[a].members.size() + S[a].cmembers.size() < S[b].members.size() + S[b].cmembers.size()) std::swap(a, b);
    S[a].members.insert(S[a].members.end(), S[b].members.begin(), S[b].members.end());
    S[a].queue.insert(S[a].queue.end(), S[b].queue.begin() + static_cast<long>(S[b].head), S[b].queue.end());
    S[a].cmembers.insert(S[a].cmembers.end(), S[b].cmembers.begin(), S[b].cmembers.end());
    S[a].cqueue.insert(S[a].cqueue.end(), S[b].cqueue.begin() + static_cast<long>(S[b].chead), S[b].cqueue.end());
    S[a].supported = S[a].supported || S[b].supported;
    std::vector<IVec3>().swap(S[b].members);
    std::vector<IVec3>().swap(S[b].queue);
    std::vector<u64>().swap(S[b].cmembers);
    std::vector<u64>().swap(S[b].cqueue);
    sp[b] = a;
    return a;
  };
  bool any_supported = false;
  for (const IVec3& s : seeds) {
    const Vox v = g.get(s);
    if (!vox_solid(v) || owner.count(key3(s[0], s[1], s[2]))) continue;
    const i32 id = static_cast<i32>(S.size());
    S.emplace_back();
    sp.push_back(id);
    S[id].members.push_back(s);
    S[id].queue.push_back(s);
    owner.emplace(key3(s[0], s[1], s[2]), id);
    if (vox_anchored(v)) S[id].supported = any_supported = true;
    if (stats) ++stats->searches;
  }
  // search r reaches a node: returns true when r is (now) supported
  auto reach_voxel = [&](i32& r, const IVec3& nb, Vox v) {
    if (vox_anchored(v)) {
      S[r].supported = any_supported = true;
      return true;
    }
    const u64 k = key3(nb[0], nb[1], nb[2]);
    const auto it = owner.find(k);
    if (it == owner.end()) {
      owner.emplace(k, r);
      S[r].members.push_back(nb);
      S[r].queue.push_back(nb);
      return false;
    }
    const i32 o = find(it->second);
    if (o != r) {
      r = merge(r, o);
      if (S[r].supported) return true;
    }
    return false;
  };
  auto reach_comp = [&](i32& r, i32 slot, i32 comp) {
    const u64 k = (u64(slot) << 16) | u64(comp);
    const auto it = cowner.find(k);
    if (it == cowner.end()) {
      cowner.emplace(k, r);
      S[r].cmembers.push_back(k);
      S[r].cqueue.push_back(k);
      return false;
    }
    const i32 o = find(it->second);
    if (o != r) {
      r = merge(r, o);
      if (S[r].supported) return true;
    }
    return false;
  };
  auto support = [&](i32 r) {
    S[r].supported = any_supported = true;
    return true;
  };
  // from resident voxel c into the non-resident chunk across its face (axis a, direction dir)
  auto cross = [&](i32& r, const IVec3& c, int a, int dir) {
    IVec3 q = c;
    q[a] += dir;
    const i32 slot = slot_for(chunk_of(q));
    if (slot < 0) return support(r);  // unknown: taken as a support (never a false detachment)
    const int b = (a + 1) % 3, d = (a + 2) % 3;
    const u16 lab = slots[size_t(slot)].second->label(2 * a + (dir > 0 ? 0 : 1), q[b] & (kChunk - 1), q[d] & (kChunk - 1));
    if (lab == 0 || (dir > 0 && g.broken(c, a))) return false;  // (a + face label holds its own bond bit)
    if (lab == ChunkSummary::kAnchor) return support(r);
    return reach_comp(r, slot, lab - 1);
  };
  // expands voxel c of search r
  auto expand_voxel = [&](i32& r, const IVec3& c) {
    for (int a = 0; a < 3; ++a) {
      const int x = c[a] & (kChunk - 1);
      if (g.bond(c, a)) {
        IVec3 q = c;
        q[a] += 1;
        if (reach_voxel(r, q, g.get(q))) return;
      } else if (coarse && x == kChunk - 1) {
        IVec3 q = c;
        q[a] += 1;
        if (!coarse->resident(chunk_of(q)) && cross(r, c, a, +1)) return;
      }
      IVec3 m = c;
      m[a] -= 1;
      if (g.bond(m, a)) {
        if (reach_voxel(r, m, g.get(m))) return;
      } else if (coarse && x == 0) {
        if (!coarse->resident(chunk_of(m)) && cross(r, c, a, -1)) return;
      }
    }
  };
  // expands component node k of search r: its anchorage, then every connection across its
  // chunk's faces (to resident voxels or to components of other non-resident chunks)
  auto expand_comp = [&](i32& r, u64 k) {
    const i32 slot = static_cast<i32>(k >> 16), comp = static_cast<i32>(k & 0xFFFF);
    const IVec3 cc = slots[size_t(slot)].first;
    const ChunkSummary* cs = slots[size_t(slot)].second;
    if (cs->anchored[size_t(comp)]) {
      support(r);
      return;
    }
    const u16 me = static_cast<u16>(1 + comp);
    for (int a = 0; a < 3; ++a) {
      const int b = (a + 1) % 3, d = (a + 2) % 3;
      for (int side = 0; side < 2; ++side) {
        const std::vector<u16>& face = cs->face[size_t(2 * a + side)];
        if (face.empty()) continue;
        IVec3 nc = cc;
        nc[a] += side ? 1 : -1;
        const bool res = coarse->resident(nc);
        const i32 nslot = res ? -1 : slot_for(nc);
        if (!res && nslot < 0) {
          for (u16 l : face)
            if (l == me) {
              support(r);  // unknown neighbour chunk
              return;
            }
          continue;
        }
        for (int i = 0; i < kChunk * kChunk; ++i) {
          if (face[size_t(i)] != me) continue;
          const int u = i / kChunk, v = i % kChunk;
          if (!res) {
            // both labels non-zero: solid on both sides, and the lower voxel's + face label
            // holds the bond's broken bit
            const u16 lab = slots[size_t(nslot)].second->label(2 * a + (1 - side), u, v);
            if (lab == 0) continue;
            if (lab == ChunkSummary::kAnchor) {
              support(r);
              return;
            }
            if (reach_comp(r, nslot, lab - 1)) return;
            continue;
          }
          IVec3 p;
          p[a] = nc[a] * kChunk + (side ? 0 : kChunk - 1);
          p[b] = cc[b] * kChunk + u;
          p[d] = cc[d] * kChunk + v;
          const Vox vp = g.get(p);
          if (!vox_solid(vp) || (side == 0 && g.broken(p, a))) continue;  // p below: its bit
          if (reach_voxel(r, p, vp)) return;
        }
      }
    }
  };
  std::vector<i32> active, islands;
  for (;;) {
    active.clear();
    for (size_t k = 0; k < S.size(); ++k) {
      const i32 r = static_cast<i32>(k);
      if (sp[r] == r && !S[r].supported && !S[r].exhausted) active.push_back(r);
    }
    if (active.empty()) break;
    if (!supports_removed && !any_supported && active.size() == 1) {
      S[active[0]].supported = true;
      if (stats) ++stats->shortcut;
      break;
    }
    for (i32 r0 : active) {
      i32 r = find(r0);
      if (r != r0 || S[r].supported || S[r].exhausted) continue;
      if (S[r].head < S[r].queue.size()) {
        const IVec3 c = S[r].queue[S[r].head++];
        if (stats) ++stats->visited;
        expand_voxel(r, c);
      } else if (S[r].chead < S[r].cqueue.size()) {
        const u64 k = S[r].cqueue[S[r].chead++];
        if (stats) ++stats->visited;
        expand_comp(r, k);
      } else {
        S[r].exhausted = true;
        islands.push_back(r);
      }
    }
  }
  std::vector<std::vector<IVec3>> out;
  std::vector<u64> far;
  for (i32 r : islands) {
    if (find(r) != r || S[r].supported) continue;
    std::vector<IVec3> isl = S[r].members;
    std::sort(isl.begin(), isl.end());
    for (u64 k : S[r].cmembers) {
      const IVec3& cc = slots[size_t(k >> 16)].first;
      far.push_back(key3(cc[0], cc[1], cc[2]));
    }
    if (!isl.empty()) out.push_back(std::move(isl));
  }
  if (nonresident && !far.empty()) {
    nonresident->insert(nonresident->end(), far.begin(), far.end());
    std::sort(nonresident->begin(), nonresident->end());
    nonresident->erase(std::unique(nonresident->begin(), nonresident->end()), nonresident->end());
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.front() < b.front(); });
  return out;
}

}  // namespace svx
