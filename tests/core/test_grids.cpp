// Oriented grids (docs/GRIDS.md): voxel lattices placed in the world with frames of their own,
// bonded to each other and to the world grid by junctions; pieces made of several grids.
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "doctest.h"
#include "svx/base/parallel.hpp"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

constexpr f64 kPi = 3.14159265358979323846;

Quat axis_angle(const V3& axis, f64 deg) {
  const V3 a = normalized(axis);
  const f64 t = 0.5 * deg * kPi / 180.0;
  const f64 s = std::sin(t);
  return Quat{a.x * s, a.y * s, a.z * s, std::cos(t)};
}
Quat yaw(f64 deg) { return axis_angle(V3{0, 0, 1}, deg); }

void box(VoxelGrid& g, const IVec3& lo, const IVec3& hi, Vox v) {
  for (i32 x = lo[0]; x < hi[0]; ++x)
    for (i32 y = lo[1]; y < hi[1]; ++y) g.fill_column(x, y, lo[2], hi[2], v);
}

// An anchored rock ground (z < 0) of half-width `half` voxels.
VoxelGrid ground(i32 half = 64) {
  VoxelGrid g;
  g.h = 0.125;
  box(g, {-half, -half, -4}, {half, half, 0}, make_vox(MaterialId::Rock, true));
  g.compact();
  g.lo = {-half, -half, -4};
  g.hi = {half, half, 64};
  return g;
}

f64 qdot(const Quat& a, const Quat& b) { return std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w); }

// A cantilever: an anchored pier [0, 8) x [0, 8) x [0, 24) and a concrete beam from it along +x,
// [8, 8 + len) x [2, 6) x [16, 22).
void cantilever(VoxelGrid& g, i32 len) {
  box(g, {0, 0, 0}, {8, 8, 24}, make_vox(MaterialId::Rock, true));
  box(g, {8, 2, 16}, {8 + len, 6, 22}, make_vox(MaterialId::Concrete, false));
}

}  // namespace

TEST_CASE("grids: a structure in a grid turned about z carries its weight exactly as in the world grid") {
  // the same cantilever, once in the world grid, once in a grid turned 30 degrees (and moved):
  // gravity is along both lattices' z, so the solve is the same up to rounding
  f64 phi[2];
  i32 over[2];
  for (int k = 0; k < 2; ++k) {
    World w;
    VoxelGrid g = ground();
    VoxelGrid c;
    c.h = g.h;
    cantilever(k == 0 ? g : c, 24);
    g.compact();
    c.compact();
    w.load(std::move(g));
    GridId id = kWorldGrid;
    if (k == 1) {
      id = w.add_grid(GridFrame{V3{3.2, -1.7, 0.0}, yaw(30.0)}, std::move(c));
      REQUIRE(id != 0);
    }
    phi[k] = w.probe_utilization(id, IVec3{28, 3, 18}, &over[k]);
  }
  MESSAGE("cantilever: world grid phi " << phi[0] << " (" << over[0] << " over), turned grid " << phi[1] << " (" << over[1] << ")");
  CHECK(phi[0] > 0.05);
  CHECK(phi[1] == doctest::Approx(phi[0]).epsilon(1e-6));
  CHECK(over[0] == over[1]);
}

TEST_CASE("grids: a turned wall stands on the world grid through its junctions, and falls as a piece of its grid") {
  World w;
  w.load(ground());
  VoxelGrid wall;
  wall.h = 0.125;
  box(wall, {-16, -2, 0}, {16, 2, 24}, make_vox(MaterialId::Masonry, false));  // 4 m x 0.5 m x 3 m
  wall.compact();
  const Quat rot = yaw(35.0);
  const GridId id = w.add_grid(GridFrame{V3{0.3, 0.2, 0.0}, rot}, std::move(wall));
  REQUIRE(id != 0);
  w.bake();
  CHECK(w.design_report().floating_voxels == 0);  // (held by the ground: not floating)
  for (int t = 0; t < 60; ++t) w.tick();
  CHECK(w.pieces().empty());
  const f64 phi = w.probe_utilization(id, IVec3{0, 0, 12});
  MESSAGE("turned wall: max utilization " << phi);
  CHECK(phi >= 0.0);
  CHECK(phi < 1.0);
  // its bottom two rows go: the rest reaches no support and falls, a piece of its grid
  std::vector<VoxelEdit> cut;
  for (i32 x = -16; x < 16; ++x)
    for (i32 y = -2; y < 2; ++y)
      for (i32 z = 0; z < 2; ++z) cut.push_back({{x, y, z}, kAir});
  CHECK(w.set_voxels(id, cut) == static_cast<i32>(cut.size()));
  w.tick();
  const auto ps = w.pieces();
  REQUIRE(!ps.empty());
  const Body* b = w.piece(ps.front().id);
  REQUIRE(b != nullptr);
  CHECK(b->shapes.size() == 1);
  CHECK(b->shapes[0].grid == id);
  CHECK(b->shapes[0].xf.identity);
  CHECK(qdot(b->q, rot) > 0.9999);  // (its frame: the grid's)
  const f64 z0 = b->x.z;
  for (int t = 0; t < 240; ++t) w.tick();
  f64 zmax = 0.0;
  for (const PieceState& p : w.pieces()) zmax = std::max(zmax, p.pos.z);
  MESSAGE("turned wall: piece centre from " << z0 << " m to at most " << zmax << " m, " << w.pieces().size() << " pieces");
  CHECK(zmax < z0);
}

TEST_CASE("grids: a turned beam on two world columns hangs by its junctions and falls when they go") {
  World w;
  VoxelGrid g = ground();
  // the beam: 6 m long, 0.5 m x 0.5 m, in a grid turned 25 degrees; its ends' footprints in the
  // world get a column each (2 m tall, 0.75 m square)
  const Quat rot = yaw(25.0);
  const V3 origin{0.0, 0.0, 2.0};
  const f64 h = 0.125;
  VoxelGrid beam;
  beam.h = h;
  box(beam, {-24, -2, 0}, {24, 2, 4}, make_vox(MaterialId::Concrete, false));
  beam.compact();
  std::vector<IVec3> cols;
  for (f64 u : {-2.6, 2.6}) {
    const V3 e = origin + rotate(rot, V3{u, 0.0, 0.0});
    const IVec3 c{static_cast<i32>(std::floor(e.x / h + 0.5)), static_cast<i32>(std::floor(e.y / h + 0.5)), 0};
    cols.push_back(c);
    box(g, {c[0] - 3, c[1] - 3, 0}, {c[0] + 3, c[1] + 3, 16}, make_vox(MaterialId::Concrete, false));  // top face at z = 2 - h/2
  }
  g.compact();
  w.load(std::move(g));
  const GridId id = w.add_grid(GridFrame{origin, rot}, std::move(beam));
  REQUIRE(id != 0);
  w.bake();
  CHECK(w.design_report().floating_voxels == 0);
  for (int t = 0; t < 60; ++t) w.tick();
  CHECK(w.pieces().empty());
  const f64 phi_beam = w.probe_utilization(id, IVec3{0, 0, 2});
  const f64 phi_col = w.probe_utilization(IVec3{cols[0][0], cols[0][1], 8});
  MESSAGE("turned beam on columns: beam phi " << phi_beam << ", column phi " << phi_col);
  CHECK(phi_beam >= 0.0);
  CHECK(phi_col >= 0.0);
  // the columns go: the beam falls
  std::vector<VoxelEdit> cut;
  for (const IVec3& c : cols)
    for (i32 x = c[0] - 3; x < c[0] + 3; ++x)
      for (i32 y = c[1] - 3; y < c[1] + 3; ++y)
        for (i32 z = 0; z < 16; ++z) cut.push_back({{x, y, z}, kAir});
  w.set_voxels(cut);
  w.tick();
  bool beam_piece = false;
  for (const PieceState& p : w.pieces())
    for (const BodyShape& S : w.piece(p.id)->shapes) beam_piece = beam_piece || S.grid == id;
  CHECK(beam_piece);
}

TEST_CASE("grids: a piece of two grids keeps both lattices, bonded, and breaks between them") {
  World w;
  VoxelGrid g = ground();
  // a free concrete column of the world grid on a thin masonry stub, and a turned slab resting
  // on its top (flush): cut the stub and the column falls with the slab on it, one piece
  box(g, {-4, -4, 0}, {4, 4, 2}, make_vox(MaterialId::Masonry, false));
  box(g, {-4, -4, 2}, {4, 4, 24}, make_vox(MaterialId::Concrete, false));  // top face at z = 3 - h/2
  g.compact();
  w.load(std::move(g));
  VoxelGrid slab;
  slab.h = 0.125;
  box(slab, {-8, -8, 0}, {8, 8, 3}, make_vox(MaterialId::Concrete, false));
  slab.compact();
  const Quat rot = yaw(40.0);
  const GridId id = w.add_grid(GridFrame{V3{0.0, 0.0, 3.0}, rot}, std::move(slab));
  REQUIRE(id != 0);
  w.bake();
  for (int t = 0; t < 30; ++t) w.tick();
  CHECK(w.pieces().empty());
  std::vector<VoxelEdit> cut;
  for (i32 x = -4; x < 4; ++x)
    for (i32 y = -4; y < 4; ++y)
      for (i32 z = 0; z < 2; ++z) cut.push_back({{x, y, z}, kAir});
  w.set_voxels(cut);
  w.tick();
  const Body* both = nullptr;
  for (const PieceState& p : w.pieces())
    if (w.piece(p.id)->shapes.size() == 2) both = w.piece(p.id);
  REQUIRE(both != nullptr);
  CHECK(both->shapes[0].grid == kWorldGrid);
  CHECK(both->shapes[1].grid == id);
  CHECK(both->shapes[0].xf.identity);
  CHECK(qdot(both->shapes[1].xf.q, rot) > 0.9999);  // (the slab's lattice in the column's frame)
  // the slab's voxels are where the grid had them, relative to the column's (the piece moved a
  // little in its first tick): the slab's lattice point (0, 0, 1) was 0.625 m above the
  // column's (0, 0, 20)
  const V3 rel = both->lattice_to_world(1, V3{0.0, 0.0, 0.125}) - both->lattice_to_world(0, V3{0.0, 0.0, 2.5});
  CHECK(norm(rel) == doctest::Approx(0.625).epsilon(1e-9));
  CHECK(norm(rel - V3{0.0, 0.0, 0.625}) < 1e-3);
  // it lands; whatever breaks, the voxels stay in their lattices
  i32 total = both->count;
  for (int t = 0; t < 300; ++t) w.tick();
  i32 now = 0, shapes_world = 0, shapes_grid = 0;
  for (const PieceState& p : w.pieces()) {
    now += p.voxels;
    for (const BodyShape& S : w.piece(p.id)->shapes) (S.grid == id ? shapes_grid : shapes_world) += S.count;
  }
  MESSAGE("two-grid piece: " << total << " voxels, then " << now << " in " << w.pieces().size() << " pieces (" << shapes_world
                             << " of the world grid, " << shapes_grid << " of the turned grid)");
  CHECK(shapes_grid > 0);
  CHECK(shapes_world > 0);
}

TEST_CASE("grids: a turned object dropped into the world falls as a piece, and its grid goes when it is empty") {
  World w;
  w.load(ground());
  w.bake();
  VoxelGrid crate;
  crate.h = 0.125;
  box(crate, {0, 0, 0}, {6, 6, 6}, make_vox(MaterialId::Wood, false));
  crate.compact();
  const Quat rot = axis_angle(V3{1, 1, 0}, 30.0);
  const GridId id = w.add_grid(GridFrame{V3{1.0, 1.0, 3.0}, rot}, std::move(crate), false);
  REQUIRE(id != 0);
  CHECK(w.grids().size() == 1);
  w.tick();
  REQUIRE(w.pieces().size() == 1);
  const Body* b = w.piece(w.pieces().front().id);
  CHECK(qdot(b->q, rot) > 0.9999);
  CHECK(w.grids().empty());  // (all its voxels left: a grid of this session goes)
  for (int t = 0; t < 360; ++t) w.tick();
  const PieceState p = w.pieces().front();
  MESSAGE("dropped crate: rests at z " << p.pos.z << ", asleep " << p.asleep);
  CHECK(p.pos.z < 0.6);
  CHECK(p.pos.z > 0.0);
}

TEST_CASE("grids: rays, box sweeps and collide see turned voxels at their faces") {
  World w;
  w.load(ground());
  VoxelGrid slab;
  slab.h = 0.125;
  box(slab, {-16, -16, 0}, {16, 16, 4}, make_vox(MaterialId::Concrete, true));
  slab.compact();
  // a slab tilted 20 degrees about x, its centre at (0, 0, 3)
  const Quat rot = axis_angle(V3{1, 0, 0}, 20.0);
  const GridId id = w.add_grid(GridFrame{V3{0.0, 0.0, 3.0}, rot}, std::move(slab));
  REQUIRE(id != 0);
  const V3 up = rotate(rot, V3{0, 0, 1});
  // straight down onto the slab: its top face, normal the slab's up
  const RayHit hit = w.raycast(V3{0.1, 0.05, 8.0}, V3{0, 0, -1}, 20.0);
  REQUIRE(hit.hit);
  CHECK(hit.grid == id);
  CHECK(norm(hit.normal - up) < 1e-9);
  // the top face's plane: centre + up * (3.5 h)
  const f64 plane = dot(V3{0.0, 0.0, 3.0}, up) + 3.5 * 0.125;
  CHECK(std::abs(dot(hit.pos, up) - plane) < 1e-6);
  // a box dropped onto it stops on its surface
  const V3 mn{-0.2, -0.2, 6.0}, mx{0.2, 0.2, 7.0};
  const SweepHit sh = w.sweep(mn, mx, V3{0, 0, -6});
  REQUIRE(sh.hit);
  CHECK(sh.grid == id);
  CHECK(dot(sh.normal, up) > 0.999);
  const CollideResult cr = w.collide(mn, mx, V3{0, 0, -6});
  CHECK(cr.on_ground);
  CHECK(cr.move.z == doctest::Approx(sh.t * -6.0).epsilon(1e-3));
  // (the box's lowest corner touches the tilted face: above it, never inside)
  const f64 lowest = std::min({dot(V3{mn.x, mn.y, mn.z + cr.move.z}, up), dot(V3{mx.x, mn.y, mn.z + cr.move.z}, up),
                               dot(V3{mn.x, mx.y, mn.z + cr.move.z}, up), dot(V3{mx.x, mx.y, mn.z + cr.move.z}, up)});
  CHECK(lowest > plane - 1e-3);
  CHECK(lowest < plane + 0.01);
}

TEST_CASE("grids: carving a turned grid removes the sphere around the world point") {
  World w;
  w.load(ground());
  VoxelGrid block;
  block.h = 0.125;
  box(block, {-16, -16, 0}, {16, 16, 16}, make_vox(MaterialId::Rock, true));
  block.compact();
  const GridId id = w.add_grid(GridFrame{V3{0.0, 0.0, 1.0}, axis_angle(V3{1, 2, 3}, 50.0)}, std::move(block));
  const V3 c{0.2, -0.3, 2.0};
  w.carve(c, 0.6);
  w.tick();
  i32 kept_in = 0, gone_out = 0, gone = 0;
  const VoxelGrid* g = w.grid(id);
  for (i32 x = -16; x < 16; ++x)
    for (i32 y = -16; y < 16; ++y)
      for (i32 z = 0; z < 16; ++z) {
        const f64 d = norm(w.grid_to_world(id, V3{0.125 * x, 0.125 * y, 0.125 * z}) - c);
        const bool solid = vox_solid(g->get(x, y, z));
        gone += solid ? 0 : 1;
        if (solid && d < 0.6 * 0.9) ++kept_in;
        if (!solid && d > 0.6 * 1.1) ++gone_out;
      }
  MESSAGE("carve in a turned grid: " << gone << " voxels removed");
  CHECK(gone > 300);
  CHECK(kept_in == 0);
  CHECK(gone_out == 0);
}

TEST_CASE("grids: the changes of grids are saved and restored; grids of the session come back whole") {
  auto level = [](World& w) {
    w.load(ground());
    VoxelGrid wall;
    wall.h = 0.125;
    box(wall, {-12, -2, 0}, {12, 2, 20}, make_vox(MaterialId::Concrete, false));
    wall.compact();
    const GridId id = w.add_grid(GridFrame{V3{0.0, 1.0, 0.0}, yaw(-20.0)}, std::move(wall));
    w.bake();
    return id;
  };
  World a;
  const GridId id = level(a);
  a.carve(a.grid_to_world(id, V3{0.0, 0.0, 1.5}), 0.5);
  VoxelGrid extra;
  extra.h = 0.125;
  box(extra, {0, 0, 0}, {4, 4, 4}, make_vox(MaterialId::Rock, true));
  extra.compact();
  const GridId spawned = a.add_grid(GridFrame{V3{4.0, 4.0, 0.0}, yaw(10.0)}, std::move(extra), false);
  REQUIRE(spawned != 0);
  for (int t = 0; t < 60; ++t) a.tick();
  const std::vector<u8> delta = a.save_delta();
  World b;
  const GridId id2 = level(b);
  CHECK(id2 == id);
  REQUIRE(b.load_delta(delta));
  CHECK(b.grids().size() == 2);
  // (the voxels, junction breaks and frames: the states match; pieces in flight are not saved)
  u64 ha = 0, hb = 0;
  {
    // (compare the grids' contents alone: a's pieces took voxels that b never had)
    const VoxelGrid* ga = a.grid(id);
    const VoxelGrid* gb = b.grid(id);
    REQUIRE(ga);
    REQUIRE(gb);
    for (i32 x = -12; x < 12; ++x)
      for (i32 y = -2; y < 2; ++y)
        for (i32 z = 0; z < 20; ++z) {
          ha = ha * 31 + ga->get(x, y, z);
          hb = hb * 31 + gb->get(x, y, z);
        }
  }
  CHECK(ha == hb);
  const VoxelGrid* sb = b.grid(spawned);
  REQUIRE(sb);
  CHECK(sb->solid_count() == 64);
  GridFrame fa, fb;
  REQUIRE(a.grid_frame(spawned, &fa));
  REQUIRE(b.grid_frame(spawned, &fb));
  CHECK(norm(fa.origin - fb.origin) == 0.0);
  CHECK(qdot(fa.rot, fb.rot) == 1.0);
  // a malformed delta is refused whole
  std::vector<u8> bad = delta;
  bad.resize(bad.size() - 3);
  World c;
  level(c);
  const u64 before = c.state_hash();
  CHECK_FALSE(c.load_delta(bad));
  CHECK(c.state_hash() == before);
}

TEST_CASE("grids: a session with grids is bit-identical on any thread count") {
  auto run = [](int threads) {
    set_num_threads(threads);
    World w;
    VoxelGrid g = ground();
    box(g, {-20, -4, 0}, {-12, 4, 24}, make_vox(MaterialId::Concrete, false));
    box(g, {12, -4, 0}, {20, 4, 24}, make_vox(MaterialId::Concrete, false));
    g.compact();
    w.load(std::move(g));
    VoxelGrid beam;
    beam.h = 0.125;
    box(beam, {-20, -3, 0}, {20, 3, 4}, make_vox(MaterialId::Rc, false));
    beam.compact();
    w.add_grid(GridFrame{V3{0.0, 0.0, 3.0}, yaw(8.0)}, std::move(beam));
    w.bake();
    for (int t = 0; t < 20; ++t) w.tick();
    w.blast(V3{-2.0, 0.0, 3.2}, 0.6, 4e5);
    for (int t = 0; t < 240; ++t) w.tick();
    return std::make_pair(w.session_hash(), static_cast<i32>(w.pieces().size()));
  };
  const int hw = num_threads();
  const auto a = run(1), b = run(4);
  set_num_threads(hw);
  MESSAGE("grids session: " << a.second << " pieces, hash " << a.first);
  CHECK(a.second > 0);
  CHECK(a.first == b.first);
}

namespace {

// A flat anchored ground (chunk layer z = -1) and, at home in chunk (4, 4, -1), a turned wall
// standing on it.
class GridSource final : public ChunkSource {
 public:
  bool generate(const IVec3& c, std::vector<Vox>& out) const override {
    if (c[2] != -1) return false;
    out.assign(kChunkVox, make_vox(MaterialId::Rock, true));
    return true;
  }
  IVec3 chunk_lo() const override { return {-64, -64, -1}; }
  IVec3 chunk_hi() const override { return {64, 64, 2}; }
  std::vector<SourceGrid> grids(const IVec3& c) const override {
    if (c != IVec3{4, 4, -1}) return {};
    return {SourceGrid{77, V3{0.125 * 4 * kChunk + 2.0, 0.125 * 4 * kChunk + 2.0, 0.0}, yaw(23.0)}};
  }
  bool generate_grid(u32 id, VoxelGrid& out) const override {
    if (id != 77) return false;
    box(out, {-16, -2, 0}, {16, 2, 24}, make_vox(MaterialId::Concrete, false));
    out.compact();
    return true;
  }
};

}  // namespace

TEST_CASE("grids: a streamed world's grids come and go with their home chunk, and keep their changes") {
  World w;
  VoxelGrid g;
  g.h = 0.125;
  w.load(std::move(g));
  StreamConfig sc;
  sc.load_radius = 20.0;
  sc.evict_radius = 28.0;
  sc.chunks_per_tick = 400;
  w.enable_streaming(std::make_shared<GridSource>(), sc);
  const V3 home{0.125 * 4 * kChunk + 2.0, 0.125 * 4 * kChunk + 2.0, 0.0}, far{home.x + 80.0, home.y, 0.0};
  w.set_focus(home);
  for (int t = 0; t < 10; ++t) w.tick();
  REQUIRE(w.grids().size() == 1);
  CHECK(w.grids().front() == 77);
  bool added = false;
  for (const WorldEvent& e : w.take_events()) added = added || (e.kind == WorldEvent::Kind::GridAdded && e.id == 77);
  CHECK(added);
  const i64 solid0 = w.grid(77)->solid_count();
  CHECK(solid0 == 32 * 4 * 24);
  // it stands on the streamed ground (designed on first touch): a small carve high up
  w.carve(w.grid_to_world(77, V3{0.0, 0.0, 2.5}), 0.3);
  for (int t = 0; t < 60; ++t) w.tick();
  const i64 solid1 = w.grid(77)->solid_count();
  CHECK(solid1 < solid0);
  CHECK(solid1 > solid0 - 200);
  for (const PieceState& p : w.pieces()) CHECK(p.voxels < 200);  // (nothing big fell)
  // away: the grid goes with its home chunk (its changes archived) ...
  w.set_focus(far);
  for (int t = 0; t < 30; ++t) w.tick();
  CHECK(w.grids().empty());
  bool removed = false;
  for (const WorldEvent& e : w.take_events()) removed = removed || (e.kind == WorldEvent::Kind::GridRemoved && e.id == 77);
  CHECK(removed);
  // ... is saved from the archive ...
  const std::vector<u8> delta = w.save_delta();
  // ... and comes back with them
  w.set_focus(home);
  for (int t = 0; t < 10; ++t) w.tick();
  REQUIRE(w.grids().size() == 1);
  CHECK(w.grid(77)->solid_count() == solid1);
  // a fresh session from the delta: the grid, when it comes, has the carve
  World b;
  VoxelGrid g2;
  g2.h = 0.125;
  b.load(std::move(g2));
  b.enable_streaming(std::make_shared<GridSource>(), sc);
  REQUIRE(b.load_delta(delta));
  b.set_focus(home);
  for (int t = 0; t < 10; ++t) b.tick();
  REQUIRE(b.grids().size() == 1);
  CHECK(b.grid(77)->solid_count() == solid1);
}

TEST_CASE("grids: a diagonal strut in a grid of its own makes a cantilever a truss") {
  // A concrete tie cantilevering 4 m from an anchored wall, propped at its tip for the design; the
  // prop goes. Alone, the tie breaks off at the wall. With a concrete strut from low on the wall to
  // under its tip - a bar in a grid pitched 40 degrees, its ends cast a voxel into the wall and
  // the tie - it is a truss: the strut's junctions carry the tip, nothing breaks. (The same strut
  // stepped into the world grid breaks at its steps: the point of oriented grids.)
  const f64 h = 0.125;
  const Vox conc = make_vox(MaterialId::Concrete, false);
  auto run = [&](bool strut) {
    World w;
    VoxelGrid g = ground();
    box(g, {-8, -4, 0}, {0, 4, 40}, make_vox(MaterialId::Rock, true));  // the wall
    box(g, {0, -2, 32}, {32, 2, 36}, conc);                             // the tie
    box(g, {28, -2, 0}, {32, 2, 32}, conc);                             // the prop
    g.compact();
    w.load(std::move(g));
    if (strut) {
      const V3 a{-1, 0, 8}, b{28, 0, 32};  // (voxels: low on the wall, under the tie's tip)
      const f64 ang = std::atan2(b.z - a.z, b.x - a.x) * 180.0 / kPi;
      const i32 half = static_cast<i32>(std::ceil(0.5 * norm(b - a))) + 1;
      VoxelGrid s;
      s.h = h;
      box(s, {-half, -1, -1}, {half, 2, 2}, conc);
      s.compact();
      REQUIRE(w.add_grid(GridFrame{(a + b) * (0.5 * h), axis_angle(V3{0, 1, 0}, -ang)}, std::move(s)) != 0);
    }
    w.bake();
    for (int t = 0; t < 30; ++t) w.tick();
    CHECK(w.pieces().empty());
    std::vector<VoxelEdit> cut;
    for (i32 x = 28; x < 32; ++x)
      for (i32 y = -2; y < 2; ++y)
        for (i32 z = 0; z < 32; ++z) cut.push_back({{x, y, z}, kAir});
    w.set_voxels(cut);
    for (int t = 0; t < 120; ++t) w.tick();
    i32 tie = 0;
    for (i32 x = 0; x < 32; ++x) tie += vox_solid(w.grid().get(x, 0, 34)) ? 1 : 0;
    const f64 phi = tie == 32 ? w.probe_utilization(IVec3{2, 0, 34}) : -1.0;
    return std::make_pair(tie, phi);
  };
  const auto [bare, phi_bare] = run(false);
  const auto [braced, phi] = run(true);
  MESSAGE("tie without its prop: " << bare << " of 32 voxels alone, " << braced << " on the strut (max utilization " << phi << ")");
  (void)phi_bare;
  CHECK(bare == 0);
  CHECK(braced == 32);
  CHECK(phi > 0.0);
  CHECK(phi < 1.0);
}

TEST_CASE("grids: a bar joined by junctions carries its load like a bar of the same lattice cast in whole") {
  // A steel bar through a free concrete block, cantilevering 2 m: part of the world grid, or in a
  // grid of its own at yaw 0 (the same lattice: only its bonds are junctions), cast into the block
  // (overlapping it) or set in a hole of it (flush). One configuration's utilization depends on
  // the fragments' layout, which the lattice offset changes: compared over twelve offsets.
  const f64 h = 0.125;
  const Vox steel = make_vox(MaterialId::Steel, false), conc = make_vox(MaterialId::Concrete, false);
  f64 mean[3] = {0, 0, 0};
  for (int mode = 0; mode < 3; ++mode) {
    for (i32 dx : {0, 5, 11, 19})
      for (i32 dy : {0, 7, 13}) {
        VoxelGrid g;
        g.h = h;
        box(g, {-64, -64, -4}, {96, 64, 0}, make_vox(MaterialId::Rock, true));
        box(g, {-8 + dx, -8 + dy, 0}, {8 + dx, 8 + dy, 16}, conc);
        if (mode == 2) box(g, {-4 + dx, -2 + dy, 8}, {8 + dx, 2 + dy, 12}, kAir);
        VoxelGrid bar;
        bar.h = h;
        if (mode == 0) box(g, {-4 + dx, -2 + dy, 8}, {24 + dx, 2 + dy, 12}, steel);
        else box(bar, {-4, -2, 8}, {24, 2, 12}, steel);
        g.compact();
        bar.compact();
        World w;
        w.load(std::move(g));
        if (mode == 0) {
          mean[mode] += w.probe_utilization(IVec3{9 + dx, dy, 10}) / 12.0;
        } else {
          const GridId id = w.add_grid(GridFrame{V3{h * dx, h * dy, 0.0}, Quat{0, 0, 0, 1}}, std::move(bar));
          REQUIRE(id != 0);
          mean[mode] += w.probe_utilization(id, IVec3{9, 0, 10}) / 12.0;
        }
      }
  }
  MESSAGE("bar in a block, mean max utilization: cast in whole " << mean[0] << ", by junctions cast in " << mean[1] << ", set flush "
                                                                  << mean[2]);
  CHECK(mean[0] > 0.2);
  CHECK(mean[1] / mean[0] > 0.8);
  CHECK(mean[1] / mean[0] < 1.5);
  CHECK(mean[2] / mean[0] > 0.7);
  CHECK(mean[2] / mean[0] < 1.3);
}

TEST_CASE("grids: a member cast in at an angle carries its load like one cast in square") {
  // A 3 m concrete cantilever cast 2 voxels into an anchored pier of the world grid, in a grid
  // turned 10, 30 and 45 degrees about the vertical (its root skewed across the pier's face): its
  // utilization over sub-voxel placements, against the same cantilever square to the pier. (The
  // interface is measured by the cantilever's faces alone, the newer grid's: measured from both
  // sides, half each, a skewed root had slivers of the pier's faces carrying it.)
  const f64 h = 0.125;
  auto mean_phi = [&](f64 yaw_deg) {
    f64 sum = 0.0;
    i32 n = 0;
    for (f64 dx : {0.0, 0.03, 0.06, 0.09})
      for (f64 dy : {0.0, 0.05}) {
        World w;
        VoxelGrid g = ground();
        box(g, {-16, -16, 0}, {0, 16, 32}, make_vox(MaterialId::Rock, true));
        g.compact();
        w.load(std::move(g));
        VoxelGrid beam;
        beam.h = h;
        box(beam, {-2, -2, 16}, {24, 2, 22}, make_vox(MaterialId::Concrete, false));
        beam.compact();
        const GridId id = w.add_grid(GridFrame{V3{dx, dy, 0.0}, yaw(yaw_deg)}, std::move(beam));
        REQUIRE(id != 0);
        sum += w.probe_utilization(id, IVec3{20, 0, 18});
        ++n;
      }
    return sum / n;
  };
  const f64 square = mean_phi(0.0);
  MESSAGE("cantilever cast in at an angle, mean max utilization: square " << square << ", 10 degrees " << mean_phi(10.0) << ", 30 "
                                                                           << mean_phi(30.0) << ", 45 " << mean_phi(45.0));
  CHECK(square > 0.5);
  for (f64 a : {10.0, 30.0, 45.0}) {
    const f64 r = mean_phi(a) / square;
    CHECK(r > 0.85);
    CHECK(r < 1.35);
  }
}

TEST_CASE("grids: a slab of the world grid on a column of a turned grid falls when that grid is removed") {
  const f64 h = 0.125;
  const Vox concrete = make_vox(MaterialId::Concrete, false);
  World w;
  VoxelGrid g = ground();
  box(g, {-12, -12, 16}, {12, 12, 18}, concrete);  // (bottom face at z = 15.5 h: flush on the column)
  g.compact();
  w.load(std::move(g));
  VoxelGrid col;
  col.h = h;
  box(col, {-3, -3, 0}, {3, 3, 16}, concrete);
  col.compact();
  const GridId id = w.add_grid(GridFrame{V3{0.0, 0.0, 0.0}, yaw(30.0)}, std::move(col));
  REQUIRE(id != 0);
  w.bake();
  CHECK(w.design_report().floating_voxels == 0);
  for (int t = 0; t < 30; ++t) w.tick();
  CHECK(w.pieces().empty());
  CHECK(vox_solid(w.grid().get(0, 0, 16)));
  REQUIRE(w.remove_grid(id));
  CHECK(w.grids().empty());
  for (int t = 0; t < 30; ++t) w.tick();
  // (nothing holds the slab: it is a piece, falling)
  CHECK_FALSE(vox_solid(w.grid().get(0, 0, 16)));
  bool fell = false;
  for (const PieceState& p : w.pieces()) fell = fell || (p.voxels >= 24 * 24 * 2 / 2 && p.vel.z < -0.5);
  CHECK(fell);
}

TEST_CASE("grids: junction breaks are saved and restored with the voxels they belong to") {
  // A slab in a grid tilted 10 degrees about x, held by its end face alone, flush against the side
  // of an anchored pier; loaded at its tip, its junction breaks. The broken samples stay with the
  // voxels whose faces they sample (the slab's: the newer grid measures the interface), on the
  // voxels that stay and on the pieces that broke off, and a saved session restores them.
  const f64 h = 0.125;
  auto level = [&](World& w) {
    VoxelGrid g = ground();
    box(g, {-8, -8, 0}, {0, 8, 24}, make_vox(MaterialId::Rock, true));
    g.compact();
    w.load(std::move(g));
    VoxelGrid slab;
    slab.h = h;
    box(slab, {0, -4, 0}, {24, 4, 4}, make_vox(MaterialId::Concrete, false));
    slab.compact();
    const GridId id = w.add_grid(GridFrame{V3{0.0, 0.0, h * 16}, axis_angle(V3{1, 0, 0}, 10.0)}, std::move(slab));
    w.bake();
    return id;
  };
  auto marks = [](const World& w) {
    // every grid's junction breaks: (grid, chunk key, code)
    std::vector<std::array<u64, 3>> out;
    std::vector<GridId> ids{kWorldGrid};
    for (GridId id : w.grids()) ids.push_back(id);
    for (GridId id : ids)
      for (const auto& [k, c] : w.grid(id)->chunks())
        for (u32 code : c.jbroken) out.push_back({u64(id), k, u64(code)});
    std::sort(out.begin(), out.end());
    return out;
  };
  World a;
  const GridId id = level(a);
  for (int t = 0; t < 20; ++t) a.tick();
  CHECK(a.pieces().empty());
  std::vector<VoxelLoad> tip;
  for (i32 y = -4; y < 4; ++y) tip.push_back({IVec3{23, y, 3}, V3{0.0, 0.0, -1.25e4}, id});
  a.set_loads(1, tip);
  for (int t = 0; t < 60; ++t) a.tick();
  const auto ma = marks(a);
  i64 in_slab = 0;
  for (const auto& m : ma) in_slab += m[0] == id ? 1 : 0;
  MESSAGE("junction breaks kept: " << ma.size() << " (" << in_slab << " on the slab's faces), " << a.pieces().size() << " pieces");
  REQUIRE(in_slab > 0);
  CHECK(in_slab == static_cast<i64>(ma.size()));
  const std::vector<u8> delta = a.save_delta();
  World b;
  CHECK(level(b) == id);
  REQUIRE(b.load_delta(delta));
  CHECK(marks(b) == ma);
}
