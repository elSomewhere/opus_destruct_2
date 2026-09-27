"""Multigrid-preconditioned CG on the full fine lattice: iterations vs problem size."""
import sys, time
import numpy as np
import scipy.sparse.linalg as spla
from lattice import Lattice
from structures import building, remove_column
from hierarchy import Multigrid, pcg

cases = [(3, 2), (5, 2), (6, 3), (8, 3)]
if len(sys.argv) > 1:
    cases = cases[:int(sys.argv[1])]
for (nb, st) in cases:
    solid, anchor, cols = building(nx_bays=nb, ny_bays=nb, stories=st)
    lat = Lattice(solid, anchor)
    K = lat.K_free()
    f = lat.gravity_free()
    pos = lat.coords[lat.free].astype(float)
    blk = lat.coords[lat.free].astype(np.int64)
    w = np.ones(len(lat.free))
    print(f"\nbuilding {nb}x{nb} bays, {st} storeys: free cells={len(lat.free)} dofs={K.shape[0]}")
    for (deg, cs) in ((2, 1.0), (3, 1.0), (3, 0.5), (4, 0.5)):
        t = time.time()
        mg = Multigrid(K, pos, blk, w, coarse_size=2000, degree=deg, coarse_scale=cs)
        ts = time.time() - t
        t = time.time()
        x, hist = pcg(K, f, M=mg.as_linear_operator(), tol=1e-8, maxit=300)
        tsol = time.time() - t
        # also: plain block-Jacobi CG for reference (capped)
        print(f"  MG cheb-deg={deg} coarse_scale={cs}: levels={mg.sizes} setup={ts:.1f}s PCG iters={len(hist) - 1} "
              f"({tsol:.1f}s) final rel.res={hist[-1]:.1e}")
    # warm-started update after removing a column: how many iterations from the old solution?
    target = [c for c in cols if c[0] == 0 and c[1] == 1 and c[2] == 1][0]
    solid1, center = remove_column(solid, target)
    lat1 = Lattice(solid1, anchor)
    K1 = lat1.K_free(); f1 = lat1.gravity_free()
    idx = lat.cid[tuple(lat1.coords[lat1.free].T)]  # post free cell -> pre cell id
    u_old = lat.full_u(x).reshape(-1, 6)[idx].ravel()
    mg1 = Multigrid(K1, lat1.coords[lat1.free].astype(float), lat1.coords[lat1.free].astype(np.int64),
                    np.ones(len(lat1.free)), coarse_size=2000, degree=3, coarse_scale=0.5)
    for tol in (1e-4, 1e-6, 1e-8):
        _, h0 = pcg(K1, f1, M=mg1.as_linear_operator(), tol=tol, maxit=300)
        _, h1 = pcg(K1, f1, M=mg1.as_linear_operator(), x0=u_old, tol=tol * (np.linalg.norm(f1) / np.linalg.norm(f1 - K1 @ u_old)), maxit=300)
        print(f"  after column removal, tol={tol:.0e}: cold iters={len(h0) - 1}, warm-start iters={len(h1) - 1} "
              f"(initial warm residual {np.linalg.norm(f1 - K1 @ u_old) / np.linalg.norm(f1):.2e})")
