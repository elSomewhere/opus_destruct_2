"""Can coarse-face resultants bound the fine-bond stresses inside coarse regions?
For each coarse composite node, compare an elastic beam-theory estimate from its face resultants
(composite solution) with the true max fine-bond stress (reference solution) on any bond touching it."""
import numpy as np
import scipy.sparse.linalg as spla
from lattice import Lattice, stress_measure, H
from structures import building, remove_column
from hierarchy import composite_partition, composite_system2

solid0, anchor, cols = building(nx_bays=3, ny_bays=3, stories=2)
target = [c for c in cols if c[0] == 0 and c[1] == 1 and c[2] == 1][0]
solid1, center = remove_column(solid0, target)
lat = Lattice(solid1, anchor)
u_ref = lat.full_u(spla.spsolve(lat.K_free().tocsc(), lat.gravity_free()))
F_ref = lat.bond_forces(u_ref)
st, sc, ta = stress_measure(F_ref)
sig_ref = np.maximum(np.maximum(st, sc), 0)

for (R0, L, g) in ((8, 3, 2.0), (4, 3, 2.0), (1e-9, 2, 2.0), (1e-9, 3, 2.0)):
    label, level, nn = composite_partition(lat, center, R0, g, L)
    Ac, fc, P, scale, npos = composite_system2(lat, label, nn, True, True)
    uc = spla.spsolve(Ac.tocsc(), fc)
    u = lat.full_u(P @ uc)
    F = lat.bond_forces(u, scale)
    b = lat.bonds
    cl = -np.ones(lat.n, dtype=np.int64); cl[lat.free] = label
    li, lj = cl[b[:, 0]], cl[b[:, 1]]
    crossing = li != lj
    idx = np.where(crossing)[0]
    ki = np.where(li[idx] >= 0, li[idx], -1 - b[idx, 0])
    kj = np.where(lj[idx] >= 0, lj[idx], -1 - b[idx, 1])
    keys = np.stack([ki, kj, b[idx, 2]], 1)
    uk, inv = np.unique(keys, axis=0, return_inverse=True); inv = inv.ravel()
    nf = len(uk)
    p = lat.bond_points()[idx]
    a = b[idx, 2]; t1 = (a + 1) % 3; t2 = (a + 2) % 3
    ar = np.arange(len(idx))
    cnt = np.bincount(inv, minlength=nf).astype(float)
    cen = np.zeros((nf, 3)); np.add.at(cen, inv, p); cen /= cnt[:, None]
    r = p - cen[inv]; r1 = r[ar, t1]; r2 = r[ar, t2]
    Fi = F[idx]
    N = np.bincount(inv, weights=Fi[:, 0], minlength=nf)
    V1 = np.bincount(inv, weights=Fi[:, 1], minlength=nf)
    V2 = np.bincount(inv, weights=Fi[:, 2], minlength=nf)
    # moments about face centroid: bond moment + r x f (in local frame n,t1,t2: r=(0,r1,r2), f=(N,V1,V2))
    T = np.bincount(inv, weights=Fi[:, 3] + r1 * Fi[:, 2] - r2 * Fi[:, 1], minlength=nf)
    M1 = np.bincount(inv, weights=Fi[:, 4] + r2 * Fi[:, 0], minlength=nf)
    M2 = np.bincount(inv, weights=Fi[:, 5] - r1 * Fi[:, 0], minlength=nf)
    I1 = np.bincount(inv, weights=r2 ** 2, minlength=nf) + cnt / 12
    I2 = np.bincount(inv, weights=r1 ** 2, minlength=nf) + cnt / 12
    c1 = np.zeros(nf); np.maximum.at(c1, inv, np.abs(r2)); c1 += 0.5
    c2 = np.zeros(nf); np.maximum.at(c2, inv, np.abs(r1)); c2 += 0.5
    sig_est = np.abs(N) / cnt + np.abs(M1) * c1 / I1 + np.abs(M2) * c2 / I2
    # per node: indicator = max over its faces; truth = max ref stress on any bond touching the node
    ind = np.zeros(nn)
    for side in (0, 1):
        k = uk[:, side]
        m = k >= 0
        np.maximum.at(ind, k[m], sig_est[m])
    truth = np.zeros(nn)
    for side in (li, lj):
        m = side >= 0
        np.maximum.at(truth, side[m], sig_ref[m])
    counts = np.bincount(label, minlength=nn)
    sel = (counts > 1) & (truth > 0.1 * sig_ref.max())
    ratio = truth[sel] / np.maximum(ind[sel], 1e-12)
    q = np.quantile(ratio, [0.5, 0.9, 0.99, 1.0])
    print(f"R0={R0:g} L={L}: coarse nodes checked={sel.sum()}  truth/indicator quantiles (50/90/99/100%) = {np.round(q, 2)}"
          f"  | global ref max={sig_ref.max():.1f}, max truth in coarse nodes={truth[counts > 1].max():.1f}, max indicator={ind[counts > 1].max():.1f}")
