"""How accurate is a coarse (rigid-aggregate) model of simple members, with/without scaling?"""
import numpy as np
import scipy.sparse.linalg as spla
from lattice import Lattice
from hierarchy import composite_partition, composite_system

def run(solid, anchor, probe_mask_fn, label):
    lat = Lattice(solid, anchor)
    u_ref = lat.full_u(spla.spsolve(lat.K_free().tocsc(), lat.gravity_free())).reshape(-1, 6)
    probe = probe_mask_fn(lat.coords)
    w_ref = u_ref[probe, 2].mean()
    out = [f"{label}: fine w={w_ref:.4e}"]
    for L in (1, 2, 3):
        for scaling in (False, True):
            lab, lev, nn = composite_partition(lat, np.array([-1e9, -1e9, -1e9]), 1e-9, 2.0, L)
            Ac, fc, P, sc, _ = composite_system(lat, lab, nn, scaling)
            uc = spla.spsolve(Ac.tocsc(), fc)
            u = lat.full_u(P @ uc).reshape(-1, 6)
            out.append(f"L{L}{'s' if scaling else 'g'}={u[probe, 2].mean() / w_ref:.3f}")
    print("  ".join(out))

# cantilever 64 x 4 x 4 anchored at x=0 (anchor layer x=0)
for (Lx, b) in ((64, 4), (64, 8), (32, 8)):
    solid = np.ones((Lx + 1, b, b), bool); anchor = np.zeros_like(solid); anchor[0] = True
    run(solid, anchor, lambda c: c[:, 0] == Lx, f"cantilever L={Lx} b={b}")

# fixed-fixed beam 64 x 4 x 4 anchored at both ends
for (Lx, b) in ((64, 4), (64, 8)):
    solid = np.ones((Lx + 2, b, b), bool); anchor = np.zeros_like(solid); anchor[0] = True; anchor[-1] = True
    run(solid, anchor, lambda c: np.abs(c[:, 0] - (Lx + 1) / 2) < 1.0, f"fixed-fixed L={Lx} b={b}")

# slab 48 x 48 x 2 supported on its 4 edges (anchored rim)
S = 48
solid = np.zeros((S + 2, S + 2, 2), bool); solid[:, :, :] = True
anchor = np.zeros_like(solid); anchor[0] = anchor[-1] = True; anchor[:, 0] = anchor[:, -1] = True
run(solid, anchor, lambda c: (np.abs(c[:, 0] - (S + 1) / 2) < 1.0) & (np.abs(c[:, 1] - (S + 1) / 2) < 1.0), "slab 48x48x2 edge-fixed")
# column: axial shortening
solid = np.ones((4, 4, 41), bool); anchor = np.zeros_like(solid); anchor[:, :, 0] = True
run(solid, anchor, lambda c: c[:, 2] == 40, "column 4x4x40 axial")
