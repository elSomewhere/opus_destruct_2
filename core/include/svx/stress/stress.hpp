// structvox — stress of a fragment graph (docs/V2_DESIGN.md §2-3).
//
// Nodes are rigid fragments (6 DOFs: u, theta about the node centre c), bonds are the interfaces
// between them (rigid-body-spring model: each side's half-length in series; normal, two shear,
// torsion and two bending springs at the interface centroid). K u = f is solved for the
// equilibrium under the node loads f (gravity, contact forces, inertia) with fixed supports
// (static structures) or one pinned node (free bodies, whose load is self-equilibrated); the
// bond forces follow from u. A bond fails when its fibre stresses N/A +- M c/I exceed the
// section strengths of its materials: tension (direct + flexural), crushing, or Mohr-Coulomb
// shear.
#pragma once

#include <vector>

#include "svx/base/vec.hpp"
#include "svx/material/material.hpp"
#include "svx/solve/amg.hpp"

namespace svx {

struct SNode {
  V3 c;                   // centre (the solve's frame)
  f64 mass = 0.0;
  bool fixed = false;     // pinned: no DOFs (a free body's reference node)
  bool gone = false;      // retired (detached, carved away): an identity row, no bonds
};

struct SBond {
  i32 a = -1, b = -1;     // nodes; b < 0: a fixed support (anchored voxels, a frozen frontier)
  bool broken = false;    // severed (the topology: connectivity reads this)
  bool in_k = false;      // (its stiffness is in the assembled K; kept by StressProblem)
  MaterialId ma = MaterialId::Concrete, mb = MaterialId::Concrete;
  // Local frame: n from a towards b (supports: out of a into the support), t1, t2 in the section
  // plane. The section is the shared voxel faces projected onto the plane normal to n.
  V3 n{0, 0, 1}, t1{1, 0, 0}, t2{0, 1, 0};
  f64 area = 0.0;         // m^2 (projected)
  V3 p;                   // section centroid (the solve's frame)
  f64 s1 = 0.0, s2 = 0.0; // second moments of area about p: s_k = integral of y_k^2 dA, y_k along t_k
  f64 c1 = 0.0, c2 = 0.0; // extreme fibre distances along t1, t2
  f64 rmax = 0.0;         // largest distance of the section from p (torsion)
  f64 la = 0.0, lb = 0.0; // lengths of the two sides along n (node to section, section to node)
  f64 strength = 1.0;     // design strength multiplier
  // Section strengths (Pa) from the section's faces: each face as strong as the weaker material
  // of the two voxels it joins, times their condition; the mean over the faces (a concrete
  // section with a reinforcing bar in it is stronger in tension by the bar's share). Not
  // sectioned: the weaker of ma and mb.
  bool sectioned = false;
  f32 ft = 0, fb = 0, fc = 0, coh = 0, mu = 0;
  i32 faces = 0;          // voxel faces in the section
  i32 tag = -1;           // owner's id (e.g. its face list)
};

// Section strengths of a bond (the weaker side governs each).
struct BondStrength {
  f64 ft, fb, fc, coh, mu;
};
BondStrength bond_strength(const SBond& b, f64 fragility);

// Bond loads in bond terms: N (tension > 0), shear V along t1, t2, torsion T, bending (M1 about t1,
// M2 about t2).
struct BondLoad {
  f64 N = 0, V1 = 0, V2 = 0, T = 0, M1 = 0, M2 = 0;
};
enum class FailMode : u8 { None = 0, Tension, Crush, Shear };
// Utilization (1 = at strength) and the governing mode.
f64 bond_utilization(const SBond& b, const BondLoad& L, f64 fragility, FailMode* mode = nullptr);

struct StressOptions {
  f64 rtol = 2e-3;        // relative residual of a converged solve
  AmgOptions amg{};
  i32 amg_min_nodes = 160;  // smaller graphs: block-Jacobi preconditioning (no hierarchy to build)
};

class StressProblem {
 public:
  std::vector<SNode> nodes;
  std::vector<SBond> bonds;

  // Assembles K over the intact bonds and builds the preconditioner. Free nodes need a path to
  // a support or a pinned node (callers split graphs into components first).
  bool assemble(const StressOptions& opt = {});
  bool assembled() const { return assembled_; }
  void invalidate() {
    assembled_ = false;
    run_.active = false;
  }
  // Incremental changes that keep the preconditioner (a stale one still converges; callers
  // rebuild it with assemble() when solves slow down or much has changed):
  //   remove_bond: the bond breaks, its stiffness leaves K in place;
  //   retire_nodes: the nodes leave (with all their bonds), their rows become identities (a
  //   caller may restore `broken = false` on bonds between retired nodes to keep their topology:
  //   they stay out of K);
  //   reassemble: K again from nodes and bonds (nodes appended since the last assemble() are
  //   preconditioned by their diagonal blocks).
  void remove_bond(i32 b);
  void retire_nodes(const std::vector<i32>& list);
  bool reassemble();
  // Warm start of nodes appended since the last assemble(): the mean of their bonded, older
  // neighbours' displacement (a few passes), so a patch does not start from a torn state.
  void extend_warm_start(std::vector<f64>& u, i32 first_new_node) const;
  i32 appended() const { return nfree_ - pc_n_; }   // free nodes the preconditioner does not cover
  i32 free_nodes() const { return nfree_; }
  i64 matrix_blocks() const { return K_.blocks(); }
  i64 memory_bytes() const;
  i64 work_per_iteration() const { return 2 * K_.blocks() + amg_.work_per_apply(); }

  // f, u: 6 per node (u: warm start in, solution out; fixed nodes stay 0). maxit < 0: no cap.
  PcgResult solve(const std::vector<f64>& f, std::vector<f64>& u, f64 rtol, int maxit, bool warm);
  // The same solve spread over several calls (converging over ticks): begin() takes the load
  // and the warm start, iterate() continues the same conjugate-gradient sequence, and u() is the
  // current iterate. A new begin() (new loads, a changed matrix) restarts it.
  void begin(const std::vector<f64>& f, const std::vector<f64>& u0);
  PcgResult iterate(int maxit, f64 rtol);
  bool running() const { return run_.active; }
  void stop() { run_.active = false; }
  void current(std::vector<f64>& u) const;  // 6 per node
  // Bond load at the interface from node displacements u (6 per node).
  BondLoad bond_load(i32 b, const std::vector<f64>& u) const;
  // Load vector helpers (6 per node): gravity on every non-fixed node, a force at a point.
  static void add_force(std::vector<f64>& f, i32 node, const V3& c, const V3& F, const V3& at);

 private:
  void bond_matrices(const SBond& b, f64 D[36], f64 Ba[36], f64 Bb[36]) const;
  bool build_matrix();
  void precondition(const f64* r, f64* z) const;
  f64* block(i32 r, i32 c);  // K's block (nullptr if absent)
  bool assembled_ = false;
  bool jacobi_only_ = false;  // small graph: block Jacobi for every node
  i32 pc_n_ = 0;          // free nodes covered by the multigrid (the first ones)
  f64 id_scale_ = 1.0;    // diagonal of retired rows
  std::vector<f64> jinv_; // appended nodes: inverse diagonal blocks
  i32 nfree_ = 0;
  std::vector<i32> dof_;  // node -> free index (-1 fixed)
  Bsr6 K_;
  Amg amg_;
  Amg amg2_;              // appended nodes (a patched region): their own multigrid, added
  StressOptions opt_;
  struct Run {
    bool active = false;
    std::vector<f64> x, r, z, p, q;
    f64 rz = 0.0, bn = 0.0;
    int iters = 0;
  } run_;
};

}  // namespace svx
