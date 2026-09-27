#include "svx/game/procgen.hpp"

#include <algorithm>

#include "svx/game/reinforce.hpp"

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

// Water at rest in the box (the "water" layer: svx_env's WaterSystem).
void water(VoxelGrid& g, int x0, int x1, int y0, int y1, int z0, int z1) {
  int L = g.layer_index("water");
  if (L < 0) L = g.add_layer({"water", true});
  for (int x = x0; x < x1; ++x)
    for (int y = y0; y < y1; ++y)
      for (int z = z0; z < z1; ++z) g.set_layer(L, {x, y, z}, 255);
}

// A column / slab frame building with perimeter walls and window openings.
void building(VoxelGrid& g, Rng& rng, int ox, int oy, int bays_x, int bays_y, int storeys, int bay, int storey_h) {
  const Vox rc = make_vox(MaterialId::Rc, false);
  const int col = 3, slab = 2;
  const int X = bays_x * bay + col, Y = bays_y * bay + col;
  for (int s = 0; s < storeys; ++s) {
    const int z0 = s * (storey_h + slab);
    for (int ix = 0; ix <= bays_x; ++ix)
      for (int iy = 0; iy <= bays_y; ++iy) {
        box(g, ox + ix * bay, ox + ix * bay + col, oy + iy * bay, oy + iy * bay + col, z0, z0 + storey_h, rc);
        reinforce(g, {ox + ix * bay, oy + iy * bay, z0}, {ox + ix * bay + col, oy + iy * bay + col, z0 + storey_h});
      }
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

// The yard: one of each kind of construction, to be shot at, blown up, set on fire and flooded.
void yard(VoxelGrid& g) {
  const Vox wood = make_vox(MaterialId::Wood, false), stone = make_vox(MaterialId::Stone, false);
  const Vox glass = make_vox(MaterialId::Glass, false), steel = make_vox(MaterialId::Steel, false);
  const Vox rc = make_vox(MaterialId::Rc, false), concrete = make_vox(MaterialId::Concrete, false);
  // A timber house (8 x 6 m): posts, plank walls with a door and windows, a floor of boards on
  // joists, a pitched roof of boards on rafters.
  {
    const int x0 = 24, x1 = 88, y0 = 24, y1 = 72, H = 24;
    for (int x = x0; x <= x1 - 3; x += 15)
      for (int y : {y0, y1 - 3}) box(g, x, x + 3, y, y + 3, 0, H, wood);
    for (int y = y0; y <= y1 - 3; y += 16)
      for (int x : {x0, x1 - 3}) box(g, x, x + 3, y, y + 3, 0, H, wood);
    box(g, x0, x1, y0 + 1, y0 + 2, 1, H, wood);  // plank walls
    box(g, x0, x1, y1 - 2, y1 - 1, 1, H, wood);
    box(g, x0 + 1, x0 + 2, y0, y1, 1, H, wood);
    box(g, x1 - 2, x1 - 1, y0, y1, 1, H, wood);
    box(g, x0 + 26, x0 + 34, y0, y0 + 3, 1, 17, kAir);  // door
    for (int wx : {x0 + 8, x0 + 44}) box(g, wx, wx + 10, y0, y0 + 3, 9, 17, kAir);
    for (int wy : {y0 + 12, y0 + 30}) box(g, x1 - 3, x1, wy, wy + 8, 9, 17, kAir);
    for (int y = y0 + 3; y < y1 - 3; y += 8) box(g, x0 + 3, x1 - 3, y, y + 2, 0, 1, wood);  // joists
    box(g, x0 + 2, x1 - 2, y0 + 2, y1 - 2, 1, 2, wood);                                   // floor boards
    for (int x = x0; x < x1; x += 8) box(g, x, x + 2, y0, y1, H, H + 1, wood);          // tie beams
    for (int k = 0; k < 12; ++k) {                                                          // rafters and boards
      const int z = H + 1 + k;
      box(g, x0 - 2, x1 + 2, y0 - 2 + 2 * k, y0 + 2 * k, z, z + 1, wood);
      box(g, x0 - 2, x1 + 2, y1 - 2 * k, y1 + 2 - 2 * k, z, z + 1, wood);
    }
  }
  // A stone tower (5 m square, walls 0.5 m, 11 m high) with windows and a door.
  {
    const int x0 = 150, x1 = 190, y0 = 30, y1 = 70, H = 88;
    box(g, x0, x1, y0, y1, 0, H, stone);
    box(g, x0 + 4, x1 - 4, y0 + 4, y1 - 4, 0, H, kAir);
    box(g, x0 + 16, x0 + 24, y0, y0 + 4, 0, 18, kAir);
    for (int z = 24; z < H - 12; z += 20)
      for (int side = 0; side < 4; ++side) {
        const int c = side < 2 ? x0 + 18 : y0 + 18;
        if (side == 0) box(g, c, c + 4, y0, y0 + 4, z, z + 8, kAir);
        if (side == 1) box(g, c, c + 4, y1 - 4, y1, z, z + 8, kAir);
        if (side == 2) box(g, x0, x0 + 4, c, c + 4, z, z + 8, kAir);
        if (side == 3) box(g, x1 - 4, x1, c, c + 4, z, z + 8, kAir);
      }
    for (int x = x0; x < x1; x += 8) box(g, x, x + 4, y0, y1, H, H + 4, stone);  // crenellations
    box(g, x0 + 4, x1 - 4, y0 + 4, y1 - 4, H, H + 4, kAir);
    box(g, x0 + 4, x1 - 4, y0 + 4, y1 - 4, 48, 50, wood);                            // a timber floor
  }
  // A greenhouse: a steel frame glazed on its sides and roof.
  {
    const int x0 = 36, x1 = 100, y0 = 140, y1 = 196, H = 28;
    for (int x = x0; x <= x1 - 2; x += 16)
      for (int y = y0; y <= y1 - 2; y += 14) box(g, x, x + 2, y, y + 2, 0, H, steel);
    for (int x = x0; x <= x1 - 2; x += 16) box(g, x, x + 2, y0, y1, H, H + 2, steel);
    for (int y = y0; y <= y1 - 2; y += 14) box(g, x0, x1, y, y + 2, H, H + 2, steel);
    box(g, x0, x1, y0, y0 + 1, 1, H, glass);
    box(g, x0, x1, y1 - 1, y1, 1, H, glass);
    box(g, x0, x0 + 1, y0, y1, 1, H, glass);
    box(g, x1 - 1, x1, y0, y1, 1, H, glass);
    box(g, x0 + 1, x1 - 1, y0 + 1, y1 - 1, H + 1, H + 2, glass);
    box(g, x0, x1, y0, y1, 0, 1, concrete);  // (a concrete kerb)
    box(g, x0 + 30, x0 + 38, y0, y0 + 1, 1, 17, kAir);
  }
  // A steel shed: portal frames (columns and roof beams) carrying a thin concrete roof.
  {
    const int x0 = 150, x1 = 214, y0 = 140, y1 = 196, H = 40;
    for (int x = x0; x <= x1 - 2; x += 16) {
      for (int y : {y0, y1 - 2}) box(g, x, x + 2, y, y + 2, 0, H, steel);
      box(g, x, x + 2, y0, y1, H, H + 3, steel);
    }
    box(g, x0, x1, y0, y1, H + 3, H + 5, concrete);
  }
  // A reinforced concrete wall (7.5 m x 3 m, 0.375 m thick) with its bars.
  box(g, 240, 300, 40, 43, 0, 24, rc);
  reinforce(g, {240, 40, 0}, {300, 43, 24});
  // A reservoir: masonry walls (0.25 m) holding 2 m of water, 8 m square.
  {
    const Vox masonry = make_vox(MaterialId::Masonry, false);
    const int x0 = 236, x1 = 300, y0 = 150, y1 = 214;
    box(g, x0, x1, y0, y1, 0, 20, masonry);
    box(g, x0 + 2, x1 - 2, y0 + 2, y1 - 2, 0, 20, kAir);
    water(g, x0 + 2, x1 - 2, y0 + 2, y1 - 2, 0, 16);
  }
  // A water tower: a timber tank (2 m square, 1.5 m of water) on four posts, 4 m up.
  {
    const int x0 = 250, x1 = 266, y0 = 92, y1 = 108, z0 = 32;
    for (int x : {x0, x1 - 3})
      for (int y : {y0, y1 - 3}) box(g, x, x + 3, y, y + 3, 0, z0, wood);
    box(g, x0 - 1, x1 + 1, y0 - 1, y1 + 1, z0, z0 + 2, wood);  // platform
    box(g, x0, x1, y0, y1, z0 + 2, z0 + 16, wood);             // tank
    box(g, x0 + 1, x1 - 1, y0 + 1, y1 - 1, z0 + 2, z0 + 16, kAir);
    water(g, x0 + 1, x1 - 1, y0 + 1, y1 - 1, z0 + 2, z0 + 14);
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
  } else if (kind == "yard") {
    ground(g, 0, 320, 0, 240, 4);
    yard(g);
    g.lo = {0, 0, -4};
    g.hi = {320, 240, 112};
    w.spawn_pos = {h * 120, h * 110, -0.5 * h + 0.02};
    w.spawn_dir = {0, -1, 0.1};
  } else if (kind == "slab") {
    // physics test: a 6 x 6 m RC slab (0.25 m) on four 2 x 2 voxel columns, 8 m up
    ground(g, 0, 96, 0, 96, 4);
    const int x0 = 24, x1 = 72, z0 = 64;
    for (int cx : {x0, x1 - 3})
      for (int cy : {x0, x1 - 3}) {
        box(g, cx, cx + 3, cy, cy + 3, 0, z0, rc);
        reinforce(g, {cx, cy, 0}, {cx + 3, cy + 3, z0});
      }
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
    for (int px : {16, 144}) {
      box(g, px, px + 16, 20, 44, 0, 48, rc);
      reinforce(g, {px, 20, 0}, {px + 16, 44, 48});
    }
    box(g, 8, 168, 20, 44, 48, 52, rc);
    reinforce(g, {8, 20, 48}, {168, 44, 52});
    g.lo = {0, 0, -4};
    g.hi = {176, 64, 60};
    w.spawn_pos = {h * 88, h * 4, -0.5 * h + 0.02};
    w.spawn_dir = {0, 1, 0.2};
  } else if (kind == "tower") {
    ground(g, -160, 320, -160, 320, 4);  // (60 m square: room for the rubble)
    building(g, rng, 40, 40, 3, 3, 10, 26, 22);
    g.lo = {-160, -160, -4};
    g.hi = {320, 320, 10 * 24 + 8};
    w.spawn_pos = {h * 16, h * 16, -0.5 * h + 0.02};
    w.spawn_dir = {1, 1, 0.2};
  } else {
    // rooms: a 3 x 2 grid of rooms (6 x 5 m) with doors, walls 3 voxels, ceiling slab 3
    const int nx = 3, ny = 2, rx = 48, ry = 40, wall = 3, height = 24, slab = 3;
    const int X = nx * (rx + wall) + wall, Y = ny * (ry + wall) + wall;
    ground(g, -8, X + 8, -8, Y + 8, 4);
    for (int i = 0; i <= nx; ++i) box(g, i * (rx + wall), i * (rx + wall) + wall, 0, Y, 0, height, rc);
    for (int j = 0; j <= ny; ++j) box(g, 0, X, j * (ry + wall), j * (ry + wall) + wall, 0, height, rc);
    for (int i = 0; i <= nx; ++i) reinforce(g, {i * (rx + wall), 0, 0}, {i * (rx + wall) + wall, Y, height});
    for (int j = 0; j <= ny; ++j) reinforce(g, {0, j * (ry + wall), 0}, {X, j * (ry + wall) + wall, height});
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
    reinforce(g, {0, 0, height}, {X, Y, height + slab});
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
