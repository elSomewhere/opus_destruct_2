#include "doctest.h"
#include "svx/procgen/domain.hpp"
#include "svx/anim/system.hpp"
#include "svx/anim/characters/humans.hpp"
#include "svx/anim/motion/travel.hpp"

using namespace svx;
using namespace svx::anim;

TEST_CASE("domains: roadless worlds own characters, physical drops and repeatable terrain") {
  for (bool deep : {false, true}) {
    auto run = [&]() {
      Game game;
      DomainConfig domain;
      domain.terrain = DomainTerrain::Stairs;
      domain.seed = 17;
      domain.loose_objects = 3;
      load_domain(game, domain);
      auto& system = game.ensure_characters();
      system.config.policy = deep ? BodyPolicy::Deep : BodyPolicy::Shallow;
      system.config.parallel = false;
      auto look = make_civilian(7);
      CharacterDesc desc;
      desc.model = look.model;desc.palette = look.palette;
      desc.pos = {0,0,.0625};desc.yaw = kPi/2;
      auto* c = system.get(system.spawn(desc));
      REQUIRE(c);c->physics = true;
      TravelState travel;travel.root = desc.pos;
      f64 high = 0;
      for (int tick = 0; tick < 300; ++tick) {
        travel.update(*c, *system.collision(), 1.2, kPi/2, 1./60);
        c->set_root(travel.root, travel.yaw);
        game.tick();
        high = std::max(high, c->pose.p[H::pelvis].z);
        for (const auto& p : c->pose.p) REQUIRE(std::isfinite(norm(p)));
      }
      CHECK(c->bound() == deep);
      CHECK(travel.root.y > 3);
      CHECK(high > 1.3);
      CHECK_FALSE(game.pieces().empty());
      return game.world().session_hash();
    };
    CHECK(run() == run());
  }
}

TEST_CASE("travel: a wall stops root requests and a prone body turns gradually") {
  FlatGround ground;
  auto look = make_civilian(2);
  CharacterOptions options;options.model=look.model;options.collision=&ground;
  Character c(options);
  TravelState travel;
  c.motion.input.stance = Stance::Prone;
  for(int i=0;i<60;++i) travel.update(c,ground,.3,-kPi/2,1./60);
  CHECK(std::abs(wrap_angle(travel.yaw-kPi/2)) <= .651);
  VoxelCollision wall(.125,[](i32,i32 y,i32 z){return z<=0 || (y==20 && z<20);});
  travel=TravelState{};travel.root.z=.0625;c.motion.input.stance=Stance::Stand;
  for(int i=0;i<240;++i)travel.update(c,wall,1.2,kPi/2,1./60);
  CHECK(travel.blocked);CHECK(travel.root.y<2.4);CHECK(travel.speed==0);
}
