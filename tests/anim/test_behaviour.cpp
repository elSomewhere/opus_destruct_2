// svx_anim behaviours: the body carrying out its plan, hits, balance, trips, stairs and rubble,
// reflexes, bracing, knockouts, lying and crawling, dying, bodies among bodies, limbs lost - each
// on the shallow path (the original's) and the deep one (bodies of a core World).
#include <algorithm>
#include <cmath>

#include "doctest.h"
#include "scene.hpp"
#include "svx/anim/characters/props.hpp"

using namespace scene;

namespace {

const Path kPaths[] = {Path::Shallow, Path::Deep};

f64 hypot3v(const V3& a, const V3& b) { return norm(a - b); }

}  // namespace

// ---- carrying out the plan -----------------------------------------------------------------------

TEST_CASE("behaviour: walking and running, the body follows its plan closely and stays on its feet") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    for (f64 speed : {1.4, 3.0, 5.0}) {
      Scene s(path);
      Character& c = s.civilian();
      f64 worst = 0.0;
      bool calm = true;
      run(s, c, 4.0, speed, [&](Character& x, f64 t, Host&) {
        if (t < 1.0) return;
        worst = std::max(worst, hypot3v(x.pose.p[H::pelvis], x.motion.world.p[H::pelvis]));
        calm = calm && x.behaviours.mode == BodyMode::Animated;
      });
      INFO(speed << " m/s: pelvis " << worst * 100.0 << " cm off the plan");
      MESSAGE(pn << " " << speed << " m/s: pelvis " << worst * 100.0 << " cm off the plan");
      CHECK(calm);
      CHECK(worst < 0.08);
    }
  }
}

TEST_CASE("behaviour: a calm body can rest on its plan alone (physics off), and a hit wakes it") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    Scene s(path);
    Character& c = s.civilian();
    c.physics = false;
    run(s, c, 2.0);
    CHECK(!c.behaviours.physical);
    CHECK(!c.bound());
    CHECK(hypot3v(c.pose.p[H::pelvis], c.motion.world.p[H::pelvis]) < 1e-6);
    const V3 ch = c.pose.p[H::chest];
    HitInfo hi;
    hi.point = V3{ch.x, ch.y + 0.1, ch.z};
    hi.dir = V3{0, -1, 0};
    hi.force = 1.0;
    hi.kind = HitKind::Bullet;
    hi.bone = H::chest;
    c.hit_at(hi);
    CHECK(c.behaviours.physical);
    if (path == Path::Deep) CHECK(c.bound());
  }
}

// ---- hits ----------------------------------------------------------------------------------------

TEST_CASE("behaviour: a round in the chest rocks the trunk back along the shot, 5-35 degrees; one round does not fell") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    Scene s(path);
    Character& c = s.soldier();
    run(s, c, 1.0);
    const V3 up0 = chest_up(c);
    const V3 ch = c.pose.p[H::chest];
    HitInfo hi;
    hi.point = V3{ch.x, ch.y + 0.12, ch.z + 0.1};
    hi.dir = V3{0, -1, 0};
    hi.force = 1.1;
    hi.kind = HitKind::Bullet;
    hi.bone = H::chest;
    c.hit_at(hi);
    f64 peak = 0.0, back = 0.0;
    run(s, c, 1.5, 0.0, [&](Character& x, f64, Host&) {
      const V3 u = chest_up(x);
      peak = std::max(peak, std::acos(std::min(1.0, dot(u, up0))));
      back = std::min(back, u.y);
    });
    const f64 deg = peak * 180.0 / kPi;
    INFO("chest tipped " << deg << " deg");
    CHECK(deg > 5.0);
    CHECK(deg < 35.0);
    CHECK(back < -0.05);
    CHECK(!c.down());
    CHECK(c.alive());
  }
}

TEST_CASE("behaviour: a gut wound folds the body over it and a hand goes to it; a leg wound leaves a limp on that side") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    {
      Scene s(path);
      Character& c = s.civilian();
      run(s, c, 0.5);
      const V3 sp = c.pose.p[H::spine];
      HitInfo hi;
      hi.point = V3{sp.x + 0.03, sp.y + 0.12, sp.z + 0.06};
      hi.dir = V3{0, -1, 0};
      hi.force = 1.0;
      hi.kind = HitKind::Bullet;
      hi.bone = H::spine;
      c.hit_at(hi);
      f64 fold = 0.0, near = 1e300;
      const f64 f0 = rotate(c.pose.q[H::chest], V3{0, 1, 0}).z;
      run(s, c, 2.0, 0.0, [&](Character& x, f64, Host&) {
        fold = std::max(fold, f0 - rotate(x.pose.q[H::chest], V3{0, 1, 0}).z);
        const V3 w = x.pose.p[H::spine];
        for (i32 hb : {H::handL, H::handR}) {
          const V3 p = x.pose.p[size_t(hb)];
          near = std::min(near, std::sqrt((p.x - w.x) * (p.x - w.x) + (p.y - w.y - 0.1) * (p.y - w.y - 0.1) + (p.z - w.z) * (p.z - w.z)));
        }
      });
      INFO("fold " << fold << ", hand within " << near);
      CHECK(fold > 0.08);
      CHECK(near < 0.2);
    }
    {
      Scene s(path);
      Character& d = s.civilian();
      run(s, d, 0.5);
      const V3 th = d.pose.p[H::thighL];
      HitInfo hi;
      hi.point = V3{th.x, th.y + 0.08, th.z - 0.15};
      hi.dir = V3{0, -1, 0};
      hi.force = 1.0;
      hi.kind = HitKind::Bullet;
      hi.bone = H::thighL;
      d.hit_at(hi);
      run(s, d, 3.0);
      const Injuries& inj = d.behaviours.injuries;
      CHECK(inj.legL > 0.2);
      CHECK(inj.legR == 0.0);
      CHECK(d.motion.control.limp[0] > 0.2);
    }
  }
}

TEST_CASE("behaviour: a kick shoves the body along the blow: it steps with it and recovers; the host follows the root") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    Scene s(path);
    Character& c = s.civilian();
    Host h = run(s, c, 0.5);
    const f64 x0 = h.pos.x;
    const V3 sp = c.pose.p[H::spine];
    HitInfo hi;
    hi.point = V3{sp.x - 0.1, sp.y, sp.z};
    hi.dir = V3{1, 0, 0};
    hi.force = 1.8;
    hi.kind = HitKind::Blunt;
    hi.bone = H::spine;
    c.hit_at(hi);
    bool reacted = false;
    h = run(s, c, 4.0, 0.0, [&](Character& x, f64, Host&) { reacted = reacted || x.behaviours.mode == BodyMode::Reacting; }, h);
    INFO("the host followed " << h.pos.x - x0 << " m");
    CHECK(reacted);
    CHECK(h.pos.x - x0 > 0.15);
    CHECK(c.behaviours.mode == BodyMode::Animated);
    CHECK(!c.down());
  }
}

// ---- balance -------------------------------------------------------------------------------------

TEST_CASE("behaviour: a light shove is taken in place, harder ones take steps, the hardest fell; it gets up") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    std::vector<i32> steps;
    bool fell = false;
    for (f64 sv : {0.5, 1.2, 1.8}) {
      Scene s(path);
      Character& c = s.civilian();
      run(s, c, 0.5);
      c.push(V3{1, 0, 0}, sv);
      i32 most = 0;
      run(s, c, 3.0, 0.0, [&](Character& x, f64, Host&) {
        most = std::max(most, x.behaviours.steps);
        if (x.down()) fell = true;
      });
      steps.push_back(most);
      INFO("shove " << sv);
      CHECK(c.behaviours.mode == BodyMode::Animated);
    }
    INFO("steps " << steps[0] << ", " << steps[1] << ", " << steps[2]);
    CHECK(!fell);
    CHECK(steps[0] <= 1);
    CHECK(steps[2] > steps[0]);
    Scene s(path);
    Character& c = s.civilian();
    run(s, c, 0.5);
    c.push(V3{0, -1, 0}, 3.5);
    bool down = false, lying = false;
    run(s, c, 9.0, 0.0, [&](Character& x, f64, Host&) {
      if (x.down()) down = true;
      if (x.behaviours.mode == BodyMode::Lying) lying = true;
    });
    CHECK(down);
    CHECK(lying);
    CHECK(c.behaviours.mode == BodyMode::Animated);
    CHECK(c.motion.stance == Stance::Stand);
    CHECK(c.pose.p[H::pelvis].z > 0.7 + s.ground);
  }
}

TEST_CASE("behaviour: a foot caught mid-stride pitches the body forward; it catches itself or goes down, and carries on") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    for (f64 speed : {1.5, 4.0}) {
      INFO(speed << " m/s");
      Scene s(path);
      Character& c = s.civilian();
      Host h = run(s, c, 1.2, speed);
      const f64 y0 = c.pose.p[H::pelvis].y;
      c.trip();
      bool reacted = false;
      f64 furthest = y0;
      h = run(s, c, 0.8, speed, [&](Character& x, f64, Host&) {
        reacted = reacted || x.controlled();
        furthest = std::max(furthest, x.pose.p[H::pelvis].y);
      }, h);
      CHECK(reacted);
      // It carries forward momentum into the trip, then may step back to brake.
      // Check the excursion, not its position at one arbitrary recovery frame.
      CHECK(furthest > y0 + 0.2);
      h = run(s, c, 8.0, 0.0, nullptr, h);
      CHECK(c.behaviours.mode == BodyMode::Animated);
      const V3 recovered = c.pose.p[H::pelvis];
      run(s, c, 1.5, speed, nullptr, h);
      CHECK(norm(c.pose.p[H::pelvis] - recovered) > 0.5);
    }
  }
}

TEST_CASE("behaviour: walking and running up a flight of voxel stairs and off the landing, nobody trips") {
  // 6 steps of 0.125 m (0.375 m treads) from x = 4 up to a landing at 0.75 m that ends in a drop
  auto top = [](f64 x) -> f64 { return x >= 4.0 && x < 6.25 ? std::floor((x - 4.0) / 0.375) + 1.0 : x >= 6.25 && x < 9.0 ? 6.0 : 0.0; };
  const Solid solid = [&](i32 i, i32, i32 k) { return k <= top(i * kH); };
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    Scene s(path, solid, IVec3{-8, -24, -4}, IVec3{112, 24, 16});
    for (f64 speed : {1.4, 3.5}) {
      for (f64 seed : {1.0, 3.0}) {
        INFO(speed << " m/s, seed " << seed);
        Character& c = s.civilian(seed, 0.0, V3{1.0, 0.0, 0.0625}, false);
        Host h0{V3{1.0, 0.0, 0.0625}, 0.0, speed};
        bool calm = true;
        run(s, c, (11.0 - 1.0) / speed, 0.0, [&](Character& x, f64, Host& hh) {
          calm = calm && x.behaviours.mode == BodyMode::Animated;
          if (!x.controlled()) {
            hh.pos.x += speed * DT;
            const std::optional<f64> g = s.col->ground_height(hh.pos.x, hh.pos.y, hh.pos.z + 0.7, hh.pos.z - 1.5);
            if (g) hh.pos.z = *g;
          }
        }, h0);
        CHECK(calm);
        // (the next one walks on its own)
        if (path == Path::Deep) c.unbound();
        s.chars.pop_back();
      }
    }
  }
}

TEST_CASE("behaviour: walkers pick their way across a field of rubble (a toe that touches is lifted over)") {
  struct Box {
    f64 x0, y0, x1, y1, z;
  };
  std::vector<Box> boxes;
  for (i32 k = 0; k < 90; ++k) {
    const f64 x = 5.0 + ((k * 37) % 50) / 10.0, y = -8.0 + ((k * 53) % 40) / 10.0, z = ((k * 13) % 3) * 0.125 + 0.125;
    boxes.push_back(Box{x, y, x + 0.25 + (k % 3) * 0.125, y + 0.25 + (k % 2) * 0.125, z});
  }
  const Solid solid = [&](i32 i, i32 j, i32 k) {
    if (k <= 0) return true;
    const f64 x = i * kH, y = j * kH, z = k * kH;
    for (const Box& b : boxes)
      if (x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1 && z < b.z + 0.0625) return true;
    return false;
  };
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    Scene s(path, solid, IVec3{0, -80, -4}, IVec3{112, 8, 16});
    i32 stumbled = 0, runs = 0;
    for (f64 lane : {-7.5, -6.8, -6.1, -5.4, -4.7}) {
      ++runs;
      Character& c = s.civilian(1.0, 0.0, V3{3.5, lane, 0.0625}, false);
      bool hit = false;
      run(s, c, 7.0, 0.0, [&](Character& x, f64, Host& hh) {
        hit = hit || x.behaviours.mode != BodyMode::Animated;
        if (!x.controlled()) {
          hh.pos.x += 1.2 * DT;
          const std::optional<f64> g = s.col->ground_height(hh.pos.x, hh.pos.y, hh.pos.z + 0.7, hh.pos.z - 1.5);
          if (g) hh.pos.z = *g;
        }
      }, Host{V3{3.5, lane, 0.0625}, 0.0, 0.0});
      if (hit) ++stumbled;
      if (path == Path::Deep) c.unbound();
      s.chars.pop_back();
    }
    INFO(stumbled << " of " << runs << " walkers stumbled");
    CHECK(stumbled <= 1);
  }
}

TEST_CASE("behaviour: running over a beam without lifting the feet catches a foot on it") {
  // a 15 cm beam across the way at y = 3
  const Solid solid = [](i32, i32 j, i32 k) { return k < 0 || (k == 0 && j == 24); };
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    i32 tripped = 0;
    for (f64 seed : {1.0, 2.0, 3.0, 5.0, 8.0, 13.0}) {
      Scene s(path, solid, IVec3{-16, -16, -4}, IVec3{16, 80, 8});
      Character& c = s.civilian(seed, kPi / 2.0, V3{0, 0, 0}, false);
      c.motion.input.mood = Mood::Panic;
      run(s, c, 2.2, 4.5, [&](Character& x, f64, Host&) {
        if (x.behaviours.mode == BodyMode::Reacting || x.down()) ++tripped;
      });
    }
    CHECK(tripped > 0);
  }
}

// ---- reflexes ------------------------------------------------------------------------------------

TEST_CASE("behaviour: a round landing close turns the head away and brings a hand up between the face and it") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    Scene s(path);
    Character& c = s.civilian();
    run(s, c, 1.0);
    const V3 head0 = c.pose.p[H::head];
    const V3 f0 = rotate(c.pose.q[H::head], V3{0, 1, 0});
    // just in front, to the right of the face
    Perception p;
    p.point = V3{head0.x + 0.5, head0.y + 0.5, head0.z};
    p.strength = 1.0;
    p.kind = PerceptionKind::Impact;
    c.perceive(p);
    f64 away = 0.0, hand = 1e300, duck = 0.0;
    run(s, c, 0.5, 0.0, [&](Character& x, f64, Host&) {
      const V3 f = rotate(x.pose.q[H::head], V3{0, 1, 0});
      // the face turns from the danger (+x)
      away = std::max(away, f0.x - f.x);
      duck = std::max(duck, head0.z - x.pose.p[H::head].z);
      const V3 e = x.eyes();
      for (i32 hb : {H::handL, H::handR}) hand = std::min(hand, norm(x.pose.p[size_t(hb)] - V3{e.x + 0.2, e.y + 0.2, e.z}));
    });
    INFO("away " << away << ", ducked " << duck << ", hand " << hand);
    CHECK(away > 0.15);
    CHECK(duck > 0.03);
    CHECK(hand < 0.3);
    run(s, c, 2.5);
    CHECK(c.pose.p[H::head].z > head0.z - 0.05);
  }
}

TEST_CASE("behaviour: shoved towards a wall, a hand goes out to it and holds on") {
  // a wall at x = 0.75 (a plane of voxels), the character beside it facing +y
  const Solid solid = [](i32 i, i32, i32 k) { return k < 0 || (i >= 6 && i <= 7 && k < 24); };
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    Scene s(path, solid, IVec3{-24, -24, -4}, IVec3{24, 24, 30});
    Character& c = s.civilian(3.0, kPi / 2.0, V3{0, 0, 0}, false);
    run(s, c, 0.6);
    c.push(V3{1, 0, 0}, 1.6);
    bool held = false;
    f64 palm = 1e300;
    run(s, c, 2.0, 0.0, [&](Character& x, f64, Host&) {
      if (x.behaviours.brace && x.behaviours.brace->holding) held = true;
      for (i32 hb : {H::handL, H::handR}) palm = std::min(palm, std::abs(x.pose.p[size_t(hb)].x - 0.6));
    });
    INFO("a hand at " << palm << " m from the wall");
    CHECK(held);
    CHECK(palm < 0.2);
    CHECK(!c.down());
  }
}

TEST_CASE("behaviour: a heavy blow drops the body; it stays down, then gets up") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    Scene s(path);
    Character& c = s.civilian();
    run(s, c, 0.5);
    const V3 head = c.pose.p[H::head];
    c.health = 5.0;
    c.melee(V3{head.x, head.y + 0.1, head.z + 0.05}, V3{0, -1, 0}, HitKind::Blunt, 2.3);
    CHECK(c.knocked_out);
    f64 lay = 0.0;
    run(s, c, 4.0, 0.0, [&](Character& x, f64, Host&) {
      if (x.behaviours.mode == BodyMode::Lying) lay += DT;
    });
    INFO("down " << lay << " s");
    CHECK(lay > 1.5);
    run(s, c, 12.0);
    CHECK(c.behaviours.mode == BodyMode::Animated);
    CHECK(!c.knocked_out);
  }
}

TEST_CASE("behaviour: the plan lies as the body lies (front or back), and a badly hurt body face down crawls away") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    bool crawled_away = false;
    for (f64 seed : {2.0, 4.0, 1.0, 3.0, 5.0, 6.0, 7.0}) {
      Scene s(path);
      Character& c = s.civilian(seed);
      run(s, c, 0.3);
      c.add_injury(H::thighL, 0.8);
      c.health = 26.0;
      const V3 ch = c.pose.p[H::chest];
      HitInfo hi;
      hi.point = V3{ch.x, ch.y - 0.1, ch.z};
      hi.dir = V3{0, 1, 0};
      hi.force = 3.0;
      hi.kind = HitKind::Blast;
      hi.bone = H::chest;
      c.hit_at(hi);
      c.push(V3{0, 1, 0}, 3.0);
      c.behaviours.collapse(10.0);
      bool crawled = false, consistent = true;
      Host h = run(s, c, 6.0, 0.0, [&](Character& x, f64, Host&) {
        if (x.behaviours.mode == BodyMode::Lying) {
          // the pelvis's forward points up on the back, down face down
          const V3 f = rotate(x.pose.q[H::pelvis], V3{0, 1, 0});
          if (std::abs(f.z) > 0.8) consistent = consistent && x.motion.lying_on_back() == (f.z > 0.0);
          if (!x.motion.lying_on_back()) x.motion.input.stance = Stance::Prone;
        }
        crawled = crawled || (x.behaviours.mode == BodyMode::Animated && x.motion.stance == Stance::Prone);
      });
      CHECK(consistent);
      if (!crawled) continue;
      // crawling: the host moves it slowly, the body stays low and goes with it
      const V3 p0 = c.pose.p[H::pelvis];
      run(s, c, 4.0, 0.35, nullptr, h);
      const V3 p1 = c.pose.p[H::pelvis];
      const f64 along = (p1.x - p0.x) * std::cos(h.yaw) + (p1.y - p0.y) * std::sin(h.yaw);
      INFO("crawling: head at " << c.pose.p[H::head].z << ", crawled " << along << " m");
      CHECK(c.pose.p[H::head].z < 0.45 + s.ground);
      CHECK(along > 0.8);
      crawled_away = true;
      break;
    }
    CHECK(crawled_away);
  }
}

TEST_CASE("behaviour: dying, the muscles fade, the body goes down within a couple of seconds and comes to rest") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    Scene s(path);
    Character& c = s.soldier();
    run(s, c, 0.5);
    const V3 ch = c.pose.p[H::chest];
    HitInfo hi;
    hi.point = V3{ch.x, ch.y + 0.12, ch.z + 0.1};
    hi.dir = V3{0, -1, 0};
    hi.force = 1.2;
    hi.kind = HitKind::Bullet;
    hi.bone = H::chest;
    c.hit_at(hi);
    c.die(nullptr, nullptr, 0.8);
    run(s, c, 2.0);
    INFO("head at " << c.pose.p[H::head].z);
    CHECK(c.pose.p[H::head].z < 0.5 + s.ground);
    CHECK(c.behaviours.mode == BodyMode::Dead);
    run(s, c, 4.0);
    CHECK(c.asleep());
  }
}

// ---- bodies among bodies ---------------------------------------------------------------------------

TEST_CASE("behaviour: a body shoved into a bystander knocks into it, and the bystander gives way") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    Scene s(path);
    Character& a = s.civilian(3.0, kPi / 2.0, V3{0, 0, 0});
    Character& b = s.civilian(5.0, kPi / 2.0, V3{0.7, 0, 0});
    Host hosts[2] = {host_of(a), host_of(b)};
    Character* cs[2] = {&a, &b};
    auto go = [&](f64 seconds, const std::function<void()>& each) {
      for (i32 i = 0; i < static_cast<i32>(seconds * 60.0); ++i) {
        s.frame({&a, &b}, [&] {
          gather_obstacles({&a, &b});
          for (i32 k = 0; k < 2; ++k) {
            const V3 rm = cs[k]->take_root_motion();
            if (cs[k]->controlled()) {
              hosts[k].pos.x += rm.x;
              hosts[k].pos.y += rm.y;
            }
            cs[k]->set_root(hosts[k].pos, hosts[k].yaw);
          }
        });
        if (each) each();
      }
    };
    go(0.5, nullptr);
    const f64 bx = b.pose.p[H::pelvis].x;
    a.push(V3{1, 0, 0}, 2.8);
    bool bumped = false;
    go(2.0, [&] {
      bumped = bumped || b.behaviours.mode != BodyMode::Animated;
      for (const RigidBody* p : b.body.parts) bumped = bumped || p->bumped > 0.0;
    });
    INFO("the bystander was pushed " << b.pose.p[H::pelvis].x - bx << " m");
    CHECK(bumped);
    CHECK(b.pose.p[H::pelvis].x - bx > 0.05);
    // they never pass through each other
    CHECK(b.pose.p[H::pelvis].x - a.pose.p[H::pelvis].x > 0.2);
  }
}

TEST_CASE("behaviour: walked into, a body gives way with a stumble; run into, it goes down") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    for (const auto& [speed, falls] : {std::pair<f64, bool>{1.5, false}, std::pair<f64, bool>{5.0, true}}) {
      INFO(speed << " m/s");
      Scene s(path);
      Character& c = s.civilian(2.0);
      // (from the side: something the body cannot move, the player's capsule at knee, hip and chest
      // height - obstacles on the shallow path, a kinematic body of the world on the deep one)
      V3 pl{-1.5, 0.1, s.ground};
      ArticulationId capsule = 0;
      if (path == Path::Deep) {
        ArticulationDesc d;
        LinkDesc L;
        L.mass = 0.0;  // (kinematic)
        L.pos = pl;
        for (f64 z : {0.58, 0.95, 1.3}) L.spheres.push_back(BodySphere{V3{0, 0, z}, 0.28});
        L.vel = V3{speed, 0, 0};
        d.links.push_back(L);
        capsule = s.world->add_articulation(d);
        REQUIRE(capsule != 0);
      }
      bool fell = false, reacted = false;
      run(s, c, 3.0, 0.0, [&](Character& x, f64, Host&) {
        pl.x += speed * DT;
        if (path == Path::Shallow) {
          std::vector<Obstacle> extra;
          for (f64 z : {0.58, 0.95, 1.3}) {
            Obstacle o;
            o.c = V3{pl.x, pl.y, z};
            o.r = 0.28;
            extra.push_back(o);
          }
          gather_obstacles({&x}, 2.2, extra);
        } else {
          s.world->set_link(capsule, 0, pl, Quat{}, V3{speed, 0, 0}, V3{});
        }
        reacted = reacted || x.behaviours.mode == BodyMode::Reacting;
        fell = fell || x.behaviours.mode == BodyMode::Falling;
      });
      CHECK(reacted);
      CHECK(fell == falls);
    }
  }
}

TEST_CASE("behaviour: a runner catches a foot on a body lying across the way") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    i32 tripped = 0;
    for (f64 seed : {1.0, 2.0, 3.0}) {
      Scene s(path);
      Character& dead = s.civilian(9.0, 0.0, V3{0.55, 2.5, 0});
      dead.die(nullptr, nullptr, 0.05);
      for (i32 i = 0; i < 180; ++i) s.frame({&dead});
      // (the runners run at where the body came to lie - how a body falls is chaotic - one at its
      // middle, one a little to either side)
      f64 mid = 0.0;
      for (const RigidBody* b : dead.body.parts) mid += b->x.x / static_cast<f64>(kBodyCount);
      Character& c = s.civilian(seed, 1.5707963267948966, V3{mid + 0.2 * (seed - 2.0), 0, 0});
      c.motion.input.mood = Mood::Panic;
      Host h = host_of(c);
      f64 v = 0.0;
      for (i32 i = 0; i < 120; ++i) {
        s.frame({&c, &dead}, [&] {
          gather_obstacles({&c, &dead});
          const V3 rm = c.take_root_motion();
          if (c.controlled()) {
            h.pos = h.pos + rm;
            h.yaw = c.motion.root_yaw;
            v = 0.0;
          } else {
            v = std::min(4.5, v + (4.5 / 0.5) * DT);
            h.pos.x += std::cos(h.yaw) * v * DT;
            h.pos.y += std::sin(h.yaw) * v * DT;
          }
          c.set_root(h.pos, h.yaw);
        });
        if (c.behaviours.mode == BodyMode::Reacting || c.down()) ++tripped;
      }
    }
    MESSAGE(pn << ": three runners at a body lying across the way, caught or down " << tripped << " frames of 360");
    CHECK(tripped > 0);
  }
}

TEST_CASE("behaviour: a leg shot off, the body goes down and does not stand again") {
  for (Path path : kPaths) {
    const std::string pn = path_name(path);
    INFO("path: " << pn);
    Scene s(path);
    Character& c = s.soldier();
    run(s, c, 1.0);
    auto shoot = [&](i32 bone, i32 times) {
      for (i32 k = 0; k < times; ++k) {
        const V3 p = c.pose.p[size_t(bone)], t = c.pose.tail(bone);
        const V3 mid = (p + t) * 0.5;
        const std::optional<CharacterHit> hit = c.raycast(V3{mid.x - 3.0, mid.y, mid.z}, V3{1, 0, 0}, 6.0);
        if (hit && hit->bone == bone) c.wound(*hit, V3{1, 0, 0}, 10.0, 0.07);
        run(s, c, 0.1);
      }
    };
    shoot(H::shinL, 3);
    CHECK(c.behaviours.legless);
    run(s, c, 12.0);
    CHECK(c.alive());
    INFO("pelvis at " << c.pose.p[H::pelvis].z);
    CHECK(c.pose.p[H::pelvis].z < 0.45 + s.ground);
    CHECK(c.behaviours.mode != BodyMode::Animated);
  }
}
