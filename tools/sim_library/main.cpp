// svx_sim_library — scenario library: static closure (+DIF) vs implicit dynamics.
//
// Plan Phase 1 gates:
//   * calibrated static + DIF agrees with dynamics on >= 90% of collapse classifications;
//   * with linear kinematics the collapse classification is identical for S in {1, 4, 16}.
// Every scenario is a small structure at the design pitch (h = 0.125 m, game law) that is in
// equilibrium before a sudden event (a carved support or a blast); the fragility sweep puts
// some cases on each side of the stand / collapse boundary. The static path runs the closure
// with the event amplified by the DIF; the dynamic path steps the fine lattice until it sleeps
// (settle projection) or a step limit. Classification: "collapse" when more than 2% of the
// cells surviving the event itself are lost afterwards.
//
// usage: svx_sim_library [--dif LIST] [--compliance S] [--steps N] [--linear] [--threads T]
//                        [--sinv] [--only NAME] [--verbose]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "svx/base/parallel.hpp"
#include "svx/sim/blast.hpp"
#include "svx/sim/dynamics.hpp"
#include "svx/sim/statics.hpp"

using namespace svx;

namespace {

struct Event {
  std::vector<std::array<i32, 3>> carve;  // cells removed (lattice coordinates)
  bool blast = false;
  BlastParams bp;
};

struct Scenario {
  std::string name;
  f64 fragility = 1.0;
  std::vector<CellIn> cells;
  Event ev;
};

void add_box(std::vector<CellIn>& cells, int x0, int x1, int y0, int y1, int z0, int z1, bool anchored = false) {
  for (int x = x0; x < x1; ++x)
    for (int y = y0; y < y1; ++y)
      for (int z = z0; z < z1; ++z) {
        CellIn c;
        c.p = {x, y, z};
        c.mat = MaterialId::Concrete;
        c.anchored = anchored;
        cells.push_back(c);
      }
}

void carve_box(Event& ev, int x0, int x1, int y0, int y1, int z0, int z1) {
  for (int x = x0; x < x1; ++x)
    for (int y = y0; y < y1; ++y)
      for (int z = z0; z < z1; ++z) ev.carve.push_back({x, y, z});
}

constexpr f64 kH = 0.125;

std::vector<Scenario> library() {
  std::vector<Scenario> out;
  const f64 frag[] = {0.02, 0.04, 0.08, 0.16, 0.32, 0.64, 1.0};
  for (f64 F : frag) {
    // A: slab on four corner columns; one column is cut
    for (int t : {1, 2}) {
      Scenario s;
      s.name = "table_t" + std::to_string(t);
      s.fragility = F;
      for (int cx : {0, 14})
        for (int cy : {0, 14}) {
          add_box(s.cells, cx, cx + 2, cy, cy + 2, 0, 1, true);
          add_box(s.cells, cx, cx + 2, cy, cy + 2, 1, 9);
        }
      add_box(s.cells, 0, 16, 0, 16, 9, 9 + t);
      carve_box(s.ev, 0, 2, 0, 2, 4, 6);
      out.push_back(std::move(s));
    }
    // B: portal frame with overhangs; one column is cut
    {
      Scenario s;
      s.name = "portal";
      s.fragility = F;
      for (int cx : {4, 16}) {
        add_box(s.cells, cx, cx + 2, 0, 2, 0, 1, true);
        add_box(s.cells, cx, cx + 2, 0, 2, 1, 11);
      }
      add_box(s.cells, 0, 22, 0, 2, 11, 13);
      carve_box(s.ev, 16, 18, 0, 2, 5, 7);
      out.push_back(std::move(s));
    }
    // C: propped cantilever (wall-anchored slab, prop under the tip); the prop is removed
    {
      Scenario s;
      s.name = "propped_cantilever";
      s.fragility = F;
      add_box(s.cells, -2, 0, 0, 4, 0, 9, true);   // anchored wall
      add_box(s.cells, 0, 16, 0, 4, 7, 9);         // slab (2 cells thick)
      add_box(s.cells, 14, 16, 1, 3, 0, 1, true);  // prop footing
      add_box(s.cells, 14, 16, 1, 3, 1, 7);        // prop
      carve_box(s.ev, 14, 16, 1, 3, 1, 7);
      out.push_back(std::move(s));
    }
    // D: wall with a door opening; a blast at the lintel
    {
      Scenario s;
      s.name = "wall_lintel";
      s.fragility = F;
      add_box(s.cells, 0, 24, 0, 2, 0, 1, true);
      for (int x = 0; x < 24; ++x)
        for (int z = 1; z < 17; ++z) {
          if (x >= 9 && x < 15 && z < 10) continue;  // opening
          add_box(s.cells, x, x + 1, 0, 2, z, z + 1);
        }
      s.ev.blast = true;
      s.ev.bp.center = {15.5 * kH, 0.5 * kH, 10.5 * kH};
      s.ev.bp.radius = 1.6 * kH;
      out.push_back(std::move(s));
    }
  }
  return out;
}

struct Outcome {
  i32 before = 0;    // cells alive after the event itself (carve / blast core)
  i32 after = 0;     // cells alive at the end
  bool collapse = false;
  f64 ms = 0.0;
  int steps = 0;
  bool slept = false;
};

std::vector<i32> event_cells(const Lattice& L, const Event& ev) {
  std::vector<i32> out;
  for (const auto& p : ev.carve)
    for (i32 i = 0; i < L.n; ++i)
      if (L.p[i] == p) {
        out.push_back(i);
        break;
      }
  return out;
}

Lattice build(const Scenario& sc, bool* pre_ok, std::vector<f64>* u0, bool corot, f64 S) {
  LatticeOptions o;
  o.h = kH;
  o.law.game = true;
  o.law.fragility = sc.fragility;
  Lattice L = build_lattice(sc.cells, o);
  L.kscale = 1.0 / S;
  StaticsOptions so;
  so.corot = corot;
  const auto f = gravity_vector(L, so.g);
  const ClosureResult cr = solve_to_closure(L, *u0, f, so);
  *pre_ok = cr.converged && cr.ruptured.empty() && cr.deleted.empty();
  return L;
}

i32 alive(const Lattice& L) { return L.num_alive(); }

// The pre-event state three ways: plain secant iteration, Anderson(3), and gravity ramped up in
// ten converged increments (the path the structure was built on).
void pre_report(const Scenario& sc, bool corot, f64 S) {
  for (int mode = 0; mode < 3; ++mode) {
    LatticeOptions o;
    o.h = kH;
    o.law.game = true;
    o.law.fragility = sc.fragility;
    Lattice L = build_lattice(sc.cells, o);
    L.kscale = 1.0 / S;
    StaticsOptions so;
    so.corot = corot;
    so.anderson = mode == 1 ? 3 : 0;
    const auto f = gravity_vector(L, so.g);
    std::vector<f64> u;
    const int ramp = mode == 2 ? 10 : 1;
    int iters = 0, passes = 0;
    size_t ruptured = 0, deleted = 0;
    bool conv = true, eqs = true;
    for (int k = 1; k <= ramp; ++k) {
      std::vector<f64> fk = f;
      for (auto& x : fk) x *= f64(k) / ramp;
      const ClosureResult cr = solve_to_closure(L, u, fk, so);
      for (const auto& pl : cr.log) iters += pl.eq.iters;
      passes += cr.passes;
      ruptured += cr.ruptured.size();
      deleted += cr.deleted.size();
      conv = conv && cr.converged;
      eqs = eqs && cr.all_equilibria_converged;
    }
    static const char* names[3] = {"secant", "anderson3", "ramp10"};
    std::printf("%-20s %6.3f %-9s | converged %d (equilibria %d) passes %3d iterations %5d ruptured %4zu deleted %4zu\n",
                sc.name.c_str(), sc.fragility, names[mode], conv ? 1 : 0, eqs ? 1 : 0, passes, iters, ruptured, deleted);
  }
}

Outcome run_static(const Scenario& sc, f64 dif, bool corot, f64 S) {
  bool ok = false;
  std::vector<f64> u;
  Lattice L = build(sc, &ok, &u, corot, S);
  Outcome o;
  if (!ok) return o;
  const auto t0 = std::chrono::steady_clock::now();
  if (sc.ev.blast) {
    apply_blast(L, u.data(), sc.ev.bp);
  } else {
    for (i32 c : event_cells(L, sc.ev)) L.remove_cell(c);
  }
  o.before = alive(L);
  StaticsOptions so;
  so.corot = corot;
  so.dif = dif;
  so.event = true;
  const auto f = gravity_vector(L, so.g);
  solve_to_closure(L, u, f, so);
  o.after = alive(L);
  o.ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
  o.collapse = o.before - o.after > 0.02 * o.before;
  return o;
}

Outcome run_dynamic(const Scenario& sc, bool corot, f64 S, int max_steps, bool verbose) {
  bool ok = false;
  std::vector<f64> u;
  Lattice L = build(sc, &ok, &u, corot, S);
  Outcome o;
  if (!ok) return o;
  const auto t0 = std::chrono::steady_clock::now();
  DynamicsOptions dop;
  dop.corot = corot;
  dop.compliance = S;
  dop.rayleigh_alpha = 0.5;
  dop.statics.corot = corot;
  Dynamics dyn;
  dyn.init(L, dop);
  dyn.set_state(u);
  if (sc.ev.blast) dyn.blast(sc.ev.bp);
  else dyn.carve(event_cells(L, sc.ev));
  o.before = alive(L) + static_cast<i32>(dyn.detached_cells());
  for (int s = 0; s < max_steps; ++s) {
    const StepStats st = dyn.step();
    ++o.steps;
    if (verbose && (st.ruptured || st.detached_cells || s % 20 == 0 || st.settled))
      std::printf("      step %4d pcg %3d newton %d rupt %3d det %4d vmax %.3e | ms %.2f solve %.2f mg %.2f law %.2f "
                  "topo %.2f settle %.2f%s%s%s\n",
                  s, st.pcg, st.newton, st.ruptured, st.detached_cells, st.max_speed, st.ms_total, st.ms_solve,
                  st.ms_mg, st.ms_law, st.ms_topo, st.ms_settle, st.rebuilt ? " R" : "", st.settled ? " settled" : "",
                  st.asleep ? " asleep" : "");
    if (dyn.asleep()) {
      o.slept = true;
      break;
    }
  }
  o.after = alive(L);
  o.ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
  o.collapse = o.before - o.after > 0.02 * o.before;
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<f64> difs = {1.0, 1.25, 1.5, 1.75, 2.0};
  f64 S = 4.0;
  int steps = 900;
  bool corot = true, sinv = false, verbose = false, pre = false;
  std::string only;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--dif" && i + 1 < argc) {
      difs.clear();
      std::string l = argv[++i];
      size_t p = 0;
      while (p < l.size()) {
        const size_t q = l.find(',', p);
        difs.push_back(std::atof(l.substr(p, q == std::string::npos ? std::string::npos : q - p).c_str()));
        if (q == std::string::npos) break;
        p = q + 1;
      }
    } else if (a == "--compliance" && i + 1 < argc) S = std::atof(argv[++i]);
    else if (a == "--steps" && i + 1 < argc) steps = std::atoi(argv[++i]);
    else if (a == "--linear") corot = false;
    else if (a == "--threads" && i + 1 < argc) set_num_threads(std::atoi(argv[++i]));
    else if (a == "--sinv") sinv = true;
    else if (a == "--only" && i + 1 < argc) only = argv[++i];
    else if (a == "--verbose") verbose = true;
    else if (a == "--pre") pre = true;
  }
  const auto lib = library();
  if (pre) {
    for (const Scenario& sc : library())
      if (only.empty() || sc.name == only) pre_report(sc, corot, S);
    return 0;
  }
  std::printf("scenarios: %zu, compliance S=%g, %s kinematics, dynamics <= %d steps (dt 1/60)\n", lib.size(), S,
              corot ? "corotational" : "linear", steps);
  if (sinv) {
    // collapse classification of the dynamics for S in {1, 4, 16}
    int same = 0, total = 0;
    for (const Scenario& sc : lib) {
      if (!only.empty() && sc.name != only) continue;
      Outcome r[3];
      const f64 Ss[3] = {1.0, 4.0, 16.0};
      for (int k = 0; k < 3; ++k) r[k] = run_dynamic(sc, corot, Ss[k], steps, false);
      if (r[0].before == 0) continue;
      ++total;
      const bool eq = r[0].collapse == r[1].collapse && r[0].collapse == r[2].collapse;
      same += eq ? 1 : 0;
      std::printf("%-20s F=%.3f  lost S1 %5d  S4 %5d  S16 %5d  %s\n", sc.name.c_str(), sc.fragility,
                  r[0].before - r[0].after, r[1].before - r[1].after, r[2].before - r[2].after,
                  eq ? "same" : "DIFFERENT");
    }
    std::printf("S invariance of the classification: %d / %d\n", same, total);
    return same == total ? 0 : 1;
  }
  std::vector<int> agree(difs.size(), 0);
  int total = 0;
  std::printf("%-20s %6s | %6s %6s %5s %8s %6s |", "scenario", "F", "cells", "lost", "dyn", "dyn ms", "steps");
  for (f64 d : difs) std::printf(" DIF %4.2f", d);
  std::printf("\n");
  for (const Scenario& sc : lib) {
    if (!only.empty() && sc.name != only) continue;
    const Outcome dyn = run_dynamic(sc, corot, S, steps, verbose);
    if (dyn.before == 0) {
      std::printf("%-20s %6.3f | fails before the event: skipped\n", sc.name.c_str(), sc.fragility);
      continue;
    }
    ++total;
    std::printf("%-20s %6.3f | %6d %6d %5s %8.0f %5d%s |", sc.name.c_str(), sc.fragility, dyn.before,
                dyn.before - dyn.after, dyn.collapse ? "COLL" : "stand", dyn.ms, dyn.steps, dyn.slept ? "" : "+");
    for (size_t k = 0; k < difs.size(); ++k) {
      const Outcome st = run_static(sc, difs[k], corot, S);
      const bool ok = st.collapse == dyn.collapse;
      agree[k] += ok ? 1 : 0;
      std::printf(" %5d%s%s", st.before - st.after, st.collapse ? "C" : "s", ok ? " " : "!");
    }
    std::printf("\n");
    std::fflush(stdout);
  }
  std::printf("agreement with dynamics:");
  size_t best = 0;
  for (size_t k = 0; k < difs.size(); ++k) {
    std::printf("  DIF %.2f: %d/%d (%.0f%%)", difs[k], agree[k], total, total ? 100.0 * agree[k] / total : 0.0);
    if (agree[k] > agree[best]) best = k;
  }
  std::printf("\ncalibrated DIF = %.2f (%.0f%%)\n", difs[best], total ? 100.0 * agree[best] / total : 0.0);
  return (total && agree[best] * 10 >= total * 9) ? 0 : 1;
}
