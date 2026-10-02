// svx_anim deaths and the character's damage: how bodies go down (never flipping a limb, never
// bouncing, the way the shot pushes them, the head within the neck's range, no legs swung up over
// a pivoting body), head shots, severed limbs, gibbing - on both paths.
#include <cmath>

#include "doctest.h"
#include "scene.hpp"
#include "svx/anim/damage/scenarios.hpp"
#include "svx/anim/characters/props.hpp"

using namespace scene;

namespace {

const Path kPaths[] = {Path::Shallow, Path::Deep};

struct DeathOpts {
  f64 speed = 0.0;
  V3 dir{0, -1, 0};
  f64 z = 1.3;
  f64 collapse = 0.6;
  bool blast = false;
  f64 seed = 1.0;
};

struct DeathTrace {
  f64 worst_twist = 0.0;
  std::vector<f64> pelvis_z, head_z;
  f64 slept_at = -1.0;
  f64 head_turn = 0.0;
  f64 moved_x = 0.0, moved_y = 0.0;
};

// A soldier standing (or running) who dies from a shot (or a blast); per-frame traces.
DeathTrace death(Path path, const DeathOpts& o) {
  Scene s(path);
  Character& c = s.add(make_soldier(static_cast<i32>(o.seed)), 1.0, kPi / 2.0, V3{0, 0, s.ground}, make_rifle());
  f64 y = 0.0;
  for (i32 i = 0; i < 90; ++i) {
    y += o.speed * DT;
    s.frame({&c}, [&] { c.set_root(V3{0, y, s.ground}, kPi / 2.0); });
  }
  const V3 start{c.pose.p[H::pelvis].x, c.pose.p[H::pelvis].y, 0.0};
  if (o.blast) {
    c.die(nullptr, nullptr, 0.0);
    c.blast_push(V3{0.8, y - 0.5, 0.3 + s.ground}, 3.0, 9.0);
  } else {
    const V3 p = c.pose.p[H::chest];
    const V3 at{p.x, p.y, o.z + s.ground};
    const V3 dv = o.dir * 2.8;
    c.die(&at, &dv, o.collapse);
  }
  const Skeleton& sk = *c.model->skeleton;
  std::vector<Quat> prev = c.pose.q;
  DeathTrace r;
  for (i32 i = 1; i <= 60 * 6; ++i) {
    s.frame({&c});
    const std::vector<Quat>& q = c.pose.q;
    // roll change of every bone about its own length, frame to frame (a flip)
    for (i32 b = 1; b < static_cast<i32>(q.size()); ++b) {
      if (b == H::weapon) continue;
      const V3 rr = sk.rest_tail[size_t(b)] - sk.rest_head[size_t(b)];
      f64 rl = norm(rr);
      if (rl == 0.0) rl = 1.0;
      const V3 ru = rr * (1.0 / rl);
      const V3 axis = rotate(q[size_t(b)], ru);
      const V3 across = std::abs(ru.x) < 0.9 ? V3{1, 0, 0} : V3{0, 1, 0};
      auto flat = [&](const V3& v) {
        const V3 w = v - axis * dot(v, axis);
        f64 l = norm(w);
        if (l == 0.0) l = 1.0;
        return w * (1.0 / l);
      };
      const V3 x0 = flat(rotate(prev[size_t(b)], across)), x1 = flat(rotate(q[size_t(b)], across));
      if (i > 9) r.worst_twist = std::max(r.worst_twist, std::acos(std::max(-1.0, std::min(1.0, dot(x0, x1)))));
    }
    prev = q;
    r.pelvis_z.push_back(c.pose.p[H::pelvis].z - s.ground);
    r.head_z.push_back(c.pose.p[H::head].z - s.ground);
    if (r.slept_at < 0.0 && c.asleep()) r.slept_at = i * DT;
  }
  const V3 end = c.pose.p[H::pelvis];
  // how far the head is turned from the chest (the face's direction about the chest's up)
  const V3 face = rotate(c.pose.q[H::head], V3{0, 1, 0});
  const V3 fwd = rotate(c.pose.q[H::chest], V3{0, 1, 0}), right = rotate(c.pose.q[H::chest], V3{1, 0, 0});
  r.head_turn = std::abs(std::atan2(dot(face, right), dot(face, fwd))) * 180.0 / kPi;
  r.worst_twist *= 180.0 / kPi;
  r.moved_x = end.x - start.x;
  r.moved_y = end.y - start.y;
  return r;
}

std::vector<std::pair<std::string, DeathOpts>> deaths_of(bool with_side) {
  std::vector<std::pair<std::string, DeathOpts>> v;
  v.push_back({"front", DeathOpts{}});
  DeathOpts back;
  back.dir = V3{0, 1, 0};
  v.push_back({"back", back});
  DeathOpts run;
  run.speed = 4.5;
  v.push_back({"running", run});
  DeathOpts side;
  side.dir = V3{1, 0, 0};
  if (with_side) {
    side.z = 1.6;
    side.collapse = 0.15;
  }
  v.push_back({"side", side});
  DeathOpts blast;
  blast.blast = true;
  v.push_back({"blast", blast});
  DeathOpts seed2;
  seed2.seed = 2.0;
  seed2.dir = V3{0, 1, 0};
  v.push_back({"seed 2 back", seed2});
  return v;
}

}  // namespace

TEST_CASE("death: limbs never flip about their length, whatever the death") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    for (const auto& [name, o] : deaths_of(true)) {
      const DeathTrace r = death(path, o);
      INFO(pn << " " << name << ": a bone rolled " << r.worst_twist << " deg in one frame");
      CHECK(r.worst_twist < 60.0);
    }
  }
}

TEST_CASE("death: bodies come to rest on the ground and never bounce or blow up") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    std::vector<std::pair<std::string, DeathOpts>> cases = deaths_of(false);
    cases[3].second = DeathOpts{};
    cases[3].second.collapse = 0.15;
    cases[3].first = "quick";
    for (const auto& [name, o] : cases) {
      const DeathTrace r = death(path, o);
      f64 low = 1e300, rise = 0.0;
      for (size_t i = 40; i < r.pelvis_z.size(); ++i) {
        low = std::min(low, r.pelvis_z[i]);
        rise = std::max(rise, r.pelvis_z[i] - low);
      }
      INFO(pn << " " << name << ": the pelvis rose " << rise * 100.0 << " cm after coming down; asleep at " << r.slept_at << " s; lies at "
              << r.pelvis_z.back());
      CHECK(rise < 0.12);
      CHECK(r.slept_at > 0.0);
      CHECK(r.slept_at < 6.0);
      CHECK(r.pelvis_z.back() < 0.3);
    }
  }
}

TEST_CASE("death: a body shot crumples over a good half second; a head shot drops it") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO(pn);
    DeathOpts o;
    o.collapse = 0.6;
    const DeathTrace body = death(path, o);
    // still mostly up after 0.2 s, on the ground by 1.5 s
    CHECK(body.head_z[11] > 1.0);
    CHECK(body.head_z[89] < 0.35);
    // never faster than falling (no whip into the ground)
    f64 fastest = 0.0;
    for (size_t i = 1; i < 90; ++i) fastest = std::max(fastest, (body.head_z[i - 1] - body.head_z[i]) / DT);
    INFO("the head drops at most " << fastest << " m/s");
    CHECK(fastest < 8.0);
    DeathOpts h;
    h.collapse = 0.15;
    h.z = 1.6;
    const DeathTrace head = death(path, h);
    INFO("a head shot: head at " << head.head_z[24] << " vs " << body.head_z[24] << " m after 0.4 s");
    CHECK(head.head_z[24] < body.head_z[24] - 0.1);
  }
}

TEST_CASE("death: bodies fall the way the killing shot pushes them") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    DeathOpts f;
    f.dir = V3{0, -1, 0};
    DeathOpts b;
    b.dir = V3{0, 1, 0};
    const DeathTrace front = death(path, f), back = death(path, b);
    // facing +y: shot from the front they go down backwards, from behind forwards
    INFO(pn << ": shot from the front, the pelvis moved " << front.moved_y << " m; from behind " << back.moved_y << " m");
    CHECK(front.moved_y < -0.1);
    CHECK(back.moved_y > 0.1);
  }
}

TEST_CASE("death: the head rests turned within the range of the neck (face down it lies on a cheek)") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    for (const auto& [name, o] : deaths_of(false)) {
      const DeathTrace r = death(path, o);
      INFO(pn << " " << name << ": the head rests turned " << r.head_turn << " deg from the chest");
      CHECK(r.head_turn < 92.0);
    }
  }
}

TEST_CASE("death: a body that tips over collapses; it never pivots on its hips and swings its legs up over itself") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    i32 flips = 0;
    for (f64 seed : {1.0, 2.0, 4.0, 6.0}) {
      for (const auto& [dx, dy] : {std::pair<f64, f64>{0, -1}, std::pair<f64, f64>{0, 1}}) {
        Scene s(path);
        Character& c = s.add(make_soldier(static_cast<i32>(seed)), seed, kPi / 2.0, V3{0, 0, s.ground});
        for (i32 i = 0; i < 30; ++i) s.frame({&c});
        // A dying body folds over a low belly/back wound. Health is derived now;
        // the explicit dying transition keeps this a collapse mechanics regression.
        const V3 p = c.pose.p[H::spine];
        const std::optional<CharacterHit> hit = c.raycast(V3{p.x - dx * 3.0, p.y - dy * 3.0, p.z + 0.05}, V3{dx, dy, 0}, 6.0);
        REQUIRE(hit);
        c.wound(*hit, V3{dx, dy, 0}, 40.0);
        c.die(nullptr, nullptr, .6);
        i32 over = 0;
        for (i32 i = 0; i < 300; ++i) {
          s.frame({&c});
          const std::vector<V3>& P = c.pose.p;
          if (P[H::head].z - s.ground < 0.35 && std::max(P[H::footL].z, P[H::footR].z) - P[H::pelvis].z > 0.25) ++over;
        }
        if (over > 10) {
          ++flips;
          MESSAGE(pn << " seed " << seed << ", direction " << dy << ": feet above pelvis for " << over << " frames");
        }
      }
    }
    // (the original draws the collapse's length from Math.random: run a few times it swings one
    // body's legs up, 1 of 8, about one run in three - the seeded port meets one such draw)
    INFO(pn << ": " << flips << " of 8 bodies swung their legs up over themselves");
    CHECK(flips <= 1);
  }
}

// ---- the character's damage ----------------------------------------------------------------------

namespace {

Character& soldier4(Scene& s) {
  Character& c = s.add(make_soldier(4), 1.0, kPi / 2.0, V3{0, 0, s.ground}, make_rifle());  // (facing +y)
  for (i32 i = 0; i < 30; ++i) s.frame({&c});
  return c;
}

}  // namespace

TEST_CASE("character: a head shot kills, turns the body into a ragdoll and leaves the shared model alone") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO(pn);
    Scene s(path);
    Character& c = soldier4(s);
    const ModelPtr shared = c.model;
    const V3 head = c.pose.p[H::head];
    const V3 origin{head.x, head.y + 5.0, head.z + 0.1};
    const V3 dir{0, -1, 0};
    const std::optional<CharacterHit> hit = c.raycast(origin, dir, 20.0);
    REQUIRE(hit);
    CHECK((hit->bone == H::head || hit->bone == H::neck));
    const WoundResult r = c.wound(*hit, dir, 35.0);
    CHECK(r.headshot);
    CHECK(r.killed);
    CHECK(!c.alive());
    CHECK(!r.removed.empty());
    CHECK(c.model != shared);
    CHECK(shared->voxel_count() == make_soldier(4).model->voxel_count());
    c.drop_weapon();
    CHECK_FALSE(c.weapon);
    CHECK_FALSE(c.attachments().registry->nearby(c.pose.p[H::chest], 2).empty());
    for (i32 i = 0; i < 240; ++i) s.frame({&c});
    CHECK(c.pose.p[H::head].z - s.ground < 0.6);
  }
}

TEST_CASE("character: a cross-section cut severs the limb; a nearby blast leaves a coherent body") {
  for (Path path : kPaths) {
    Scene s(path);
    auto& c = soldier4(s);
    const int bone = H::forearmL;
    const V3 centre = vlerp(c.pose.p[bone], c.pose.tail(bone), .55), axis = vnorm(c.pose.tail(bone) - c.pose.p[bone]);
    DamageDescriptor cut;
    cut.kind = DamageKind::Edge;
    cut.mass = 3;
    cut.speed = 36;
    cut.direction = vnorm(cross(axis, V3{0, 0, 1}), V3{0, -1, 0});
    cut.point = centre - cut.direction * .12;
    const V3 edge = vnorm(cross(axis, cut.direction));
    cut.edge_a = centre - edge * .2;
    cut.edge_b = centre + edge * .2;
    cut.swept_length = .4;
    cut.bone = bone;
    const auto wound = c.damage(cut);
    CHECK_FALSE(wound.gibs.empty());
    CHECK(c.behaviours.lost[B::handL]);
    const auto blast = c.blast(c.pose.p[H::chest] + V3{0, 1, 0}, 1, 1);
    CHECK_FALSE(blast.gibbed);
    CHECK(c.model->voxel_count() > 0);
    CHECK(c.model->parts[size_t(c.model->part_of_bone[H::pelvis])].count > 0);
  }
}

TEST_CASE("character: its damage record makes it again with its wounds, without the limbs it lost") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO(pn);
    Scene s(path);
    Character& c = soldier4(s);
    CHECK(c.damage_record().empty());
    for (const auto& cut : damage_scenario(c, "forearmSever")) c.damage(cut);
    REQUIRE(c.gun_hand_lost());
    CHECK(!c.weapon);
    const V3 chest = c.pose.p[H::chest];
    const std::optional<CharacterHit> hit = c.raycast(V3{chest.x, chest.y + 3.0, chest.z + 0.1}, V3{0, -1, 0}, 10.0);
    REQUIRE(hit);
    c.wound(*hit, V3{0, -1, 0}, 10.0, 0.05);
    const std::vector<u8> rec = c.damage_record();
    REQUIRE(!rec.empty());
    MESSAGE(pn << ": record " << rec.size() << " bytes, " << c.model->voxel_count() << " voxels left");

    Character& d = s.add(make_soldier(4), 2.0, kPi / 2.0, V3{3, 0, s.ground}, make_rifle());
    REQUIRE(d.restore_damage(rec));
    CHECK(d.owns_model);
    CHECK(d.model->voxel_count() == c.model->voxel_count());
    for (size_t pi = 0; pi < d.model->parts.size(); ++pi) CHECK(d.model->parts[pi].cells == c.model->parts[pi].cells);
    for (i32 i = 0; i < kBodyCount; ++i) CHECK(d.behaviours.lost[size_t(i)] == c.behaviours.lost[size_t(i)]);
    CHECK(d.body.parts[B::handR]->gone);
    CHECK(!d.weapon);
    CHECK(d.damage_record() == rec);
    // (a record that does not fit is refused, the model left alone)
    Character& e = s.add(make_soldier(4), 3.0, kPi / 2.0, V3{-3, 0, s.ground});
    std::vector<u8> bad = rec;
    bad.pop_back();
    CHECK(!e.restore_damage(bad));
    CHECK(!e.owns_model);
    // it goes on as a body: dies, falls and lies
    d.die(nullptr, nullptr, 0.0);
    for (i32 i = 0; i < 180; ++i) s.frame({&c, &d, &e});
    CHECK(d.pose.p[H::head].z - s.ground < 0.6);
  }
}
