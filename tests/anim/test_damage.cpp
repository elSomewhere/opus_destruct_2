// svx_anim damage (the TypeScript test/damage.test.ts, and more): rays against posed models, carving
// wounds, severing what lost its joint, detaching limbs with what hangs from them.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <set>

#include "doctest.h"
#include "svx/anim/characters/humans.hpp"
#include "svx/anim/voxel/damage.hpp"

using namespace svx;
using namespace svx::anim;

namespace {

struct Posed {
  std::vector<f32> skin;
  WorldPose world;
};

Posed posed(const VoxelModel& model, const V3& pos = V3{0, 0, 0}, f64 yaw = 0.0) {
  const SkeletonPtr sk = model.skeleton;
  Posed r{std::vector<f32>(size_t(sk->count) * 16), WorldPose(sk)};
  r.world.compute(Pose(sk), pos, yaw == 0.0 ? Quat{} : qz(yaw));
  r.world.write_skin(r.skin.data());
  return r;
}

f64 ms_since(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count(); }

}  // namespace

TEST_CASE("anim damage: a ray from the front at chest height hits the chest; a ray pointing away misses") {
  const ModelPtr model = make_soldier(1).model->clone();
  const Posed p = posed(*model);
  const std::optional<CharacterHit> hit = raycast_model(*model, p.skin, V3{0, 2, 1.3}, V3{0, -1, 0}, 10);
  REQUIRE_MESSAGE(hit, "hit");
  CHECK(hit->bone == H::chest);
  CHECK_MESSAGE(hit->distance > 1.7, "distance " << hit->distance);
  CHECK_MESSAGE(hit->distance < 1.95, "distance " << hit->distance);
  CHECK_MESSAGE(hit->normal.y > 0.99, "normal " << hit->normal.x << " " << hit->normal.y << " " << hit->normal.z);
  for (int a = 0; a < 3; ++a) CHECK_MESSAGE(std::abs(hit->point[a] - hit->rest_point[a]) < 1e-5, "rest pose: rest point = world point");
  CHECK(!raycast_model(*model, p.skin, V3{0, 2, 1.3}, V3{0, 1, 0}, 10));
  CHECK_MESSAGE(!raycast_model(*model, p.skin, V3{0, 2, 1.3}, V3{0, -1, 0}, 1.5), "out of range");
  // (what it reports: the part and the lattice cell hit, its slot, on the side the ray came from)
  const VoxelPart& part = model->parts[size_t(hit->part)];
  CHECK(part.bone == hit->bone);
  CHECK(part.cell(hit->cell[0], hit->cell[1], hit->cell[2]) == hit->slot + 1);
  CHECK(hit->rest_point.y == doctest::Approx((hit->cell[1] + 1) * model->voxel_size).epsilon(1e-9));
}

TEST_CASE("anim damage: carving removes voxels, bumps versions and counts") {
  const ModelPtr model = make_soldier(1).model->clone();
  const Posed p = posed(*model);
  const std::optional<CharacterHit> hit = raycast_model(*model, p.skin, V3{0, 2, 1.3}, V3{0, -1, 0}, 10);
  REQUIRE(hit);
  const VoxelPart& part = model->parts[size_t(hit->part)];
  const i32 before = part.count;
  const u32 v0 = part.version;
  const auto t0 = std::chrono::steady_clock::now();
  const std::vector<RemovedVoxel> removed = carve_model(*model, hit->rest_point, 0.05);
  const f64 ms = ms_since(t0);
  CHECK_MESSAGE(removed.size() > 5, removed.size() << " removed");
  CHECK(part.count < before);
  CHECK(part.version > v0);
  CHECK(part_integrity(part) < 1.0);
  CHECK(part_integrity(part) > 0.5);
  // (one cold call: a generous bound)
  CHECK_MESSAGE(ms < 20.0, "carve took " << ms << " ms");
  // the same ray now goes deeper
  const std::optional<CharacterHit> again = raycast_model(*model, p.skin, V3{0, 2, 1.3}, V3{0, -1, 0}, 10);
  REQUIRE(again);
  CHECK_MESSAGE(again->distance > hit->distance + 0.02, again->distance << " vs " << hit->distance);
  // removed voxels are unique and inside the sphere
  std::set<std::array<i64, 3>> keys;
  for (const RemovedVoxel& r : removed) keys.insert({std::llround(r.rest.x * 1e4), std::llround(r.rest.y * 1e4), std::llround(r.rest.z * 1e4)});
  CHECK(keys.size() == removed.size());
  for (const RemovedVoxel& r : removed) CHECK(hypot3(r.rest.x - hit->rest_point.x, r.rest.y - hit->rest_point.y, r.rest.z - hit->rest_point.z) <= 0.05 + 1e-9);
  // (the count is the part's solid cells; carving where nothing is left removes nothing)
  i32 n = 0;
  for (const u8 c : part.cells) n += c != 0;
  CHECK(n == part.count);
  const u32 v1 = part.version;
  std::vector<RemovedVoxel> none;
  carve_model(*model, hit->rest_point, 0.02, nullptr, &none);
  CHECK(none.empty());
  CHECK(part.version == v1);
}

TEST_CASE("anim damage: cutting through the forearm severs the lower forearm; detaching takes the hand") {
  const ModelPtr model = make_soldier(1).model->clone();
  const Skeleton& sk = *model->skeleton;
  const i32 fa = model->part_of_bone[H::forearmL];
  const VoxelPart& part = model->parts[size_t(fa)];
  const i32 before = part.count;
  const V3 cut = vlerp(sk.rest_head[H::forearmL], sk.rest_tail[H::forearmL], 0.45);
  carve_model(*model, cut, 0.06);
  const std::vector<VoxelPart> pieces = sever_disconnected(*model, fa, 0.05);
  REQUIRE_MESSAGE(pieces.size() >= 1, "something came off");
  i32 off = 0;
  for (const VoxelPart& p : pieces) off += p.count;
  CHECK_MESSAGE(off > 10, "off " << off << ", left " << part.count << " of " << before);
  CHECK_MESSAGE(part.count > 10, "off " << off << ", left " << part.count << " of " << before);
  CHECK_MESSAGE(part.count + off < before, "off " << off << ", left " << part.count << " of " << before);
  for (const VoxelPart& p : pieces) {
    CHECK(p.bone == H::forearmL);
    CHECK(p.version == 0u);
    CHECK(p.initial_count == p.count);
  }
  // the pieces are below the cut, the rest above it
  const f64 s = model->voxel_size;
  const VoxelPart& big = pieces[0];
  CHECK_MESSAGE((big.origin[2] + big.dims[2]) * s <= cut.z + 0.02, "the severed piece lies below the cut");
  // the pelvis never severs
  CHECK(sever_disconnected(*model, model->part_of_bone[H::pelvis], 0.05).empty());

  const ModelPtr m2 = make_soldier(1).model->clone();
  const std::vector<VoxelPart> parts = detach_subtree(*m2, H::forearmL, true);
  std::vector<i32> bones;
  for (const VoxelPart& p : parts) bones.push_back(p.bone);
  CHECK(std::count(bones.begin(), bones.end(), H::forearmL) == 1);
  CHECK(std::count(bones.begin(), bones.end(), H::handL) == 1);
  CHECK(m2->parts[size_t(m2->part_of_bone[H::handL])].count == 0);
  CHECK(m2->parts[size_t(m2->part_of_bone[H::upperarmL])].count > 0);
  // cut at the joint: no anchor left -> the whole part comes off
  const ModelPtr m3 = make_soldier(1).model->clone();
  const i32 hand = m3->part_of_bone[H::handL];
  carve_model(*m3, m3->skeleton->rest_head[H::handL], 0.05);
  const std::vector<VoxelPart> whole = sever_disconnected(*m3, hand, 0.03);
  CHECK(whole.size() == 1);
  CHECK(m3->parts[size_t(hand)].count == 0);
}

TEST_CASE("anim damage: rays hit a rotated, translated character") {
  const ModelPtr model = make_soldier(1).model->clone();
  const Posed p = posed(*model, V3{5, 3, 0.5}, kPi / 2);
  const V3 hr = model->skeleton->rest_head[H::head];
  const V3 head = p.world.point_of(H::head, V3{hr.x, hr.y + 0.012, hr.z + 0.1});
  // the model's right (+x) is world +y after a quarter turn: shoot the head from that side
  const std::optional<CharacterHit> hit = raycast_model(*model, p.skin, V3{head.x, head.y + 2, head.z}, V3{0, -1, 0}, 10);
  REQUIRE_MESSAGE(hit, "head hit");
  CHECK(hit->bone == H::head);
  CHECK_MESSAGE(hit->normal.y > 0.99, "normal " << hit->normal.x << " " << hit->normal.y << " " << hit->normal.z);
  CHECK_MESSAGE(hit->rest_point.x > 0.05, "hit on the right side in rest space: " << hit->rest_point.x);
  // performance: many rays against the posed model
  const auto t0 = std::chrono::steady_clock::now();
  i32 n = 0;
  for (i32 i = 0; i < 2000; ++i) {
    const f64 z = 0.1 + (i % 100) * 0.018;
    if (raycast_model(*model, p.skin, V3{head.x + ((i * 7) % 11) * 0.02 - 0.1, head.y + 3, z}, V3{0, -1, 0}, 10)) ++n;
  }
  MESSAGE("2000 rays: " << ms_since(t0) << " ms, " << n << " hits");
  CHECK(n > 100);
}

TEST_CASE("anim damage: listed parts, joint balls, pieces largest first, a limb without what hangs from it") {
  // a wound at the knee opens both parts that hold its joint ball; listing a part keeps it to that one
  const ModelPtr model = make_civilian(4).model->clone();
  const i32 thigh = model->part_of_bone[H::thighR], shin = model->part_of_bone[H::shinR];
  const V3 knee = model->skeleton->rest_head[H::shinR];
  const i32 t0 = model->parts[size_t(thigh)].count, s0 = model->parts[size_t(shin)].count;
  const std::vector<i32> only{shin};
  const std::vector<RemovedVoxel> a = carve_model(*model, knee, 0.03, &only);
  CHECK(!a.empty());
  CHECK(model->parts[size_t(thigh)].count == t0);
  CHECK(model->parts[size_t(shin)].count == s0 - i32(a.size()));
  const std::vector<RemovedVoxel> b = carve_model(*model, knee + V3{0.0, 0.0, 0.03}, 0.03);
  CHECK(model->parts[size_t(thigh)].count < t0);
  // (a voxel removed from both parts is reported once)
  i32 lost = (t0 - model->parts[size_t(thigh)].count) + (s0 - i32(a.size()) - model->parts[size_t(shin)].count);
  CHECK(i32(b.size()) < lost);
  for (const RemovedVoxel& r : b) CHECK((r.bone == H::thighR || r.bone == H::shinR));

  // two cuts through a shin: two pieces come off, the larger first
  const ModelPtr m2 = make_soldier(3).model->clone();
  const Skeleton& sk = *m2->skeleton;
  const i32 sn = m2->part_of_bone[H::shinL];
  carve_model(*m2, vlerp(sk.rest_head[H::shinL], sk.rest_tail[H::shinL], 0.3), 0.075, nullptr, nullptr);
  carve_model(*m2, vlerp(sk.rest_head[H::shinL], sk.rest_tail[H::shinL], 0.6), 0.075, nullptr, nullptr);
  const i32 left = m2->parts[size_t(sn)].count;
  const u32 v = m2->parts[size_t(sn)].version;
  const std::vector<VoxelPart> pieces = sever_disconnected(*m2, sn, 0.05);
  REQUIRE(pieces.size() >= 2);
  i32 off = 0;
  for (size_t i = 0; i < pieces.size(); ++i) {
    off += pieces[i].count;
    if (i > 0) CHECK(pieces[i].count <= pieces[i - 1].count);
  }
  CHECK(m2->parts[size_t(sn)].count == left - off);
  CHECK(m2->parts[size_t(sn)].version == v + 1);
  // nothing more to sever
  CHECK(sever_disconnected(*m2, sn, 0.05).empty());

  // a hand without its forearm; a detached part keeps its place, emptied
  const ModelPtr source = make_thug(2).model;
  const ModelPtr m3 = source->clone();
  const std::vector<VoxelPart> hand = detach_subtree(*m3, H::forearmR, false);
  REQUIRE(hand.size() == 1);
  CHECK(hand[0].bone == H::forearmR);
  CHECK(m3->parts[size_t(m3->part_of_bone[H::handR])].count > 0);
  const VoxelPart& emptied = m3->parts[size_t(m3->part_of_bone[H::forearmR])];
  CHECK(emptied.count == 0);
  CHECK(emptied.version == 1u);
  CHECK(part_integrity(emptied) == 0.0);
  CHECK(detach_subtree(*m3, H::forearmR, false).empty());
  // (the shared model is untouched: damage goes to its clone)
  CHECK(source->parts[size_t(source->part_of_bone[H::forearmR])].count == hand[0].count);
  CHECK(source->parts[size_t(source->part_of_bone[H::forearmR])].version == 0u);
}

TEST_CASE("anim damage: a damage record gives a copy of the whole model the same holes and cuts") {
  const ModelPtr whole = make_soldier(1).model;
  const ModelPtr hurt = whole->clone();
  CHECK(encode_damage(*whole, *hurt).empty());
  const Skeleton& sk = *hurt->skeleton;
  carve_model(*hurt, vlerp(sk.rest_head[H::chest], sk.rest_tail[H::chest], 0.5) + V3{0, -0.1, 0}, 0.05);
  carve_model(*hurt, vlerp(sk.rest_head[H::forearmL], sk.rest_tail[H::forearmL], 0.45), 0.06);
  REQUIRE(!sever_disconnected(*hurt, hurt->part_of_bone[H::forearmL], 0.05).empty());
  REQUIRE(!detach_subtree(*hurt, H::shinR, true).empty());
  const std::vector<u8> record = encode_damage(*whole, *hurt);
  REQUIRE(!record.empty());
  // (runs: far smaller than the model)
  CHECK_MESSAGE(record.size() < 400, record.size() << " bytes");

  const ModelPtr again = whole->clone();
  REQUIRE(apply_damage(*again, record));
  CHECK(again->voxel_count() == hurt->voxel_count());
  for (size_t pi = 0; pi < again->parts.size(); ++pi) {
    const VoxelPart& a = again->parts[pi];
    const VoxelPart& b = hurt->parts[pi];
    CHECK(a.cells == b.cells);
    CHECK(a.count == b.count);
    // (a changed part has a new version: meshes cached by it are made again)
    CHECK((a.version != whole->parts[pi].version) == (b.count != whole->parts[pi].count));
  }
  CHECK(encode_damage(*whole, *again) == record);
  // (twice: nothing more is gone)
  REQUIRE(apply_damage(*again, record));
  CHECK(again->voxel_count() == hurt->voxel_count());

  // a record that does not fit changes nothing
  const ModelPtr other = whole->clone();
  const i32 n0 = other->voxel_count();
  std::vector<u8> cut(record.begin(), record.end() - 1);
  CHECK(!apply_damage(*other, cut));
  std::vector<u8> bad = record;
  bad[2] ^= 1;  // (the part count)
  CHECK(!apply_damage(*other, bad));
  bad = record;
  bad[0] = 'X';
  CHECK(!apply_damage(*other, bad));
  CHECK(other->voxel_count() == n0);
  CHECK(apply_damage(*other, std::vector<u8>{}));
  CHECK(other->voxel_count() == n0);
}
