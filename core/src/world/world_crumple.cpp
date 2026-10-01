// structvox — crumpling (docs/DAMAGE.md §4): the crash damage of ductile, crumpling material -
// a vehicle's body, a machine's housing, a container, a duct: whatever a host builds of it.
//
// A contact with a crumpling side (Material::crush: sheet metal, a thin-walled frame) carries at most
// crush x its area (phys/rigid.cpp): the bodies keep closing while it does, and here, after the
// substep, the crumpling side folds out of what it hit. Along the axis of its lattice nearest the
// push, in columns: each column whose front voxel is pressed into what it hit is pushed back until
// its front is out of it - and its neighbours are dragged along, a cell less per column (a dent
// has sloped sides: the panel around it bends, it does not shear). A column's front segment moves
// back as a whole where there is room behind it (a panel over a hollow body); where there
// is not (a run of material: a fender, a rail, along the push), what does not fit folds out to
// the side of the run near its new front, outwards and upwards first (the crumpled metal piles up
// in folds), or where it cannot, is compacted. When both sides crumple (two such bodies), the first
// folds by half the overlap and the other by the rest. Glass near what folds shatters.
//
// The piece is edited in place: it keeps its id, its place and its motion (its mass and samples
// are made again), and is announced reshaped once per tick (PieceReshaped: its host meshes it
// again); bits that no longer hold on to it come off (dust, or pieces of their own).
//
// With the contact's force capped, the collision's energy goes out over the distance the body's
// front folds, as in a crash: with a car's sheet metal (the game's: docs/VEHICLES.md) a 1.2 t
// car at 50 km/h folds some half a metre of its front against a wall, at a few hundred kN, over
// tens of milliseconds - and the wall feels those few hundred kN, not the rigid spike of a body
// stopped in a substep.
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstdlib>

#include "svx/world/world.hpp"
#include "world_internal.hpp"

namespace svx {

using namespace world_detail;

namespace {

// (glass, lamps: brittle and nearly free to crack - they shatter as the body around them folds)
bool shatters(const Material& M) { return !M.ductile && M.Gf <= 10.0; }

// Grows a shape's box to take in lattice voxel q (its cells re-indexed).
void grow_shape(BodyShape& S, const IVec3& q) {
  IVec3 lo = S.lo, hi{S.lo[0] + S.dim[0] - 1, S.lo[1] + S.dim[1] - 1, S.lo[2] + S.dim[2] - 1};
  for (int a = 0; a < 3; ++a) {
    lo[a] = std::min(lo[a], q[a]);
    hi[a] = std::max(hi[a], q[a]);
  }
  BodyShape N;
  N.lo = lo;
  N.dim = {hi[0] - lo[0] + 1, hi[1] - lo[1] + 1, hi[2] - lo[2] + 1};
  const size_t cells = size_t(N.dim[0]) * size_t(N.dim[1]) * size_t(N.dim[2]);
  std::vector<Vox> vox(cells, kAir);
  std::vector<u32> frag(cells, 0);
  std::vector<u8> brk(cells, 0);
  std::array<std::vector<u8>, kMaxLayers> layer;
  for (int L = 0; L < kMaxLayers; ++L)
    if (!S.layer[size_t(L)].empty()) layer[size_t(L)].assign(cells, 0);
  std::vector<i32> remap(S.vox.size(), -1);
  for (i32 i = 0; i < static_cast<i32>(S.vox.size()); ++i) {
    const i32 j = N.index(S.voxel(i));
    remap[size_t(i)] = j;
    vox[size_t(j)] = S.vox[size_t(i)];
    frag[size_t(j)] = S.frag[size_t(i)];
    brk[size_t(j)] = S.brk[size_t(i)];
    for (int L = 0; L < kMaxLayers; ++L)
      if (!layer[size_t(L)].empty()) layer[size_t(L)][size_t(j)] = S.layer[size_t(L)][size_t(i)];
  }
  for (u64& e : S.jbrk) e = shape_junction_code(remap[size_t(e >> 16)], static_cast<int>((e >> 8) & 0xFF), static_cast<int>(e & 0xFF));
  std::sort(S.jbrk.begin(), S.jbrk.end());
  S.lo = N.lo;
  S.dim = N.dim;
  S.vox.swap(vox);
  S.frag.swap(frag);
  S.brk.swap(brk);
  S.layer.swap(layer);
}

}  // namespace

void World::Impl::crumple(f64 dt) {
  const std::vector<Contact>& cs = rigid_.contacts();
  bool any = false;
  for (const Contact& c : cs) any = any || c.crushing;
  if (!any) return;
  // the patches: per crumpling body and what it hit (a static grid, a body), the contacts' box and
  // the direction into the body
  struct Patch {
    i64 body = 0;
    i64 other = 0;   // a body's id, or -(1 + grid slot)
    V3 lo, hi;
    V3 n;            // (sum) into the body
    bool both = false;  // (both sides crumple)
    f64 impulse = 0.0;  // (its contacts' normal impulses, this substep)
  };
  std::vector<Patch> patches;
  auto add = [&](i32 bi, i64 other, const V3& p, const V3& n, bool both, f64 ln) {
    const i64 id = rigid_.bodies[size_t(bi)]->id;
    Patch* P = nullptr;
    for (Patch& q : patches)
      if (q.body == id && q.other == other) P = &q;
    if (!P) {
      patches.push_back(Patch{});
      P = &patches.back();
      P->body = id;
      P->other = other;
      P->lo = V3{INFINITY, INFINITY, INFINITY};
      P->hi = V3{-INFINITY, -INFINITY, -INFINITY};
    }
    for (int a = 0; a < 3; ++a) {
      P->lo[a] = std::min(P->lo[a], p[a]);
      P->hi[a] = std::max(P->hi[a], p[a]);
    }
    P->n += n;
    P->both = P->both || both;
    P->impulse += ln;
  };
  // (the pairs that crushed: every contact of theirs spans the patch, crushing or not)
  std::vector<std::pair<i32, i32>> crushed_pairs;
  for (const Contact& c : cs)
    if (c.crushing) crushed_pairs.push_back({c.a, c.b});
  std::sort(crushed_pairs.begin(), crushed_pairs.end());
  crushed_pairs.erase(std::unique(crushed_pairs.begin(), crushed_pairs.end()), crushed_pairs.end());
  for (const Contact& c : cs) {
    if (!(c.crush != 0) || c.a < 0 || size_t(c.a) >= rigid_.bodies.size()) continue;
    if (c.b >= 0 && size_t(c.b) >= rigid_.bodies.size()) continue;
    if (!std::binary_search(crushed_pairs.begin(), crushed_pairs.end(), std::pair<i32, i32>{c.a, c.b})) continue;
    const i64 other_a = c.b < 0 ? -(1 + static_cast<i64>(c.grid)) : rigid_.bodies[size_t(c.b)]->id;
    const bool both = c.crush == 3;
    // (the push: the normals of the contacts being closed, by how fast - a sample deep in a thin
    // panel of the other body finds a face of it across the push, and does not close on it)
    const V3 m = c.n * (std::max(0.0, c.approach) + 1e-3);
    if (c.crush & 1) add(c.a, other_a, c.p, m, both, c.ln);                                             // (n pushes a out of b: into a)
    if ((c.crush & 2) && c.b >= 0) add(c.b, rigid_.bodies[size_t(c.a)]->id, c.p, m * -1.0, both, c.ln);  // (into b)
  }
  std::sort(patches.begin(), patches.end(), [](const Patch& x, const Patch& y) { return x.body < y.body || (x.body == y.body && x.other < y.other); });
  std::vector<i64> touched;
  for (const Patch& P : patches) {
    Body* bp = rigid_.find(P.body);
    if (!bp || !(norm2(P.n) > 0.0)) continue;
    Body& b = *bp;
    const V3 n = normalized(P.n);
    // what it hit: its solid at a world point
    const Body* O = P.other > 0 ? rigid_.find(P.other) : nullptr;
    const i32 slot = P.other < 0 ? static_cast<i32>(-P.other - 1) : -1;
    if (P.other > 0 && !O) continue;
    if (slot >= 0 && !live(static_cast<u16>(slot))) continue;
    auto inside = [&](const V3& X) {
      if (O) {
        for (size_t k = 0; k < O->shapes.size(); ++k) {
          const BodyShape& S = O->shapes[k];
          if (vox_solid(S.get(voxel_of(O->world_to_lattice(k, X), S.h)))) return true;
        }
        return false;
      }
      const u16 g = static_cast<u16>(slot);
      return vox_solid(vg(g).get(voxel_of(g == 0 ? X : xf_of(g).from(X), h_of(g))));
    };
    // (a wall it presses through gives way first)
    if (slot >= 0 && punch(b, static_cast<u16>(slot), n, P.lo, P.hi, P.impulse / dt, dt)) touched.push_back(b.id);
    // (two crumpling bodies: the first folds by half of the overlap, the other out of the rest)
    const bool half = P.both && P.other > 0 && P.body < P.other;
    bool changed = false;
    std::vector<std::pair<i32, IVec3>> crushed;  // (shape, its lattice voxel: glass near it shatters)
    for (size_t k = 0; k < b.shapes.size(); ++k) {
      BodyShape& S = b.shapes[k];
      const f64 h = S.h;
      const Quat Ql = b.lattice_rot(k);
      // (the axis of the lattice nearest the push, and the direction into the body along it)
      const V3 nl = rotate_inv(Ql, n);
      int ax = 0;
      for (int a = 1; a < 3; ++a)
        if (std::abs(nl[a]) > std::abs(nl[ax])) ax = a;
      const i32 in = nl[ax] > 0.0 ? 1 : -1;
      const int a1 = (ax + 1) % 3, a2 = (ax + 2) % 3;
      // the columns: across the patch, and as far again as a dent's sides drag (kDrag cells)
      const f64 mg = std::max(3.0 * h, 0.25);  // (the manifold's contacts span the patch loosely)
      const V3 wlo = P.lo - V3{mg, mg, mg}, whi = P.hi + V3{mg, mg, mg};
      V3 llo{INFINITY, INFINITY, INFINITY}, lhi{-INFINITY, -INFINITY, -INFINITY};
      for (int c = 0; c < 8; ++c) {
        const V3 l = b.world_to_lattice(k, V3{(c & 1) ? whi.x : wlo.x, (c & 2) ? whi.y : wlo.y, (c & 4) ? whi.z : wlo.z});
        for (int a = 0; a < 3; ++a) {
          llo[a] = std::min(llo[a], l[a]);
          lhi[a] = std::max(lhi[a], l[a]);
        }
      }
      constexpr i32 kDrag = 6, kFold = 5, kPile = 8;
      IVec3 vlo = voxel_of(llo, h), vhi = voxel_of(lhi, h);
      vlo[a1] -= kDrag;
      vlo[a2] -= kDrag;
      vhi[a1] += kDrag;
      vhi[a2] += kDrag;
      for (int a = 0; a < 3; ++a) {
        vlo[a] = std::max(vlo[a], S.lo[a]);
        vhi[a] = std::min(vhi[a], S.lo[a] + S.dim[a] - 1);
      }
      if (vlo[0] > vhi[0] || vlo[1] > vhi[1] || vlo[2] > vhi[2]) continue;
      const i32 n1 = vhi[a1] - vlo[a1] + 1, n2 = vhi[a2] - vlo[a2] + 1;
      // (a column's cells from its outer end - towards what it hit - inwards)
      const i32 outer = in < 0 ? vhi[ax] : vlo[ax], inner = in < 0 ? vlo[ax] : vhi[ax];
      const i32 span = (inner - outer) * in + 1;
      const i32 ext_lo = S.lo[ax], ext_hi = S.lo[ax] + S.dim[ax] - 1;
      auto cell = [&](i32 c1, i32 c2, i32 d) {
        IVec3 p;
        p[ax] = d;
        p[a1] = c1;
        p[a2] = c2;
        return p;
      };
      auto world_of = [&](const IVec3& p) { return b.lattice_to_world(k, V3{h * p[0], h * p[1], h * p[2]}); };
      auto air = [&](const IVec3& q) { return !vox_solid(S.get(q)); };
      // the anchors on its voxels follow them (a joint's end, a wheel's mount: the voxel, and the
      // point it acts at, in the same lattice); a voxel gone lets go
      auto follow = [&](const IVec3& from, const IVec3& to) {
        const V3 shift{h * (to[0] - from[0]), h * (to[1] - from[1]), h * (to[2] - from[2])};
        for (JointRec& r : att_.joints)
          for (JointRec::End* E : {&r.a, &r.b})
            if (E->piece == b.id && E->shape == static_cast<i32>(k) && E->voxel == from) {
              E->voxel = to;
              E->point += shift;
            }
        for (WheelRec& r : att_.wheels)
          if (r.mount.piece == b.id && r.mount.shape == static_cast<i32>(k) && r.mount.voxel == from) {
            r.mount.voxel = to;
            r.mount.point += shift;
          }
      };
      auto clear = [&](i32 i) {
        S.vox[size_t(i)] = kAir;
        S.frag[size_t(i)] = 0;
        S.brk[size_t(i)] = 0;
        for (auto& l : S.layer)
          if (!l.empty()) l[size_t(i)] = 0;
      };
      auto move = [&](const IVec3& p, const IVec3& q) {
        if (S.index(q) < 0) grow_shape(S, q);
        const i32 i = S.index(p), j = S.index(q);
        S.vox[size_t(j)] = S.vox[size_t(i)];
        S.frag[size_t(j)] = S.frag[size_t(i)];
        S.brk[size_t(j)] = 0;
        for (auto& l : S.layer)
          if (!l.empty()) l[size_t(j)] = l[size_t(i)];
        clear(i);
        // (it bonds where it is now: the faces into it are whole)
        for (int a = 0; a < 3; ++a) {
          IVec3 lower = q;
          lower[a] -= 1;
          const i32 li = S.index(lower);
          if (li >= 0) S.brk[size_t(li)] = static_cast<u8>(S.brk[size_t(li)] & ~(1u << a));
        }
        follow(p, q);
      };
      // fronts and dents: a column's front (its first solid cell from the outer end), and the
      // cells it is pushed back by
      std::vector<i32> front(size_t(n1) * size_t(n2), INT32_MIN), dent(size_t(n1) * size_t(n2), 0);
      auto at = [&](i32 c1, i32 c2) { return size_t(c1 - vlo[a1]) * size_t(n2) + size_t(c2 - vlo[a2]); };
      bool pressed_any = false;
      for (i32 c1 = vlo[a1]; c1 <= vhi[a1]; ++c1)
        for (i32 c2 = vlo[a2]; c2 <= vhi[a2]; ++c2) {
          i32 f = INT32_MIN;
          for (i32 s = 0; s < span; ++s) {
            const IVec3 p = cell(c1, c2, outer + in * s);
            if (air(p)) continue;
            // (glass pressed in shatters, and the column goes on behind it)
            if (inside(world_of(p)) && shatters(mats()[vox_mat(S.get(p))])) {
              clear(S.index(p));
              crushed.push_back({static_cast<i32>(k), p});
              changed = true;
              continue;
            }
            f = outer + in * s;
            break;
          }
          front[at(c1, c2)] = f;
          if (f == INT32_MIN || !inside(world_of(cell(c1, c2, f)))) continue;
          i32 d = 1;
          while (d < span && inside(world_of(cell(c1, c2, f + in * d)))) ++d;
          if (half) d = (d + static_cast<i32>(st_.ticks & 1)) / 2;  // (odd cells to each side in turn)
          dent[at(c1, c2)] = d;
          pressed_any = true;
        }
      if (!pressed_any) continue;
      // (the sides of a dent: each column dragged back to a cell less than its neighbours)
      for (int pass = 0; pass < kDrag; ++pass) {
        bool more = false;
        for (i32 c1 = vlo[a1]; c1 <= vhi[a1]; ++c1)
          for (i32 c2 = vlo[a2]; c2 <= vhi[a2]; ++c2) {
            if (front[at(c1, c2)] == INT32_MIN) continue;
            i32 d = dent[at(c1, c2)];
            const i32 nb[4][2] = {{c1 - 1, c2}, {c1 + 1, c2}, {c1, c2 - 1}, {c1, c2 + 1}};
            for (const auto& q : nb) {
              if (q[0] < vlo[a1] || q[0] > vhi[a1] || q[1] < vlo[a2] || q[1] > vhi[a2]) continue;
              if (front[at(q[0], q[1])] == INT32_MIN) continue;
              d = std::max(d, dent[at(q[0], q[1])] - 1);
            }
            if (d != dent[at(c1, c2)]) {
              dent[at(c1, c2)] = d;
              more = true;
            }
          }
        if (!more) break;
      }
      // the lateral directions a fold may go, per column: outwards (from the body's centre) and
      // upwards first
      IVec3 sdir[4];
      V3 sw[4];
      {
        int m = 0;
        for (int a : {a1, a2})
          for (int sg = -1; sg <= 1; sg += 2) {
            IVec3 e{0, 0, 0};
            e[a] = sg;
            V3 el;
            el[a] = sg;
            sdir[m] = e;
            sw[m] = rotate(Ql, el);
            ++m;
          }
      }
      const V3 centre = b.x;
      // the columns pushed back, in order
      auto run_len = [&](i32 c1, i32 c2, i32 from, i32 cap) {
        i32 m = 0;
        while (m < cap && !air(cell(c1, c2, from + in * m))) ++m;
        return m;
      };
      const u64 salt = mix64(static_cast<u64>(b.id) * 0x9E3779B97F4A7C15ull ^ static_cast<u64>(st_.ticks) ^ (static_cast<u64>(k) << 40));
      std::vector<i32> moved(front.size(), 0);  // (the panels that went back: their fronts' new places)
      for (i32 c1 = vlo[a1]; c1 <= vhi[a1]; ++c1)
        for (i32 c2 = vlo[a2]; c2 <= vhi[a2]; ++c2) {
          const i32 d = dent[at(c1, c2)];
          const i32 f = front[at(c1, c2)];
          if (d <= 0 || f == INT32_MIN) continue;
          const u64 hc = mix64(salt ^ (static_cast<u64>(static_cast<u32>(c1)) << 32 | static_cast<u32>(c2)));
          const i32 nf = f + in * d;  // (its new front)
          // a short run (a panel over the hollow behind it; metal already folded): back as a whole,
          // where there is room
          const i32 run = run_len(c1, c2, f, kPile + 1);
          if (run <= kPile) {
            // (where it goes: air now - or its own cells - and out of what it hit; the first of two
            // crumpling bodies leaves the rest of the overlap to the other)
            bool room = true;
            for (i32 s = d; s < run + d && room; ++s) {
              const i32 dd = f + in * s;
              const IVec3 q = cell(c1, c2, dd);
              room = dd >= ext_lo && dd <= ext_hi && (s < run || air(q)) && (half || !inside(world_of(q)));
            }
            if (room) {
              for (i32 s = run - 1; s >= 0; --s) {
                const IVec3 p = cell(c1, c2, f + in * s);
                crushed.push_back({static_cast<i32>(k), p});
                move(p, cell(c1, c2, f + in * (s + d)));
              }
              moved[at(c1, c2)] = d;
              changed = true;
              continue;
            }
          }
          // a long run (a fender, a rail along the push) or no room: its front segment folds out
          // beside it near the new front, outwards and upwards first (now and then the other way:
          // the folds of crumpled metal); what cannot is compacted
          int order[4] = {0, 1, 2, 3};
          {
            const V3 out = world_of(cell(c1, c2, nf)) - centre;
            f64 score[4];
            for (int e = 0; e < 4; ++e) score[e] = dot(sw[e], out) / std::max(1e-9, norm(out)) + 0.75 * sw[e].z;
            for (int i = 1; i < 4; ++i)
              for (int j = i; j > 0 && score[order[j]] > score[order[j - 1]]; --j) std::swap(order[j], order[j - 1]);
          }
          for (i32 s = d - 1; s >= 0; --s) {
            const IVec3 p = cell(c1, c2, f + in * s);
            if (air(p)) continue;
            crushed.push_back({static_cast<i32>(k), p});
            changed = true;
            bool done = false;
            const bool flip = ((hc >> (8 + s)) % 3) == 0;
            for (i32 r = 0; r <= kFold && !done; ++r) {
              const i32 dr = nf + in * r;
              if (run_len(c1, c2, dr, 3) < 3) continue;  // (beside the run itself, not beside a fold)
              for (int e = 0; e < 4 && !done; ++e) {
                const int o = order[flip ? 3 - e : e];
                const IVec3 t = cell(c1 + sdir[o][a1], c2 + sdir[o][a2], dr);
                if (!air(t) || (!half && inside(world_of(t)))) continue;
                move(p, t);
                done = true;
              }
            }
            if (!done) clear(S.index(p));  // (compacted)
          }
        }
      // a panel pushed back further than its neighbour stays joined to it: the neighbour's panel
      // stretches back along the step (a dent's sides, not a sheared staircase)
      for (i32 c1 = vlo[a1]; c1 <= vhi[a1]; ++c1)
        for (i32 c2 = vlo[a2]; c2 <= vhi[a2]; ++c2) {
          const i32 dc = moved[at(c1, c2)];
          if (dc <= 0) continue;
          const i32 nfc = front[at(c1, c2)] + in * dc;
          const i32 nb[4][2] = {{c1 - 1, c2}, {c1 + 1, c2}, {c1, c2 - 1}, {c1, c2 + 1}};
          for (const auto& q : nb) {
            if (q[0] < vlo[a1] || q[0] > vhi[a1] || q[1] < vlo[a2] || q[1] > vhi[a2]) continue;
            const i32 fq = front[at(q[0], q[1])];
            if (fq == INT32_MIN || (dent[at(q[0], q[1])] > 0 && moved[at(q[0], q[1])] == 0)) continue;  // (folded: no panel there)
            const i32 nfq = fq + in * moved[at(q[0], q[1])];
            const i32 step = (nfc - nfq) * in;  // (> 0: this one went further back)
            const IVec3 top = cell(q[0], q[1], nfq);
            const i32 ti = S.index(top);
            if (step <= 0 || ti < 0 || !vox_solid(S.vox[size_t(ti)])) continue;
            for (i32 j = 1; j <= step; ++j) {
              const IVec3 r = cell(q[0], q[1], nfq + in * j);
              const i32 dd = nfq + in * j;
              if (dd < ext_lo || dd > ext_hi || !air(r)) break;
              const i32 ri = S.index(r);
              S.vox[size_t(ri)] = S.vox[size_t(ti)];
              S.frag[size_t(ri)] = S.frag[size_t(ti)];
              S.brk[size_t(ri)] = 0;
              IVec3 lower = r;
              for (int a = 0; a < 3; ++a) {
                lower = r;
                lower[a] -= 1;
                const i32 li = S.index(lower);
                if (li >= 0) S.brk[size_t(li)] = static_cast<u8>(S.brk[size_t(li)] & ~(1u << a));
              }
            }
          }
        }
    }
    // glass near what folded shatters: shards
    V3 shard_c;
    i32 shards = 0;
    int shard_mat = -1;
    for (const auto& [k, p] : crushed) {
      BodyShape& S = b.shapes[size_t(k)];
      for (i32 dx = -2; dx <= 2; ++dx)
        for (i32 dy = -2; dy <= 2; ++dy)
          for (i32 dz = -2; dz <= 2; ++dz) {
            const IVec3 q{p[0] + dx, p[1] + dy, p[2] + dz};
            const i32 i = S.index(q);
            if (i < 0 || !vox_solid(S.vox[size_t(i)])) continue;
            const Material& M = mats()[vox_mat(S.vox[size_t(i)])];
            if (!shatters(M)) continue;
            shard_mat = static_cast<int>(vox_mat(S.vox[size_t(i)]));
            shard_c += b.lattice_to_world(size_t(k), V3{S.h * q[0], S.h * q[1], S.h * q[2]});
            ++shards;
            S.vox[size_t(i)] = kAir;
            S.frag[size_t(i)] = 0;
            S.brk[size_t(i)] = 0;
            for (auto& l : S.layer)
              if (!l.empty()) l[size_t(i)] = 0;
            changed = true;
          }
    }
    if (shards > 0) {
      const V3 X = shard_c * (1.0 / shards);
      const size_t before = events_.size();
      dust_event(X, b.v + cross(b.w, X - b.x), shards, false);
      if (events_.size() > before) events_.back().material = shard_mat;
      st_.pulverized_voxels += shards;
    }
    if (changed) touched.push_back(b.id);
  }
  std::sort(touched.begin(), touched.end());
  touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
  for (i64 id : touched)
    if (Body* b = rigid_.find(id)) reshape_in_place(*b);
  flush_body_changes();
}

// A crumpling body pressing into a static structure (a vehicle into a wall): where the force it
// presses with is more than the wall takes around the patch - its punching shear: the patch's
// perimeter x the wall's thickness there x its material's shear strength - the wall's fragments
// in front of the patch, through its thickness, break out and go on with the body, which slows
// as they take up their share of its momentum (a plastic collision). A car's crumpling front
// presses at a few hundred kN: glass gives way to it, a brick wall to its stiffest part (an
// engine block) once the front has folded back to it, concrete to neither. (Slower failure - a wall bending over, its
// bonds overloaded - is the structure's own solve.)
bool World::Impl::punch(Body& b, u16 g, const V3& n, const V3& plo, const V3& phi, f64 force, f64 dt) {
  if (!(force > 0.0) || !live(g)) return false;
  const V3 u = n * -1.0;  // (into the wall)
  const f64 vn = dot(b.v, u);
  if (!(vn > 0.5)) return false;
  const VoxelGrid& G = vg(g);
  const f64 h = h_of(g);
  const LatticeXf& X = xf_of(g);
  // the wall's lattice: the axis nearest the push
  const V3 ul = g == 0 ? u : X.dir_from(u);
  int ax = 0;
  for (int a = 1; a < 3; ++a)
    if (std::abs(ul[a]) > std::abs(ul[ax])) ax = a;
  const i32 s = ul[ax] > 0.0 ? 1 : -1;
  const int a1 = (ax + 1) % 3, a2 = (ax + 2) % 3;
  // the footprint: the body's crush front - the columns of its lattice along the push whose
  // foremost voxels are near the foremost of all, near the patch (a body's whole front against a
  // wall, the contacts kept being a few of it) - in the wall's lattice
  V3 llo{INFINITY, INFINITY, INFINITY}, lhi{-INFINITY, -INFINITY, -INFINITY};
  for (size_t k = 0; k < b.shapes.size(); ++k) {
    const BodyShape& S = b.shapes[k];
    const f64 hb = S.h;
    const V3 bl = rotate_inv(b.lattice_rot(k), u);
    int bx = 0;
    for (int a = 1; a < 3; ++a)
      if (std::abs(bl[a]) > std::abs(bl[bx])) bx = a;
    const i32 bs = bl[bx] > 0.0 ? 1 : -1;
    const int b1 = (bx + 1) % 3, b2 = (bx + 2) % 3;
    f64 dlo = INFINITY, dhi = -INFINITY;
    for (int c = 0; c < 8; ++c) {
      const V3 l = b.world_to_lattice(k, V3{(c & 1) ? phi.x : plo.x, (c & 2) ? phi.y : plo.y, (c & 4) ? phi.z : plo.z}) * (1.0 / hb);
      dlo = std::min(dlo, l[bx]);
      dhi = std::max(dhi, l[bx]);
    }
    constexpr i32 kFront = 3;
    const i32 from = bs > 0 ? std::min(S.lo[bx] + S.dim[bx] - 1, static_cast<i32>(std::floor(dhi + 0.5)) + kFront)
                            : std::max(S.lo[bx], static_cast<i32>(std::floor(dlo + 0.5)) - kFront);
    const i32 to = bs > 0 ? std::max(S.lo[bx], static_cast<i32>(std::floor(dlo + 0.5)) - kFront)
                          : std::min(S.lo[bx] + S.dim[bx] - 1, static_cast<i32>(std::floor(dhi + 0.5)) + kFront);
    const i32 span = (to - from) * -bs + 1;
    if (span <= 0) continue;
    std::vector<i32> fr(size_t(S.dim[b1]) * size_t(S.dim[b2]), -1);
    i32 best = -1;
    for (i32 c1 = 0; c1 < S.dim[b1]; ++c1)
      for (i32 c2 = 0; c2 < S.dim[b2]; ++c2) {
        IVec3 p;
        p[b1] = S.lo[b1] + c1;
        p[b2] = S.lo[b2] + c2;
        for (i32 d = 0; d < span; ++d) {
          p[bx] = from - bs * d;
          if (!vox_solid(S.get(p))) continue;
          fr[size_t(c1) * size_t(S.dim[b2]) + size_t(c2)] = span - d;  // (larger: nearer the wall)
          best = std::max(best, span - d);
          break;
        }
      }
    if (best < 0) continue;
    for (i32 c1 = 0; c1 < S.dim[b1]; ++c1)
      for (i32 c2 = 0; c2 < S.dim[b2]; ++c2) {
        const i32 f = fr[size_t(c1) * size_t(S.dim[b2]) + size_t(c2)];
        if (f < 0 || f < best - kFront) continue;
        IVec3 p;
        p[b1] = S.lo[b1] + c1;
        p[b2] = S.lo[b2] + c2;
        p[bx] = from - bs * (span - f);
        const V3 wp = b.lattice_to_world(k, V3{hb * p[0], hb * p[1], hb * p[2]});
        const V3 l = (g == 0 ? wp : X.from(wp)) * (1.0 / h);
        for (int a = 0; a < 3; ++a) {
          llo[a] = std::min(llo[a], l[a] - 0.5 * hb / h);
          lhi[a] = std::max(lhi[a], l[a] + 0.5 * hb / h);
        }
      }
  }
  if (!(llo.x <= lhi.x)) return false;
  IVec3 vlo, vhi;
  for (int a = 0; a < 3; ++a) {
    vlo[a] = static_cast<i32>(std::floor(llo[a] + 0.5));
    vhi[a] = static_cast<i32>(std::floor(lhi[a] + 0.5));
  }
  vlo[ax] -= 1;
  vhi[ax] += 1;
  // (depth: from the patch's near side into the wall)
  const i32 start = s > 0 ? vlo[ax] : vhi[ax];
  constexpr i32 kDeep = 16;
  i32 thick = 0, cols = 0;
  std::array<i32, kMaxMaterials> seen{};  // (per material id: a host's registered ones too)
  for (i32 c1 = vlo[a1]; c1 <= vhi[a1]; ++c1)
    for (i32 c2 = vlo[a2]; c2 <= vhi[a2]; ++c2) {
      i32 d = 0, run = 0;
      IVec3 p;
      p[a1] = c1;
      p[a2] = c2;
      for (; d < kDeep; ++d) {
        p[ax] = start + s * d;
        if (vox_solid(G.get(p))) break;
      }
      for (; d < kDeep; ++d) {
        p[ax] = start + s * d;
        const Vox v = G.get(p);
        if (!vox_solid(v)) break;
        if (run == 0) {
          ++seen[std::min(static_cast<size_t>(vox_mat(v)), seen.size() - 1)];
        }
        ++run;
      }
      if (run > 0) {
        thick = std::max(thick, run);
        ++cols;
      }
    }
  if (cols == 0) return false;
  size_t mat = 0;
  for (size_t m = 1; m < seen.size(); ++m)
    if (seen[m] > seen[mat]) mat = m;
  const Material& M = mats()[static_cast<MaterialId>(mat)];
  if (M.indestructible) return false;
  // (its strength to break out: between its tensile and shear strengths; glass: next to nothing)
  const f64 tau = (!M.ductile && M.Gf <= 10.0) ? 0.05e6 : 0.5 * (M.ft + M.cohesion);
  const f64 w1 = (vhi[a1] - vlo[a1] + 1) * h, w2 = (vhi[a2] - vlo[a2] + 1) * h;
  const f64 capacity = 2.0 * (w1 + w2) * (thick * h) * tau;
  if (!(force > capacity)) return false;
  // its fragments in front of the patch, through its thickness
  const i32 dlo = std::min(start, start + s * (kDeep - 1)), dhi = std::max(start, start + s * (kDeep - 1));
  IVec3 blo = vlo, bhi = vhi;
  blo[ax] = dlo;
  bhi[ax] = dhi;
  std::vector<FragKey> out;
  f64 m_out = 0.0;
  V3 at;
  for (i32 cx = blo[0] >> kChunkBits; cx <= bhi[0] >> kChunkBits; ++cx)
    for (i32 cy = blo[1] >> kChunkBits; cy <= bhi[1] >> kChunkBits; ++cy)
      for (i32 cz = blo[2] >> kChunkBits; cz <= bhi[2] >> kChunkBits; ++cz) {
        const IVec3 cc{cx, cy, cz};
        if (!G.chunk(cc)) continue;
        FragChunk& fc = frag_chunk(g, cc);
        const u64 key = key3(cx, cy, cz);
        for (i32 f = 0; f < static_cast<i32>(fc.frags.size()); ++f) {
          const FragInfo& fi = fc.frags[size_t(f)];
          if (fi.count <= 0) continue;
          const V3 c = fi.com * (1.0 / h);  // (lattice voxels)
          bool in = true;
          for (int a = 0; a < 3 && in; ++a) in = c[a] >= blo[a] - 0.5 && c[a] <= bhi[a] + 0.5;
          if (!in) continue;
          out.push_back(FragKey{key, f, g});
          m_out += fi.mass;
          at += (g == 0 ? fi.com : X.to(fi.com)) * fi.mass;
        }
      }
  if (out.empty() || !(m_out > 0.0)) return false;
  at *= 1.0 / m_out;
  // (the wall gave way at its capacity: what the contacts took beyond it in this substep is the
  // body's again; then a plastic collision along the push: the body and what it knocks out go on
  // together)
  const f64 v0 = vn + (force - capacity) * dt / b.mass;
  const f64 v1 = b.mass * v0 / (b.mass + m_out);
  b.v += u * (v1 - vn);
  const V3 vd = b.v;
  for (const FragKey& f : out) {
    FragChunk* fc = frag_chunk_if(f);
    if (!fc || f.idx >= static_cast<i32>(fc->frags.size()) || fc->frags[size_t(f.idx)].count <= 0) continue;
    const FragInfo fi = fc->frags[size_t(f.idx)];
    const u64 hs = mix64(frag_ident_of(f, fi.first) ^ static_cast<u64>(st_.ticks) * 0x9E3779B97F4A7C15ull);
    const V3 spread{(unit01(hs) - 0.5) * 2.0, (unit01(mix64(hs ^ 1)) - 0.5) * 2.0, unit01(mix64(hs ^ 2)) * 1.5};
    const V3 spin{(unit01(mix64(hs ^ 3)) - 0.5) * 8.0, (unit01(mix64(hs ^ 4)) - 0.5) * 8.0, (unit01(mix64(hs ^ 5)) - 0.5) * 8.0};
    if (owner_of(f)) mark_owners_stale(f.grid, f.chunk);
    std::vector<IVec3> vox;
    voxels_of(f, vox);
    tear_fragment(f, vox);
    make_body_from_world({f}, vd + spread, spin);
    std::vector<GVox> gv;
    gv.reserve(vox.size());
    for (const IVec3& p : vox) gv.push_back(GVox{p, f.grid});
    seed_near(gv);
  }
  {
    // (the dust of it)
    const size_t before = events_.size();
    dust_event(at, vd, static_cast<i32>(std::min<f64>(400.0, 0.05 * m_out)), false);
    if (events_.size() > before) events_.back().material = static_cast<int>(mat);
  }
  ++st_.punches;
  rigid_.wake(b);
  return true;
}

void World::Impl::reshape_in_place(Body& b) {
  const V3 com0 = b.com, x0 = b.x;
  const i32 cooldown = b.stress_cooldown;
  b.crumpling = cfg_.body_check_ticks;
  refragment_body(b);
  if (b.count == 0) {
    wake_around(b);
    pw_.pending_retire.push_back(b.id);
    return;
  }
  // (bits that no longer hold on to the rest come off - dust, or pieces of their own - and its
  // largest part stays the piece; too small to be a piece at all, it turns to dust)
  if (b.count < cfg_.min_body_voxels) {
    split_body(b, false, true);
    return;
  }
  rebuild_body_graph(b);
  if (b.graph->components > 1) {
    split_body(b, false, false, nullptr, 0.0, true);
    return;
  }
  refresh_in_place(b, com0, x0);
  pin_reference(*b.graph, b.com);  // (its graph stands: only its centre of mass moved)
  b.stress_cooldown = cooldown;  // (its checks keep their spacing while it crumples: crumple_check_gap)
}

}  // namespace svx
