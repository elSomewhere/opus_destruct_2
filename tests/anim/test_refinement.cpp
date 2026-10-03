#include "scene.hpp"
#include <limits>

using namespace scene;

TEST_CASE("refinement: folded two-bone IK keeps a continuous elbow frame") {
  auto sk=humanoid_skeleton();Pose p(sk);ModelFK fk(sk);
  Quat previous;bool first=true;
  for(int i=0;i<100;++i){
    p.reset();fk.update(p);
    const V3 target=fk.p[H::upperarmR]+V3{0,.18+i*.0035,-.04};
    solve_two_bone(p,fk,H::upperarmR,H::forearmR,H::handR,target,V3{1,0,-1},0,&kElbowRest);
    CHECK(norm(fk.p[H::handR]-target)<1e-5);
    if(!first)CHECK(norm(qerror(p.r[H::forearmR],previous))<.12);
    previous=p.r[H::forearmR];first=false;
  }
}

TEST_CASE("refinement: stance hands fade continuously when a target is absent") {
  StanceSample a,b,out;b.hands[0]=V3{0,.3,.4};a.hands[0].reset();
  for(double w:{.1,.49,.5,.51,.9}){
    blend_samples(a,b,w,out);REQUIRE(out.hands[0]);CHECK(out.hand_weight[0]==doctest::Approx(w));
    blend_samples(b,a,w,out);CHECK(out.hand_weight[0]==doctest::Approx(1-w));
  }
}

TEST_CASE("refinement: standalone strikes aim forward and manual poses remain held") {
  FlatGround ground;MotionPlan p(humanoid_skeleton(),&ground,7);p.place(V3{},kPi/2);p.input.idle=false;
  CHECK_FALSE(p.play("slash"));CHECK_FALSE(p.play("jab",std::nullopt,0));
  CHECK_FALSE(p.play("jab",std::nullopt,std::numeric_limits<double>::infinity()));
  REQUIRE(p.play("jab"));bool struck=false;
  for(int i=0;i<120;++i){p.update(DT);for(const auto& e:p.take_events())if(e.name=="strike"){
    REQUIRE(e.target);CHECK(e.target->y>.5);CHECK(e.pos.y>.4);struck=true;
  }}
  CHECK(struck);REQUIRE(p.play("handsOnHips"));
  for(int i=0;i<240;++i)p.update(DT);
  CHECK(p.pose_action_name()=="handsOnHips");p.interrupt();
  for(int i=0;i<60;++i)p.update(DT);
  CHECK(p.pose_action_name().empty());
  p.weapon=prop_archetype("knife");REQUIRE(p.play("slash.m"));p.update(DT);CHECK(p.weapon_hand==H::handL);
  CHECK_FALSE(p.play("jab",V3{0,std::numeric_limits<double>::quiet_NaN(),1}));
}

TEST_CASE("refinement: mirrored knife ownership follows the left weapon hand") {
  Scene s(Path::Shallow);auto& c=s.add(make_civilian(7),7,kPi/2,V3{},prop_archetype("knife"));run(s,c,.5);
  REQUIRE(c.motion.play("slash.m"));c.behaviours.lose_limb(B::handR);CHECK_FALSE(c.gun_hand_lost());
  c.behaviours.lose_limb(B::handL);CHECK(c.gun_hand_lost());
}

TEST_CASE("refinement: strike easing accelerates continuously from rest") {
  Track t({Key{0,{0}},Key{1,{1},Ease::Snap}});double a[1],b[1];
  t.sample(0,a);t.sample(.0001,b);CHECK((b[0]-a[0])/.0001<.01);
  t.sample(.33,a);CHECK(a[0]>.75);t.sample(.9999,a);t.sample(1,b);CHECK((b[0]-a[0])/.0001<.01);
}

TEST_CASE("refinement: interruption during preparation fades monotonically and cancels events") {
  ActionPlayer player(action_def("jab"));player.preparation=.18;player.time=-.18;
  player.advance(.05);double previous=player.weight();player.stop();
  std::vector<const ActionEvent*> events;
  for(int i=0;i<60;++i){player.advance(DT,&events);CHECK(player.weight()<=previous+1e-12);previous=player.weight();}
  CHECK(player.done());CHECK(events.empty());
  ActionPlayer strike(action_def("jab"));strike.advance(.1);strike.stop();strike.advance(1,&events);CHECK(events.empty());
}

namespace {
V3 momentum(const Character& c){V3 p;for(const auto& b:c.body.parts)p+=b->v*b->mass;return p;}
}
TEST_CASE("refinement: localized hits conserve requested linear momentum including vertical impact") {
  for(Path path:{Path::Shallow,Path::Deep})for(HitKind kind:{HitKind::Bullet,HitKind::Blunt,HitKind::Blade,HitKind::Blast}){
    Scene s(path);auto& c=s.civilian(7);run(s,c,.5);
    HitInfo hit;hit.bone=H::chest;hit.kind=kind;hit.force=1;hit.impulse_ns=5;
    hit.dir=vnorm(V3{.4,-1,.5});hit.point=c.body.parts[B::chest]->x+V3{.08,.06,.1};
    const V3 before=momentum(c),spin=c.body.parts[B::chest]->w;c.hit_at(hit);
    CHECK(norm(momentum(c)-before-hit.dir*5)<1e-8);
    CHECK(norm(c.body.parts[B::chest]->w-spin)>.01);
    // Stun/pain may change later muscle forces, but cannot add momentum at impact.
    hit.force=2;hit.impulse_ns=0;const V3 after=momentum(c);c.hit_at(hit);
    CHECK(norm(momentum(c)-after)<1e-8);
    const auto wounds = c.behaviours.damage.wound_count();
    hit.force = 0;
    c.hit_at(hit);
    CHECK(c.behaviours.damage.wound_count() == wounds);
    run(s,c,.5);
  }
}

TEST_CASE("refinement: wound momentum applies equally before and after death") {
  for(Path path:{Path::Shallow,Path::Deep})for(bool dead:{false,true}){
    Scene s(path);auto& c=s.civilian(7);run(s,c,.5);
    const V3 p=c.pose.p[H::chest];const V3 dir{0,-1,0};
    const auto hit=c.raycast(p+V3{0,3,.05},dir,6);REQUIRE(hit);
    if(dead)c.die(nullptr,nullptr,0);
    const V3 before=momentum(c);c.wound(*hit,dir,1,.005,8);
    CHECK(norm(momentum(c)-before-dir*8)<1e-8);
  }
}
