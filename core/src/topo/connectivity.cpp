#include "svx/topo/connectivity.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace svx {

namespace {

template <typename F>
inline void for_each_neighbor(const Lattice& L, i32 c, F&& f) {
  for (int a = 0; a < 3; ++a) {
    const i32 p = L.nbr[a][c];
    if (p >= 0 && f(p)) return;
    const i32 m = L.nbrm[a][c];
    if (m >= 0 && f(m)) return;
  }
}

// ... over uncracked bonds only (intact: carrying tension as well)
template <typename F>
inline void for_each_intact_neighbor(const Lattice& L, i32 c, F&& f) {
  for (int a = 0; a < 3; ++a) {
    const i32 p = L.nbr[a][c];
    if (p >= 0 && !L.is_cracked(a, c) && f(p)) return;
    const i32 m = L.nbrm[a][c];
    if (m >= 0 && !L.is_cracked(a, m) && f(m)) return;
  }
}

struct Search {
  std::vector<i32> members;
  std::vector<i32> queue;
  size_t head = 0;
  bool supported = false;
  bool exhausted = false;
};

std::vector<std::vector<i32>> run(const Lattice& L, const CutSet& cut, bool shortcut, ConnStats* stats,
                                  bool intact_only = false) {
  std::unordered_map<i32, i32> owner;  // cell -> search id (not necessarily a root)
  owner.reserve(cut.seeds.size() * 8 + 16);
  std::vector<Search> S;
  std::vector<i32> sp;  // union-find over searches
  auto find = [&](i32 x) {
    while (sp[x] != x) {
      sp[x] = sp[sp[x]];
      x = sp[x];
    }
    return x;
  };
  auto merge = [&](i32 a, i32 b) {  // roots; returns the new root
    if (a == b) return a;
    if (S[a].members.size() < S[b].members.size()) std::swap(a, b);
    Search& A = S[a];
    Search& B = S[b];
    A.members.insert(A.members.end(), B.members.begin(), B.members.end());
    A.queue.insert(A.queue.end(), B.queue.begin() + static_cast<long>(B.head), B.queue.end());
    A.supported = A.supported || B.supported;
    std::vector<i32>().swap(B.members);
    std::vector<i32>().swap(B.queue);
    sp[b] = a;
    return a;
  };
  bool any_supported = false;
  for (i32 s : cut.seeds) {
    if (s < 0 || s >= L.n || L.dead[s] || owner.count(s)) continue;
    const i32 id = static_cast<i32>(S.size());
    S.emplace_back();
    sp.push_back(id);
    S[id].members.push_back(s);
    S[id].queue.push_back(s);
    owner.emplace(s, id);
    if (L.support(s)) S[id].supported = any_supported = true;
    if (stats) ++stats->searches;
  }
  const bool shortcut_ok = shortcut && !cut.supports_removed;
  std::vector<i32> active, islands_roots;
  for (;;) {
    active.clear();
    for (size_t k = 0; k < S.size(); ++k) {
      const i32 r = static_cast<i32>(k);
      if (sp[r] == r && !S[r].supported && !S[r].exhausted) active.push_back(r);
    }
    if (active.empty()) break;
    if (shortcut_ok && !any_supported && active.size() == 1) {
      S[active[0]].supported = true;  // the last live search holds the pre-change support
      if (stats) ++stats->shortcut;
      break;
    }
    for (i32 r0 : active) {
      i32 r = find(r0);
      if (r != r0 || S[r].supported || S[r].exhausted) continue;
      if (S[r].head == S[r].queue.size()) {
        S[r].exhausted = true;
        islands_roots.push_back(r);
        continue;
      }
      const i32 c = S[r].queue[S[r].head++];
      if (stats) ++stats->visited;
      auto visit = [&](i32 nb) {
        if (L.dead[nb]) return false;
        if (L.support(nb)) {
          S[r].supported = any_supported = true;
          return true;
        }
        const auto it = owner.find(nb);
        if (it == owner.end()) {
          owner.emplace(nb, r);
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
      if (intact_only) for_each_intact_neighbor(L, c, visit);
      else for_each_neighbor(L, c, visit);
    }
  }
  std::vector<std::vector<i32>> out;
  for (i32 r : islands_roots) {
    if (find(r) != r || S[r].supported) continue;
    std::vector<i32> isl = S[r].members;
    std::sort(isl.begin(), isl.end());
    out.push_back(std::move(isl));
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.front() < b.front(); });
  return out;
}

}  // namespace

std::vector<std::vector<i32>> detached_islands(const Lattice& L, const CutSet& cut, ConnStats* stats) {
  return run(L, cut, true, stats);
}

std::vector<std::vector<i32>> detached_islands(const Lattice& L, std::span<const i32> seeds, ConnStats* stats) {
  CutSet c;
  c.seeds.assign(seeds.begin(), seeds.end());
  return run(L, c, false, stats);
}

std::vector<std::vector<i32>> contact_held_pieces(const Lattice& L, std::span<const i32> seeds, ConnStats* stats) {
  CutSet c;
  c.seeds.assign(seeds.begin(), seeds.end());
  return run(L, c, false, stats, true);
}

CutSet removal_cut(const Lattice& L, std::span<const i32> cells) {
  CutSet c;
  std::unordered_set<i32> removed(cells.begin(), cells.end());
  std::unordered_set<i32> done;
  for (i32 r : cells) {
    if (r < 0 || r >= L.n || L.dead[r] || done.count(r)) continue;
    std::vector<i32> stack{r}, surv;
    done.insert(r);
    while (!stack.empty()) {
      const i32 x = stack.back();
      stack.pop_back();
      if (L.support(x)) c.supports_removed = true;
      for_each_neighbor(L, x, [&](i32 nb) {
        if (L.dead[nb]) return false;
        if (removed.count(nb)) {
          if (done.insert(nb).second) stack.push_back(nb);
        } else {
          surv.push_back(nb);
        }
        return false;
      });
    }
    c.seeds.insert(c.seeds.end(), surv.begin(), surv.end());
  }
  std::sort(c.seeds.begin(), c.seeds.end());
  c.seeds.erase(std::unique(c.seeds.begin(), c.seeds.end()), c.seeds.end());
  return c;
}

std::vector<std::vector<i32>> unsupported_components(const Lattice& L) {
  std::vector<u8> seen(L.n, 0);
  std::vector<std::vector<i32>> out;
  std::vector<i32> q;
  for (i32 s = 0; s < L.n; ++s) {
    if (seen[s] || L.dead[s] || L.anchored[s]) continue;
    q.clear();
    q.push_back(s);
    seen[s] = 1;
    bool supported = false;
    for (size_t h = 0; h < q.size(); ++h) {
      const i32 c = q[h];
      if (L.support(c)) supported = true;
      for_each_neighbor(L, c, [&](i32 nb) {
        if (!L.dead[nb] && !seen[nb]) {
          seen[nb] = 1;
          q.push_back(nb);
        }
        return false;
      });
    }
    if (!supported) {
      std::vector<i32> isl = q;
      std::sort(isl.begin(), isl.end());
      out.push_back(std::move(isl));
    }
  }
  return out;
}

}  // namespace svx
