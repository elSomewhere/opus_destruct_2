"""Sanity check of the lattice model: cantilever under self weight vs Timoshenko."""
import numpy as np
import scipy.sparse.linalg as spla
from lattice import Lattice, E, G, KAPPA, RHO_G, stress_measure

for L, b in [(20, 2), (40, 4), (24, 4), (64, 4)]:
    X, Y, Z = L + 1, b, b
    solid = np.ones((X, Y, Z), bool)
    anchor = np.zeros_like(solid)
    anchor[0] = True
    lat = Lattice(solid, anchor)
    K = lat.K_free()
    f = lat.gravity_free()
    u = spla.spsolve(K.tocsc(), f)
    uf = lat.full_u(u).reshape(-1, 6)
    tip = lat.coords[:, 0] == L
    w_tip = uf[tip, 2].mean()
    A = b * b
    I = b ** 4 / 12.0
    q = RHO_G * A
    # beam length measured from the anchor cell centre to the tip cell centre
    w_eb = q * L ** 4 / (8 * E * I)
    w_sh = q * L ** 2 / (2 * KAPPA * G * A)
    # root bending moment check: bonds between x=0 (anchor) and x=1
    F = lat.bond_forces(lat.full_u(u))
    root = (lat.bonds[:, 2] == 0) & (lat.coords[lat.bonds[:, 0], 0] == 0)
    from lattice import resultant
    Fr, Mr = resultant(lat, F, root, origin=np.array([0.5, (b - 1) / 2, (b - 1) / 2]))
    M_theory = q * (L + 0.5) ** 2 / 2  # load from x=1..L acting about x=0.5 (approx)
    print(f"L={L:3d} b={b}: tip w={w_tip:.4e}  EB+shear={w_eb + w_sh:.4e}  ratio={w_tip / (w_eb + w_sh):.4f}"
          f" | root M_y={Mr[1]:.2f} vs q(L)^2/2-ish {q * L * (L / 2 + 0.5):.2f}  V_z={Fr[2]:.2f} vs {q * L:.2f}")
