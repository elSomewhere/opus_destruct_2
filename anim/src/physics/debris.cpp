#include "svx/anim/physics/debris.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>

#include "svx/anim/character.hpp"

namespace svx::anim {

namespace {

constexpr f64 kDensity = 1000.0;
constexpr f64 kRestitution = 0.25;
constexpr f64 kFriction = 0.6;
// Collision samples per gib at most (a hand gets a dozen, a piece of torso all of them).
constexpr i32 kMaxSamples = 64;
// The furthest any sample point may move in one substep (m).
constexpr f64 kMaxStepDisp = 0.04;
constexpr f64 kBaseSubstep = 1.0 / 120.0;
constexpr i32 kMaxSubsteps = 32;
constexpr f64 kMaxSpeed = 60.0;
constexpr f64 kMaxSpin = 40.0;
constexpr f64 kSleepSpeed = 0.08;
constexpr f64 kSleepSpin = 0.35;
constexpr f64 kSleepTime = 0.5;
// Penetration the positional correction leaves alone (m).
constexpr f64 kSlop = 0.002;
constexpr f64 kDropLife = 4.0;
constexpr f64 kDropDrag = 0.4;

void clamp_velocity(Gib& g) {
  const f64 v = norm(g.vel);
  if (v > kMaxSpeed) g.vel = V3{g.vel.x * kMaxSpeed / v, g.vel.y * kMaxSpeed / v, g.vel.z * kMaxSpeed / v};
  const f64 w = norm(g.ang);
  if (w > kMaxSpin) g.ang = V3{g.ang.x * kMaxSpin / w, g.ang.y * kMaxSpin / w, g.ang.z * kMaxSpin / w};
}

// The directions whose extreme points are always sampled: the 26 neighbour directions of a cube.
const std::array<V3, 26>& extreme_dirs() {
  static const std::array<V3, 26> dirs = [] {
    std::array<V3, 26> out{};
    size_t n = 0;
    for (i32 x = -1; x <= 1; ++x)
      for (i32 y = -1; y <= 1; ++y)
        for (i32 z = -1; z <= 1; ++z)
          if (x || y || z) out[n++] = vnorm(V3{static_cast<f64>(x), static_cast<f64>(y), static_cast<f64>(z)});
    return out;
  }();
  return dirs;
}

// At most `max` of the points (offsets from the pivot), spread evenly: the extremes along the
// extreme directions, then the outermost point in each cell of a grid, the grid coarsened until the
// budget holds (so no stretch of surface goes unsampled). In the original's order: the extremes as
// found, then the cells as first met (the order the sequential impulses see them in).
std::vector<V3> pick_samples(const std::vector<V3>& points, i32 max, f64 cell) {
  if (static_cast<i32>(points.size()) <= max) return points;
  std::vector<i32> chosen;
  for (const V3& d : extreme_dirs()) {
    i32 best = -1;
    f64 best_dot = -std::numeric_limits<f64>::infinity();
    for (size_t i = 0; i < points.size(); ++i) {
      const f64 k = dot(points[i], d);
      if (k > best_dot) {
        best_dot = k;
        best = static_cast<i32>(i);
      }
    }
    if (best >= 0 && std::find(chosen.begin(), chosen.end(), best) == chosen.end()) chosen.push_back(best);
  }
  std::vector<i32> bins, all;  // (bins: the outermost point of each cell, the cells in the order first met)
  std::map<std::array<i64, 3>, size_t> bin_of;
  std::vector<u8> taken(points.size());
  for (f64 size = cell * 2.0;; size *= 1.25) {
    bins.clear();
    bin_of.clear();
    for (size_t i = 0; i < points.size(); ++i) {
      const V3& p = points[i];
      const std::array<i64, 3> key{static_cast<i64>(std::floor(p.x / size)), static_cast<i64>(std::floor(p.y / size)),
                                   static_cast<i64>(std::floor(p.z / size))};
      const auto [it, fresh] = bin_of.try_emplace(key, bins.size());
      if (fresh) bins.push_back(static_cast<i32>(i));
      else if (norm(p) > norm(points[size_t(bins[it->second])])) bins[it->second] = static_cast<i32>(i);
    }
    all = chosen;
    std::fill(taken.begin(), taken.end(), u8{0});
    for (i32 i : chosen) taken[size_t(i)] = 1;
    for (i32 i : bins) {
      if (taken[size_t(i)]) continue;
      taken[size_t(i)] = 1;
      all.push_back(i);
    }
    if (static_cast<i32>(all.size()) <= max || size > 1.0) {
      std::vector<V3> out;
      for (size_t k = 0; k < all.size() && k < size_t(max); ++k) out.push_back(points[size_t(all[k])]);
      return out;
    }
  }
}

}  // namespace

GibSystem::GibSystem(const CollisionWorld* c, const GibSystemOptions& o)
    : collision(c),
      max_gibs(o.max_gibs),
      max_drops(o.max_drops),
      max_stains(o.max_stains),
      gravity(o.gravity),
      kill_z(o.kill_z),
      rng_(o.seed) {}

// ---- gibs ------------------------------------------------------------------------------------

Gib* GibSystem::spawn(VoxelPart part, f64 voxel_size, const V3& bone_pos, const Quat& bone_rot, const V3& bone_rest_head, const V3& vel,
                      const V3& ang, u64 user) {
  const f64 s = voxel_size;
  const i32 nx = part.dims[0], ny = part.dims[1], nz = part.dims[2];
  const i32 ox = part.origin[0], oy = part.origin[1], oz = part.origin[2];
  auto solid = [&](i32 x, i32 y, i32 z) {
    return x >= 0 && y >= 0 && z >= 0 && x < nx && y < ny && z < nz && part.cells[size_t(x + nx * (y + ny * z))] != 0;
  };
  // centre of mass, cell bounds, surface cells
  f64 cx = 0.0, cy = 0.0, cz = 0.0;
  i32 count = 0;
  i32 x0 = std::numeric_limits<i32>::max(), y0 = x0, z0 = x0, x1 = std::numeric_limits<i32>::min(), y1 = x1, z1 = x1;
  std::vector<V3> surface;
  for (i32 z = 0; z < nz; ++z)
    for (i32 y = 0; y < ny; ++y)
      for (i32 x = 0; x < nx; ++x) {
        if (!solid(x, y, z)) continue;
        const V3 p{(ox + x + 0.5) * s, (oy + y + 0.5) * s, (oz + z + 0.5) * s};
        cx += p.x;
        cy += p.y;
        cz += p.z;
        ++count;
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        z0 = std::min(z0, z);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
        z1 = std::max(z1, z);
        if (!solid(x - 1, y, z) || !solid(x + 1, y, z) || !solid(x, y - 1, z) || !solid(x, y + 1, z) || !solid(x, y, z - 1) || !solid(x, y, z + 1))
          surface.push_back(p);
      }
  const V3 pivot = count > 0 ? V3{cx / count, cy / count, cz / count} : V3{(ox + nx / 2.0) * s, (oy + ny / 2.0) * s, (oz + nz / 2.0) * s};
  // box inertia from the cell bounds
  const f64 mass = std::max(1e-3, count * s * s * s * kDensity);
  const f64 dx = count > 0 ? (x1 - x0 + 1) * s : s, dy = count > 0 ? (y1 - y0 + 1) * s : s, dz = count > 0 ? (z1 - z0 + 1) * s : s;
  const f64 ix = (mass / 12.0) * (dy * dy + dz * dz);
  const f64 iy = (mass / 12.0) * (dx * dx + dz * dz);
  const f64 iz = (mass / 12.0) * (dx * dx + dy * dy);
  const f64 floor_i = mass * s * s * 0.1;
  // sample points: the extremes along the 26 directions, then evenly spread surface cells
  std::vector<V3> offsets;
  offsets.reserve(surface.size());
  for (const V3& p : surface) offsets.push_back(p - pivot);
  std::vector<V3> samples = pick_samples(offsets, kMaxSamples, s);
  if (samples.empty()) samples.push_back(V3{});
  f64 radius = 0.0;
  for (const V3& o : offsets) radius = std::max(radius, norm(o));
  radius += s * 0.87;
  // the gib's transform: rest point x at bone_rot (x - bone_rest_head) + bone_pos
  const Quat rot = qnormalize(bone_rot);
  const V3 pos = rotate(rot, pivot - bone_rest_head) + bone_pos;
  auto g = std::make_unique<Gib>();
  g->id = next_id_++;
  g->part = std::move(part);
  g->voxel_size = s;
  g->pos = pos;
  g->rot = rot;
  g->pivot = pivot;
  g->vel = vel;
  g->ang = ang;
  g->radius = radius;
  g->user = user;
  make_room();
  gibs.push_back(std::move(g));
  Body b;
  b.mass = mass;
  b.inv_mass = 1.0 / mass;
  b.inv_i = V3{1.0 / std::max(ix, floor_i), 1.0 / std::max(iy, floor_i), 1.0 / std::max(iz, floor_i)};
  b.samples = std::move(samples);
  b.calm_v = norm(vel);
  b.calm_w = norm(ang);
  bodies_.push_back(std::move(b));
  return gibs.back().get();
}

void GibSystem::set_mass(const Gib* g, f64 mass, const V3& inertia) {
  if (mass <= 0) return;
  for (size_t i = 0; i < gibs.size(); ++i)
    if (gibs[i].get() == g) {
      auto& b = bodies_[i];
      b.mass = mass;
      b.inv_mass = 1 / mass;
      b.inv_i = {1 / std::max(1e-6, inertia.x), 1 / std::max(1e-6, inertia.y), 1 / std::max(1e-6, inertia.z)};
      return;
    }
}
void GibSystem::remove(const Gib* g) {
  for (size_t i = 0; i < gibs.size(); ++i) {
    if (gibs[i].get() != g) continue;
    gibs.erase(gibs.begin() + static_cast<std::ptrdiff_t>(i));
    bodies_.erase(bodies_.begin() + static_cast<std::ptrdiff_t>(i));
    return;
  }
}

void GibSystem::make_room() {
  // (a cap below one keeps just the newest: the original's loop never ended there)
  while (!gibs.empty() && static_cast<i32>(gibs.size()) >= max_gibs) {
    // the oldest sleeping gib goes first, else the oldest
    size_t victim = 0;
    for (size_t i = 0; i < gibs.size(); ++i)
      if (gibs[i]->asleep) {
        victim = i;
        break;
      }
    gibs.erase(gibs.begin() + static_cast<std::ptrdiff_t>(victim));
    bodies_.erase(bodies_.begin() + static_cast<std::ptrdiff_t>(victim));
  }
}

V3 GibSystem::world_point(const Gib& g, const V3& rest) const { return rotate(g.rot, rest - g.pivot) + g.pos; }

void GibSystem::write_skin(const Gib& g, f32* out) const { write_rigid(out, g.pos, g.rot, g.pivot); }

void GibSystem::impulse(const V3& center, f64 radius, f64 speed) {
  if (radius <= 0.0) return;
  for (size_t i = 0; i < gibs.size(); ++i) {
    Gib& g = *gibs[i];
    const V3 d = g.pos - center;
    const f64 dist = norm(d);
    if (dist >= radius + g.radius) continue;
    const f64 f = std::max(0.0, 1.0 - std::max(0.0, dist - g.radius) / radius);
    if (f <= 0.0) continue;
    V3 dir = vnorm(d, V3{0, 0, 1});
    dir.z += 0.35;
    dir = vnorm(dir);
    Body& b = bodies_[i];
    const f64 k = speed * f;
    for (int a = 0; a < 3; ++a) {
      g.vel[a] = g.vel[a] + dir[a] * k;
      g.ang[a] = g.ang[a] + (rng_.next() * 2.0 - 1.0) * k * 1.5;
    }
    clamp_velocity(g);
    g.asleep = false;
    b.sleep_timer = 0.0;
    b.calm_v = norm(g.vel);
    b.calm_w = norm(g.ang);
  }
  for (BloodDrop& dr : drops) {
    const V3 d = dr.pos - center;
    const f64 dist = norm(d);
    if (dist >= radius) continue;
    const f64 k = speed * (1.0 - dist / radius);
    const V3 dir = vnorm(d, V3{0, 0, 1});
    for (int a = 0; a < 3; ++a) dr.vel[a] = dr.vel[a] + dir[a] * k;
  }
}

void GibSystem::update(f64 dt) {
  if (dt <= 0.0) return;
  for (size_t i = gibs.size(); i-- > 0;) {
    Gib& g = *gibs[i];
    Body& b = bodies_[i];
    g.age += dt;
    if (g.asleep) {
      check_support(g, b, dt);
      if (g.asleep) continue;
    }
    const f64 reach = norm(g.vel) + norm(g.ang) * g.radius;
    const i32 n = static_cast<i32>(std::min(static_cast<f64>(kMaxSubsteps), std::max(std::ceil(dt / kBaseSubstep), std::ceil(reach * dt / kMaxStepDisp))));
    const f64 h = dt / n;
    for (i32 k = 0; k < n && !g.asleep; ++k) step_gib(g, b, h);
    if (g.bleed > 0.0 && !g.asleep && norm(g.vel) > 0.5) {
      b.bleed_acc += g.bleed * dt;
      while (b.bleed_acc >= 1.0) {
        b.bleed_acc -= 1.0;
        const V3& o = b.samples[size_t(std::floor(rng_.next() * static_cast<f64>(b.samples.size())))];
        const V3 p = rotate(g.rot, o) + g.pos;
        // (the draws in the original's order: the velocity's, then the size)
        const f64 jx = rng_.range(-0.4, 0.4), jy = rng_.range(-0.4, 0.4), jz = rng_.range(0.0, 0.6);
        const f64 size = rng_.range(0.008, 0.018);
        add_drop(p, V3{g.vel.x * 0.3 + jx, g.vel.y * 0.3 + jy, g.vel.z * 0.3 + jz}, size, kBlood);
      }
    }
    if (g.pos.z < kill_z) {
      gibs.erase(gibs.begin() + static_cast<std::ptrdiff_t>(i));
      bodies_.erase(bodies_.begin() + static_cast<std::ptrdiff_t>(i));
    }
  }
  update_drops(dt);
  for (BloodStain& st : stains) st.age += dt;
}

void GibSystem::step_gib(Gib& g, Body& b, f64 h) {
  const CollisionWorld* col = collision;
  const f64 r = g.voxel_size * 0.5;
  // integrate
  g.vel.z -= gravity * h;
  clamp_velocity(g);
  g.pos.x += g.vel.x * h;
  g.pos.y += g.vel.y * h;
  g.pos.z += g.vel.z * h;
  const f64 spin = norm(g.ang);
  if (spin > 1e-9) g.rot = qnormalize(qexp(V3{g.ang.x * h, g.ang.y * h, g.ang.z * h}) * g.rot);
  // broad phase: nothing near the whole chunk
  SphereContact out;
  if (!col || !col->sphere(g.pos, g.radius + r, &out)) {
    b.sleep_timer = 0.0;
    return;
  }
  // contacts of the sample points
  contacts_.clear();
  for (const V3& o : b.samples) {
    const V3 rw = rotate(g.rot, o);
    const V3 p = g.pos + rw;
    if (col->sphere(p, r, &out)) contacts_.push_back(Contact{rw, out.normal});
  }
  if (contacts_.empty()) {
    b.sleep_timer = 0.0;
    return;
  }
  // velocity: sequential impulses (the normal's with restitution, then Coulomb friction)
  const Quat inv = conj(g.rot);
  auto apply_inv_i = [&](const V3& v) {
    V3 l = rotate(inv, v);
    l.x *= b.inv_i.x;
    l.y *= b.inv_i.y;
    l.z *= b.inv_i.z;
    return rotate(g.rot, l);
  };
  auto point_vel = [&](const V3& rw) { return g.vel + cross(g.ang, rw); };
  auto apply_impulse = [&](const V3& rw, const V3& j) {
    g.vel.x += j.x * b.inv_mass;
    g.vel.y += j.y * b.inv_mass;
    g.vel.z += j.z * b.inv_mass;
    g.ang += apply_inv_i(cross(rw, j));
  };
  auto eff_mass = [&](const V3& rw, const V3& n) { return b.inv_mass + dot(n, cross(apply_inv_i(cross(rw, n)), rw)); };
  for (i32 pass = 0; pass < 4; ++pass) {
    for (const Contact& c : contacts_) {
      const V3 vp = point_vel(c.rw);
      const f64 vn = dot(vp, c.n);
      if (vn >= 0.0) continue;
      const f64 e = vn < -1.2 && pass == 0 ? kRestitution : 0.0;
      const f64 jn = (-(1.0 + e) * vn) / eff_mass(c.rw, c.n);
      apply_impulse(c.rw, c.n * jn);
      // friction against the tangential velocity, bounded by mu jn
      const V3 vp2 = point_vel(c.rw);
      const f64 vn2 = dot(vp2, c.n);
      const V3 vt = vp2 - c.n * vn2;
      const f64 vtl = norm(vt);
      if (vtl < 1e-6) continue;
      const V3 t{vt.x / vtl, vt.y / vtl, vt.z / vtl};
      const f64 jt = std::min(vtl / eff_mass(c.rw, t), kFriction * jn);
      apply_impulse(c.rw, V3{-t.x * jt, -t.y * jt, -t.z * jt});
    }
  }
  // rolling resistance; slow bodies in contact settle (no micro-jitter between contacts)
  const bool slow = norm(g.vel) < 0.6 && norm(g.ang) < 2.0;
  const f64 damp = exp(-(slow ? 12.0 : 4.0) * h);
  g.ang = g.ang * damp;
  if (slow) {
    const f64 lin = exp(-6.0 * h);
    g.vel.x *= lin;
    g.vel.y *= lin;
    if (g.vel.z > 0.0) g.vel.z *= lin;
  }
  // positions: out of the deepest penetration, a few times
  for (i32 it = 0; it < 3; ++it) {
    f64 best = 0.0;
    V3 push;
    for (const V3& o : b.samples) {
      const V3 rw = rotate(g.rot, o);
      const V3 p = g.pos + rw;
      if (!col->sphere(p, r, &out)) continue;
      const f64 l = norm(out.push);
      if (l > best) {
        best = l;
        push = out.push;
      }
    }
    if (best <= kSlop) break;
    const f64 k = (best - kSlop * 0.5) / best;
    g.pos.x += push.x * k;
    g.pos.y += push.y * k;
    g.pos.z += push.z * k;
  }
  // sleep on the smoothed motion (a resting body still sees tiny contact impulses)
  const f64 a = 1.0 - exp(-h / 0.15);
  b.calm_v += (norm(g.vel) - b.calm_v) * a;
  b.calm_w += (norm(g.ang) - b.calm_w) * a;
  if (b.calm_v < kSleepSpeed && b.calm_w < kSleepSpin) {
    b.sleep_timer += h;
    if (b.sleep_timer >= kSleepTime) {
      g.asleep = true;
      g.vel = V3{};
      g.ang = V3{};
      b.support_timer = 0.0;
    }
  } else {
    b.sleep_timer = 0.0;
  }
}

// A sleeping gib whose support was blasted away wakes (checked a few times a second).
void GibSystem::check_support(Gib& g, Body& b, f64 dt) {
  b.support_timer += dt;
  if (b.support_timer < 0.25) return;
  b.support_timer = 0.0;
  const f64 r = g.voxel_size * 0.5;
  // the three lowest sample points, probed a little below
  V3 pts[kMaxSamples];
  const size_t n = std::min(b.samples.size(), size_t(kMaxSamples));
  for (size_t i = 0; i < n; ++i) pts[i] = rotate(g.rot, b.samples[i]) + g.pos;
  std::stable_sort(pts, pts + n, [](const V3& p, const V3& q) { return p.z < q.z; });
  SphereContact out;
  for (size_t i = 0; i < std::min(size_t(3), n); ++i) {
    const V3& p = pts[i];
    if (collision && collision->sphere(V3{p.x, p.y, p.z - r * 1.5}, r, &out)) return;
  }
  g.asleep = false;
  b.sleep_timer = 0.0;
  b.calm_v = b.calm_w = 1.0;
}

// ---- blood -----------------------------------------------------------------------------------

void GibSystem::spray(const V3& pos, const V3& dir, i32 count, f64 speed, f64 spread, const Rgb& color) {
  const V3 d = vnorm(dir, V3{0, 0, 1});
  for (i32 i = 0; i < count; ++i) {
    // a random direction in the unit ball, scaled by the spread
    f64 rx = 0.0, ry = 0.0, rz = 0.0;
    for (i32 t = 0; t < 8; ++t) {
      rx = rng_.next() * 2.0 - 1.0;
      ry = rng_.next() * 2.0 - 1.0;
      rz = rng_.next() * 2.0 - 1.0;
      if (rx * rx + ry * ry + rz * rz <= 1.0) break;
    }
    const V3 v = vnorm(V3{d.x + rx * spread * 1.5, d.y + ry * spread * 1.5, d.z + rz * spread * 1.5}, d);
    const f64 sp = speed * rng_.range(0.35, 1.0);
    const f64 size = rng_.range(0.008, 0.022);
    add_drop(pos, V3{v.x * sp, v.y * sp, v.z * sp}, size, color);
  }
}

void GibSystem::add_drop(const V3& pos, const V3& vel, f64 size, const Rgb& color) {
  if (static_cast<i32>(drops.size()) >= max_drops && !drops.empty()) drops.pop_front();
  drops.push_back(BloodDrop{pos, vel, size, 0.0, color});
}

void GibSystem::update_drops(f64 dt) {
  const f64 drag = exp(-kDropDrag * dt);
  size_t w = 0;
  for (size_t i = 0; i < drops.size(); ++i) {
    BloodDrop& dr = drops[i];
    dr.age += dt;
    if (dr.age > kDropLife || dr.pos.z < kill_z) continue;
    dr.vel.z -= gravity * dt;
    dr.vel.x *= drag;
    dr.vel.y *= drag;
    dr.vel.z *= drag;
    const V3 step{dr.vel.x * dt, dr.vel.y * dt, dr.vel.z * dt};
    const f64 len = norm(step);
    if (len > 1e-9) {
      const V3 dir{step.x / len, step.y / len, step.z / len};
      const f64 t = collision ? collision->raycast(dr.pos, dir, len + dr.size) : -1.0;
      if (t >= 0.0) {
        const V3 hit{dr.pos.x + dir.x * t, dr.pos.y + dir.y * t, dr.pos.z + dir.z * t};
        const V3 n = surface_normal(hit, dir);
        add_stain(hit, n, dr.size * rng_.range(1.6, 2.6), dr.color);
        continue;
      }
      dr.pos += step;
    }
    if (w != i) drops[w] = dr;
    ++w;
  }
  drops.resize(w);
}

// The normal of the surface at a ray's hit: the contact normal of a small sphere there.
V3 GibSystem::surface_normal(const V3& hit, const V3& dir) const {
  const V3 back{hit.x - dir.x * 1e-3, hit.y - dir.y * 1e-3, hit.z - dir.z * 1e-3};
  SphereContact out;
  if (collision->sphere(back, 0.01, &out)) return vnorm(out.normal);
  return V3{-dir.x, -dir.y, -dir.z};
}

void GibSystem::add_stain(const V3& pos, const V3& normal, f64 size, const Rgb& color) {
  // merge into a recent stain nearby on the same surface
  const size_t from = stains.size() > 64 ? stains.size() - 64 : 0;
  for (size_t i = stains.size(); i-- > from;) {
    BloodStain& st = stains[i];
    if (dot(st.normal, normal) < 0.9) continue;
    const f64 d = hypot3(st.pos.x - pos.x, st.pos.y - pos.y, st.pos.z - pos.z);
    if (d < st.size * 0.6) {
      st.size = std::min(0.35, std::sqrt(st.size * st.size + size * size * 0.6));
      return;
    }
  }
  if (static_cast<i32>(stains.size()) >= max_stains && !stains.empty()) stains.pop_front();
  stains.push_back(BloodStain{pos, normal, size, 0.0, color});
}

void GibSystem::clear() {
  gibs.clear();
  bodies_.clear();
  drops.clear();
  stains.clear();
}

i64 GibSystem::memory_bytes() const {
  i64 n = static_cast<i64>(sizeof(*this));
  n += static_cast<i64>(gibs.capacity() * sizeof(std::unique_ptr<Gib>) + bodies_.capacity() * sizeof(Body) + contacts_.capacity() * sizeof(Contact));
  for (const auto& g : gibs) n += static_cast<i64>(sizeof(Gib) + g->part.cells.capacity() + g->part.shade.capacity());
  for (const Body& b : bodies_) n += static_cast<i64>(b.samples.capacity() * sizeof(V3));
  n += static_cast<i64>(drops.size() * sizeof(BloodDrop) + stains.size() * sizeof(BloodStain));
  return n;
}

// ---- a character's pieces ------------------------------------------------------------------------

Gib* spawn_gib(GibSystem& gibs, const GibSpec& spec, f64 bleed, u64 user) {
  Gib* g = gibs.spawn(spec.part, spec.voxel_size, spec.bone_pos, spec.bone_rot, spec.bone_rest_head, spec.vel, spec.ang, user);
  g->bleed = bleed;
  return g;
}

std::vector<Gib*> wound_gibs(GibSystem& gibs, const Character& c, const WoundResult& r, const V3& point, const V3& dir, u64 user) {
  // the blood: along the shot, a little back out of the wound, and a drop in the colour of every
  // fourth voxel carved out
  gibs.spray(point, dir, 6 + std::min(18, static_cast<i32>(r.removed.size()) >> 1), 3.2, 0.5);
  gibs.spray(point, dir * -1.0, 3, 1.6, 0.9);
  for (size_t i = 0; i < r.removed.size(); i += 4) {
    const u8 slot = r.removed[i].slot;
    // (the original host's own blood for a slot not in the palette)
    const Rgb color = slot < kSlotCount ? Rgb{c.palette[slot][0], c.palette[slot][1], c.palette[slot][2]} : Rgb{0.3, 0.012, 0.01};
    gibs.spray(point, dir, 1, 2.8, 0.7, color);
  }
  std::vector<Gib*> out;
  for (const GibSpec& g : r.gibs) out.push_back(spawn_gib(gibs, g, kSeveredBleed, user));
  return out;
}

std::vector<Gib*> blast_gibs(GibSystem& gibs, const Character& c, const BlastResult& r, const V3& center, u64 user) {
  std::vector<Gib*> out;
  const V3 mid = c.bounds_center();
  if (r.gibbed) {
    gibs.spray(mid, V3{0, 0, 1}, 60, 5.0, 1.2);
    for (const GibSpec& g : r.gibs) out.push_back(spawn_gib(gibs, g, kBlastBleed, user));
  } else if (r.damage > 15.0) {
    gibs.spray(mid, vnorm(mid - center, V3{0, 0, 1}), std::min(40, static_cast<i32>(std::floor(r.damage / 6.0 + 0.5))), 3.0, 1.0);
  }
  return out;
}

Gib* drop_weapon_gib(GibSystem& gibs, Character& c, const V3& dir, u64 user) {
  std::optional<GibSpec> w = c.drop_weapon();
  if (!w) return nullptr;
  w->vel = w->vel + dir * 1.5;
  return spawn_gib(gibs, *w, 0.0, user);
}

}  // namespace svx::anim
