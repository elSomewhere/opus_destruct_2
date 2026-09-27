"""Plate-dominated "Doom-like" rooms: walls + ceiling slabs on an anchored floor.
Composite telescoping bubble vs full fine solve for rocket craters (plan §A5)."""
import numpy as np, scipy.sparse.linalg as spla, time
from lattice import Lattice, stress_measure
from structures import carve_sphere
from hierarchy import composite_partition, fiber_lengths, rigid_prolongator, torsion_correction

def rooms(nx=3, ny=2, room=(28, 20), wall=4, height=20, slab=3):
    X = nx * (room[0] + wall) + wall; Y = ny * (room[1] + wall) + wall; Z = 1 + height + slab
    s = np.zeros((X, Y, Z), bool); a = np.zeros_like(s)
    s[:, :, 0] = True; a[:, :, 0] = True
    for i in range(nx + 1):
        x0 = i * (room[0] + wall); s[x0:x0 + wall, :, 1:1 + height] = True
    for j in range(ny + 1):
        y0 = j * (room[1] + wall); s[:, y0:y0 + wall, 1:1 + height] = True
    for i in range(1, nx):
        x0 = i * (room[0] + wall)
        for j in range(ny):
            yc = j * (room[1] + wall) + wall + room[1] // 2
            s[x0:x0 + wall, yc - 3:yc + 3, 1:13] = False
    s[:, :, 1 + height:1 + height + slab] = True
    return s, a

def solve_comp(lat, center, R0, g, L, soft=1.0):
    label, level, nn = composite_partition(lat, center, R0, g, L)
    pc = lat.coords[lat.free].astype(float)
    pos = np.zeros((nn, 3)); cnt = np.zeros(nn)
    np.add.at(pos, label, pc); np.add.at(cnt, label, 1.0); pos /= cnt[:, None]
    P = rigid_prolongator(pc, label, pos, nn)
    fl = fiber_lengths(lat, label); scale = 2.0 / (fl[:, 0] + fl[:, 1])
    scale = np.where((fl[:, 0] > 1) | (fl[:, 1] > 1), scale * soft, scale)
    Ac = (P.T @ lat.K_free(scale) @ P).tocsr()
    dA = torsion_correction(lat, label, scale, pos)
    if dA is not None: Ac = Ac + dA
    uc = spla.spsolve(Ac.tocsc(), P.T @ lat.gravity_free())
    return lat.bond_forces(lat.full_u(P @ uc), scale), nn, np.bincount(level, minlength=L + 1)

if __name__ == "__main__":
    s0, anc = rooms()
    events = [("rocket r=6 at base of interior wall", np.array([34.0, 12.0, 3.0]), 6.0),
              ("rocket r=6 mid-ceiling", np.array([46.0, 34.0, 22.0]), 6.0),
              ("rocket r=8 at wall/ceiling corner", np.array([34.0, 26.0, 20.0]), 8.0)]
    for name, cen, r in events:
        lat = Lattice(carve_sphere(s0, anc, cen, r), anc)
        u = lat.full_u(spla.spsolve(lat.K_free().tocsc(), lat.gravity_free()))
        F_ref = lat.bond_forces(u); p = lat.bond_points()
        m = np.linalg.norm(p - cen, axis=1) <= 2 * r
        sr = np.abs(np.stack(stress_measure(F_ref[m]))).max(1)
        print(f"\n{name}: free cells={len(lat.free)}")
        for (R0, g, L) in ((12, 2.0, 4), (16, 2.0, 4), (16, 3.0, 4), (24, 2.0, 4)):
            F, nn, per = solve_comp(lat, cen, R0, g, L)
            sx = np.abs(np.stack(stress_measure(F[m]))).max(1)
            print(f"   R0={R0:2d} g={g}: nodes={nn:6d} {per} peak got/ref={np.round(sx / sr, 3)}")
