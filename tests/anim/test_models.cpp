// svx_anim models (the TypeScript test/models.test.ts, and more): soldiers and civilians build with a
// part per voxel-bearing bone and mesh; the rest pose skins every vertex to its rest position; the
// looks, meshes and palettes are the original's, bit for bit; the 20-byte character vertex; the
// mesher's cache; flesh and bone inside, joint balls in both parts; props and furniture.
#include <bit>
#include <chrono>
#include <cmath>

#include "doctest.h"
#include "svx/anim/characters/furniture.hpp"
#include "svx/anim/characters/humans.hpp"
#include "svx/anim/characters/props.hpp"
#include "svx/anim/voxel/damage.hpp"
#include "svx/anim/voxel/mesh.hpp"
#include "svx/anim/voxel/sculpt.hpp"

using namespace svx;
using namespace svx::anim;

namespace {

f64 ms_since(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count(); }

// FNV-1a digests (32-bit words little-endian), as the original's are taken for the golden values.
u32 fnv(const u8* p, size_t n, u32 h) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}
u32 word(u32 h, u32 v) {
  const u8 b[4] = {u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24)};
  return fnv(b, 4, h);
}
u32 model_digest(const VoxelModel& m) {
  u32 h = 0x811c9dc5u;
  for (const VoxelPart& p : m.parts) {
    h = word(h, u32(p.bone));
    for (const i32 v : p.origin) h = word(h, u32(v));
    for (const i32 v : p.dims) h = word(h, u32(v));
    h = fnv(p.cells.data(), p.cells.size(), h);
    h = fnv(p.shade.data(), p.shade.size(), h);
  }
  return h;
}
u32 mesh_digest(const CharacterMesh& m) {
  u32 h = fnv(m.vertices.data(), m.vertices.size(), 0x811c9dc5u);
  for (i32 i = 0; i < m.index_count; ++i) h = word(h, m.indices[size_t(i)]);
  return h;
}
u32 palette_digest(const Palette& p) {
  u32 h = 0x811c9dc5u;
  for (const auto& c : p)
    for (const f32 v : c) h = word(h, std::bit_cast<u32>(v));
  return h;
}

// A character vertex, read back from its bytes.
struct Vertex {
  f32 p[3];
  i8 n[4];
  u32 word;
};
u32 le32(const u8* b) { return u32(b[0]) | u32(b[1]) << 8 | u32(b[2]) << 16 | u32(b[3]) << 24; }
Vertex vertex_at(const CharacterMesh& m, i32 i) {
  const u8* b = m.vertices.data() + size_t(i) * kCharVertexStride;
  Vertex v;
  for (int a = 0; a < 3; ++a) v.p[a] = std::bit_cast<f32>(le32(b + 4 * a));
  for (int a = 0; a < 4; ++a) v.n[a] = static_cast<i8>(b[12 + a]);
  v.word = le32(b + 16);
  return v;
}

bool same_mesh(const CharacterMesh& a, const CharacterMesh& b) {
  return a.vertex_count == b.vertex_count && a.index_count == b.index_count && a.vertices == b.vertices && a.indices == b.indices;
}

}  // namespace

TEST_CASE("anim models: soldier and civilian models build with a part per voxel-bearing bone") {
  auto t0 = std::chrono::steady_clock::now();
  const HumanVariant s = make_soldier(1);
  const f64 ms_s = ms_since(t0);
  t0 = std::chrono::steady_clock::now();
  const HumanVariant c = make_civilian(2);
  const f64 ms_c = ms_since(t0);
  MESSAGE("soldier " << s.model->voxel_count() << " voxels " << ms_s << " ms, civilian " << c.model->voxel_count() << " voxels " << ms_c << " ms");
  for (const HumanVariant* v : {&s, &c}) {
    CHECK_MESSAGE(v->model->voxel_count() > 1500, v->spec.name);
    for (const i32 b : {H::pelvis, H::chest, H::head, H::handL, H::footR, H::shinL}) CHECK_MESSAGE(v->model->part_of_bone[size_t(b)] >= 0, "bone " << b << " has voxels");
    CHECK(v->model->part_of_bone[H::weapon] == -1);
  }
  ModelMesher mesher;
  const CharacterMesh m = mesher.mesh(*s.model);
  MESSAGE("soldier mesh: " << m.vertex_count << " vertices, " << m.index_count / 3 << " triangles");
  CHECK(m.index_count > 0);
  CHECK(m.index_count % 3 == 0);
  CHECK(m.vertices.size() == size_t(m.vertex_count) * kCharVertexStride);
  const PropPtr rifle = make_rifle();
  CHECK(rifle->model->voxel_count() > 100);
  CHECK(mesh_part(rifle->model->parts[0], rifle->model->voxel_size).vertex_count > 0);
}

TEST_CASE("anim models: the rest pose skins every vertex to its rest position") {
  const HumanVariant s = make_soldier(3);
  const SkeletonPtr sk = s.model->skeleton;
  Pose pose(sk);
  WorldPose wp(sk);
  wp.compute(pose, V3{0, 0, 0}, Quat{});
  for (i32 i = 0; i < sk->count; ++i) {
    const V3 h = sk->rest_head[size_t(i)];
    for (int a = 0; a < 3; ++a) CHECK(std::abs(wp.p[size_t(i)][a] - h[a]) < 1e-9);
  }
  // (and the vertices themselves: skin[bone] x = x)
  std::vector<f32> skin(size_t(16 * sk->count));
  wp.write_skin(skin.data());
  const CharacterMesh mesh = ModelMesher().mesh(*s.model);
  f64 worst = 0.0;
  for (i32 v = 0; v < mesh.vertex_count; ++v) {
    const Vertex x = vertex_at(mesh, v);
    const f32* m = &skin[size_t(x.word & 0xff) * 16];
    for (int a = 0; a < 3; ++a) worst = std::max(worst, std::abs(m[a] * x.p[0] + m[4 + a] * x.p[1] + m[8 + a] * x.p[2] + m[12 + a] - f64(x.p[a])));
  }
  CHECK(worst < 1e-5);
}

TEST_CASE("anim models: procedural anatomy and exterior geometry match the approved fixtures") {
  // Interior cores now follow enclosed limb cross-sections after sculpting. Keep exact hashes;
  // counts, exterior topology and palettes retain the original fixtures.
  struct Golden {
    const char* what;
    i32 voxels;
    u32 model;
    i32 vertices;
    u32 mesh;
    u32 palette;
  };
  const Golden humans[] = {
      {"soldier 1", 3958, 0x6e7a1e87u, 19864, 0xdb2eb66du, 0xf577584du},
      {"civilian 2", 2698, 0x90c9aa67u, 15488, 0x7a47fef5u, 0xc5b6e421u},
      {"thug 3", 4203, 0x30f7b848u, 21616, 0x47c00915u, 0x072bc63bu},
      {"civilian 7", 3398, 0xa9286c27u, 17864, 0x8e37e72du, 0xd5589324u},
      {"soldier 6, scheme 3", 4416, 0x5230b39du, 21864, 0xf8685dadu, 0xfadaa760u},
  };
  const HumanVariant made[] = {make_soldier(1), make_civilian(2), make_thug(3), make_civilian(7), make_soldier(6, {.scheme = 3})};
  for (size_t i = 0; i < std::size(humans); ++i) {
    const Golden& g = humans[i];
    const VoxelModel& m = *made[i].model;
    const CharacterMesh mesh = ModelMesher().mesh(m);
    CHECK_MESSAGE(m.voxel_count() == g.voxels, g.what);
    CHECK_MESSAGE(model_digest(m) == g.model, g.what);
    CHECK_MESSAGE(mesh.vertex_count == g.vertices, g.what);
    CHECK_MESSAGE(mesh_digest(mesh) == g.mesh, g.what);
    CHECK_MESSAGE(palette_digest(made[i].palette) == g.palette, g.what);
  }
  CHECK(geometry_key(made[0].spec) == "m:0.98:1.03:1.00:0.98:uniform:uniform:boots:short:beret:0:0:1:1:1:1:1:1:1:0.0313");
  CHECK(geometry_key(made[1].spec) == "f:0.91:0.90:1.07:0.94:tshirt:trousers:sneakers:long:none:0:0:0:0:0:0:0:0:0:0.0313");
  CHECK(geometry_key(made[4].spec) == "m:0.97:1.04:1.00:1.10:uniform:uniform:boots:buzz:helmet:1:0:1:0:1:1:0:1:2:0.0313");

  const Golden props[] = {
      {"rifle 1", 130, 0x531f7a73u, 1128, 0xcd243eddu, 0},
      {"smg", 56, 0xd04890cau, 576, 0x7bb7fcd9u, 0},
      {"lmg", 162, 0xb3a05b15u, 1272, 0x9ef8fe49u, 0},
      {"pistol", 127, 0x65466cb6u, 912, 0xa9b71bfbu, 0},
      {"knife", 54, 0xaa892ef0u, 568, 0xd23f29a9u, 0},
  };
  const PropPtr made_props[] = {make_rifle(kDefaultVoxelSize, 1), make_smg(), make_lmg(), make_pistol(), make_knife()};
  for (size_t i = 0; i < std::size(props); ++i) {
    const Golden& g = props[i];
    const VoxelModel& m = *made_props[i]->model;
    const CharacterMesh mesh = ModelMesher().mesh(m);
    CHECK_MESSAGE(m.voxel_count() == g.voxels, g.what);
    CHECK_MESSAGE(model_digest(m) == g.model, g.what);
    CHECK_MESSAGE(mesh.vertex_count == g.vertices, g.what);
    CHECK_MESSAGE(mesh_digest(mesh) == g.mesh, g.what);
  }

  const Golden furniture[] = {
      {"bench", 1024, 0x95ffacf1u, 10536, 0x924ae265u, 0x30f02738u},
      {"chair", 460, 0x3cba7177u, 4560, 0xb50224e5u, 0x30f02738u},
      {"desk", 2190, 0x81e65d49u, 19288, 0x8de368b5u, 0x30f02738u},
      {"cafe table", 1098, 0xd3e52905u, 10160, 0xa48758d1u, 0x30f02738u},
  };
  const Furniture made_furniture[] = {make_bench(), make_chair(), make_desk(), make_cafe_table()};
  for (size_t i = 0; i < std::size(furniture); ++i) {
    const Golden& g = furniture[i];
    const VoxelModel& m = *made_furniture[i].model;
    const CharacterMesh mesh = ModelMesher().mesh(m);
    CHECK_MESSAGE(m.voxel_count() == g.voxels, g.what);
    CHECK_MESSAGE(model_digest(m) == g.model, g.what);
    CHECK_MESSAGE(mesh.vertex_count == g.vertices, g.what);
    CHECK_MESSAGE(mesh_digest(mesh) == g.mesh, g.what);
    CHECK_MESSAGE(palette_digest(made_furniture[i].palette) == g.palette, g.what);
  }
}

TEST_CASE("anim models: the character vertex - 20 bytes, CCW faces, AO in w, bone | slot | shade") {
  // one voxel, and one diagonally above-right of it (sharing an edge): 12 faces, the four that see
  // the other voxel darkened at two corners
  VoxelPart p;
  p.bone = 5;
  p.origin = {-1, 2, 3};
  p.dims = {2, 1, 2};
  p.cells = {u8(Slot::Top2 + 1), 0, 0, u8(Slot::Metal + 1)};  // (0, 0, 0) and (1, 0, 1)
  p.shade = {200, 0, 0, 90};
  p.count = p.initial_count = 2;
  const f64 s = 0.25;
  const CharacterMesh m = mesh_part(p, s);
  REQUIRE(m.vertex_count == 48);
  REQUIRE(m.index_count == 72);
  CHECK(m.vertices.size() == 48u * 20u);
  i32 darkened = 0;
  for (i32 f = 0; f < 12; ++f) {
    const Vertex v0 = vertex_at(m, 4 * f);
    const bool first = (v0.word >> 8 & 0xf) == Slot::Top2;
    CHECK(v0.word == (first ? pack_vertex_word(5, Slot::Top2, 200) : pack_vertex_word(5, Slot::Metal, 90)));
    CHECK(v0.word == (first ? (5u | 3u << 8 | 200u << 12) : (5u | 9u << 8 | 90u << 12)));
    const V3 n{v0.n[0] / 127.0, v0.n[1] / 127.0, v0.n[2] / 127.0};
    CHECK(std::abs(norm(n) - 1.0) < 1e-12);  // (axis-aligned: one of +-127)
    for (i32 q = 0; q < 4; ++q) {
      const Vertex v = vertex_at(m, 4 * f + q);
      CHECK(v.word == v0.word);
      for (int a = 0; a < 3; ++a) CHECK(v.n[a] == v0.n[a]);
      // corners on the lattice of the part's cells, on the cell's face
      for (int a = 0; a < 3; ++a) {
        const f64 c = v.p[a] / s;
        CHECK(c == std::floor(c));
      }
      CHECK((v.n[3] == 127 || v.n[3] == 42));  // (AO 1 or 2/3: round((2 ao - 1) 127))
      if (v.n[3] == 42) ++darkened;
    }
  }
  CHECK(darkened == 8);  // (two corners on each of the four faces that see the other voxel's edge)
  // triangles wind counter-clockwise seen from outside
  for (i32 t = 0; t < m.index_count; t += 3) {
    const Vertex a = vertex_at(m, i32(m.indices[size_t(t)])), b = vertex_at(m, i32(m.indices[size_t(t + 1)])), c = vertex_at(m, i32(m.indices[size_t(t + 2)]));
    const V3 e1{f64(b.p[0]) - a.p[0], f64(b.p[1]) - a.p[1], f64(b.p[2]) - a.p[2]}, e2{f64(c.p[0]) - a.p[0], f64(c.p[1]) - a.p[1], f64(c.p[2]) - a.p[2]};
    CHECK(dot(cross(e1, e2), V3{f64(a.n[0]), f64(a.n[1]), f64(a.n[2])}) > 0.0);
  }
  // a bone override (gibs), and a merge rebasing indices
  const CharacterMesh g = mesh_part(p, s, 9);
  CHECK((vertex_at(g, 0).word & 0xff) == 9u);
  const CharacterMesh both = merge_meshes({&m, &g});
  CHECK(both.vertex_count == 96);
  CHECK(both.indices[72] == m.indices[0] + 48u);
}

TEST_CASE("anim models: the mesher re-meshes the parts that changed, and only those") {
  const ModelPtr model = make_civilian(5).model->clone();
  ModelMesher mesher;
  const CharacterMesh a = mesher.mesh(*model);
  CHECK(same_mesh(a, mesher.mesh(*model)));
  auto by_parts = [&] {
    std::vector<CharacterMesh> ms;
    for (const VoxelPart& p : model->parts)
      if (p.count > 0) ms.push_back(mesh_part(p, model->voxel_size));
    std::vector<const CharacterMesh*> ptrs;
    for (const CharacterMesh& m : ms) ptrs.push_back(&m);
    return merge_meshes(ptrs);
  };
  CHECK(same_mesh(a, by_parts()));
  // a wound in the chest: the chest (and whatever joint balls it reaches) re-meshed
  const i32 chest = model->part_of_bone[H::chest];
  const u32 v0 = model->parts[size_t(chest)].version;
  const std::vector<RemovedVoxel> removed = carve_model(*model, model->skeleton->rest_head[H::chest] + V3{0.0, 0.1, 0.05}, 0.05);
  CHECK(!removed.empty());
  CHECK(model->parts[size_t(chest)].version > v0);
  const CharacterMesh b = mesher.mesh(*model);
  CHECK(!same_mesh(a, b));
  CHECK(same_mesh(b, by_parts()));
  // a limb cut off: its parts drop out of the mesh
  const size_t pieces = detach_subtree(*model, H::forearmR, true).size();
  CHECK(pieces == 2);
  const CharacterMesh c = mesher.mesh(*model);
  CHECK(c.vertex_count < b.vertex_count);
  CHECK(same_mesh(c, by_parts()));
}

TEST_CASE("anim models: flesh and bone inside, joint balls in both parts, the rig's parts") {
  const HumanVariant v = make_soldier(2);
  const VoxelModel& m = *v.model;
  CHECK(m.parts.size() == 19);  // (23 bones: no root, clavicles or weapon socket)
  for (const i32 b : {H::root, H::clavicleL, H::clavicleR, H::weapon}) CHECK(m.part_of_bone[size_t(b)] == -1);
  // a thigh: dressed outside, flesh and bone inside
  const VoxelPart& thigh = m.parts[size_t(m.part_of_bone[H::thighL])];
  i32 slots[kSlotCount] = {};
  for (const u8 c : thigh.cells)
    if (c) ++slots[c - 1];
  CHECK(slots[Slot::Flesh] > 20);
  CHECK(slots[Slot::Bone] > 5);
  CHECK(slots[Slot::Bottom] + slots[Slot::Bottom2] + slots[Slot::Top] + slots[Slot::Top2] > 50);
  // the knee: the cells of its joint ball are in both the thigh and the shin
  const V3 knee = m.skeleton->rest_head[H::shinL];
  const VoxelPart& shin = m.parts[size_t(m.part_of_bone[H::shinL])];
  const f64 s = m.voxel_size;
  i32 shared = 0, in_ball = 0;
  const i32 ci = i32(std::floor(knee.x / s)), cj = i32(std::floor(knee.y / s)), ck = i32(std::floor(knee.z / s));
  for (i32 k = ck - 3; k <= ck + 3; ++k)
    for (i32 j = cj - 3; j <= cj + 3; ++j)
      for (i32 i = ci - 3; i <= ci + 3; ++i) {
        const V3 c = m.cell_centre(i, j, k);
        if (norm(c - knee) > 0.05) continue;
        if (thigh.cell(i, j, k) || shin.cell(i, j, k)) ++in_ball;
        if (thigh.cell(i, j, k) && thigh.cell(i, j, k) == shin.cell(i, j, k)) ++shared;
      }
  CHECK(in_ball > 10);  // (a 5 cm ball holds about 17 cells)
  CHECK(shared == in_ball);
  // every part's count is its solid cells, and it starts whole
  for (const VoxelPart& p : m.parts) {
    i32 n = 0;
    for (const u8 c : p.cells) n += c != 0;
    CHECK(n == p.count);
    CHECK(p.initial_count == p.count);
    CHECK(p.version == 0u);
    CHECK(part_integrity(p) == 1.0);
  }
}

TEST_CASE("anim models: sculpting primitives, painting filters and procedural paint") {
  const SkeletonPtr sk = humanoid_skeleton();
  Sculptor sc(sk, 0.02, V3{-0.3, -0.3, 0.0}, V3{0.3, 0.3, 0.6}, 7);
  CHECK(sc.dims == std::array<i32, 3>{30, 30, 30});
  CHECK(sc.lo == std::array<i32, 3>{-15, -15, 0});
  // a hard ball (no flesh inside) of radius 5 cells about a lattice corner: the 552 cell centres
  // within 5 cells of it (none on the sphere: three odd squares never sum to 100)
  sc.add(sphere(V3{0.0, 0.0, 0.3}, 0.1), H::chest, Slot::Gear, {.organic = false});
  i32 n = 0;
  for (const u8 c : sc.cells) n += c != 0;
  CHECK(n == 552);
  // paint only the chest's gear in the upper quarter at +x; then carve the lower half away
  sc.paint(box(V3{0.1, 0, 0.45}, V3{0.1, 0.2, 0.15}), Slot::Accent, {.only = {Slot::Gear}, .bones = {H::chest}});
  sc.paint(box(V3{0, 0, 0.45}, V3{0.2, 0.2, 0.15}), Slot::Skin, {.bones = {H::head}});  // (nothing: no head cells)
  sc.paint(box(V3{0, 0, 0.45}, V3{0.2, 0.2, 0.15}), Slot::Skin, {.only = {Slot::Hair}});  // (nothing: no hair)
  sc.carve(box(V3{0, 0, 0.15}, V3{0.2, 0.2, 0.15}));
  i32 accent = 0, gear = 0, other = 0;
  for (const u8 c : sc.cells) {
    if (c == 0) continue;
    if (c - 1 == Slot::Accent) ++accent;
    else if (c - 1 == Slot::Gear) ++gear;
    else ++other;
  }
  CHECK(accent > 0);
  CHECK(gear > 0);
  CHECK(other == 0);
  // a procedural stripe
  sc.paint_fn([](f64, f64, f64 z, u8 slot, i32, i32, i32) -> i32 { return slot == Slot::Gear && z > 0.32 ? i32(Slot::Detail) : -1; });
  const ModelPtr m = sc.finish({.name = "ball"});
  REQUIRE(m->parts.size() == 1);
  const VoxelPart& p = m->parts[0];
  CHECK(p.bone == H::chest);
  CHECK(m->name == "ball");
  i32 detail = 0;
  for (size_t i = 0; i < p.cells.size(); ++i)
    if (p.cells[i]) {
      CHECK(p.shade[i] >= 1);
      detail += p.cells[i] - 1 == Slot::Detail;
    }
  CHECK(detail > 0);
  // the part's box is the box of its cells (upper half of the ball: z 0.3 .. 0.4)
  V3 lo, hi;
  part_bounds(p, m->voxel_size, &lo, &hi);
  CHECK(lo.z == doctest::Approx(0.3).epsilon(1e-9));
  CHECK(hi.z == doctest::Approx(0.4).epsilon(1e-9));
}

TEST_CASE("anim models: specs, palettes, props and furniture") {
  // the same seed, the same spec and palette
  CHECK(geometry_key(soldier_spec(9)) == geometry_key(soldier_spec(9)));
  CHECK(soldier_palette(9) == soldier_palette(9));
  CHECK(soldier_spec(9).seed == (9 & 3));
  CHECK(civilian_spec(4).name == "civilian-4");
  CHECK(soldier_spec(-3).name == "soldier--3");
  // a chosen camouflage scheme
  const Palette urban = soldier_palette(5, 2);
  const Rgb top = hex(kCamoSchemes[2].top);
  for (int a = 0; a < 3; ++a) CHECK(urban[Slot::Top][size_t(a)] == static_cast<f32>(top[size_t(a)]));
  // sRGB to linear
  for (int a = 0; a < 3; ++a) {
    CHECK(hex(0xffffff)[size_t(a)] == doctest::Approx(1.0).epsilon(1e-12));
    CHECK(hex(0x000000)[size_t(a)] == 0.0);
  }
  CHECK(hex(0x808080)[0] == doctest::Approx(0.2158605).epsilon(1e-6));
  const Palette pal = make_palette({});
  CHECK(pal[Slot::Flesh][0] == static_cast<f32>(hex(0x8c1c1c)[0]));
  CHECK(pal[Slot::Blood][2] == static_cast<f32>(hex(0x5c0808)[2]));

  const PropPtr rifle = make_rifle(), pistol = make_pistol(), knife = make_knife();
  CHECK(rifle->long_gun());
  CHECK(!rifle->one_handed);
  CHECK(pistol->one_handed);
  CHECK(!pistol->long_gun());
  CHECK(knife->has("short_blade"));
  CHECK(rifle->model->name == "rifle-0");
  CHECK(make_rifle(kDefaultVoxelSize, 1)->model->voxel_count() > rifle->model->voxel_count());  // (the scope)
  CHECK(pistol->model->voxel_size == kDefaultVoxelSize / 2.0);
  CHECK(rifle->muzzle.y == doctest::Approx(0.544));

  const Furniture bench = make_bench(), desk = make_desk(), table = make_cafe_table();
  CHECK(bench.kind == FurnitureKind::Bench);
  CHECK(!bench.desk_height);
  CHECK(!bench.approach);
  CHECK(desk.kind == FurnitureKind::Desk);
  CHECK(desk.desk_height == 0.745);
  CHECK(table.approach == 0.2);
  CHECK(table.model->name == "cafe table");
  CHECK(bench.palette == furniture_palette());
}
