"""Test structures on the voxel lattice."""
import numpy as np


def building(nx_bays=3, ny_bays=3, stories=2, bay=12, story_h=10, slab_t=2, col=2, walls=True):
    """Column + slab frame building on an anchored ground layer (z=0)."""
    X = nx_bays * bay + col + 2
    Y = ny_bays * bay + col + 2
    Z = stories * (story_h + slab_t) + 1
    solid = np.zeros((X, Y, Z), bool)
    anchor = np.zeros_like(solid)
    solid[:, :, 0] = True
    anchor[:, :, 0] = True
    cols = []
    for s in range(stories):
        z0 = 1 + s * (story_h + slab_t)
        for ix in range(nx_bays + 1):
            for iy in range(ny_bays + 1):
                x0 = 1 + ix * bay
                y0 = 1 + iy * bay
                solid[x0:x0 + col, y0:y0 + col, z0:z0 + story_h] = True
                cols.append((s, ix, iy, x0, y0, z0))
        zs = z0 + story_h
        solid[1:1 + nx_bays * bay + col, 1:1 + ny_bays * bay + col, zs:zs + slab_t] = True
        if walls and s == stories - 1:
            # perimeter wall along y = 1 (thickness 1) with a door
            solid[1:1 + nx_bays * bay + col, 1, z0:z0 + story_h] = True
            solid[1 + bay // 2:1 + bay // 2 + 3, 1, z0:z0 + 6] = False
    return solid, anchor, cols


def remove_column(solid, col_rec, col=2, story_h=10):
    s, ix, iy, x0, y0, z0 = col_rec
    out = solid.copy()
    out[x0:x0 + col, y0:y0 + col, z0:z0 + story_h] = False
    center = np.array([x0 + (col - 1) / 2, y0 + (col - 1) / 2, z0 + story_h - 1], float)
    return out, center


def carve_sphere(solid, anchor, center, radius):
    X, Y, Z = solid.shape
    g = np.stack(np.meshgrid(np.arange(X), np.arange(Y), np.arange(Z), indexing='ij'), -1)
    m = np.linalg.norm(g - center, axis=-1) <= radius
    out = solid.copy()
    out[m & ~anchor] = False
    return out
