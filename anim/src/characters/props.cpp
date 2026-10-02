#include "svx/anim/characters/props.hpp"

#include <string>

#include "svx/anim/rig.hpp"
#include "svx/anim/voxel/sculpt.hpp"

namespace svx::anim {
bool Prop::has(std::string_view tag) const { return std::find(tags.begin(), tags.end(), tag) != tags.end(); }
bool Prop::satisfies(const std::vector<std::string>& req) const {
  for (const auto& tag : req)
    if (!has(tag)) return false;
  return true;
}
const PropSocket* Prop::socket(std::string_view id) const {
  for (const auto& s : sockets)
    if (s.id == id) return &s;
  return nullptr;
}
const ContactFeature* Prop::feature(std::string_view id) const {
  for (const auto& f : features)
    if (f.id == id) return &f;
  return nullptr;
}

static void finish_prop(Prop& p) {
  if (!p.has("non_combat")) p.tags.push_back("blocking");
  p.points = {{"muzzle", p.muzzle}, {"stock", p.stock}, {"magazine", p.magazine}, {"tip", p.muzzle}, {"pommel", p.stock}, {"handle", p.grip}};
  p.sockets = {{"primary", p.grip, {}, 350}, {"secondary", p.support, {}, 300}, {"grasp", p.grip, {}, 350}, {"wear", {}, {}, 900}};
  if (p.has("edged")) {
    p.sockets.push_back({"reverse", p.grip, qx(kPi), 300});
    p.features = {{"edge", ImpactClass::Edge, {0, .05, 0}, p.muzzle, .004, 1}, {"tip", ImpactClass::Point, p.muzzle, p.muzzle, .005, 1}};
  } else
    p.features = {{"stock", ImpactClass::Blunt, p.stock, p.stock, .035, 0}, {"muzzle", ImpactClass::Blunt, p.muzzle, p.muzzle, .02, 0}};
  V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
  f64 count = 0;
  const f64 s = p.model->voxel_size;
  for (const auto& part : p.model->parts)
    for (i32 z = 0; z < part.dims[2]; ++z)
      for (i32 y = 0; y < part.dims[1]; ++y)
        for (i32 x = 0; x < part.dims[0]; ++x) {
          if (!part.cells[size_t(x + part.dims[0] * (y + part.dims[1] * z))]) continue;
          const V3 v{(part.origin[0] + x + .5) * s, (part.origin[1] + y + .5) * s, (part.origin[2] + z + .5) * s};
          p.centre += v;
          ++count;
          for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], v[a] - s * .5);
            hi[a] = std::max(hi[a], v[a] + s * .5);
          }
        }
  if (count > 0) p.centre = p.centre * (1.0 / count);
  p.dimensions = hi - lo;
  const V3 d = p.dimensions;
  p.inertia = V3{d.y * d.y + d.z * d.z, d.x * d.x + d.z * d.z, d.x * d.x + d.y * d.y} * (p.mass / 12);
}

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
  p->id = "rifle";
  p->name = "Rifle";
  p->tags = {"firearm", "long_firearm", "two_handed", "blunt"};
  p->mass = 3.4;
  finish_prop(*p);
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
  p->id = "smg";
  p->name = "SMG";
  p->tags = {"firearm", "long_firearm", "two_handed", "blunt"};
  p->mass = 2.8;
  finish_prop(*p);
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
  p->id = "lmg";
  p->name = "LMG";
  p->tags = {"firearm", "long_firearm", "two_handed", "blunt"};
  p->mass = 7.5;
  p->ready_pitch = -0.32;
  p->ready_roll = 0.45;
  p->ready_stock_offset = {0.01, -0.12, -0.1};
  finish_prop(*p);
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
  p->id = "pistol";
  p->name = "Pistol";
  p->tags = {"firearm", "handgun", "one_handed", "two_handed", "blunt"};
  p->mass = 0.85;
  p->hanging_rotation = qx(-0.3);
  p->one_handed = true;
  finish_prop(*p);
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
  p->id = "knife";
  p->name = "Knife";
  p->tags = {"edged", "pointed", "short_blade", "one_handed"};
  p->mass = 0.25;
  p->hanging_rotation = Quat{};
  p->one_handed = true;
  finish_prop(*p);
  return p;
}

// New objects are definitions: geometry primitives, sockets, capabilities and material.
struct PropShape {
  V3 centre, half;
  u8 slot;
};
struct PropDefinition {
  const char* id;
  const char* name;
  f64 mass;
  std::vector<std::string> tags, attachments;
  std::vector<PropShape> shapes;
  V3 tip, support;
  PropMaterial material;
};
static PropPtr sculpt_prop(const PropDefinition& d) {
  Sculptor sc(prop_skeleton(), kDefaultVoxelSize / 2, {-0.5, -.5, -.65}, {.5, 1.1, .65}, 71);
  for (const auto& shape : d.shapes) sc.add(box(shape.centre, shape.half, .003), 0, shape.slot, {.organic = false});
  auto p = std::make_shared<Prop>();
  p->id = d.id;
  p->name = d.name;
  p->mass = d.mass;
  p->tags = d.tags;
  p->attachments = d.attachments;
  p->model = sc.finish({.name = d.id});
  p->muzzle = d.tip;
  p->support = d.support;
  p->stock = {0, -.12, 0};
  p->material = d.material;
  p->one_handed = p->has("one_handed");
  p->hanging_rotation = Quat{};
  finish_prop(*p);
  if (p->has("blunt"))
    p->features = {{"surface", ImpactClass::Blunt, {0, .12, 0}, d.tip, .035, 0}, {"pommel", ImpactClass::Blunt, p->stock, p->stock, .025, 0}};
  if (p->has("non_combat")) p->features.clear();
  if (p->has("wearable")) p->sockets.push_back({"strap", {0, 0, .12}, {}, 650});
  return p;
}
const std::vector<PropPtr>& prop_catalog() {
  static const std::vector<PropPtr> all = [] {
    std::vector<PropPtr> out{make_rifle(), make_smg(), make_lmg(), make_pistol(), make_knife()};
    const std::vector<std::string> hands{"rightHand", "leftHand", "hip", "thigh", "back"};
    const PropMaterial steel{7800, 8e7, 500}, wood{650, 3e6, 130}, cloth{300, 4e5, 60};
    const std::vector<PropDefinition> defs{
        {"dagger",
         "Long dagger",
         .45,
         {"edged", "pointed", "short_blade", "one_handed"},
         hands,
         {{{0, -.015, 0}, {.018, .065, .018}, Slot::GearDark}, {{0, .2, 0}, {.012, .15, .025}, Slot::Metal}},
         {0, .35, 0},
         {},
         steel},
        {"machete",
         "Machete",
         .85,
         {"edged", "pointed", "long_blade", "one_handed"},
         hands,
         {{{0, -.02, 0}, {.02, .08, .02}, Slot::Furniture}, {{0, .29, 0}, {.012, .23, .04}, Slot::Metal}},
         {0, .52, 0},
         {},
         steel},
        {"sword",
         "Street sword",
         1.35,
         {"edged", "pointed", "long_blade", "one_handed", "two_handed"},
         hands,
         {{{0, -.035, 0}, {.018, .1, .018}, Slot::GearDark}, {{0, .06, 0}, {.07, .012, .02}, Slot::Metal}, {{0, .44, 0}, {.012, .37, .03}, Slot::Metal}},
         {0, .81, 0},
         {0, -.1, 0},
         steel},
        {"baton", "Baton", .65, {"blunt", "club", "one_handed"}, hands, {{{0, .2, 0}, {.022, .29, .022}, Slot::GearDark}}, {0, .49, 0}, {}, wood},
        {"bat",
         "Baseball bat",
         1.3,
         {"blunt", "club", "one_handed", "two_handed"},
         hands,
         {{{0, 0, 0}, {.016, .11, .016}, Slot::Furniture}, {{0, .38, 0}, {.037, .34, .037}, Slot::Furniture}},
         {0, .72, 0},
         {0, -.075, 0},
         wood},
        {"phone",
         "Phone",
         .19,
         {"non_combat", "communication", "one_handed"},
         {"rightHand", "leftHand", "chest"},
         {{{0, .035, 0}, {.04, .075, .009}, Slot::GearDark}, {{0, .04, .01}, {.033, .06, .003}, Slot::Accent}},
         {0, .1, 0},
         {},
         {2200, 2e6, 30}},
        {"bottle",
         "Bottle",
         .65,
         {"non_combat", "container", "one_handed"},
         {"rightHand", "leftHand"},
         {{{0, 0, -.04}, {.038, .038, .11}, Slot::Accent}, {{0, 0, .1}, {.014, .014, .035}, Slot::Metal}},
         {},
         {},
         {2500, 8e5, 25}},
        {"briefcase",
         "Briefcase",
         4,
         {"non_combat", "container", "one_handed", "hanging"},
         {"rightHand", "leftHand"},
         {{{0, 0, -.055}, {.018, .06, .015}, Slot::Metal}, {{0, 0, -.21}, {.055, .22, .14}, Slot::Furniture}},
         {},
         {},
         cloth},
        {"suitcase",
         "Suitcase",
         12,
         {"non_combat", "container", "one_handed", "hanging"},
         {"rightHand", "leftHand"},
         {{{0, 0, -.06}, {.025, .055, .02}, Slot::Metal}, {{0, 0, -.32}, {.105, .23, .24}, Slot::GearDark}},
         {},
         {},
         cloth},
        {"backpack",
         "Backpack",
         8,
         {"non_combat", "container", "wearable"},
         {"back", "chest"},
         {{{0, -.03, -.05}, {.18, .1, .24}, Slot::Gear},
          {{-.1, .06, .06}, {.025, .025, .22}, Slot::GearDark},
          {{.1, .06, .06}, {.025, .025, .22}, Slot::GearDark}},
         {},
         {},
         cloth},
        {"shoulder_bag",
         "Shoulder bag",
         3,
         {"non_combat", "container", "wearable", "one_handed", "hanging"},
         {"shoulder", "hip", "rightHand", "leftHand"},
         {{{0, 0, -.23}, {.075, .17, .13}, Slot::Furniture}, {{0, 0, -.05}, {.018, .12, .018}, Slot::GearDark}},
         {},
         {},
         cloth},
        // Extensibility check: this additional archetype uses the existing hanging-container path.
        {"shopping_bag",
         "Shopping bag",
         2,
         {"non_combat", "container", "one_handed", "hanging"},
         {"rightHand", "leftHand"},
         {{{0, 0, -.2}, {.075, .14, .16}, Slot::Top2}, {{0, 0, -.03}, {.015, .07, .015}, Slot::Top2}},
         {},
         {},
         cloth}};
    for (const auto& d : defs) out.push_back(sculpt_prop(d));
    return out;
  }();
  return all;
}
PropPtr prop_archetype(std::string_view id) {
  for (const auto& p : prop_catalog())
    if (p->id == id) return p;
  return {};
}
PropPtr legacy_prop(i32 index) {
  const auto& all = prop_catalog();
  return index > 0 && index <= 5 ? all[size_t(index - 1)] : PropPtr{};
}
}  // namespace svx::anim
