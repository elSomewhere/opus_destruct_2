// structvox — rigid pieces: creation from the world, stress under contact and inertia, fracture
// and splitting, carving, blasts, lifecycle events (docs/V2_DESIGN.md §4).
#include <algorithm>
#include <map>
#include <tuple>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#include "svx/base/diag.hpp"
#include "svx/base/mem.hpp"
#include "svx/base/parallel.hpp"
#include "svx/world/world.hpp"
#include "world_internal.hpp"

namespace svx {

using namespace world_detail;

Body* World::Impl::make_body_from_world(const std::vector<FragKey>& frags, const V3& v, const V3& w, const V3* about) {
  if (frags.empty()) return nullptr;
  auto b = std::make_unique<Body>();
  // One shape per grid its fragments are in (in order of first appearance); the body's frame is
  // its first shape's lattice.
  std::vector<u16> grids;               // shape -> grid
  std::vector<std::vector<IVec3>> vox;  // shape -> its voxels
  std::vector<std::vector<i32>> vfrag;  // ... and their body fragments
  for (const FragKey& f : frags) {
    FragChunk* fc = frag_chunk_if(f);
    if (!fc || f.idx < 0 || f.idx >= static_cast<i32>(fc->frags.size())) continue;
    const FragInfo& fi = fc->frags[size_t(f.idx)];
    if (fi.count <= 0) continue;
    size_t k = 0;
    while (k < grids.size() && grids[k] != f.grid) ++k;
    if (k == grids.size()) {
      grids.push_back(f.grid);
      vox.emplace_back();
      vfrag.emplace_back();
    }
    const size_t before = vox[k].size();
    voxels_of(f, vox[k]);
    if (vox[k].size() == before) continue;
    const i32 j = static_cast<i32>(b->frags.size());
    for (size_t q = before; q < vox[k].size(); ++q) vfrag[k].push_back(j);
    BodyFrag bf;
    bf.com = fi.com;
    bf.mass = fi.mass;
    bf.inertia = fi.inertia;
    bf.mat = fi.mat;
    bf.count = static_cast<i32>(vox[k].size() - before);
    bf.strength = class_mult(frag_class(f));
    bf.shape = static_cast<u16>(k);
    b->frags.push_back(bf);
  }
  size_t total = 0;
  for (const auto& vk : vox) total += vk.size();
  if (total == 0) return nullptr;
  const LatticeXf& X0 = xf_of(grids[0]);
  const LatticeXf into0 = inverse(X0);  // (the world -> the first shape's lattice)
  // (the voxels' world box, to wake what rests on them)
  V3 wlo{INFINITY, INFINITY, INFINITY}, whi{-INFINITY, -INFINITY, -INFINITY};
  b->shapes.resize(grids.size());
  for (size_t k = 0; k < grids.size(); ++k) {
    const u16 g = grids[k];
    VoxelGrid& G = vg(g);
    BodyShape& S = b->shapes[k];
    const f64 h = G.h;
    S.grid = id_of(g);
    S.h = h;
    S.priority = gs(g).priority;
    S.xf = k == 0 ? LatticeXf{} : compose(into0, xf_of(g));
    IVec3 lo{INT_MAX, INT_MAX, INT_MAX}, hi{INT_MIN, INT_MIN, INT_MIN};
    for (const IVec3& p : vox[k])
      for (int a = 0; a < 3; ++a) {
        lo[a] = std::min(lo[a], p[a]);
        hi[a] = std::max(hi[a], p[a]);
      }
    S.lo = lo;
    S.dim = {hi[0] - lo[0] + 1, hi[1] - lo[1] + 1, hi[2] - lo[2] + 1};
    const size_t cells = size_t(S.dim[0]) * size_t(S.dim[1]) * size_t(S.dim[2]);
    S.vox.assign(cells, kAir);
    S.frag.assign(cells, 0);
    S.brk.assign(cells, 0);
    for (size_t q = 0; q < vox[k].size(); ++q) {
      const i32 i = S.index(vox[k][q]);
      S.vox[size_t(i)] = static_cast<Vox>(G.get(vox[k][q]) & ~kAnchorBit);
      S.frag[size_t(i)] = static_cast<u32>(vfrag[k][q] + 1);
      ++S.count;
    }
    // (the voxels' layer values go with them)
    for (int L = 0; L < static_cast<int>(ext_.layers.size()); ++L)
      for (const IVec3& p : vox[k]) {
        const u8 lv = G.layer(L, p);
        if (!lv) continue;
        if (S.layer[size_t(L)].empty()) S.layer[size_t(L)].assign(cells, 0);
        S.layer[size_t(L)][size_t(S.index(p))] = lv;
        G.set_layer(L, p, 0);
      }
    for (const IVec3& p : vox[k]) {
      const i32 i = S.index(p);
      for (int a = 0; a < 3; ++a) {
        IVec3 q = p;
        q[a] += 1;
        const i32 j = S.index(q);
        if (j < 0 || !vox_solid(S.vox[size_t(j)])) continue;
        if (G.broken(p, a)) S.brk[size_t(i)] |= static_cast<u8>(1u << a);
      }
    }
    // (junction samples broken in the world stay broken between its shapes)
    if (grids.size() > 1)
      for (const IVec3& p : vox[k]) {
        const Chunk* ch = G.chunk(chunk_of(p));
        if (!ch || ch->jbroken.empty()) continue;
        const u32 li = static_cast<u32>(chunk_index(p));
        auto it = std::lower_bound(ch->jbroken.begin(), ch->jbroken.end(), li << 9);
        for (; it != ch->jbroken.end() && (*it >> 9) == li; ++it) S.break_junction(S.index(p), static_cast<int>((*it >> 6) & 7), static_cast<int>(*it & 63));
      }
    // (its world box)
    const V3 llo{h * (lo[0] - 0.5), h * (lo[1] - 0.5), h * (lo[2] - 0.5)}, lhi{h * (hi[0] + 0.5), h * (hi[1] + 0.5), h * (hi[2] + 0.5)};
    for (int c = 0; c < 8; ++c) {
      const V3 cw = xf_of(g).to(V3{(c & 1) ? lhi.x : llo.x, (c & 2) ? lhi.y : llo.y, (c & 4) ? lhi.z : llo.z});
      for (int a = 0; a < 3; ++a) {
        wlo[a] = std::min(wlo[a], cw[a]);
        whi[a] = std::max(whi[a], cw[a]);
      }
    }
  }
  // (fragments of the other shapes: into the body frame)
  for (BodyFrag& bf : b->frags) {
    const LatticeXf& X = b->shapes[bf.shape].xf;
    if (X.identity) continue;
    bf.com = X.to(bf.com);
    bf.inertia = X.R * bf.inertia * X.Rt;
  }
  body_refresh(*b, grid_.h, cfg_.rigid.max_points);
  b->id = next_id_++;
  b->x = X0.to(b->com);
  b->q = X0.q;
  b->v = about ? v + cross(w, b->x - *about) : v;
  b->w = w;
  b->v_pre = b->v;
  b->w_pre = w;
  b->refresh_box();
  // the voxels leave their grids; the fragment caches that are current are patched in place (the
  // structures holding the remaining fragments stay valid)
  std::unordered_map<GKey, bool, GKeyHash> current;
  for (const FragKey& f : frags) {
    const GKey key{f.grid, f.chunk};
    if (current.count(key)) continue;
    FragChunk* fc = frag_chunk_if(f);
    const Chunk* ch = vg(f.grid).chunk(unkey3(f.chunk));
    current[key] = fc && ch && fc->vox_version == ch->vox_version;
  }
  for (size_t k = 0; k < grids.size(); ++k)
    for (const IVec3& p : vox[k]) vg(grids[k]).set(p, kAir);
  for (const FragKey& f : frags) {
    if (!current[GKey{f.grid, f.chunk}]) continue;
    FragChunk* fc = frag_chunk_if(f);
    const Chunk* ch = vg(f.grid).chunk(unkey3(f.chunk));
    if (!fc || !ch || f.idx < 0 || f.idx >= static_cast<i32>(fc->frags.size())) continue;  // (as the first loop)
    for (i32 k = fc->vox_start[size_t(f.idx)]; k < fc->vox_start[size_t(f.idx) + 1]; ++k) {
      const i32 i = fc->vox[size_t(k)];
      if (fc->id[size_t(i)] == static_cast<u16>(f.idx + 1)) fc->id[size_t(i)] = 0;
    }
    fc->frags[size_t(f.idx)].count = 0;
    fc->vox_version = ch->vox_version;
    auto& owner = gs(f.grid).owner;
    auto ot = owner.find(f.chunk);
    if (ot != owner.end() && f.idx < static_cast<i32>(ot->second.size())) ot->second[size_t(f.idx)] = 0;
  }
  // (what held on to them alone - edge to edge, corner to corner - is checked for support again:
  // after the caches are patched, so that what they hold is known, not seeded blind)
  if (cfg_.recheck_vacated)
    for (size_t k = 0; k < grids.size(); ++k) recheck_vacated(grids[k], b->shapes[k]);
  if (grids.size() == 1 && grids[0] == 0) {
    const f64 h = grid_.h;
    const V3 m{h, h, h};
    const BodyShape& S = b->shapes[0];
    const IVec3 lo = S.lo, hi{S.lo[0] + S.dim[0] - 1, S.lo[1] + S.dim[1] - 1, S.lo[2] + S.dim[2] - 1};
    rigid_.wake_box(V3{h * lo[0], h * lo[1], h * lo[2]} - m * 2.0, V3{h * hi[0], h * hi[1], h * hi[2]} + m * 2.0);
  } else {
    f64 hm = 0.0;
    for (const BodyShape& S : b->shapes) hm = std::max(hm, S.h);
    const V3 m{hm, hm, hm};
    rigid_.wake_box(wlo - m * 2.0, whi + m * 2.0);
  }
  st_.detached_voxels += static_cast<i64>(total);
  if (static_cast<i32>(total) < cfg_.min_body_voxels) {
    // (a shard: dust and a few chips, not a rigid piece)
    dust_event(b->x, v, static_cast<i32>(total), false);
    st_.pulverized_voxels += static_cast<i64>(total);
    return nullptr;
  }
  ++st_.detached_pieces;
  Body* ptr = b.get();
  rigid_.add(std::move(b));
  if (!att_.joints.empty()) joints_to_piece(*ptr);  // (joints on its voxels hold on to it now)
  if (!att_.wheels.empty()) wheels_to_piece(*ptr);  // (wheels too: a carrier dropped in)
  return ptr;
}

namespace {

// Junction samples between the shapes of a body (in its frame): each shape's exposed faces
// sampled into the other shapes (JSample: vg, og are shape indices).
void body_junctions(const Body& b, i32 S, f64 reach, std::vector<JSample>& out) {
  const size_t ns = b.shapes.size();
  if (ns < 2) return;
  // (the shapes' boxes in the body frame: pairs that cannot meet are skipped)
  std::vector<V3> lo(ns), hi(ns);
  for (size_t k = 0; k < ns; ++k) {
    const BodyShape& Sk = b.shapes[k];
    const f64 h = Sk.h;
    lo[k] = V3{INFINITY, INFINITY, INFINITY};
    hi[k] = V3{-INFINITY, -INFINITY, -INFINITY};
    const f64 m = (1.0 + reach) * h;
    for (int c = 0; c < 8; ++c) {
      const V3 p{h * ((c & 1) ? Sk.lo[0] + Sk.dim[0] - 0.5 : Sk.lo[0] - 0.5), h * ((c & 2) ? Sk.lo[1] + Sk.dim[1] - 0.5 : Sk.lo[1] - 0.5),
                 h * ((c & 4) ? Sk.lo[2] + Sk.dim[2] - 0.5 : Sk.lo[2] - 0.5)};
      const V3 w = Sk.xf.to(p);
      for (int a = 0; a < 3; ++a) {
        lo[k][a] = std::min(lo[k][a], w[a] - m);
        hi[k][a] = std::max(hi[k][a], w[a] + m);
      }
    }
  }
  for (size_t k = 0; k < ns; ++k) {
    const BodyShape& A = b.shapes[k];
    const f64 h = A.h;
    std::vector<size_t> others;
    for (size_t l = 0; l < ns; ++l)
      if (l != k && !(hi[l].x < lo[k].x || lo[l].x > hi[k].x || hi[l].y < lo[k].y || lo[l].y > hi[k].y || hi[l].z < lo[k].z ||
                      lo[l].z > hi[k].z))
        others.push_back(l);
    if (others.empty()) continue;
    for (i32 i = 0; i < static_cast<i32>(A.vox.size()); ++i) {
      if (!vox_solid(A.vox[size_t(i)])) continue;
      const IVec3 p = A.voxel(i);
      for (int face = 0; face < 6; ++face) {
        IVec3 q = p;
        q[face >> 1] += (face & 1) ? 1 : -1;
        if (vox_solid(A.get(q))) continue;
        for (int sub = 0; sub < S * S; ++sub) {
          if (!A.jbrk.empty() && A.junction_broken(i, face, sub)) continue;
          f64 pushed = -1.0;
          V3 X;
          for (size_t l : others) {
            const BodyShape& B = b.shapes[l];
            // (junction_reach voxels of the other shape out of the face, as in the world)
            if (const f64 push = reach * B.h; push != pushed) {
              X = A.xf.to(junction_point(p, face, sub, S, h, push));
              pushed = push;
            }
            const IVec3 o = voxel_of(B.xf.from(X), B.h);
            const i32 oi = B.index(o);
            if (oi < 0 || !vox_solid(B.vox[size_t(oi)])) continue;
            // (as in the world: an interface is measured by its owner's faces alone)
            if (!(A.priority != B.priority ? A.priority > B.priority : A.grid > B.grid)) break;
            out.push_back({p, o, static_cast<u16>(k), static_cast<u16>(l), static_cast<u8>(face), static_cast<u8>(sub), 0});
            break;
          }
        }
      }
    }
  }
}

}  // namespace

void World::Impl::rebuild_body_graph(Body& b) {
  const f64 h = grid_.h;
  // (made anew: a graph takes what its piece needs now, however large it was before a split)
  b.graph = std::make_shared<BodyGraph>();
  BodyGraph& G = *b.graph;
  G.P.mats = mats_.get();
  const i32 nf = static_cast<i32>(b.frags.size());
  // fragment-level bonds from the shapes
  std::vector<SecAcc> fine;
  std::unordered_map<u64, i32> index;
  for (size_t sk = 0; sk < b.shapes.size(); ++sk) {
    const BodyShape& S = b.shapes[sk];
    const i32 cells = static_cast<i32>(S.vox.size());
    for (i32 i = 0; i < cells; ++i) {
      if (!vox_solid(S.vox[size_t(i)])) continue;
      const i32 fp = static_cast<i32>(S.frag[size_t(i)]) - 1;
      if (fp < 0) continue;
      const IVec3 p = S.voxel(i);
      for (int a = 0; a < 3; ++a) {
        if ((S.brk[size_t(i)] >> a) & 1) continue;
        IVec3 q = p;
        q[a] += 1;
        const i32 j = S.index(q);
        if (j < 0 || !vox_solid(S.vox[size_t(j)])) continue;
        const i32 fq = static_cast<i32>(S.frag[size_t(j)]) - 1;
        if (fq == fp || fq < 0) continue;
        const u64 key = acc_key(fp, fq, a, 1);
        auto it = index.find(key);
        i32 ai;
        if (it == index.end()) {
          ai = static_cast<i32>(fine.size());
          index.emplace(key, ai);
          fine.emplace_back();
          SecAcc& A = fine.back();
          A.a = std::min(fp, fq);
          A.b = std::max(fp, fq);
          A.grid = static_cast<u16>(sk);
        } else {
          ai = it->second;
        }
        fine[size_t(ai)].add(p, a, fp < fq ? 1 : -1);
      }
    }
  }
  // junctions between its shapes (both sides' samples, half each)
  const i32 JS = std::clamp(cfg_.junction_samples, 1, 7);
  if (b.shapes.size() > 1) {
    std::vector<JSample> js;
    body_junctions(b, JS, std::clamp(cfg_.junction_reach, 0.0, 2.0), js);
    for (const JSample& j : js) {
      const BodyShape& A = b.shapes[j.vg];
      const BodyShape& B = b.shapes[j.og];
      const i32 fp = static_cast<i32>(A.frag[size_t(A.index(j.v))]) - 1;
      const i32 fq = static_cast<i32>(B.frag[size_t(B.index(j.o))]) - 1;
      if (fp < 0 || fq < 0 || fp == fq) continue;
      const u64 key = acc_key(fp, fq, 0, 1);
      auto it = index.find(key);
      i32 ai;
      if (it == index.end()) {
        ai = static_cast<i32>(fine.size());
        index.emplace(key, ai);
        fine.emplace_back();
        SecAcc& A2 = fine.back();
        A2.a = std::min(fp, fq);
        A2.b = std::max(fp, fq);
        A2.grid = j.vg;
      } else {
        ai = it->second;
      }
      fine[size_t(ai)].add_sample(j, fp < fq ? 1 : -1, junction_weight(j));
    }
  }
  // resolution: clusters of fragments for large pieces (cells in the body frame, within a shape)
  i64 live = 0;
  for (const BodyFrag& f : b.frags) live += f.count > 0 ? 1 : 0;
  // (pieces: per fragment up to body_cluster_nodes fragments, 1 m cells up to 3x that, 2 m
  // beyond: a large piece cracks along coarse seams first, and finer once it is smaller)
  const i32 cell = live <= cfg_.body_cluster_nodes ? 0 : (live <= 3 * static_cast<i64>(cfg_.body_cluster_nodes) ? 8 : 16);
  std::vector<u64> key(static_cast<size_t>(nf));
  for (i32 f = 0; f < nf; ++f) {
    if (cell <= 0) {
      key[size_t(f)] = static_cast<u64>(f);
      continue;
    }
    const V3& c = b.frags[size_t(f)].com;
    key[size_t(f)] = key3(static_cast<i32>(std::floor(c.x / (h * cell))), static_cast<i32>(std::floor(c.y / (h * cell))),
                          static_cast<i32>(std::floor(c.z / (h * cell))));
  }
  std::vector<std::pair<i32, i32>> links;
  for (const SecAcc& A : fine) links.push_back({A.a, A.b});
  std::vector<u16> group;
  if (b.shapes.size() > 1) {
    group.resize(size_t(nf));
    for (i32 f = 0; f < nf; ++f) group[size_t(f)] = b.frags[size_t(f)].shape;
  }
  std::vector<i32> cl;
  cluster_items(key, links, &cl, group.empty() ? nullptr : &group);
  // nodes: clusters of live fragments (renumbered densely)
  G.frag_node.assign(static_cast<size_t>(nf), -1);
  std::vector<i32> node_of_cluster(static_cast<size_t>(nf), -1);
  G.node_com.clear();
  G.node_mass.clear();
  G.node_inertia.clear();
  std::vector<MaterialId> nmat;
  std::vector<f64> nstr, heavy;
  for (i32 f = 0; f < nf; ++f) {
    const BodyFrag& bf = b.frags[size_t(f)];
    if (bf.count <= 0) continue;
    i32& nd = node_of_cluster[size_t(cl[size_t(f)])];
    if (nd < 0) {
      nd = static_cast<i32>(G.node_mass.size());
      G.node_mass.push_back(0.0);
      G.node_com.push_back(V3{});
      G.node_inertia.push_back(M3{});
      nmat.push_back(bf.mat);
      nstr.push_back(1e30);
      heavy.push_back(-1.0);
    }
    G.frag_node[size_t(f)] = nd;
    G.node_mass[size_t(nd)] += bf.mass;
    G.node_com[size_t(nd)] += bf.com * bf.mass;
    nstr[size_t(nd)] = std::min(nstr[size_t(nd)], bf.strength);
    if (bf.mass > heavy[size_t(nd)]) {
      heavy[size_t(nd)] = bf.mass;
      nmat[size_t(nd)] = bf.mat;
    }
  }
  const i32 n = static_cast<i32>(G.node_mass.size());
  for (i32 i = 0; i < n; ++i)
    if (G.node_mass[size_t(i)] > 0) G.node_com[size_t(i)] *= 1.0 / G.node_mass[size_t(i)];
  for (i32 f = 0; f < nf; ++f) {
    const i32 nd = G.frag_node[size_t(f)];
    if (nd < 0) continue;
    const BodyFrag& bf = b.frags[size_t(f)];
    const V3 d = bf.com - G.node_com[size_t(nd)];
    const f64 dd = dot(d, d);
    M3& I = G.node_inertia[size_t(nd)];
    for (int r = 0; r < 3; ++r)
      for (int q = 0; q < 3; ++q) I(r, q) += bf.inertia(r, q) + bf.mass * ((r == q ? dd : 0.0) - d[r] * d[q]);
  }
  for (i32 i = 0; i < n; ++i) {
    SNode nd;
    nd.c = G.node_com[size_t(i)];
    nd.mass = G.node_mass[size_t(i)];
    G.P.nodes.push_back(nd);
  }
  // the reference node: nearest the centre of mass
  i32 pin = 0;
  f64 best = 1e300;
  for (i32 i = 0; i < n; ++i) {
    const f64 d = norm2(G.P.nodes[size_t(i)].c - b.com);
    if (d < best) {
      best = d;
      pin = i;
    }
  }
  if (n > 0) G.P.nodes[size_t(pin)].fixed = true;
  const std::vector<SecAcc> merged = merge_accs(fine, [&](i32 f) { return G.frag_node[size_t(f)]; });
  G.face_start.assign(1, 0);
  G.face_p.clear();
  G.face_axis.clear();
  G.face_shape.clear();
  G.jstart.assign(1, 0);
  G.jref.clear();
  auto xf = [&b](u16 k) -> const LatticeXf& { return b.shapes[k].xf; };
  auto hx = [&b](u16 k) { return b.shapes[k].h; };
  auto at = [&](u16 k, const IVec3& p) { return piece_voxel_at(b, k, p); };
  for (const SecAcc& A0 : merged) {
    if (A0.a < 0 || A0.b < 0) continue;
    SecAcc A = A0;
    A.mb = nmat[size_t(A.b)];
    A.strength_b = nstr[size_t(A.b)];
    SBond B = A.finish(hx, xf, JS, G.P.nodes[size_t(A.a)].c, &G.P.nodes[size_t(A.b)].c, nmat[size_t(A.a)], nstr[size_t(A.a)]);
    if (A.js.empty())
      section_strengths(mats(), A.faces.data(), A.fax.data(), A.faces.size(), [&](const IVec3& p) { return piece_voxel_at(b, A.grid, p); }, B);
    else
      section_strengths_general(mats(), A.grid, A.faces.data(), A.fax.data(), A.faces.size(), A.js.data(), A.js.size(), at, B, JS);
    B.tag = static_cast<i32>(G.P.bonds.size());
    G.P.bonds.push_back(B);
    for (size_t k = 0; k < A.faces.size(); ++k) {
      G.face_p.push_back(A.faces[k]);
      G.face_axis.push_back(A.fax[k]);
    }
    G.face_start.push_back(static_cast<i32>(G.face_p.size()));
    G.face_shape.push_back(A.grid);
    G.jref.insert(G.jref.end(), A.js.begin(), A.js.end());
    G.jstart.push_back(static_cast<i32>(G.jref.size()));
  }
  G.u.assign(6 * size_t(n), 0.0);
  {
    std::vector<i32> comp;
    std::vector<u8> seed(static_cast<size_t>(n), 0);
    if (n > 0) seed[0] = 1;
    G.components = n > 0 ? graph_components(n, G.P.bonds, seed, &comp) : 0;
  }
  // (what grew by appending, at its size: pieces are many, and each keeps its graph)
  G.P.nodes.shrink_to_fit();
  G.P.bonds.shrink_to_fit();
  G.node_com.shrink_to_fit();
  G.node_mass.shrink_to_fit();
  G.node_inertia.shrink_to_fit();
  G.face_start.shrink_to_fit();
  G.face_p.shrink_to_fit();
  G.face_axis.shrink_to_fit();
  G.face_shape.shrink_to_fit();
  G.jstart.shrink_to_fit();
  G.jref.shrink_to_fit();
  b.graph_dirty = false;
}

void World::Impl::refragment_body(Body& b) {
  std::vector<BodyFrag> nf;
  std::vector<std::array<f64, 10>> sums;
  std::vector<i32> stack;
  for (size_t sk = 0; sk < b.shapes.size(); ++sk) {
    BodyShape& S = b.shapes[sk];
    const f64 h = S.h;
    const i32 cells = static_cast<i32>(S.vox.size());
    std::vector<i32> nl(size_t(cells), -1);
    const i32 stride[3] = {S.dim[1] * S.dim[2], S.dim[2], 1};
    const size_t first = nf.size();
    S.count = 0;
    for (i32 i = 0; i < cells; ++i) {
      if (!vox_solid(S.vox[size_t(i)]) || nl[size_t(i)] >= 0) continue;
      const u32 lab = S.frag[size_t(i)];
      const i32 f = static_cast<i32>(nf.size());
      BodyFrag bf;
      bf.mat = vox_mat(S.vox[size_t(i)]);
      bf.strength = (lab > 0 && lab <= b.frags.size()) ? b.frags[size_t(lab - 1)].strength : 1.0;
      bf.shape = static_cast<u16>(sk);
      nf.push_back(bf);
      sums.push_back({});
      nl[size_t(i)] = f;
      stack.assign(1, i);
      while (!stack.empty()) {
        const i32 k = stack.back();
        stack.pop_back();
        const IVec3 p = S.voxel(k);
        ++nf[size_t(f)].count;
        ++S.count;
        accumulate_voxel(mats()[vox_mat(S.vox[size_t(k)])].rho * h * h * h, V3{h * p[0], h * p[1], h * p[2]}, h,
                         sums[size_t(f)].data());
        const IVec3 l{p[0] - S.lo[0], p[1] - S.lo[1], p[2] - S.lo[2]};
        for (int a = 0; a < 3; ++a)
          for (int sg = -1; sg <= 1; sg += 2) {
            if (l[a] + sg < 0 || l[a] + sg >= S.dim[a]) continue;
            const i32 j = k + sg * stride[a];
            if (nl[size_t(j)] >= 0 || !vox_solid(S.vox[size_t(j)]) || S.frag[size_t(j)] != lab) continue;
            const bool broken = sg > 0 ? ((S.brk[size_t(k)] >> a) & 1) : ((S.brk[size_t(j)] >> a) & 1);
            if (broken) continue;
            nl[size_t(j)] = f;
            stack.push_back(j);
          }
      }
    }
    for (size_t f = first; f < nf.size(); ++f) {
      const MassProps mp = finish_mass(sums[f].data(), h);
      nf[f].mass = mp.mass;
      nf[f].com = mp.com;
      nf[f].inertia = mp.inertia;
      if (!S.xf.identity) {
        nf[f].com = S.xf.to(nf[f].com);
        nf[f].inertia = S.xf.R * nf[f].inertia * S.xf.Rt;
      }
    }
    for (i32 i = 0; i < cells; ++i) S.frag[size_t(i)] = nl[size_t(i)] >= 0 ? static_cast<u32>(nl[size_t(i)] + 1) : 0;
  }
  b.count = 0;
  for (const BodyShape& S : b.shapes) b.count += S.count;
  b.frags.swap(nf);
  b.graph_dirty = true;
}

std::vector<i32> World::Impl::body_stress(Body& b, const std::vector<PointForce>& forces, bool inertia, f64 energy,
                                     std::vector<i32>* crushed) {
  StressOut o;
  body_stress_run(b, forces, inertia, energy, o);
  apply_stress_out(o);
  if (crushed) *crushed = o.crushed;
  return o.broken;
}

void World::Impl::apply_stress_out(const StressOut& o) {
  st_.body_checks += o.checks;
  st_.pcg_iters += o.pcg_iters;
  st_.bonds_broken += static_cast<i64>(o.broken.size());
  st_.impact_breaks += o.impact_breaks;
  st_.steady_breaks += o.steady_breaks;
  for (int m = 0; m < 4; ++m) st_.mode_breaks[m] += o.modes[m];
  for (const auto& [p, n] : o.cracks) crack_event(p, n, 1.0);
}

void World::Impl::body_stress_run(Body& b, const std::vector<PointForce>& forces, bool inertia, f64 energy, StressOut& o) {
  std::vector<i32>& out = o.broken;
  if (b.graph_dirty || !b.graph) rebuild_body_graph(b);
  BodyGraph& G = *b.graph;
  if (G.P.bonds.empty()) return;
  const i32 n = static_cast<i32>(G.P.nodes.size());
  const M3 R = to_matrix(b.q), Rt = transpose(R);
  std::vector<f64> f(6 * size_t(n), 0.0);
  const V3 g{0, 0, -cfg_.rigid.gravity};
  if (inertia) {
    V3 Ft, tau;
    for (const PointForce& pf : forces) {
      Ft += pf.F;
      tau += cross(pf.p - b.x, pf.F);
    }
    const M3 Iw = R * b.inertia * Rt;
    const M3 Iwi = R * b.inv_inertia * Rt;
    const V3 a = Ft * b.inv_mass + g;
    const V3 alpha = Iwi * (tau - cross(b.w, Iw * b.w));
    for (i32 i = 0; i < n; ++i) {
      const V3 rw = R * (G.node_com[size_t(i)] - b.com);
      const V3 ai = a + cross(alpha, rw) + cross(b.w, cross(b.w, rw));
      const V3 Fi = Rt * ((g - ai) * G.node_mass[size_t(i)]);
      const M3 Ii = R * G.node_inertia[size_t(i)] * Rt;
      const V3 Mi = Rt * ((Ii * alpha + cross(b.w, Ii * b.w)) * -1.0);
      f64* fi = &f[6 * size_t(i)];
      fi[0] += Fi.x;
      fi[1] += Fi.y;
      fi[2] += Fi.z;
      fi[3] += Mi.x;
      fi[4] += Mi.y;
      fi[5] += Mi.z;
    }
  }
  for (const PointForce& pf : forces) {
    if (pf.frag < 0 || pf.frag >= static_cast<i32>(G.frag_node.size())) continue;
    const i32 i = G.frag_node[size_t(pf.frag)];
    if (i < 0) continue;
    StressProblem::add_force(f, i, G.P.nodes[size_t(i)].c, Rt * pf.F, b.to_shape(pf.p));
  }
  if (!G.P.assembled()) {
    const auto a0 = std::chrono::steady_clock::now();
    StressOptions so = solver_options();
    so.rtol = cfg_.body_stress_rtol;
    so.amg_min_nodes = 0;  // (small pieces: the coarsest level is the whole graph, solved exactly)
    const bool ok = G.P.assemble(so);
    o.assemble_ms += std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - a0).count();
    if (!ok) return;
  }
  // Break rounds: a progressive failure within the substep (steps of a sequentially linear
  // analysis). The worst bonds go first, the load redistributes, and the first time the piece
  // comes apart the rounds stop. The parts then separate (the contact step is solved again
  // without them), so the part above a failed storey keeps falling and meets what is below in its
  // own collision: collapse and breakup proceed through the collisions, crack by crack, rather
  // than as one overloaded solve that pulverizes everything at once. An impact pays for its
  // cracks from the energy the collision dissipates; a resting load (gravity) is not limited.
  const bool impact = energy >= 0.0;
  const i32 rounds = std::max(1, cfg_.impact_rounds);
  static const bool dbg = diag("SVX_DEBUG_BODY");
  f64 spent = 0.0;
  std::vector<std::pair<f64, i32>> over;
  std::vector<FailMode> modes(G.P.bonds.size(), FailMode::None);
  std::vector<i32> comp;
  bool rebuilt = false;
  for (i32 round = 0; round < rounds; ++round) {
    const auto s0 = std::chrono::steady_clock::now();
    PcgResult r = G.P.solve(f, G.u, cfg_.body_stress_rtol, cfg_.body_stress_maxit, true);
    o.solve_ms += std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - s0).count();
    if (!r.converged && (r.breakdown || r.rel_res > 5.0 * cfg_.body_stress_rtol) && round > 0 && !rebuilt) {
      // (the multigrid of the intact piece is stale after the breaks and the chips' retirement:
      // rebuilt once for the damaged piece)
      rebuilt = true;
      G.P.invalidate();
      StressOptions so = solver_options();
      so.rtol = cfg_.body_stress_rtol;
      so.amg_min_nodes = 0;
      if (G.P.assemble(so)) r = G.P.solve(f, G.u, cfg_.body_stress_rtol, cfg_.body_stress_maxit, true);
    }
    if (!r.converged && (r.breakdown || r.rel_res > 5.0 * cfg_.body_stress_rtol)) {
      if (dbg && b.count > 5000)
        std::printf("    [round %d] solve failed: pcg %d rel %.1e breakdown %d\n", round, r.iters, r.rel_res, r.breakdown ? 1 : 0);
      G.P.invalidate();  // (no reliable answer: nothing more breaks; the next check starts afresh)
      std::fill(G.u.begin(), G.u.end(), 0.0);
      break;
    }
    ++o.checks;
    o.pcg_iters += r.iters;
    over.clear();
    f64 maxphi = 0.0;
    for (i32 k = 0; k < static_cast<i32>(G.P.bonds.size()); ++k) {
      const SBond& B = G.P.bonds[size_t(k)];
      if (B.broken || G.P.nodes[size_t(B.a)].gone) continue;
      const f64 phi = bond_utilization(B, G.P.bond_load(k, G.u), par_.fragility, mats(), &modes[size_t(k)]);
      maxphi = std::max(maxphi, phi);
      if (phi >= 1.0) over.push_back({phi, k});
    }
    if (round == 0) b.last_phi = maxphi;
    if (dbg && b.count > 5000) {
      f64 fs = 0.0;
      for (const PointForce& pf : forces) fs += norm(pf.F);
      std::printf("  [body t%lld r%d] id %lld: %d voxels, %zu nodes (%zu bonds), %zu forces sum %.3g N (weight %.3g), pcg %d (%.1e), max phi %.2f, %zu over, energy %.3g (spent %.3g)\n",
                  static_cast<long long>(st_.ticks), round, static_cast<long long>(b.id), b.count, G.P.nodes.size(), G.P.bonds.size(),
                  forces.size(), fs, b.mass * cfg_.rigid.gravity, r.iters, r.rel_res, maxphi, over.size(), energy, spent);
    }
    if (over.empty()) break;
    std::sort(over.begin(), over.end(), [](const auto& x, const auto& y) { return x.first > y.first || (x.first == y.first && x.second < y.second); });
    // (an impact's rounds are steps of a sequentially linear analysis: the worst bonds, and at
    // least the worst fraction of the overloaded ones, so that the failure reaches through the
    // piece in a few rounds)
    const f64 thr = std::max(1.0, cfg_.break_band * over.front().first);
    const size_t quota = static_cast<size_t>(std::ceil(cfg_.impact_round_fraction * static_cast<f64>(over.size())));
    const size_t before = out.size();
    bool poor = false;
    for (size_t q = 0; q < over.size(); ++q) {
      const f64 phi = over[q].first;
      const i32 k = over[q].second;
      if ((phi < thr && q >= quota) || static_cast<i32>(out.size() - before) >= cfg_.max_breaks_per_round) break;
      if (impact) {
        // A crack costs its fracture energy, crushing costs much more; an impact has only what it
        // takes out of the motion.
        const SBond& B = G.P.bonds[size_t(k)];
        const f64 crush = modes[size_t(k)] == FailMode::Crush ? cfg_.crush_energy : 1.0;
        const f64 cost = cfg_.fracture_energy * crush * B.area * std::min(mats()[B.ma].Gf, mats()[B.mb].Gf) * B.strength;
        if (spent + cost > energy) {
          poor = true;
          break;
        }
        spent += cost;
        o.spent += cost;
      }
      out.push_back(k);
    }
    for (size_t q = before; q < out.size(); ++q) {
      const i32 k = out[q];
      // (a ductile section bent past its strength: a plastic hinge, if the piece comes apart there)
      if (cfg_.plastic_hinges && G.P.bonds[size_t(k)].b >= 0) {
        const SBond& B = G.P.bonds[size_t(k)];
        const auto rot = [&](i32 i) { return V3{G.u[6 * size_t(i) + 3], G.u[6 * size_t(i) + 4], G.u[6 * size_t(i) + 5]}; };
        HingeCut h;
        if (hinge_of(B, G.P.bond_load(k, G.u), rot(B.a), rot(B.b), &h)) o.hinges.push_back(h);
      }
      G.P.remove_bond(k);
      const SBond& B = G.P.bonds[size_t(k)];
      BodyShape& Sk = b.shapes[G.face_shape[size_t(k)]];
      for (i32 e = G.face_start[size_t(k)]; e < G.face_start[size_t(k) + 1]; ++e) {
        const i32 i = Sk.index(G.face_p[size_t(e)]);
        if (i >= 0) Sk.brk[size_t(i)] |= static_cast<u8>(1u << G.face_axis[size_t(e)]);
      }
      for (i32 e = G.jstart[size_t(k)]; e < G.jstart[size_t(k) + 1]; ++e) {
        const JSample& j = G.jref[size_t(e)];
        BodyShape& Sj = b.shapes[j.vg];
        const i32 i = Sj.index(j.v);
        if (i >= 0) Sj.break_junction(i, j.face, j.sub);
      }
      ++(impact ? o.impact_breaks : o.steady_breaks);
      if (modes[size_t(k)] == FailMode::Crush) o.crushed.push_back(k);
      ++o.modes[static_cast<int>(modes[size_t(k)])];
      o.cracks.push_back({b.to_world(B.p), rotate(b.q, B.n)});
    }
    if (poor || out.size() == before || round + 1 == rounds) {
      if (dbg && b.count > 5000) std::printf("    [round %d] stop: poor %d, none %d, last %d\n", round, poor ? 1 : 0, out.size() == before ? 1 : 0, round + 1 == rounds ? 1 : 0);
      break;
    }
    // Come apart? A part of some size (freed, or on supports of its own) goes its own way: the
    // rounds end and the parts separate. Chips crushed off where the piece struck still pass the
    // load on (crushed material in between transmits it): their loads move to the piece through
    // the crack, they leave the solve, and the rounds go on.
    i32 pin = -1;
    for (i32 i = 0; i < n; ++i)
      if (G.P.nodes[size_t(i)].fixed) pin = i;
    if (pin < 0) break;
    std::vector<u8> seed(size_t(n), 0);
    seed[size_t(pin)] = 1;
    const i32 nc = graph_components(n, G.P.bonds, seed, &comp);
    if (nc <= 1) continue;
    std::vector<f64> cmass(size_t(nc), 0.0);
    for (i32 i = 0; i < n; ++i)
      if (!G.P.nodes[size_t(i)].gone) cmass[size_t(comp[size_t(i)])] += G.node_mass[size_t(i)];
    bool apart = false;
    for (i32 c = 1; c < nc; ++c)
      if (cmass[size_t(c)] > cfg_.impact_chip_fraction * b.mass) apart = true;
    if (dbg && b.count > 5000) {
      std::printf("    [round %d] %d components:", round, nc);
      for (i32 c = 0; c < std::min(nc, 8); ++c) std::printf(" %.1f%%", 100.0 * cmass[size_t(c)] / b.mass);
      std::printf("%s, %zu broken so far\n", apart ? " -> apart" : "", out.size());
    }
    if (apart) break;
    // chips: each passes its net load on to the piece it broke from, shared by the nodes it was
    // bonded to (force equally, the moment about their centre as nodal moments), else to the
    // nearest node
    std::vector<std::vector<i32>> attach(static_cast<size_t>(nc));
    for (const SBond& B : G.P.bonds) {
      if (!B.broken || B.b < 0) continue;
      const i32 ca = comp[size_t(B.a)], cb = comp[size_t(B.b)];
      if (ca > 0 && cb == 0 && !G.P.nodes[size_t(B.b)].gone) attach[size_t(ca)].push_back(B.b);
      if (cb > 0 && ca == 0 && !G.P.nodes[size_t(B.a)].gone) attach[size_t(cb)].push_back(B.a);
    }
    std::vector<V3> cF(static_cast<size_t>(nc)), cM(static_cast<size_t>(nc));
    std::vector<i32> chip_nodes;
    for (i32 i = 0; i < n; ++i) {
      const i32 c = comp[size_t(i)];
      if (c == 0 || G.P.nodes[size_t(i)].gone) continue;
      chip_nodes.push_back(i);
      auto& A = attach[size_t(c)];
      if (A.empty()) {
        f64 best = 1e300;
        i32 bj = -1;
        for (i32 j = 0; j < n; ++j) {
          if (comp[size_t(j)] != 0 || G.P.nodes[size_t(j)].gone) continue;
          const f64 d = norm2(G.P.nodes[size_t(j)].c - G.P.nodes[size_t(i)].c);
          if (d < best) {
            best = d;
            bj = j;
          }
        }
        if (bj < 0) continue;
        A.push_back(bj);
      }
      f64* fi = &f[6 * size_t(i)];
      const V3 Fi{fi[0], fi[1], fi[2]};
      cF[size_t(c)] += Fi;
      cM[size_t(c)] += V3{fi[3], fi[4], fi[5]} + cross(G.P.nodes[size_t(i)].c, Fi);  // (about the origin)
      for (int q = 0; q < 6; ++q) fi[q] = 0.0;
    }
    for (i32 c = 1; c < nc; ++c) {
      auto& A = attach[size_t(c)];
      if (A.empty()) continue;
      std::sort(A.begin(), A.end());
      A.erase(std::unique(A.begin(), A.end()), A.end());
      const f64 w = 1.0 / static_cast<f64>(A.size());
      const V3 Fj = cF[size_t(c)] * w;
      V3 Mrest = cM[size_t(c)];
      for (i32 j : A) Mrest = Mrest - cross(G.P.nodes[size_t(j)].c, Fj);
      const V3 Mj = Mrest * w;
      for (i32 j : A) {
        f64* fj = &f[6 * size_t(j)];
        fj[0] += Fj.x;
        fj[1] += Fj.y;
        fj[2] += Fj.z;
        fj[3] += Mj.x;
        fj[4] += Mj.y;
        fj[5] += Mj.z;
      }
    }
    // (a chip holds together: its own bonds leave the matrix but not the topology the split reads)
    std::vector<i32> inner;
    for (i32 k = 0; k < static_cast<i32>(G.P.bonds.size()); ++k) {
      const SBond& B = G.P.bonds[size_t(k)];
      if (!B.broken && B.b >= 0 && comp[size_t(B.a)] > 0 && comp[size_t(B.a)] == comp[size_t(B.b)]) inner.push_back(k);
    }
    G.P.retire_nodes(chip_nodes);
    for (i32 k : inner) G.P.bonds[size_t(k)].broken = false;
    for (i32 i : chip_nodes)
      for (int q = 0; q < 6; ++q) G.u[6 * size_t(i) + size_t(q)] = 0.0;
  }
}

void World::Impl::piece_hinges(Body& b, const std::vector<HingeCut>& hinges) {
  // the parts it comes apart in (its bond graph as the breaks left it)
  if (!b.graph || b.shapes.empty()) return;
  const BodyGraph& G = *b.graph;
  const i32 n = static_cast<i32>(G.P.nodes.size());
  std::vector<i32> comp;
  graph_components(n, G.P.bonds, std::vector<u8>(size_t(n), 0), &comp);
  // one hinge per pair of parts, its sections' moments summed, at their moment-weighted pivot
  // (as a structure's: World::Impl::detach_unsupported)
  struct Pair {
    f64 mp = 0.0, pull = 0.0;
    V3 p, axis, n;
  };
  std::map<std::pair<i32, i32>, Pair> pairs;
  for (const HingeCut& h : hinges) {
    if (h.a < 0 || h.b < 0 || h.a >= n || h.b >= n) continue;
    const i32 ca = comp[size_t(h.a)], cb = comp[size_t(h.b)];
    if (ca == cb) continue;  // (still one part there: nothing turns)
    const bool flip = ca > cb;
    Pair& P = pairs[{std::min(ca, cb), std::max(ca, cb)}];
    const V3 ax = P.mp > 0.0 && dot(P.axis, h.axis) < 0.0 ? h.axis * -1.0 : h.axis;
    P.p += h.p * h.mp;
    P.axis += ax * h.mp;
    P.n += (flip ? h.n * -1.0 : h.n) * h.mp;
    P.mp += h.mp;
    P.pull += h.pull;
  }
  f64 hh = b.shapes.front().h;
  for (const BodyShape& S : b.shapes) hh = std::min(hh, S.h);
  for (auto& [key, P] : pairs) {
    if (!(P.mp > 0.0) || norm2(P.axis) < 1e-24) continue;
    const V3 at = b.to_world(P.p * (1.0 / P.mp));
    const V3 nn = rotate(b.q, norm2(P.n) > 1e-24 ? normalized(P.n) : V3{0, 0, 1});
    JointDesc d;
    d.type = JointType::Hinge;
    d.axis = rotate(b.q, normalized(P.axis));
    d.a.kind = d.b.kind = JointAnchor::Kind::Piece;
    d.a.id = d.b.id = static_cast<u64>(b.id);
    d.a.point = at - nn * (0.5 * hh);  // (a voxel of the lower part's side, and of the higher's)
    d.b.point = at + nn * (0.5 * hh);
    d.drive.kind = JointDrive::Kind::Speed;
    d.drive.speed = 0.0;
    d.drive.max = P.mp;
    d.break_force = P.pull;
    d.break_angle = std::max(0.0, cfg_.hinge_rotation);
    d.collide = false;
    const JointId id = add_joint_impl(d, 0);
    if (id == 0) continue;
    ++st_.plastic_hinges;
    // (both ends at the pivot, each held by its side's voxel: the parts turn about it)
    for (size_t k = 0; k < att_.joints.size(); ++k) {
      if (att_.joints[k].id != id) continue;
      for (JointRec::End* E : {&att_.joints[k].a, &att_.joints[k].b})
        if (E->shape >= 0 && E->shape < static_cast<i32>(b.shapes.size())) E->point = b.world_to_lattice(size_t(E->shape), at);
      fill_joint_end(att_.joints[k], false);
      fill_joint_end(att_.joints[k], true);
    }
  }
}

bool World::Impl::pulverize(Body& b, const std::vector<i32>& crushed) {
  if (crushed.empty() || !b.graph) return false;
  const BodyGraph& G = *b.graph;
  std::vector<u8> kill(b.frags.size(), 0);
  auto mark = [&](const BodyShape& S, const IVec3& p, i32 side) {
    const i32 i = S.index(p);
    if (i < 0) return;
    const i32 f = static_cast<i32>(S.frag[size_t(i)]) - 1;
    if (f >= 0 && f < static_cast<i32>(kill.size()) && G.frag_node[size_t(f)] == side) kill[size_t(f)] = 1;
  };
  for (i32 k : crushed) {
    const SBond& B = G.P.bonds[size_t(k)];
    if (B.b < 0) continue;
    const i32 side = G.node_mass[size_t(B.a)] <= G.node_mass[size_t(B.b)] ? B.a : B.b;
    const BodyShape& S = b.shapes[G.face_shape[size_t(k)]];
    for (i32 e = G.face_start[size_t(k)]; e < G.face_start[size_t(k) + 1]; ++e) {
      IVec3 p = G.face_p[size_t(e)];
      for (int s2 = 0; s2 < 2; ++s2) {
        if (s2) p[G.face_axis[size_t(e)]] += 1;
        mark(S, p, side);
      }
    }
    for (i32 e = G.jstart[size_t(k)]; e < G.jstart[size_t(k) + 1]; ++e) {
      const JSample& j = G.jref[size_t(e)];
      mark(b.shapes[j.vg], j.v, side);
      mark(b.shapes[j.og], j.o, side);
    }
  }
  i64 removed = 0;
  std::vector<V3> at(b.frags.size());
  std::vector<i32> cnt(b.frags.size(), 0);
  for (BodyShape& S : b.shapes)
    for (i32 i = 0; i < static_cast<i32>(S.vox.size()); ++i) {
      if (!vox_solid(S.vox[size_t(i)])) continue;
      const i32 f = static_cast<i32>(S.frag[size_t(i)]) - 1;
      if (f < 0 || !kill[size_t(f)]) continue;
      // (ductile material yields where brittle material crushes: the bars of a crushed concrete
      // fragment stay, the concrete around them falls away as dust)
      if (mats()[vox_mat(S.vox[size_t(i)])].ductile) continue;
      const IVec3 p = S.voxel(i);
      at[size_t(f)] += S.xf.to(V3{S.h * p[0], S.h * p[1], S.h * p[2]});
      ++cnt[size_t(f)];
      S.vox[size_t(i)] = kAir;
      S.frag[size_t(i)] = 0;
      S.brk[size_t(i)] = 0;
      for (auto& l : S.layer)
        if (!l.empty()) l[size_t(i)] = 0;
      ++removed;
    }
  if (!removed) return false;
  for (size_t f = 0; f < kill.size(); ++f) {
    if (!cnt[f]) continue;
    const V3 c = at[f] * (1.0 / cnt[f]);
    const V3 X = b.to_world(c);
    dust_event(X, b.v + cross(b.w, X - b.x), cnt[f], true);
  }
  st_.pulverized_voxels += removed;
  refragment_body(b);
  b.graph_dirty = true;
  return true;
}

std::unique_ptr<Body> World::Impl::sub_body(const Body& parent, const std::vector<SVox>& voxels, bool use_pre) {
  auto c = std::make_unique<Body>();
  const size_t np = parent.shapes.size();
  // its shapes: the parent's that hold some of its voxels, in the parent's order
  std::vector<IVec3> lo(np, IVec3{INT_MAX, INT_MAX, INT_MAX}), hi(np, IVec3{INT_MIN, INT_MIN, INT_MIN});
  std::vector<i32> smap(np, -1);
  for (const SVox& v : voxels) {
    const IVec3 p = parent.shapes[v.shape].voxel(v.cell);
    smap[v.shape] = 0;
    for (int a = 0; a < 3; ++a) {
      lo[v.shape][a] = std::min(lo[v.shape][a], p[a]);
      hi[v.shape][a] = std::max(hi[v.shape][a], p[a]);
    }
  }
  i32 nshapes = 0;
  for (size_t k = 0; k < np; ++k)
    if (smap[k] >= 0) smap[k] = nshapes++;
  // fragments of the child, in parent order
  std::vector<i32> remap(parent.frags.size(), -1);
  for (const SVox& v : voxels) {
    const i32 fp = static_cast<i32>(parent.shapes[v.shape].frag[size_t(v.cell)]) - 1;
    if (fp >= 0 && remap[size_t(fp)] < 0) remap[size_t(fp)] = 0;
  }
  for (size_t k = 0; k < parent.frags.size(); ++k)
    if (remap[k] == 0) {
      remap[k] = static_cast<i32>(c->frags.size());
      c->frags.push_back(parent.frags[k]);
      c->frags.back().shape = static_cast<u16>(smap[parent.frags[k].shape]);
    }
  c->shapes.resize(size_t(nshapes));
  std::vector<std::vector<i32>> cellmap(np);  // (parent shape cell -> child cell, for its broken junctions)
  for (size_t k = 0; k < np; ++k) {
    if (smap[k] < 0) continue;
    const BodyShape& P = parent.shapes[k];
    BodyShape& S = c->shapes[size_t(smap[k])];
    S.xf = P.xf;
    S.grid = P.grid;
    S.h = P.h;
    S.priority = P.priority;
    S.lo = lo[k];
    S.dim = {hi[k][0] - lo[k][0] + 1, hi[k][1] - lo[k][1] + 1, hi[k][2] - lo[k][2] + 1};
    const size_t cells = size_t(S.dim[0]) * size_t(S.dim[1]) * size_t(S.dim[2]);
    S.vox.assign(cells, kAir);
    S.frag.assign(cells, 0);
    S.brk.assign(cells, 0);
    if (!P.jbrk.empty()) cellmap[k].assign(P.vox.size(), -1);
  }
  for (const SVox& v : voxels) {
    const BodyShape& P = parent.shapes[v.shape];
    BodyShape& S = c->shapes[size_t(smap[v.shape])];
    const i32 i = v.cell;
    const IVec3 p = P.voxel(i);
    const i32 j = S.index(p);
    S.vox[size_t(j)] = P.vox[size_t(i)];
    const i32 fp = static_cast<i32>(P.frag[size_t(i)]) - 1;
    S.frag[size_t(j)] = fp >= 0 ? static_cast<u32>(remap[size_t(fp)] + 1) : 0;
    S.brk[size_t(j)] = P.brk[size_t(i)];
    const size_t cells = S.vox.size();
    for (int L = 0; L < kMaxLayers; ++L) {
      const u8 lv = P.layer_at(L, i);
      if (!lv) continue;
      if (S.layer[size_t(L)].empty()) S.layer[size_t(L)].assign(cells, 0);
      S.layer[size_t(L)][size_t(j)] = lv;
    }
    if (!cellmap[v.shape].empty()) cellmap[v.shape][size_t(i)] = j;
    ++S.count;
  }
  for (size_t k = 0; k < np; ++k) {
    if (smap[k] < 0 || cellmap[k].empty()) continue;
    BodyShape& S = c->shapes[size_t(smap[k])];
    for (u64 e : parent.shapes[k].jbrk) {
      const i64 pc = static_cast<i64>(e >> 16);
      if (pc < 0 || pc >= static_cast<i64>(cellmap[k].size()) || cellmap[k][size_t(pc)] < 0) continue;
      S.jbrk.push_back(shape_junction_code(cellmap[k][size_t(pc)], static_cast<int>((e >> 8) & 0xFF), static_cast<int>(e & 0xFF)));
    }
    std::sort(S.jbrk.begin(), S.jbrk.end());
  }
  // (a part of one shape, placed in the parent's frame: its frame becomes its lattice's)
  LatticeXf rebase;
  if (nshapes == 1 && !c->shapes[0].xf.identity) {
    rebase = c->shapes[0].xf;
    c->shapes[0].xf = LatticeXf{};
    for (BodyFrag& bf : c->frags) {
      bf.com = rebase.from(bf.com);
      bf.inertia = rebase.Rt * bf.inertia * rebase.R;
    }
  }
  body_refresh(*c, grid_.h, cfg_.rigid.max_points);
  c->id = next_id_++;
  if (rebase.identity) {
    c->q = parent.q;
    c->x = parent.to_world(c->com);
  } else {
    c->q = qnormalized(parent.q * rebase.q);
    c->x = parent.to_world(rebase.to(c->com));
  }
  c->parent = parent.announced ? parent.id : parent.parent;
  c->keep = parent.keep;
  c->max_speed = parent.max_speed;
  const V3 vp = use_pre ? parent.v_pre : parent.v;
  const V3 wp = use_pre ? parent.w_pre : parent.w;
  c->v = vp + cross(wp, c->x - parent.x);
  c->w = wp;
  c->v_pre = c->v;
  c->w_pre = c->w;
  c->asleep = false;
  c->graph_dirty = true;
  c->stress_cooldown = 0;
  c->refresh_box();
  return c;
}

bool World::Impl::split_body(Body& b, bool use_pre, bool force_replace, const std::vector<Carried>* carried, f64 spent, bool in_place) {
  if (b.graph_dirty || !b.graph) rebuild_body_graph(b);
  BodyGraph& G = *b.graph;
  const i32 n = static_cast<i32>(G.P.nodes.size());
  std::vector<i32> comp;
  std::vector<u8> seed(size_t(n), 0);
  if (n > 0) seed[0] = 1;
  const i32 nc = n > 0 ? graph_components(n, G.P.bonds, seed, &comp) : 0;
  if (nc <= 1 && !force_replace) return false;  // (its matrix lost the broken bonds in place)
  std::vector<i32> frag_comp(b.frags.size(), -1);
  for (size_t f = 0; f < b.frags.size(); ++f)
    if (G.frag_node[f] >= 0) frag_comp[f] = comp[size_t(G.frag_node[f])];
  std::vector<std::vector<SVox>> parts(size_t(std::max(1, nc)));
  for (size_t k = 0; k < b.shapes.size(); ++k) {
    const BodyShape& S = b.shapes[k];
    for (i32 i = 0; i < static_cast<i32>(S.vox.size()); ++i) {
      if (!vox_solid(S.vox[size_t(i)])) continue;
      const i32 f = static_cast<i32>(S.frag[size_t(i)]) - 1;
      const i32 c = f >= 0 ? frag_comp[size_t(f)] : -1;
      if (c >= 0) parts[size_t(c)].push_back(SVox{static_cast<u16>(k), i});
    }
  }
  // A part of some size came apart in this contact step: the step is solved again without the
  // piece (its parts keep the velocity they had before it). Chips only: they take the velocity the
  // step left the piece with, and the step stands - but for what the chips carried: chips that
  // broke off with most of the step's contact impulses (a stub the piece landed on) take them
  // along, as far as breaking them did not cost what those contacts took out of the motion (a
  // thin stub stops nothing; storeys of columns crushing brake a building as they go).
  // (One part only - the piece reshaped, e.g. crushed chips turned to dust - is no separation:
  // it keeps the step's velocity. Given the pre-solve one, it would drive on into what it hit
  // for another substep and grind itself down.)
  bool separated = false;
  if (use_pre && nc > 1) {
    std::vector<size_t> sizes;
    for (const auto& part : parts) sizes.push_back(part.size());
    size_t largest = 0;
    for (size_t c = 1; c < sizes.size(); ++c)
      if (sizes[c] > sizes[largest]) largest = c;
    std::vector<size_t> sorted = sizes;
    std::sort(sorted.rbegin(), sorted.rend());
    separated = sorted.size() > 1 && static_cast<i32>(sorted[1]) >= cfg_.rollback_part_voxels;
    if (separated) rollback_ = true;
    if (!separated && carried) {
      f64 total = 0.0, lost = 0.0, e_lost = 0.0;
      V3 J, L;
      for (const Carried& k : *carried) {
        const f64 j = norm(k.J);
        total += j;
        if (k.frag >= 0 && k.frag < static_cast<i32>(frag_comp.size()) && frag_comp[size_t(k.frag)] == static_cast<i32>(largest)) continue;
        lost += j;
        e_lost += k.e;
        J += k.J;
        L += cross(k.p - b.x, k.J);
      }
      const f64 release = e_lost > 0.0 ? std::clamp(1.0 - spent / e_lost, 0.0, 1.0) : 1.0;
      if (total > 0.0 && lost > 0.5 * total && release > 0.0) {
        b.v -= J * (release * b.inv_mass);
        b.w -= b.inv_inertia_world() * (L * release);
        ++st_.chip_releases;
      }
    }
  }
  use_pre = use_pre && separated;
  // (a piece that keeps its identity: its largest part, the one most of its wheels are on, stays)
  i32 stay = -1;
  if (in_place || keeps_identity(b)) {
    std::vector<i32> wheels_on(parts.size(), 0);
    for (const WheelRec& r : att_.wheels) {
      if (r.mount.piece != b.id || r.mount.shape < 0 || size_t(r.mount.shape) >= b.shapes.size()) continue;
      const BodyShape& S = b.shapes[size_t(r.mount.shape)];
      const i32 cell = S.index(r.mount.voxel);
      if (cell < 0 || !vox_solid(S.vox[size_t(cell)])) continue;
      const i32 f = static_cast<i32>(S.frag[size_t(cell)]) - 1;
      const i32 c = f >= 0 ? frag_comp[size_t(f)] : -1;
      if (c >= 0) ++wheels_on[size_t(c)];
    }
    for (i32 c = 0; c < static_cast<i32>(parts.size()); ++c) {
      if (static_cast<i32>(parts[size_t(c)].size()) < cfg_.min_body_voxels) continue;
      if (stay < 0 || wheels_on[size_t(c)] > wheels_on[size_t(stay)] ||
          (wheels_on[size_t(c)] == wheels_on[size_t(stay)] && parts[size_t(c)].size() > parts[size_t(stay)].size()))
        stay = c;
    }
  }
  // (voxels lost on the way - removed, crushed, carved (force_replace), or shards turned to dust:
  // what rested on them would hover over the gap)
  bool lost = force_replace;
  for (i32 pc = 0; pc < static_cast<i32>(parts.size()); ++pc) {
    auto& part = parts[size_t(pc)];
    if (part.empty() || pc == stay) continue;
    // (too small to be a piece: a part split off, or what is left of a piece cut down - a single
    // part, made again - turns to dust, as it would breaking off)
    if ((nc > 1 || force_replace) && static_cast<i32>(part.size()) < cfg_.min_body_voxels) {
      lost = true;
      // (a shard: dust and a few chips, not a rigid piece)
      V3 c;
      for (const SVox& v : part) {
        const BodyShape& S = b.shapes[v.shape];
        const IVec3 p = S.voxel(v.cell);
        c += S.xf.to(V3{S.h * p[0], S.h * p[1], S.h * p[2]});
      }
      c *= 1.0 / static_cast<f64>(part.size());
      const V3 X = b.to_world(c);
      const V3 vp = use_pre ? b.v_pre : b.v, wp = use_pre ? b.w_pre : b.w;
      dust_event(X, vp + cross(wp, X - b.x), static_cast<i32>(part.size()), false);
      st_.pulverized_voxels += static_cast<i64>(part.size());
      continue;
    }
    pw_.pending_add.push_back(sub_body(b, part, use_pre));
    pw_.pending_add.back()->origin = b.id;
    if (separated) {
      // (broken in this substep's collision: the parts part for the rest of it)
      pw_.pending_add.back()->family = b.id;
      pw_.pending_add.back()->family_ticks = 1;
    }
  }
  if (lost) wake_around(b);
  if (nc > 1) ++st_.body_splits;
  if (stay < 0) {
    pw_.pending_retire.push_back(b.id);
    return true;
  }
  // the part that stays: the others' voxels (and any in no part) leave the piece, edited in place
  std::vector<u8> mine;
  for (size_t k = 0; k < b.shapes.size(); ++k) {
    BodyShape& S = b.shapes[k];
    mine.assign(S.vox.size(), 0);
    for (const SVox& v : parts[size_t(stay)])
      if (v.shape == k) mine[size_t(v.cell)] = 1;
    for (size_t i = 0; i < S.vox.size(); ++i) {
      if (mine[i] || !vox_solid(S.vox[i])) continue;
      S.vox[i] = kAir;
      S.frag[i] = 0;
      S.brk[i] = 0;
      for (auto& l : S.layer)
        if (!l.empty()) l[i] = 0;
    }
  }
  const V3 com0 = b.com, x0 = b.x;
  if (use_pre) {
    b.v = b.v_pre;
    b.w = b.w_pre;
  }
  if (separated) {
    b.family = b.id;
    b.family_ticks = 1;
  }
  refragment_body(b);
  refresh_in_place(b, com0, x0);
  pw_.split_kept.push_back(b.id);
  return true;
}

bool World::Impl::keeps_identity(const Body& b) const {
  if (b.keep) return true;
  for (const WheelRec& r : att_.wheels)
    if (r.mount.piece == b.id) return true;
  if (cfg_.jointed_keep_identity)
    for (const JointRec& r : att_.joints)
      if (r.a.piece == b.id || r.b.piece == b.id) return true;
  return false;
}

void World::Impl::refresh_in_place(Body& b, const V3& com0, const V3& x0) {
  rigid_.wake(b);
  body_refresh(b, grid_.h, cfg_.rigid.max_points);
  // (the same place in the world: its lattice does not move, its centre of mass does)
  b.x = x0 + rotate(b.q, b.com - com0);
  b.v += cross(b.w, b.x - x0);
  b.v_pre = b.v;
  b.w_pre = b.w;
  b.graph_dirty = true;
  b.stress_cooldown = 0;
  b.refresh_box();
  pw_.reshaped.push_back(b.id);
  ++st_.reshapes;
}

void World::Impl::wake_around(const Body& b) {
  const f64 m = 2.0 * grid_.h;
  rigid_.wake_box(b.box_lo - V3{m, m, m}, b.box_hi + V3{m, m, m});
}

void World::Impl::flush_body_changes() {
  std::sort(pw_.pending_retire.begin(), pw_.pending_retire.end());
  std::sort(pw_.split_kept.begin(), pw_.split_kept.end());
  joints_follow_splits();  // (the joints on the pieces split: onto the parts their voxels are in)
  wheels_follow_splits();  // (and the wheels)
  const bool kept = !pw_.split_kept.empty();
  pw_.split_kept.clear();
  if (!pw_.pending_retire.empty()) {
    remove_bodies(std::move(pw_.pending_retire), PieceEnd::Split);
    pw_.pending_retire.clear();
  }
  const bool joined = !att_.joints.empty() && !pw_.pending_add.empty();
  const bool wheeled = !att_.wheels.empty() && !pw_.pending_add.empty();
  for (auto& c : pw_.pending_add) rigid_.add(std::move(c));
  pw_.pending_add.clear();
  if (joined || (kept && !att_.joints.empty())) update_joint_ends();
  if (wheeled || (kept && !att_.wheels.empty())) update_wheel_mounts();
}

namespace {

// A rigid solver's contact forces are one of many statically admissible answers: a piece resting
// on many points may carry its whole weight on a few of them, which a stress check reads as
// crushing point loads. The stress check takes the elastic answer instead: the same net force and
// moment shared over the contact points as by a rigid body on equal springs (the least-squares
// distribution), f_c = u + theta x r_c.
// (the rigid-body share of one partner's contacts: the same net force and moment over its points)
template <class PF>
void spread_group(std::vector<PF>& fs, const std::vector<size_t>& idx) {
  const size_t n = idx.size();
  if (n < 2) return;
  V3 F, c0;
  for (size_t k : idx) {
    F += fs[k].F;
    c0 += fs[k].p;
  }
  c0 *= 1.0 / static_cast<f64>(n);
  V3 M;
  M3 A;
  f64 r2sum = 0.0;
  for (size_t k : idx) {
    const V3 r = fs[k].p - c0;
    M += cross(r, fs[k].F);
    const f64 r2 = dot(r, r);
    r2sum += r2;
    const f64 rv[3] = {r.x, r.y, r.z};
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) A.m[size_t(3 * i + j)] += (i == j ? r2 : 0.0) - rv[i] * rv[j];
  }
  // (collinear or coincident points cannot carry a moment about their line: a small
  // regularization leaves that part to the inertia)
  const f64 eps = 1e-6 * r2sum + 1e-12;
  for (int i = 0; i < 3; ++i) A.m[size_t(4 * i)] += eps;
  M3 Ai;
  V3 theta;
  if (inverse(A, Ai)) theta = Ai * M;
  const V3 u = F * (1.0 / static_cast<f64>(n));
  for (size_t k : idx) fs[k].F = u + cross(theta, fs[k].p - c0);
}

// Each partner's contacts are spread on their own: what presses on a piece from above (a block on
// a beam) and what holds it from below (its supports) keep their places - pooled, they would
// cancel, and a beam loaded between its supports would feel no bending.
template <class PF>
void spread_contact_forces(std::vector<PF>& fs, bool per_partner) {
  if (fs.size() < 2) return;
  if (!per_partner) {
    // (pooled: all of them, the reference's way - WorldConfig::spread_per_partner)
    std::vector<size_t> all(fs.size());
    for (size_t k = 0; k < all.size(); ++k) all[k] = k;
    spread_group(fs, all);
    return;
  }
  std::vector<size_t> order;
  for (size_t k = 0; k < fs.size(); ++k)
    if (fs[k].with != 0) order.push_back(k);
  std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return fs[x].with < fs[y].with; });
  std::vector<size_t> idx;
  for (size_t s = 0; s < order.size();) {
    size_t e = s;
    idx.clear();
    while (e < order.size() && fs[order[e]].with == fs[order[s]].with) idx.push_back(order[e++]);
    spread_group(fs, idx);
    s = e;
  }
}

}  // namespace

int World::Impl::fracture_hook(f64 dt) {
  rollback_ = false;
  const auto& cs = rigid_.contacts();
  const size_t nb = rigid_.bodies.size();
  std::vector<std::vector<PointForce>> per(nb);
  std::vector<std::vector<Carried>> carried(nb);  // (what the contacts did to each piece this step)
  std::vector<f64> fsum(nb, 0.0), approach(nb, 0.0), dissipated(nb, 0.0);
  // A rigid contact stops a body within one substep; a real impact takes the time a stress wave
  // (slowed by the crushing zone) needs to cross the piece. Contact impulses load a piece's
  // stress spread over that time: large pieces crush progressively from where they hit instead
  // of feeling a uniform deceleration of tens of g.
  // (a resting contact, closing no faster than gravity makes it within a substep or two, carries
  // a steady force, J / dt: a piece's weight, and what rests on it, in full)
  std::vector<f64> kf(nb);
  for (size_t i = 0; i < nb; ++i) {
    const Body& b = *rigid_.bodies[i];
    const f64 tau = std::max(dt, 2.0 * b.radius / std::max(1.0, cfg_.impact_wave_speed));
    kf[i] = par_.impact / tau;
  }
  const f64 v_rest = 2.0 * cfg_.rigid.gravity * dt + 0.05;
  for (const Contact& c : cs) {
    const V3 J = c.impulse();
    const bool resting = c.approach <= v_rest;
    approach[size_t(c.a)] = std::max(approach[size_t(c.a)], c.approach);
    if (c.b >= 0) approach[size_t(c.b)] = std::max(approach[size_t(c.b)], c.approach);
    // the kinetic energy the contact takes out of the collision (inelastic: 1/2 J v)
    const f64 e = 0.5 * c.ln * c.approach;
    if (c.b >= 0) {
      dissipated[size_t(c.a)] += 0.5 * e;
      dissipated[size_t(c.b)] += 0.5 * e;
    } else {
      dissipated[size_t(c.a)] += e;
    }
    if (norm2(J) <= 0.0) continue;
    Body& A = *rigid_.bodies[size_t(c.a)];
    // (a link's side has no voxels to load: what it presses on still feels it)
    if (!A.link) {
      const V3 Fa = J * (resting ? 1.0 / dt : kf[size_t(c.a)]);
      const i32 fa = static_cast<i32>(A.shapes[size_t(c.shape_a)].frag[size_t(c.vox_a)]) - 1;
      per[size_t(c.a)].push_back({fa, Fa, c.p, c.b >= 0 ? rigid_.bodies[size_t(c.b)]->id : -1 - static_cast<i64>(c.grid)});
      carried[size_t(c.a)].push_back({fa, J, c.p, c.b >= 0 ? 0.5 * e : e});
      fsum[size_t(c.a)] += norm(Fa);
    }
    if (c.b >= 0 && !rigid_.bodies[size_t(c.b)]->link) {
      Body& B = *rigid_.bodies[size_t(c.b)];
      const V3 Fb = J * -(resting ? 1.0 / dt : kf[size_t(c.b)]);
      const i32 fb = static_cast<i32>(B.shapes[size_t(c.shape_b)].frag[size_t(c.vox_b)]) - 1;
      per[size_t(c.b)].push_back({fb, Fb, c.p, A.id});
      carried[size_t(c.b)].push_back({fb, J * -1.0, c.p, 0.5 * e});
      fsum[size_t(c.b)] += norm(Fb);
    }
  }
  // (what hangs on joints, and what they pull: a steady load; a carrier on its wheels, and what
  // they stand on)
  if (!att_.joints.empty()) joint_piece_forces(per, fsum);
  if (!att_.wheels.empty()) wheel_piece_forces(per, fsum);
  using FClock = std::chrono::steady_clock;
  const auto f0 = FClock::now();
  // which pieces are checked (in body order)
  struct Check {
    size_t i;
    bool impact;
    f64 budget;
    bool split = false;  // (in several parts already: they go their own ways)
    StressOut out;
    f64 rebuild_ms = 0.0, stress_ms = 0.0;
    i32 voxels = 0, nodes = 0;
  };
  std::vector<Check> checks;
  for (size_t i = 0; i < nb; ++i) {
    Body& b = *rigid_.bodies[i];
    if (b.asleep || static_cast<i32>(b.frags.size()) < cfg_.min_fracture_frags) continue;
    if (b.stress_cooldown > 0) --b.stress_cooldown;
    if (b.crumpling > 0) --b.crumpling;
    const f64 weight = b.mass * cfg_.rigid.gravity;
    // A collision: a contact closing faster than jostling in a pile does (small pieces need a
    // harder knock). Its cracks are paid from the approach's kinetic energy, so rubble cannot
    // grind itself down and a hard landing shatters what it overloads.
    const f64 v_min = std::max(cfg_.body_impact_speed, cfg_.small_impact_speed * (1.0 - b.mass / cfg_.small_piece_mass));
    // (a piece crumpling as it goes - a vehicle along a wall - spends the collision in its folds: its
    // checks come further apart while it does)
    const i32 gap = b.crumpling > 0 ? std::max(2, cfg_.crumple_check_gap) : 2;
    const bool impact = approach[i] > v_min && fsum[i] > cfg_.body_trigger * weight && b.stress_cooldown <= cfg_.body_check_ticks - gap;
    // resting on new supports (a first landing, rubble shifting under it, a load put on it):
    // checked once, again when the supporting forces changed by much of its weight
    // (or its strengths changed: damage, a fire eating into it)
    const bool steady = fsum[i] > 0.5 * weight && b.stress_cooldown <= 0 &&
                        (std::abs(fsum[i] - b.last_load) > 0.5 * weight || b.recheck);
    const f64 wr = norm(b.w);
    const bool spin = wr * wr * b.radius > 2.0 * cfg_.rigid.gravity && b.stress_cooldown <= 0;
    if (!impact && !steady && !spin) continue;
    b.recheck = false;
    if (impact && dissipated[i] > cfg_.impact_event_energy && impact_budget_ > 0) {
      // (a heavy landing: dust and camera shake at its contacts)
      V3 at;
      f64 wsum = 0.0;
      for (const PointForce& pf : per[i]) {
        const f64 w = norm(pf.F);
        at += pf.p * w;
        wsum += w;
      }
      if (wsum > 0.0) {
        --impact_budget_;
        WorldEvent ev;
        ev.kind = WorldEvent::Kind::Impact;
        ev.pos = at * (1.0 / wsum);
        ev.radius = b.radius;
        ev.strength = dissipated[i];
        events_.push_back(std::move(ev));
      }
    }
    if (cfg_.spread_contacts) spread_contact_forces(per[i], cfg_.spread_per_partner);
    Check c;
    c.i = i;
    c.impact = impact;
    c.budget = impact ? dissipated[i] : -1.0;
    checks.push_back(std::move(c));
  }
  // the checks, concurrently (each touches only its piece; a single check keeps the threads for
  // its own solve)
  auto run = [&](Check& c) {
    Body& b = *rigid_.bodies[c.i];
    const auto r0 = FClock::now();
    if (b.graph_dirty || !b.graph) rebuild_body_graph(b);
    const auto r1 = FClock::now();
    c.rebuild_ms = std::chrono::duration<f64, std::milli>(r1 - r0).count();
    c.voxels = b.count;
    c.nodes = static_cast<i32>(b.graph->P.nodes.size());
    if (b.graph->components > 1) {
      c.split = true;
      return;
    }
    body_stress_run(b, per[c.i], true, c.budget, c.out);
    c.stress_ms = std::chrono::duration<f64, std::milli>(FClock::now() - r1).count();
  };
  const auto f1 = FClock::now();
  // (large pieces one after the other, each with all the threads for its own solve; the small ones
  // concurrently, each on one thread)
  std::vector<size_t> small;
  for (size_t k = 0; k < checks.size(); ++k) {
    const Body& b = *rigid_.bodies[checks[k].i];
    if (checks.size() == 1 || b.count > cfg_.big_piece_voxels) run(checks[k]);
    else small.push_back(k);
  }
  parallel_for(static_cast<i64>(small.size()), 1, [&](i64 k0, i64 k1) {
    SerialScope serial;
    for (i64 k = k0; k < k1; ++k) run(checks[small[size_t(k)]]);
  });
  const auto f2 = FClock::now();
  // their outcomes, in body order
  bool changed = false;
  for (Check& c : checks) {
    Body& b = *rigid_.bodies[c.i];
    if (c.split) {
      if (split_body(b, true, false, &carried[c.i])) changed = true;
      continue;
    }
    b.stress_cooldown = c.impact ? cfg_.body_check_ticks : 3 * cfg_.body_check_ticks;
    b.last_load = fsum[c.i];
    apply_stress_out(c.out);
    if (c.out.broken.empty()) continue;
    const bool reshaped = cfg_.pulverize && pulverize(b, c.out.crushed);
    if (reshaped && b.count == 0) {
      wake_around(b);
      pw_.pending_retire.push_back(b.id);
      changed = true;
      continue;
    }
    // (its plastic hinges, on its voxels either side of their sections: they go with the parts)
    if (!reshaped && !c.out.hinges.empty()) piece_hinges(b, c.out.hinges);
    // (reshaped: its fragments are new, what carried the contacts is dust or unknown)
    if (split_body(b, true, reshaped, reshaped ? nullptr : &carried[c.i], c.out.spent)) changed = true;
  }
  const auto f3 = FClock::now();
  flush_body_changes();
  const int result = rollback_ ? 2 : (changed ? 1 : 0);
  static const bool fprof = diag("SVX_PROFILE_FRACTURE");
  if (fprof) {
    auto ms = [](FClock::time_point a, FClock::time_point b) { return std::chrono::duration<f64, std::milli>(b - a).count(); };
    static f64 acc[4] = {0, 0, 0, 0}, reb = 0.0, str = 0.0, reb_big = 0.0, str_big = 0.0;
    static i64 calls = 0, nchecks = 0, big = 0;
    for (const Check& c : checks) {
      const bool bg = rigid_.bodies.size() > c.i && rigid_.bodies[c.i]->count > cfg_.big_piece_voxels;
      (bg ? reb_big : reb) += c.rebuild_ms;
      (bg ? str_big : str) += c.stress_ms;
      if (c.rebuild_ms + c.stress_ms > 5.0)
        std::printf("    [slow check t%lld] %d voxels, %d nodes: rebuild %.1f ms, stress %.1f ms (assemble %.1f, %lld solves %.1f ms, %lld its, %zu broken), split %d\n",
                    static_cast<long long>(st_.ticks), c.voxels, c.nodes, c.rebuild_ms, c.stress_ms, c.out.assemble_ms,
                    static_cast<long long>(c.out.checks), c.out.solve_ms, static_cast<long long>(c.out.pcg_iters), c.out.broken.size(), c.split ? 1 : 0);
    }
    acc[0] += ms(f0, f1);
    acc[1] += ms(f1, f2);
    acc[2] += ms(f2, f3);
    acc[3] += ms(f3, FClock::now());
    nchecks += static_cast<i64>(checks.size());
    for (const Check& c : checks) big += rigid_.bodies.size() > c.i && rigid_.bodies[c.i]->count > cfg_.big_piece_voxels ? 1 : 0;
    if (++calls % 120 == 0) {
      std::printf("  [fracture] per substep: triggers %.2f checks %.2f splits %.2f flush %.2f ms (%.1f checks, %.2f big) | cpu: rebuild %.2f stress %.2f, big: rebuild %.2f stress %.2f\n",
                  acc[0] / 120, acc[1] / 120, acc[2] / 120, acc[3] / 120, nchecks / 120.0, big / 120.0, reb / 120, str / 120, reb_big / 120,
                  str_big / 120);
      acc[0] = acc[1] = acc[2] = acc[3] = 0;
      reb = str = reb_big = str_big = 0.0;
      nchecks = big = 0;
    }
  }
  return result;
}

void World::Impl::carve_bodies(const V3& c, f64 r, f64 energy) {
  std::vector<i64> hit;
  for (const auto& bp : rigid_.bodies) {
    const Body& b = *bp;
    if (c.x + r < b.box_lo.x || c.x - r > b.box_hi.x || c.y + r < b.box_lo.y || c.y - r > b.box_hi.y ||
        c.z + r < b.box_lo.z || c.z - r > b.box_hi.z)
      continue;
    hit.push_back(b.id);
  }
  for (i64 id : hit) {
    Body* bp = rigid_.find(id);
    if (!bp) continue;
    Body& b = *bp;
    const V3 sb = b.to_shape(c);
    i32 removed = 0;
    for (BodyShape& S : b.shapes) {
      const f64 h = S.h;
      const i32 R = static_cast<i32>(std::ceil(r / h)) + 1;
      const V3 s = S.xf.from(sb);  // (the centre in the shape's lattice)
      const IVec3 sv = voxel_of(s, h);
      for (i32 x = sv[0] - R; x <= sv[0] + R; ++x)
        for (i32 y = sv[1] - R; y <= sv[1] + R; ++y)
          for (i32 z = sv[2] - R; z <= sv[2] + R; ++z) {
            const IVec3 p{x, y, z};
            const i32 i = S.index(p);
            if (i < 0 || !vox_solid(S.vox[size_t(i)])) continue;
            const f64 d = norm(V3{h * x, h * y, h * z} - s);
            if (d > r) continue;
            if (!penetrates(mats()[vox_mat(S.vox[size_t(i)])], energy, r, d)) continue;  // (as in the world)
            S.vox[size_t(i)] = kAir;
            S.frag[size_t(i)] = 0;
            for (auto& l : S.layer)
              if (!l.empty()) l[size_t(i)] = 0;
            ++removed;
          }
    }
    if (!removed) continue;
    refragment_body(b);
    rigid_.wake(b);
    if (b.count == 0) {
      wake_around(b);
      pw_.pending_retire.push_back(b.id);
      continue;
    }
    split_body(b, false, true);  // (re-announced: its mesh changed)
  }
  flush_body_changes();
}

void World::Impl::blast_bodies(const PendingEvent& e) {
  const f64 rl = cfg_.blast_reach * e.radius;
  const f64 vmax = std::min(cfg_.blast_max_speed, std::sqrt(2.0 * cfg_.blast_kinetic * e.energy / 400.0));
  auto speed = [&](f64 d) { return vmax * std::min(1.0, (e.radius * e.radius) / std::max(1e-6, d * d)); };
  std::vector<i64> near;
  for (const auto& bp : rigid_.bodies)
    if (norm(bp->x - e.pos) - bp->radius < rl) near.push_back(bp->id);
  for (i64 id : near) {
    Body* bp = rigid_.find(id);
    if (!bp) continue;
    Body& b = *bp;
    std::vector<PointForce> forces;
    V3 J, L;
    for (size_t k = 0; k < b.frags.size(); ++k) {
      const BodyFrag& bf = b.frags[k];
      if (bf.count <= 0) continue;
      const V3 cw = b.to_world(bf.com);
      const f64 d = norm(cw - e.pos);
      if (d > rl) continue;
      const V3 Jk = (cw - e.pos) * (bf.mass * speed(d) / std::max(1e-3, d));
      J += Jk;
      L += cross(cw - b.x, Jk);
      forces.push_back({static_cast<i32>(k), Jk * (par_.impact / cfg_.dt), cw});
    }
    if (forces.empty()) continue;
    rigid_.wake(b);
    b.v += J * b.inv_mass;
    b.w += b.inv_inertia_world() * L;
    if (b.frags.size() >= 2) {
      const std::vector<i32> broken = body_stress(b, forces, true);
      if (!broken.empty()) split_body(b, false, false);
    }
  }
  flush_body_changes();
}

i64 World::Impl::body_bytes(const Body& b, Bytes kind) {
  // (the records themselves - of pointer-sized containers - as fixed sizes in what is used: the
  // same on every platform)
  const i64 shapes = kind == Bytes::Held ? static_cast<i64>(b.shapes.capacity() * sizeof(BodyShape)) : static_cast<i64>(b.shapes.size()) * 552;
  i64 n = record_bytes<Body>(kind, 832) + shapes + vec_bytes(b.frags, kind) + vec_bytes(b.pts, kind) + vec_bytes(b.pt_vox, kind) +
          vec_bytes(b.pt_shape, kind) + vec_bytes(b.pt_area, kind) + vec_bytes(b.wpts, kind);
  if (b.link) n += record_bytes<LinkData>(kind, 176) + vec_bytes(b.link->spheres, kind);
  for (const BodyShape& S : b.shapes) {
    n += vec_bytes(S.vox, kind) + vec_bytes(S.frag, kind) + vec_bytes(S.brk, kind) + vec_bytes(S.jbrk, kind);
    for (const auto& l : S.layer) n += vec_bytes(l, kind);
  }
  if (b.graph) {
    const BodyGraph& G = *b.graph;
    n += record_bytes<BodyGraph>(kind, 944) + G.P.memory_bytes(kind) + vec_bytes(G.frag_node, kind) + vec_bytes(G.node_com, kind) +
         vec_bytes(G.node_mass, kind) + vec_bytes(G.node_inertia, kind) + vec_bytes(G.face_start, kind) + vec_bytes(G.face_p, kind) +
         vec_bytes(G.face_axis, kind) + vec_bytes(G.face_shape, kind) + vec_bytes(G.jstart, kind) + vec_bytes(G.jref, kind) + vec_bytes(G.u, kind);
  }
  return n;
}

void World::Impl::announce_bodies() {
  for (auto& bp : rigid_.bodies) {
    Body& b = *bp;
    if (b.announced || b.link) continue;  // (a link is its articulation's, not a piece)
    WorldEvent ev;
    ev.kind = WorldEvent::Kind::PieceAdded;
    ev.id = b.id;
    ev.parent = b.parent;
    ev.pos = b.x;
    ev.rot = b.q;
    ev.vel = b.v;
    ev.ang = b.w;
    ev.voxels = b.count;
    b.announced = true;
    events_.push_back(std::move(ev));
  }
}

void World::Impl::remove_bodies(std::vector<i64> ids, PieceEnd end) {
  if (ids.empty()) return;
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  for (i64 id : ids) {
    const Body* b = rigid_.find(id);
    if (!b) continue;
    dead_loads_.erase(id);
    // what rested on it falls (a split piece's parts take its place; where it lost voxels on the
    // way, split_body woke what rested on it)
    if (end != PieceEnd::Split) wake_around(*b);
    if (!b->announced) continue;  // (made and gone within one tick: never reported)
    WorldEvent ev;
    ev.kind = WorldEvent::Kind::PieceRemoved;
    ev.end = end;
    ev.id = id;
    ev.pos = b->x;
    ev.rot = b->q;
    ev.vel = b->v;
    ev.ang = b->w;
    ev.voxels = b->count;
    events_.push_back(std::move(ev));
  }
  rigid_.remove_if([&](const Body& b) { return std::binary_search(ids.begin(), ids.end(), b.id); });
  // (a carrier gone other than by breaking - removed, culled, fallen out, archived - takes its
  // wheels with it; a split's parts keep theirs: flush_body_changes)
  if (end != PieceEnd::Split && !att_.wheels.empty())
    for (size_t k = att_.wheels.size(); k-- > 0;) {
      const i64 on = att_.wheels[k].mount.piece;
      if (on <= 0 || !std::binary_search(ids.begin(), ids.end(), on)) continue;
      att_.wheels.erase(att_.wheels.begin() + static_cast<std::ptrdiff_t>(k));
      rigid_.wheels.erase(rigid_.wheels.begin() + static_cast<std::ptrdiff_t>(k));
    }
}

void World::Impl::limit_bodies() {
  // Beyond max_bodies, or beyond the pieces' memory budget, what is least missed goes:
  //   - beyond the budget, first the fracture solvers of awake pieces, the largest first
  //     (release_solvers; sleeping pieces hold none): assembled again at their next stress check,
  //     nothing leaves the world;
  //   - then the smallest pieces are culled, sleeping ones first (rubble at rest), then moving
  //     ones (the finest debris of a collapse). Kept pieces are not: the host's, a joint's (a
  //     machine's parts, what hangs on it), a carrier on wheels.
  // (By what the pieces use - Bytes::Used: the same decisions on every platform.)
  const i64 budget = budget_bytes(cfg_.memory.piece_mb);
  i64 bytes = 0;
  for (const auto& bp : rigid_.bodies) bytes += body_bytes(*bp, Bytes::Used);
  i32 excess = static_cast<i32>(rigid_.bodies.size()) - cfg_.max_bodies;
  if (excess <= 0 && bytes <= budget) return;
  if (bytes > budget && cfg_.release_solvers) {
    std::vector<std::pair<i64, i64>> solvers;  // (- the bytes it frees, id)
    for (const auto& bp : rigid_.bodies)
      if (bp->graph)
        if (const i64 s = bp->graph->P.solver_bytes(Bytes::Used); s > 0) solvers.push_back({-s, bp->id});
    std::sort(solvers.begin(), solvers.end());
    for (const auto& [freed, id] : solvers) {
      if (bytes <= budget) break;
      rigid_.find(id)->graph->P.release();
      bytes += freed;
      ++st_.released_solvers;
    }
    if (excess <= 0 && bytes <= budget) return;
  }
  std::vector<i64> jointed;
  for (const Joint& j : rigid_.joints)
    if (!j.broken)
      for (const i64 id : {j.a.body, j.b.body})
        if (id != 0) jointed.push_back(id);
  for (const Wheel& w : rigid_.wheels)  // (a carrier on wheels)
    if (!w.broken && w.body != 0) jointed.push_back(w.body);
  std::sort(jointed.begin(), jointed.end());
  std::vector<std::tuple<int, i32, i64>> cand;  // (awake, voxels, id)
  for (const auto& bp : rigid_.bodies)
    if (!bp->keep && !bp->link && !std::binary_search(jointed.begin(), jointed.end(), bp->id)) cand.push_back({bp->asleep ? 0 : 1, bp->count, bp->id});
  std::sort(cand.begin(), cand.end());
  std::vector<i64> ids;
  for (const auto& [awake, voxels, id] : cand) {
    if (excess <= 0 && bytes <= budget) break;
    const Body* b = rigid_.find(id);
    bytes -= body_bytes(*b, Bytes::Used);
    --excess;
    ids.push_back(id);
  }
  st_.culled_pieces += static_cast<i64>(ids.size());
  remove_bodies(std::move(ids), PieceEnd::Culled);
}

std::vector<PieceState> World::Impl::pieces() const {
  std::vector<PieceState> out;
  out.reserve(rigid_.bodies.size());
  for (const auto& bp : rigid_.bodies) {
    const Body& b = *bp;
    if (!b.announced) continue;
    PieceState p;
    p.id = b.id;
    p.pos = b.x;
    p.rot = b.q;
    p.vel = b.v;
    p.ang = b.w;
    p.voxels = b.count;
    p.mass = b.mass;
    p.asleep = b.asleep;
    out.push_back(p);
  }
  return out;
}

const Body* World::Impl::piece(i64 id) const {
  const auto it = std::lower_bound(rigid_.bodies.begin(), rigid_.bodies.end(), id,
                                   [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
  return it != rigid_.bodies.end() && (*it)->id == id && !(*it)->link ? it->get() : nullptr;
}

}  // namespace svx
