// svx_composite_study — accuracy of the telescoping composite vs the full fine solve (plan
// §A5 spike, now in C++ with the baseline + event-delta formulation of §B2).
//
// For each scenario: u0 = fine static equilibrium before the event; the event (removed
// cells) releases r = f - K_post u0 at the surviving neighbours; the fine truth solves
// K_post D = r, the composite solves A_c d = P^T r and prolongs D ~ P d. Reported: node counts
// per level, near-field peak demand ratio (composite / truth) for the extreme-fibre tension,
// compression and shear measures of the bonds within R of the event, the load-path error
// (storey-1 column axial loads for the frame scenario) and solve times.
//
// usage: svx_composite_study [--scenario frame|rooms|all] [--threads N] [--nx BAYS]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "svx/base/parallel.hpp"
#include "svx/bubble/composite.hpp"
#include "svx/sim/statics.hpp"

using namespace svx;

namespace {

using Clock = std::chrono::steady_clock;
f64 ms_since(Clock::time_point t) { return std::chrono::duration<f64, std::milli>(Clock::now() - t).count(); }

struct Grid {
  int X, Y, Z;
  std::vector<u8> solid, anchor;
  u8& s(int x, int y, int z) { return solid[(size_t(x) * Y + y) * Z + z]; }
  u8& a(int x, int y, int z) { return anchor[(size_t(x) * Y + y) * Z + z]; }
};

Grid make_grid(int X, int Y, int Z) {
  Grid g{X, Y, Z, std::vector<u8>(size_t(X) * Y * Z, 0), std::vector<u8>(size_t(X) * Y * Z, 0)};
  return g;
}

void fill(Grid& g, int x0, int x1, int y0, int y1, int z0, int z1, u8 v) {
  for (int x = std::max(0, x0); x < std::min(g.X, x1); ++x)
    for (int y = std::max(0, y0); y < std::min(g.Y, y1); ++y)
      for (int z = std::max(0, z0); z < std::min(g.Z, z1); ++z) g.s(x, y, z) = v;
}

struct ColRec {
  int s, ix, iy, x0, y0, z0;
};

// research/mlexp/structures.py building()
Grid building(int nxb, int nyb, int stories, std::vector<ColRec>* cols, int bay = 12, int story_h = 10, int slab_t = 2,
              int col = 2) {
  const int X = nxb * bay + col + 2, Y = nyb * bay + col + 2, Z = stories * (story_h + slab_t) + 1;
  Grid g = make_grid(X, Y, Z);
  for (int x = 0; x < X; ++x)
    for (int y = 0; y < Y; ++y) {
      g.s(x, y, 0) = 1;
      g.a(x, y, 0) = 1;
    }
  for (int s = 0; s < stories; ++s) {
    const int z0 = 1 + s * (story_h + slab_t);
    for (int ix = 0; ix <= nxb; ++ix)
      for (int iy = 0; iy <= nyb; ++iy) {
        const int x0 = 1 + ix * bay, y0 = 1 + iy * bay;
        fill(g, x0, x0 + col, y0, y0 + col, z0, z0 + story_h, 1);
        cols->push_back({s, ix, iy, x0, y0, z0});
      }
    const int zs = z0 + story_h;
    fill(g, 1, 1 + nxb * bay + col, 1, 1 + nyb * bay + col, zs, zs + slab_t, 1);
    if (s == stories - 1) {
      fill(g, 1, 1 + nxb * bay + col, 1, 2, z0, z0 + story_h, 1);
      fill(g, 1 + bay / 2, 1 + bay / 2 + 3, 1, 2, z0, z0 + 6, 0);
    }
  }
  return g;
}

// research/mlexp/exp_rooms.py rooms()
Grid rooms(int nx = 3, int ny = 2, int rx = 28, int ry = 20, int wall = 4, int height = 20, int slab = 3) {
  const int X = nx * (rx + wall) + wall, Y = ny * (ry + wall) + wall, Z = 1 + height + slab;
  Grid g = make_grid(X, Y, Z);
  for (int x = 0; x < X; ++x)
    for (int y = 0; y < Y; ++y) {
      g.s(x, y, 0) = 1;
      g.a(x, y, 0) = 1;
    }
  for (int i = 0; i <= nx; ++i) fill(g, i * (rx + wall), i * (rx + wall) + wall, 0, Y, 1, 1 + height, 1);
  for (int j = 0; j <= ny; ++j) fill(g, 0, X, j * (ry + wall), j * (ry + wall) + wall, 1, 1 + height, 1);
  for (int i = 1; i < nx; ++i) {
    const int x0 = i * (rx + wall);
    for (int j = 0; j < ny; ++j) {
      const int yc = j * (ry + wall) + wall + ry / 2;
      fill(g, x0, x0 + wall, yc - 3, yc + 3, 1, 13, 0);
    }
  }
  fill(g, 0, X, 0, Y, 1 + height, 1 + height + slab, 1);
  return g;
}

Lattice to_lattice(Grid& g, f64 h) {
  std::vector<CellIn> cells;
  for (int x = 0; x < g.X; ++x)
    for (int y = 0; y < g.Y; ++y)
      for (int z = 0; z < g.Z; ++z)
        if (g.s(x, y, z)) {
          CellIn c;
          c.p = {x, y, z};
          c.mat = MaterialId::Concrete;
          c.anchored = g.a(x, y, z);
          cells.push_back(c);
        }
  LatticeOptions o;
  o.h = h;
  return build_lattice(cells, o);
}

// Spike stress measures on a full cell (lattice.py stress_measure): extreme-fibre tension,
// compression and shear stress of each bond force (svx order N, V1, V2, T, M1, M2).
void stress(const Vec6& F, f64 h, f64 out[3]) {
  const f64 A = h * h, I = h * h * h * h / 12.0;
  const f64 N = F[0], V = std::hypot(F[1], F[2]), T = std::abs(F[3]), M = std::abs(F[4]) + std::abs(F[5]);
  out[0] = N / A + M * (h / 2) / I;
  out[1] = -N / A + M * (h / 2) / I;
  out[2] = V / A + T / (0.208 * h * h * h);
}

struct Truth {
  std::vector<f64> u0, D;  // baseline and event delta (fine)
};

std::vector<f64> fine_solve(const Lattice& L, const std::vector<f64>& f, int* iters) {
  Multigrid mg;
  mg.build(L, MGOptions{});
  std::vector<f64> u(f.size(), 0.0);
  const PcgStats ps = pcg_solve(mg, L.n, f.data(), u.data(), 1e-10, 2000, false);
  if (iters) *iters = ps.iters;
  return u;
}

struct Result {
  i32 nodes = 0;
  std::vector<i32> per_level;
  f64 ratio[3] = {0, 0, 0};
  f64 err[3] = {0, 0, 0};
  f64 load_err = 0.0;
  f64 ms_build = 0.0, ms_solve = 0.0;
  int iters = 0;
};

// Near-field bonds: within R (cells) of the centre, both cells alive.
Result evaluate(const Lattice& L, const Composite& C, const std::vector<f64>& u_ref, const std::vector<f64>& u0,
                const std::vector<f64>& Dc, const std::array<f64, 3>& c, f64 R) {
  Result r;
  f64 pr[3] = {0, 0, 0}, px[3] = {0, 0, 0}, pe[3] = {0, 0, 0};
  for (int a = 0; a < 3; ++a)
    for (i32 i = 0; i < L.n; ++i) {
      const i32 j = L.nbr[a][i];
      if (j < 0) continue;
      f64 d2 = 0.0;
      for (int q = 0; q < 3; ++q) {
        const f64 pq = L.h * (L.p[i][q] + (q == a ? 0.5 : 0.0)) - c[q];
        d2 += pq * pq;
      }
      if (std::sqrt(d2) > R * L.h) continue;
      const Vec6 Fr = bond_force(L, a, i, u_ref.data());
      // baseline forces are exact; the composite delta acts on the scaled bond
      const Vec6 F0 = bond_force(L, a, i, u0.data());
      const Vec6 Fd = bond_force(L, a, i, Dc.data());
      const f64 s = C.scale[a].empty() ? 1.0 : C.scale[a][i];
      Vec6 Fx;
      for (int q = 0; q < 6; ++q) Fx[q] = F0[q] + s * Fd[q];
      f64 sr[3], sx[3];
      stress(Fr, L.h, sr);
      stress(Fx, L.h, sx);
      for (int q = 0; q < 3; ++q) {
        pr[q] = std::max(pr[q], std::abs(sr[q]));
        px[q] = std::max(px[q], std::abs(sx[q]));
        pe[q] = std::max(pe[q], std::abs(sx[q] - sr[q]));
      }
    }
  for (int q = 0; q < 3; ++q) {
    r.ratio[q] = pr[q] > 0 ? px[q] / pr[q] : 1.0;
    r.err[q] = pr[q] > 0 ? pe[q] / pr[q] : 0.0;
  }
  return r;
}

// Composite solve of the event residual on the post-event lattice.
Result composite_run(const Lattice& L, const std::vector<f64>& r_fine, const std::array<f64, 3>& c,
                     const CompositeOptions& co, std::vector<f64>* D_out, Composite* C_out) {
  Result res;
  const auto t0 = Clock::now();
  const std::array<f64, 3> centers[1] = {c};
  Composite C = build_composite(L, centers, co);
  Multigrid mg;
  MGOptions mo;
  build_composite_mg(C, mg, mo);
  res.ms_build = ms_since(t0);
  if (std::getenv("SVX_MG_LEVELS")) {
    std::printf("    mg levels:");
    for (i32 k : mg.level_sizes()) std::printf(" %d", k);
    std::printf("\n");
  }
  std::vector<f64> rc(6 * size_t(C.n)), dc(6 * size_t(C.n), 0.0);
  composite_restrict(L, C, r_fine.data(), rc.data());
  const auto t1 = Clock::now();
  const PcgStats ps = pcg_solve(mg, C.n, rc.data(), dc.data(), 1e-10, 2000, false);
  res.ms_solve = ms_since(t1);
  res.iters = ps.iters;
  D_out->assign(6 * size_t(L.n), 0.0);
  composite_prolong(L, C, dc.data(), D_out->data());
  res.nodes = C.n;
  res.per_level.assign(co.max_level + 1, 0);
  for (i32 k = 0; k < C.n; ++k) res.per_level[C.level[k]]++;
  *C_out = std::move(C);
  return res;
}

void print_result(const char* tag, const CompositeOptions& co, const Result& r, const Result& m) {
  std::printf("  %-6s R0=%4.0f g=%.1f L=%d nodes=%6d [", tag, co.R0, co.grading, co.max_level, r.nodes);
  for (size_t k = 0; k < r.per_level.size(); ++k) std::printf("%s%d", k ? " " : "", r.per_level[k]);
  std::printf("] peak(sig_t,sig_c,tau) got/ref = %.3f %.3f %.3f  err/peak = %.3f %.3f %.3f", m.ratio[0], m.ratio[1],
              m.ratio[2], m.err[0], m.err[1], m.err[2]);
  if (m.load_err >= 0.0) std::printf("  load-path err %.3f", m.load_err);
  std::printf("  (build %.0f ms, solve %.0f ms, %d its)\n", r.ms_build, r.ms_solve, r.iters);
}

void run_frame(int nxb) {
  std::vector<ColRec> cols;
  Grid g = building(nxb, nxb, 2, &cols);
  const f64 h = 1.0;
  Lattice L = to_lattice(g, h);
  ColRec target{};
  for (const ColRec& c : cols)
    if (c.s == 0 && c.ix == 1 && c.iy == 1) target = c;
  const std::array<f64, 3> center{h * (target.x0 + 0.5), h * (target.y0 + 0.5), h * (target.z0 + 10 - 1)};
  std::printf("\nFRAME %dx%d bays, 2 storeys: cells=%d, event: remove storey-1 interior column (%d,%d)\n", nxb, nxb, L.n,
              target.ix, target.iy);
  const auto f = gravity_vector(L, 9.81);
  int it0 = 0;
  const auto t0 = Clock::now();
  const std::vector<f64> u0 = fine_solve(L, f, &it0);
  std::printf("  baseline fine solve %.0f ms (%d its)\n", ms_since(t0), it0);
  std::vector<u8> removed(L.n, 0);
  std::vector<i32> rm;
  for (i32 i = 0; i < L.n; ++i)
    if (L.p[i][0] >= target.x0 && L.p[i][0] < target.x0 + 2 && L.p[i][1] >= target.y0 && L.p[i][1] < target.y0 + 2 &&
        L.p[i][2] >= target.z0 && L.p[i][2] < target.z0 + 10) {
      removed[i] = 1;
      rm.push_back(i);
    }
  std::vector<f64> r(6 * size_t(L.n));
  released_bond_residual(L, removed, u0.data(), r.data());
  for (i32 c : rm) L.remove_cell(c);
  const auto t1 = Clock::now();
  int it1 = 0;
  const std::vector<f64> D = fine_solve(L, r, &it1);
  std::printf("  truth delta fine solve %.0f ms (%d its)\n", ms_since(t1), it1);
  std::vector<f64> uref(u0.size());
  for (size_t k = 0; k < u0.size(); ++k) uref[k] = u0[k] + D[k];
  // storey-1 column loads (axial force of the bonds from the ground into each column)
  auto column_loads = [&](const std::vector<f64>& u, const std::vector<f64>* Dc, const Composite* C) {
    std::vector<f64> out;
    for (const ColRec& c : cols) {
      if (c.s != 0) continue;
      f64 s = 0.0;
      for (i32 i = 0; i < L.n; ++i) {
        if (L.p[i][2] != 0 || L.p[i][0] < c.x0 || L.p[i][0] >= c.x0 + 2 || L.p[i][1] < c.y0 || L.p[i][1] >= c.y0 + 2)
          continue;
        if (L.nbr[2][i] < 0) continue;
        s += bond_force(L, 2, i, u.data())[0];
        if (Dc) s += (C && !C->scale[2].empty() ? C->scale[2][i] : 1.0) * bond_force(L, 2, i, Dc->data())[0];
      }
      out.push_back(s);
    }
    return out;
  };
  const auto cl_ref = column_loads(uref, nullptr, nullptr);
  f64 clmax = 0.0;
  for (f64 v : cl_ref) clmax = std::max(clmax, std::abs(v));
  auto co_of = [](f64 R0, f64 g, int L) {
    CompositeOptions c;
    c.R0 = R0;
    c.grading = g;
    c.max_level = L;
    return c;
  };
  const CompositeOptions sweep[] = {co_of(8, 2.0, 2), co_of(8, 2.0, 3), co_of(12, 2.0, 3),
                                    co_of(16, 2.0, 3), co_of(8, 3.0, 3), co_of(12, 3.0, 2)};
  for (bool scaling : {false, true}) {
    for (CompositeOptions co : sweep) {
      co.scaling = scaling;
      co.torsion = scaling;
      std::vector<f64> Dc;
      Composite C;
      Result r0 = composite_run(L, r, center, co, &Dc, &C);
      Result m = evaluate(L, C, uref, u0, Dc, center, co.R0);
      const auto cl = column_loads(u0, &Dc, &C);
      f64 e = 0.0;
      for (size_t k = 0; k < cl.size(); ++k) e = std::max(e, std::abs(cl[k] - cl_ref[k]));
      m.load_err = e / clmax;
      print_result(scaling ? "COMP" : "GAL", co, r0, m);
    }
  }
}

void run_rooms() {
  Grid g0 = rooms();
  struct Ev {
    const char* name;
    std::array<f64, 3> c;
    f64 r;
  };
  const Ev evs[] = {{"rocket r=6 at base of interior wall", {34, 12, 3}, 6},
                    {"rocket r=6 mid-ceiling", {46, 34, 22}, 6},
                    {"rocket r=8 at wall/ceiling corner", {34, 26, 20}, 8}};
  for (const Ev& ev : evs) {
    Grid g = g0;
    Lattice L = to_lattice(g, 1.0);
    const auto f = gravity_vector(L, 9.81);
    int it0 = 0;
    const std::vector<f64> u0 = fine_solve(L, f, &it0);
    std::vector<u8> removed(L.n, 0);
    std::vector<i32> rm;
    for (i32 i = 0; i < L.n; ++i) {
      if (L.anchored[i]) continue;
      const f64 dx = L.p[i][0] - ev.c[0], dy = L.p[i][1] - ev.c[1], dz = L.p[i][2] - ev.c[2];
      if (std::sqrt(dx * dx + dy * dy + dz * dz) <= ev.r) {
        removed[i] = 1;
        rm.push_back(i);
      }
    }
    std::vector<f64> r(6 * size_t(L.n));
    released_bond_residual(L, removed, u0.data(), r.data());
    for (i32 c : rm) L.remove_cell(c);
    for (const auto& isl : unsupported_components(L))
      for (i32 c : isl) L.remove_cell(c);
    int it1 = 0;
    const auto t1 = Clock::now();
    const std::vector<f64> D = fine_solve(L, r, &it1);
    std::printf("\nROOMS %s: cells=%d (truth delta %.0f ms, %d its)\n", ev.name, L.num_alive(), ms_since(t1), it1);
    std::vector<f64> uref(u0.size());
    for (size_t k = 0; k < u0.size(); ++k) uref[k] = u0[k] + D[k];
    auto co_of = [](f64 R0, f64 g, int L) {
      CompositeOptions c;
      c.R0 = R0;
      c.grading = g;
      c.max_level = L;
      return c;
    };
    const CompositeOptions sweep[] = {co_of(12, 2.0, 4), co_of(16, 2.0, 4), co_of(16, 3.0, 4), co_of(24, 2.0, 4)};
    for (const CompositeOptions& co : sweep) {
      std::vector<f64> Dc;
      Composite C;
      Result r0 = composite_run(L, r, ev.c, co, &Dc, &C);
      Result m = evaluate(L, C, uref, u0, Dc, ev.c, 2 * ev.r);
      m.load_err = -1.0;
      print_result("COMP", co, r0, m);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string scen = "all";
  int nxb = 3;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--scenario" && i + 1 < argc) scen = argv[++i];
    else if (a == "--threads" && i + 1 < argc) set_num_threads(std::atoi(argv[++i]));
    else if (a == "--nx" && i + 1 < argc) nxb = std::atoi(argv[++i]);
  }
  if (scen == "frame" || scen == "all") run_frame(nxb);
  if (scen == "rooms" || scen == "all") run_rooms();
  return 0;
}
