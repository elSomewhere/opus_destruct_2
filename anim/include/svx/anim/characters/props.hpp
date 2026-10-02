// svx_anim — immutable prop archetypes. Hosts keep instance state separately.
#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "svx/anim/voxel/model.hpp"

namespace svx::anim {

struct NamedPropPoint {
  std::string id;
  V3 point;
};
struct PropSocket {
  std::string id;
  V3 point;
  Quat rotation;
  f64 retention = 350.0;  // N; strap or grip failure load
};
enum class ImpactClass : u8 { Edge, Point, Blunt };
struct ContactFeature {
  std::string id;
  ImpactClass impact = ImpactClass::Blunt;
  V3 a, b;
  f64 radius = 0.02, sharpness = 0.0;
};
struct PropMaterial {
  f64 density = 700.0, penetration = 2e6, fracture = 150.0;
};
struct Prop {
  std::string id, name;
  ModelPtr model;
  std::vector<std::string> tags;
  f64 mass = 1.0;
  V3 centre, inertia, dimensions;
  PropMaterial material;
  std::vector<PropSocket> sockets;
  std::vector<NamedPropPoint> points;
  std::vector<ContactFeature> features;
  std::vector<std::string> attachments{"rightHand", "leftHand", "hip", "thigh"};
  // Named points and carry tuning are authored data; no identity is inspected by the holds.
  V3 grip, support, stock, muzzle, magazine;
  Quat hanging_rotation = qx(-0.9);
  f64 ready_pitch = -0.5, ready_roll = 0.3;
  V3 ready_stock_offset{0.01, 0.0, -0.05};
  bool one_handed = false;
  bool has(std::string_view tag) const;
  bool satisfies(const std::vector<std::string>& requirements) const;
  bool long_gun() const { return has("long_firearm"); }
  const PropSocket* socket(std::string_view id) const;
  const ContactFeature* feature(std::string_view id) const;
};
using PropPtr = std::shared_ptr<const Prop>;
constexpr f64 kDefaultVoxelSize = 1.0 / 32.0;
PropPtr make_rifle(f64 voxel_size = kDefaultVoxelSize, i32 variant = 0);
PropPtr make_smg(f64 voxel_size = kDefaultVoxelSize);
PropPtr make_lmg(f64 voxel_size = kDefaultVoxelSize);
PropPtr make_pistol(f64 voxel_size = kDefaultVoxelSize / 2.0);
PropPtr make_knife(f64 voxel_size = kDefaultVoxelSize / 2.0);
const std::vector<PropPtr>& prop_catalog();
PropPtr prop_archetype(std::string_view id);
// Compatibility for stored Foundry weapon values 0..5 (zero is empty).
PropPtr legacy_prop(i32 weapon);
}  // namespace svx::anim
