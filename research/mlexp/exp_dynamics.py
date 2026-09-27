"""Implicit (backward Euler) lattice dynamics: MG-PCG iterations per step, and the dynamic
telescoping composite ("baseline + delta") vs the full fine dynamic reference (plan §A5)."""
import numpy as np, scipy.sparse as sp, scipy.sparse.linalg as spla
from lattice import Lattice, stress_measure, E
from structures import building, remove_column
from hierarchy import Multigrid, pcg, composite_partition, fiber_lengths, rigid_prolongator, torsion_correction

def iterations_per_step():
    s0, anc, cols = building(6, 6, 2)
    t = [c for c in cols if c[0] == 0 and c[1] == 2 and c[2] == 3][0]
    s1, center = remove_column(s0, t)
    pre, post = Lattice(s0, anc), Lattice(s1, anc)
    u_pre = pre.full_u(spla.spsolve(pre.K_free().tocsc(), pre.gravity_free())).reshape(-1, 6)
    x = u_pre[pre.cid[tuple(post.coords[post.free].T)]].ravel()
    K = post.K_free(); f = post.gravity_free(); nf = len(post.free)
    M = sp.diags(np.tile([1, 1, 1, 1 / 6, 1 / 6, 1 / 6], nf))
    pos = post.coords[post.free].astype(float); blk = post.coords[post.free].astype(np.int64)
    for beta in (1e2, 1e4, 1e6):  # k_axial / (m/dt^2)
        dt = np.sqrt(beta / E); A = (M / dt ** 2 + K).tocsr()
        for cs in (1.0, 0.5):
            Mop = Multigrid(A, pos, blk, np.ones(nf), coarse_size=2000, degree=3, coarse_scale=cs).as_linear_operator()
            xs, v, its = x.copy(), np.zeros_like(x), []
            for _ in range(20):
                rhs = (M / dt ** 2) @ (xs + dt * v) + f
                x_new, h = pcg(A, rhs, M=Mop, x0=xs + dt * v, tol=1e-4, maxit=200)
                its.append(len(h) - 1); v = (x_new - xs) / dt; xs = x_new
            print(f"beta={beta:.0e} coarse_scale={cs}: iters/step {its[:5]}...{its[-3:]} mean={np.mean(its):.1f}")

def dynamic_composite():
    s0, anc, cols = building(3, 3, 2)
    t = [c for c in cols if c[0] == 0 and c[1] == 1 and c[2] == 1][0]
    s1, center = remove_column(s0, t)
    pre, post = Lattice(s0, anc), Lattice(s1, anc)
    u_pre = pre.full_u(spla.spsolve(pre.K_free().tocsc(), pre.gravity_free())).reshape(-1, 6)
    u0 = u_pre[pre.cid[tuple(post.coords[post.free].T)]].ravel()
    K = post.K_free(); f = post.gravity_free(); r0 = f - K @ u0
    nf = len(post.free); M = sp.diags(np.tile([1, 1, 1, 1 / 6, 1 / 6, 1 / 6], nf))
    p = post.bond_points(); m = np.linalg.norm(p - center, axis=1) <= 8
    def run(A, Mm, r, Pop, steps, dt, scale):
        lu = spla.splu(A.tocsc()); d = np.zeros(A.shape[0]); v = np.zeros_like(d); peaks = []
        for _ in range(steps):
            dn = lu.solve(Mm @ (d + dt * v) / dt ** 2 + r); v = (dn - d) / dt; d = dn
            F = post.bond_forces(post.full_u(u0 + (Pop @ d if Pop is not None else d)), scale)
            peaks.append(np.abs(np.stack(stress_measure(F[m]))).max(1))
        return np.array(peaks)
    for beta in (1e4, 1e3):
        dt = np.sqrt(beta / E)
        ref = run((M / dt ** 2 + K).tocsr(), M, r0, None, 60, dt, None)
        for (R0, g) in ((8, 2.0), (8, 3.0), (12, 2.0)):
            label, level, nn = composite_partition(post, center, R0, g, 4)
            pc = post.coords[post.free].astype(float); pos = np.zeros((nn, 3)); cnt = np.zeros(nn)
            np.add.at(pos, label, pc); np.add.at(cnt, label, 1.0); pos /= cnt[:, None]
            P = rigid_prolongator(pc, label, pos, nn)
            fl = fiber_lengths(post, label); scale = 2.0 / (fl[:, 0] + fl[:, 1])
            Kc = (P.T @ post.K_free(scale) @ P).tocsr()
            dA = torsion_correction(post, label, scale, pos); Kc = Kc + dA if dA is not None else Kc
            Mc = (P.T @ M @ P).tocsr()
            comp = run((Mc / dt ** 2 + Kc).tocsr(), Mc, P.T @ r0, P, 60, dt, scale)
            print(f"beta={beta:.0e} R0={R0} g={g}: nodes={nn} max-over-time got/ref={np.round(comp.max(0) / ref.max(0), 3)}")

if __name__ == "__main__":
    iterations_per_step()
    dynamic_composite()
