#include "svx/phys/rigid.hpp"

#include <algorithm>
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
  const size_t cap = static_cast<size_t>(std::max(8, max_points));
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

void RigidWorld::collide(const VoxelGrid& g) {
  contacts_.clear();
  const f64 h = g.h, ih = 1.0 / h;
  refresh_boxes();
  const i32 nb = static_cast<i32>(bodies.size());
  std::vector<Contact> pair;
  auto reduce_push = [&](std::vector<Contact>& cs) {
    if (cs.empty()) return;
    const size_t cap = static_cast<size_t>(std::max(4, par.manifold));
    if (cs.size() > cap) {
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
    for (Contact& c : cs) contacts_.push_back(c);
    cs.clear();
  };
  // world positions of every body's samples, once per call (pairs reuse them)
  wpts_.resize(size_t(nb));
  for (i32 ib = 0; ib < nb; ++ib) {
    const Body& B = *bodies[size_t(ib)];
    const M3 R = to_matrix(B.q);
    auto& w = wpts_[size_t(ib)];
    w.resize(B.pts.size());
    for (size_t k = 0; k < B.pts.size(); ++k) w[k] = B.x + R * B.pts[k];
  }
  // world: a chunk pointer cache (samples are spatially coherent)
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
  for (i32 ia = 0; ia < nb; ++ia) {
    Body& A = *bodies[size_t(ia)];
    if (A.asleep) continue;
    const auto& W = wpts_[size_t(ia)];
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
      pair.push_back(c);
    }
    reduce_push(pair);
  }
  // body pairs: sweep over x
  std::vector<i32> order(static_cast<size_t>(nb));
  for (i32 i = 0; i < nb; ++i) order[size_t(i)] = i;
  std::sort(order.begin(), order.end(), [&](i32 x, i32 y) {
    const f64 ax = bodies[size_t(x)]->box_lo.x, ay = bodies[size_t(y)]->box_lo.x;
    return ax < ay || (ax == ay && x < y);
  });
  std::vector<std::pair<i32, i32>> pairs;
  for (size_t i = 0; i < order.size(); ++i) {
    const Body& A = *bodies[size_t(order[i])];
    for (size_t j = i + 1; j < order.size(); ++j) {
      const Body& B = *bodies[size_t(order[j])];
      if (B.box_lo.x > A.box_hi.x) break;
      if (A.asleep && B.asleep) continue;
      if (A.box_hi.y < B.box_lo.y || B.box_hi.y < A.box_lo.y || A.box_hi.z < B.box_lo.z || B.box_hi.z < A.box_lo.z) continue;
      pairs.push_back({std::min(order[i], order[j]), std::max(order[i], order[j])});
    }
  }
  std::sort(pairs.begin(), pairs.end());
  for (const auto& pr : pairs) {
    const size_t before = contacts_.size();
    for (int dir = 0; dir < 2; ++dir) {
      const i32 ia = dir == 0 ? pr.first : pr.second;
      const i32 ib = dir == 0 ? pr.second : pr.first;
      Body& A = *bodies[size_t(ia)];
      Body& B = *bodies[size_t(ib)];
      const M3 RB = to_matrix(B.q);
      const M3 RBt = transpose(RB);
      const f64 reach2 = (B.radius + h) * (B.radius + h);
      const auto& W = wpts_[size_t(ia)];
      auto solidB = [&](const IVec3& q) { return vox_solid(B.shape.get(q)); };
      for (size_t k = 0; k < A.pts.size(); ++k) {
        const V3& X = W[k];
        if (norm2(X - B.x) > reach2) continue;
        const V3 s = B.com + RBt * (X - B.x);
        const IVec3 p = voxel_of(s, ih);
        const i32 vi = B.shape.index(p);
        if (vi < 0 || !vox_solid(B.shape.vox[size_t(vi)])) continue;
        int axis = 2, sign = 1;
        f64 depth = 0.5 * h;
        V3 ns{0, 0, 0};
        if (exit_face(s, p, h, solidB, &axis, &sign, &depth)) {
          ns[axis] = sign;
        } else {
          ns = normalized(s - B.com);
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
        pair.push_back(c);
      }
      reduce_push(pair);
    }
    // an awake body moving into a sleeping one wakes it
    Body& A = *bodies[size_t(pr.first)];
    Body& B = *bodies[size_t(pr.second)];
    if (A.asleep != B.asleep && contacts_.size() > before) {
      Body& moving = A.asleep ? B : A;
      Body& sleeper = A.asleep ? A : B;
      const f64 sp = norm(moving.v) + moving.radius * norm(moving.w);
      if (sp > 2.0 * par.sleep_speed) wake(sleeper);
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
    c.bounce = vn < -par.bounce_speed ? -par.restitution * vn : 0.0;
    c.mu = par.friction;
    c.bias = std::min(par.max_correction, par.baumgarte * std::max(0.0, c.depth - par.slop) / dt);
    const auto it = warm_.find(c.key);
    if (it != warm_.end()) {
      c.ln = 0.85 * it->second[0];
      c.l1 = 0.85 * it->second[1];
      c.l2 = 0.85 * it->second[2];
      apply(c, c.n * c.ln + c.t1 * c.l1 + c.t2 * c.l2);
    }
  }
  for (int it = 0; it < par.iterations; ++it)
    for (Contact& c : contacts_) {
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
    }
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
  for (int it = 0; it < par.position_iterations; ++it)
    for (Contact& c : contacts_) {
      if (c.bias <= 0.0) continue;
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
    }
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
  (void)dt;
  // Bodies sleep one by one once slow for a while (a sleeping body is a static support for the
  // others); an awake body touching a sleeping one fast enough wakes it (collide).
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
      b.v *= 1.0 - par.rest_damping;
      b.w *= 1.0 - par.rest_damping;
    }
    b.still = sp < par.sleep_speed ? b.still + 1 : 0;
    if (b.still >= par.sleep_substeps) {
      b.asleep = true;
      b.v = V3{};
      b.w = V3{};
    }
  }
}

void RigidWorld::substep(f64 dt, const VoxelGrid& g, const std::function<bool(f64)>& fracture) {
  integrate_velocities(dt);
  collide(g);
  solve(dt);
  if (fracture && fracture(dt)) {
    for (auto& bp : bodies) {
      Body& b = *bp;
      if (b.asleep) continue;
      b.v = b.v_pre;
      b.w = b.w_pre;
    }
    collide(g);
    solve(dt);
  }
  integrate_positions(dt);
  refresh_boxes();
  sleep_update(dt);
}

}  // namespace svx
