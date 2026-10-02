#include "svx/anim/damage/mechanics.hpp"
#include "svx/anim/damage/anatomy.hpp"
#include <algorithm>
#include <set>
namespace svx::anim {
namespace {
V3 world(const f32* m, const V3& v) {
  return {m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12], m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13], m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14]};
}
V3 rest(const f32* m, const V3& v) {
  const V3 d = v - V3{m[12], m[13], m[14]};
  return {m[0] * d.x + m[1] * d.y + m[2] * d.z, m[4] * d.x + m[5] * d.y + m[6] * d.z, m[8] * d.x + m[9] * d.y + m[10] * d.z};
}
struct Cell {
  size_t part, index;
  V3 rest;
  f64 t;
  f64 weight = 1;
};
// DDA visits only the cells crossed by the ray, in each posed part's own
// lattice.
std::vector<Cell> path(const VoxelModel& model, std::span<const f32> skin, const V3& origin, const V3& direction) {
  std::vector<Cell> hits;
  const f64 s = model.voxel_size;
  for (size_t pi = 0; pi < model.parts.size(); ++pi) {
    const auto& p = model.parts[pi];
    if (!p.count || size_t(p.bone * 16 + 16) > skin.size()) continue;
    const auto* m = skin.data() + p.bone * 16;
    const V3 o = rest(m, origin), d = rest(m, origin + direction) - o;
    f64 begin = 0, end = 4;
    bool miss = false;
    for (int a = 0; a < 3; ++a) {
      const f64 lo = p.origin[size_t(a)] * s, hi = (p.origin[size_t(a)] + p.dims[size_t(a)]) * s;
      if (std::abs(d[a]) < 1e-12) {
        if (o[a] < lo || o[a] >= hi) miss = true;
        continue;
      }
      f64 x = (lo - o[a]) / d[a], y = (hi - o[a]) / d[a];
      if (x > y) std::swap(x, y);
      begin = std::max(begin, x);
      end = std::min(end, y);
    }
    if (miss || begin > end) continue;
    const V3 q = o + d * (begin + 1e-8);
    std::array<i32, 3> at;
    V3 next, delta;
    std::array<i32, 3> step;
    for (int a = 0; a < 3; ++a) {
      at[size_t(a)] = std::clamp(i32(std::floor(q[a] / s)) - p.origin[size_t(a)], 0, p.dims[size_t(a)] - 1);
      step[size_t(a)] = d[a] > 0 ? 1 : -1;
      delta[a] = std::abs(d[a]) > 1e-12 ? s / std::abs(d[a]) : 1e30;
      next[a] = std::abs(d[a]) > 1e-12 ? ((p.origin[size_t(a)] + at[size_t(a)] + (d[a] > 0 ? 1 : 0)) * s - o[a]) / d[a] : 1e30;
    }
    f64 t = begin;
    for (int n = 0; n < 2048 && t <= end; ++n) {
      const size_t index = size_t(p.index(at[0], at[1], at[2]));
      if (p.cells[index]) hits.push_back({pi, index, model.cell_centre(at[0] + p.origin[0], at[1] + p.origin[1], at[2] + p.origin[2]), t});
      const int a = next.x <= next.y && next.x <= next.z ? 0 : next.y <= next.z ? 1 : 2;
      t = next[a];
      next[a] += delta[a];
      at[size_t(a)] += step[size_t(a)];
      if (at[size_t(a)] < 0 || at[size_t(a)] >= p.dims[size_t(a)]) break;
    }
  }
  std::stable_sort(hits.begin(), hits.end(), [](const Cell& a, const Cell& b) { return a.t < b.t; });
  return hits;
}
}  // namespace
WoundMechanics wound_mechanics(VoxelModel& m, std::span<const f32> skin, const DamageDescriptor& d, const PropMaterial* material) {
  WoundMechanics out;
  if (!d.valid() || d.kind == DamageKind::Thermal) return out;
  out.remaining_energy = d.energy();
  out.exit_direction = vnorm(d.direction);
  if (d.energy() == 0) return out;
  const f64 s = m.voxel_size, volume = s * s * s;
  f64 energy = d.energy();
  const V3 direction = vnorm(d.direction);
  std::set<size_t> changed;
  auto remove = [&](const Cell& c) {
    auto& p = m.parts[c.part];
    if (!p.cells[c.index]) return;
    out.removed.push_back({p.bone, c.rest, u8(p.cells[c.index] - 1), p.shade.empty() ? u8(128) : p.shade[c.index]});
    p.cells[c.index] = 0;
    --p.count;
    changed.insert(c.part);
  };
  if (d.kind == DamageKind::Projectile || d.kind == DamageKind::Point) {
    V3 ray = direction;
    auto cells = path(m, skin, d.point - ray * (s * .51), ray);
    V3 last;
    size_t crossed = 0;
    Cell final_cell{};
    int deflections = 0;
    for (size_t index = 0; index < cells.size(); ++index) {
      const Cell c = cells[index];
      auto& p = m.parts[c.part];
      if (!p.cells[c.index]) continue;
      const u8 slot = u8(p.cells[c.index] - 1);
      const f64 cost = (material ? material->penetration : tissue_resistance(slot)) * volume;
      const f64 take = std::min(energy, cost);
      out.tissue.push_back({p.bone, c.rest, take, energy >= cost ? 1.0 : 0.0, slot == Slot::Bone});
      if (energy < cost) {
        energy = 0;
        break;
      }
      energy -= cost;
      remove(c);
      ++crossed;
      last = world(skin.data() + p.bone * 16, c.rest);
      final_cell = c;
      const f64 channel = std::max(s * .52, d.diameter * (d.construction == ProjectileConstruction::Expanding ? 1.1 : .65));
      const f64 halo = !material && d.kind == DamageKind::Projectile && d.speed > 600 ? std::min(.075, channel + d.speed * .000035) : channel;
      const int reach = int(std::ceil(halo / s));
      std::vector<Cell> bruised;
      for (int z = -reach; z <= reach; ++z)
        for (int y = -reach; y <= reach; ++y)
          for (int x = -reach; x <= reach; ++x) {
            if (x == 0 && y == 0 && z == 0) continue;
            const V3 at = c.rest + V3{f64(x), f64(y), f64(z)} * s;
            if (vdist(at, c.rest) > halo) continue;
            const i32 ix = i32(std::floor(at.x / s)) - p.origin[0], iy = i32(std::floor(at.y / s)) - p.origin[1],
                      iz = i32(std::floor(at.z / s)) - p.origin[2];
            if (ix < 0 || iy < 0 || iz < 0 || ix >= p.dims[0] || iy >= p.dims[1] || iz >= p.dims[2]) continue;
            const size_t n = size_t(p.index(ix, iy, iz));
            if (!p.cells[n]) continue;
            const Cell adjacent{c.part, n, at, c.t};
            const u8 tissue = u8(p.cells[n] - 1);
            const f64 resistance = (material ? material->penetration : tissue_resistance(tissue)) * volume;
            if (vdist(at, c.rest) <= channel && energy >= resistance) {
              energy -= resistance;
              out.tissue.push_back({p.bone, at, resistance, 1, tissue == Slot::Bone});
              remove(adjacent);
            } else if (halo > channel) bruised.push_back(adjacent);
          }
      // A temporary cavity spends the same finite budget as the permanent
      // channel.
      const f64 cavity = bruised.empty() ? 0 : std::min(energy, take * .5);
      energy -= cavity;
      if (cavity > 0)
        for (const auto& adjacent : bruised) out.tissue.push_back({p.bone, adjacent.rest, cavity / bruised.size(), 0, false});
      // Bone can turn a penetrator, with less deviation as the remaining energy
      // rises. The lattice coordinates choose the side, so this adds no
      // backend-dependent randomness.
      if (!material && slot == Slot::Bone && d.kind == DamageKind::Projectile && energy > 0 && deflections < 2) {
        const f64 side = std::fmod(std::abs(c.rest.x * 73 + c.rest.y * 151 + c.rest.z * 199), 2) < 1 ? -1 : 1;
        const V3 tangent = vnorm(cross(ray, V3{0, 0, 1}), V3{1, 0, 0});
        ray = vnorm(ray + tangent * (side * clamp(cost / std::max(cost, energy) * .25, .008, .18)));
        cells = path(m, skin, last + ray * s * .51, ray);
        index = size_t(-1);
        ++deflections;
      }
      if (energy <= 0) break;
    }
    out.exited = crossed > 0 && energy > 0;
    out.exit = last;
    out.exit_direction = ray;
    if (out.exited) {
      auto& p = m.parts[final_cell.part];
      const f64 radius = std::max(s * .8, d.diameter * 1.5);
      const int reach = int(std::ceil(radius / s));
      for (int z = -reach; z <= reach; ++z)
        for (int y = -reach; y <= reach; ++y)
          for (int x = -reach; x <= reach; ++x) {
            const V3 at = final_cell.rest + V3{f64(x), f64(y), f64(z)} * s;
            if (vdist(at, final_cell.rest) > radius) continue;
            const int ix = int(std::floor(at.x / s)) - p.origin[0], iy = int(std::floor(at.y / s)) - p.origin[1],
                      iz = int(std::floor(at.z / s)) - p.origin[2];
            if (ix < 0 || iy < 0 || iz < 0 || ix >= p.dims[0] || iy >= p.dims[1] || iz >= p.dims[2]) continue;
            const size_t n = size_t(p.index(ix, iy, iz));
            if (!p.cells[n]) continue;
            const f64 cost = (material ? material->penetration : tissue_resistance(p.cells[n] - 1)) * volume;
            if (energy >= cost) {
              energy -= cost;
              out.tissue.push_back({p.bone, at, cost, 1, p.cells[n] == Slot::Bone + 1});
              remove({final_cell.part, n, at, 0});
            }
          }
    }
  } else {
    const bool edged = d.kind == DamageKind::Edge && d.alignment > .15;
    const V3 edge = norm(d.edge_b - d.edge_a) > .001 ? vnorm(d.edge_b - d.edge_a) : vnorm(cross(direction, V3{0, 0, 1}), V3{1, 0, 0});
    const V3 normal = vnorm(cross(edge, direction), V3{0, 0, 1});
    const f64 cutting = edged ? clamp(d.sharpness * d.alignment, .01, 1.0) : 1;
    const f64 depth = clamp(energy * cutting / (edged ? 1800.0 : 5000.0), edged ? .005 : .08, .35);
    const f64 length = std::max(d.swept_length, s), radius = std::sqrt(d.area / kPi);
    std::vector<Cell> contacts;
    for (size_t pi = 0; pi < m.parts.size(); ++pi) {
      const auto& p = m.parts[pi];
      if (!p.count || size_t(p.bone * 16 + 16) > skin.size()) continue;
      const auto* mat = skin.data() + p.bone * 16;
      for (i32 z = 0; z < p.dims[2]; ++z)
        for (i32 y = 0; y < p.dims[1]; ++y)
          for (i32 x = 0; x < p.dims[0]; ++x) {
            const size_t n = size_t(p.index(x, y, z));
            if (!p.cells[n]) continue;
            const V3 at = m.cell_centre(x + p.origin[0], y + p.origin[1], z + p.origin[2]), delta = world(mat, at) - d.point;
            const f64 along = dot(delta, direction), across = dot(delta, edge), thin = std::abs(dot(delta, normal));
            const f64 radial2 = std::max(0.0, dot(delta, delta) - along * along);
            const f64 spread = radius + s * .5 + std::max(0.0, along) * .5;
            const bool touches = edged ? (along >= -s && along <= depth && std::abs(across) <= length * .5 + s * .5 && thin <= s * .55)
                                       : (along >= -s * .5 && along <= depth + s * .5 && radial2 <= spread * spread);
            // Pressure spreads inward from the contact patch. A larger impact
            // no longer bruises an arbitrary sphere behind, beside and above
            // its point of entry.
            if (touches) contacts.push_back({pi, n, at, along, edged ? 1 : std::exp(-radial2 / (spread * spread) - std::max(0.0, along) / depth)});
          }
    }
    // Spend the budget front to back. Bone-array order must not decide which
    // limb a blade cuts.
    std::stable_sort(contacts.begin(), contacts.end(), [](const Cell& a, const Cell& b) { return a.t < b.t; });
    const size_t first_contact = out.tissue.size();
    for (const auto& c : contacts) {
      auto& p = m.parts[c.part];
      const u8 slot = u8(p.cells[c.index] - 1);
      const f64 cost = (material ? material->penetration : tissue_resistance(slot)) * volume / cutting;
      if (edged) {
        if (energy <= 0) break;
        const bool removed = energy >= cost;
        out.tissue.push_back({p.bone, c.rest, std::min(cost, energy), removed ? 1.0 : 0.0, slot == Slot::Bone});
        energy = std::max(0.0, energy - cost);
        if (removed) remove(c);
      } else {
        out.tissue.push_back({p.bone, c.rest, 0, 0, slot == Slot::Bone});
        if (d.kind == DamageKind::Crush && d.energy() > 1200 && energy >= cost) {
          energy -= cost;
          out.tissue.back().energy = cost;
          out.tissue.back().removed = 1;
          remove(c);
        }
      }
    }
    if (!edged && !contacts.empty()) {
      // Removal spends its tissue resistance first; compression absorbs the
      // rest.
      f64 weight = 0;
      for (const auto& c : contacts) weight += c.weight;
      for (size_t i = first_contact; i < out.tissue.size(); ++i) {
        const auto& c = contacts[i - first_contact];
        out.tissue[i].energy += energy * c.weight / weight;
        auto& p = m.parts[c.part];
        if (out.tissue[i].energy <= 0 || !p.cells[c.index]) continue;
        if (p.shade.empty()) p.shade.resize(p.cells.size(), 128);
        p.shade[c.index] = u8(std::max(35, int(p.shade[c.index]) - 35));
        changed.insert(c.part);
      }
      std::erase_if(out.tissue, [](const auto& t) { return t.energy <= 0; });
      energy = 0;
    }
  }
  // Dark wound rims leave exposed flesh and bone intact and readable.
  if (!material)
    for (const auto& v : out.removed)
      for (size_t pi = 0; pi < m.parts.size(); ++pi) {
        auto& p = m.parts[pi];
        if (p.bone != v.bone) continue;
        const i32 x = i32(std::floor(v.rest.x / s)) - p.origin[0], y = i32(std::floor(v.rest.y / s)) - p.origin[1],
                  z = i32(std::floor(v.rest.z / s)) - p.origin[2];
        for (const auto& q : std::array<std::array<int, 3>, 6>{{{{1, 0, 0}}, {{-1, 0, 0}}, {{0, 1, 0}}, {{0, -1, 0}}, {{0, 0, 1}}, {{0, 0, -1}}}}) {
          const int a = x + q[0], b = y + q[1], c = z + q[2];
          if (a < 0 || b < 0 || c < 0 || a >= p.dims[0] || b >= p.dims[1] || c >= p.dims[2]) continue;
          const size_t n = size_t(p.index(a, b, c));
          if (!p.cells[n]) continue;
          if (p.cells[n] != Slot::Bone + 1 && p.cells[n] != Slot::Flesh + 1) p.cells[n] = Slot::Blood + 1;
          if (p.shade.empty()) p.shade.resize(p.cells.size(), 128);
          p.shade[n] = 90;
          changed.insert(pi);
        }
      }
  for (size_t pi : changed) {
    ++m.parts[pi].version;
    out.changed_bones.push_back(m.parts[pi].bone);
  }
  out.remaining_energy = energy;
  return out;
}
}  // namespace svx::anim
