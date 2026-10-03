// svx_anim — physical causes of wounds, in SI units. Thermal is reserved.
#pragma once
#include <string>
#include "svx/anim/math.hpp"
namespace svx::anim {
enum class DamageKind : u8 { Projectile, Edge, Point, Blunt, Blast, Crush, Thermal };
enum class ProjectileConstruction : u8 { FullMetalJacket, Expanding, Buckshot };
struct DamageDescriptor {
  DamageKind kind = DamageKind::Blunt;
  u32 attacker = 0;
  u64 prop = 0;
  std::string feature;
  V3 point, direction{0, 1, 0}, edge_a, edge_b;
  f64 mass = .008, speed = 350, diameter = .009, area = .003;
  f64 sharpness = 1, alignment = 1, swept_length = .15, duration = 1.0 / 60;
  f64 radius = 3, pressure = 100000;
  f64 impact_scale = 1;  // projectile gameplay recoil; 1 preserves transferred momentum alone
  i32 fragments = 24;
  ProjectileConstruction construction = ProjectileConstruction::FullMetalJacket;
  i32 bone = -1;
  u64 target_prop = 0;
  bool blocked = false, impulse_delivered = false;
  f64 energy() const { return .5 * mass * speed * speed; }
  f64 momentum() const { return mass * speed; }
  bool valid() const;
};
}  // namespace svx::anim
