#include "svx/anim/characters/furniture.hpp"

#include "svx/anim/characters/palette.hpp"
#include "svx/anim/rig.hpp"
#include "svx/anim/voxel/sculpt.hpp"

namespace svx::anim {

namespace {

const AddOptions kHard{.organic = false};

// The chair of the desks and tables: legs, seat and back.
void add_chair(Sculptor& sc) {
  for (const f64 x : {-0.19, 0.19})
    for (const f64 y : {-0.18, 0.18}) sc.add(box(V3{x, y, 0.22}, V3{0.02, 0.02, 0.22}), 0, Slot::Metal, kHard);
  sc.add(box(V3{0, 0, 0.455}, V3{0.22, 0.21, 0.025}, 0.01), 0, Slot::Accent, kHard);
  sc.add(box(V3{0, -0.2, 0.72}, V3{0.21, 0.02, 0.17}, 0.02), 0, Slot::Accent, kHard);
}

}  // namespace

const Palette& furniture_palette() {
  static const Palette p = [] {
    PaletteSpec s;
    s.gear = 0x6b4a2e;
    s.gear_dark = 0x2c2f33;
    s.metal = 0x3d4146;
    s.furniture = 0x8a6440;
    s.detail = 0x1a1a1a;
    s.accent = 0x55606b;
    return make_palette(s);
  }();
  return p;
}

Furniture make_bench(f64 voxel_size) {
  Sculptor sc(prop_skeleton(), voxel_size, V3{-0.85, -0.35, 0}, V3{0.85, 0.3, 0.95}, 61);
  for (const f64 x : {-0.7, 0.7}) {
    sc.add(box(V3{x, 0.08, 0.22}, V3{0.03, 0.03, 0.22}), 0, Slot::Metal, kHard);
    sc.add(box(V3{x, -0.14, 0.44}, V3{0.03, 0.03, 0.44}), 0, Slot::Metal, kHard);
    sc.add(box(V3{x, -0.03, 0.42}, V3{0.03, 0.14, 0.02}), 0, Slot::Metal, kHard);
  }
  for (const f64 y : {-0.12, 0.0, 0.12}) sc.add(box(V3{0, y, 0.44}, V3{0.8, 0.045, 0.018}), 0, Slot::Furniture, {.organic = false, .jitter = 0.06});
  for (const f64 z : {0.6, 0.76}) sc.add(box(V3{0, -0.17, z}, V3{0.8, 0.016, 0.055}), 0, Slot::Furniture, {.organic = false, .jitter = 0.06});
  Furniture f;
  f.model = sc.finish({.name = "bench"});
  f.palette = furniture_palette();
  f.seat = V3{0, -0.02, 0.46};
  f.backrest = true;
  f.kind = FurnitureKind::Bench;
  return f;
}

Furniture make_chair(f64 voxel_size) {
  Sculptor sc(prop_skeleton(), voxel_size, V3{-0.28, -0.3, 0}, V3{0.28, 0.28, 1.0}, 62);
  add_chair(sc);
  for (const f64 x : {-0.19, 0.19}) sc.add(box(V3{x, -0.2, 0.62}, V3{0.02, 0.02, 0.18}), 0, Slot::Metal, kHard);
  Furniture f;
  f.model = sc.finish({.name = "chair"});
  f.palette = furniture_palette();
  f.seat = V3{0, 0.02, 0.48};
  f.backrest = true;
  f.kind = FurnitureKind::Chair;
  return f;
}

Furniture make_desk(f64 voxel_size) {
  Sculptor sc(prop_skeleton(), voxel_size, V3{-0.7, -0.35, 0}, V3{0.7, 1.0, 1.0}, 63);
  // desk top and legs, in front of the chair
  sc.add(box(V3{0, 0.62, 0.725}, V3{0.62, 0.32, 0.02}), 0, Slot::Furniture, {.organic = false, .jitter = 0.05});
  for (const f64 x : {-0.58, 0.58}) sc.add(box(V3{x, 0.62, 0.36}, V3{0.025, 0.28, 0.35}), 0, Slot::Metal, kHard);
  // a monitor or papers
  sc.add(box(V3{0.1, 0.82, 0.9}, V3{0.22, 0.02, 0.15}), 0, Slot::Detail, kHard);
  sc.add(box(V3{0.1, 0.84, 0.76}, V3{0.03, 0.02, 0.02}), 0, Slot::Metal, kHard);
  sc.add(box(V3{-0.25, 0.5, 0.75}, V3{0.1, 0.13, 0.006}), 0, Slot::Bone, kHard);
  // the chair
  add_chair(sc);
  Furniture f;
  f.model = sc.finish({.name = "desk"});
  f.palette = furniture_palette();
  f.seat = V3{0, 0.02, 0.48};
  f.desk_height = 0.745;
  f.backrest = true;
  f.kind = FurnitureKind::Desk;
  return f;
}

Furniture make_cafe_table(f64 voxel_size) {
  Sculptor sc(prop_skeleton(), voxel_size, V3{-0.5, -0.35, 0}, V3{0.5, 1.05, 1.0}, 67);
  // a round-ish top on one pedestal
  sc.add(box(V3{0, 0.7, 0.725}, V3{0.36, 0.3, 0.02}, 0.08), 0, Slot::Furniture, {.organic = false, .jitter = 0.05});
  sc.add(box(V3{0, 0.7, 0.36}, V3{0.03, 0.03, 0.35}), 0, Slot::Metal, kHard);
  sc.add(box(V3{0, 0.7, 0.02}, V3{0.2, 0.2, 0.02}, 0.02), 0, Slot::Metal, kHard);
  // an open laptop and a cup
  sc.add(box(V3{0.02, 0.55, 0.755}, V3{0.15, 0.1, 0.008}), 0, Slot::GearDark, kHard);
  sc.add(box(V3{0.02, 0.66, 0.85}, V3{0.15, 0.012, 0.1}), 0, Slot::GearDark, kHard);
  sc.add(box(V3{0.02, 0.648, 0.85}, V3{0.13, 0.004, 0.085}), 0, Slot::Detail, {.organic = false, .shade = 1.3});
  sc.add(box(V3{-0.26, 0.62, 0.785}, V3{0.035, 0.035, 0.045}, 0.01), 0, Slot::Bone, kHard);
  // the chair
  add_chair(sc);
  Furniture f;
  f.model = sc.finish({.name = "cafe table"});
  f.palette = furniture_palette();
  f.seat = V3{0, 0.02, 0.48};
  f.desk_height = 0.745;
  f.backrest = true;
  f.kind = FurnitureKind::Desk;
  f.approach = 0.2;
  return f;
}

}  // namespace svx::anim
