#include "svx/world/procgen.hpp"

#include <algorithm>

namespace svx {

namespace {

struct Rng {
  u64 s;
  u64 next() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    u64 x = s;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x;
  }
  int range(int lo, int hi) { return lo + static_cast<int>(next() % u64(hi - lo + 1)); }
};

void box(VoxelGrid& g, int x0, int x1, int y0, int y1, int z0, int z1, Vox v) {
  for (int x = x0; x < x1; ++x)
    for (int y = y0; y < y1; ++y) g.fill_column(x, y, z0, z1, v);
}

void ground(VoxelGrid& g, int x0, int x1, int y0, int y1, int depth) {
  box(g, x0, x1, y0, y1, -depth, 0, make_vox(MaterialId::Rock, true));
}

// A column / slab frame building with perimeter walls and window openings.
void building(VoxelGrid& g, Rng& rng, int ox, int oy, int bays_x, int bays_y, int storeys, int bay, int storey_h) {
  const Vox rc = make_vox(MaterialId::Rc, false);
  const int col = 3, slab = 2;
  const int X = bays_x * bay + col, Y = bays_y * bay + col;
  for (int s = 0; s < storeys; ++s) {
    const int z0 = s * (storey_h + slab);
    for (int ix = 0; ix <= bays_x; ++ix)
      for (int iy = 0; iy <= bays_y; ++iy)
        box(g, ox + ix * bay, ox + ix * bay + col, oy + iy * bay, oy + iy * bay + col, z0, z0 + storey_h, rc);
    box(g, ox, ox + X, oy, oy + Y, z0 + storey_h, z0 + storey_h + slab, rc);
    // two perimeter walls with windows
    const int wall_t = 2;
    for (int side = 0; side < 2; ++side) {
      const int yw = side == 0 ? oy : oy + Y - wall_t;
      for (int x = ox; x < ox + X; ++x)
        for (int z = z0; z < z0 + storey_h; ++z) {
          const int u = (x - ox) % bay;
          const bool window = u > col + 2 && u < bay - 3 && z > z0 + storey_h / 3 && z < z0 + storey_h - 3;
          if (window && rng.range(0, 9) < 8) continue;
          box(g, x, x + 1, yw, yw + wall_t, z, z + 1, rc);
        }
    }
  }
}

}  // namespace

ProcWorld make_procedural(const std::string& kind, u64 seed, f64 h) {
  ProcWorld w;
  VoxelGrid& g = w.grid;
  g.h = h;
  Rng rng{seed * 0x9E3779B97F4A7C15ull + 1};
  const Vox rc = make_vox(MaterialId::Rc, false);
  const Vox masonry = make_vox(MaterialId::Masonry, false);
  if (kind == "city") {
    const int blocks = 3, block = 120, street = 32;
    const int extent = blocks * (block + street) + street;
    ground(g, 0, extent, 0, extent, 4);
    for (int bx = 0; bx < blocks; ++bx)
      for (int by = 0; by < blocks; ++by) {
        const int ox = street + bx * (block + street), oy = street + by * (block + street);
        const int bays = rng.range(2, 3);
        const int storeys = rng.range(2, 5);
        const int bay = (block - 3) / bays;
        building(g, rng, ox, oy, bays, bays, storeys, std::min(bay, 36), 22);
      }
    g.lo = {0, 0, -4};
    g.hi = {extent, extent, 5 * 24 + 8};
    w.spawn_pos = {h * street / 2.0, h * street / 2.0, -0.5 * h + 0.02};
    w.spawn_dir = {1, 1, 0};
  } else if (kind == "slab") {
    // physics test: a 6 x 6 m RC slab (0.25 m) on four 2 x 2 voxel columns, 8 m up
    ground(g, 0, 96, 0, 96, 4);
    const int x0 = 24, x1 = 72, z0 = 64;
    for (int cx : {x0, x1 - 3})
      for (int cy : {x0, x1 - 3}) box(g, cx, cx + 3, cy, cy + 3, 0, z0, rc);
    box(g, x0, x1, x0, x1, z0, z0 + 2, rc);
    g.lo = {0, 0, -4};
    g.hi = {96, 96, z0 + 8};
    w.spawn_pos = {h * 4, h * 4, -0.5 * h + 0.02};
    w.spawn_dir = {1, 1, 0.3};
  } else if (kind == "chimney") {
    // physics test: a 24 m masonry chimney (1.5 m square, 0.25 m walls) on the ground
    ground(g, 0, 160, 0, 160, 4);
    const int c0 = 74, c1 = 86, H = 192;
    box(g, c0, c1, c0, c1, 0, H, masonry);
    box(g, c0 + 2, c1 - 2, c0 + 2, c1 - 2, 0, H, kAir);
    g.lo = {0, 0, -4};
    g.hi = {160, 160, H + 8};
    w.spawn_pos = {h * 20, h * 20, -0.5 * h + 0.02};
    w.spawn_dir = {1, 1, 0.3};
  } else if (kind == "bridge") {
    // physics test: a 16 m RC deck (0.5 m, 3 m wide) on two piers, 6 m up
    ground(g, 0, 176, 0, 64, 4);
    for (int px : {16, 144}) box(g, px, px + 16, 20, 44, 0, 48, rc);
    box(g, 8, 168, 20, 44, 48, 52, rc);
    g.lo = {0, 0, -4};
    g.hi = {176, 64, 60};
    w.spawn_pos = {h * 88, h * 4, -0.5 * h + 0.02};
    w.spawn_dir = {0, 1, 0.2};
  } else if (kind == "tower") {
    ground(g, 0, 160, 0, 160, 4);
    building(g, rng, 40, 40, 3, 3, 10, 26, 22);
    g.lo = {0, 0, -4};
    g.hi = {160, 160, 10 * 24 + 8};
    w.spawn_pos = {h * 16, h * 16, -0.5 * h + 0.02};
    w.spawn_dir = {1, 1, 0.2};
  } else {
    // rooms: a 3 x 2 grid of rooms (6 x 5 m) with doors, walls 3 voxels, ceiling slab 3
    const int nx = 3, ny = 2, rx = 48, ry = 40, wall = 3, height = 24, slab = 3;
    const int X = nx * (rx + wall) + wall, Y = ny * (ry + wall) + wall;
    ground(g, -8, X + 8, -8, Y + 8, 4);
    for (int i = 0; i <= nx; ++i) box(g, i * (rx + wall), i * (rx + wall) + wall, 0, Y, 0, height, rc);
    for (int j = 0; j <= ny; ++j) box(g, 0, X, j * (ry + wall), j * (ry + wall) + wall, 0, height, rc);
    // doors
    for (int i = 1; i < nx; ++i)
      for (int j = 0; j < ny; ++j) {
        const int x0 = i * (rx + wall), yc = j * (ry + wall) + wall + ry / 2;
        box(g, x0, x0 + wall, yc - 4, yc + 4, 0, 17, kAir);
      }
    for (int i = 0; i < nx; ++i)
      for (int j = 1; j < ny; ++j) {
        const int y0 = j * (ry + wall), xc = i * (rx + wall) + wall + rx / 2;
        box(g, xc - 4, xc + 4, y0, y0 + wall, 0, 17, kAir);
      }
    // ceiling slab, and a masonry partition with a lintel in the middle room
    box(g, 0, X, 0, Y, height, height + slab, rc);
    const int px = 1 * (rx + wall) + wall + rx / 2;
    box(g, px, px + 2, wall, wall + ry, 0, height, masonry);
    box(g, px, px + 2, wall + 14, wall + 26, 0, 16, kAir);
    g.lo = {-8, -8, -4};
    g.hi = {X + 8, Y + 8, height + slab};
    w.spawn_pos = {h * (wall + 8), h * (wall + ry / 2.0), -0.5 * h + 0.02};
    w.spawn_dir = {1, 0, 0};
  }
  (void)masonry;
  g.compact();
  g.mark_all_dirty();
  return w;
}

}  // namespace svx
