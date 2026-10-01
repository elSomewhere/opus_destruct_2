#include "svx/procgen/levels.hpp"

#include <algorithm>
#include <cmath>

#include "svx/procgen/reinforce.hpp"

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
  if (L < 0) L = g.add_layer({"water", true, LayerBind::Air});
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
  // (steel members as rolled sections smeared over their voxels - docs/VEHICLES.md: a 2 x 2 voxel
  // column is an HEB 200, not 490 kg/m of solid steel)
  const Vox glass = make_vox(MaterialId::Glass, false), steel = make_vox(MaterialId::SteelSection, false);
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

// A turn of deg degrees about the unit axis (x, y, z).
Quat turn(f64 deg, f64 x, f64 y, f64 z) {
  const f64 th = 0.5 * deg * 3.14159265358979323846 / 180.0, s = std::sin(th);
  return Quat{x * s, y * s, z * s, std::cos(th)};
}

// Structures off the lattice, each in a grid of its own (docs/GRIDS.md): what they stand on or are
// cast into is the world grid's (or another grid's), their joints are the grids' junctions.
void angles(Level& w, Rng& rng) {
  VoxelGrid& g = w.grid;
  const f64 h = g.h;
  const Vox rc = make_vox(MaterialId::Rc, false), steel = make_vox(MaterialId::SteelSection, false);  // (sections)
  const Vox masonry = make_vox(MaterialId::Masonry, false), wood = make_vox(MaterialId::Wood, false);
  const Vox stone = make_vox(MaterialId::Stone, false);
  // (a grid at world voxel coordinates o, turned by r: its voxel p's centre is h (o + R p))
  auto place = [&](const V3& o, const Quat& r) -> VoxelGrid& {
    LevelGrid pg;
    pg.frame = GridFrame{V3{h * o.x, h * o.y, h * o.z}, r};
    pg.grid.h = h;
    w.grids.push_back(std::move(pg));
    return w.grids.back().grid;
  };
  // A frame building (2 x 2 bays, 5 storeys) turned 30 degrees about the vertical.
  building(place({80, 200, 0}, turn(30, 0, 0, 1)), rng, -27, -27, 2, 2, 5, 26, 22);
  // A bridge deck (17 m, 3 m wide, 0.5 m) running diagonally, cast 2 voxels into the tops of two
  // piers of the world grid.
  for (const auto& [px, py] : {std::pair{216, 160}, std::pair{308, 224}}) {
    box(g, px, px + 16, py, py + 16, 0, 50, rc);
    reinforce(g, {px, py, 0}, {px + 16, py + 16, 50});
  }
  {
    VoxelGrid& d = place({270, 200, 48}, turn(35, 0, 0, 1));
    box(d, -68, 68, -12, 12, 0, 4, rc);
    reinforce(d, {-68, -12, 0}, {68, 12, 4});
  }
  // A ramp (9 m, 3 m wide) rising at 15 degrees from the ground (its foot a voxel into it) onto a
  // block of the world grid.
  box(g, 100, 112, 48, 72, 0, 18, rc);
  reinforce(g, {100, 48, 0}, {112, 72, 18});
  {
    VoxelGrid& r = place({40, 60, -1}, turn(-15, 0, 1, 0));
    box(r, 0, 72, -12, 12, 0, 3, rc);
    reinforce(r, {0, -12, 0}, {72, 12, 3});
  }
  // A steel portal (6 m span, 5.5 m high) with a cross brace: two diagonal bars in grids of their
  // own crossing at mid-height, each running from low on one column to high on the other through
  // both (welded: a junction is as strong as the faces that overlap).
  for (int x : {160, 208}) box(g, x, x + 3, 40, 43, 0, 44, steel);
  box(g, 160, 211, 40, 43, 44, 47, steel);
  const f64 brace = std::atan2(36.0, 51.0) * 180.0 / 3.14159265358979323846;
  for (f64 a : {-brace, brace}) box(place({185, 41, 22}, turn(a, 0, 1, 0)), -32, 32, -1, 2, -1, 2, steel);
  // Masonry walls (8 m, 3 m high, 3 voxels thick) turned 20 degrees (with a doorway under a
  // masonry lintel) and 45 degrees.
  {
    VoxelGrid& m = place({290, 50, 0}, turn(20, 0, 0, 1));
    box(m, -32, 32, -1, 2, 0, 24, masonry);
    box(m, -4, 4, -1, 2, 0, 16, kAir);
  }
  box(place({290, 110, 0}, turn(45, 0, 0, 1)), -32, 32, -1, 2, 0, 24, masonry);
  // Timber crates (1 m, walls one voxel) at yaws of their own, one stacked on another.
  const std::array<std::array<f64, 4>, 5> crates{{{170, 120, 0, 10}, {190, 126, 0, 25}, {212, 118, 0, 40}, {182, 144, 0, 55},
                                                  {170, 120, 8, 50}}};
  for (const auto& c : crates) {
    VoxelGrid& b = place({c[0], c[1], c[2]}, turn(c[3], 0, 0, 1));
    box(b, -4, 4, -4, 4, 0, 8, wood);
    box(b, -3, 3, -3, 3, 1, 7, kAir);
  }
  // A stone monolith (1 x 0.5 x 5 m) leaning 12 degrees, its foot 2 voxels in the ground.
  box(place({350, 140, 0}, turn(30, 0, 0, 1) * turn(12, 1, 0, 0)), -4, 4, -2, 2, -2, 40, stone);
}

// Machines (docs/MOTION.md): free parts of the level on joints to structures - driven ones (a
// lift's car, a turntable, a drawbridge, a crane's jib), and ones that hang or swing (a wrecking
// ball, a pendulum's bob, a chain, a door). Every joint holds on to a voxel of a structure, which
// carries the machine (its weight, its drive's reaction) and can be shot away: the machine comes
// down with it. The free parts are grids of the level the bake keeps (they are held): pieces from
// the first tick. (Each keeps a voxel's gap to what it moves along: it bonds to nothing.)
void machines(Level& w) {
  VoxelGrid& g = w.grid;
  const f64 h = g.h;
  constexpr f64 kPi = 3.14159265358979323846;
  const Vox rc = make_vox(MaterialId::Rc, false), conc = make_vox(MaterialId::Concrete, false);
  const Vox steel = make_vox(MaterialId::Steel, false), wood = make_vox(MaterialId::Wood, false);
  const Vox masonry = make_vox(MaterialId::Masonry, false);
  const Quat id{0, 0, 0, 1};
  // (a free part of the level: a grid on the world's lattice, voxel p of it at world voxel o + p;
  // its grid id: the order it is added in)
  auto part = [&](const IVec3& o) -> std::pair<VoxelGrid&, GridId> {
    LevelGrid pg;
    pg.frame = GridFrame{V3{h * o[0], h * o[1], h * o[2]}, id};
    pg.grid.h = h;
    w.grids.push_back(std::move(pg));
    return {w.grids.back().grid, static_cast<GridId>(w.grids.size())};
  };
  auto joint = [&](JointType type, JointAnchor a, JointAnchor b) -> JointDesc& {
    JointDesc d;
    d.type = type;
    d.a = a;
    d.b = b;
    w.joints.push_back(d);
    return w.joints.back();
  };
  // (an end on a voxel of a grid: at world point p, in the voxel or next to it)
  auto at = [](GridId grid, const V3& p) {
    JointAnchor a;
    a.kind = JointAnchor::Kind::Grid;
    a.id = grid;
    a.point = p;
    return a;
  };
  auto wp = [&](f64 x, f64 y, f64 z) { return V3{h * x, h * y, h * z}; };  // (world voxel coordinates, metres)

  // A lift beside a reinforced concrete tower (3 x 3 m, 5 m) at (13..16, 5..8): a timber car
  // (2.5 m square) on a slider held by the tower's face, rising to its top and down again every
  // 12 s.
  box(g, 104, 128, 40, 64, 0, 40, rc);
  reinforce(g, {104, 40, 0}, {128, 64, 40});
  {
    auto [car, cid] = part({129, 42, 1});
    box(car, 0, 20, 0, 20, 0, 2, wood);
    const V3 p = wp(127.5, 52, 1.5);  // (the tower's face)
    JointDesc& d = joint(JointType::Slider, at(kWorldGrid, p), at(cid, p));
    d.axis = V3{0, 0, 1};
    d.limited = true;
    d.lower = -0.05;
    d.upper = 4.6;
    d.drive.kind = JointDrive::Kind::Oscillate;
    d.drive.target = 0.0;
    d.drive.target2 = 4.5;
    d.drive.period = 12.0;
    d.drive.speed = 1.5;
    d.drive.max = 30000.0;
  }
  // A turntable: a timber disc (3 m radius) at (30, 8) turning at 0.4 rad/s on a hinge held by a
  // concrete pedestal (1 m square) under its centre; crates dropped on it. (Its motor's reaction
  // twists the pedestal: a pedestal too small for it shears off.)
  box(g, 236, 244, 60, 68, 0, 2, rc);
  {
    auto [disc, did] = part({240, 64, 3});
    for (int x = -24; x < 24; ++x)
      for (int y = -24; y < 24; ++y)
        if ((x + 0.5) * (x + 0.5) + (y + 0.5) * (y + 0.5) < 24.0 * 24.0) disc.fill_column(x, y, 0, 2, wood);
    const V3 p = wp(239.5, 63.5, 1.5);  // (on its axis, the pedestal's top)
    JointDesc& d = joint(JointType::Hinge, at(kWorldGrid, p), at(did, p));
    d.axis = V3{0, 0, 1};
    d.drive.kind = JointDrive::Kind::Speed;
    d.drive.speed = 0.4;
    d.drive.max = 8000.0;
    for (int k = 0; k < 3; ++k) {
      const f64 a = 2.1 * k, r = 1.6;
      Drop c;
      c.desc.frame = GridFrame{V3{h * 239.5 + r * std::cos(a) - 1.5 * h, h * 63.5 + r * std::sin(a) - 1.5 * h, h * 5.5 + 0.5 * h + 0.02}, id};
      c.desc.base = false;
      c.voxels.h = h;
      box(c.voxels, 0, 4, 0, 4, 0, 4, wood);
      w.drops.push_back(std::move(c));
    }
  }
  // A drawbridge over a 4.9 m gap between two concrete abutments at (39..42) and (47..50): a
  // timber deck (5.4 m, 2 m wide) on a hinge in a steel seat set into the near abutment's edge
  // (a hinge on the concrete's corner would tear it out), raised 70 degrees and lowered again
  // every 16 s; lowered, it reaches over the far abutment.
  box(g, 312, 336, 40, 64, 0, 16, conc);
  box(g, 332, 336, 44, 60, 12, 16, steel);
  box(g, 376, 400, 40, 64, 0, 16, conc);
  {
    auto [deck, kid] = part({337, 44, 17});
    box(deck, 0, 43, 0, 16, 0, 2, wood);
    const V3 p = wp(336, 52, 16);  // (between the abutment's edge and the deck's)
    JointDesc& d = joint(JointType::Hinge, at(kWorldGrid, p), at(kid, p));
    d.axis = V3{0, -1, 0};  // (raising it: a positive turn)
    d.drive.kind = JointDrive::Kind::Oscillate;
    d.drive.target = 0.0;
    d.drive.target2 = 1.2;
    d.drive.period = 16.0;
    d.drive.speed = 0.6;
    d.drive.max = 80000.0;
  }
  // A crane: a reinforced concrete mast (1 m square, 8 m) at (12, 28) with a steel cap its jib's
  // hinge is seated in (the cap spreads the jib's moment over the mast's section: a hinge in the
  // concrete would tear its fragment out), and on it a steel jib (5.5 m), swinging 125 degrees
  // and back every 10 s; from its tip a 1 t steel ball on a 5 m wire rope (it stretches a little:
  // a shock reaches the jib over the time it takes), into a masonry wall (a grid of its own
  // across the ball's path, 60 degrees round).
  box(g, 92, 100, 220, 228, 0, 62, rc);
  reinforce(g, {92, 220, 0}, {100, 228, 62});
  box(g, 92, 100, 220, 228, 62, 64, steel);
  {
    auto [jib, jid] = part({94, 223, 65});
    box(jib, 0, 44, 0, 2, 0, 2, make_vox(MaterialId::SteelSection, false));  // (a rolled section: 0.3 t, not 2.7)
    const V3 p = wp(95.5, 223.5, 64);  // (on the mast's axis, its top)
    JointDesc& d = joint(JointType::Hinge, at(kWorldGrid, p), at(jid, p));
    d.axis = V3{0, 0, 1};
    d.drive.kind = JointDrive::Kind::Oscillate;
    d.drive.target = 0.0;
    d.drive.target2 = 2.2;
    d.drive.period = 10.0;
    d.drive.speed = 1.5;
    d.drive.max = 150000.0;
    auto [ball, bid] = part({135, 222, 21});
    box(ball, 0, 4, 0, 4, 0, 4, steel);
    JointDesc& rope = joint(JointType::Distance, at(jid, wp(136.5, 223.5, 64.5)), at(bid, wp(136.5, 223.5, 24.5)));
    rope.stiffness = 1e6;
    rope.damping = 2e4;
    const f64 a = 60.0 * kPi / 180.0, r = 5.2;
    LevelGrid pg;
    pg.frame = GridFrame{V3{h * 95.5 + r * std::cos(a), h * 223.5 + r * std::sin(a), 0.0}, Quat{0.0, 0.0, std::sin(0.5 * a), std::cos(0.5 * a)}};
    pg.grid.h = h;
    box(pg.grid, -12, 12, -1, 1, 0, 32, masonry);
    w.grids.push_back(std::move(pg));
  }
  // A pendulum: a steel bob (0.4 t) on a 3 m rod from the beam of a timber frame at
  // (27..34.5, 28), let go 60 degrees out.
  box(g, 216, 220, 222, 226, 0, 44, wood);
  box(g, 272, 276, 222, 226, 0, 44, wood);
  box(g, 216, 276, 222, 226, 44, 48, wood);
  const V3 pivot = wp(245.5, 223.5, 43.5);  // (the beam's underside)
  {
    const f64 L = 3.0, th = 60.0 * kPi / 180.0;
    const V3 bob{pivot.x + L * std::sin(th), pivot.y, pivot.z - L * std::cos(th)};
    LevelGrid pg;
    pg.frame = GridFrame{bob, id};
    pg.grid.h = h;
    box(pg.grid, -1, 2, -1, 2, -1, 2, steel);
    w.grids.push_back(std::move(pg));
    JointDesc& rod = joint(JointType::Distance, at(kWorldGrid, pivot), at(static_cast<GridId>(w.grids.size()), bob));
    rod.rope = false;
  }
  // A chain of four wooden links, 1 m apart, on rods from the arm of a timber gallows at
  // (36..39, 28).
  box(g, 288, 292, 222, 226, 0, 48, wood);
  box(g, 288, 312, 222, 226, 44, 48, wood);
  {
    const V3 hook{h * 306, pivot.y, pivot.z};
    GridId above = kWorldGrid;
    V3 up = hook;
    for (int k = 0; k < 4; ++k) {
      const V3 c{hook.x, hook.y, hook.z - 0.75 - 1.0 * k};
      LevelGrid pg;
      pg.frame = GridFrame{c, id};
      pg.grid.h = h;
      box(pg.grid, -2, 2, -2, 2, -2, 2, wood);
      w.grids.push_back(std::move(pg));
      const GridId link = static_cast<GridId>(w.grids.size());
      JointDesc& rod = joint(JointType::Distance, at(above, up), at(link, V3{c.x, c.y, c.z + 1.5 * h}));
      rod.rope = false;
      above = link;
      up = V3{c.x, c.y, c.z - 2.5 * h};
    }
  }
  // A wooden door on a hinge in the doorway of a brick wall at (45, 28): its leaf clear of the
  // wall all round (bonded to nothing, it swings).
  box(g, 344, 400, 222, 225, 0, 24, masonry);
  box(g, 364, 372, 222, 225, 0, 17, kAir);
  {
    auto [leaf, lid] = part({365, 223, 1});
    box(leaf, 0, 6, 0, 1, 0, 15, wood);
    const V3 p = wp(364, 223, 8);  // (between the jamb and the leaf)
    JointDesc& d = joint(JointType::Hinge, at(kWorldGrid, p), at(lid, p));
    d.axis = V3{0, 0, 1};
    d.limited = true;
    d.lower = -1.6;
    d.upper = 1.6;
  }
}

}  // namespace

Level make_procedural(const std::string& kind, u64 seed, f64 h) {
  Level w;
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
  } else if (kind == "machines") {
    ground(g, 0, 480, 0, 320, 4);
    machines(w);
    g.lo = {0, 0, -4};
    g.hi = {480, 320, 96};
    w.spawn_pos = {h * 32, h * 128, -0.5 * h + 0.02};
    w.spawn_dir = {1, 0, 0.05};
  } else if (kind == "angles") {
    ground(g, 0, 384, 0, 288, 4);
    angles(w, rng);
    g.lo = {0, 0, -4};
    g.hi = {384, 288, 128};
    w.spawn_pos = {h * 190, h * 16, -0.5 * h + 0.02};
    w.spawn_dir = {0, 1, 0.1};
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
