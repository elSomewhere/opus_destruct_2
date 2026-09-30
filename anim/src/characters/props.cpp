#include "svx/anim/characters/props.hpp"

#include <string>

#include "svx/anim/rig.hpp"
#include "svx/anim/voxel/sculpt.hpp"

namespace svx::anim {

PropPtr make_rifle(f64 voxel_size, i32 variant) {
  const f64 s = voxel_size;
  const f64 L = 0.8;  // (the lengths below were drawn for a 1.1 m rifle)
  auto y = [L](f64 v) { return v * L; };
  Sculptor sc(prop_skeleton(), s, V3{-0.06, -0.4, -0.2}, V3{0.06, 0.6, 0.2}, static_cast<i32>(11u + static_cast<u32>(variant)));
  const u8 M = Slot::Metal, F = Slot::Furniture, D = Slot::GearDark;
  const AddOptions o{.organic = false};
  // receiver
  sc.add(box(V3{0, y(0.06), 0.064}, V3{0.021, y(0.15), 0.032}, 0.006), 0, M, o);
  // stock and butt pad
  sc.add(box(V3{0, y(-0.25), 0.05}, V3{0.019, y(0.17), 0.036}, 0.01), 0, F, o);
  sc.add(box(V3{0, y(-0.2), 0.014}, V3{0.017, y(0.1), 0.02}, 0.008), 0, F, o);
  sc.add(box(V3{0, y(-0.425), 0.045}, V3{0.021, 0.012, 0.048}, 0.004), 0, D, o);
  // pistol grip (raked back) and trigger guard
  sc.add(box(V3{0, -0.012, -0.032}, V3{0.017, 0.02, 0.048}, 0.008), 0, F, o);
  sc.add(box(V3{0, 0.028, 0.012}, V3{0.011, 0.026, 0.011}, 0.004), 0, M, o);
  // curved magazine
  sc.add(box(V3{0, y(0.115), -0.028}, V3{0.016, 0.026, 0.056}, 0.006), 0, M, {.organic = false, .shade = 0.8});
  sc.add(box(V3{0, y(0.135) + 0.01, -0.094}, V3{0.015, 0.026, 0.032}, 0.006), 0, M, {.organic = false, .shade = 0.8});
  // handguard, barrel, gas block, front sight, muzzle
  sc.add(box(V3{0, y(0.33), 0.062}, V3{0.025, y(0.12), 0.029}, 0.01), 0, F, o);
  sc.add(cylinder(V3{0, y(0.44), 0.068}, V3{0, y(0.66), 0.068}, 0.012), 0, M, o);
  sc.add(box(V3{0, y(0.46), 0.074}, V3{0.014, 0.018, 0.02}), 0, M, o);
  sc.add(box(V3{0, y(0.44), 0.115}, V3{0.006, 0.01, 0.028}), 0, M, o);
  sc.add(cylinder(V3{0, y(0.62), 0.068}, V3{0, y(0.67), 0.068}, 0.016), 0, D, o);
  // rear sight / optic rail
  sc.add(box(V3{0, y(0.08), 0.106}, V3{0.013, y(0.05), 0.013}), 0, D, o);
  if (variant % 2 == 1) sc.add(cylinder(V3{0, y(0.03), 0.13}, V3{0, y(0.14), 0.13}, 0.021), 0, D, o);  // scope
  auto p = std::make_shared<Prop>();
  p->model = sc.finish({.name = "rifle-" + std::to_string(variant)});
  p->grip = V3{0, 0, 0};
  p->support = V3{0, y(0.3), 0.03};
  p->stock = V3{0, y(-0.43), 0.05};
  p->muzzle = V3{0, y(0.68), 0.068};
  p->magazine = V3{0, y(0.12), -0.06};
  p->kind = PropKind::Rifle;
  return p;
}

PropPtr make_smg(f64 voxel_size) {
  const f64 s = voxel_size;
  Sculptor sc(prop_skeleton(), s, V3{-0.06, -0.34, -0.2}, V3{0.06, 0.5, 0.2}, 21);
  const u8 M = Slot::Metal, F = Slot::Furniture, D = Slot::GearDark;
  const AddOptions o{.organic = false};
  sc.add(box(V3{0, 0.08, 0.06}, V3{0.022, 0.13, 0.034}, 0.008), 0, M, o);
  sc.add(box(V3{0, -0.19, 0.055}, V3{0.012, 0.13, 0.02}, 0.006), 0, D, o);
  sc.add(box(V3{0, -0.31, 0.05}, V3{0.02, 0.012, 0.04}), 0, D, o);
  sc.add(box(V3{0, -0.01, -0.03}, V3{0.018, 0.022, 0.048}, 0.008), 0, F, o);
  sc.add(box(V3{0, 0.1, -0.06}, V3{0.016, 0.02, 0.07}, 0.005), 0, M, {.organic = false, .shade = 0.8});
  sc.add(cylinder(V3{0, 0.2, 0.066}, V3{0, 0.4, 0.066}, 0.014), 0, M, o);
  sc.add(box(V3{0, 0.26, 0.05}, V3{0.02, 0.06, 0.02}, 0.006), 0, F, o);
  auto p = std::make_shared<Prop>();
  p->model = sc.finish({.name = "smg"});
  p->grip = V3{0, 0, 0};
  p->support = V3{0, 0.25, 0.025};
  p->stock = V3{0, -0.3, 0.05};
  p->muzzle = V3{0, 0.41, 0.066};
  p->magazine = V3{0, 0.1, -0.08};
  p->kind = PropKind::Smg;
  return p;
}

PropPtr make_lmg(f64 voxel_size) {
  Sculptor sc(prop_skeleton(), voxel_size, V3{-0.08, -0.45, -0.2}, V3{0.08, 0.66, 0.2}, 31);
  const u8 M = Slot::Metal, F = Slot::Furniture, D = Slot::GearDark;
  const AddOptions o{.organic = false};
  sc.add(box(V3{0, 0.07, 0.066}, V3{0.026, 0.16, 0.038}, 0.006), 0, M, o);
  sc.add(box(V3{0, -0.25, 0.05}, V3{0.022, 0.15, 0.042}, 0.012), 0, F, o);
  sc.add(box(V3{0, -0.39, 0.045}, V3{0.024, 0.012, 0.055}, 0.004), 0, D, o);
  sc.add(box(V3{0, -0.012, -0.03}, V3{0.018, 0.02, 0.046}, 0.008), 0, F, o);
  sc.add(box(V3{0.03, 0.1, -0.02}, V3{0.03, 0.06, 0.05}, 0.008), 0, D, o);  // box magazine
  sc.add(box(V3{0, 0.33, 0.066}, V3{0.03, 0.1, 0.034}, 0.01), 0, M, o);
  sc.add(cylinder(V3{0, 0.4, 0.07}, V3{0, 0.64, 0.07}, 0.016), 0, M, o);
  sc.add(box(V3{0, 0.5, 0.045}, V3{0.008, 0.12, 0.01}), 0, D, o);  // folded bipod
  sc.add(box(V3{0, 0.08, 0.115}, V3{0.02, 0.06, 0.012}), 0, D, o);
  sc.add(box(V3{0, 0.2, 0.11}, V3{0.006, 0.012, 0.04}), 0, D, o);  // carry handle post
  auto p = std::make_shared<Prop>();
  p->model = sc.finish({.name = "lmg"});
  p->grip = V3{0, 0, 0};
  p->support = V3{0, 0.25, 0.03};
  p->stock = V3{0, -0.39, 0.05};
  p->muzzle = V3{0, 0.65, 0.07};
  p->magazine = V3{0.03, 0.1, -0.05};
  p->kind = PropKind::Lmg;
  return p;
}

// Small props are sculpted at half the body's voxel size (a 3 cm slide would be under one body
// voxel); the support hand of a pistol wraps the gripping hand.
PropPtr make_pistol(f64 voxel_size) {
  Sculptor sc(prop_skeleton(), voxel_size, V3{-0.05, -0.08, -0.14}, V3{0.05, 0.24, 0.1}, 41);
  const u8 M = Slot::Metal, D = Slot::GearDark;
  const AddOptions o{.organic = false};
  // (thin parts are centred on a voxel column so they keep at least one voxel)
  const f64 c = voxel_size / 2.0;
  sc.add(box(V3{0, 0.07, 0.042}, V3{0.016, 0.1, 0.022}, 0.004), 0, M, o);     // slide
  sc.add(box(V3{0, -0.01, -0.035}, V3{0.016, 0.024, 0.055}, 0.006), 0, D, o);  // grip
  sc.add(box(V3{c, 0.03, 0.0}, V3{0.006, 0.026, 0.01}, 0.003), 0, M, o);       // trigger guard
  sc.add(box(V3{c, 0.17, 0.045}, V3{0.006, 0.012, 0.008}), 0, M, o);           // muzzle
  auto p = std::make_shared<Prop>();
  p->model = sc.finish({.name = "pistol"});
  p->grip = V3{0, 0, 0};
  p->support = V3{-0.018, -0.01, -0.035};
  p->stock = V3{0, -0.03, 0.02};
  p->muzzle = V3{0, 0.18, 0.045};
  p->magazine = V3{0, -0.01, -0.08};
  p->kind = PropKind::Pistol;
  p->one_handed = true;
  return p;
}

// A fighting knife, held blade forward along the fingers (prop +y); fine voxels like the pistol.
PropPtr make_knife(f64 voxel_size) {
  Sculptor sc(prop_skeleton(), voxel_size, V3{-0.04, -0.08, -0.05}, V3{0.04, 0.26, 0.05}, 51);
  const AddOptions o{.organic = false};
  const f64 c = voxel_size / 2.0;
  sc.add(box(V3{0, -0.02, 0}, V3{0.016, 0.05, 0.016}, 0.004), 0, Slot::GearDark, o);                               // handle
  sc.add(box(V3{0, 0.04, 0}, V3{0.02, 0.008, 0.024}), 0, Slot::Metal, o);                                           // guard
  sc.add(box(V3{c, 0.13, 0.004}, V3{0.005, 0.085, 0.017}, 0.002), 0, Slot::Bone, {.organic = false, .shade = 1.15});  // blade (one voxel thin)
  auto p = std::make_shared<Prop>();
  p->model = sc.finish({.name = "knife"});
  p->grip = V3{0, 0, 0};
  p->support = V3{0, 0, 0};
  p->stock = V3{0, -0.07, 0};
  p->muzzle = V3{0, 0.22, 0.004};
  p->magazine = V3{0, 0, 0};
  p->kind = PropKind::Knife;
  p->one_handed = true;
  return p;
}

}  // namespace svx::anim
