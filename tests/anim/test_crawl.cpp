#include "doctest.h"
#include "scene.hpp"
#include "svx/anim/motion/crawl.hpp"
#include "svx/anim/motion/travel.hpp"
#include "svx/anim/damage/scenarios.hpp"
using namespace scene;

TEST_CASE("crawl: hands take turns reaching while the supporting palm stays on its surface") {
  FlatGround floor;
  CrawlHands planner;
  int reaches[2]={};
  for(int i=0;i<600;++i) {
    const V3 root{0,i*DT*.2,0};
    const auto previous=planner.hands;
    const std::array<V3,2> initial{root+V3{-.17,.6,.07},root+V3{.17,.6,.07}};
    planner.update(DT,root,kPi/2,1,{0,.2,0},{true,true},initial,floor);
    CHECK((planner.hands[0].planted || planner.hands[1].planted));
    for(size_t side=0;side<2;++side) {
      const auto& h=planner.hands[side];
      if(previous[side].planted && !h.planted)++reaches[side];
      if(previous[side].active && previous[side].planted && h.planted)CHECK(norm(h.pos-previous[side].pos)<1e-12);
      CHECK(h.pos.z>=.06-1e-10);
      CHECK(h.pos.z<.15);
    }
  }
  CHECK(reaches[0]>=5);CHECK(reaches[1]>=5);
  planner.update(DT,{0,2,0},kPi/2,1,{}, {false,true},{{V3{-.17,2.6,.07},V3{.17,2.6,.07}}},floor);
  CHECK_FALSE(planner.hands[0].active);
  CHECK(planner.hands[1].active);
}

TEST_CASE("crawl: a missing surface cannot become an invisible hand support") {
  VoxelCollision edge(.125,[](i32 x,i32,i32 z){return x<0 && z<=0;});
  CrawlHands planner;
  planner.update(DT,{0,0,.0625},kPi/2,1,{}, {true,true},{{V3{-.2,.6,.13},V3{.2,.6,.13}}},edge);
  CHECK(planner.hands[0].active);
  CHECK_FALSE(planner.hands[1].active);
}

TEST_CASE("crawl: shotgun recovery physically turns the trunk before travel resumes") {
  for(Path path:{Path::Shallow,Path::Deep}) {
    Scene s(path);auto& c=s.civilian(7);c.physics=true;c.motion.input.idle=false;
    TravelState travel;travel.root={0,0,s.ground};
    auto tick=[&](f64 speed) {
      s.frame({&c},[&]{travel.update(c,*s.col,speed,kPi/2,DT);c.set_root(travel.root,travel.yaw);});
    };
    for(int i=0;i<90;++i)tick(0);
    for(const auto& d:damage_scenario(c,"shotgunLegs"))c.damage(d);
    int active=0;V3 start;f64 side2=0,peak_delta=0;V3 previous=c.body.com();
    for(int i=0;i<1200;++i) {
      tick(.3);
      if(c.behaviours.mode==BodyMode::Animated && c.motion.stance==Stance::Prone) {
        CHECK(rotate(c.pose.q[H::pelvis],V3{0,1,0}).z<-.55);
        ++active;
      }
      if(i==900)start=c.body.com();
      if(i>=900) { const V3 delta=c.body.com()-previous;side2+=delta.x*delta.x;peak_delta=std::max(peak_delta,norm(delta)); }
      previous=c.body.com();
    }
    INFO(path_name(path));
    CHECK(active>600);
    CHECK(c.body.com().y-start.y>.5);
    CHECK(std::sqrt(side2/300)*60<.08);
    CHECK(peak_delta<.015);
  }
}
