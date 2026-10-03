// A leg without support capacity cannot supply the other half of a walking gait.
#include "scene.hpp"

using namespace scene;

TEST_CASE("mobility: one usable leg hops without planting or balancing on the disabled leg") {
  for (Path path : {Path::Shallow, Path::Deep})
    for (bool physical : {false, true})
      for (size_t bad : {size_t(0), size_t(1)}) for (f64 control : {0.0, .7}) {
        Scene s(path);
        auto& c = s.civilian(7);
        c.physics = physical;
        c.motion.input.idle = false;
        c.motion.style = kNeutralStyle;
        run(s, c, 1);
        auto cap = c.capabilities();
        cap.legs[bad] = {0, 0, control};
        const size_t thigh = bad ? B::thighR : B::thighL;
        for (size_t i = thigh; i < thigh + 3; ++i) cap.muscle[i] = control;
        derive_mobility(cap);
        c.behaviours.damage.override_capabilities(cap);
        auto host = run(s, c, 2);
        const V3 start = c.pose.p[H::pelvis];
        int bad_contacts = 0, good_landings = 0, flight = 0, clear = 0;
        f64 lock_error = 0, low = 10, high = 0;
        V3 last;
        bool planted = false;
        run(s, c, 5, .3, [&](Character& x, f64 t, Host&) {
          if (t < 1) return;
          const auto& good = x.motion.feet_planner.feet[1 - bad];
          if (x.motion.feet_planner.feet[bad].planted) ++bad_contacts;
          if (x.motion.feet_planner.landed[1 - bad]) ++good_landings;
          if (!good.planted) ++flight;
          const size_t foot = bad ? H::footL : H::footR;
          const auto& dims = x.motion.feet_planner.dims;
          const V3 heel = x.pose.p[foot] + rotate(x.pose.q[foot], V3{0, -dims.heel_back, -dims.ankle_h});
          const V3 toe = x.pose.p[foot] + rotate(x.pose.q[foot], V3{0, dims.ball_fwd, -dims.ankle_h});
          if (!good.planted && std::min(heel.z, toe.z) - s.ground > .02) ++clear;
          if (planted && good.planted) lock_error = std::max(lock_error, norm(good.pos - last));
          planted = good.planted;
          last = good.pos;
          low = std::min(low, x.pose.p[H::pelvis].z - s.ground);
          high = std::max(high, x.pose.p[H::pelvis].z - s.ground);
        }, host);
        MESSAGE(std::string(path_name(path)) << " physical " << physical << " bad " << bad << " control " << control << ": landings " << good_landings
                << ", flight " << flight << ", clear " << clear << ", pelvis " << low << ".." << high << ", progress " << c.pose.p[H::pelvis].y - start.y);
        CHECK(bad_contacts == 0);
        CHECK(good_landings >= 3);
        CHECK(flight >= 20);
        CHECK(clear >= 10);
        CHECK(lock_error < 1e-8);
        CHECK(low > .65);
        CHECK(high - low > .025);
        CHECK(c.pose.p[H::pelvis].y - start.y > .8);
        CHECK(c.behaviours.mode == BodyMode::Animated);
      }
}

TEST_CASE("mobility: restoring leg support places the foot smoothly before walking again") {
  FlatGround ground;
  MotionPlan p(humanoid_skeleton(), &ground, 7);
  p.place({}, kPi / 2);
  p.input.idle = false;
  p.capabilities.legs[0] = {0, 0, .7};
  derive_mobility(p.capabilities);
  for (int i = 0; i < 120; ++i) p.update(DT);
  REQUIRE(p.feet_planner.feet[0].unloaded);
  const V3 before = p.world.p[H::footL];
  p.capabilities = Capabilities{};
  p.update(DT);
  CHECK(norm(p.world.p[H::footL] - before) < .03);
  for (int i = 0; i < 60; ++i) p.update(DT);
  CHECK_FALSE(p.feet_planner.feet[0].unloaded);
  CHECK(p.feet_planner.feet[0].planted);
  CHECK(std::abs(p.feet_planner.feet[0].pos.z) < 1e-8);
}
