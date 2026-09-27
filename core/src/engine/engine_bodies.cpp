// structvox v2 — rigid pieces: creation from the world, stress under contact and inertia,
// fracture and splitting, carving, blasts, announcement to the front end (docs/V2_DESIGN.md §4).
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#include "svx/base/parallel.hpp"
#include "svx/engine/engine.hpp"
#include "svx/engine/engine_internal.hpp"

namespace svx {

using namespace engine_detail;

Body* Engine::make_body_from_world(const std::vector<FragKey>& frags, const V3& v, const V3& w) {
  if (frags.empty()) return nullptr;
  const f64 h = grid_.h;
  auto b = std::make_unique<Body>();
  std::vector<IVec3> vox;
  std::vector<i32> vfrag;
  for (const FragKey& f : frags) {
    FragChunk* fc = frag_chunk_if(f.chunk);
    if (!fc || f.idx < 0 || f.idx >= static_cast<i32>(fc->frags.size())) continue;
    const FragInfo& fi = fc->frags[size_t(f.idx)];
    if (fi.count <= 0) continue;
    const size_t before = vox.size();
    voxels_of(f, vox);
    if (vox.size() == before) continue;
    const i32 j = static_cast<i32>(b->frags.size());
    for (size_t k = before; k < vox.size(); ++k) vfrag.push_back(j);
    BodyFrag bf;
    bf.com = fi.com;
    bf.mass = fi.mass;
    bf.inertia = fi.inertia;
    bf.mat = fi.mat;
    bf.count = static_cast<i32>(vox.size() - before);
    bf.strength = class_mult(frag_class(f));
    b->frags.push_back(bf);
  }
  if (vox.empty()) return nullptr;
  IVec3 lo{INT_MAX, INT_MAX, INT_MAX}, hi{INT_MIN, INT_MIN, INT_MIN};
  for (const IVec3& p : vox)
    for (int a = 0; a < 3; ++a) {
      lo[a] = std::min(lo[a], p[a]);
      hi[a] = std::max(hi[a], p[a]);
    }
  BodyShape& S = b->shape;
  S.lo = lo;
  S.dim = {hi[0] - lo[0] + 1, hi[1] - lo[1] + 1, hi[2] - lo[2] + 1};
  const size_t cells = size_t(S.dim[0]) * size_t(S.dim[1]) * size_t(S.dim[2]);
  S.vox.assign(cells, kAir);
  S.frag.assign(cells, 0);
  S.brk.assign(cells, 0);
  for (size_t k = 0; k < vox.size(); ++k) {
    const i32 i = S.index(vox[k]);
    S.vox[size_t(i)] = static_cast<Vox>(grid_.get(vox[k]) & ~kAnchorBit);
    S.frag[size_t(i)] = static_cast<u16>(vfrag[k] + 1);
    ++S.count;
  }
  for (const IVec3& p : vox) {
    const i32 i = S.index(p);
    for (int a = 0; a < 3; ++a) {
      IVec3 q = p;
      q[a] += 1;
      const i32 j = S.index(q);
      if (j < 0 || !vox_solid(S.vox[size_t(j)])) continue;
      if (grid_.broken(p, a)) S.brk[size_t(i)] |= static_cast<u8>(1u << a);
    }
  }
  body_refresh(*b, h, cfg_.rigid.max_points);
  b->id = next_id_++;
  b->x = b->com;
  b->q = Quat{};
  b->v = v;
  b->w = w;
  b->v_pre = v;
  b->w_pre = w;
  b->refresh_box();
  // the voxels leave the grid; the fragment caches that are current are patched in place (the
  // structures holding the remaining fragments stay valid)
  std::unordered_map<u64, bool> current;
  for (const FragKey& f : frags) {
    if (current.count(f.chunk)) continue;
    FragChunk* fc = frag_chunk_if(f.chunk);
    const Chunk* ch = grid_.chunk(unkey3(f.chunk));
    current[f.chunk] = fc && ch && fc->vox_version == ch->vox_version;
  }
  for (const IVec3& p : vox) grid_.set(p, kAir);
  for (const FragKey& f : frags) {
    if (!current[f.chunk]) continue;
    FragChunk* fc = frag_chunk_if(f.chunk);
    const Chunk* ch = grid_.chunk(unkey3(f.chunk));
    if (!fc || !ch) continue;
    for (i32 k = fc->vox_start[size_t(f.idx)]; k < fc->vox_start[size_t(f.idx) + 1]; ++k) {
      const i32 i = fc->vox[size_t(k)];
      if (fc->id[size_t(i)] == static_cast<u16>(f.idx + 1)) fc->id[size_t(i)] = 0;
    }
    fc->frags[size_t(f.idx)].count = 0;
    fc->vox_version = ch->vox_version;
    auto ot = owner_.find(f.chunk);
    if (ot != owner_.end() && f.idx < static_cast<i32>(ot->second.size())) ot->second[size_t(f.idx)] = 0;
  }
  const V3 m{h, h, h};
  rigid_.wake_box(V3{h * lo[0], h * lo[1], h * lo[2]} - m * 2.0, V3{h * hi[0], h * hi[1], h * hi[2]} + m * 2.0);
  st_.detached_voxels += static_cast<i64>(vox.size());
  if (static_cast<i32>(vox.size()) < cfg_.min_body_voxels) {
    // (a shard: dust and a few chips for the front end, not a rigid piece)
    if (crack_budget_ > 0) {
      --crack_budget_;
      EngineEvent ev;
      ev.kind = EngineEvent::Kind::Crack;
      ev.pos = to_arr(b->x);
      ev.vel = to_arr(v);
      ev.strength = 1.5;
      ev.voxels = static_cast<i32>(vox.size());
      ev.radius = 0.5 * h * std::cbrt(static_cast<f64>(vox.size()));
      events_.push_back(std::move(ev));
    }
    st_.pulverized_voxels += static_cast<i64>(vox.size());
    return nullptr;
  }
  ++st_.detached_pieces;
  Body* ptr = b.get();
  rigid_.add(std::move(b));
  return ptr;
}

void Engine::rebuild_body_graph(Body& b) {
  const f64 h = grid_.h;
  if (!b.graph) b.graph = std::make_shared<BodyGraph>();
  BodyGraph& G = *b.graph;
  G.P = StressProblem{};
  const i32 nf = static_cast<i32>(b.frags.size());
  // fragment-level bonds from the shape
  std::vector<SecAcc> fine;
  std::unordered_map<u64, i32> index;
  const BodyShape& S = b.shape;
  const i32 cells = static_cast<i32>(S.vox.size());
  for (i32 i = 0; i < cells; ++i) {
    if (!vox_solid(S.vox[size_t(i)])) continue;
    const i32 fp = S.frag[size_t(i)] - 1;
    if (fp < 0) continue;
    const IVec3 p = S.voxel(i);
    for (int a = 0; a < 3; ++a) {
      if ((S.brk[size_t(i)] >> a) & 1) continue;
      IVec3 q = p;
      q[a] += 1;
      const i32 j = S.index(q);
      if (j < 0 || !vox_solid(S.vox[size_t(j)])) continue;
      const i32 fq = S.frag[size_t(j)] - 1;
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
      } else {
        ai = it->second;
      }
      fine[size_t(ai)].add(p, a, fp < fq ? 1 : -1);
    }
  }
  // resolution: clusters of fragments for large pieces (cells in the shape frame)
  i64 live = 0;
  for (const BodyFrag& f : b.frags) live += f.count > 0 ? 1 : 0;
  const i32 cell = cluster_cell(live);
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
  std::vector<i32> cl;
  cluster_items(key, links, &cl);
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
  for (const SecAcc& A0 : merged) {
    if (A0.a < 0 || A0.b < 0) continue;
    SecAcc A = A0;
    A.mb = nmat[size_t(A.b)];
    A.strength_b = nstr[size_t(A.b)];
    SBond B = A.finish(h, G.P.nodes[size_t(A.a)].c, &G.P.nodes[size_t(A.b)].c, nmat[size_t(A.a)], nstr[size_t(A.a)]);
    B.tag = static_cast<i32>(G.P.bonds.size());
    G.P.bonds.push_back(B);
    for (size_t k = 0; k < A.faces.size(); ++k) {
      G.face_p.push_back(A.faces[k]);
      G.face_axis.push_back(A.fax[k]);
    }
    G.face_start.push_back(static_cast<i32>(G.face_p.size()));
  }
  G.u.assign(6 * size_t(n), 0.0);
  {
    std::vector<i32> comp;
    std::vector<u8> seed(static_cast<size_t>(n), 0);
    if (n > 0) seed[0] = 1;
    G.components = n > 0 ? graph_components(n, G.P.bonds, seed, &comp) : 0;
  }
  b.graph_dirty = false;
}

void Engine::refragment_body(Body& b) {
  const f64 h = grid_.h;
  BodyShape& S = b.shape;
  const i32 cells = static_cast<i32>(S.vox.size());
  std::vector<i32> nl(size_t(cells), -1);
  std::vector<BodyFrag> nf;
  std::vector<std::array<f64, 10>> sums;
  std::vector<i32> stack;
  const i32 stride[3] = {S.dim[1] * S.dim[2], S.dim[2], 1};
  S.count = 0;
  for (i32 i = 0; i < cells; ++i) {
    if (!vox_solid(S.vox[size_t(i)]) || nl[size_t(i)] >= 0) continue;
    const u16 lab = S.frag[size_t(i)];
    const i32 f = static_cast<i32>(nf.size());
    BodyFrag bf;
    bf.mat = vox_mat(S.vox[size_t(i)]);
    bf.strength = (lab > 0 && lab <= b.frags.size()) ? b.frags[size_t(lab - 1)].strength : 1.0;
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
      accumulate_voxel(material(vox_mat(S.vox[size_t(k)])).rho * h * h * h, V3{h * p[0], h * p[1], h * p[2]}, h,
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
  for (size_t f = 0; f < nf.size(); ++f) {
    const MassProps mp = finish_mass(sums[f].data(), h);
    nf[f].mass = mp.mass;
    nf[f].com = mp.com;
    nf[f].inertia = mp.inertia;
  }
  for (i32 i = 0; i < cells; ++i) S.frag[size_t(i)] = nl[size_t(i)] >= 0 ? static_cast<u16>(nl[size_t(i)] + 1) : 0;
  b.frags.swap(nf);
  b.graph_dirty = true;
}

std::vector<i32> Engine::body_stress(Body& b, const std::vector<PointForce>& forces, bool inertia, f64 energy,
                                     std::vector<i32>* crushed) {
  StressOut o;
  body_stress_run(b, forces, inertia, energy, o);
  apply_stress_out(o);
  if (crushed) *crushed = o.crushed;
  return o.broken;
}

void Engine::apply_stress_out(const StressOut& o) {
  st_.body_checks += o.checks;
  st_.pcg_iters += o.pcg_iters;
  st_.bonds_broken += static_cast<i64>(o.broken.size());
  st_.impact_breaks += o.impact_breaks;
  st_.steady_breaks += o.steady_breaks;
  for (int m = 0; m < 4; ++m) st_.mode_breaks[m] += o.modes[m];
  for (const auto& [p, n] : o.cracks) crack_event(p, n, 1.0);
}

void Engine::body_stress_run(Body& b, const std::vector<PointForce>& forces, bool inertia, f64 energy, StressOut& o) {
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
    StressOptions so;
    so.rtol = cfg_.body_stress_rtol;
    so.amg_min_nodes = 0;  // (small pieces: the coarsest level is the whole graph, solved exactly)
    if (!G.P.assemble(so)) return;
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
  static const bool dbg = std::getenv("SVX_DEBUG_BODY") != nullptr;
  f64 spent = 0.0;
  std::vector<std::pair<f64, i32>> over;
  std::vector<FailMode> modes(G.P.bonds.size(), FailMode::None);
  std::vector<i32> comp;
  bool rebuilt = false;
  for (i32 round = 0; round < rounds; ++round) {
    PcgResult r = G.P.solve(f, G.u, cfg_.body_stress_rtol, cfg_.body_stress_maxit, true);
    if (!r.converged && (r.breakdown || r.rel_res > 5.0 * cfg_.body_stress_rtol) && round > 0 && !rebuilt) {
      // (the multigrid of the intact piece is stale after the breaks and the chips' retirement:
      // rebuilt once for the damaged piece)
      rebuilt = true;
      G.P.invalidate();
      StressOptions so;
      so.rtol = cfg_.body_stress_rtol;
      so.amg_min_nodes = 0;
      if (G.P.assemble(so)) r = G.P.solve(f, G.u, cfg_.body_stress_rtol, cfg_.body_stress_maxit, true);
    }
    if (!r.converged && (r.breakdown || r.rel_res > 5.0 * cfg_.body_stress_rtol)) {
      if (dbg && b.shape.count > 5000)
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
      const f64 phi = bond_utilization(B, G.P.bond_load(k, G.u), par_.fragility, &modes[size_t(k)]);
      maxphi = std::max(maxphi, phi);
      if (phi >= 1.0) over.push_back({phi, k});
    }
    if (round == 0) b.last_phi = maxphi;
    if (dbg && b.shape.count > 5000) {
      f64 fs = 0.0;
      for (const PointForce& pf : forces) fs += norm(pf.F);
      std::printf("  [body t%lld r%d] id %lld: %d voxels, %zu nodes (%zu bonds), %zu forces sum %.3g N (weight %.3g), pcg %d (%.1e), max phi %.2f, %zu over, energy %.3g (spent %.3g)\n",
                  static_cast<long long>(st_.ticks), round, static_cast<long long>(b.id), b.shape.count, G.P.nodes.size(), G.P.bonds.size(),
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
        const f64 cost = cfg_.fracture_energy * crush * B.area * std::min(material(B.ma).Gf, material(B.mb).Gf) * B.strength;
        if (spent + cost > energy) {
          poor = true;
          break;
        }
        spent += cost;
      }
      out.push_back(k);
    }
    for (size_t q = before; q < out.size(); ++q) {
      const i32 k = out[q];
      G.P.remove_bond(k);
      const SBond& B = G.P.bonds[size_t(k)];
      for (i32 e = G.face_start[size_t(k)]; e < G.face_start[size_t(k) + 1]; ++e) {
        const i32 i = b.shape.index(G.face_p[size_t(e)]);
        if (i >= 0) b.shape.brk[size_t(i)] |= static_cast<u8>(1u << G.face_axis[size_t(e)]);
      }
      ++(impact ? o.impact_breaks : o.steady_breaks);
      if (modes[size_t(k)] == FailMode::Crush) o.crushed.push_back(k);
      ++o.modes[static_cast<int>(modes[size_t(k)])];
      o.cracks.push_back({b.to_world(B.p), rotate(b.q, B.n)});
    }
    if (poor || out.size() == before || round + 1 == rounds) {
      if (dbg && b.shape.count > 5000) std::printf("    [round %d] stop: poor %d, none %d, last %d\n", round, poor ? 1 : 0, out.size() == before ? 1 : 0, round + 1 == rounds ? 1 : 0);
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
    if (dbg && b.shape.count > 5000) {
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

bool Engine::pulverize(Body& b, const std::vector<i32>& crushed) {
  if (crushed.empty() || !b.graph) return false;
  const BodyGraph& G = *b.graph;
  BodyShape& S = b.shape;
  std::vector<u8> kill(b.frags.size(), 0);
  for (i32 k : crushed) {
    const SBond& B = G.P.bonds[size_t(k)];
    if (B.b < 0) continue;
    const i32 side = G.node_mass[size_t(B.a)] <= G.node_mass[size_t(B.b)] ? B.a : B.b;
    for (i32 e = G.face_start[size_t(k)]; e < G.face_start[size_t(k) + 1]; ++e) {
      IVec3 p = G.face_p[size_t(e)];
      for (int s2 = 0; s2 < 2; ++s2) {
        if (s2) p[G.face_axis[size_t(e)]] += 1;
        const i32 i = S.index(p);
        if (i < 0) continue;
        const i32 f = S.frag[size_t(i)] - 1;
        if (f >= 0 && f < static_cast<i32>(kill.size()) && G.frag_node[size_t(f)] == side) kill[size_t(f)] = 1;
      }
    }
  }
  i64 removed = 0;
  std::vector<V3> at(b.frags.size());
  std::vector<i32> cnt(b.frags.size(), 0);
  const f64 h = grid_.h;
  for (i32 i = 0; i < static_cast<i32>(S.vox.size()); ++i) {
    if (!vox_solid(S.vox[size_t(i)])) continue;
    const i32 f = S.frag[size_t(i)] - 1;
    if (f < 0 || !kill[size_t(f)]) continue;
    const IVec3 p = S.voxel(i);
    at[size_t(f)] += V3{h * p[0], h * p[1], h * p[2]};
    ++cnt[size_t(f)];
    S.vox[size_t(i)] = kAir;
    S.frag[size_t(i)] = 0;
    S.brk[size_t(i)] = 0;
    ++removed;
  }
  if (!removed) return false;
  for (size_t f = 0; f < kill.size(); ++f) {
    if (!cnt[f]) continue;
    const V3 c = at[f] * (1.0 / cnt[f]);
    // (dust: a crack event of full strength, the size of what was crushed)
    if (crack_budget_ > 0) {
      --crack_budget_;
      EngineEvent ev;
      ev.kind = EngineEvent::Kind::Crack;
      ev.pos = to_arr(b.to_world(c));
      const V3 vel = b.v + cross(b.w, b.to_world(c) - b.x);
      ev.vel = to_arr(vel);
      ev.strength = 2.0;
      ev.voxels = cnt[f];
      ev.radius = 0.5 * h * std::cbrt(static_cast<f64>(cnt[f]));
      events_.push_back(std::move(ev));
    }
  }
  st_.pulverized_voxels += removed;
  refragment_body(b);
  b.graph_dirty = true;
  return true;
}

std::unique_ptr<Body> Engine::sub_body(const Body& parent, const std::vector<i32>& voxels, const std::vector<i32>& frag_map,
                                       bool use_pre) {
  auto c = std::make_unique<Body>();
  const BodyShape& P = parent.shape;
  IVec3 lo{INT_MAX, INT_MAX, INT_MAX}, hi{INT_MIN, INT_MIN, INT_MIN};
  for (i32 i : voxels) {
    const IVec3 p = P.voxel(i);
    for (int a = 0; a < 3; ++a) {
      lo[a] = std::min(lo[a], p[a]);
      hi[a] = std::max(hi[a], p[a]);
    }
  }
  // fragments of the child, in parent order
  std::vector<i32> remap(parent.frags.size(), -1);
  for (i32 i : voxels) {
    const i32 fp = P.frag[size_t(i)] - 1;
    if (fp >= 0 && remap[size_t(fp)] < 0) remap[size_t(fp)] = 0;
  }
  for (size_t k = 0; k < parent.frags.size(); ++k)
    if (remap[k] == 0) {
      remap[k] = static_cast<i32>(c->frags.size());
      c->frags.push_back(parent.frags[k]);
    }
  (void)frag_map;
  BodyShape& S = c->shape;
  S.lo = lo;
  S.dim = {hi[0] - lo[0] + 1, hi[1] - lo[1] + 1, hi[2] - lo[2] + 1};
  const size_t cells = size_t(S.dim[0]) * size_t(S.dim[1]) * size_t(S.dim[2]);
  S.vox.assign(cells, kAir);
  S.frag.assign(cells, 0);
  S.brk.assign(cells, 0);
  for (i32 i : voxels) {
    const IVec3 p = P.voxel(i);
    const i32 j = S.index(p);
    S.vox[size_t(j)] = P.vox[size_t(i)];
    const i32 fp = P.frag[size_t(i)] - 1;
    S.frag[size_t(j)] = fp >= 0 ? static_cast<u16>(remap[size_t(fp)] + 1) : 0;
    S.brk[size_t(j)] = P.brk[size_t(i)];
    ++S.count;
  }
  body_refresh(*c, grid_.h, cfg_.rigid.max_points);
  c->id = next_id_++;
  c->q = parent.q;
  c->x = parent.to_world(c->com);
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

bool Engine::split_body(Body& b, bool use_pre, bool force_replace) {
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
  std::vector<std::vector<i32>> parts(size_t(std::max(1, nc)));
  const BodyShape& S = b.shape;
  for (i32 i = 0; i < static_cast<i32>(S.vox.size()); ++i) {
    if (!vox_solid(S.vox[size_t(i)])) continue;
    const i32 f = S.frag[size_t(i)] - 1;
    const i32 c = f >= 0 ? frag_comp[size_t(f)] : -1;
    if (c >= 0) parts[size_t(c)].push_back(i);
  }
  for (auto& part : parts) {
    if (part.empty()) continue;
    if (nc > 1 && static_cast<i32>(part.size()) < cfg_.min_body_voxels) {
      // (a shard: dust and a few chips for the front end, not a rigid piece)
      V3 c;
      for (i32 i : part) {
        const IVec3 p = S.voxel(i);
        c += V3{grid_.h * p[0], grid_.h * p[1], grid_.h * p[2]};
      }
      c *= 1.0 / static_cast<f64>(part.size());
      const V3 X = b.to_world(c);
      const V3 vp = use_pre ? b.v_pre : b.v, wp = use_pre ? b.w_pre : b.w;
      if (crack_budget_ > 0) {
        --crack_budget_;
        EngineEvent ev;
        ev.kind = EngineEvent::Kind::Crack;
        ev.pos = to_arr(X);
        ev.vel = to_arr(vp + cross(wp, X - b.x));
        ev.strength = 1.5;
        ev.voxels = static_cast<i32>(part.size());
        ev.radius = 0.5 * grid_.h * std::cbrt(static_cast<f64>(part.size()));
        events_.push_back(std::move(ev));
      }
      st_.pulverized_voxels += static_cast<i64>(part.size());
      continue;
    }
    pending_add_.push_back(sub_body(b, part, frag_comp, use_pre));
    if (use_pre && nc > 1) {
      // (broken in this substep's collision: the parts part for the rest of it)
      pending_add_.back()->family = b.id;
      pending_add_.back()->family_ticks = 1;
    }
  }
  pending_retire_.push_back(b.id);
  if (nc > 1) ++st_.body_splits;
  return true;
}

void Engine::flush_body_changes() {
  if (!pending_retire_.empty()) {
    std::sort(pending_retire_.begin(), pending_retire_.end());
    rigid_.remove_if([&](const Body& b) { return std::binary_search(pending_retire_.begin(), pending_retire_.end(), b.id); });
    for (i64 id : pending_retire_) {
      retired_.push_back(id);
      dead_loads_.erase(id);
    }
    pending_retire_.clear();
  }
  for (auto& c : pending_add_) rigid_.add(std::move(c));
  pending_add_.clear();
}

namespace {

// A rigid solver's contact forces are one of many statically admissible answers: a piece resting
// on many points may carry its whole weight on a few of them, which a stress check reads as
// crushing point loads. The stress check takes the elastic answer instead: the same net force and
// moment shared over the contact points as by a rigid body on equal springs (the least-squares
// distribution), f_c = u + theta x r_c.
template <class PF>
void spread_contact_forces(std::vector<PF>& fs) {
  const size_t n = fs.size();
  if (n < 2) return;
  V3 F, c0;
  for (const auto& pf : fs) {
    F += pf.F;
    c0 += pf.p;
  }
  c0 *= 1.0 / static_cast<f64>(n);
  V3 M;
  M3 A;
  f64 r2sum = 0.0;
  for (const auto& pf : fs) {
    const V3 r = pf.p - c0;
    M += cross(r, pf.F);
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
  for (auto& pf : fs) pf.F = u + cross(theta, pf.p - c0);
}

}  // namespace

bool Engine::fracture_hook(f64 dt) {
  const auto& cs = rigid_.contacts();
  const size_t nb = rigid_.bodies.size();
  std::vector<std::vector<PointForce>> per(nb);
  std::vector<f64> fsum(nb, 0.0), approach(nb, 0.0), dissipated(nb, 0.0);
  // A rigid contact stops a body within one substep; a real impact takes the time a stress wave
  // (slowed by the crushing zone) needs to cross the piece. Contact impulses load a piece's
  // stress spread over that time: large pieces crush progressively from where they hit instead
  // of feeling a uniform deceleration of tens of g.
  std::vector<f64> kf(nb);
  for (size_t i = 0; i < nb; ++i) {
    const Body& b = *rigid_.bodies[i];
    const f64 tau = std::max(dt, 2.0 * b.radius / std::max(1.0, cfg_.impact_wave_speed));
    kf[i] = par_.impact / tau;
  }
  for (const Contact& c : cs) {
    const V3 J = c.impulse();
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
    const V3 Fa = J * kf[size_t(c.a)];
    per[size_t(c.a)].push_back({A.shape.frag[size_t(c.vox_a)] - 1, Fa, c.p});
    fsum[size_t(c.a)] += norm(Fa);
    if (c.b >= 0) {
      Body& B = *rigid_.bodies[size_t(c.b)];
      const V3 Fb = J * -kf[size_t(c.b)];
      per[size_t(c.b)].push_back({B.shape.frag[size_t(c.vox_b)] - 1, Fb, c.p});
      fsum[size_t(c.b)] += norm(Fb);
    }
  }
  // which pieces are checked (in body order)
  struct Check {
    size_t i;
    bool impact;
    f64 budget;
    bool split = false;  // (in several parts already: they go their own ways)
    StressOut out;
  };
  std::vector<Check> checks;
  for (size_t i = 0; i < nb; ++i) {
    Body& b = *rigid_.bodies[i];
    if (b.asleep || static_cast<i32>(b.frags.size()) < cfg_.min_fracture_frags) continue;
    if (b.stress_cooldown > 0) --b.stress_cooldown;
    const f64 weight = b.mass * cfg_.rigid.gravity;
    // A collision: a contact closing faster than jostling in a pile does (small pieces need a
    // harder knock). Its cracks are paid from the approach's kinetic energy, so rubble cannot
    // grind itself down and a hard landing shatters what it overloads.
    const f64 v_min = std::max(cfg_.body_impact_speed, cfg_.small_impact_speed * (1.0 - b.mass / cfg_.small_piece_mass));
    const bool impact = approach[i] > v_min && fsum[i] > cfg_.body_trigger * weight && b.stress_cooldown <= cfg_.body_check_ticks - 2;
    // resting on new supports (a first landing, rubble shifting under it, a load put on it):
    // checked once, again when the supporting forces changed by much of its weight
    const bool steady = fsum[i] > 0.5 * weight && b.stress_cooldown <= 0 && std::abs(fsum[i] - b.last_load) > 0.5 * weight;
    const f64 wr = norm(b.w);
    const bool spin = wr * wr * b.radius > 2.0 * cfg_.rigid.gravity && b.stress_cooldown <= 0;
    if (!impact && !steady && !spin) continue;
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
        EngineEvent ev;
        ev.kind = EngineEvent::Kind::Impact;
        ev.pos = to_arr(at * (1.0 / wsum));
        ev.radius = b.radius;
        ev.strength = dissipated[i];
        events_.push_back(std::move(ev));
      }
    }
    static const bool no_spread = std::getenv("SVX_NO_SPREAD") != nullptr;
    if (!no_spread) spread_contact_forces(per[i]);
    checks.push_back({i, impact, impact ? dissipated[i] : -1.0});
  }
  // the checks, concurrently (each touches only its piece; a single check keeps the threads for
  // its own solve)
  auto run = [&](Check& c) {
    Body& b = *rigid_.bodies[c.i];
    if (b.graph_dirty || !b.graph) rebuild_body_graph(b);
    if (b.graph->components > 1) {
      c.split = true;
      return;
    }
    body_stress_run(b, per[c.i], true, c.budget, c.out);
  };
  if (checks.size() == 1) {
    run(checks[0]);
  } else {
    parallel_for(static_cast<i64>(checks.size()), 1, [&](i64 k0, i64 k1) {
      SerialScope serial;
      for (i64 k = k0; k < k1; ++k) run(checks[size_t(k)]);
    });
  }
  // their outcomes, in body order
  bool changed = false;
  for (Check& c : checks) {
    Body& b = *rigid_.bodies[c.i];
    if (c.split) {
      if (split_body(b, true, false)) changed = true;
      continue;
    }
    b.stress_cooldown = c.impact ? cfg_.body_check_ticks : 3 * cfg_.body_check_ticks;
    b.last_load = fsum[c.i];
    apply_stress_out(c.out);
    if (c.out.broken.empty()) continue;
    const bool reshaped = cfg_.pulverize && pulverize(b, c.out.crushed);
    if (reshaped && b.shape.count == 0) {
      pending_retire_.push_back(b.id);
      changed = true;
      continue;
    }
    if (split_body(b, true, reshaped)) changed = true;
  }
  flush_body_changes();
  return changed;
}

void Engine::carve_bodies(const V3& c, f64 r) {
  const f64 h = grid_.h;
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
    const V3 s = b.to_shape(c);
    const IVec3 sv = voxel_of(s, h);
    const i32 R = static_cast<i32>(std::ceil(r / h)) + 1;
    i32 removed = 0;
    for (i32 x = sv[0] - R; x <= sv[0] + R; ++x)
      for (i32 y = sv[1] - R; y <= sv[1] + R; ++y)
        for (i32 z = sv[2] - R; z <= sv[2] + R; ++z) {
          const IVec3 p{x, y, z};
          const i32 i = b.shape.index(p);
          if (i < 0 || !vox_solid(b.shape.vox[size_t(i)])) continue;
          if (norm(V3{h * x, h * y, h * z} - s) > r) continue;
          b.shape.vox[size_t(i)] = kAir;
          b.shape.frag[size_t(i)] = 0;
          ++removed;
        }
    if (!removed) continue;
    refragment_body(b);
    rigid_.wake(b);
    if (b.shape.count == 0) {
      pending_retire_.push_back(b.id);
      continue;
    }
    split_body(b, false, true);  // (re-announced: its mesh changed)
  }
  flush_body_changes();
}

void Engine::blast_bodies(const PendingEvent& e) {
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

ChunkMesh Engine::body_mesh(const Body& b) const {
  const f64 h = grid_.h;
  VoxelGrid piece;
  piece.h = h;
  const BodyShape& S = b.shape;
  std::vector<u64> keys;
  for (i32 i = 0; i < static_cast<i32>(S.vox.size()); ++i) {
    if (!vox_solid(S.vox[size_t(i)])) continue;
    const IVec3 p = S.voxel(i);
    piece.set(p, S.vox[size_t(i)]);
    keys.push_back(key3(p[0] >> kChunkBits, p[1] >> kChunkBits, p[2] >> kChunkBits));
  }
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  MeshOptions mo;
  mo.texels_per_metre = mesh_base_.texels_per_metre;
  mo.texture = mesh_base_.texture;
  if (par_.debug_view == 2) {
    mo.debug = [&](const IVec3& p) -> u8 {
      const i32 i = S.index(p);
      if (i < 0) return 0;
      return static_cast<u8>(1 + (mix64(static_cast<u64>(b.id) * 131 + S.frag[size_t(i)]) % 254));
    };
  }
  ChunkMesh out;
  const M3 R = to_matrix(b.q);
  for (u64 k : keys) {
    const ChunkMesh m = mesh_chunk(piece, unkey3(k), mo, false);
    const u32 base = static_cast<u32>(out.vertices.size());
    for (MeshVertex v : m.vertices) {
      const V3 s{v.pos[0], v.pos[1], v.pos[2]};
      const V3 w = b.x + R * (s - b.com);
      v.pos[0] = static_cast<f32>(w.x);
      v.pos[1] = static_cast<f32>(w.y);
      v.pos[2] = static_cast<f32>(w.z);
      const V3 n = R * V3{v.normal[0] / 127.0, v.normal[1] / 127.0, v.normal[2] / 127.0};
      v.normal[0] = static_cast<i8>(std::lround(std::clamp(n.x, -1.0, 1.0) * 127.0));
      v.normal[1] = static_cast<i8>(std::lround(std::clamp(n.y, -1.0, 1.0) * 127.0));
      v.normal[2] = static_cast<i8>(std::lround(std::clamp(n.z, -1.0, 1.0) * 127.0));
      out.vertices.push_back(v);
    }
    for (u32 i : m.indices) out.indices.push_back(base + i);
  }
  return out;
}

void Engine::announce_bodies() {
  // new pieces' meshes (concurrently), then their events in body order
  std::vector<Body*> fresh;
  for (auto& bp : rigid_.bodies)
    if (!bp->announced) fresh.push_back(bp.get());
  std::vector<ChunkMesh> meshes(fresh.size());
  parallel_for(static_cast<i64>(fresh.size()), 4, [&](i64 k0, i64 k1) {
    SerialScope serial;
    for (i64 k = k0; k < k1; ++k) meshes[size_t(k)] = body_mesh(*fresh[size_t(k)]);
  });
  for (size_t k = 0; k < fresh.size(); ++k) {
    Body& b = *fresh[k];
    EngineEvent ev;
    ev.kind = EngineEvent::Kind::Detached;
    ev.id = b.id;
    ev.pos = to_arr(b.x);
    ev.vel = to_arr(b.v);
    ev.ang = to_arr(b.w);
    ev.voxels = b.shape.count;
    ev.rigid = true;
    ev.mesh = std::move(meshes[k]);
    b.x0 = b.x;
    b.q0 = b.q;
    b.announced = true;
    events_.push_back(std::move(ev));
  }
}

void Engine::limit_bodies() {
  for (Fading& f : fading_) f.t += cfg_.dt;
  fading_.erase(std::remove_if(fading_.begin(), fading_.end(), [&](const Fading& f) { return f.t >= cfg_.fade_time; }),
                fading_.end());
  const i32 excess = static_cast<i32>(rigid_.bodies.size()) - cfg_.max_bodies;
  if (excess <= 0) return;
  std::vector<std::pair<i32, i64>> cand;  // (voxels, id): the smallest sleeping pieces go first
  for (const auto& bp : rigid_.bodies)
    if (bp->asleep) cand.push_back({bp->shape.count, bp->id});
  std::sort(cand.begin(), cand.end());
  const size_t k = std::min(cand.size(), static_cast<size_t>(excess));
  std::vector<i64> ids;
  for (size_t i = 0; i < k; ++i) ids.push_back(cand[i].second);
  std::sort(ids.begin(), ids.end());
  for (i64 id : ids) {
    const Body* b = rigid_.find(id);
    if (!b) continue;
    fading_.push_back({id, 0.0, b->x, b->q * conj(b->q0)});
    dead_loads_.erase(id);
  }
  rigid_.remove_if([&](const Body& b) { return std::binary_search(ids.begin(), ids.end(), b.id); });
}

std::vector<PiecePose> Engine::pieces() const {
  std::vector<PiecePose> out;
  out.reserve(rigid_.bodies.size() + fading_.size());
  for (const auto& bp : rigid_.bodies) {
    const Body& b = *bp;
    if (!b.announced) continue;
    out.push_back({b.id, b.x, b.q * conj(b.q0), 1.0});
  }
  for (const Fading& f : fading_) out.push_back({f.id, f.pos, f.rot, std::max(0.0, 1.0 - f.t / cfg_.fade_time)});
  std::sort(out.begin(), out.end(), [](const PiecePose& a, const PiecePose& b) { return a.id < b.id; });
  return out;
}

}  // namespace svx
