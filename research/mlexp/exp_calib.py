"""Does a single calibration factor on coarse-coupled bonds remove the stiffness bias?"""
import numpy as np
import scipy.sparse.linalg as spla
import hierarchy
from lattice import Lattice, stress_measure
from structures import building, remove_column, carve_sphere
from hierarchy import composite_partition, fiber_lengths, rigid_prolongator, torsion_correction

def solve_comp(lat, center, R0, g, L, soft):
    label, level, nn = composite_partition(lat, center, R0, g, L)
    free = lat.free
    pos_cells = lat.coords[free].astype(float)
    pos = np.zeros((nn, 3)); cnt = np.zeros(nn)
    np.add.at(pos, label, pos_cells); np.add.at(cnt, label, 1.0); pos /= cnt[:, None]
    P = rigid_prolongator(pos_cells, label, pos, nn)
    fl = fiber_lengths(lat, label)
    scale = 2.0 / (fl[:, 0] + fl[:, 1])
    coarse = (fl[:, 0] > 1) | (fl[:, 1] > 1)
    scale = np.where(coarse, scale * soft, scale)
    Ac = (P.T @ lat.K_free(scale) @ P).tocsr()
    dA = torsion_correction(lat, label, scale, pos)
    if dA is not None:
        Ac = Ac + dA
    uc = spla.spsolve(Ac.tocsc(), P.T @ lat.gravity_free())
    return lat.bond_forces(lat.full_u(P @ uc), scale), nn

scenes = []
s0, anc, cols = building(3, 3, 2)
t = [c for c in cols if c[0] == 0 and c[1] == 1 and c[2] == 1][0]
s1, c1 = remove_column(s0, t); scenes.append(("3x3 col-removal", s1, anc, c1))
s0b, ancb, colsb = building(6, 6, 2)
t = [c for c in colsb if c[0] == 0 and c[1] == 2 and c[2] == 3][0]
s1b, c1b = remove_column(s0b, t); scenes.append(("6x6 col-removal", s1b, ancb, c1b))
# blast hole in the upper slab of the 3x3 building near mid-bay
cen = np.array([20.0, 20.0, 23.5]); scenes.append(("3x3 slab blast r=4", carve_sphere(s0, anc, cen, 4.0), anc, cen))
# blast at the base of a wall/column junction on storey 2
cen2 = np.array([14.0, 1.0, 14.0]); scenes.append(("3x3 wall blast r=3", carve_sphere(s0, anc, cen2, 3.0), anc, cen2))

for name, solid, anchor, center in scenes:
    lat = Lattice(solid, anchor)
    u_ref = lat.full_u(spla.spsolve(lat.K_free().tocsc(), lat.gravity_free()))
    F_ref = lat.bond_forces(u_ref)
    p = lat.bond_points()
    m = np.linalg.norm(p - center, axis=1) <= 8
    sr = np.abs(np.stack(stress_measure(F_ref[m]))).max(1)
    row = []
    for soft in (1.0, 0.9, 0.8):
        F, nn = solve_comp(lat, center, 8, 2.0, 4, soft)
        sx = np.abs(np.stack(stress_measure(F[m]))).max(1)
        row.append(f"soft={soft}: {np.round(sx / sr, 3)}")
    print(f"{name:22s} nodes={nn:5d} (fine cells {len(lat.free)}) | " + " | ".join(row))
