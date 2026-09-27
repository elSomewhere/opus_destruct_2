"""
Experiment: event = remove an interior ground-floor column of a 2-storey frame building.
Compare, against the full fine reference solve:
  - FIXED:  prototype-style fixed bubble, kinematic boundary pinned at pre-event pose
  - COMP:   telescoping composite (fine within R0, rigid aggregates coarsening outward,
            WHOLE structure included), with and without series-length bond scaling
  - COARSE: whole structure at a coarse level only (no fine region)
"""
import sys
import time
import numpy as np
import scipy.sparse.linalg as spla
from lattice import Lattice, stress_measure, resultant
from structures import building, remove_column
from hierarchy import composite_partition, composite_system

np.set_printoptions(precision=3, suppress=True, linewidth=160)

NXB = int(sys.argv[1]) if len(sys.argv) > 1 else 3
solid0, anchor, cols = building(nx_bays=NXB, ny_bays=NXB, stories=2)
target = [c for c in cols if c[0] == 0 and c[1] == 1 and c[2] == 1][0]
solid1, center = remove_column(solid0, target)

pre = Lattice(solid0, anchor)
post = Lattice(solid1, anchor)
print(f"cells pre={pre.n} post={post.n} free dofs post={6 * len(post.free)} bonds={post.nb}")

t = time.time()
u_pre = pre.full_u(spla.spsolve(pre.K_free().tocsc(), pre.gravity_free()))
u_ref = post.full_u(spla.spsolve(post.K_free().tocsc(), post.gravity_free()))
print(f"reference solves {time.time() - t:.2f}s")
F_ref = post.bond_forces(u_ref)
F_pre = pre.bond_forces(u_pre)


def column_loads(lat, F):
    out = []
    for (s, ix, iy, x0, y0, z0) in cols:
        if s != 0:
            continue
        zc = z0 + 4
        b = lat.bonds
        c0 = lat.coords[b[:, 0]]
        m = (b[:, 2] == 2) & (c0[:, 2] == zc) & (c0[:, 0] >= x0) & (c0[:, 0] < x0 + 2) & (c0[:, 1] >= y0) & (c0[:, 1] < y0 + 2)
        out.append(F[m, 0].sum() if m.any() else 0.0)
    return np.array(out)


# ---- map post cells -> pre cells (by coordinate) to pin the fixed-bubble boundary ----
pre_idx = pre.cid[tuple(post.coords.T)]
u_pre_on_post = u_pre.reshape(-1, 6)[pre_idx].ravel()


def fixed_bubble(RB):
    lat = post
    K = lat.K_free()
    fd = lat.free_dofs()
    ffree = lat.free
    d = np.linalg.norm(lat.coords[ffree] - center, axis=1)
    inb = d <= RB
    bdofs = (np.where(inb)[0][:, None] * 6 + np.arange(6)).ravel()
    odofs = (np.where(~inb)[0][:, None] * 6 + np.arange(6)).ravel()
    uo = u_pre_on_post[fd][odofs]
    f = lat.gravity_free()
    rhs = f[bdofs] - K[bdofs][:, odofs] @ uo
    ub = spla.spsolve(K[bdofs][:, bdofs].tocsc(), rhs)
    uf = np.zeros(len(fd))
    uf[bdofs] = ub
    uf[odofs] = uo
    return lat.full_u(uf), int(inb.sum())


def composite(R0, grading=2.0, Lmax=3, scaling=True):
    lat = post
    label, level, nn = composite_partition(lat, center, R0, grading, Lmax)
    Ac, fc, P, scale, _ = composite_system(lat, label, nn, scaling)
    uc = spla.spsolve(Ac.tocsc(), fc)
    uf = P @ uc
    return lat.full_u(uf), nn, level, scale


def region_errors(F, scale_used=None, R=8.0):
    """Stress errors on fine bonds within R of the event (fine-fine bonds only)."""
    p = post.bond_points()
    m = np.linalg.norm(p - center, axis=1) <= R
    st_r = np.stack(stress_measure(F_ref[m]))
    st_x = np.stack(stress_measure(F[m]))
    peak = np.abs(st_r).max(axis=1)
    err = np.abs(st_x - st_r).max(axis=1) / peak
    return err, peak, np.abs(st_x).max(axis=1)


cl_pre = column_loads(pre, F_pre)
cl_ref = column_loads(post, F_ref)
print("\nStorey-1 column axial loads (compression negative), row-major over (ix,iy):")
print("pre   :", cl_pre)
print("ref   :", cl_ref)

for RB in (8, 12, 16, 24):
    u, nb = fixed_bubble(RB)
    F = post.bond_forces(u)
    err, peak, got = region_errors(F, R=min(RB, 8))
    cl = column_loads(post, F)
    print(f"\nFIXED bubble R={RB:2d} ({nb} free cells): fine-region stress err (sig_t, sig_c, tau) rel-to-peak = {err}")
    print(f"   peaks ref={peak} got={got}")
    print("   col loads:", cl, " max|err|/max|ref| =", f"{np.abs(cl - cl_ref).max() / np.abs(cl_ref).max():.3f}")

for scaling in (False, True):
    for R0, Lmax, gr in ((6, 3, 2.0), (8, 3, 2.0), (8, 4, 2.0), (8, 3, 1.5), (12, 3, 2.0)):
        t = time.time()
        u, nn, level, scale = composite(R0, gr, Lmax, scaling)
        dt = time.time() - t
        F = post.bond_forces(u, scale)
        err, peak, got = region_errors(F, R=R0)
        cl = column_loads(post, F)
        counts = np.bincount(level, minlength=Lmax + 1)
        print(f"\nCOMP scaling={scaling} R0={R0} Lmax={Lmax} grading={gr}: nodes={nn} per-level={counts} ({dt:.2f}s)")
        print(f"   fine-region stress err rel-to-peak = {err}; peaks ref={peak} got={got}")
        print("   col loads:", cl, " max|err|/max|ref| =", f"{np.abs(cl - cl_ref).max() / np.abs(cl_ref).max():.3f}")

for scaling in (False, True):
    for Lmax in (2, 3):
        u, nn, level, scale = composite(1e-9, 2.0, Lmax, scaling)
        F = post.bond_forces(u, scale)
        cl = column_loads(post, F)
        print(f"\nCOARSE-ONLY L={Lmax} scaling={scaling}: nodes={nn}; col loads:", cl,
              " max|err|/max|ref| =", f"{np.abs(cl - cl_ref).max() / np.abs(cl_ref).max():.3f}")
