// structvox — links (phys/rigid.hpp LinkData; docs/MOTION.md §6): rigid bodies of no voxels that
// collide as spheres - the parts of articulations, a character's limbs - and the fine stepping of
// the articulations that touch no awake piece.
//
// A link's sphere touches a voxel where the voxel's cube comes within the sphere's radius (plus a
// margin: a speculative contact, acting only if the gap closes); per sphere and per thing it
// touches, the deepest touch and those of other normals (a floor and a wall at a corner) are its
// contacts. They load what they press on like any piece's contacts: a crowd on a slab loads it.
//
// Fine stepping. An articulation whose links touch no awake piece is stepped on its own, in
// RigidParams::link_substeps steps of each substep, its contacts found once (with a margin the
// substep's motion needs) and followed as planes (a surface) or spheres (another link) through the
// steps. Many short steps keep a chain of light and heavy links stiff (a hand on a forearm on a
// chest); the pieces keep their substep. One that touches an awake piece is solved with it, in
// their substep - a tick in which one is can be stepped finer, everything in it
// (RigidParams::mixed_substeps: the World's choice, links_mixed), the fine links then in as many
// fewer steps (link_steps).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <climits>
#include <cmath>

#include "phys_internal.hpp"
#include "svx/base/diag.hpp"
#include "svx/base/dmath.hpp"
#include "svx/base/parallel.hpp"
#include "svx/phys/rigid.hpp"

namespace svx {

using namespace phys_detail;

namespace {

// (an articulation's index in the rules, ascending id; -1: none)
i32 art_index(const std::vector<ArticulationRules>& arts, u32 id) {
  const auto it = std::lower_bound(arts.begin(), arts.end(), id, [](const ArticulationRules& r, u32 v) { return r.id < v; });
  return it != arts.end() && it->id == id ? static_cast<i32>(it - arts.begin()) : -1;
}

// A sphere's touch of a voxel: the normal out of the voxel towards the sphere's centre, the depth
// (radius - distance; < 0: a gap within the margin), the voxel's surface point nearest the centre.
struct Touch {
  V3 n, q;
  f64 depth = 0.0;
  IVec3 v{0, 0, 0};
  u32 where = 0;  // (the static grid's index, or the piece's shape)
};

// Sphere (centre L, radius r: a lattice's metres) against its voxel p (size h): false beyond reach.
template <class Solid>
bool touch_voxel(const V3& L, f64 r, f64 reach, const IVec3& p, f64 h, Solid&& solid, Touch* t) {
  const V3 lo{h * (p[0] - 0.5), h * (p[1] - 0.5), h * (p[2] - 0.5)};
  V3 q{std::clamp(L.x, lo.x, lo.x + h), std::clamp(L.y, lo.y, lo.y + h), std::clamp(L.z, lo.z, lo.z + h)};
  // (past a face whose neighbour is solid the surface goes on: a flat floor's voxels have no edges
  // between them, a wall's no corners along it - only its outer ones touch)
  bool outside = false;
  for (int a = 0; a < 3; ++a) {
    if (q[a] == L[a]) continue;
    IVec3 nb = p;
    nb[a] += L[a] > q[a] ? 1 : -1;
    if (solid(nb)) q[a] = L[a];
    else outside = true;
  }
  if (!outside && !(L.x >= lo.x && L.x <= lo.x + h && L.y >= lo.y && L.y <= lo.y + h && L.z >= lo.z && L.z <= lo.z + h)) return false;  // (the neighbours' to touch)
  const V3 d = L - q;
  const f64 dist = norm(d);
  if (dist > 1e-9) {
    if (dist >= reach) return false;
    t->n = d * (1.0 / dist);
    t->depth = r - dist;
    t->q = q;
  } else {
    // (the centre inside the voxel: out through the nearest face whose neighbour is open; buried,
    // up through the top - the way it came in)
    f64 best = 1e300;
    int ba = 2, bs = 1;
    for (int a = 0; a < 3; ++a)
      for (int sg = -1; sg <= 1; sg += 2) {
        IVec3 nb = p;
        nb[a] += sg;
        const f64 face = sg > 0 ? lo[a] + h - L[a] : L[a] - lo[a];
        const f64 cost = face + (solid(nb) ? 4.0 * h : 0.0);
        if (cost < best) {
          best = cost;
          ba = a;
          bs = sg;
        }
      }
    f64 out = best;
    if (best >= 4.0 * h) {
      ba = 2;
      bs = 1;
      for (int up = 1; up <= 12; ++up) {
        IVec3 nb = p;
        nb[2] += up;
        if (solid(nb)) continue;
        out = h * (p[2] + up - 0.5) - L.z;
        break;
      }
    }
    V3 n{0, 0, 0};
    n[ba] = bs;
    t->n = n;
    t->depth = r + out;
    t->q = L + n * out;
  }
  t->v = p;
  return true;
}

// The touches of one sphere kept: the deepest, and those whose normals differ from every one kept
// (a corner), at most three; in a fixed order (the same on every run).
void keep_touches(std::vector<Touch>& ts, std::vector<const Touch*>& kept) {
  kept.clear();
  if (ts.empty()) return;
  std::sort(ts.begin(), ts.end(), [](const Touch& a, const Touch& b) {
    if (a.depth != b.depth) return a.depth > b.depth;
    if (a.where != b.where) return a.where < b.where;
    for (int i = 0; i < 3; ++i)
      if (a.v[i] != b.v[i]) return a.v[i] < b.v[i];
    return false;
  });
  for (const Touch& t : ts) {
    bool other = true;
    for (const Touch* k : kept)
      if (dot(k->n, t.n) > 0.8) {
        other = false;
        break;
      }
    if (!other) continue;
    kept.push_back(&t);
    if (kept.size() >= 3) break;
  }
}

void tangents(const V3& n, V3& t1, V3& t2) {
  if (std::abs(n.x) < 0.57) {
    t1 = normalized(cross(n, V3{1, 0, 0}));
  } else {
    t1 = normalized(cross(n, V3{0, 1, 0}));
  }
  t2 = cross(n, t1);
}

bool boxes_meet(const V3& alo, const V3& ahi, const V3& blo, const V3& bhi) {
  return !(ahi.x < blo.x || bhi.x < alo.x || ahi.y < blo.y || bhi.y < alo.y || ahi.z < blo.z || bhi.z < alo.z);
}

constexpr u64 kP1 = 0x100000001B3ull, kP2 = 0x9E3779B97F4A7C15ull, kP3 = 0xC2B2AE3D27D4EB4Full;

}  // namespace

// ---------------------------------------------------------------------------------------------
// Rules

const ArticulationRules* RigidWorld::rules_of(u32 id) const {
  const auto it = std::lower_bound(articulations.begin(), articulations.end(), id, [](const ArticulationRules& r, u32 v) { return r.id < v; });
  return it != articulations.end() && it->id == id ? &*it : nullptr;
}

bool RigidWorld::may_collide(const Body& A, const Body& B) const {
  const LinkData* la = A.link.get();
  const LinkData* lb = B.link.get();
  if ((la && (la->gone || la->ghost)) || (lb && (lb->gone || lb->ghost))) return false;
  if (!la || !lb) return true;
  if (la->kinematic && lb->kinematic) return false;
  if (la->articulation == 0 || la->articulation != lb->articulation) return true;
  const ArticulationRules* r = rules_of(la->articulation);
  if (!r || !r->self_collide) return false;
  const u32 a = std::min(la->index, lb->index), b = std::max(la->index, lb->index);
  return std::binary_search(r->pairs.begin(), r->pairs.end(), (a << 16) | b);
}

f64 RigidWorld::link_margin(const Body& b, f64 dt) const {
  return std::min(par.link_margin + (norm(b.v) + b.radius * norm(b.w)) * dt, 0.6);
}

// ---------------------------------------------------------------------------------------------
// Contacts

void RigidWorld::link_grid_contacts(const Body& A, i32 ia, const std::vector<StaticGrid>& statics, f64 margin, std::vector<Contact>& out,
                                    std::vector<GridCache>& caches) const {
  const LinkData& L = *A.link;
  if (L.gone) return;
  caches.resize(statics.size());
  const MaterialTable& mt = mats ? *mats : default_materials();
  const bool passable = mt.any_passable();
  auto grid_vox = [&](u32 s, const IVec3& p) -> Vox {
    GridCache& c = caches[s];
    const IVec3 cc = chunk_of(p);
    if (cc != c.cc) {
      c.cc = cc;
      c.ch = statics[s].g->chunk(cc);
    }
    if (!c.ch) return kAir;
    const Vox v = c.ch->uniform ? c.ch->value : c.ch->v[size_t(chunk_index(p))];
    return passable && (mt.vox_kind(v) & kVoxPassable) ? kAir : v;  // (leaves, grass: walked through)
  };
  const M3 R = to_matrix(A.q);
  std::vector<Touch> ts;
  std::vector<const Touch*> kept;
  for (size_t k = 0; k < L.spheres.size(); ++k) {
    const V3 c = A.x + R * L.spheres[k].c;
    const f64 r = L.spheres[k].r, reach = r + margin;
    ts.clear();
    for (u32 s = 0; s < static_cast<u32>(statics.size()); ++s) {
      const StaticGrid& G = statics[s];
      if (!G.unbounded && !boxes_meet(c - V3{reach, reach, reach}, c + V3{reach, reach, reach}, G.lo, G.hi)) continue;
      const f64 h = G.g->h, ih = 1.0 / h;
      const V3 Lc = G.xf.from(c);
      const IVec3 lo = voxel_of(Lc - V3{reach, reach, reach}, ih), hi = voxel_of(Lc + V3{reach, reach, reach}, ih);
      auto solid = [&](const IVec3& q) { return vox_solid(grid_vox(s, q)); };
      for (i32 x = lo[0]; x <= hi[0]; ++x)
        for (i32 y = lo[1]; y <= hi[1]; ++y)
          for (i32 z = lo[2]; z <= hi[2]; ++z) {
            const IVec3 p{x, y, z};
            if (!solid(p)) continue;
            Touch t;
            if (!touch_voxel(Lc, r, reach, p, h, solid, &t)) continue;
            t.n = G.xf.dir_to(t.n);
            t.q = G.xf.to(t.q);
            t.where = s;
            ts.push_back(t);
          }
    }
    keep_touches(ts, kept);
    for (size_t i = 0; i < kept.size(); ++i) {
      const Touch& t = *kept[i];
      Contact ct;
      ct.a = ia;
      ct.b = -1;
      ct.n = t.n;
      ct.depth = t.depth;
      ct.p = c - t.n * r;
      ct.vox_a = -1;
      ct.shape_a = static_cast<i16>(k);
      ct.grid = statics[t.where].slot;
      ct.wvox = t.v;
      ct.mu = L.friction;
      ct.key = mix64(static_cast<u64>(A.id) * kP1 ^ mix64(0xA11CE000ull + 16 * k + i) ^ (static_cast<u64>(ct.grid) * kP2));
      out.push_back(ct);
    }
  }
}

void RigidWorld::link_piece_contacts(const Body& A, i32 ia, const Body& B, i32 ib, f64 margin, std::vector<Contact>& out) const {
  const LinkData& L = *A.link;
  if (L.gone || B.shapes.empty()) return;
  const M3 RA = to_matrix(A.q), RB = to_matrix(B.q), RBt = transpose(RB);
  f64 hb = 0.0;
  for (const BodyShape& S : B.shapes) hb = std::max(hb, S.h);
  std::vector<Touch> ts;
  std::vector<const Touch*> kept;
  for (size_t k = 0; k < L.spheres.size(); ++k) {
    const V3 c = A.x + RA * L.spheres[k].c;
    const f64 r = L.spheres[k].r, reach = r + margin;
    const f64 far = B.radius + hb + reach;
    if (norm2(c - B.x) > far * far) continue;
    const V3 sb = B.com + RBt * (c - B.x);  // (B's body frame)
    ts.clear();
    for (size_t m = 0; m < B.shapes.size(); ++m) {
      const BodyShape& S = B.shapes[m];
      const f64 h = S.h, ih = 1.0 / h;
      const V3 Lc = S.xf.from(sb);
      const IVec3 lo = voxel_of(Lc - V3{reach, reach, reach}, ih), hi = voxel_of(Lc + V3{reach, reach, reach}, ih);
      auto solid = [&](const IVec3& q) { return vox_solid(S.get(q)); };
      for (i32 x = std::max(lo[0], S.lo[0]); x <= std::min(hi[0], S.lo[0] + S.dim[0] - 1); ++x)
        for (i32 y = std::max(lo[1], S.lo[1]); y <= std::min(hi[1], S.lo[1] + S.dim[1] - 1); ++y)
          for (i32 z = std::max(lo[2], S.lo[2]); z <= std::min(hi[2], S.lo[2] + S.dim[2] - 1); ++z) {
            const IVec3 p{x, y, z};
            if (!solid(p)) continue;
            Touch t;
            if (!touch_voxel(Lc, r, reach, p, h, solid, &t)) continue;
            t.n = RB * S.xf.dir_to(t.n);
            t.q = B.x + RB * (S.xf.to(t.q) - B.com);
            t.where = static_cast<u32>(m);
            ts.push_back(t);
          }
    }
    keep_touches(ts, kept);
    for (size_t i = 0; i < kept.size(); ++i) {
      const Touch& t = *kept[i];
      Contact ct;
      ct.a = ia;
      ct.b = ib;
      ct.n = t.n;
      ct.depth = t.depth;
      ct.p = c - t.n * r;
      ct.vox_a = -1;
      ct.vox_b = B.shapes[t.where].index(t.v);
      ct.shape_a = static_cast<i16>(k);
      ct.shape_b = static_cast<i16>(t.where);
      ct.mu = L.friction;
      ct.key = mix64(static_cast<u64>(A.id) * kP1 ^ mix64(static_cast<u64>(B.id)) ^ ((16 * k + i) * kP3) ^ (static_cast<u64>(t.where) << 40));
      out.push_back(ct);
    }
  }
}

void RigidWorld::link_link_contacts(const Body& A, i32 ia, const Body& B, i32 ib, f64 margin, std::vector<Contact>& out) const {
  const LinkData& La = *A.link;
  const LinkData& Lb = *B.link;
  if (La.gone || Lb.gone) return;
  const M3 RA = to_matrix(A.q), RB = to_matrix(B.q);
  // (an articulation's own links slide over each other: a leg brushing the other is kept out of
  // it, not held by it)
  const bool own = La.articulation != 0 && La.articulation == Lb.articulation;
  const f64 mu = own ? 0.0 : 0.5 * (La.friction + Lb.friction);
  for (size_t ka = 0; ka < La.spheres.size(); ++ka) {
    const V3 ca = A.x + RA * La.spheres[ka].c;
    const f64 ra = La.spheres[ka].r;
    for (size_t kb = 0; kb < Lb.spheres.size(); ++kb) {
      const V3 cb = B.x + RB * Lb.spheres[kb].c;
      const f64 rb = Lb.spheres[kb].r;
      const V3 d = ca - cb;
      const f64 dist = norm(d);
      if (!(dist < ra + rb + margin)) continue;
      Contact ct;
      ct.a = ia;
      ct.b = ib;
      ct.n = dist > 1e-9 ? d * (1.0 / dist) : V3{0, 0, 1};
      ct.depth = ra + rb - dist;
      ct.p = ca - ct.n * ra;
      ct.vox_a = -1;
      ct.shape_a = static_cast<i16>(ka);
      ct.shape_b = static_cast<i16>(kb);
      ct.mu = mu;
      ct.key = mix64(static_cast<u64>(A.id) * kP1 ^ mix64(static_cast<u64>(B.id) ^ 0x5BD1E995ull) ^ ((256 * ka + kb) * kP2));
      out.push_back(ct);
    }
  }
}

size_t RigidWorld::piece_contacts() const {
  if (articulations.empty()) return contacts_.size();  // (no links)
  // (the last substep's: bodies removed since leave some that point past the end)
  const size_t nb = bodies.size();
  size_t n = 0;
  for (const Contact& c : contacts_) {
    if (c.a < 0 || size_t(c.a) >= nb || (c.b >= 0 && size_t(c.b) >= nb)) continue;
    if (!bodies[size_t(c.a)]->link && (c.b < 0 || !bodies[size_t(c.b)]->link)) ++n;
  }
  return n;
}

// ---------------------------------------------------------------------------------------------
// Senses

void RigidWorld::begin_tick() {
  sleep_clock_ = 0.0;
  for (auto& bp : bodies) {
    if (!bp->link) continue;
    LinkData& L = *bp->link;
    L.contact = false;
    L.impact = 0.0;
    L.bumped = 0.0;
    L.load = -1.0;
  }
}

void RigidWorld::sense_links() {
  for (const Contact& c : contacts_) {
    for (int side = 0; side < 2; ++side) {
      const i32 i = side ? c.b : c.a;
      if (i < 0) continue;
      Body& B = *bodies[size_t(i)];
      if (!B.link) continue;
      if (!(c.depth >= 0.0) && !(c.ln > 0.0)) continue;  // (a gap that did not close: not touching)
      LinkData& L = *B.link;
      // (what it meets - the world, other bodies - not its own links: a foot brushing the other
      // leg has not touched anything)
      const i32 o = side ? c.a : c.b;
      const Body* O = o >= 0 ? bodies[size_t(o)].get() : nullptr;
      if (O && O->link && L.articulation != 0 && O->link->articulation == L.articulation) continue;
      const V3 n = side ? c.n * -1.0 : c.n;
      L.contact = true;
      if (c.ln >= L.load) {
        L.load = c.ln;
        L.contact_normal = n;
        L.contact_point = c.p;
      }
      L.impact = std::max(L.impact, c.ln);
      // (pushed by another body - not resting on it: the sideways part)
      if (!O) continue;
      L.bumped += c.ln * std::sqrt(n.x * n.x + n.y * n.y);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Fine stepping

std::vector<u8> RigidWorld::articulation_steps(f64 dt) const {
  const size_t nb = bodies.size(), na = articulations.size();
  struct Box {
    V3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    bool any = false, fine = true;
  };
  std::vector<Box> box(na);
  for (size_t i = 0; i < nb; ++i) {
    const Body& b = *bodies[i];
    if (!b.link || b.asleep || b.link->articulation == 0) continue;
    const i32 a = art_index(articulations, b.link->articulation);
    if (a < 0) continue;
    const f64 m = (norm(b.v) + b.radius * norm(b.w)) * dt + par.link_margin;
    Box& B = box[size_t(a)];
    for (int k = 0; k < 3; ++k) {
      B.lo[k] = std::min(B.lo[k], b.box_lo[k] - m);
      B.hi[k] = std::max(B.hi[k], b.box_hi[k] + m);
    }
    B.any = true;
  }
  // (held by a joint to a piece: solved with it; held by one to another articulation's link: as
  // that one is stepped - below)
  std::vector<std::pair<i32, i32>> tied;
  for (const Joint& j : joints) {
    if (j.broken || j.a.body == 0 || j.b.body == 0) continue;
    const Body* A = find(j.a.body);
    const Body* B = find(j.b.body);
    if (!A || !B) continue;
    if (A->link && B->link) {
      const i32 a = art_index(articulations, A->link->articulation), b = art_index(articulations, B->link->articulation);
      if (a >= 0 && b >= 0 && a != b) tied.push_back({a, b});
      continue;
    }
    if (!A->link && !B->link) continue;
    const Body* L = A->link ? A : B;
    const i32 a = art_index(articulations, L->link->articulation);
    if (a >= 0) box[size_t(a)].fine = false;
  }
  // (near an awake piece - its box grown by its motion this substep: solved with it)
  for (size_t i = 0; i < nb; ++i) {
    const Body& p = *bodies[i];
    if (p.link || p.asleep) continue;
    const f64 m = (norm(p.v) + p.radius * norm(p.w)) * dt;
    const V3 lo = p.box_lo - V3{m, m, m}, hi = p.box_hi + V3{m, m, m};
    for (Box& B : box)
      if (B.any && B.fine && boxes_meet(B.lo, B.hi, lo, hi)) B.fine = false;
  }
  // (near an articulation solved with the pieces, or tied to one by a joint: with them too)
  for (bool more = true; more;) {
    more = false;
    for (const auto& [a, b] : tied)
      if (box[size_t(a)].any && box[size_t(b)].any && box[size_t(a)].fine != box[size_t(b)].fine) {
        box[size_t(a)].fine = box[size_t(b)].fine = false;
        more = true;
      }
    for (size_t a = 0; a < na; ++a) {
      if (!box[a].any || !box[a].fine) continue;
      for (size_t b = 0; b < na; ++b) {
        if (b == a || !box[b].any || box[b].fine || !boxes_meet(box[a].lo, box[a].hi, box[b].lo, box[b].hi)) continue;
        box[a].fine = false;
        more = true;
        break;
      }
    }
  }
  std::vector<u8> out(na, 0);
  for (size_t a = 0; a < na; ++a) out[a] = !box[a].any ? 0 : box[a].fine ? 1 : 2;
  return out;
}

void RigidWorld::mark_fine(f64 dt) {
  any_fine_ = false;
  if (par.link_substeps <= 1 || articulations.empty()) {
    fine_.clear();
    return;
  }
  const std::vector<u8> steps = articulation_steps(dt);
  const size_t nb = bodies.size();
  fine_.assign(nb, 0);
  for (size_t i = 0; i < nb; ++i) {
    const Body& b = *bodies[i];
    if (!b.link || b.asleep || b.link->articulation == 0) continue;
    const i32 a = art_index(articulations, b.link->articulation);
    if (a < 0 || steps[size_t(a)] != 1) continue;
    fine_[i] = 1;
    any_fine_ = true;
  }
}

bool RigidWorld::links_mixed(f64 dt) const {
  if (par.link_substeps <= 1 || articulations.empty()) return false;
  const std::vector<u8> steps = articulation_steps(dt);
  return std::find(steps.begin(), steps.end(), u8{2}) != steps.end();
}

bool RigidWorld::collide_fine(f64 dt, const std::vector<StaticGrid>& statics, bool may_wake) {
  fine_cs_.clear();
  const size_t nb = bodies.size();
  std::vector<i32> fb;
  V3 ulo{1e300, 1e300, 1e300}, uhi{-1e300, -1e300, -1e300};
  for (size_t i = 0; i < nb; ++i) {
    if (!fine_[i]) continue;
    fb.push_back(static_cast<i32>(i));
    const Body& b = *bodies[i];
    const f64 m = link_margin(b, dt);
    for (int k = 0; k < 3; ++k) {
      ulo[k] = std::min(ulo[k], b.box_lo[k] - m);
      uhi[k] = std::max(uhi[k], b.box_hi[k] + m);
    }
  }
  if (fb.empty()) return false;
  // the contacts, found once for the substep: with the grids (each link in parallel, joined in
  // order), with each other and with the sleeping bodies near (static to them)
  std::vector<Contact>& cs = fine_cs_;
  {
    std::vector<std::vector<Contact>> per(fb.size());
    parallel_for(static_cast<i64>(fb.size()), 8, [&](i64 k0, i64 k1) {
      std::vector<GridCache> caches;
      for (i64 k = k0; k < k1; ++k) {
        const Body& A = *bodies[size_t(fb[size_t(k)])];
        link_grid_contacts(A, fb[size_t(k)], statics, link_margin(A, dt), per[size_t(k)], caches);
      }
    });
    for (const auto& v : per) cs.insert(cs.end(), v.begin(), v.end());
  }
  {
    std::vector<i32> cand;
    for (size_t i = 0; i < nb; ++i) {
      const Body& b = *bodies[i];
      if ((!fine_[i] && !b.asleep) || !boxes_meet(b.box_lo, b.box_hi, ulo, uhi)) continue;
      cand.push_back(static_cast<i32>(i));
    }
    std::sort(cand.begin(), cand.end(), [&](i32 x, i32 y) {
      const f64 ax = bodies[size_t(x)]->box_lo.x, ay = bodies[size_t(y)]->box_lo.x;
      return ax < ay || (ax == ay && x < y);
    });
    for (size_t x = 0; x < cand.size(); ++x) {
      const Body& P = *bodies[size_t(cand[x])];
      for (size_t y = x + 1; y < cand.size(); ++y) {
        const Body& Q = *bodies[size_t(cand[y])];
        if (Q.box_lo.x > P.box_hi.x) break;
        if (!fine_[size_t(cand[x])] && !fine_[size_t(cand[y])]) continue;  // (two sleepers)
        if (!boxes_meet(P.box_lo, P.box_hi, Q.box_lo, Q.box_hi) || !may_collide(P, Q)) continue;
        // (a is one stepped finely - a link, the one that moves; b the other, fine too or asleep and
        // static to it: a sleeping link, a body at rest, is never a - whichever comes first)
        const bool pf = fine_[size_t(cand[x])] != 0;
        const i32 ia = pf ? cand[x] : cand[y], ib = pf ? cand[y] : cand[x];
        const Body& A = *bodies[size_t(ia)];
        const Body& B = *bodies[size_t(ib)];
        const f64 m = std::min(0.6, link_margin(A, dt) + (norm(B.v) + B.radius * norm(B.w)) * dt);
        if (B.link) link_link_contacts(A, ia, B, ib, m, cs);
        else link_piece_contacts(A, ia, B, ib, m, cs);
      }
    }
  }
  if (!may_wake) return false;
  // A sleeper struck hard (a kick, a body falling on it, one thrown into it) wakes before anything
  // is solved - its part in the blow is its own - as a body moving near a sleeping one does: faster
  // than jostling, with a momentum its weight notices. (A foot set down on a slab is not.)
  bool woke = false;
  const f64 g = par.gravity;
  for (const Contact& c : cs) {
    if (c.b < 0) continue;
    Body& B = *bodies[size_t(c.b)];
    if (!B.asleep) continue;
    const Body& A = *bodies[size_t(c.a)];
    const f64 approach = -dot(A.v + cross(A.w, c.p - A.x), c.n);
    if (approach > std::max(2.0 * sleep_speed_, 0.5) && A.mass * approach > 0.25 * B.mass * g * dt) {
      wake(B);
      woke = true;
    }
  }
  if (woke && !joints.empty()) wake_jointed();
  return woke;
}

void RigidWorld::fine_islands(const std::vector<i32>& fb) {
  const size_t nb = bodies.size();
  auto index_of = [&](i64 id) -> i32 {
    const auto it = std::lower_bound(bodies.begin(), bodies.end(), id, [](const std::unique_ptr<Body>& b, i64 v) { return b->id < v; });
    return (it != bodies.end() && (*it)->id == id) ? static_cast<i32>(it - bodies.begin()) : -1;
  };
  // (fine links joined to each other, or touching each other, are one island)
  std::vector<i32> up(nb, -1);
  for (i32 i : fb) up[size_t(i)] = i;
  auto root = [&](i32 x) {
    while (up[size_t(x)] != x) {
      up[size_t(x)] = up[size_t(up[size_t(x)])];
      x = up[size_t(x)];
    }
    return x;
  };
  auto unite = [&](i32 a, i32 b) {
    a = root(a);
    b = root(b);
    if (a == b) return;
    if (a < b) up[size_t(b)] = a;
    else up[size_t(a)] = b;
  };
  std::vector<std::pair<i32, i32>> ends(joints.size(), {-1, -1});
  for (size_t k = 0; k < joints.size(); ++k) {
    const Joint& j = joints[k];
    if (j.broken) continue;
    const i32 ia = j.a.body != 0 ? index_of(j.a.body) : -1, ib = j.b.body != 0 ? index_of(j.b.body) : -1;
    ends[k] = {ia >= 0 && fine_[size_t(ia)] ? ia : -1, ib >= 0 && fine_[size_t(ib)] ? ib : -1};
    if (ends[k].first >= 0 && ends[k].second >= 0) unite(ends[k].first, ends[k].second);
  }
  for (const Contact& c : fine_cs_)
    if (c.b >= 0 && fine_[size_t(c.b)]) unite(c.a, c.b);
  // (in the order of their first body)
  std::vector<i32> slot(nb, -1);
  fine_islands_.clear();
  for (i32 i : fb) {
    const i32 r = root(i);
    if (slot[size_t(r)] < 0) {
      slot[size_t(r)] = static_cast<i32>(fine_islands_.size());
      fine_islands_.emplace_back();
    }
    fine_islands_[size_t(slot[size_t(r)])].bodies.push_back(i);
  }
  for (size_t k = 0; k < joints.size(); ++k) {
    const i32 e = ends[k].first >= 0 ? ends[k].first : ends[k].second;
    if (e >= 0) fine_islands_[size_t(slot[size_t(root(e))])].joints.push_back(static_cast<u32>(k));
  }
  for (size_t k = 0; k < targets.size(); ++k) {
    const i32 ib = index_of(targets[k].body);
    if (ib >= 0 && fine_[size_t(ib)]) fine_islands_[size_t(slot[size_t(root(ib))])].targets.push_back(static_cast<u32>(k));
  }
  for (size_t k = 0; k < fine_cs_.size(); ++k) fine_islands_[size_t(slot[size_t(root(fine_cs_[k].a))])].contacts.push_back(static_cast<u32>(k));
}

void RigidWorld::step_fine(f64 dt, std::vector<Contact>& report) {
  const int n = std::max(1, link_steps > 0 ? link_steps : par.link_substeps);
  const f64 h = dt / n;
  const size_t nb = bodies.size();
  std::vector<i32> fb;
  for (size_t i = 0; i < nb; ++i)
    if (fine_[i]) fb.push_back(static_cast<i32>(i));
  if (fb.empty()) return;
  std::vector<Contact>& cs = fine_cs_;
  // each contact followed through the steps: a surface as its plane, another link by its spheres
  const size_t nc = cs.size();
  std::vector<f64> plane(nc);
  std::vector<u8> spheres(nc);
  for (size_t k = 0; k < nc; ++k) {
    plane[k] = dot(cs[k].n, cs[k].p) + cs[k].depth;
    spheres[k] = cs[k].b >= 0 && bodies[size_t(cs[k].b)]->link ? 1 : 0;
  }
  std::vector<std::array<f64, 3>> lam(nc, {0.0, 0.0, 0.0}), tot(nc, {0.0, 0.0, 0.0});
  for (size_t k = 0; k < nc; ++k) {
    const auto it = fine_warm_.find(cs[k].key);
    if (it != fine_warm_.end()) lam[k] = it->second;
  }
  auto invm = [&](i32 i) -> f64 { return (i < 0 || bodies[size_t(i)]->asleep) ? 0.0 : bodies[size_t(i)]->inv_mass; };
  std::vector<M3> Iw(nb);
  std::vector<V3> pv(nb), pw(nb);
  // (what a link keeps of its velocity per step, and of its spin about its length: the same every step)
  std::vector<std::array<f64, 3>> keep(nb);
  for (i32 i : fb) keep[size_t(i)] = link_keep(*bodies[size_t(i)], h);
  auto vel = [&](i32 i, const V3& r) -> V3 {
    if (i < 0) return V3{};
    const Body& B = *bodies[size_t(i)];
    return B.v + cross(B.w, r);
  };
  auto apply = [&](const Contact& c, const V3& J) {
    Body& A = *bodies[size_t(c.a)];
    A.v += J * A.inv_mass;
    A.w += Iw[size_t(c.a)] * cross(c.ra, J);
    if (c.b >= 0 && !bodies[size_t(c.b)]->asleep) {
      Body& B = *bodies[size_t(c.b)];
      B.v -= J * B.inv_mass;
      B.w -= Iw[size_t(c.b)] * cross(c.rb, J);
    }
  };
  auto eff = [&](const Contact& c, const V3& d) {
    f64 k = invm(c.a) + invm(c.b);
    const V3 ra = cross(c.ra, d);
    k += dot(ra, Iw[size_t(c.a)] * ra);
    if (c.b >= 0 && !bodies[size_t(c.b)]->asleep) {
      const V3 rb = cross(c.rb, d);
      k += dot(rb, Iw[size_t(c.b)] * rb);
    }
    return k > 0.0 ? 1.0 / k : 0.0;
  };
  auto pvel = [&](i32 i, const V3& r) -> V3 {
    if (i < 0 || !fine_[size_t(i)]) return V3{};
    return pv[size_t(i)] + cross(pw[size_t(i)], r);
  };
  // (the pieces' joint rows are kept for the sleep rules: a machine's drive at work)
  std::vector<JointPrep> coarse_prep;
  coarse_prep.swap(jprep_);
  jprep_.assign(joints.size(), JointPrep{});
  tprep_.assign(targets.size(), TargetPrep{});
  const f64 coarse_joint_dt = joint_dt_;
  joint_dt_ = h;
  // the islands, each stepped on its own (in parallel: they share no body, and each is stepped
  // the same whatever the threads)
  static const bool fprof = diag("SVX_PROFILE_FINE");
  const auto p0 = fprof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  fine_islands(fb);
  const auto p1 = fprof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  // (the contacts start each step from link_warm of the last one's impulses, as the joints' point
  // rows do: the two in step - with the contacts behind, a body at rest creeps along the ground)
  const f64 cw = par.link_warm;
  auto step_island = [&](const FineIsland& I) {
    for (int s = 0; s < n; ++s) {
      for (i32 i : I.bodies) {
        Body& B = *bodies[size_t(i)];
        integrate_link(B, h, keep[size_t(i)]);
        Iw[size_t(i)] = B.inv_inertia_world();
      }
      // the contacts where the bodies are now
      for (u32 k : I.contacts) {
        Contact& c = cs[k];
        const Body& A = *bodies[size_t(c.a)];
        const BodySphere& S = A.link->spheres[size_t(c.shape_a)];
        const V3 ca = A.x + rotate(A.q, S.c);
        if (spheres[k]) {
          const Body& B = *bodies[size_t(c.b)];
          const BodySphere& T = B.link->spheres[size_t(c.shape_b)];
          const V3 d = ca - (B.x + rotate(B.q, T.c));
          const f64 dist = norm(d);
          if (dist > 1e-9) c.n = d * (1.0 / dist);
          c.depth = S.r + T.r - dist;
        } else {
          c.depth = S.r - (dot(c.n, ca) - plane[k]);
        }
        c.p = ca - c.n * S.r;
        c.ra = c.p - A.x;
        c.rb = c.b >= 0 ? c.p - bodies[size_t(c.b)]->x : V3{};
        tangents(c.n, c.t1, c.t2);
        c.kn = eff(c, c.n);
        c.k1 = eff(c, c.t1);
        c.k2 = eff(c, c.t2);
        // (links do not bounce; a gap may close within this step, no more)
        c.bounce = c.depth < 0.0 ? c.depth / h : 0.0;
        c.bias = std::min(par.max_correction, par.baumgarte * std::max(0.0, c.depth - par.slop) / h);
        c.ln = c.depth < 0.0 && s == 0 ? 0.0 : cw * lam[k][0];
        c.l1 = cw * lam[k][1];
        c.l2 = cw * lam[k][2];
        c.lp = 0.0;
        apply(c, c.n * c.ln + c.t1 * c.l1 + c.t2 * c.l2);
      }
      for (u32 k : I.joints) {
        jprep_[k] = JointPrep{};
        prepare_joint(k, h, Iw, &fine_);
      }
      for (u32 k : I.targets) {
        tprep_[k] = TargetPrep{};
        prepare_target(k, h, Iw, &fine_);
      }
      // (joints, targets, then contacts - the order of the XPBD bodies the behaviours were made
      // with: what the ground holds up last is held up)
      for (int it = 0; it < std::max(1, par.link_iterations); ++it) {
        // (the joints swept both ways in turn: along a chain and back, an impulse reaches its end)
        const size_t nj = I.joints.size();
        const bool reverse = (it & 1) != 0;
        for (size_t q = 0; q < nj; ++q) solve_joint(I.joints[reverse ? nj - 1 - q : q]);
        for (u32 k : I.targets) solve_target(k);
        for (u32 k : I.contacts) {
          Contact& c = cs[k];
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
      }
      // (a limp link spins no faster than its limit: an impact does not set it whirling)
      for (i32 i : I.bodies) {
        Body& B = *bodies[size_t(i)];
        const f64 cap = B.link->max_spin, ws = norm(B.w);
        if (cap > 0.0 && ws > cap) B.w *= cap / ws;
      }
      // position error, on pseudo velocities (split impulse: no energy added)
      for (i32 i : I.bodies) {
        pv[size_t(i)] = V3{};
        pw[size_t(i)] = V3{};
      }
      for (int it = 0; it < std::max(0, par.link_position_iterations); ++it) {
        for (u32 k : I.joints) solve_joint_position(k, pv, pw);
        for (u32 k : I.contacts) {
          Contact& c = cs[k];
          if (c.bias <= 0.0) continue;
          const f64 vn = dot(pvel(c.a, c.ra) - pvel(c.b, c.rb), c.n);
          const f64 lp = std::max(0.0, c.lp + c.kn * (c.bias - vn));
          const V3 J = c.n * (lp - c.lp);
          c.lp = lp;
          pv[size_t(c.a)] += J * bodies[size_t(c.a)]->inv_mass;
          pw[size_t(c.a)] += Iw[size_t(c.a)] * cross(c.ra, J);
          if (c.b >= 0 && fine_[size_t(c.b)]) {
            pv[size_t(c.b)] -= J * bodies[size_t(c.b)]->inv_mass;
            pw[size_t(c.b)] -= Iw[size_t(c.b)] * cross(c.rb, J);
          }
        }
      }
      for (u32 k : I.joints) finish_joint(k, h);
      for (u32 k : I.targets) finish_target(k, h);
      for (i32 i : I.bodies) {
        Body& B = *bodies[size_t(i)];
        B.x += (B.v + pv[size_t(i)]) * h;
        B.q = integrate(B.q, B.w + pw[size_t(i)], h);
        B.age += h;
      }
      for (u32 k : I.contacts) {
        lam[k] = {cs[k].ln, cs[k].l1, cs[k].l2};
        tot[k][0] += cs[k].ln;
        tot[k][1] += cs[k].l1;
        tot[k][2] += cs[k].l2;
      }
    }
  };
  const std::vector<FineIsland>& islands = fine_islands_;
  if (islands.size() == 1) {
    step_island(islands[0]);
  } else {
    parallel_for(static_cast<i64>(islands.size()), 1, [&](i64 i0, i64 i1) {
      for (i64 i = i0; i < i1; ++i) step_island(islands[size_t(i)]);
    });
  }
  const auto p2 = fprof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  // (the pieces' rows back, for the sleep rules; the fine joints' prepared as last stepped)
  for (size_t k = 0; k < coarse_prep.size() && k < jprep_.size(); ++k)
    if (coarse_prep[k].on) jprep_[k] = coarse_prep[k];
  joint_dt_ = coarse_joint_dt;
  // the substep's impulses (their sum over the steps): the loads on what the links press on, the
  // sleep rules, the senses
  fine_warm_.clear();
  const f64 g = par.gravity;
  for (size_t k = 0; k < nc; ++k) {
    Contact c = cs[k];
    fine_warm_[c.key] = lam[k];
    c.ln = tot[k][0];
    c.l1 = tot[k][1];
    c.l2 = tot[k][2];
    c.approach = 0.0;
    // (a sleeper pushed on harder than half its weight bears wakes: a brick shoved, not a slab
    // stood on)
    if (c.b >= 0) {
      Body& B = *bodies[size_t(c.b)];
      if (B.asleep && norm(c.impulse()) > 0.5 * B.mass * g * dt) wake(B);
    }
    report.push_back(c);
  }
  fine_cs_.clear();
  if (fprof) {
    const auto p3 = std::chrono::steady_clock::now();
    static f64 acc[3] = {0, 0, 0};
    static int calls = 0;
    auto msd = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) { return std::chrono::duration<f64, std::milli>(b - a).count(); };
    acc[0] += msd(p0, p1);
    acc[1] += msd(p1, p2);
    acc[2] += msd(p2, p3);
    if (++calls % 120 == 0) {
      std::printf("  [step_fine] islands %.3f parallel %.3f after %.3f ms (%zu islands, %zu contacts)\n", acc[0] / 120, acc[1] / 120, acc[2] / 120, fine_islands_.size(), nc);
      for (f64& a : acc) a = 0.0;
    }
  }
}

// What a link keeps of its velocity over a step of dt (air, tissue), and of its spin about its
// length.
std::array<f64, 3> RigidWorld::link_keep(const Body& b, f64 dt) const {
  const LinkData& L = *b.link;
  return {L.keep_linear < 1.0 ? pow01(std::max(0.0, L.keep_linear), dt) : 1.0, L.keep_angular < 1.0 ? pow01(std::max(0.0, L.keep_angular), dt) : 1.0,
          L.twist_damping > 0.0 ? 1.0 - dm::exp(-L.twist_damping * dt) : 0.0};
}

// A link's velocity over a step of dt: gravity and the forces on it, air drag and tissue, its
// limits. (A kinematic link keeps the velocity its host gives it.)
void RigidWorld::integrate_link(Body& b, f64 dt) { integrate_link(b, dt, link_keep(b, dt)); }

void RigidWorld::integrate_link(Body& b, f64 dt, const std::array<f64, 3>& keep) {
  const LinkData& L = *b.link;
  if (!L.kinematic) {
    b.v.z -= par.gravity * dt;
    if (norm2(b.force) > 0.0) b.v += b.force * (b.inv_mass * dt);
    if (norm2(b.torque) > 0.0) b.w += b.inv_inertia_world() * b.torque * dt;
    if (L.keep_linear < 1.0) b.v *= keep[0];
    if (L.keep_angular < 1.0) b.w *= keep[1];
    if (L.twist_damping > 0.0) {
      const V3 a = normalized(rotate(b.q, L.long_axis));
      b.w -= a * (dot(b.w, a) * keep[2]);
    }
    const f64 top = b.max_speed > 0.0 ? b.max_speed : par.link_max_speed;
    const f64 s = norm(b.v);
    if (s > top) b.v *= top / s;
    const f64 ws = norm(b.w);
    if (L.max_spin > 0.0 && ws > L.max_spin) b.w *= L.max_spin / ws;
  }
  b.v_pre = b.v;
  b.w_pre = b.w;
}

}  // namespace svx
