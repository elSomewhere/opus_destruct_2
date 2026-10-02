// svx_anim — the only damage state read by motion, props and hosts.
#pragma once
#include <array>
#include <optional>
#include "svx/anim/math.hpp"
namespace svx::anim {
enum class CrawlStyle : u8 { BothArms, LeftArm, RightArm, Scoot };
enum class Mobility : u8 { Walk, Limp, Hobble, Kneel, Crawl, Immobile };
const char* mobility_name(Mobility mobility);
struct LegCapability {
  f64 support = 1, drive = 1, control = 1;
};
struct ArmCapability {
  f64 strength = 1, control = 1, grip = 1;
};
struct CareTarget {
  i32 part = 0;
  V3 local, normal;
  f64 age = 0, hold_until = 0, urgency = 0;
  bool from_bone = false;
};
struct Capabilities {
  std::array<LegCapability, 2> legs;
  std::array<ArmCapability, 2> arms;
  f64 trunk = 1, neck = 1, consciousness = 1, vigor = 1, pain = 0;
  f64 max_speed = 8;
  bool fatal = false;
  Mobility mobility = Mobility::Walk;
  CrawlStyle crawl = CrawlStyle::BothArms;
  std::array<f64, 16> muscle{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  std::array<bool, 16> paralysed{};
  std::optional<CareTarget> care;
};
void derive_mobility(Capabilities& capabilities);
}  // namespace svx::anim
