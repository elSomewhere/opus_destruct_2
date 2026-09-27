import numpy as np
import scipy.sparse.linalg as spla
from lattice import Lattice
from hierarchy import composite_partition, composite_system2

def run(solid, anchor, probe_mask_fn, label, probe_dof=2):
    lat = Lattice(solid, anchor)
    u_ref = lat.full_u(spla.spsolve(lat.K_free().tocsc(), lat.gravity_free())).reshape(-1, 6)
    probe = probe_mask_fn(lat.coords)
    w_ref = u_ref[probe, probe_dof].mean()
    out = [f"{label}: fine={w_ref:.4e}"]
    for L in (1, 2, 3):
        for (sc, to, tag) in ((True, False, 's'), (True, True, 'st')):
            lab, lev, nn = composite_partition(lat, np.array([-1e9, -1e9, -1e9]), 1e-9, 2.0, L)
            Ac, fc, P, s, _ = composite_system2(lat, lab, nn, sc, to)
            uc = spla.spsolve(Ac.tocsc(), fc)
            u = lat.full_u(P @ uc).reshape(-1, 6)
            out.append(f"L{L}{tag}={u[probe, probe_dof].mean() / w_ref:.3f}")
    print("  ".join(out))

for (Lx, b) in ((64, 4), (64, 8)):
    solid = np.ones((Lx + 1, b, b), bool); anchor = np.zeros_like(solid); anchor[0] = True
    run(solid, anchor, lambda c: c[:, 0] == Lx, f"cantilever L={Lx} b={b}")
S = 48
for t in (2, 4):
    solid = np.ones((S + 2, S + 2, t), bool)
    anchor = np.zeros_like(solid); anchor[0] = anchor[-1] = True; anchor[:, 0] = anchor[:, -1] = True
    run(solid, anchor, lambda c: (np.abs(c[:, 0] - (S + 1) / 2) < 1.0) & (np.abs(c[:, 1] - (S + 1) / 2) < 1.0), f"slab 48x48x{t} edge-fixed")
# slab supported only at 4 corner columns-ish: anchored 2x2 patches at corners
solid = np.ones((S + 2, S + 2, 2), bool)
anchor = np.zeros_like(solid)
for (x, y) in ((0, 0), (0, S), (S, 0), (S, S)):
    anchor[x:x + 2, y:y + 2, :] = True
run(solid, anchor, lambda c: (np.abs(c[:, 0] - (S + 1) / 2) < 1.0) & (np.abs(c[:, 1] - (S + 1) / 2) < 1.0), "slab 48x48x2 corner-supported")
# torsion of a beam: cantilever 64 x 8 x 2 (wide flat strip) loaded eccentrically: add mass only on one edge? use rotation probe
Lx = 48
solid = np.ones((Lx + 1, 8, 2), bool); anchor = np.zeros_like(solid); anchor[0] = True
solid[:, 4:, 1] = False  # L-shaped section -> gravity induces torsion
run(solid, anchor, lambda c: c[:, 0] == Lx, "L-section cantilever (torsion+bending) w", 2)
run(solid, anchor, lambda c: c[:, 0] == Lx, "L-section cantilever twist theta_x", 3)
