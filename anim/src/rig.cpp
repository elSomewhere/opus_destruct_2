#include "svx/anim/rig.hpp"

#include <string>

namespace svx::anim {

const i32 kHMirror[8][2] = {{H::clavicleL, H::clavicleR}, {H::upperarmL, H::upperarmR}, {H::forearmL, H::forearmR}, {H::handL, H::handR},
                            {H::thighL, H::thighR},       {H::shinL, H::shinR},         {H::footL, H::footR},       {H::toeL, H::toeR}};

namespace {

// The unscaled rig's bone heads and tails (the left side; the right is mirrored).
struct Rest {
  const char* name;
  const char* parent;
  V3 head, tail;
  int side;  // 0 none, 1 shoulder, 2 hip
};

const Rest kRest[] = {
    {"root", nullptr, {0, 0, 0}, {0, 0.15, 0}, 0},
    {"pelvis", "root", {0, 0, 0.97}, {0, 0, 1.07}, 0},
    {"spine", "pelvis", {0, -0.01, 1.07}, {0, -0.015, 1.22}, 0},
    {"chest", "spine", {0, -0.015, 1.22}, {0, -0.005, 1.45}, 0},
    {"neck", "chest", {0, -0.005, 1.455}, {0, 0.01, 1.545}, 0},
    {"head", "neck", {0, 0.01, 1.545}, {0, 0.02, 1.765}, 0},
    {"clavicleL", "chest", {-0.025, 0.005, 1.42}, {-0.175, -0.01, 1.425}, 1},
    {"upperarmL", "clavicleL", {-0.185, -0.01, 1.42}, {-0.27, -0.01, 1.145}, 1},
    {"forearmL", "upperarmL", {-0.27, -0.01, 1.145}, {-0.345, 0.005, 0.895}, 1},
    {"handL", "forearmL", {-0.345, 0.005, 0.895}, {-0.39, 0.01, 0.73}, 1},
    {"thighL", "pelvis", {-0.1, 0, 0.925}, {-0.1, 0.01, 0.51}, 2},
    {"shinL", "thighL", {-0.1, 0.01, 0.51}, {-0.1, -0.015, 0.085}, 2},
    {"footL", "shinL", {-0.1, -0.015, 0.085}, {-0.1, 0.115, 0.022}, 2},
    {"toeL", "footL", {-0.1, 0.115, 0.022}, {-0.1, 0.175, 0.015}, 2},
};

const char* kOrder[] = {"root",      "pelvis",   "spine",  "chest",  "neck",      "head",     "clavicleL", "upperarmL",
                        "forearmL",  "handL",    "clavicleR", "upperarmR", "forearmR", "handR", "thighL", "shinL",
                        "footL",     "toeL",     "thighR", "shinR",  "footR",     "toeR",     "weapon"};

const Rest* rest_of(const std::string& name) {
  for (const Rest& r : kRest)
    if (name == r.name) return &r;
  return nullptr;
}

}  // namespace

SkeletonPtr humanoid_skeleton(const HumanoidBuild& b) {
  std::vector<BoneDef> defs;
  for (const char* cname : kOrder) {
    const std::string name = cname;
    if (name == "weapon") {
      const f64 k = b.height;
      defs.push_back({name, "root", V3{0.25 * k, 0.35 * k, 1.25 * k}, V3{0.25 * k, 0.8 * k, 1.25 * k}});
      continue;
    }
    const bool right = name.size() > 1 && name.back() == 'R' && name != "root";
    const Rest* src = rest_of(right ? name.substr(0, name.size() - 1) + "L" : name);
    const f64 sx = src->side == 1 ? b.shoulders : src->side == 2 ? b.hips : 1.0;
    auto map = [&](const V3& p) { return V3{(right ? -1.0 : 1.0) * p.x * sx * b.height, p.y * b.height, p.z * b.height}; };
    std::string parent = src->parent ? src->parent : "";
    if (right && !parent.empty() && parent.back() == 'L') parent = parent.substr(0, parent.size() - 1) + "R";
    defs.push_back({name, parent, map(src->head), map(src->tail)});
  }
  return std::make_shared<const Skeleton>(defs);
}

SkeletonPtr prop_skeleton() {
  return std::make_shared<const Skeleton>(std::vector<BoneDef>{{"prop", "", V3{0, 0, 0}, V3{0, 0.3, 0}}});
}

}  // namespace svx::anim
