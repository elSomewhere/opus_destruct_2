// svx_fixture_compare — golden parity against the prototype oracle (plan §E.2).
//
// Rebuilds each fixture scene (tests/fixtures/prototype/*.json, schema svx-fixture-v1) in
// the structvox lattice and compares the damage-free static equilibrium with the oracle's
// `static` block. Two checks per scene and variant:
//   * model parity:  structvox's own section/bond model (svx::make_bond) vs the compiled
//                    k0 of the fixture (per interface archetype, ratio per component);
//   * solver parity: the fixture's k0 injected per edge, then displacements / rotations
//                    compared cell by cell (relative to the largest reference value).
// Partial supports (pinned, semifixed) map to per-cell DOF masks, spring supports to
// cell-centre springs; unilateral (contact) supports are reported as unsupported.
//
// Outcome mode (--outcome): the fixture's compiled interface archetypes (k0, onset/break
// thresholds, residual stiffness) are injected per edge and the oracle protocol is replayed:
// the gameplay blast prefracture (if any) followed by static closure (svx::solve_to_closure,
// corotational, rupture cap and order as the oracle). Compared by prototype id: blast
// removed cells / fractured edges / damaged edges, ruptured edges, deleted cells and the
// final alive set (symmetric difference relative to the initial cell count).
//
// Feel mode (--feel, plan Phase 5 feel spec): the fixture's XPBD captures (the prototype app's
// settle and gameplay blasts, `feel.runs.delete`) against the game law with the engine knobs
// (compliance S_p, fragility F, damping zeta, render amplification A) on the fixture's
// stiffness: settle fail / stand, settled sag, and per captured blast fail / stand, ruptures,
// deletions, event-induced peak sag (x A, as rendered) and time to quiet. --feel-grid scans
// S_p, F and zeta and ranks the settings (A fitted per setting to the captured peaks).
//
// usage: svx_fixture_compare [--variant prototype|fixY|fixJ|fixYJ] [--k0 fixture|model] [--tol T]
//                            [--outcome] [--law fixture|model] [--linear] [--verbose]
//                            [--feel | --feel-grid] [--sp S] [--fragility F] [--damping Z] [--amp A] FILE...
// (--linear: small-displacement kinematics; the oracle's static block is corotational)
// Exit code 1 if any listed scene fails to compare or exceeds the tolerance (outcome mode:
// any scene whose oracle closure converged with converged Newton solves that disagrees on
// collapse classification or has an alive-set difference above 2%).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <set>

#include <optional>

#include "nlohmann/json.hpp"
#include "svx/sim/blast.hpp"
#include "svx/sim/dynamics.hpp"
#include "svx/sim/statics.hpp"
#include "svx/topo/connectivity.hpp"
#include "svx/solve/lattice.hpp"
#include "svx/solve/multigrid.hpp"

using namespace svx;
using nlohmann::json;

namespace {

Vec6 k0_from(const json& a) {
  const json& k = a.at("k0");
  Vec6 p{};
  for (int i = 0; i < 6; ++i) p[i] = k.at(i).get<f64>();
  return proto_to_svx_order(p);  // [N,V1,V2,M1,M2,T] -> [N,V1,V2,T,M1,M2]
}

f64 num(const json& v) {
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    if (s == "Infinity") return INFINITY;
    if (s == "-Infinity") return -INFINITY;
    return NAN;
  }
  return v.get<f64>();
}

std::optional<LawParams> g_law;  // feel mode: the game law instead of the reference law

struct SceneResult {
  std::string id, status;
  i32 cells = 0;
  int iters = 0;
  f64 u_err = 0.0, th_err = 0.0, probe_ref = 0.0, probe_svx = 0.0;
  f64 k_ratio_min = 1.0, k_ratio_max = 1.0;
};

struct Scene {
  json d;
  Lattice L;
  std::map<i64, i32> id2idx;
  std::vector<i64> idx2id;
  std::vector<f64> f;
  std::string status;  // empty = ok
};

BondThresholds thresholds_from(const json& t) {
  BondThresholds b;
  b.open = t.at("openN").get<f64>();
  b.comp = t.at("compN").get<f64>();
  b.comp6 = {0.0, t.at("t1").get<f64>(), t.at("t2").get<f64>(), t.at("tau").get<f64>(), t.at("b1").get<f64>(),
             t.at("b2").get<f64>()};
  return b;
}

// Builds the scene lattice. inject: per-edge fixture k0 (and, with laws, thresholds + residual).
bool build_scene(Scene& S, const std::string& path, const std::string& variant, bool fixture_k0, bool fixture_law,
                 SceneResult& R) {
  std::ifstream in(path);
  S.d = json::parse(in);
  const json& d = S.d;
  R.id = d.at("scene").at("id").get<std::string>();
  const json& model = d.at("model");
  if (!d.at("variants").contains(variant)) {
    R.status = "no variant";
    return false;
  }
  const json& var = d.at("variants").at(variant);
  const f64 h = model.at("cellSize").get<f64>();
  const f64 g = d.at("scene").at("gravity").get<f64>();
  const json& tables = model.at("tables");
  // supports: exact DOF masks + springs (x springScale, or springOverride)
  struct Sup {
    u8 mask = 0;
    Vec6 spring{};
  };
  std::map<i64, Sup> support_by_id;
  for (const json& s : model.at("supports")) {
    const json& prof = tables.at("supportProfiles").at(s.at("profile").get<std::string>());
    Sup sp;
    for (int q = 0; q < 6; ++q) {
      if (prof.at("unilateral").at(q).get<bool>()) {
        R.status = "unsupported unilateral support '" + s.at("profile").get<std::string>() + "'";
        return false;
      }
      if (prof.at("exactMask").at(q).get<bool>()) sp.mask |= static_cast<u8>(1u << q);
      sp.spring[q] = num(prof.at("spring").at(q)) * s.value("springScale", 1.0);
    }
    if (!s.at("springOverride").is_null())
      for (int q = 0; q < 6; ++q) sp.spring[q] = num(s.at("springOverride").at(q));
    support_by_id[s.at("id").get<i64>()] = sp;
  }
  // cells
  const json& cells = model.at("cells");
  std::vector<CellIn> cin;
  bool has_applied = false;
  for (const json& c : cells) {
    CellIn ci;
    for (int q = 0; q < 3; ++q) ci.p[q] = c.at("ijk").at(q).get<i32>();
    bool ok = false;
    ci.mat = material_from_name(c.at("material").get<std::string>().c_str(), &ok);
    if (!ok) {
      R.status = "unknown material " + c.at("material").get<std::string>();
      return false;
    }
    const json& arch = tables.at("cellArchetypes").at(c.at("archetype").get<std::string>());
    for (int q = 0; q < 3; ++q) ci.eff[q] = arch.at("effDims").at(q).get<f64>();
    // structvox's archetype (member kind -> interface profile), when the name is known
    ci.arch = static_cast<i16>(archetype_by_name(c.at("archetype").get<std::string>().c_str()));
    if (!c.at("support").is_null()) {
      const Sup& sp = support_by_id.at(c.at("support").get<i64>());
      ci.anchored = sp.mask == kAllDofs;
      ci.fixmask = sp.mask;
      ci.spring = sp.spring;
    }
    has_applied = has_applied || c.contains("appliedForce") || c.contains("appliedMoment");
    S.id2idx[c.at("id").get<i64>()] = static_cast<i32>(cin.size());
    S.idx2id.push_back(c.at("id").get<i64>());
    cin.push_back(ci);
  }
  LatticeOptions lo;
  lo.h = h;
  if (g_law) lo.law = *g_law;
  lo.fixes.fix_y_axes = variant == "fixY" || variant == "fixYJ";
  lo.fixes.st_venant_j = variant == "fixJ" || variant == "fixYJ";
  S.L = build_lattice(cin, lo);
  Lattice& L = S.L;
  R.cells = L.n;
  // exact masses / inertias from the fixture
  for (const json& c : cells) {
    const i32 i = S.id2idx.at(c.at("id").get<i64>());
    L.mass[i] = c.at("mass").get<f64>();
    for (int q = 0; q < 3; ++q) L.inertia[i][q] = c.at("inertia").at(q).get<f64>();
  }
  // edges: keep exactly the fixture's bonds; optionally inject the fixture archetype per edge
  std::array<std::vector<u8>, 3> keep;
  for (int a = 0; a < 3; ++a) {
    keep[a].assign(L.n, 0);
    L.ext_id[a].assign(L.n, -1);
  }
  const json& archs = var.at("interfaceArchetypes");
  std::map<std::string, u16> injected;
  for (const json& e : model.at("edges")) {
    const i32 a = S.id2idx.at(e.at("a").get<i64>());
    const i32 b = S.id2idx.at(e.at("b").get<i64>());
    const int ax = e.at("axis").get<int>();
    if (L.nbr[ax][a] != b) {
      if (L.anchored[a] && L.anchored[b]) continue;  // anchored-anchored bonds are irrelevant
      R.status = "edge not found in lattice";
      return false;
    }
    keep[ax][a] = 1;
    L.ext_id[ax][a] = static_cast<i32>(e.at("id").get<i64>());
    // authored per-edge profile override (e.g. braceCorner braces, seamCompare seams)
    {
      bool okp = false;
      const Profile pe = profile_from_name(e.at("profile").get<std::string>().c_str(), &okp);
      if (okp && pe != L.bond(ax, a).profile) {
        const std::string pk = "profile:" + std::to_string(ax) + ":" + std::to_string(int(pe)) + ":" +
                               std::to_string(L.bid[ax][a]);
        auto it = injected.find(pk);
        if (it == injected.end()) {
          L.models.push_back(make_bond(ax, L.mat[a], L.eff[a], L.mat[b], L.eff[b], L.h, pe, L.fixes, L.law));
          it = injected.emplace(pk, static_cast<u16>(L.models.size() - 1)).first;
        }
        L.bid[ax][a] = it->second;
      }
    }
    const std::string key = e.at("archetype").get<std::string>();
    const json& aj = archs.at(key);
    const Vec6 k0 = k0_from(aj);
    const Vec6& km = L.bond(ax, a).k;
    for (int q = 0; q < 6; ++q) {
      const f64 r = km[q] / k0[q];
      R.k_ratio_min = std::min(R.k_ratio_min, r);
      R.k_ratio_max = std::max(R.k_ratio_max, r);
    }
    if (fixture_k0 || fixture_law) {
      auto it = injected.find(key);
      if (it == injected.end()) {
        BondModel bm = L.bond(ax, a);
        if (fixture_k0) bm.k = k0;
        if (fixture_law) {
          bm.onset = thresholds_from(aj.at("onset"));
          bm.brk = thresholds_from(aj.at("break"));
          bm.residual = aj.at("residualStiffness").get<f64>();
        }
        L.models.push_back(bm);
        it = injected.emplace(key, static_cast<u16>(L.models.size() - 1)).first;
      }
      L.bid[ax][a] = it->second;
    }
  }
  for (int a = 0; a < 3; ++a)
    for (i32 i = 0; i < L.n; ++i)
      if (L.nbr[a][i] >= 0 && !keep[a][i]) L.break_bond(i, a);
  // loads
  std::vector<f64>& f = S.f;
  f.assign(6 * size_t(L.n), 0.0);
  for (i32 i = 0; i < L.n; ++i)
    if (!((L.fixmask[i] >> 2) & 1)) f[6 * size_t(i) + 2] = L.mass[i] * g;
  if (has_applied)
    for (const json& c : cells) {
      const i32 i = S.id2idx.at(c.at("id").get<i64>());
      if (L.anchored[i]) continue;
      if (c.contains("appliedForce"))
        for (int q = 0; q < 3; ++q)
          if (!((L.fixmask[i] >> q) & 1)) f[6 * size_t(i) + q] += c.at("appliedForce").at(q).get<f64>();
      if (c.contains("appliedMoment"))
        for (int q = 0; q < 3; ++q)
          if (!((L.fixmask[i] >> (3 + q)) & 1)) f[6 * size_t(i) + 3 + q] += c.at("appliedMoment").at(q).get<f64>();
    }
  return true;
}

bool g_verbose = false;

SceneResult compare(const std::string& path, const std::string& variant, bool fixture_k0, bool corot) {
  SceneResult R;
  Scene S;
  if (!build_scene(S, path, variant, fixture_k0, false, R)) return R;
  const json& d = S.d;
  const json& st = d.at("variants").at(variant).at("static");
  if (!st.value("converged", false)) {
    R.status = "oracle static not converged";
    return R;
  }
  Lattice& L = S.L;
  const json& cells = d.at("model").at("cells");
  const auto& id2idx = S.id2idx;
  std::vector<f64>& f = S.f;
  // Components without support are not solved by the oracle either (their rows stay zero).
  {
    std::vector<f64> tmp;
    for (const auto& isl : unsupported_components(L))
      for (i32 c : isl) {
        L.remove_cell(c);
        for (int q = 0; q < 6; ++q) f[6 * size_t(c) + q] = 0.0;
      }
  }
  // solve (small scenes: many iterations, tight tolerance)
  MGOptions mo;
  mo.coarse_max = 64;
  std::vector<f64> u(f.size(), 0.0);
  if (corot) {
    StaticsOptions so;
    so.corot = true;
    so.damage = false;
    so.res_tol = 1e-11;
    so.res_accept = 1e-9;
    so.lin_rtol = 1e-6;
    so.mg = mo;
    so.verbose = g_verbose;
    const DamageField dc{};
    const EquilibriumStats es = solve_equilibrium(L, u, f, dc, so, nullptr);
    R.iters = es.pcg_iters;
    if (!es.converged) {
      R.status = "svx solve not converged";
      return R;
    }
  } else {
    Multigrid mg;
    mg.build(L, mo);
    const PcgStats ps = pcg_solve(mg, L.n, f.data(), u.data(), 1e-12, 5000, false);
    R.iters = ps.iters;
    if (!ps.converged) {
      R.status = "svx solve not converged";
      return R;
    }
  }
  // compare
  const json& ru = st.at("cells").at("u");
  const json& rt = st.at("cells").at("theta");
  f64 umax = 0.0, tmax = 0.0, ue = 0.0, te = 0.0;
  for (const json& c : cells) {
    const i64 id = c.at("id").get<i64>();
    const i32 i = id2idx.at(id);
    const size_t k = static_cast<size_t>(&c - &cells[0]);
    if (ru.at(k).is_null() || rt.at(k).is_null() || L.dead[i]) continue;  // unsolved (unsupported) cell
    for (int q = 0; q < 3; ++q) {
      const f64 uref = num(ru.at(k).at(q)), tref = num(rt.at(k).at(q));
      umax = std::max(umax, std::abs(uref));
      tmax = std::max(tmax, std::abs(tref));
      ue = std::max(ue, std::abs(u[6 * size_t(i) + q] - uref));
      te = std::max(te, std::abs(u[6 * size_t(i) + 3 + q] - tref));
    }
  }
  R.u_err = umax > 0 ? ue / umax : ue;
  // rotations are normalized by max(max |theta|, max |u| / h): scenes whose reference rotations
  // vanish by symmetry would otherwise compare round-off against round-off
  const f64 tscale = std::max(tmax, umax / S.L.h);
  R.th_err = tscale > 0 ? te / tscale : te;
  const json& probes = st.at("probes");
  if (probes.contains("minDz") && !probes.at("minDz").is_null()) {
    R.probe_ref = num(probes.at("minDz"));
    f64 mz = 0.0;
    for (i32 i = 0; i < L.n; ++i) mz = std::min(mz, u[6 * size_t(i) + 2]);
    R.probe_svx = mz;
  }
  R.status = "ok";
  return R;
}


struct OutcomeResult {
  std::string id, status;
  i32 cells = 0;
  bool gate = false;  // oracle closure converged with converged Newton solves in every pass
  i32 alive_ref = 0, alive_svx = 0, symdiff = 0;
  i32 rupt_ref = 0, rupt_svx = 0, rupt_common = 0;
  i32 removed_ref = 0, removed_svx = 0, frac_ref = 0, frac_svx = 0;
  bool blast = false, blast_exact = true;
  i32 dmg_ref = 0, dmg_svx = 0;
  f64 dmg_err = 0.0;
  int passes_ref = 0, passes_svx = 0;
  bool svx_converged = false;
  f64 dz_ref = 0.0, dz_svx = 0.0;
};

OutcomeResult outcome(const std::string& path, const std::string& variant, bool corot, bool verbose, bool fixture_k0,
                      bool fixture_law) {
  OutcomeResult O;
  SceneResult R;
  Scene S;
  if (!build_scene(S, path, variant, fixture_k0, fixture_law, R)) {
    O.id = R.id;
    O.status = R.status;
    return O;
  }
  O.id = R.id;
  O.cells = R.cells;
  const json& oc = S.d.at("variants").at(variant).at("outcome");
  O.gate = oc.value("converged", false) && oc.value("allPassesNewtonConverged", false);
  Lattice& L = S.L;
  L.enable_damage();
  std::set<i64> ref_removed, ref_frac;
  for (const json& x : oc.at("blastRemovedCellIds")) ref_removed.insert(x.get<i64>());
  for (const json& x : oc.at("blastFracturedEdgeIds")) ref_frac.insert(x.get<i64>());
  O.removed_ref = static_cast<i32>(ref_removed.size());
  O.frac_ref = static_cast<i32>(ref_frac.size());
  if (!oc.at("blast").is_null()) {
    O.blast = true;
    const json& b = oc.at("blast");
    BlastParams bp;
    for (int q = 0; q < 3; ++q) bp.center[q] = b.at("center").at(q).get<f64>();
    bp.radius = b.at("radius").get<f64>();
    const BlastResult br = apply_blast(L, nullptr, bp);
    std::set<i64> rm, fr(br.fractured.begin(), br.fractured.end());
    for (i32 c : br.removed) rm.insert(S.idx2id[c]);
    O.removed_svx = static_cast<i32>(rm.size());
    O.frac_svx = static_cast<i32>(fr.size());
    O.blast_exact = rm == ref_removed && fr == ref_frac;
  }
  StaticsOptions so;
  so.corot = corot;
  so.max_breaks = oc.at("settings").value("maxBreaksPerStep", 512);
  std::vector<f64> u;
  const ClosureResult cr = solve_to_closure(L, u, S.f, so);
  O.passes_svx = cr.passes;
  O.passes_ref = oc.value("passes", 0);
  O.svx_converged = cr.converged;
  // alive sets
  std::set<i64> a_ref, a_svx;
  for (const json& x : oc.at("aliveCellIds")) a_ref.insert(x.get<i64>());
  for (i32 i = 0; i < L.n; ++i)
    if (!L.dead[i]) a_svx.insert(S.idx2id[i]);
  O.alive_ref = static_cast<i32>(a_ref.size());
  O.alive_svx = static_cast<i32>(a_svx.size());
  for (i64 id : a_ref) O.symdiff += a_svx.count(id) ? 0 : 1;
  for (i64 id : a_svx) O.symdiff += a_ref.count(id) ? 0 : 1;
  // ruptures
  std::set<i64> r_ref, r_svx(cr.ruptured.begin(), cr.ruptured.end());
  for (const json& x : oc.at("rupturedEdgeIds")) r_ref.insert(x.get<i64>());
  O.rupt_ref = static_cast<i32>(r_ref.size());
  O.rupt_svx = static_cast<i32>(r_svx.size());
  for (i64 id : r_ref) O.rupt_common += r_svx.count(id) ? 1 : 0;
  // damage of surviving edges
  std::map<i64, f64> d_ref, d_svx;
  for (const json& x : oc.at("damagedEdges")) d_ref[x.at(0).get<i64>()] = x.at(1).get<f64>();
  for (int a = 0; a < 3; ++a)
    for (i32 i = 0; i < L.n; ++i) {
      const i32 j = L.nbr[a][i];
      if (j < 0 || L.dead[i] || L.dead[j] || L.dmg[a][i] <= 0.0f) continue;
      d_svx[bond_key(L, a, i)] = L.dmg[a][i];
    }
  O.dmg_ref = static_cast<i32>(d_ref.size());
  O.dmg_svx = static_cast<i32>(d_svx.size());
  for (const auto& [id, dv] : d_ref) {
    const auto it = d_svx.find(id);
    O.dmg_err = std::max(O.dmg_err, std::abs(dv - (it == d_svx.end() ? 0.0 : it->second)));
  }
  for (const auto& [id, dv] : d_svx)
    if (!d_ref.count(id)) O.dmg_err = std::max(O.dmg_err, dv);
  // probe
  const json& probes = oc.at("probes");
  if (probes.contains("minDz") && !probes.at("minDz").is_null()) O.dz_ref = num(probes.at("minDz"));
  for (i32 i = 0; i < L.n; ++i)
    if (!L.dead[i]) O.dz_svx = std::min(O.dz_svx, u[6 * size_t(i) + 2]);
  if (verbose)
    for (const PassLog& pl : cr.log)
      std::printf("    pass %d: eq conv=%d iters=%d pcg=%d res=%.2e maxd=%.3f maxphi=%.3f ruptured=%d deleted=%d\n", pl.pass,
                  pl.eq.converged ? 1 : 0, pl.eq.iters, pl.eq.pcg_iters, pl.eq.residual, pl.eq.max_damage,
                  pl.eq.max_phi, pl.ruptured, pl.deleted);
  O.status = "ok";
  return O;
}

struct FeelResult {
  std::string id, status;
  i32 cells = 0;
  // settle (from rest under self-weight)
  bool x_coll = false, s_coll = false;
  f64 x_sag = 0.0, s_sag = 0.0;  // settled sag (m); ours raw (x A when rendered)
  i32 x_rupt = 0, s_rupt = 0, x_del = 0, s_del = 0;
  // the first captured blast
  bool blast = false;
  bool xb_coll = false, sb_coll = false;
  i32 xb_rupt = 0, sb_rupt = 0, xb_del = 0, sb_del = 0;
  f64 xb_peak = 0.0, sb_peak = 0.0;    // event-induced peak sag (m); ours raw
  f64 xb_quiet = 0.0, sb_quiet = 0.0;  // s from the blast to quiet
};

FeelResult feel(const std::string& path, f64 Sp, f64 F, f64 zeta, bool corot) {
  FeelResult O;
  LawParams law;
  law.game = true;
  law.fragility = F;
  g_law = law;
  Scene S;
  SceneResult R;
  const bool built = build_scene(S, path, "prototype", true, false, R);
  g_law.reset();
  O.id = R.id;
  if (!built) {
    O.status = R.status;
    return O;
  }
  if (!S.d.contains("feel") || !S.d.at("feel").at("runs").contains("delete")) {
    O.status = "no feel capture";
    return O;
  }
  const json& fe = S.d.at("feel");
  const f64 tick = fe.at("xpbd").value("frameDt", 1.0 / 60.0);
  const json& run = fe.at("runs").at("delete");
  const json& xs = run.at("settle").at("summary");
  O.x_coll = xs.value("collapsed", false);
  O.x_sag = num(xs.value("finalSag", json(0.0)));
  O.x_rupt = xs.value("totalRuptures", 0);
  O.x_del = xs.value("totalDeleted", 0);
  Lattice& L = S.L;
  O.cells = L.n;
  L.enable_damage();
  DynamicsOptions dop;
  dop.compliance = Sp;
  dop.rayleigh_alpha = 2.0 * zeta * (2.0 * 3.14159265358979 * 5.0);  // as the engine maps damping
  dop.corot = corot;
  // pieces without any support fall at once (the engine's bake removes them): a collapse
  for (const auto& isl : unsupported_components(L)) {
    O.s_del += static_cast<i32>(isl.size());
    for (i32 c : isl) L.remove_cell(c);
  }
  Dynamics dyn;
  dyn.init(L, dop);
  dyn.settle_to_equilibrium();
  // settle: from the elastic rest state the law may rupture overloaded bonds (the XPBD settle)
  auto sag = [&](const std::vector<f64>& u, const std::vector<f64>* ref) {
    f64 m = 0.0;
    for (i32 i = 0; i < L.n; ++i) {
      if (L.dead[i] || L.anchored[i]) continue;
      const f64 dz = u[6 * size_t(i) + 2] - (ref ? (*ref)[6 * size_t(i) + 2] : 0.0);
      m = std::max(m, -dz);
    }
    return m;
  };
  for (int t = 0; t < 600 && !dyn.asleep(); ++t) {
    const StepStats st = dyn.step();
    O.s_rupt += st.ruptured;
    O.s_del += st.detached_cells;
  }
  O.s_sag = sag(dyn.u(), nullptr);
  // deleted pieces, or pieces hanging from pins as a mechanism, are a collapse
  O.s_coll = O.s_del > 0 || O.s_sag > 0.5;
  // the first captured blast, from the settled state
  const json& blasts = run.at("blasts");
  if (!blasts.empty()) {
    const json& b = blasts.begin().value();
    const json& bs = b.at("summary");
    O.blast = true;
    O.xb_coll = bs.value("collapsed", false);
    O.xb_rupt = bs.value("totalRuptures", 0);
    O.xb_del = bs.value("totalDeleted", 0);
    O.xb_peak = std::max(0.0, num(bs.value("peakSag", json(0.0))) - O.x_sag);
    O.xb_quiet = bs.value("quietTick", 0) * tick;
    BlastParams bp;
    for (int q = 0; q < 3; ++q) bp.center[q] = b.at("center").at(q).get<f64>();
    bp.radius = b.at("radius").get<f64>();
    const std::vector<f64> rest = dyn.u();
    dyn.wake();
    dyn.blast(bp);  // the cells it removes are the blast itself, not a collapse
    int t = 0;
    for (; t < 900; ++t) {
      const StepStats st = dyn.step();
      O.sb_rupt += st.ruptured;
      O.sb_del += st.detached_cells;
      O.sb_peak = std::max(O.sb_peak, sag(dyn.u(), &rest));
      if (dyn.asleep()) break;
    }
    O.sb_coll = O.sb_del > 0 || O.sb_peak > 0.5;
    O.sb_quiet = (t + 1) * dop.dt;
  }
  O.status = "ok";
  return O;
}

struct FeelScore {
  int settle_agree = 0, settle_n = 0, blast_agree = 0, blast_n = 0;
  f64 amp = 1.0;          // A fitted: median captured peak / our raw peak
  f64 peak_err = 0.0;     // median |log(A ours / captured)| over blasts with a captured peak
  f64 quiet_err = 0.0;    // median |log(ours / captured)| of the time to quiet
  f64 score() const {
    const f64 cls = (settle_n - settle_agree) + 2.0 * (blast_n - blast_agree);
    return cls + peak_err + quiet_err;
  }
};

f64 median(std::vector<f64> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

FeelScore score_feel(const std::vector<FeelResult>& rs, f64 amp_fixed) {
  FeelScore S;
  std::vector<f64> ratios, quiet;
  for (const FeelResult& r : rs) {
    if (r.status != "ok") continue;
    ++S.settle_n;
    S.settle_agree += r.x_coll == r.s_coll ? 1 : 0;
    if (!r.blast) continue;
    ++S.blast_n;
    S.blast_agree += r.xb_coll == r.sb_coll ? 1 : 0;
    if (r.xb_peak > 1e-5 && r.sb_peak > 1e-9) ratios.push_back(r.xb_peak / r.sb_peak);
    if (r.xb_quiet > 0.0 && r.sb_quiet > 0.0) quiet.push_back(std::abs(std::log(r.sb_quiet / r.xb_quiet)));
  }
  S.amp = amp_fixed > 0.0 ? amp_fixed : (ratios.empty() ? 1.0 : median(ratios));
  std::vector<f64> perr;
  for (f64 q : ratios) perr.push_back(std::abs(std::log(S.amp / q)));
  S.peak_err = median(perr);
  S.quiet_err = median(quiet);
  return S;
}

}  // namespace

int main(int argc, char** argv) {
  std::string variant = "prototype";
  bool fixture_k0 = true;
  bool outcome_mode = false, corot = true, verbose = false, fixture_law = true, feel_mode = false, feel_grid = false;
  f64 sp = 6.0, frag = 1.0, zeta = 0.1, amp = 0.0;
  f64 tol = 1e-4;  // relative displacement error allowed for scenes that compare "ok"
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--variant" && i + 1 < argc) variant = argv[++i];
    else if (a == "--k0" && i + 1 < argc) fixture_k0 = std::string(argv[++i]) != "model";
    else if (a == "--tol" && i + 1 < argc) tol = std::atof(argv[++i]);
    else if (a == "--outcome") outcome_mode = true;
    else if (a == "--law" && i + 1 < argc) fixture_law = std::string(argv[++i]) != "model";
    else if (a == "--linear") corot = false;
    else if (a == "--verbose") verbose = g_verbose = true;
    else if (a == "--feel") feel_mode = true;
    else if (a == "--feel-grid") feel_grid = true;
    else if (a == "--sp" && i + 1 < argc) sp = std::atof(argv[++i]);
    else if (a == "--fragility" && i + 1 < argc) frag = std::atof(argv[++i]);
    else if (a == "--damping" && i + 1 < argc) zeta = std::atof(argv[++i]);
    else if (a == "--amp" && i + 1 < argc) amp = std::atof(argv[++i]);
    else files.push_back(a);
  }
  if (feel_mode || feel_grid) {
    auto run_all = [&](f64 S_, f64 F_, f64 Z_) {
      std::vector<FeelResult> rs;
      for (const std::string& f : files) {
        try {
          rs.push_back(feel(f, S_, F_, Z_, corot));
        } catch (const std::exception& e) {
          FeelResult r;
          r.id = f;
          r.status = std::string("json error: ") + e.what();
          rs.push_back(r);
        }
      }
      return rs;
    };
    if (feel_grid) {
      std::printf("%6s %6s %6s | %8s %8s | %6s %9s %9s | %7s\n", "S_p", "F", "zeta", "settle", "blast", "A fit",
                  "peak err", "quiet err", "score");
      f64 best = 1e30;
      f64 bs = 0, bf = 0, bz = 0, ba = 0;
      for (f64 S_ : {4.0, 6.0, 9.0})
        for (f64 F_ : {0.25, 0.5, 1.0})
          for (f64 Z_ : {0.05, 0.1, 0.2}) {
            const FeelScore sc = score_feel(run_all(S_, F_, Z_), amp);
            std::printf("%6.1f %6.2f %6.2f | %4d/%-3d %4d/%-3d | %6.2f %9.3f %9.3f | %7.3f\n", S_, F_, Z_, sc.settle_agree,
                        sc.settle_n, sc.blast_agree, sc.blast_n, sc.amp, sc.peak_err, sc.quiet_err, sc.score());
            if (sc.score() < best) {
              best = sc.score();
              bs = S_;
              bf = F_;
              bz = Z_;
              ba = sc.amp;
            }
          }
      std::printf("best: S_p %.1f, F %.2f, zeta %.2f, A %.2f (score %.3f)\n", bs, bf, bz, ba, best);
      return 0;
    }
    const std::vector<FeelResult> rs = run_all(sp, frag, zeta);
    const FeelScore sc = score_feel(rs, amp);
    const f64 A = sc.amp;
    std::printf("feel spec: S_p %.1f, F %.2f, zeta %.2f, A %.2f%s\n", sp, frag, zeta, A, amp > 0.0 ? "" : " (fitted)");
    std::printf("%-30s %5s | %-11s %9s %9s | %-11s %9s %9s %11s %11s | %s\n", "scene", "cells", "settle x/s", "sag x",
                "sag s*A", "blast x/s", "rupt x/s", "del x/s", "peak x", "peak s*A", "quiet x/s (s)");
    for (const FeelResult& r : rs) {
      if (r.status != "ok") {
        std::printf("%-30s %s\n", r.id.c_str(), r.status.c_str());
        continue;
      }
      char st[16], bl[16], ru[24], de[24], qu[32];
      std::snprintf(st, sizeof st, "%s/%s", r.x_coll ? "fail" : "stand", r.s_coll ? "fail" : "stand");
      std::snprintf(bl, sizeof bl, "%s/%s", r.blast ? (r.xb_coll ? "fail" : "stand") : "-", r.blast ? (r.sb_coll ? "fail" : "stand") : "-");
      std::snprintf(ru, sizeof ru, "%d/%d", r.xb_rupt, r.sb_rupt);
      std::snprintf(de, sizeof de, "%d/%d", r.xb_del, r.sb_del);
      std::snprintf(qu, sizeof qu, "%.2f/%.2f", r.xb_quiet, r.sb_quiet);
      std::printf("%-30s %5d | %-11s %9.2e %9.2e | %-11s %9s %9s %11.3e %11.3e | %s\n", r.id.c_str(), r.cells, st, r.x_sag,
                  A * r.s_sag, bl, r.blast ? ru : "-", r.blast ? de : "-", r.xb_peak, A * r.sb_peak, r.blast ? qu : "-");
    }
    std::printf("settle classification %d/%d, blast classification %d/%d, peak error %.3f, quiet error %.3f\n",
                sc.settle_agree, sc.settle_n, sc.blast_agree, sc.blast_n, sc.peak_err, sc.quiet_err);
    return 0;
  }
  if (outcome_mode) {
    std::printf("variant=%s outcome (k0 %s, law thresholds %s, %s kinematics)\n", variant.c_str(),
                fixture_k0 ? "fixture" : "model", fixture_law ? "fixture" : "model", corot ? "corotational" : "linear");
    std::printf("%-30s %5s %4s | %5s %5s %4s | %9s %9s | %7s %7s %5s | %7s %7s %8s | %5s %5s | %12s %12s | %s\n", "scene",
                "cells", "gate", "alive", "svx", "sym", "blast rm", "blast fr", "rupt", "svx", "comm", "dmg", "svx",
                "dmg err", "pass", "svx", "minDz(ref)", "minDz(svx)", "status");
    int gated = 0, gated_ok = 0, bad = 0;
    for (const std::string& f : files) {
      OutcomeResult r;
      try {
        r = outcome(f, variant, corot, verbose, fixture_k0, fixture_law);
      } catch (const std::exception& e) {
        r.id = f;
        r.status = std::string("json error: ") + e.what();
      }
      const bool class_ref = r.alive_ref < r.cells, class_svx = r.alive_svx < r.cells;
      const f64 sym = r.cells ? f64(r.symdiff) / r.cells : 0.0;
      const bool agree = r.status == "ok" && class_ref == class_svx && sym <= 0.02 && r.blast_exact;
      std::string st = r.status;
      if (r.status == "ok") st = agree ? "match" : "DIFF";
      if (r.status == "ok" && !r.gate) st += " (oracle Newton unconverged: not gated)";
      char brm[32], bfr[32];
      std::snprintf(brm, sizeof brm, "%d/%d", r.removed_ref, r.removed_svx);
      std::snprintf(bfr, sizeof bfr, "%d/%d", r.frac_ref, r.frac_svx);
      std::printf("%-30s %5d %4s | %5d %5d %4d | %9s %9s | %7d %7d %5d | %7d %7d %8.1e | %5d %5d | %12.5e %12.5e | %s\n",
                  r.id.c_str(), r.cells, r.gate ? "yes" : "no", r.alive_ref, r.alive_svx, r.symdiff,
                  r.blast ? brm : "-", r.blast ? bfr : "-", r.rupt_ref, r.rupt_svx, r.rupt_common, r.dmg_ref, r.dmg_svx,
                  r.dmg_err, r.passes_ref, r.passes_svx, r.dz_ref, r.dz_svx, st.c_str());
      if (r.status != "ok") ++bad;
      if (r.status == "ok" && r.gate) {
        ++gated;
        if (agree) ++gated_ok;
        else ++bad;
      }
    }
    std::printf("gated scenes: %d, matching: %d (%.1f%%)\n", gated, gated_ok, gated ? 100.0 * gated_ok / gated : 100.0);
    return bad ? 1 : 0;
  }
  std::printf("variant=%s k0=%s %s kinematics\n", variant.c_str(), fixture_k0 ? "fixture" : "model",
              corot ? "corotational" : "linear");
  std::printf("%-34s %6s %6s %11s %11s %13s %13s %9s %9s  %s\n", "scene", "cells", "iters", "u_err", "th_err",
              "minDz(ref)", "minDz(svx)", "k/k0 min", "k/k0 max", "status");
  int bad = 0;
  for (const std::string& f : files) {
    SceneResult r;
    try {
      r = compare(f, variant, fixture_k0, corot);
    } catch (const std::exception& e) {
      r.id = f;
      r.status = std::string("json error: ") + e.what();
    }
    std::printf("%-34s %6d %6d %11.3e %11.3e %13.6e %13.6e %9.4f %9.4f  %s\n", r.id.c_str(), r.cells, r.iters, r.u_err,
                r.th_err, r.probe_ref, r.probe_svx, r.k_ratio_min, r.k_ratio_max, r.status.c_str());
    const bool skipped = r.status == "oracle static not converged" || r.status == "no variant";
    if (!skipped && (r.status != "ok" || r.u_err > tol || r.th_err > 10 * tol)) ++bad;
  }
  return bad ? 1 : 0;
}
