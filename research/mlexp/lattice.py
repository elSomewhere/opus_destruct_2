"""
Methodology spike: 3D rigid-body-spring (6-DOF cell) lattice, quasi-static.

Purpose: test the core claims behind the proposed engine methodology:
  (1) rigid-aggregation multigrid converges well on voxel structures with voids,
  (2) a telescoping "composite" bubble (fine near the event, coarser rigid
      aggregates farther away, whole structure included) reproduces the fine
      solution near the event and the load redistribution far away,
  (3) versus the prototype's fixed bubble with kinematic (Dirichlet) boundary.

Model: cells on an integer lattice (h = 1). DOFs per cell: u (3), theta (3).
Bond between face neighbours i -> j = i + e_a, measured at the interface
midpoint:  delta = u_j - u_i - (h/2)(theta_i + theta_j) x e_a,  phi = theta_j - theta_i.
Generalized jumps g = [delta.n, delta.t1, delta.t2, phi.n, phi.t1, phi.t2],
forces = D g with D = diag(EA/h, kGA/h, kGA/h, GJ/h, EI/h, EI/h)  (Timoshenko segment).
"""
import numpy as np
import scipy.sparse as sp

E = 1000.0
NU = 0.2
G = E / (2 * (1 + NU))
H = 1.0
AREA = H * H
I_SEC = H ** 4 / 12.0
J_SEC = 0.1406 * H ** 4
KAPPA = 5.0 / 6.0
D0 = np.array([E * AREA / H, KAPPA * G * AREA / H, KAPPA * G * AREA / H,
               G * J_SEC / H, E * I_SEC / H, E * I_SEC / H])
RHO_G = 1.0


class Lattice:
    def __init__(self, solid, anchor):
        self.shape = solid.shape
        self.solid = solid.copy()
        coords = np.argwhere(solid)
        self.coords = coords
        self.n = len(coords)
        cid = -np.ones(solid.shape, dtype=np.int64)
        cid[tuple(coords.T)] = np.arange(self.n)
        self.cid = cid
        self.anchor = anchor[tuple(coords.T)].copy()
        bl = []
        for a in range(3):
            si = [slice(None)] * 3
            sj = [slice(None)] * 3
            si[a] = slice(0, solid.shape[a] - 1)
            sj[a] = slice(1, solid.shape[a])
            both = solid[tuple(si)] & solid[tuple(sj)]
            ii = cid[tuple(si)][both]
            jj = cid[tuple(sj)][both]
            bl.append(np.stack([ii, jj, np.full(len(ii), a)], axis=1))
        bonds = np.concatenate(bl)
        keep = ~(self.anchor[bonds[:, 0]] & self.anchor[bonds[:, 1]])
        self.bonds = bonds[keep]
        self.nb = len(self.bonds)
        self.free = np.where(~self.anchor)[0]
        self.fmap = -np.ones(self.n, dtype=np.int64)
        self.fmap[self.free] = np.arange(len(self.free))
        self._B = None

    # ---- strain operator -------------------------------------------------
    def B(self):
        if self._B is not None:
            return self._B
        b = self.bonds
        nb = self.nb
        i, j, a = b[:, 0], b[:, 1], b[:, 2]
        t1 = (a + 1) % 3
        t2 = (a + 2) % 3
        rows, cols, vals = [], [], []
        r = np.arange(nb) * 6

        def add(row_off, cell, dof, v):
            rows.append(r + row_off)
            cols.append(cell * 6 + dof)
            vals.append(np.broadcast_to(v, (nb,)).astype(float))
        # delta . n
        add(0, j, a, 1.0); add(0, i, a, -1.0)
        # delta . t1 = du_t1 - h/2 (th_i + th_j)_t2
        add(1, j, t1, 1.0); add(1, i, t1, -1.0)
        add(1, i, 3 + t2, -H / 2); add(1, j, 3 + t2, -H / 2)
        # delta . t2 = du_t2 + h/2 (th_i + th_j)_t1
        add(2, j, t2, 1.0); add(2, i, t2, -1.0)
        add(2, i, 3 + t1, H / 2); add(2, j, 3 + t1, H / 2)
        # rotations
        add(3, j, 3 + a, 1.0); add(3, i, 3 + a, -1.0)
        add(4, j, 3 + t1, 1.0); add(4, i, 3 + t1, -1.0)
        add(5, j, 3 + t2, 1.0); add(5, i, 3 + t2, -1.0)
        self._B = sp.csr_matrix((np.concatenate(vals), (np.concatenate(rows), np.concatenate(cols))),
                                shape=(6 * nb, 6 * self.n))
        return self._B

    def dvec(self, scale=None):
        d = np.tile(D0, self.nb)
        if scale is not None:
            d = d * np.repeat(scale, 6)
        return d

    def free_dofs(self):
        return (self.free[:, None] * 6 + np.arange(6)[None, :]).ravel()

    def K_free(self, scale=None):
        B = self.B()
        K = (B.T @ sp.diags(self.dvec(scale)) @ B).tocsr()
        fd = self.free_dofs()
        return K[fd][:, fd].tocsr()

    def gravity_free(self):
        f = np.zeros(6 * len(self.free))
        f[np.arange(len(self.free)) * 6 + 2] = -RHO_G * H ** 3
        return f

    def full_u(self, uf):
        u = np.zeros(6 * self.n)
        u[self.free_dofs()] = uf
        return u

    def bond_forces(self, u_full, scale=None):
        g = self.B() @ u_full
        return (self.dvec(scale) * g).reshape(-1, 6)

    def bond_points(self):
        b = self.bonds
        p = self.coords[b[:, 0]].astype(float)
        p[np.arange(self.nb), b[:, 2]] += 0.5
        return p


def resultant(lat, F, mask, origin):
    """Total force + moment (about origin) transmitted by bonds in mask, in world frame.
    Sign: force exerted by the j-side on the i-side is -F in local frame... we keep the
    bond-local convention consistently (positive N = tension)."""
    b = lat.bonds[mask]
    Fm = F[mask]
    a = b[:, 2]
    t1 = (a + 1) % 3
    t2 = (a + 2) % 3
    nb = len(b)
    fw = np.zeros((nb, 3))
    mw = np.zeros((nb, 3))
    idx = np.arange(nb)
    fw[idx, a] += Fm[:, 0]; fw[idx, t1] += Fm[:, 1]; fw[idx, t2] += Fm[:, 2]
    mw[idx, a] += Fm[:, 3]; mw[idx, t1] += Fm[:, 4]; mw[idx, t2] += Fm[:, 5]
    pts = lat.bond_points()[mask] - origin
    mtot = mw.sum(0) + np.cross(pts, fw).sum(0)
    return fw.sum(0), mtot


def stress_measure(F):
    """Max fibre tensile stress + shear stress per bond (simple utilization proxy)."""
    N = F[:, 0]
    V = np.hypot(F[:, 1], F[:, 2])
    T = np.abs(F[:, 3])
    M = np.abs(F[:, 4]) + np.abs(F[:, 5])
    sig_t = N / AREA + M * (H / 2) / I_SEC      # tension-positive extreme fibre
    sig_c = -N / AREA + M * (H / 2) / I_SEC     # compression extreme fibre
    tau = V / AREA + T / (0.208 * H ** 3)
    return sig_t, sig_c, tau
