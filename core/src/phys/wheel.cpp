// structvox — the wheel solver (phys/wheel.hpp, docs/MOTION.md §7): each wheel's tyre cast
// along its suspension, and its rows - suspension, bump stop, brake, tyre - solved with the
// contacts and the joints.
//
// Conventions (world): d the suspension axis down, u = -d; a the axle (steered), f = d x a the
// direction the wheel rolls; a positive spin rolls it forward (its contact patch moves back
// against the hub at spin x radius). In the contact plane (normal n): fx along f, fy = n x fx
// (to the left). Rows act between the carrier (A) and what the wheel stands on (B: a body, or a
// static grid or a sleeping body that nothing moves).
#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "phys_internal.hpp"
#include "svx/base/dmath.hpp"
#include "svx/base/parallel.hpp"
#include "svx/phys/rigid.hpp"

namespace svx {

using namespace phys_detail;

namespace {

// The tyre's cast: samples on its lower arc (angles from straight down, towards where it rolls)
// and across its width. (cos, sin of 0, +-15, +-30, +-45 degrees: constants, the same bits
// everywhere.)
constexpr f64 kArcCos[7] = {1.0, 0.96592582628906829, 0.96592582628906829, 0.86602540378443865, 0.86602540378443865,
                            0.70710678118654752, 0.70710678118654752};
constexpr f64 kArcSin[7] = {0.0, 0.25881904510252076, -0.25881904510252076, 0.5, -0.5, 0.70710678118654752, -0.70710678118654752};
constexpr f64 kAcross[3] = {0.0, 0.35, -0.35};  // x the width
// The cast starts this far (x the radius) above the bump stop: a tyre pressed in through its
// travel still finds the ground (the bump stop holds it).
constexpr f64 kAbove = 0.35;
// Tyre model: slip stiffness per unit load (1/rad, per unit slip ratio), where grip peaks (the
// slip angle and ratio), how much it keeps when it slides, and the speed below which the tyre
// rows are rigid (a carrier at rest holds on a slope, a creeping one does not drift).
constexpr f64 kCornering = 12.0, kLongitudinal = 16.0;
constexpr f64 kPeakAngle = 0.10, kPeakRatio = 0.08;
constexpr f64 kSlide = 0.72;
constexpr f64 kRigidBelow = 0.6;  // m/s
constexpr f64 kRollingResistance = 0.012;  // x the load x the radius: rolling resistance torque
constexpr f64 kMaxSpin = 450.0;            // rad/s
constexpr f64 kWarm = 0.85;
constexpr f64 kTwoPi = 6.283185307179586;


// The effective inverse mass of a linear row along n at arms ra (a) and rb (b).
f64 row_k(f64 ma, f64 mb, const M3& Ia, const M3& Ib, const V3& ra, const V3& rb, const V3& n, bool has_a, bool has_b) {
  f64 k = 0.0;
  if (has_a) {
    const V3 c = cross(ra, n);
    k += ma + dot(c, Ia * c);
  }
  if (has_b) {
    const V3 c = cross(rb, n);
    k += mb + dot(c, Ib * c);
  }
  return k;
}

// How much of its peak grip a tyre keeps at combined slip s (1: at its peak): all of it up to
// the peak, falling off towards kSlide beyond it (a sliding tyre grips less: drifts, lock-ups).
f64 grip_falloff(f64 s) {
  if (!(s > 1.0)) return 1.0;
  const f64 e = s - 1.0;
  return kSlide + (1.0 - kSlide) / (1.0 + 1.5 * e * e);
}

}  // namespace

void RigidWorld::cast_wheels(const std::vector<StaticGrid>& statics) {
  if (wheels.empty()) return;
  const size_t nb = bodies.size();
  auto index_of = [&](i64 id) -> i32 {
    const auto it = std::lower_bound(bodies.begin(), bodies.end(), id, [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
    return (it != bodies.end() && (*it)->id == id) ? static_cast<i32>(it - bodies.begin()) : -1;
  };
  // (what a wheel does not see: its carrier and what is joined to it - the carrier's own parts)
  std::vector<i32> group(nb);
  for (size_t i = 0; i < nb; ++i) group[i] = static_cast<i32>(i);
  auto root = [&](i32 i) {
    while (group[size_t(i)] != i) i = group[size_t(i)] = group[size_t(group[size_t(i)])];
    return i;
  };
  for (const Joint& j : joints) {
    if (j.broken || j.a.body == 0 || j.b.body == 0) continue;
    const i32 a = index_of(j.a.body), b = index_of(j.b.body);
    if (a < 0 || b < 0) continue;
    const i32 ra = root(a), rb = root(b);
    if (ra != rb) group[size_t(std::max(ra, rb))] = std::min(ra, rb);
  }
  for (size_t i = 0; i < nb; ++i) group[i] = root(static_cast<i32>(i));
  // (the bodies by 4 m cells of their boxes: a wheel looks at the ones near it)
  constexpr f64 kCell = 4.0;
  auto cell_of = [](f64 x) { return static_cast<i32>(std::floor(x / kCell)); };
  std::unordered_map<u64, std::vector<i32>> cells;
  for (size_t i = 0; i < nb; ++i) {
    const Body& b = *bodies[i];
    for (i32 x = cell_of(b.box_lo.x); x <= cell_of(b.box_hi.x); ++x)
      for (i32 y = cell_of(b.box_lo.y); y <= cell_of(b.box_hi.y); ++y) cells[key3(x, y, 0)].push_back(static_cast<i32>(i));
  }
  parallel_for(static_cast<i64>(wheels.size()), 8, [&](i64 w0, i64 w1) {
    struct Cache {
      IVec3 cc{INT32_MIN, 0, 0};
      const Chunk* ch = nullptr;
    };
    std::vector<Cache> caches(statics.size());
    auto grid_vox = [&](u32 s, const IVec3& p) -> Vox {
      Cache& c = caches[s];
      const IVec3 cc = chunk_of(p);
      if (cc != c.cc) {
        c.cc = cc;
        c.ch = statics[s].g->chunk(cc);
      }
      if (!c.ch) return kAir;
      return c.ch->uniform ? c.ch->value : c.ch->v[size_t(chunk_index(p))];
    };
    std::vector<i32> near;
    for (i64 k = w0; k < w1; ++k) {
      Wheel& w = wheels[size_t(k)];
      w.contact = false;
      w.ground_body = 0;
      const i32 ia = w.body != 0 && !w.broken ? index_of(w.body) : -1;
      if (ia < 0) {
        w.length = w.rest;
        continue;
      }
      const Body& A = *bodies[size_t(ia)];
      if (A.asleep) {  // (at rest: it stands where it stood)
        w.contact = w.load > 0.0;
        continue;
      }
      const M3 R = to_matrix(A.q);
      const V3 d = normalized(R * w.down);
      const V3 a0 = normalized(R * w.axle);
      const V3 u = d * -1.0;
      // (the axle steered about the suspension's axis)
      const f64 cs = dm::cos(w.steer), sn = dm::sin(w.steer);
      const V3 a = normalized(a0 * cs + cross(u, a0) * sn + u * (dot(u, a0) * (1.0 - cs)));
      const V3 f = normalized(cross(d, a));
      const V3 M = A.x + R * w.p;
      const f64 r = w.radius;
      const f64 lmin = std::max(0.0, w.rest - w.travel);
      const f64 start = lmin - kAbove * r;           // (the cast's first centre, above the bump stop)
      const f64 reach = w.rest - start;              // (to full droop)
      const V3 c0 = M + d * start;
      f64 best = 1e300;
      V3 bp, bn;
      i64 bbody = 0;
      u16 bgrid = 0;
      IVec3 bvox{0, 0, 0};
      u8 bmat = 0;
      // the grids near the wheel
      const f64 span = r + 0.5 * w.width + reach;
      const V3 wlo = M - V3{span, span, span}, whi = M + V3{span, span, span};
      V3 lo, hi;
      for (int q = 0; q < 3; ++q) {
        lo[q] = std::min(wlo[q], wlo[q] + d[q] * (w.rest + r));
        hi[q] = std::max(whi[q], whi[q] + d[q] * (w.rest + r));
      }
      // the bodies near it (not its own)
      near.clear();
      for (i32 x = cell_of(lo.x); x <= cell_of(hi.x); ++x)
        for (i32 y = cell_of(lo.y); y <= cell_of(hi.y); ++y) {
          const auto it = cells.find(key3(x, y, 0));
          if (it == cells.end()) continue;
          for (i32 i : it->second) {
            if (group[size_t(i)] == group[size_t(ia)]) continue;
            const Body& B = *bodies[size_t(i)];
            if (B.box_hi.x < lo.x || B.box_lo.x > hi.x || B.box_hi.y < lo.y || B.box_lo.y > hi.y || B.box_hi.z < lo.z || B.box_lo.z > hi.z) continue;
            near.push_back(i);
          }
        }
      std::sort(near.begin(), near.end());
      near.erase(std::unique(near.begin(), near.end()), near.end());
      for (int ia_ = 0; ia_ < 7; ++ia_)
        for (int iw = 0; iw < 3; ++iw) {
          const V3 O = c0 + (d * kArcCos[ia_] + f * kArcSin[ia_]) * r + a * (kAcross[iw] * w.width);
          // static grids
          for (u32 s = 0; s < static_cast<u32>(statics.size()); ++s) {
            const StaticGrid& G = statics[s];
            if (!G.unbounded && (hi.x < G.lo.x || lo.x > G.hi.x || hi.y < G.lo.y || lo.y > G.hi.y || hi.z < G.lo.z || lo.z > G.hi.z)) continue;
            const f64 h = G.g->h;
            const V3 L = G.xf.from(O);
            const IVec3 p0 = voxel_of(L, 1.0 / h);
            if (vox_solid(grid_vox(s, p0))) continue;  // (it starts in a solid: that is a wall, not the ground under it)
            f64 t;
            int axis, sign;
            IVec3 hit;
            if (!first_solid(L, G.xf.dir_from(d), std::min(reach, best), h, [&](const IVec3& q) { return vox_solid(grid_vox(s, q)); }, &t, &axis,
                             &sign, &hit))
              continue;
            if (!(t < best)) continue;
            V3 n{0, 0, 0};
            n[axis] = sign;
            best = t;
            bp = O + d * t;
            bn = G.xf.dir_to(n);
            bbody = 0;
            bgrid = G.slot;
            bvox = hit;
            bmat = static_cast<u8>(vox_mat(grid_vox(s, hit)));
          }
          // bodies
          for (i32 i : near) {
            const Body& B = *bodies[size_t(i)];
            const M3 RB = to_matrix(B.q), RBt = transpose(RB);
            const V3 sb = B.com + RBt * (O - B.x), db = RBt * d;
            for (size_t m = 0; m < B.shapes.size(); ++m) {
              const BodyShape& S = B.shapes[m];
              const f64 h = S.h;
              const V3 L = S.xf.from(sb);
              const IVec3 p0 = voxel_of(L, 1.0 / h);
              if (vox_solid(S.get(p0))) continue;
              f64 t;
              int axis, sign;
              IVec3 hit;
              if (!first_solid(L, S.xf.dir_from(db), std::min(reach, best), h, [&](const IVec3& q) { return vox_solid(S.get(q)); }, &t, &axis, &sign,
                               &hit))
                continue;
              if (!(t < best)) continue;
              V3 n{0, 0, 0};
              n[axis] = sign;
              best = t;
              bp = O + d * t;
              bn = RB * S.xf.dir_to(n);
              bbody = B.id;
              bgrid = static_cast<u16>(m);  // (a piece: its shape)
              bvox = hit;
              bmat = static_cast<u8>(vox_mat(S.get(hit)));
            }
          }
        }
      if (best >= reach) {
        w.length = w.rest;
        continue;
      }
      w.contact = true;
      w.length = start + best;
      w.point = bp;
      w.normal = normalized(bn);
      w.ground_body = bbody;
      w.ground_grid = bgrid;
      w.ground_voxel = bvox;
      w.ground_mat = bmat;
    }
  });
}

void RigidWorld::prepare_wheels(f64 dt, const std::vector<M3>& Iw) {
  wprep_.assign(wheels.size(), WheelPrep{});
  // (the last substep's impulses, scaled to this one's length: RigidParams::warm_to_step)
  const f64 warm = kWarm * (par.warm_to_step && wheel_dt_ > 0.0 ? dt / wheel_dt_ : 1.0);
  wheel_dt_ = dt;
  if (wheels.empty()) return;
  const MaterialTable& mt = mats ? *mats : default_materials();
  auto index_of = [&](i64 id) -> i32 {
    const auto it = std::lower_bound(bodies.begin(), bodies.end(), id, [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
    return (it != bodies.end() && (*it)->id == id) ? static_cast<i32>(it - bodies.begin()) : -1;
  };
  for (size_t k = 0; k < wheels.size(); ++k) {
    Wheel& w = wheels[k];
    WheelPrep& P = wprep_[k];
    const i32 ia = w.body != 0 && !w.broken ? index_of(w.body) : -1;
    if (ia < 0) continue;
    Body& A = *bodies[size_t(ia)];
    if (A.asleep) continue;
    P.on = true;
    P.ia = ia;
    P.ma = A.inv_mass;
    P.Ia = Iw[size_t(ia)];
    const f64 I = std::max(1e-3, w.inertia);
    const f64 r = w.radius;
    // drive: the drive torque spins the wheel up (explicitly; the tyre row takes it to the ground)
    w.spin = std::clamp(w.spin + w.drive * dt / I, -kMaxSpin, kMaxSpin);
    const M3 R = to_matrix(A.q);
    const V3 d = normalized(R * w.down);
    const V3 a0 = normalized(R * w.axle);
    P.u = d * -1.0;
    const f64 cs = dm::cos(w.steer), sn = dm::sin(w.steer);
    const V3 a = normalized(a0 * cs + cross(P.u, a0) * sn + P.u * (dot(P.u, a0) * (1.0 - cs)));
    const V3 f = normalized(cross(d, a));
    if (!w.contact) {
      // (in the air: the brake and the bearings hold the spin)
      const f64 stop = (std::max(0.0, w.brake) + 2.0) * dt / I;
      w.spin = std::abs(w.spin) <= stop ? 0.0 : w.spin - (w.spin > 0.0 ? stop : -stop);
      w.ls = w.lbump = w.lx = w.ly = w.lb = 0.0;
      continue;
    }
    P.touch = true;
    const V3 n = w.normal;
    P.ra = w.point - A.x;
    if (w.ground_body != 0) {
      const i32 ib = index_of(w.ground_body);
      if (ib >= 0 && !bodies[size_t(ib)]->asleep) {
        const Body& B = *bodies[size_t(ib)];
        P.ib = ib;
        P.mb = B.inv_mass;
        P.Ib = Iw[size_t(ib)];
        P.rb = w.point - B.x;
      }
    }
    P.fx = normalized(f - n * dot(f, n));
    if (!(norm2(P.fx) > 0.5)) P.fx = normalized(cross(a, n));  // (a wheel on its side: never expected)
    P.fy = normalized(cross(n, P.fx));
    const bool hb = P.ib >= 0;
    P.ks = row_k(P.ma, P.mb, P.Ia, P.Ib, P.ra, P.rb, P.u, true, hb);
    P.kx = row_k(P.ma, P.mb, P.Ia, P.Ib, P.ra, P.rb, P.fx, true, hb) + r * r / I;
    P.ky = row_k(P.ma, P.mb, P.Ia, P.Ib, P.ra, P.rb, P.fy, true, hb);
    // the spring and damper, implicit (a soft row): softness 1 / (dt (c + dt k)), target k x / (c + dt k)
    const f64 kx_ = std::max(0.0, w.stiffness), c = std::max(0.0, w.damping);
    const f64 denom = c + dt * kx_;
    const f64 x = std::max(0.0, w.rest - w.length);
    if (denom > 0.0) {
      P.gs = 1.0 / (dt * denom);
      P.bs = kx_ * x / denom;
    }
    const f64 over = x - w.travel;
    if (over > 0.0) P.bump = std::min(par.max_correction, 0.3 * over / dt);
    // the tyre: slip now, and the grip it has at that slip
    const V3 vA = A.v + cross(A.w, P.ra);
    V3 vB;
    if (hb) {
      const Body& B = *bodies[size_t(P.ib)];
      vB = B.v + cross(B.w, P.rb);
    }
    const V3 vr = vA - vB;
    const f64 vfx = dot(P.fx, vr), vfy = dot(P.fy, vr);
    const f64 load = std::max(0.0, kx_ * x - c * dot(P.u, vr));
    const f64 ref = std::max(std::abs(vfx), 3.0);
    const f64 sx = vfx - w.spin * r;
    const f64 slip = std::sqrt((sx / (kPeakRatio * ref)) * (sx / (kPeakRatio * ref)) + (vfy / (kPeakAngle * ref)) * (vfy / (kPeakAngle * ref)));
    const Material& S = mt[w.ground_mat];
    const f64 surface = S.grip > 0.0 ? S.grip : 1.35 * S.friction;
    P.mu = std::max(0.0, w.grip) * surface * grip_falloff(slip);
    // (soft at speed: a force from the slip angle and ratio, cornering and slip stiffness x the load;
    // rigid when creeping or at rest)
    const f64 soft = std::max(0.0, std::abs(vfx) - kRigidBelow);
    if (soft > 0.0 && load > 0.0) {
      P.gy = soft / (kCornering * load * dt);
      P.gx = soft / (kLongitudinal * load * dt);
    }
    // the brake (and rolling resistance) on the spin: a row of at most this torque impulse
    P.brake = (std::max(0.0, w.brake) + kRollingResistance * load * r) * dt;
    // warm start
    w.ls *= warm;
    w.lbump *= warm;
    w.lx *= warm;
    w.ly *= warm;
    w.lb *= warm;
    const V3 J = P.u * (w.ls + w.lbump) + P.fx * w.lx + P.fy * w.ly;
    A.v += J * P.ma;
    A.w += P.Ia * cross(P.ra, J);
    if (hb) {
      Body& B = *bodies[size_t(P.ib)];
      B.v -= J * P.mb;
      B.w -= P.Ib * cross(P.rb, J);
    }
    w.spin += (w.lb - r * w.lx) / I;
  }
}

void RigidWorld::solve_wheels() {
  for (size_t k = 0; k < wheels.size(); ++k) {
    WheelPrep& P = wprep_[k];
    if (!P.on || !P.touch) continue;
    Wheel& w = wheels[k];
    Body& A = *bodies[size_t(P.ia)];
    Body* B = P.ib >= 0 ? bodies[size_t(P.ib)].get() : nullptr;
    const f64 I = std::max(1e-3, w.inertia), r = w.radius;
    auto vrel = [&]() {
      V3 v = A.v + cross(A.w, P.ra);
      if (B) v -= B->v + cross(B->w, P.rb);
      return v;
    };
    auto apply = [&](const V3& J) {
      A.v += J * P.ma;
      A.w += P.Ia * cross(P.ra, J);
      if (B) {
        B->v -= J * P.mb;
        B->w -= P.Ib * cross(P.rb, J);
      }
    };
    // suspension (soft, pushes only) and bump stop (rigid, pushes only)
    {
      const f64 vs = dot(P.u, vrel());
      const f64 dl = (P.bs - vs - P.gs * w.ls) / (P.ks + P.gs);
      const f64 nl = std::max(0.0, w.ls + dl);
      apply(P.u * (nl - w.ls));
      w.ls = nl;
    }
    if (P.bump > 0.0 || w.lbump > 0.0) {
      const f64 vs = dot(P.u, vrel());
      const f64 dl = (P.bump - vs) / std::max(1e-12, P.ks);
      const f64 nl = std::max(0.0, w.lbump + dl);
      apply(P.u * (nl - w.lbump));
      w.lbump = nl;
    }
    // brake: holds the spin (relative to the carrier) at most with its torque
    if (P.brake > 0.0) {
      const f64 nl = std::clamp(w.lb - w.spin * I, -P.brake, P.brake);
      w.spin += (nl - w.lb) / I;
      w.lb = nl;
    }
    // tyre: longitudinal (with the spin), lateral; together within the friction ellipse
    const f64 limit = P.mu * (w.ls + w.lbump);
    f64 nx, ny;
    {
      const f64 sx = dot(P.fx, vrel()) - w.spin * r;
      nx = w.lx - (sx + P.gx * w.lx) / (P.kx + P.gx);
      nx = std::clamp(nx, -limit, limit);
      apply(P.fx * (nx - w.lx));
      w.spin -= r * (nx - w.lx) / I;
      w.lx = nx;
    }
    {
      const f64 sy = dot(P.fy, vrel());
      ny = w.ly - (sy + P.gy * w.ly) / (P.ky + P.gy);
      ny = std::clamp(ny, -limit, limit);
      apply(P.fy * (ny - w.ly));
      w.ly = ny;
    }
    const f64 m2 = nx * nx + ny * ny;
    if (m2 > limit * limit && m2 > 0.0) {
      const f64 s = limit / std::sqrt(m2);
      const f64 cx = nx * s, cy = ny * s;
      apply(P.fx * (cx - w.lx) + P.fy * (cy - w.ly));
      w.spin -= r * (cx - w.lx) / I;
      w.lx = cx;
      w.ly = cy;
    }
  }
}

void RigidWorld::finish_wheels(f64 dt) {
  for (size_t k = 0; k < wheels.size(); ++k) {
    Wheel& w = wheels[k];
    const WheelPrep& P = k < wprep_.size() ? wprep_[k] : WheelPrep{};
    if (!P.on) continue;  // (its carrier asleep: it carries what it did when it fell asleep)
    w.angle += w.spin * dt;
    if (w.angle >= 0.5 * kTwoPi || w.angle < -0.5 * kTwoPi) w.angle -= kTwoPi * std::floor(w.angle / kTwoPi + 0.5);
    if (!P.touch) {
      w.load = 0.0;
      w.force = V3{};
      w.slip_long = w.slip_lat = 0.0;
      continue;
    }
    const V3 J = P.u * (w.ls + w.lbump) + P.fx * w.lx + P.fy * w.ly;
    w.force = J * (1.0 / dt);
    w.load = (w.ls + w.lbump) / dt;
    const Body& A = *bodies[size_t(P.ia)];
    V3 v = A.v + cross(A.w, P.ra);
    if (P.ib >= 0) {
      const Body& B = *bodies[size_t(P.ib)];
      v -= B.v + cross(B.w, P.rb);
    }
    w.slip_long = dot(P.fx, v) - w.spin * w.radius;
    w.slip_lat = dot(P.fy, v);
    if (w.break_force > 0.0 && norm(w.force) > w.break_force) w.broken = true;
  }
}

void RigidWorld::wheel_support(const std::function<void(i32, bool, f64)>& push) const {
  // (a carrier on grounded wheels is held up by them, as by contacts facing up)
  constexpr f64 kUp = 0.1;
  auto index_of = [&](i64 id) -> i32 {
    const auto it = std::lower_bound(bodies.begin(), bodies.end(), id, [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
    return (it != bodies.end() && (*it)->id == id) ? static_cast<i32>(it - bodies.begin()) : -1;
  };
  for (size_t k = 0; k < wheels.size() && k < wprep_.size(); ++k) {
    const Wheel& w = wheels[k];
    const WheelPrep& P = wprep_[k];
    if (!P.on || !P.touch || !(w.ls + w.lbump > 0.0)) continue;
    // (the carrier by id: a fracture since the solve may have changed the body list)
    const i32 ia = index_of(w.body);
    if (ia < 0) continue;
    // (what it stands on must be held itself: the world, a sleeping body, or a held one - the
    // caller's push resolves that through its queue; here: the world or a sleeper)
    const i32 ib = w.ground_body != 0 ? index_of(w.ground_body) : -1;
    if (w.ground_body != 0 && ib >= 0 && !bodies[size_t(ib)]->asleep) continue;
    push(ia, w.normal.z > kUp, (w.ls + w.lbump) * P.u.z);
  }
}

void RigidWorld::wheel_stillness() {
  // (a wheel driven, or spinning, keeps its carrier awake: one accelerating from rest)
  auto index_of = [&](i64 id) -> i32 {
    const auto it = std::lower_bound(bodies.begin(), bodies.end(), id, [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
    return (it != bodies.end() && (*it)->id == id) ? static_cast<i32>(it - bodies.begin()) : -1;
  };
  for (const Wheel& w : wheels) {
    if (w.body == 0 || w.broken) continue;
    if (w.drive == 0.0 && std::abs(w.spin) < 0.5) continue;
    const i32 i = index_of(w.body);
    if (i >= 0) bodies[size_t(i)]->still = 0;
  }
}

}  // namespace svx
