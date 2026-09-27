#include "svx/phys/rigid.hpp"

#include "svx/base/parallel.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cmath>

namespace svx {

namespace {

inline IVec3 voxel_of(const V3& s, f64 inv_h) {
  return {static_cast<i32>(std::floor(s.x * inv_h + 0.5)), static_cast<i32>(std::floor(s.y * inv_h + 0.5)),
          static_cast<i32>(std::floor(s.z * inv_h + 0.5))};
}

inline u64 mix64(u64 x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// The face of least penetration of point s (in a voxel frame) inside solid voxel p that leads to
// air, per `solid`: normal (axis, sign) and depth. Returns false if every face is buried.
template <class Solid>
bool exit_face(const V3& s, const IVec3& p, f64 h, Solid&& solid, int* axis, int* sign, f64* depth) {
  f64 best = 1e300;
  bool found = false;
  for (int a = 0; a < 3; ++a)
    for (int sg = -1; sg <= 1; sg += 2) {
      IVec3 nb = p;
      nb[a] += sg;
      if (solid(nb)) continue;
      const f64 face = h * (p[a] + 0.5 * sg);
      const f64 d = sg > 0 ? face - s[a] : s[a] - face;
      if (d < best) {
        best = d;
        *axis = a;
        *sign = sg;
        found = true;
      }
    }
  *depth = std::max(0.0, best);
  return found;
}

void tangents(const V3& n, V3& t1, V3& t2) {
  if (std::abs(n.x) < 0.57) {
    t1 = normalized(cross(n, V3{1, 0, 0}));
  } else {
    t1 = normalized(cross(n, V3{0, 1, 0}));
  }
  t2 = cross(n, t1);
}

}  // namespace

void body_refresh(Body& b, f64 h, int max_points) {
  // mass properties from the fragments
  b.mass = 0.0;
  V3 c{0, 0, 0};
  for (const BodyFrag& f : b.frags) {
    if (f.count <= 0) continue;
    b.mass += f.mass;
    c += f.com * f.mass;
  }
  b.inv_mass = b.mass > 0 ? 1.0 / b.mass : 0.0;
  b.com = b.mass > 0 ? c * (1.0 / b.mass) : V3{};
  M3 I;
  for (const BodyFrag& f : b.frags) {
    if (f.count <= 0) continue;
    const V3 d = f.com - b.com;
    const f64 dd = dot(d, d);
    for (int r = 0; r < 3; ++r)
      for (int q = 0; q < 3; ++q) I(r, q) += f.inertia(r, q) + f.mass * ((r == q ? dd : 0.0) - d[r] * d[q]);
  }
  b.inertia = I;
  if (!inverse(I, b.inv_inertia)) b.inv_inertia = M3{};
  // collision samples: lattice corners of exposed faces, pulled 0.1 h into their voxel; each
  // corner once (a bitmap over the corner lattice), in voxel scan order (deterministic)
  const BodyShape& S = b.shape;
  struct Cand {
    V3 pt;
    i32 vox;
    bool sharp;
  };
  std::vector<Cand> uniq;
  auto solid = [&](const IVec3& p) { return vox_solid(S.get(p)); };
  const i64 cx = S.dim[0] + 1, cy = S.dim[1] + 1, cz = S.dim[2] + 1;
  std::vector<u8> seen(static_cast<size_t>((cx * cy * cz + 7) / 8), 0);
  for (i32 i = 0; i < static_cast<i32>(S.vox.size()); ++i) {
    if (!vox_solid(S.vox[size_t(i)])) continue;
    const IVec3 p = S.voxel(i);
    for (int a = 0; a < 3; ++a)
      for (int sg = -1; sg <= 1; sg += 2) {
        IVec3 nb = p;
        nb[a] += sg;
        if (solid(nb)) continue;
        const int a1 = (a + 1) % 3, a2 = (a + 2) % 3;
        for (int u = -1; u <= 1; u += 2)
          for (int w = -1; w <= 1; w += 2) {
            // the corner p + (d / 2) and its index on the (dim + 1)^3 corner lattice
            int d[3];
            d[a] = sg;
            d[a1] = u;
            d[a2] = w;
            const i64 ix = (p[0] - S.lo[0]) + (d[0] > 0 ? 1 : 0), iy = (p[1] - S.lo[1]) + (d[1] > 0 ? 1 : 0),
                      iz = (p[2] - S.lo[2]) + (d[2] > 0 ? 1 : 0);
            const i64 ci = (ix * cy + iy) * cz + iz;
            if (seen[size_t(ci >> 3)] & (1u << (ci & 7))) continue;
            seen[size_t(ci >> 3)] = static_cast<u8>(seen[size_t(ci >> 3)] | (1u << (ci & 7)));
            int around = 0;
            for (int dx = 0; dx < 2; ++dx)
              for (int dy = 0; dy < 2; ++dy)
                for (int dz = 0; dz < 2; ++dz) {
                  const IVec3 q{static_cast<i32>(S.lo[0] + ix - 1 + dx), static_cast<i32>(S.lo[1] + iy - 1 + dy),
                                static_cast<i32>(S.lo[2] + iz - 1 + dz)};
                  around += solid(q) ? 1 : 0;
                }
            const V3 centre{h * p[0], h * p[1], h * p[2]};
            const V3 corner = centre + V3{0.4 * h * d[0], 0.4 * h * d[1], 0.4 * h * d[2]};
            uniq.push_back({corner, i, around <= 2});
          }
      }
  }
  std::vector<const Cand*> keep;
  size_t sharp = 0;
  for (const Cand& cd : uniq) sharp += cd.sharp ? 1 : 0;
  // small pieces need few samples (their corners), large ones more (flat faces as well)
  const f64 surf = std::cbrt(static_cast<f64>(std::max(1, S.count)));
  const size_t cap = static_cast<size_t>(std::max(8, std::min(max_points, static_cast<int>(12.0 + 1.5 * surf * surf))));
  if (uniq.size() <= cap) {
    for (const Cand& cd : uniq) keep.push_back(&cd);
  } else {
    // all sharp corners (strided if they alone exceed the cap), flat points strided
    const size_t sharp_keep = std::min(sharp, cap * 3 / 4);
    const size_t flat = uniq.size() - sharp;
    const size_t flat_keep = cap - sharp_keep;
    size_t si = 0, fi = 0;
    for (const Cand& cd : uniq) {
      if (cd.sharp) {
        if (sharp_keep > 0 && (si * sharp_keep) / sharp != ((si + 1) * sharp_keep) / sharp) keep.push_back(&cd);
        ++si;
      } else {
        if (flat > 0 && (fi * flat_keep) / flat != ((fi + 1) * flat_keep) / flat) keep.push_back(&cd);
        ++fi;
      }
    }
  }
  b.pts.clear();
  b.pt_vox.clear();
  b.radius = 0.0;
  for (const Cand* cd : keep) {
    const V3 r = cd->pt - b.com;
    b.pts.push_back(r);
    b.pt_vox.push_back(cd->vox);
    b.radius = std::max(b.radius, norm(r));
  }
}

void RigidWorld::add(std::unique_ptr<Body> b) {
  const i64 id = b->id;
  auto it = std::lower_bound(bodies.begin(), bodies.end(), id, [](const std::unique_ptr<Body>& x, i64 v) { return x->id < v; });
  bodies.insert(it, std::move(b));
}

void RigidWorld::remove_if(const std::function<bool(const Body&)>& pred) {
  bodies.erase(std::remove_if(bodies.begin(), bodies.end(), [&](const std::unique_ptr<Body>& b) { return pred(*b); }),
               bodies.end());
}

Body* RigidWorld::find(i64 id) {
  auto it = std::lower_bound(bodies.begin(), bodies.end(), id, [](const std::unique_ptr<Body>& x, i64 v) { return x->id < v; });
  return (it != bodies.end() && (*it)->id == id) ? it->get() : nullptr;
}

void RigidWorld::wake(Body& b) {
  b.asleep = false;
  b.still = 0;
}

void RigidWorld::wake_box(const V3& lo, const V3& hi) {
  for (auto& bp : bodies) {
    Body& b = *bp;
    if (!b.asleep) continue;
    if (b.box_hi.x < lo.x || b.box_lo.x > hi.x || b.box_hi.y < lo.y || b.box_lo.y > hi.y || b.box_hi.z < lo.z ||
        b.box_lo.z > hi.z)
      continue;
    wake(b);
  }
}

i32 RigidWorld::awake_count() const {
  i32 n = 0;
  for (const auto& b : bodies) n += b->asleep ? 0 : 1;
  return n;
}

void RigidWorld::refresh_boxes() {
  for (auto& bp : bodies) bp->refresh_box();  // (a sphere bound: cheap and conservative)
}

void RigidWorld::integrate_velocities(f64 dt) {
  const f64 ld = std::max(0.0, 1.0 - par.linear_damping * dt);
  const f64 ad = std::max(0.0, 1.0 - par.angular_damping * dt);
  for (auto& bp : bodies) {
    Body& b = *bp;
    if (b.asleep) continue;
    b.v.z -= par.gravity * dt;
    b.v *= ld;
    b.w *= ad;
    const f64 s = norm(b.v);
    if (s > par.max_speed) b.v *= par.max_speed / s;
    const f64 ws = norm(b.w) * std::max(b.radius, 0.1);
    if (ws > par.max_speed) b.w *= par.max_speed / ws;
    b.v_pre = b.v;
    b.w_pre = b.w;
  }
}

void RigidWorld::reduce_manifold(std::vector<Contact>& cs, f64 h) const {
  if (cs.empty()) return;
  // (large bodies rest on many points: their loads spread as on a real bearing surface)
  const f64 rad = bodies[size_t(cs.front().a)]->radius;
  const size_t cap = static_cast<size_t>(std::max(4, par.manifold + static_cast<int>(par.manifold_per_m * rad)));
  if (cs.size() <= cap) return;
  std::sort(cs.begin(), cs.end(), [](const Contact& x, const Contact& y) {
    return x.depth > y.depth || (x.depth == y.depth && x.key < y.key);
  });
  std::vector<Contact> kept;
  std::vector<u8> used(cs.size(), 0);
  const f64 dmin2 = (1.5 * h) * (1.5 * h);
  for (size_t k = 0; k < cs.size() && kept.size() < cap; ++k) {
    bool far = true;
    for (const Contact& c : kept)
      if (norm2(c.p - cs[k].p) < dmin2) {
        far = false;
        break;
      }
    if (far) {
      kept.push_back(cs[k]);
      used[k] = 1;
    }
  }
  for (size_t k = 0; k < cs.size() && kept.size() < cap; ++k)
    if (!used[k]) kept.push_back(cs[k]);
  cs.swap(kept);
}

namespace {

// The samples' world positions, kept on the body while its pose is unchanged (sleepers).
const std::vector<V3>& world_points(Body& B) {
  if (B.wpts.size() != B.pts.size() || B.wpts_x.x != B.x.x || B.wpts_x.y != B.x.y || B.wpts_x.z != B.x.z || B.wpts_q.x != B.q.x || B.wpts_q.y != B.q.y || B.wpts_q.z != B.q.z ||
      B.wpts_q.w != B.q.w) {
    const M3 R = to_matrix(B.q);
    B.wpts.resize(B.pts.size());
    for (size_t k = 0; k < B.pts.size(); ++k) B.wpts[k] = B.x + R * B.pts[k];
    B.wpts_x = B.x;
    B.wpts_q = B.q;
  }
  return B.wpts;
}

}  // namespace

void RigidWorld::collide(const VoxelGrid& g, const std::vector<u8>* only) {
  // only (optional): contacts of these bodies alone (the others' contacts are kept by the caller)
  if (!only) contacts_.clear();
  const f64 h = g.h, ih = 1.0 / h;
  const i32 nb = static_cast<i32>(bodies.size());
  {
    bool any = false;
    for (const auto& bp : bodies)
      if (!bp->asleep) {
        any = true;
        break;
      }
    if (!any) return;  // (everything asleep: no contacts)
  }
  refresh_boxes();
  auto selected = [&](i32 i) { return !only || (*only)[size_t(i)]; };
  using CClock = std::chrono::steady_clock;
  const auto c0 = CClock::now();
  // sample positions (in parallel; every body a pair may test)
  parallel_for(nb, 64, [&](i64 b0, i64 b1) {
    for (i64 i = b0; i < b1; ++i) world_points(*bodies[size_t(i)]);
  });
  // world contacts, per body in parallel (each with its own chunk cache), joined in body order
  std::vector<std::vector<Contact>> wc(static_cast<size_t>(nb));
  parallel_for(nb, 16, [&](i64 b0, i64 b1) {
    IVec3 cache_cc{INT32_MIN, 0, 0};
    const Chunk* cache_ch = nullptr;
    auto world_vox = [&](const IVec3& p) -> Vox {
      const IVec3 cc = chunk_of(p);
      if (cc != cache_cc) {
        cache_cc = cc;
        cache_ch = g.chunk(cc);
      }
      if (!cache_ch) return kAir;
      return cache_ch->uniform ? cache_ch->value : cache_ch->v[size_t(chunk_index(p))];
    };
    for (i64 i = b0; i < b1; ++i) {
      const i32 ia = static_cast<i32>(i);
      Body& A = *bodies[size_t(ia)];
      if (A.asleep || !selected(ia)) continue;
      std::vector<Contact>& out = wc[size_t(ia)];
      const auto& W = A.wpts;
      for (size_t k = 0; k < A.pts.size(); ++k) {
        const V3& X = W[k];
        const IVec3 p = voxel_of(X, ih);
        if (!vox_solid(world_vox(p))) continue;
        int axis = 2, sign = 1;
        f64 depth = 0.5 * h;
        if (!exit_face(X, p, h, [&](const IVec3& q) { return vox_solid(world_vox(q)); }, &axis, &sign, &depth)) {
          axis = 2;
          sign = 1;
          depth = 0.5 * h;
        }
        Contact c;
        c.a = ia;
        c.b = -1;
        c.p = X;
        c.n = V3{};
        c.n[axis] = sign;
        c.depth = depth;
        c.vox_a = A.pt_vox[k];
        c.wvox = p;
        c.key = mix64(static_cast<u64>(A.id) * 0x100000001B3ull ^ mix64(~0ull) ^ (static_cast<u64>(k) << 1));
        out.push_back(c);
      }
      reduce_manifold(out, h);
    }
  });
  for (auto& v : wc)
    for (const Contact& c : v) contacts_.push_back(c);
  const auto c1 = CClock::now();
  // body pairs: sweep over x (the order of the sweep is the pairs' order)
  std::vector<i32> order(static_cast<size_t>(nb));
  for (i32 i = 0; i < nb; ++i) order[size_t(i)] = i;
  std::sort(order.begin(), order.end(), [&](i32 x, i32 y) {
    const f64 ax = bodies[size_t(x)]->box_lo.x, ay = bodies[size_t(y)]->box_lo.x;
    return ax < ay || (ax == ay && x < y);
  });
  // (in parallel over fixed runs of the sorted order, joined in order)
  constexpr i64 kSweepGrain = 64;
  const i64 nruns = (static_cast<i64>(order.size()) + kSweepGrain - 1) / kSweepGrain;
  std::vector<std::vector<std::pair<i32, i32>>> runs(static_cast<size_t>(nruns));
  parallel_for(static_cast<i64>(order.size()), kSweepGrain, [&](i64 i0, i64 i1) {
    auto& out = runs[size_t(i0 / kSweepGrain)];
    for (i64 i = i0; i < i1; ++i) {
      const Body& A = *bodies[size_t(order[size_t(i)])];
      for (size_t j = size_t(i) + 1; j < order.size(); ++j) {
        const Body& B = *bodies[size_t(order[j])];
        if (B.box_lo.x > A.box_hi.x) break;
        if (A.asleep && B.asleep) continue;
        if (!selected(order[size_t(i)]) && !selected(order[j])) continue;
        if (A.family_ticks > 0 && B.family_ticks > 0 && A.family == B.family) continue;
        if (A.box_hi.y < B.box_lo.y || B.box_hi.y < A.box_lo.y || A.box_hi.z < B.box_lo.z || B.box_hi.z < A.box_lo.z) continue;
        out.push_back({std::min(order[size_t(i)], order[j]), std::max(order[size_t(i)], order[j])});
      }
    }
  });
  std::vector<std::pair<i32, i32>> pairs;
  for (const auto& r : runs) pairs.insert(pairs.end(), r.begin(), r.end());
  const auto c2 = CClock::now();
  std::vector<std::vector<Contact>> pc(pairs.size());
  parallel_for(static_cast<i64>(pairs.size()), 8, [&](i64 q0, i64 q1) {
    std::vector<Contact> one;
    for (i64 q = q0; q < q1; ++q) {
      const auto& pr = pairs[size_t(q)];
      for (int dir = 0; dir < 2; ++dir) {
        const i32 ia = dir == 0 ? pr.first : pr.second;
        const i32 ib = dir == 0 ? pr.second : pr.first;
        const Body& A = *bodies[size_t(ia)];
        const Body& B = *bodies[size_t(ib)];
        const M3 RB = to_matrix(B.q);
        const M3 RBt = transpose(RB);
        const f64 reach2 = (B.radius + h) * (B.radius + h);
        const auto& W = A.wpts;
        auto solidB = [&](const IVec3& qv) { return vox_solid(B.shape.get(qv)); };
        for (size_t k = 0; k < A.pts.size(); ++k) {
          const V3& X = W[k];
          if (norm2(X - B.x) > reach2) continue;
          const V3 sp = B.com + RBt * (X - B.x);
          const IVec3 p = voxel_of(sp, ih);
          const i32 vi = B.shape.index(p);
          if (vi < 0 || !vox_solid(B.shape.vox[size_t(vi)])) continue;
          int axis = 2, sign = 1;
          f64 depth = 0.5 * h;
          V3 ns{0, 0, 0};
          if (exit_face(sp, p, h, solidB, &axis, &sign, &depth)) {
            ns[axis] = sign;
          } else {
            ns = normalized(sp - B.com);
            if (norm2(ns) == 0) ns = V3{0, 0, 1};
            depth = 0.5 * h;
          }
          Contact c;
          c.a = ia;
          c.b = ib;
          c.p = X;
          c.n = RB * ns;
          c.depth = depth;
          c.vox_a = A.pt_vox[k];
          c.vox_b = vi;
          c.key = mix64(static_cast<u64>(A.id) * 0x100000001B3ull ^ mix64(static_cast<u64>(B.id)) ^ (static_cast<u64>(k) << 1));
          one.push_back(c);
        }
        reduce_manifold(one, h);
        for (const Contact& c : one) pc[size_t(q)].push_back(c);
        one.clear();
      }
    }
  });
  const auto c3 = CClock::now();
  static const bool cprof = std::getenv("SVX_PROFILE_COLLIDE") != nullptr;
  if (cprof) {
    auto ms = [](CClock::time_point a, CClock::time_point b) { return std::chrono::duration<f64, std::milli>(b - a).count(); };
    static f64 acc[3] = {0, 0, 0};
    static i64 np = 0, calls = 0;
    acc[0] += ms(c0, c1);
    acc[1] += ms(c1, c2);
    acc[2] += ms(c2, c3);
    np += static_cast<i64>(pairs.size());
    if (++calls % 120 == 0) {
      std::printf("  [collide] per call: world %.2f ms, broadphase %.2f ms, pairs %.2f ms (%lld pairs)\n", acc[0] / 120, acc[1] / 120,
                  acc[2] / 120, static_cast<long long>(np / 120));
      acc[0] = acc[1] = acc[2] = 0;
      np = 0;
    }
  }
  for (size_t q = 0; q < pairs.size(); ++q) {
    for (const Contact& c : pc[q]) contacts_.push_back(c);
    // an awake body moving near a sleeping one wakes it (whether it pushes it, or moves away
    // from under it)
    Body& A = *bodies[size_t(pairs[q].first)];
    Body& B = *bodies[size_t(pairs[q].second)];
    if (A.asleep != B.asleep) {
      Body& moving = A.asleep ? B : A;
      Body& sleeper = A.asleep ? A : B;
      const f64 sp = norm(moving.v) + moving.radius * norm(moving.w);
      if (sp > (!pc[q].empty() ? 2.0 : 3.0) * par.sleep_speed) wake(sleeper);
    }
  }
}

void RigidWorld::solve(f64 dt) {
  auto invm = [&](i32 i) -> f64 { return (i < 0 || bodies[size_t(i)]->asleep) ? 0.0 : bodies[size_t(i)]->inv_mass; };
  std::vector<M3> Iw(bodies.size());
  for (size_t i = 0; i < bodies.size(); ++i)
    if (!bodies[i]->asleep) Iw[i] = bodies[i]->inv_inertia_world();
  auto eff = [&](const Contact& c, const V3& d) {
    f64 k = invm(c.a) + invm(c.b);
    if (c.a >= 0 && !bodies[size_t(c.a)]->asleep) {
      const V3 rd = cross(c.ra, d);
      k += dot(rd, Iw[size_t(c.a)] * rd);
    }
    if (c.b >= 0 && !bodies[size_t(c.b)]->asleep) {
      const V3 rd = cross(c.rb, d);
      k += dot(rd, Iw[size_t(c.b)] * rd);
    }
    return k > 0 ? 1.0 / k : 0.0;
  };
  auto vel = [&](i32 i, const V3& r) -> V3 {
    if (i < 0) return V3{};
    const Body& B = *bodies[size_t(i)];
    return B.v + cross(B.w, r);
  };
  auto apply = [&](const Contact& c, const V3& J) {
    if (c.a >= 0 && !bodies[size_t(c.a)]->asleep) {
      Body& A = *bodies[size_t(c.a)];
      A.v += J * A.inv_mass;
      A.w += Iw[size_t(c.a)] * cross(c.ra, J);
    }
    if (c.b >= 0 && !bodies[size_t(c.b)]->asleep) {
      Body& B = *bodies[size_t(c.b)];
      B.v -= J * B.inv_mass;
      B.w -= Iw[size_t(c.b)] * cross(c.rb, J);
    }
  };
  for (Contact& c : contacts_) {
    c.ra = c.p - bodies[size_t(c.a)]->x;
    c.rb = c.b >= 0 ? c.p - bodies[size_t(c.b)]->x : V3{};
    tangents(c.n, c.t1, c.t2);
    c.kn = eff(c, c.n);
    c.k1 = eff(c, c.t1);
    c.k2 = eff(c, c.t2);
    const f64 vn = dot(vel(c.a, c.ra) - vel(c.b, c.rb), c.n);
    c.approach = std::max(0.0, -vn);
    c.bounce = vn < -par.bounce_speed ? -par.restitution * vn : 0.0;
    c.mu = par.friction;
    c.bias = std::min(par.max_correction, par.baumgarte * std::max(0.0, c.depth - par.slop) / dt);
  }
  // (warm starts after every contact read its approach from the velocities before the solve)
  for (Contact& c : contacts_) {
    const auto it = warm_.find(c.key);
    if (it != warm_.end()) {
      c.ln = 0.85 * it->second[0];
      c.l1 = 0.85 * it->second[1];
      c.l2 = 0.85 * it->second[2];
      apply(c, c.n * c.ln + c.t1 * c.l1 + c.t2 * c.l2);
    }
  }
  // Parallel Gauss-Seidel by graph colouring. A pair's manifold (consecutive contacts of the same
  // two bodies, or a body and the world) is solved in order as one group; the groups of one colour
  // share no awake body, so they are solved concurrently (bitwise the same in any order). Groups
  // beyond the colours (a large piece touching many) are solved after them, in order.
  constexpr int kColors = 24;
  std::vector<i32> gstart;  // group g: contacts [gstart[g], gstart[g + 1])
  for (i32 k = 0; k < static_cast<i32>(contacts_.size()); ++k) {
    const Contact& c = contacts_[size_t(k)];
    if (k > 0) {
      const Contact& p = contacts_[size_t(k - 1)];
      const bool same = (p.a == c.a && p.b == c.b) || (p.a == c.b && p.b == c.a);
      if (same) continue;
    }
    gstart.push_back(k);
  }
  const i32 ng = static_cast<i32>(gstart.size());
  gstart.push_back(static_cast<i32>(contacts_.size()));
  std::vector<std::vector<i32>> colour(kColors + 1);
  {
    std::vector<u32> used(bodies.size(), 0);
    for (i32 gi = 0; gi < ng; ++gi) {
      const Contact& c = contacts_[size_t(gstart[size_t(gi)])];
      const bool da = c.a >= 0 && !bodies[size_t(c.a)]->asleep;
      const bool db = c.b >= 0 && !bodies[size_t(c.b)]->asleep;
      const u32 m = (da ? used[size_t(c.a)] : 0u) | (db ? used[size_t(c.b)] : 0u);
      int col = 0;
      while (col < kColors && (m >> col) & 1u) ++col;
      if (col < kColors) {
        if (da) used[size_t(c.a)] |= 1u << col;
        if (db) used[size_t(c.b)] |= 1u << col;
      }
      colour[size_t(col)].push_back(gi);
    }
  }
  static const bool dbg_col = std::getenv("SVX_DEBUG_COLOUR") != nullptr;
  if (dbg_col && contacts_.size() > 10000) {
    static int shown = 0;
    if (shown++ % 50 == 0) {
      std::printf("  [colour] %zu contacts, %d groups:", contacts_.size(), ng);
      for (int col = 0; col <= kColors; ++col) std::printf(" %zu", colour[size_t(col)].size());
      std::printf("\n");
    }
  }
  auto sweep = [&](const auto& fn) {
    auto group = [&](i32 gi) {
      for (i32 k = gstart[size_t(gi)]; k < gstart[size_t(gi) + 1]; ++k) fn(contacts_[size_t(k)]);
    };
    for (int col = 0; col < kColors; ++col) {
      const std::vector<i32>& L = colour[size_t(col)];
      if (L.size() < 32) {
        for (i32 gi : L) group(gi);
      } else {
        parallel_for(static_cast<i64>(L.size()), 16, [&](i64 q0, i64 q1) {
          for (i64 q = q0; q < q1; ++q) group(L[size_t(q)]);
        });
      }
    }
    for (i32 gi : colour[kColors]) group(gi);
  };
  // (many contacts: a collapse at its peak; fewer iterations each, a function of the contact count
  // alone so every thread count and platform does the same)
  // (a collapse's peak, or a large pile settling: fewer iterations; a function of the state and
  // the contact count alone, the same on every thread count and platform)
  const bool reduced = busy_ || contacts_.size() > par.busy_contacts;
  const int vel_iters = reduced ? par.busy_iterations : par.iterations;
  const int pos_iters = reduced ? std::min(2, par.position_iterations) : par.position_iterations;
  for (int it = 0; it < vel_iters; ++it)
    sweep([&](Contact& c) {
      V3 dv = vel(c.a, c.ra) - vel(c.b, c.rb);
      const f64 ln = std::max(0.0, c.ln + c.kn * (c.bounce - dot(dv, c.n)));
      apply(c, c.n * (ln - c.ln));
      c.ln = ln;
      dv = vel(c.a, c.ra) - vel(c.b, c.rb);
      const f64 lim = c.mu * c.ln;
      const f64 l1 = std::clamp(c.l1 - c.k1 * dot(dv, c.t1), -lim, lim);
      const f64 l2 = std::clamp(c.l2 - c.k2 * dot(dv, c.t2), -lim, lim);
      apply(c, c.t1 * (l1 - c.l1) + c.t2 * (l2 - c.l2));
      c.l1 = l1;
      c.l2 = l2;
    });
  // Squeeze guard: a light body pinned between heavy ones can come out of the iteration with a
  // speed no contact partner has (and fly off). Contacts may slow a body down freely but speed it
  // up only to about its fastest partner's speed.
  {
    std::vector<f64> partner(bodies.size(), 0.0);
    for (const Contact& c : contacts_) {
      if (c.b < 0) continue;
      const Body& A = *bodies[size_t(c.a)];
      const Body& B = *bodies[size_t(c.b)];
      partner[size_t(c.a)] = std::max(partner[size_t(c.a)], norm(B.v_pre) + B.radius * norm(B.w_pre));
      partner[size_t(c.b)] = std::max(partner[size_t(c.b)], norm(A.v_pre) + A.radius * norm(A.w_pre));
    }
    for (size_t i = 0; i < bodies.size(); ++i) {
      Body& b = *bodies[i];
      if (b.asleep) continue;
      const f64 before = norm(b.v_pre), after = norm(b.v);
      const f64 allowed = std::max(before, 1.2 * partner[i]) + 1.0;
      if (after > allowed) b.v *= allowed / after;
      const f64 wb = norm(b.w_pre), wa = norm(b.w);
      const f64 wallowed = std::max(wb, 1.2 * partner[i] / std::max(b.radius, 0.1)) + 1.0 / std::max(b.radius, 0.1);
      if (wa > wallowed) b.w *= wallowed / wa;
    }
  }
  warm_.clear();
  for (const Contact& c : contacts_) warm_[c.key] = {c.ln, c.l1, c.l2};
  // split-impulse position correction: pseudo velocities, normal only
  std::vector<V3> pv(bodies.size()), pw(bodies.size());
  auto pvel = [&](i32 i, const V3& r) -> V3 {
    if (i < 0) return V3{};
    return pv[size_t(i)] + cross(pw[size_t(i)], r);
  };
  for (int it = 0; it < pos_iters; ++it)
    sweep([&](Contact& c) {
      if (c.bias <= 0.0) return;
      const f64 vn = dot(pvel(c.a, c.ra) - pvel(c.b, c.rb), c.n);
      const f64 lp = std::max(0.0, c.lp + c.kn * (c.bias - vn));
      const V3 J = c.n * (lp - c.lp);
      c.lp = lp;
      if (!bodies[size_t(c.a)]->asleep) {
        pv[size_t(c.a)] += J * bodies[size_t(c.a)]->inv_mass;
        pw[size_t(c.a)] += Iw[size_t(c.a)] * cross(c.ra, J);
      }
      if (c.b >= 0 && !bodies[size_t(c.b)]->asleep) {
        pv[size_t(c.b)] -= J * bodies[size_t(c.b)]->inv_mass;
        pw[size_t(c.b)] -= Iw[size_t(c.b)] * cross(c.rb, J);
      }
    });
  pseudo_v_.swap(pv);
  pseudo_w_.swap(pw);
}

void RigidWorld::integrate_positions(f64 dt) {
  for (size_t i = 0; i < bodies.size(); ++i) {
    Body& b = *bodies[i];
    if (b.asleep) continue;
    const V3 pv = i < pseudo_v_.size() ? pseudo_v_[i] : V3{};
    const V3 pw = i < pseudo_w_.size() ? pseudo_w_[i] : V3{};
    b.x += (b.v + pv) * dt;
    b.q = integrate(b.q, b.w + pw, dt);
    b.age += dt;
  }
}

void RigidWorld::sleep_update(f64 dt) {
  // Bodies sleep one by one once slow for a while (a sleeping body is a static support for the
  // others); an awake body touching a sleeping one fast enough wakes it (collide). Rates are per
  // 1/120 s (the same at any substep length).
  const f64 k = dt * 120.0;
  const f64 rest = std::pow(1.0 - par.rest_damping, k);
  const f64 keep = std::pow(0.8, k);
  const i32 steps = std::max<i32>(1, static_cast<i32>(std::lround(k)));
  std::vector<u8> touching(bodies.size(), 0);
  for (const Contact& c : contacts_) {
    touching[size_t(c.a)] = 1;
    if (c.b >= 0) touching[size_t(c.b)] = 1;
  }
  for (size_t i = 0; i < bodies.size(); ++i) {
    Body& b = *bodies[i];
    if (b.asleep) continue;
    const f64 sp = norm(b.v) + b.radius * norm(b.w);
    if (touching[i] && sp < 3.0 * par.sleep_speed) {
      // rest damping: settling rubble loses its last jitter
      b.v *= rest;
      b.w *= rest;
    }
    // (a smoothed speed: a settling piece's last jitter does not restart its count; real motion does)
    b.sleep_ema = keep * b.sleep_ema + (1.0 - keep) * sp;
    if (sp > 3.0 * par.sleep_speed || !touching[i]) b.still = 0;
    else if (b.sleep_ema < par.sleep_speed) b.still += steps;
    else b.still = std::max(0, b.still - 2 * steps);
    if (b.still >= par.sleep_substeps) {
      b.asleep = true;
      b.v = V3{};
      b.w = V3{};
    }
  }
}

bool RigidWorld::busy() const {
  i32 fast = 0;
  const f64 v2 = par.busy_speed * par.busy_speed;
  for (const auto& bp : bodies)
    if (!bp->asleep && norm2(bp->v) > v2 && ++fast > par.busy_bodies) return true;
  return false;
}

void RigidWorld::substep(f64 dt, const VoxelGrid& g, const std::function<bool(f64)>& fracture) {
  busy_ = busy();
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<f64, std::milli>(b - a).count(); };
  const auto t0 = Clock::now();
  integrate_velocities(dt);
  {
    static const bool sc = std::getenv("SVX_SERIAL_COLLIDE") != nullptr;
    std::unique_ptr<SerialScope> ss(sc ? new SerialScope() : nullptr);
    collide(g);
  }
  const auto t1 = Clock::now();
  {
    static const bool sv = std::getenv("SVX_SERIAL_SOLVE") != nullptr;
    std::unique_ptr<SerialScope> ss(sv ? new SerialScope() : nullptr);
    solve(dt);
  }
  const auto t2 = Clock::now();
  // (the contacts by body identity: a fracture may add and remove bodies)
  std::vector<std::pair<i64, i64>> cid(contacts_.size());
  for (size_t k = 0; k < contacts_.size(); ++k)
    cid[k] = {bodies[size_t(contacts_[k].a)]->id, contacts_[k].b >= 0 ? bodies[size_t(contacts_[k].b)]->id : -1};
  std::vector<i64> before_ids(bodies.size());
  for (size_t i = 0; i < bodies.size(); ++i) before_ids[i] = bodies[i]->id;
  const bool changed = fracture && fracture(dt);
  const auto t3 = Clock::now();
  if (changed) {
    for (auto& bp : bodies) {
      Body& b = *bp;
      if (b.asleep) continue;
      b.v = b.v_pre;
      b.w = b.w_pre;
    }
    // Solve again with the new pieces: the contacts among the bodies that stayed are kept, the new
    // ones collide afresh (with everything).
    auto index_of = [&](i64 id) -> i32 {
      auto it = std::lower_bound(bodies.begin(), bodies.end(), id, [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
      return (it != bodies.end() && (*it)->id == id) ? static_cast<i32>(it - bodies.begin()) : -1;
    };
    std::vector<Contact> kept;
    kept.reserve(contacts_.size());
    for (size_t k = 0; k < contacts_.size(); ++k) {
      const i32 a = index_of(cid[k].first);
      const i32 b = cid[k].second >= 0 ? index_of(cid[k].second) : -1;
      if (a < 0 || (cid[k].second >= 0 && b < 0)) continue;
      Contact c = contacts_[k];
      c.a = a;
      c.b = b;
      c.ln = c.l1 = c.l2 = c.lp = 0.0;
      kept.push_back(c);
    }
    std::vector<u8> fresh(bodies.size(), 0);
    for (size_t i = 0; i < bodies.size(); ++i) fresh[i] = std::binary_search(before_ids.begin(), before_ids.end(), bodies[i]->id) ? 0 : 1;
    contacts_.swap(kept);
    collide(g, &fresh);
    // (warm-started from the first solve of this substep: fewer iterations do)
    const int iters = par.iterations;
    par.iterations = std::max(4, iters / 2);
    solve(dt);
    par.iterations = iters;
  }
  const auto t4 = Clock::now();
  integrate_positions(dt);
  refresh_boxes();
  sleep_update(dt);
  const auto t5 = Clock::now();
  prof_ms[0] += ms(t0, t1);
  prof_ms[1] += ms(t1, t2);
  prof_ms[2] += ms(t2, t3);
  prof_ms[3] += ms(t3, t4);
  prof_ms[4] += ms(t4, t5);
  for (auto& bp : bodies)
    if (bp->family_ticks > 0) --bp->family_ticks;
}

}  // namespace svx
