"""Does the composite bubble keep its size/accuracy as the structure grows?"""
import time
import numpy as np
import scipy.sparse.linalg as spla
from lattice import Lattice, stress_measure
from structures import building, remove_column
from hierarchy import composite_partition, composite_system2

for (nb, st) in ((3, 2), (6, 2), (8, 3)):
    solid0, anchor, cols = building(nx_bays=nb, ny_bays=nb, stories=st)
    target = [c for c in cols if c[0] == 0 and c[1] == 1 and c[2] == 1][0]
    solid1, center = remove_column(solid0, target)
    lat = Lattice(solid1, anchor)
    t = time.time()
    u_ref = lat.full_u(spla.spsolve(lat.K_free().tocsc(), lat.gravity_free()))
    tref = time.time() - t
    F_ref = lat.bond_forces(u_ref)
    p = lat.bond_points()
    for (R0, L, g) in ((8, 4, 2.0), (8, 4, 3.0)):
        label, level, nn = composite_partition(lat, center, R0, g, L)
        t = time.time()
        Ac, fc, P, scale, _ = composite_system2(lat, label, nn, True, True)
        uc = spla.spsolve(Ac.tocsc(), fc)
        tc = time.time() - t
        F = lat.bond_forces(lat.full_u(P @ uc), scale)
        m = np.linalg.norm(p - center, axis=1) <= R0
        sr = np.stack(stress_measure(F_ref[m])); sx = np.stack(stress_measure(F[m]))
        peak_ratio = np.abs(sx).max(1) / np.abs(sr).max(1)
        print(f"{nb}x{nb} bays {st} storeys: free cells={len(lat.free):6d} | composite R0={R0} g={g}: nodes={nn:5d} "
              f"per-level={np.bincount(level, minlength=L + 1)} peak(sig_t,sig_c,tau) got/ref={np.round(peak_ratio, 3)} "
              f"(ref solve {tref:.1f}s, composite {tc:.2f}s)")
