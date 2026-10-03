#include "scene.hpp"
#include "svx/anim/damage/mechanics.hpp"
using namespace scene;

TEST_CASE("projectile drag transfers more momentum at higher speed without creating energy") {
  VoxelPart p;
  p.bone=H::chest;p.origin={-2,0,-2};p.dims={5,8,5};
  p.cells.resize(200,Slot::Flesh+1);p.count=p.initial_count=200;
  const auto skeleton=make_civilian(3).model->skeleton;
  std::array<f32,23*16> skin{};
  for(int i=0;i<23;i++)write_rigid(skin.data()+i*16,{},Quat{},{});
  f64 previous=0;
  for(f64 speed:{200.,350.,700.,1000.}){
    VoxelModel model(skeleton,.025,{p});
    DamageDescriptor d;d.kind=DamageKind::Projectile;d.point={.0125,0,.0125};d.direction={0,1,0};d.mass=.008;d.speed=speed;d.diameter=.009;
    const auto result=wound_mechanics(model,skin,d);
    const f64 impulse=norm(result.impulse(d));
    CAPTURE(speed);CAPTURE(impulse);REQUIRE(!result.removed.empty());
    CHECK(impulse>previous);CHECK(impulse<=d.momentum());previous=impulse;
    f64 energy=result.remaining_energy;for(const auto& t:result.tissue)energy+=t.energy;
    CHECK(energy==doctest::Approx(d.energy()).epsilon(1e-10));
  }
}

TEST_CASE("projectile impact changes physical momentum at the selected limb on both bodies") {
  for(Path path:{Path::Shallow,Path::Deep}){
    Scene scene(path);auto& c=scene.civilian();run(scene,c,1.5);
    const V3 at=c.body.parts[B::thighL]->x;
    DamageDescriptor d;d.kind=DamageKind::Projectile;d.mass=.008;d.speed=350;d.diameter=.009;d.direction={0,-1,0};d.point=at+V3{0,.5,0};d.bone=H::thighL;
    auto hit=c.raycast(d.point,d.direction,1);REQUIRE(hit);d.point=hit->point;
    const V3 before=c.body.com_velocity();const auto result=c.damage(d);
    CHECK(norm(result.impulse)>.2);CHECK(dot(c.body.com_velocity()-before,d.direction)>0);
    const V3 expected=result.impulse*(1/c.body.total_mass);
    CHECK(norm(c.body.com_velocity()-before-expected)<1e-8);
    run(scene,c,10./60);
    CHECK(c.behaviours.mode!=BodyMode::Animated);
    for(const auto& p:c.pose.p)CHECK(std::isfinite(norm(p)));
  }
}
