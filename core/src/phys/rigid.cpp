#include "svx/phys/rigid.hpp"

#include "svx/base/diag.hpp"
#include "svx/base/parallel.hpp"

#include <algorithm>
#include <unordered_map>
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

// x^k for x in [0, 1], k >= 0, by basic arithmetic only: the same bits on every platform
// (std::pow's last bit is the library's, and the result is state). ln x by the atanh series of
// its mantissa, e^-y by Taylor of a small part, squared up.
f64 pow01(f64 x, f64 k) {
  if (!(x > 0.0)) return k > 0.0 ? 0.0 : 1.0;
  if (x >= 1.0 || !(k > 0.0)) return 1.0;
  f64 m = x;
  int e = 0;
  while (m < 0.5) {  // (exact)
    m *= 2.0;
    ++e;
  }
  const f64 z = (m - 1.0) / (m + 1.0), z2 = z * z;  // (|z| <= 1/3)
  f64 term = z, series = 0.0;
  for (int i = 0; i < 40; ++i) {
    series += term / static_cast<f64>(2 * i + 1);
    term *= z2;
  }
  f64 y = k * (static_cast<f64>(e) * 0.69314718055994530942 - 2.0 * series);  // -k ln x >= 0
  if (!(y < 745.0)) return 0.0;
  int sq = 0;
  while (y > 0.0625) {
    y *= 0.5;
    ++sq;
  }
  f64 r = 1.0, t = 1.0;
  for (int i = 1; i <= 14; ++i) {
    t *= -y / static_cast<f64>(i);
    r += t;
  }
  for (int i = 0; i < sq; ++i) r *= r;
  return r;
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

// The first solid voxel on the segment from s along the unit direction d, within tmax (a lattice
// of voxel size h): the distance to where it enters it, and the face it enters by (axis, and the
// side the segment comes from: the face's normal out of the voxel). The voxel s is in is not
// tested (it is not solid, or the caller has a contact for it).
template <class Solid>
bool first_solid(const V3& s, const V3& d, f64 tmax, f64 h, Solid&& solid, f64* t_hit, int* axis, int* sign, IVec3* hit) {
  IVec3 v = voxel_of(s, 1.0 / h);
  int step[3];
  f64 tnext[3], tdelta[3];
  for (int a = 0; a < 3; ++a) {
    if (d[a] > 0.0) {
      step[a] = 1;
      tnext[a] = (h * (v[a] + 0.5) - s[a]) / d[a];
      tdelta[a] = h / d[a];
    } else if (d[a] < 0.0) {
      step[a] = -1;
      tnext[a] = (h * (v[a] - 0.5) - s[a]) / d[a];
      tdelta[a] = -h / d[a];
    } else {
      step[a] = 0;
      tnext[a] = tdelta[a] = 1e300;
    }
  }
  for (int n = 0; n < 64; ++n) {
    const int a = tnext[0] < tnext[1] ? (tnext[0] < tnext[2] ? 0 : 2) : (tnext[1] < tnext[2] ? 1 : 2);
    const f64 t = tnext[a];
    if (t > tmax) return false;
    v[a] += step[a];
    tnext[a] += tdelta[a];
    if (solid(v)) {
      *t_hit = std::max(0.0, t);
      *axis = a;
      *sign = -step[a];
      *hit = v;
      return true;
    }
  }
  return false;
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

LatticeXf LatticeXf::make(const V3& off, const Quat& q) {
  LatticeXf x;
  x.q = qnormalized(q);
  if (x.q.w < 0.0) x.q = Quat{-x.q.x, -x.q.y, -x.q.z, -x.q.w};  // (one sign for one rotation)
  x.off = off;
  x.identity = off.x == 0.0 && off.y == 0.0 && off.z == 0.0 && x.q.x == 0.0 && x.q.y == 0.0 && x.q.z == 0.0 && x.q.w == 1.0;
  if (x.identity) {
    x.off = V3{};
    x.q = Quat{};
    return x;
  }
  x.R = to_matrix(x.q);
  x.Rt = transpose(x.R);
  return x;
}

LatticeXf compose(const LatticeXf& b, const LatticeXf& a) {
  if (a.identity) return b;
  if (b.identity) return a;
  return LatticeXf::make(b.to(a.off), b.q * a.q);
}

LatticeXf inverse(const LatticeXf& a) {
  if (a.identity) return a;
  return LatticeXf::make(a.Rt * (a.off * -1.0), conj(a.q));
}

bool BodyShape::junction_broken(i32 cell, int face, int sub) const {
  if (jbrk.empty()) return false;
  return std::binary_search(jbrk.begin(), jbrk.end(), shape_junction_code(cell, face, sub)) ||
         (sub != kJunctionFace && std::binary_search(jbrk.begin(), jbrk.end(), shape_junction_code(cell, face, kJunctionFace)));
}

void BodyShape::break_junction(i32 cell, int face, int sub) {
  const u64 code = shape_junction_code(cell, face, sub);
  const auto at = std::lower_bound(jbrk.begin(), jbrk.end(), code);
  if (at != jbrk.end() && *at == code) return;
  jbrk.insert(at, code);
}

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
  // corner once (a bitmap over each shape's corner lattice), in voxel scan order, shape by shape
  // (deterministic); in the body frame
  struct Cand {
    V3 pt;
    i32 vox;
    u16 shape;
    bool sharp;
  };
  std::vector<Cand> uniq;
  b.count = 0;
  for (BodyShape& S : b.shapes) {
    if (!(S.h > 0.0)) S.h = h;
    b.count += S.count;
  }
  for (size_t k = 0; k < b.shapes.size(); ++k) {
    const BodyShape& S = b.shapes[k];
    const f64 h = S.h;
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
              uniq.push_back({S.xf.to(corner), i, static_cast<u16>(k), around <= 2});
            }
        }
    }
  }
  std::vector<const Cand*> keep;
  size_t sharp = 0;
  for (const Cand& cd : uniq) sharp += cd.sharp ? 1 : 0;
  // small pieces need few samples (their corners), large ones more (flat faces as well): about
  // 12 + 1.5 count^(2/3), in integers (no cube root: the same on every platform)
  const i64 n2 = static_cast<i64>(std::max(1, b.count)) * static_cast<i64>(std::max(1, b.count));
  i64 m23 = 0;  // floor(count^(2/3)): the largest m with m^3 <= count^2
  for (i64 lo = 0, hi = 1 << 20; lo <= hi;) {  // (2^60 fits; beyond, the cap is max_points anyway)
    const i64 mid = (lo + hi) / 2;
    if (mid * mid * mid <= n2) {
      m23 = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  const size_t cap = static_cast<size_t>(std::max<i64>(8, std::min<i64>(max_points, 12 + (3 * m23) / 2)));
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
  b.pt_shape.clear();
  b.radius = 0.0;
  for (const Cand* cd : keep) {
    const V3 r = cd->pt - b.com;
    b.pts.push_back(r);
    b.pt_vox.push_back(cd->vox);
    b.pt_shape.push_back(cd->shape);
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

const Body* RigidWorld::find(i64 id) const { return const_cast<RigidWorld*>(this)->find(id); }

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
    if (norm2(b.force) > 0.0) b.v += b.force * (b.inv_mass * dt);  // (external: World::apply_force)
    if (norm2(b.torque) > 0.0) b.w += b.inv_inertia_world() * b.torque * dt;
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

void RigidWorld::collide(const std::vector<StaticGrid>& statics, const std::vector<u8>* only) {
  // only (optional): contacts of these bodies alone (the others' contacts are kept by the caller)
  if (!only) contacts_.clear();
  if (statics.empty()) return;
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
  // static contacts, per body in parallel (each with its own chunk caches), joined in body order:
  // each sample against every static grid near the body, in grid order (the world grid first)
  std::vector<std::vector<Contact>> wc(static_cast<size_t>(nb));
  parallel_for(nb, 16, [&](i64 b0, i64 b1) {
    struct Cache {
      IVec3 cc{INT32_MIN, 0, 0};
      const Chunk* ch = nullptr;
    };
    std::vector<Cache> caches(statics.size());
    std::vector<u32> near;
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
    std::vector<Contact> spec;
    for (i64 i = b0; i < b1; ++i) {
      const i32 ia = static_cast<i32>(i);
      Body& A = *bodies[size_t(ia)];
      if (A.asleep || !selected(ia)) continue;
      // (continuous collision: a body that may move more than half a voxel this substep looks
      // along its motion - its box grown by it)
      const f64 motion = (norm(A.v) + A.radius * norm(A.w)) * step_dt_;
      const bool fast = par.speculative && motion > 0.5 * statics.front().g->h;
      const f64 grow = fast ? motion : 0.0;
      near.clear();
      for (u32 s = 0; s < static_cast<u32>(statics.size()); ++s) {
        const StaticGrid& G = statics[s];
        if (G.unbounded || !(A.box_hi.x + grow < G.lo.x || A.box_lo.x - grow > G.hi.x || A.box_hi.y + grow < G.lo.y ||
                             A.box_lo.y - grow > G.hi.y || A.box_hi.z + grow < G.lo.z || A.box_lo.z - grow > G.hi.z))
          near.push_back(s);
      }
      std::vector<Contact>& out = wc[size_t(ia)];
      spec.clear();
      const auto& W = A.wpts;
      for (size_t k = 0; k < A.pts.size(); ++k) {
        const V3& X = W[k];
        const size_t before = out.size();
        for (u32 s : near) {
          const StaticGrid& G = statics[s];
          const f64 h = G.g->h;
          const V3 L = G.xf.from(X);  // (the world grid: X itself)
          const IVec3 p = voxel_of(L, 1.0 / h);
          if (!vox_solid(grid_vox(s, p))) continue;
          int axis = 2, sign = 1;
          f64 depth = 0.5 * h;
          if (!exit_face(L, p, h, [&](const IVec3& q) { return vox_solid(grid_vox(s, q)); }, &axis, &sign, &depth)) {
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
          if (!G.xf.identity) c.n = G.xf.dir_to(c.n);
          c.depth = depth;
          c.vox_a = A.pt_vox[k];
          c.shape_a = static_cast<i16>(A.pt_shape[k]);
          c.grid = G.slot;
          c.wvox = p;
          c.key = mix64(static_cast<u64>(A.id) * 0x100000001B3ull ^ mix64(~0ull) ^ (static_cast<u64>(k) << 1));
          if (G.slot != 0) c.key = mix64(c.key ^ (static_cast<u64>(G.slot) * 0x9E3779B97F4A7C15ull));
          out.push_back(c);
        }
        // (a fast sample in no solid: the nearest solid face it would reach this substep, of any grid)
        if (!fast || out.size() != before) continue;
        const V3 d = (A.v + cross(A.w, X - A.x)) * step_dt_;
        const f64 len = norm(d);
        if (!(len > 0.25 * statics.front().g->h)) continue;
        const V3 dir = d * (1.0 / len);
        Contact best;
        f64 bt = 1e300;
        for (u32 s : near) {
          const StaticGrid& G = statics[s];
          const f64 h = G.g->h;
          f64 t;
          int axis, sign;
          IVec3 p;
          if (!first_solid(G.xf.from(X), G.xf.dir_from(dir), len + 0.5 * h, h, [&](const IVec3& q) { return vox_solid(grid_vox(s, q)); }, &t, &axis,
                           &sign, &p) ||
              !(t < bt))
            continue;
          V3 n{0, 0, 0};
          n[axis] = sign;
          if (!G.xf.identity) n = G.xf.dir_to(n);
          bt = t;
          best.a = ia;
          best.b = -1;
          best.p = X;
          best.n = n;
          best.depth = -std::max(0.0, t * -dot(dir, n));  // (the gap along the normal: a speculative contact)
          best.vox_a = A.pt_vox[k];
          best.shape_a = static_cast<i16>(A.pt_shape[k]);
          best.grid = G.slot;
          best.wvox = p;
          best.key = mix64(static_cast<u64>(A.id) * 0x100000001B3ull ^ mix64(~1ull) ^ (static_cast<u64>(k) << 1)) ^ static_cast<u64>(G.slot);
        }
        if (bt < 1e300) spec.push_back(best);
      }
      reduce_manifold(out, A.shapes.front().h);
      // (the nearest few speculative contacts: where it would strike first)
      if (!spec.empty()) {
        std::sort(spec.begin(), spec.end(), [](const Contact& x, const Contact& y) { return x.depth > y.depth || (x.depth == y.depth && x.key < y.key); });
        const size_t keep = std::min(spec.size(), static_cast<size_t>(std::max(1, par.speculative_contacts)));
        out.insert(out.end(), spec.begin(), spec.begin() + static_cast<long>(keep));
      }
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
        f64 hb = 0.0;
        for (const BodyShape& SB : B.shapes) hb = std::max(hb, SB.h);
        const f64 reach2 = (B.radius + hb) * (B.radius + hb);
        const auto& W = A.wpts;
        for (size_t k = 0; k < A.pts.size(); ++k) {
          const V3& X = W[k];
          if (norm2(X - B.x) > reach2) continue;
          const V3 sb = B.com + RBt * (X - B.x);  // (B's body frame)
          for (size_t m = 0; m < B.shapes.size(); ++m) {
            const BodyShape& SB = B.shapes[m];
            const f64 h = SB.h;
            const V3 sp = SB.xf.from(sb);  // (its lattice)
            const IVec3 p = voxel_of(sp, 1.0 / h);
            const i32 vi = SB.index(p);
            if (vi < 0 || !vox_solid(SB.vox[size_t(vi)])) continue;
            int axis = 2, sign = 1;
            f64 depth = 0.5 * h;
            V3 ns{0, 0, 0};
            if (exit_face(sp, p, h, [&](const IVec3& qv) { return vox_solid(SB.get(qv)); }, &axis, &sign, &depth)) {
              ns[axis] = sign;
              ns = SB.xf.dir_to(ns);
            } else {
              ns = normalized(sb - B.com);
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
            c.shape_a = static_cast<i16>(A.pt_shape[k]);
            c.shape_b = static_cast<i16>(m);
            c.key = mix64(static_cast<u64>(A.id) * 0x100000001B3ull ^ mix64(static_cast<u64>(B.id)) ^ (static_cast<u64>(k) << 1));
            if (m != 0) c.key = mix64(c.key ^ (static_cast<u64>(m) * 0xC2B2AE3D27D4EB4Full));
            one.push_back(c);
          }
        }
        reduce_manifold(one, A.shapes.front().h);
        for (const Contact& c : one) pc[size_t(q)].push_back(c);
        one.clear();
      }
    }
  });
  const auto c3 = CClock::now();
  static const bool cprof = diag("SVX_PROFILE_COLLIDE");
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
  // (a machine at work wakes what it touches, however slowly it moves: it may push from rest)
  std::vector<u8> machine;
  if (!joints.empty()) machine = machine_parts();
  for (size_t q = 0; q < pairs.size(); ++q) {
    for (const Contact& c : pc[q]) contacts_.push_back(c);
    // an awake body moving near a sleeping one wakes it (whether it pushes it, or moves away
    // from under it)
    const i32 ia = pairs[q].first, ib = pairs[q].second;
    Body& A = *bodies[size_t(ia)];
    Body& B = *bodies[size_t(ib)];
    if (A.asleep != B.asleep) {
      Body& moving = A.asleep ? B : A;
      Body& sleeper = A.asleep ? A : B;
      const f64 sp = norm(moving.v) + moving.radius * norm(moving.w);
      const bool driven = !machine.empty() && !pc[q].empty() && machine[size_t(A.asleep ? ib : ia)];
      if (driven || sp > (!pc[q].empty() ? 2.0 : 3.0) * sleep_speed_) wake(sleeper);
    }
  }
}

void RigidWorld::solve(f64 dt) {
  if (!joints.empty()) wake_jointed();
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
    if (c.depth < 0.0) c.bounce = c.depth / dt;  // (speculative: it may close the gap this substep, no more)
    c.mu = par.friction;
    c.bias = std::min(par.max_correction, par.baumgarte * std::max(0.0, c.depth - par.slop) / dt);
  }
  // (warm starts after every contact read its approach from the velocities before the solve)
  for (Contact& c : contacts_) {
    if (c.depth < 0.0) continue;  // (a speculative contact starts from nothing: it may not act at all)
    const auto it = warm_.find(c.key);
    if (it != warm_.end()) {
      c.ln = 0.85 * it->second[0];
      c.l1 = 0.85 * it->second[1];
      c.l2 = 0.85 * it->second[2];
      apply(c, c.n * c.ln + c.t1 * c.l1 + c.t2 * c.l2);
    }
  }
  prepare_joints(dt, Iw);
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
  static const bool dbg_col = diag("SVX_DEBUG_COLOUR");
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
      // (a colour of few manifolds is cheaper on this thread than handing it to the pool)
      if (L.size() < 128) {
        for (i32 gi : L) group(gi);
      } else {
        parallel_for(static_cast<i64>(L.size()), 32, [&](i64 q0, i64 q1) {
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
  for (int it = 0; it < vel_iters; ++it) {
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
    solve_joints();
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
    joint_partner_speeds(partner);
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
  for (int it = 0; it < pos_iters; ++it) {
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
    solve_joints_position(pv, pw);
  }
  finish_joints(dt);
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

const std::vector<u8>& RigidWorld::support(f64 dt) {
  // A body is held up by a supporter (the world, a sleeping body, a held body) through a contact
  // facing up (n pushes a out of b; up is +z), or through contacts whose impulses (friction too: a
  // piece wedged between walls) carry at least half its weight this substep. Debris falling
  // together touches, and pushes, but nothing holds it.
  // (A resting contact may carry no impulse in a substep - the pile's solve left it at rest - and
  // still holds: rubble in a pile is held, whatever its contacts carry at the moment.)
  constexpr f64 kUp = 0.1;  // (a contact steeper than about 84 degrees holds nothing up)
  const size_t n = bodies.size();
  const f64 weight_dt = 0.5 * par.gravity * dt;
  held_.assign(n, 0);
  lift_.assign(n, 0.0);
  sup_queue_.clear();
  sup_pairs_.clear();
  // (to: pushed up by a supporter, or lifted by jz; held once its lifts carry half its weight)
  auto push = [&](i32 to, bool up, f64 jz) {
    if (held_[size_t(to)]) return;
    if (!up) {
      if (jz <= 0.0) return;
      lift_[size_t(to)] += jz;
      if (lift_[size_t(to)] < weight_dt * bodies[size_t(to)]->mass) return;
    }
    held_[size_t(to)] = 1;
    sup_queue_.push_back(to);
  };
  for (const Contact& c : contacts_) {
    if (c.depth < 0.0 && !(c.ln > 0.0)) continue;  // (a speculative contact that did not act: not touching)
    const f64 jz = c.n.z * c.ln + c.t1.z * c.l1 + c.t2.z * c.l2;  // (impulse().z: on a; b takes the opposite)
    const bool up_a = c.n.z > kUp, up_b = c.n.z < -kUp;
    if (c.b < 0) {
      push(c.a, up_a, jz);
      continue;
    }
    const bool sa = bodies[size_t(c.a)]->asleep, sb = bodies[size_t(c.b)]->asleep;
    if (sa || sb) {
      if (!sa) push(c.a, up_a, jz);
      if (!sb) push(c.b, up_b, -jz);
      continue;
    }
    sup_pairs_.push_back({c.a, c.b, jz, static_cast<u8>(up_a), static_cast<u8>(up_b)});
  }

  if (sup_queue_.empty() || sup_pairs_.empty()) return held_;
  // the awake pairs' edges by the supporting body (a counting sort: in contact order)
  sup_start_.assign(n + 1, 0);
  for (const SupportPair& p : sup_pairs_) {
    ++sup_start_[size_t(p.a) + 1];
    ++sup_start_[size_t(p.b) + 1];
  }
  for (size_t i = 0; i < n; ++i) sup_start_[i + 1] += sup_start_[i];
  sup_edges_.resize(2 * sup_pairs_.size());
  sup_fill_.assign(sup_start_.begin(), sup_start_.end() - 1);
  for (const SupportPair& p : sup_pairs_) {
    sup_edges_[size_t(sup_fill_[size_t(p.b)]++)] = {p.a, p.up_a, p.jz};
    sup_edges_[size_t(sup_fill_[size_t(p.a)]++)] = {p.b, p.up_b, -p.jz};
  }
  for (size_t q = 0; q < sup_queue_.size(); ++q) {
    const i32 i = sup_queue_[q];
    for (i32 e = sup_start_[size_t(i)]; e < sup_start_[size_t(i) + 1]; ++e) {
      const SupportEdge& E = sup_edges_[size_t(e)];
      push(E.to, E.up, E.jz);
    }
  }
  return held_;
}

std::vector<i32> RigidWorld::resting_on() const {
  // What each body's settling is relative to (sleep_update): kWorldRest, the world's rest (every
  // body, when no machine runs); kMachine, a machine's part (an end of a drive at work: driven,
  // not settled); else the body it rests on, where that one is a machine's part or rests on one
  // in turn (a crate on a turntable, a stack on a lift's car): it is carried, not held back.
  const size_t n = bodies.size();
  std::vector<i32> on(n, kWorldRest);
  if (joints.empty()) return on;
  const std::vector<u8> machine = machine_parts();
  bool any = false;
  for (size_t i = 0; i < n; ++i)
    if (machine[i]) {
      on[i] = kMachine;
      any = true;
    }
  if (!any) return on;
  // (the body each rests on: of the bodies under it, the one that bears on it most)
  constexpr f64 kUp = 0.1;
  std::unordered_map<u64, f64> bear;
  for (const Contact& c : contacts_) {
    if (c.b < 0) continue;
    // (n pushes a out of b: up, a rests on b)
    const i32 top = c.n.z > kUp ? c.a : c.n.z < -kUp ? c.b : -1;
    if (top < 0) continue;
    const i32 low = top == c.a ? c.b : c.a;
    bear[(static_cast<u64>(top) << 32) | static_cast<u32>(low)] += c.ln;
  }
  std::vector<i32> under(n, -1);
  std::vector<f64> most(n, 0.0);
  for (const auto& [key, ln] : bear) {
    const i32 top = static_cast<i32>(key >> 32), low = static_cast<i32>(key & 0xFFFFFFFFu);
    // (ties: the lower index, the same on every run)
    if (ln > most[size_t(top)] || (ln == most[size_t(top)] && under[size_t(top)] >= 0 && low < under[size_t(top)])) {
      most[size_t(top)] = ln;
      under[size_t(top)] = low;
    }
  }
  for (int pass = 0; pass < 8; ++pass) {
    bool more = false;
    for (size_t i = 0; i < n; ++i) {
      const i32 u = under[i];
      if (on[i] != kWorldRest || u < 0 || on[size_t(u)] == kWorldRest) continue;
      on[i] = u;
      more = true;
    }
    if (!more) break;
  }
  return on;
}

void RigidWorld::sleep_update(f64 dt) {
  // Bodies sleep one by one once slow for a while (a sleeping body is a static support for the
  // others); an awake body touching a sleeping one fast enough wakes it (collide). Rates are per
  // 1/120 s (the same at any substep length).
  const f64 k = dt * 120.0;
  // x^k: by multiplication for whole k (the usual substeps), else pow01 (the same on every platform)
  auto powk = [&](f64 x) {
    const f64 kr = std::round(k);
    if (std::abs(k - kr) > 1e-9 || kr < 1.0 || kr > 64.0) return pow01(x, k);
    f64 r = 1.0;
    for (int i = 0; i < static_cast<int>(kr); ++i) r *= x;
    return r;
  };
  const f64 rest = powk(1.0 - par.rest_damping);
  const f64 keep = powk(0.8);
  const i32 steps = std::max<i32>(1, static_cast<i32>(std::lround(k)));
  const f64 sleep_speed = sleep_speed_;
  // Settling (rest damping) and sleep are for bodies held up (support) only: debris falling
  // together touches, but nothing holds it - it falls at g.
  const std::vector<u8>& supported = support(dt);
  // (a machine's parts, and what they carry: they never sleep while it runs - asleep, a body
  // takes no part in contacts, and would be passed through)
  const std::vector<i32> on = resting_on();
  // (what hangs on a joint is held for sleep, not settled: no friction holds a pendulum still)
  std::vector<u8> hung;
  if (!joints.empty()) hung = hanging(supported);
  // (a hold lasts 0.05 s after it was lost - rubble settling in a pile loses and finds its hold
  // from one substep to the next - for all but a fall: a piece falling is not held back, and soon
  // too fast to count towards sleep)
  constexpr i32 kHold = 6;
  for (size_t i = 0; i < bodies.size(); ++i) {
    Body& b = *bodies[i];
    if (b.asleep) continue;
    b.held = supported[i] ? kHold : std::max(0, b.held - steps);
    f64 sp;
    if (on[i] == kMachine) {
      sp = norm(b.v) + b.radius * norm(b.w);  // (driven: not settled)
    } else if (on[i] == kWorldRest) {
      sp = norm(b.v) + b.radius * norm(b.w);
      if (b.held > 0 && sp < par.rest_speed && b.radius < par.rest_radius) {
        // rest damping: settling rubble loses its last jitter (its hold just lost: all but its fall)
        if (supported[i]) {
          b.v *= rest;
        } else {
          b.v.x *= rest;
          b.v.y *= rest;
        }
        b.w *= rest;
      }
    } else {
      // (carried by a machine: its jitter is what it has beyond the motion of what it rests on)
      const Body& S = *bodies[size_t(on[i])];
      const V3 vr = S.v + cross(S.w, b.x - S.x);
      sp = norm(b.v - vr) + b.radius * norm(b.w - S.w);
      if (b.held > 0 && sp < par.rest_speed && b.radius < par.rest_radius) {
        if (supported[i]) {
          b.v = vr + (b.v - vr) * rest;
        } else {
          b.v.x = vr.x + (b.v.x - vr.x) * rest;
          b.v.y = vr.y + (b.v.y - vr.y) * rest;
        }
        b.w = S.w + (b.w - S.w) * rest;
      }
    }
    // (a smoothed speed: a settling piece's last jitter does not restart its count; real motion does)
    b.sleep_ema = keep * b.sleep_ema + (1.0 - keep) * sp;
    // (only a held body counts towards sleep; a resting one that loses its hold for a substep -
    // a jitter - keeps most of its count, one that falls is soon too fast to keep any)
    if (sp > 3.0 * sleep_speed || on[i] != kWorldRest) b.still = 0;
    else if ((b.held > 0 || (!hung.empty() && hung[i])) && b.sleep_ema < sleep_speed) b.still += steps;
    else b.still = std::max(0, b.still - 2 * steps);
  }
  // (bodies joined sleep together; one joined to a moving frame never sleeps)
  if (!joints.empty()) joint_stillness();
  for (size_t i = 0; i < bodies.size(); ++i) {
    Body& b = *bodies[i];
    if (b.asleep) continue;
    if (b.still >= par.sleep_substeps) {
      b.asleep = true;
      b.v = V3{};
      b.w = V3{};
      b.v_pre = V3{};  // (woken inside a step, it is rolled back to rest, not to an old jitter)
      b.w_pre = V3{};
    }
  }
}

void RigidWorld::set_step(f64 dt) {
  // (a resting piece's leftover speed after the contact solve scales with what gravity adds in a
  // substep: the sleep and wake thresholds do too)
  sleep_speed_ = std::max(par.sleep_speed, 1.2 * par.gravity * dt);
  step_dt_ = dt;
}

bool RigidWorld::busy() const {
  i32 fast = 0;
  const f64 v2 = par.busy_speed * par.busy_speed;
  for (const auto& bp : bodies)
    if (!bp->asleep && norm2(bp->v) > v2 && ++fast > par.busy_bodies) return true;
  return false;
}

void RigidWorld::substep(f64 dt, const VoxelGrid& g, const std::function<int(f64)>& fracture) {
  StaticGrid w;
  w.g = &g;
  w.unbounded = true;
  substep(dt, std::vector<StaticGrid>{w}, fracture);
}

void RigidWorld::substep(f64 dt, const std::vector<StaticGrid>& statics, const std::function<int(f64)>& fracture) {
  busy_ = busy();
  set_step(dt);
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<f64, std::milli>(b - a).count(); };
  const auto t0 = Clock::now();
  integrate_velocities(dt);
  {
    static const bool sc = diag("SVX_SERIAL_COLLIDE");
    std::unique_ptr<SerialScope> ss(sc ? new SerialScope() : nullptr);
    collide(statics);
  }
  const auto t1 = Clock::now();
  {
    static const bool sv = diag("SVX_SERIAL_SOLVE");
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
  const int changed = fracture ? fracture(dt) : 0;
  const auto t3 = Clock::now();
  if (changed == 1) {
    // (bodies changed without a re-solve: the step's contacts and position corrections carry over
    // to the new body list; the new pieces collide from the next substep)
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
      kept.push_back(c);
    }
    contacts_.swap(kept);
    std::vector<V3> pv(bodies.size()), pw(bodies.size());
    for (size_t i = 0; i < bodies.size(); ++i) {
      const auto it = std::lower_bound(before_ids.begin(), before_ids.end(), bodies[i]->id);
      if (it != before_ids.end() && *it == bodies[i]->id) {
        const size_t old = static_cast<size_t>(it - before_ids.begin());
        if (old < pseudo_v_.size()) {
          pv[i] = pseudo_v_[old];
          pw[i] = pseudo_w_[old];
        }
      }
    }
    pseudo_v_.swap(pv);
    pseudo_w_.swap(pw);
  }
  if (changed == 2) {
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
    collide(statics, &fresh);
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
