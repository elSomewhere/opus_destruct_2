#include "svx/anim/damage/anatomy.hpp"
#include "svx/anim/rig.hpp"
#include <set>
#include <map>
namespace svx::anim {
f64 segment_distance(const V3& p, const V3& a, const V3& b) {
  const V3 d = b - a;
  return norm(p - (a + d * clamp(dot(p - a, d) / std::max(1e-12, dot(d, d)), 0.0, 1.0)));
}
f64 tissue_resistance(Tissue t) { return t == Tissue::Bone ? 5e6 : t == Tissue::Flesh ? 5e5 : t == Tissue::Metal ? 2e6 : 3e5; }
std::vector<VitalRegion> anatomy_regions(const Skeleton& sk) {
  const f64 k = sk.rest_head[H::pelvis].z / .97;
  std::vector<VitalRegion> out;
  auto add = [&](const char* name, VitalKind kind, i32 bone, V3 offset, V3 radii) {
    out.push_back({name, kind, bone, sk.rest_head[size_t(bone)] + offset * k, radii * k});
  };
  add("brain", VitalKind::Brain, H::head, {0, 0, .1}, {.075, .075, .08});
  add("high spinal cord", VitalKind::Cord, H::neck, {0, -.01, .02}, {.022, .025, .06});
  add("spinal cord", VitalKind::Cord, H::spine, {0, -.055, .12}, {.025, .025, .22});
  add("heart / great vessels", VitalKind::Heart, H::chest, {-.035, .045, .04}, {.055, .06, .07});
  add("left lung", VitalKind::Lung, H::chest, {-.095, .035, .05}, {.065, .06, .11});
  add("right lung", VitalKind::Lung, H::chest, {.095, .035, .05}, {.065, .06, .11});
  add("liver", VitalKind::Liver, H::spine, {.065, .055, .025}, {.07, .065, .05});
  add("carotid", VitalKind::Vessel, H::neck, {.027, .02, .025}, {.018, .018, .05});
  for (int side = 0; side < 2; ++side) {
    const i32 leg = side ? H::thighR : H::thighL, arm = side ? H::upperarmR : H::upperarmL;
    out.push_back({side ? "right femoral" : "left femoral",
                   VitalKind::Vessel,
                   leg,
                   vlerp(sk.rest_head[size_t(leg)], sk.rest_tail[size_t(leg)], .3) + V3{side ? -.035 : .035, .025, 0} * k,
                   {.023 * k, .024 * k, .13 * k}});
    out.push_back({side ? "right brachial" : "left brachial",
                   VitalKind::Vessel,
                   arm,
                   vlerp(sk.rest_head[size_t(arm)], sk.rest_tail[size_t(arm)], .55),
                   {.023 * k, .025 * k, .1 * k}});
  }
  return out;
}
void fill_interior(VoxelModel& m, Enclosure enclosure) {
  // (cells by the part they count for: all one in the body, each its own part's)
  auto owner = [&](i32 bone) { return enclosure == Enclosure::Part ? bone : -1; };
  std::set<std::array<i32, 4>> occupied;
  for (const auto& p : m.parts)
    for (i32 z = 0; z < p.dims[2]; ++z)
      for (i32 y = 0; y < p.dims[1]; ++y)
        for (i32 x = 0; x < p.dims[0]; ++x)
          if (p.cells[size_t(p.index(x, y, z))]) occupied.insert({owner(p.bone), p.origin[0] + x, p.origin[1] + y, p.origin[2] + z});
  const auto& sk = *m.skeleton;
  const f64 k = sk.rest_head[H::pelvis].z / .97, s = m.voxel_size;
  auto enclosed = [&](i32 bone, const std::array<i32, 3>& at) {
    for (int axis = 0; axis < 3; ++axis)
      for (int sign : {-1, 1}) {
        std::array<i32, 4> next{owner(bone), at[0], at[1], at[2]};
        next[size_t(axis) + 1] += sign;
        if (!occupied.contains(next)) return false;
      }
    return true;
  };
  auto limb = [](i32 bone) { return bone >= H::upperarmL && bone <= H::toeR && bone != H::clavicleR; };
  auto section = [&](i32 bone, const V3& v) {
    return i32(std::floor(dot(v - sk.rest_head[size_t(bone)], vnorm(sk.rest_tail[size_t(bone)] - sk.rest_head[size_t(bone)])) / s));
  };
  // A sculpted limb can be offset from its rig segment. Keep a thin bone core
  // in each enclosed cross-section instead of silently producing a boneless
  // limb.
  std::map<std::pair<i32, i32>, f64> core;
  for (const auto& p : m.parts)
    if (limb(p.bone))
      for (i32 z = 0; z < p.dims[2]; ++z)
        for (i32 y = 0; y < p.dims[1]; ++y)
          for (i32 x = 0; x < p.dims[0]; ++x) {
            if (!p.cells[size_t(p.index(x, y, z))]) continue;
            const std::array<i32, 3> at{p.origin[0] + x, p.origin[1] + y, p.origin[2] + z};
            if (!enclosed(p.bone, at)) continue;
            const V3 v = m.cell_centre(at[0], at[1], at[2]);
            const auto key = std::pair{p.bone, section(p.bone, v)};
            const f64 distance = segment_distance(v, sk.rest_head[size_t(p.bone)], sk.rest_tail[size_t(p.bone)]);
            const auto found = core.find(key);
            if (found == core.end()) core.emplace(key, distance);
            else found->second = std::min(found->second, distance);
          }
  for (auto& p : m.parts) {
    bool changed = false;
    for (i32 z = 0; z < p.dims[2]; ++z)
      for (i32 y = 0; y < p.dims[1]; ++y)
        for (i32 x = 0; x < p.dims[0]; ++x) {
          const size_t n = size_t(p.index(x, y, z));
          if (!p.cells[n]) continue;
          const std::array<i32, 3> at{p.origin[0] + x, p.origin[1] + y, p.origin[2] + z};
          if (!enclosed(p.bone, at)) continue;
          const V3 v = m.cell_centre(at[0], at[1], at[2]);
          bool bone = false;
          for (int b = 0; b < 22; ++b) bone |= segment_distance(v, sk.rest_head[size_t(b)], sk.rest_tail[size_t(b)]) < .025 * k;
          if (limb(p.bone))
            bone |=
                segment_distance(v, sk.rest_head[size_t(p.bone)], sk.rest_tail[size_t(p.bone)]) <= core.at({p.bone, section(p.bone, v)}) + s * .15;
          if (v.z >= sk.rest_head[H::head].z) {
            const V3 d = v - (sk.rest_head[H::head] + V3{0, 0, .09 * k});
            const f64 q = std::sqrt(d.x * d.x / (.083 * .083 * k * k) + d.y * d.y / (.078 * .078 * k * k) + d.z * d.z / (.105 * .105 * k * k));
            bone = q > .7 && q < 1.05;
          }
          if (v.z >= sk.rest_head[H::chest].z && v.z < sk.rest_head[H::neck].z) {
            const V3 d = v - sk.rest_head[H::chest];
            const f64 q = std::sqrt(d.x * d.x / (.15 * .15 * k * k) + d.y * d.y / (.082 * .082 * k * k));
            bone |= q > .72 && q < 1.12 && std::fmod(std::abs(d.z), .07 * k) < s;
          }
          p.cells[n] = u8((bone ? Slot::Bone : Slot::Flesh) + 1);
          if (!p.shade.empty()) p.shade[n] = 128;
          changed = true;
        }
    if (changed) ++p.version;
  }
}
}  // namespace svx::anim
