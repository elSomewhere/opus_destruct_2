"""
Rigid-aggregation hierarchy over the lattice (connected components inside 2^l blocks),
multigrid preconditioner, and composite ("telescoping bubble") systems.
"""
import numpy as np
import scipy.sparse as sp
import scipy.sparse.linalg as spla
from scipy.sparse.csgraph import connected_components
from lattice import H


def skew(r):
    """[r]_x such that [r]_x v = r x v, vectorized: r (n,3) -> (n,3,3)."""
    n = len(r)
    S = np.zeros((n, 3, 3))
    S[:, 0, 1] = -r[:, 2]; S[:, 0, 2] = r[:, 1]
    S[:, 1, 0] = r[:, 2];  S[:, 1, 2] = -r[:, 0]
    S[:, 2, 0] = -r[:, 1]; S[:, 2, 1] = r[:, 0]
    return S


def rigid_prolongator(child_pos, child_node, parent_pos, n_parent):
    """P maps parent rigid motions (U, Theta about parent_pos) to child 6-dof motions
    (u, theta about child_pos): u_c = U + Theta x r = U - [r]x Theta, theta_c = Theta."""
    nc = len(child_pos)
    r = child_pos - parent_pos[child_node]
    S = skew(r)
    rows, cols, vals = [], [], []
    for d in range(3):
        # translations: u_c[d] = U[d]
        rows.append(np.arange(nc) * 6 + d); cols.append(child_node * 6 + d); vals.append(np.ones(nc))
        # rotations: theta_c[d] = Theta[d]
        rows.append(np.arange(nc) * 6 + 3 + d); cols.append(child_node * 6 + 3 + d); vals.append(np.ones(nc))
        # u_c[d] -= sum_e S[d,e] Theta[e]
        for e in range(3):
            v = -S[:, d, e]
            m = v != 0
            rows.append((np.arange(nc) * 6 + d)[m]); cols.append((child_node * 6 + 3 + e)[m]); vals.append(v[m])
    return sp.csr_matrix((np.concatenate(vals), (np.concatenate(rows), np.concatenate(cols))),
                         shape=(6 * nc, 6 * n_parent))


def aggregate_level(node_block, node_pos, node_w, A):
    """One coarsening step. node_block: (n,3) int block coords of nodes at this level.
    Children are grouped by parent block = node_block // 2, then split into connected
    components using the operator graph A (6x6 blocks). Returns parent info and P."""
    n = len(node_block)
    # node adjacency from A block structure
    Ab = A.tocoo()
    ni = Ab.row // 6
    nj = Ab.col // 6
    m = ni != nj
    pb = node_block // 2
    same = np.all(pb[ni[m]] == pb[nj[m]], axis=1)
    adj = sp.csr_matrix((np.ones(same.sum()), (ni[m][same], nj[m][same])), shape=(n, n))
    # components restricted to same parent block
    ncomp, lab = connected_components(adj, directed=False)
    # Weighted centroids
    w = node_w
    pos = np.zeros((ncomp, 3))
    np.add.at(pos, lab, node_pos * w[:, None])
    wsum = np.zeros(ncomp)
    np.add.at(wsum, lab, w)
    pos /= wsum[:, None]
    blk = np.zeros((ncomp, 3), dtype=np.int64)
    blk[lab] = pb
    P = rigid_prolongator(node_pos, lab, pos, ncomp)
    return lab, pos, wsum, blk, P


class BlockJacobiChebyshev:
    def __init__(self, A, degree=3, lam_ratio=0.1):
        self.A = A
        n = A.shape[0] // 6
        # extract 6x6 diagonal blocks and invert
        Acoo = A.tocoo()
        m = (Acoo.row // 6) == (Acoo.col // 6)
        blocks = np.zeros((n, 6, 6))
        np.add.at(blocks, (Acoo.row[m] // 6, Acoo.row[m] % 6, Acoo.col[m] % 6), Acoo.data[m])
        # guard singular blocks
        for k in np.where(np.abs(np.linalg.det(blocks)) < 1e-300)[0]:
            blocks[k] += np.eye(6) * 1e-12
        inv = np.linalg.inv(blocks)
        self.Dinv = sp.block_diag(list(inv), format='csr') if n < 50 else self._bd(inv)
        # estimate lambda_max of Dinv A by power iteration
        x = np.random.default_rng(0).standard_normal(A.shape[0])
        for _ in range(30):
            y = self.Dinv @ (A @ x)
            lam = np.linalg.norm(y) / np.linalg.norm(x)
            x = y / np.linalg.norm(y)
        self.lmax = 1.1 * lam
        self.lmin = lam_ratio * self.lmax
        self.degree = degree

    @staticmethod
    def _bd(inv):
        n = len(inv)
        r = (np.arange(n)[:, None, None] * 6 + np.arange(6)[None, :, None]) * np.ones((1, 1, 6), int)
        c = (np.arange(n)[:, None, None] * 6 + np.arange(6)[None, None, :]) * np.ones((1, 6, 1), int)
        return sp.csr_matrix((inv.ravel(), (r.ravel(), c.ravel())), shape=(6 * n, 6 * n))

    def smooth(self, x, b):
        # Chebyshev iteration on Dinv A (standard 3-term recurrence)
        A, Dinv = self.A, self.Dinv
        theta = 0.5 * (self.lmax + self.lmin)
        delta = 0.5 * (self.lmax - self.lmin)
        sigma = theta / delta
        rho = 1.0 / sigma
        r = Dinv @ (b - A @ x)
        d = r / theta
        for _ in range(self.degree):
            x = x + d
            r = r - Dinv @ (A @ d)
            rho_new = 1.0 / (2 * sigma - rho)
            d = rho_new * rho * d + 2 * rho_new / delta * r
            rho = rho_new
        return x


class Multigrid:
    """Rigid-aggregation multigrid V-cycle used as a PCG preconditioner.
    coarse_scale: multiply each Galerkin coarse operator by this factor (1.0 = pure Galerkin;
    0.5 ~ 'series-length' rescaling of unsmoothed aggregation)."""

    def __init__(self, A0, pos0, block0, w0, coarse_size=3000, degree=3, coarse_scale=1.0, max_levels=12):
        self.A = [A0]
        self.P = []
        self.smoothers = []
        pos, blk, w = pos0, block0, w0
        A = A0
        while A.shape[0] > coarse_size and len(self.A) < max_levels:
            lab, pos, w, blk, P = aggregate_level(blk, pos, w, A)
            Ac = (P.T @ A @ P).tocsr() * coarse_scale
            if Ac.shape[0] >= 0.9 * A.shape[0]:
                break
            self.P.append(P)
            self.A.append(Ac)
            A = Ac
        for Al in self.A[:-1]:
            self.smoothers.append(BlockJacobiChebyshev(Al, degree=degree))
        self.coarse_lu = spla.splu(self.A[-1].tocsc())
        self.sizes = [Al.shape[0] // 6 for Al in self.A]

    def vcycle(self, b, lvl=0):
        if lvl == len(self.A) - 1:
            return self.coarse_lu.solve(b)
        A = self.A[lvl]
        S = self.smoothers[lvl]
        x = S.smooth(np.zeros_like(b), b)
        r = b - A @ x
        xc = self.vcycle(self.P[lvl].T @ r, lvl + 1)
        x = x + self.P[lvl] @ xc
        x = S.smooth(x, b)
        return x

    def as_linear_operator(self):
        n = self.A[0].shape[0]
        return spla.LinearOperator((n, n), matvec=lambda b: self.vcycle(b))


def pcg(A, b, M=None, x0=None, tol=1e-8, maxit=500):
    x = np.zeros_like(b) if x0 is None else x0.copy()
    r = b - A @ x
    z = M @ r if M is not None else r
    p = z.copy()
    rz = r @ z
    bn = np.linalg.norm(b)
    hist = [np.linalg.norm(r) / bn]
    for k in range(maxit):
        Ap = A @ p
        alpha = rz / (p @ Ap)
        x += alpha * p
        r -= alpha * Ap
        rn = np.linalg.norm(r) / bn
        hist.append(rn)
        if rn < tol:
            break
        z = M @ r if M is not None else r
        rz_new = r @ z
        p = z + (rz_new / rz) * p
        rz = rz_new
    return x, hist


# ---------------------------------------------------------------------------
# Composite (telescoping) partitions
# ---------------------------------------------------------------------------

def composite_partition(lat, center, R0, grading=2.0, Lmax=4, metric='euclid'):
    """Assign every free cell to a composite node. Cells within R0 of the event stay fine;
    level-l blocks (size 2^l) whose cells are all at distance >= R0*grading^(l-1) are
    aggregated (connected components within the block, using bond connectivity among
    free cells). Returns node label per free cell, node level, node count."""
    free = lat.free
    coords = lat.coords[free].astype(float)
    d = np.linalg.norm(coords - center, axis=1) if metric == 'euclid' else np.max(np.abs(coords - center), axis=1)
    desired = np.zeros(len(free), dtype=np.int64)
    for l in range(1, Lmax + 1):
        desired[d >= R0 * grading ** (l - 1)] = l
    # free-free bond graph
    b = lat.bonds
    fi = lat.fmap[b[:, 0]]
    fj = lat.fmap[b[:, 1]]
    m = (fi >= 0) & (fj >= 0)
    fi, fj = fi[m], fj[m]
    nf = len(free)
    label = -np.ones(nf, dtype=np.int64)
    level = []
    next_id = 0
    icoords = lat.coords[free]
    for l in range(Lmax, 0, -1):
        s = 2 ** l
        blk = icoords // s
        # block key
        key = (blk[:, 0] * 100003 + blk[:, 1]) * 100019 + blk[:, 2]
        # a block qualifies if all its (unassigned) cells desire >= l and none assigned yet
        uk, inv = np.unique(key, return_inverse=True)
        min_des = np.full(len(uk), 10 ** 9)
        np.minimum.at(min_des, inv, desired)
        any_assigned = np.zeros(len(uk), bool)
        np.logical_or.at(any_assigned, inv, label >= 0)
        qual = (min_des[inv] >= l) & (~any_assigned[inv])
        if not qual.any():
            continue
        # components among qualifying cells within the same block
        mm = qual[fi] & qual[fj] & (key[fi] == key[fj])
        adj = sp.csr_matrix((np.ones(mm.sum()), (fi[mm], fj[mm])), shape=(nf, nf))
        nc, lab = connected_components(adj, directed=False)
        sel = np.where(qual)[0]
        ul, newlab = np.unique(lab[sel], return_inverse=True)
        label[sel] = next_id + newlab
        level += [l] * len(ul)
        next_id += len(ul)
    rest = np.where(label < 0)[0]
    label[rest] = next_id + np.arange(len(rest))
    level += [0] * len(rest)
    return label, np.array(level), next_id + len(rest)


def fiber_lengths(lat, label):
    """For each bond endpoint, the number of cells of the endpoint's composite node lying on
    the bond's line (the 'fibre length' along the bond axis). Anchor endpoints -> 1."""
    b = lat.bonds
    nb = len(b)
    out = np.ones((nb, 2))
    free = lat.free
    cell_label = -np.ones(lat.n, dtype=np.int64)
    cell_label[free] = label
    c = lat.coords
    for a in range(3):
        t1, t2 = (a + 1) % 3, (a + 2) % 3
        ok = cell_label >= 0
        key_cells = (cell_label[ok] * 4096 + c[ok, t1]) * 4096 + c[ok, t2]
        uk, cnt = np.unique(key_cells, return_counts=True)
        sel = np.where(b[:, 2] == a)[0]
        for side in (0, 1):
            cells = b[sel, side]
            lab = cell_label[cells]
            has = lab >= 0
            k = (lab[has] * 4096 + c[cells[has], t1]) * 4096 + c[cells[has], t2]
            pos = np.searchsorted(uk, k)
            out[sel[has], side] = cnt[pos]
    return out


def composite_system(lat, label, nnodes, scaling=True):
    """Build composite operator A_c = P^T K_s P, rhs, and P (free fine dofs <- composite dofs)."""
    free = lat.free
    pos_cells = lat.coords[free].astype(float)
    pos = np.zeros((nnodes, 3))
    cnt = np.zeros(nnodes)
    np.add.at(pos, label, pos_cells)
    np.add.at(cnt, label, 1.0)
    pos /= cnt[:, None]
    P = rigid_prolongator(pos_cells, label, pos, nnodes)
    if scaling:
        fl = fiber_lengths(lat, label)
        scale = 2.0 / (fl[:, 0] + fl[:, 1])
    else:
        scale = None
    Ks = lat.K_free(scale)
    Ac = (P.T @ Ks @ P).tocsr()
    fc = P.T @ lat.gravity_free()
    return Ac, fc, P, scale, pos


def torsion_correction(lat, label, scale, npos):
    """Replace the warping-restrained (polar-moment) torsional stiffness that rigid aggregates
    impose on each coarse face by a St-Venant estimate J ~ A^4 / (4 pi^2 Ip) of the face section.
    Returns a sparse correction dA to add to the composite operator."""
    import numpy as np
    import scipy.sparse as sp
    from lattice import D0, G, H
    b = lat.bonds
    cell_label = -np.ones(lat.n, dtype=np.int64)
    cell_label[lat.free] = label
    li = cell_label[b[:, 0]]
    lj = cell_label[b[:, 1]]
    lvl_i = li >= 0
    lvl_j = lj >= 0
    # faces keyed by (node_i or -1-anchor-cell, node_j or -1-anchor-cell, axis); only faces that
    # involve at least one multi-cell node need correction
    ki = np.where(lvl_i, li, -1 - b[:, 0])
    kj = np.where(lvl_j, lj, -1 - b[:, 1])
    counts = np.bincount(label, minlength=label.max() + 1)
    multi = (lvl_i & (counts[np.maximum(li, 0)] > 1)) | (lvl_j & (counts[np.maximum(lj, 0)] > 1))
    idx = np.where(multi)[0]
    if len(idx) == 0:
        return None
    keys = np.stack([ki[idx], kj[idx], b[idx, 2]], 1)
    uk, inv = np.unique(keys, axis=0, return_inverse=True)
    inv = inv.ravel()
    p = lat.bond_points()[idx]
    a = b[idx, 2]
    t1 = (a + 1) % 3
    t2 = (a + 2) % 3
    s = scale[idx] if scale is not None else np.ones(len(idx))
    nf = len(uk)
    cnt = np.bincount(inv, minlength=nf).astype(float)
    cen = np.zeros((nf, 3))
    np.add.at(cen, inv, p)
    cen /= cnt[:, None]
    r = p - cen[inv]
    r1 = r[np.arange(len(idx)), t1]
    r2 = r[np.arange(len(idx)), t2]
    rr = r1 ** 2 + r2 ** 2
    area = cnt * H * H
    Ip = np.bincount(inv, weights=rr, minlength=nf) * H * H + cnt * (H ** 4 / 6.0)
    J_sv = area ** 4 / (4 * np.pi ** 2 * Ip)
    s_mean = np.bincount(inv, weights=s, minlength=nf) / cnt
    gal = np.bincount(inv, weights=s * (D0[3] + D0[1] * rr), minlength=nf)
    target = G * J_sv / H * s_mean
    c = np.minimum(target - gal, 0.0)
    rows, cols, vals = [], [], []
    for f in range(nf):
        if c[f] == 0.0:
            continue
        ax = uk[f, 2]
        ent = []
        if uk[f, 0] >= 0:
            ent.append((uk[f, 0] * 6 + 3 + ax, -1.0))
        if uk[f, 1] >= 0:
            ent.append((uk[f, 1] * 6 + 3 + ax, 1.0))
        for (ra, ga) in ent:
            for (rb, gb) in ent:
                rows.append(ra); cols.append(rb); vals.append(c[f] * ga * gb)
    n = 6 * (label.max() + 1)
    return sp.csr_matrix((vals, (rows, cols)), shape=(n, n))


def composite_system2(lat, label, nnodes, scaling=True, torsion=True):
    Ac, fc, P, scale, pos = composite_system(lat, label, nnodes, scaling)
    if torsion:
        dA = torsion_correction(lat, label, scale, pos)
        if dA is not None:
            Ac = (Ac + dA).tocsr()
    return Ac, fc, P, scale, pos
