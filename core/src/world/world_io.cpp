// structvox — the world's design pass (bake), persistence, streaming, output, queries, stats and
// hashes.
#include <algorithm>
#include <functional>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_set>

#include "svx/base/diag.hpp"
#include "svx/base/mem.hpp"
#include "archive.hpp"
#include "svx/base/parallel.hpp"
#include "svx/world/world.hpp"
#include "world_internal.hpp"

namespace svx {

using namespace world_detail;

namespace {
using Clock = std::chrono::steady_clock;
inline f64 ms_since(Clock::time_point t0) { return std::chrono::duration<f64, std::milli>(Clock::now() - t0).count(); }
}  // namespace

// ---------------------------------------------------------------------------------------------
// Design pass

bool World::bake(f64* ms) {
  const auto t0 = Clock::now();
  if (source_) {
    // streamed worlds: their generators build designed structures; chunks stream in lazily
    designed_all_ = true;
    if (ms) *ms = 0.0;
    return true;
  }
  const f64 target = cfg_.design_utilization;
  // every grid's chunks with structure: the world grid's first, then the oriented grids'
  std::vector<GKey> keys;
  for (size_t g = 0; g < grids_.size(); ++g) {
    if (!grids_[g]) continue;
    for (const auto& [k, ch] : vg(static_cast<u16>(g)).chunks())
      if (ch.free_count() > 0) keys.push_back(GKey{static_cast<u16>(g), k});
  }
  std::sort(keys.begin(), keys.end());
  // fragments already designed, by identity (indices shift when floating pieces are removed)
  std::unordered_set<u64> seen;
  auto ident_of = [&](const FragKey& f) -> u64 {
    FragChunk* fc = frag_chunk_if(f);
    return fc ? frag_ident_of(f, fc->frags[size_t(f.idx)].first) : 0;
  };
  auto mark = [&](const FragKey& f) { seen.insert(ident_of(f)); };
  auto is_seen = [&](const FragKey& f) { return seen.count(ident_of(f)) > 0; };
  for (const GKey& key : keys) {
    const IVec3 cc = unkey3(key.chunk);
    for (i32 fi = 0;; ++fi) {
      FragChunk& fc = frag_chunk(key.grid, cc);
      if (fi >= static_cast<i32>(fc.frags.size())) break;
      if (fc.frags[size_t(fi)].count <= 0) continue;
      const FragKey f{key.chunk, fi, key.grid};
      if (is_seen(f)) continue;
      const Chunk* ch0 = vg(key.grid).chunk(cc);
      const u32 ver = ch0 ? ch0->vox_version : 0;
      Structure* s = extract(f, 4000000, 1e9, false);
      static const bool bdbg = diag("SVX_DEBUG_BAKE");
      if (bdbg && s && s->P.nodes.size() > 1000)
        std::printf("  [bake] chunk (%d %d %d) fragment %d: structure of %zu nodes\n", cc[0], cc[1], cc[2], fi, s->P.nodes.size());
      if (!s) {
        // (a floating piece was removed: the chunk's fragments are renumbered, scan it again; the
        // ones seen are skipped by identity)
        const Chunk* ch1 = vg(key.grid).chunk(cc);
        if (!ch1 || ch1->vox_version != ver) fi = -1;
        if (!ch1) break;
        continue;
      }
      for (const FragKey& m : s->frags)
        if (m.idx >= 0) mark(m);
      StressOptions so;
      so.rtol = 1e-6;
      if (!s->P.assemble(so)) {
        drop_structure(s->id);
        continue;
      }
      std::fill(s->ext_solved.begin(), s->ext_solved.end(), 0.0);
      std::vector<f64> F = load_vector(*s);
      add_loads_to(*s, F);  // (a dam is designed for its water)
      static const bool dbg = diag("SVX_DEBUG");
      const auto ts = Clock::now();
      const PcgResult pr = s->P.solve(F, s->u, 1e-6, 4000, true);
      if (dbg) {
        f64 worst = 0.0;
        i32 wb = -1;
        for (i32 b = 0; b < static_cast<i32>(s->P.bonds.size()); ++b) {
          const f64 phi = bond_utilization(s->P.bonds[size_t(b)], s->P.bond_load(b, s->u), 1.0);
          if (phi > worst) {
            worst = phi;
            wb = b;
          }
        }
        std::printf("  bake: structure %lld: %zu nodes, %zu bonds, levels %d, pcg %d its (rel %.2e, %s) %.0f ms, worst phi %.3f",
                    static_cast<long long>(s->id), s->P.nodes.size(), s->P.bonds.size(), 0, pr.iters, pr.rel_res,
                    pr.converged ? "conv" : "NOT conv", ms_since(ts), worst);
        if (wb >= 0) {
          const SBond& B = s->P.bonds[size_t(wb)];
          const BondLoad L = s->P.bond_load(wb, s->u);
          FailMode mode;
          bond_utilization(B, L, 1.0, &mode);
          std::printf(" at (%.2f %.2f %.2f) n (%.2f %.2f %.2f) faces %d area %.4f support %d mode %d N %.0f V %.0f %.0f T %.0f M %.0f %.0f",
                      B.p.x, B.p.y, B.p.z, B.n.x, B.n.y, B.n.z, B.faces, B.area, B.b < 0 ? 1 : 0, static_cast<int>(mode), L.N, L.V1,
                      L.V2, L.T, L.M1, L.M2);
        }
        std::printf("\n");
        // histogram: bonds over 0.45 / 1 by section size
        i32 h45[4] = {0, 0, 0, 0}, h1[4] = {0, 0, 0, 0}, all[4] = {0, 0, 0, 0};
        for (i32 b = 0; b < static_cast<i32>(s->P.bonds.size()); ++b) {
          const SBond& B = s->P.bonds[size_t(b)];
          const int c = B.faces <= 1 ? 0 : (B.faces <= 2 ? 1 : (B.faces <= 5 ? 2 : 3));
          const f64 phi = bond_utilization(B, s->P.bond_load(b, s->u), 1.0);
          ++all[c];
          if (phi > 0.45) ++h45[c];
          if (phi > 1.0) ++h1[c];
        }
        std::printf("    faces 1 / 2 / 3-5 / 6+: bonds %d %d %d %d, over 0.45: %d %d %d %d, over 1: %d %d %d %d\n", all[0], all[1], all[2],
                    all[3], h45[0], h45[1], h45[2], h45[3], h1[0], h1[1], h1[2], h1[3]);
      }
      ++design_.structures;
      design_.nodes += static_cast<i64>(s->P.nodes.size());
      for (i32 b = 0; b < static_cast<i32>(s->P.bonds.size()); ++b) {
        const SBond& B = s->P.bonds[size_t(b)];
        const BondLoad L = s->P.bond_load(b, s->u);
        judged_[s->bid[size_t(b)]] = {node_chunk(*s, B.a), L};
        const f64 phi = bond_utilization(B, L, 1.0);
        design_.max_utilization = std::max(design_.max_utilization, phi);
        if (phi <= target) continue;
        const u8 cls = class_for(B.strength * phi / target);
        for (int side = 0; side < 2; ++side) {
          const i32 nd = side == 0 ? B.a : B.b;
          if (nd < 0) continue;
          design_node(*s, nd, cls, &design_.strengthened_voxels);
        }
      }
      for (size_t i = 0; i < s->P.nodes.size(); ++i) {
        std::array<f32, 6> w;
        for (int q = 0; q < 6; ++q) w[size_t(q)] = static_cast<f32>(s->u[6 * i + q]);
        warm_u_[s->ident[i]] = {node_chunk(*s, static_cast<i32>(i)), w};
      }
      drop_structure(s->id);
    }
  }
  designed_all_ = true;
  st_.design_max_utilization = design_.max_utilization;
  st_.strengthened_voxels = design_.strengthened_voxels;
  st_.floating_voxels = design_.floating_voxels;
  st_.bake_ms = ms_since(t0);
  if (ms) *ms = st_.bake_ms;
  grid_.take_dirty();
  grid_.mark_all_dirty();
  for (size_t g = 1; g < grids_.size(); ++g)
    if (grids_[g]) {
      grids_[g]->g.take_dirty();
      grids_[g]->g.mark_all_dirty();
    }
  return true;
}

void World::design_node(const Structure& s, i32 nd, u8 cls, i64* strengthened) {
  for (i32 k = s.fstart[size_t(nd)]; k < s.fstart[size_t(nd) + 1]; ++k) {
    const FragKey& f = s.frags[size_t(k)];
    if (f.idx < 0) continue;
    std::vector<IVec3> vox;
    voxels_of(f, vox);
    VoxelGrid& G = vg(f.grid);
    for (const IVec3& p : vox)
      if (G.strength(p) < cls) {
        G.set_strength(p, cls);
        ++*strengthened;
      }
  }
}

// ---------------------------------------------------------------------------------------------
// Persistence and streaming

namespace {

// The grids' part of a world delta, after the world grid's records (docs/GRIDS.md §4):
//   magic "SVXG" | version
//   the level's grids that were removed: u32 count, their ids
//   grid entries: u32 count, then per grid (a level's grid that changed or moved: its changed
//     chunks; a grid of this session: all of it)
//       v2: id u32 | flags u8 (1: a level's) | voxel size f64 | priority i32 | body u32 (its
//           kinematic body; 0: the static world) | frame in its body (origin xyz, rot xyzw: 7 f64)
//           | chunk records (u32 count; each: u32 size, the record)
//       v1: id u32 | base u8 | frame (7 f64) | chunk records
//   (v2) kinematic bodies: u32 count, then per body: id u32 | flags u8 (1: a level's) | pose (7
//     f64) | velocity (v, w: 6 f64)
constexpr u32 kGridsMagic = 0x47585653;  // "SVXG"
constexpr u32 kGridsVersion = 2;

void put32(std::vector<u8>& b, u32 v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
}
void put64(std::vector<u8>& b, u64 v) {
  for (int i = 0; i < 8; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
}
void putf(std::vector<u8>& b, f64 x) {
  u64 u;
  std::memcpy(&u, &x, 8);
  put64(b, u);
}
struct Rd {
  const std::vector<u8>& b;
  size_t p = 0;
  bool ok = true;
  bool need(size_t n) {
    if (n > b.size() - p) ok = false;
    return ok;
  }
  u8 u8_() { return need(1) ? b[p++] : 0; }
  u32 u32_() {
    if (!need(4)) return 0;
    u32 v = 0;
    for (int i = 0; i < 4; ++i) v |= u32(b[p++]) << (8 * i);
    return v;
  }
  u64 u64_() {
    if (!need(8)) return 0;
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= u64(b[p++]) << (8 * i);
    return v;
  }
  f64 f64_() {
    const u64 u = u64_();
    f64 x;
    std::memcpy(&x, &u, 8);
    return x;
  }
};

// Where the world grid's records end in a delta (the grids' part begins), or 0 if malformed.
size_t grid_part_end(const std::vector<u8>& bytes) {
  Rd in{bytes};
  in.u32_();
  in.u32_();
  const u32 n = in.u32_();
  for (u32 k = 0; k < n && in.ok; ++k) {
    const u32 sz = in.u32_();
    if (!in.need(sz)) return 0;
    in.p += sz;
  }
  return in.ok ? in.p : 0;
}

struct GridDelta {
  GridId id = 0;
  bool base = true;
  f64 h = 0.0;
  i32 priority = 0;
  u32 body = 0;
  V3 origin;
  Quat rot;
  std::vector<std::vector<u8>> recs;
};

bool finite_q(const Quat& q) { return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w); }

// One grid entry (a malformed one: false).
bool read_grid_entry(Rd& in, u32 version, f64 world_h, GridDelta* d) {
  d->id = in.u32_();
  d->base = (in.u8_() & 1) != 0;
  d->h = world_h;
  if (version >= 2) {
    d->h = in.f64_();
    d->priority = static_cast<i32>(in.u32_());
    d->body = in.u32_();
  }
  d->origin = V3{in.f64_(), in.f64_(), in.f64_()};
  d->rot = Quat{in.f64_(), in.f64_(), in.f64_(), in.f64_()};
  const u32 nc = in.u32_();
  if (!in.ok || d->id == 0 || u64(nc) * 12 > in.b.size()) return false;
  if (!std::isfinite(d->h) || !(d->h >= world_h / 64.0) || !(d->h <= world_h * 64.0)) return false;
  if (!std::isfinite(d->origin.x) || !std::isfinite(d->origin.y) || !std::isfinite(d->origin.z) || !finite_q(d->rot)) return false;
  for (u32 c = 0; c < nc && in.ok; ++c) {
    const u32 sz = in.u32_();
    if (!in.ok || !in.need(sz) || sz < 8) return false;
    std::vector<u8> r(in.b.begin() + static_cast<long>(in.p), in.b.begin() + static_cast<long>(in.p + sz));
    in.p += sz;
    if (!VoxelGrid::check_record(r)) return false;
    d->recs.push_back(std::move(r));
  }
  return in.ok;
}

std::vector<u8> write_grid_entry(const GridDelta& d) {
  std::vector<u8> out;
  put32(out, d.id);
  out.push_back(d.base ? 1 : 0);
  putf(out, d.h);
  put32(out, static_cast<u32>(d.priority));
  put32(out, d.body);
  for (f64 x : {d.origin.x, d.origin.y, d.origin.z, d.rot.x, d.rot.y, d.rot.z, d.rot.w}) putf(out, x);
  put32(out, static_cast<u32>(d.recs.size()));
  for (const auto& r : d.recs) {
    put32(out, static_cast<u32>(r.size()));
    out.insert(out.end(), r.begin(), r.end());
  }
  return out;
}

}  // namespace

std::vector<u8> World::save_delta() const {
  std::vector<u8> out;
  if (!source_) {
    out = grid_.save_delta();
  } else {
    std::vector<u64> keys = grid_.modified_chunks();
    for (u64 k : archive_->keys())
      if (!(k >> 63)) keys.push_back(k);  // (the archive's grid records are the grids' part)
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::vector<std::vector<u8>> recs;
    for (u64 k : keys) recs.push_back(!grid_.is_modified(k) && archive_->has(k) ? archive_->get(k) : grid_.chunk_record(k));
    out = VoxelGrid::pack_delta(recs);
  }
  // the grids' part (only if there is anything: a world of the world grid alone saves as always)
  std::vector<std::vector<u8>> gds;
  for (GridId id : grids()) {
    const u16 g = static_cast<u16>(slot_of(id));
    if (gs(g).base && gs(g).g.modified_chunks().empty() && !gs(g).moved) continue;  // (a level's grid as it was)
    gds.push_back(grid_entry(g));
  }
  const std::vector<u8> kins = kinematic_entries();
  std::vector<GridId> removed = removed_base_;
  std::sort(removed.begin(), removed.end());
  removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
  // (streamed grids archived out of range: their records as they were archived)
  std::vector<std::pair<u64, std::vector<u8>>> archived;
  if (source_)
    for (u64 k : archive_->keys())
      if (k >> 63) archived.push_back({k, archive_->get(k)});
  std::sort(archived.begin(), archived.end());
  if (gds.empty() && removed.empty() && archived.empty() && kins.size() <= 8) return out;
  put32(out, kGridsMagic);
  put32(out, kGridsVersion);
  put32(out, static_cast<u32>(removed.size()));
  for (GridId id : removed) put32(out, id);
  put32(out, static_cast<u32>(gds.size() + archived.size()));
  for (const auto& e : gds) out.insert(out.end(), e.begin(), e.end());
  for (const auto& [k, rec] : archived) out.insert(out.end(), rec.begin(), rec.end());  // (archived: a whole grid entry)
  out.insert(out.end(), kins.begin(), kins.end());
  return out;
}

bool World::load_delta(const std::vector<u8>& bytes) {
  if (in_tick_) return false;  // (from inside a tick: refused)
  std::vector<std::pair<u64, std::vector<u8>>> recs;
  if (!VoxelGrid::unpack_delta(bytes, &recs)) return false;
  for (const auto& [k, r] : recs)  // (all or nothing: every record is checked first)
    if (!VoxelGrid::check_record(r)) return false;
  // the grids' part, checked whole before anything is applied
  std::vector<GridId> removed;
  std::vector<GridDelta> gds;
  std::vector<KinematicId> kremoved;
  std::vector<KinState> kds;
  {
    const size_t end = grid_part_end(bytes);
    if (end == 0) return false;
    if (end < bytes.size()) {
      Rd in{bytes};
      in.p = end;
      if (in.u32_() != kGridsMagic) return false;
      const u32 version = in.u32_();
      if (version < 1 || version > kGridsVersion) return false;
      const u32 nr = in.u32_();
      if (!in.ok || u64(nr) * 4 > bytes.size()) return false;
      for (u32 k = 0; k < nr && in.ok; ++k) removed.push_back(in.u32_());
      const u32 ng = in.u32_();
      if (!in.ok || u64(ng) * 61 > bytes.size()) return false;
      for (u32 k = 0; k < ng && in.ok; ++k) {
        GridDelta d;
        if (!read_grid_entry(in, version, grid_.h, &d) || !in_range(d.origin)) return false;
        gds.push_back(std::move(d));
      }
      if (version >= 2) {
        // the kinematic bodies (kinematic_entries)
        const u32 nkr = in.u32_();
        if (!in.ok || u64(nkr) * 4 > bytes.size()) return false;
        for (u32 k = 0; k < nkr && in.ok; ++k) kremoved.push_back(in.u32_());
        const u32 nk = in.u32_();
        if (!in.ok || u64(nk) * 109 > bytes.size()) return false;
        auto v3 = [&]() { return V3{in.f64_(), in.f64_(), in.f64_()}; };
        auto q4 = [&]() { return Quat{in.f64_(), in.f64_(), in.f64_(), in.f64_()}; };
        for (u32 k = 0; k < nk && in.ok; ++k) {
          KinState d;
          d.id = in.u32_();
          const u8 flags = in.u8_();
          d.base = (flags & 1) != 0;
          d.hold = (flags & 2) != 0;
          d.driven = (flags & 4) != 0;
          d.x = v3();
          d.q = q4();
          d.v = v3();
          d.w = v3();
          if (d.hold) {
            d.cv = v3();
            d.cw = v3();
          }
          if (d.driven) {
            d.tx = v3();
            d.tq = q4();
          }
          const f64 qn = d.q.x * d.q.x + d.q.y * d.q.y + d.q.z * d.q.z + d.q.w * d.q.w;
          if (!in.ok || d.id == 0 || (flags & ~7u) != 0 || !in_range(d.x) || !finite_q(d.q) || !(qn > 0.5 && qn < 2.0) || !finite3(d.v) ||
              !finite3(d.w) || !finite3(d.cv) || !finite3(d.cw) || (d.driven && (!in_range(d.tx) || !finite_q(d.tq))))
            return false;
          kds.push_back(std::move(d));
        }
        for (size_t a = 0; a < kds.size(); ++a)
          for (size_t b2 = a + 1; b2 < kds.size(); ++b2)
            if (kds[a].id == kds[b2].id) return false;
        // (a level's kinematic body must be there: the level made it)
        for (const KinState& d : kds)
          if (d.base && kin_slot_of(d.id) < 0) return false;
      }
      // (a grid's body must be there, or come with the delta)
      for (const GridDelta& d : gds)
        if (d.body != 0 && kin_slot_of(d.body) < 0 &&
            std::none_of(kds.begin(), kds.end(), [&](const KinState& k) { return k.id == d.body; }))
          return false;
      if (!in.ok || in.p != bytes.size()) return false;
      // (a level's grid must be there - the host added the level's grids - unless streamed and
      // not resident: its changes are archived until it comes)
      for (const GridDelta& d : gds)
        if (d.base && slot_of(d.id) < 0 && !source_) return false;
      for (size_t a = 0; a < gds.size(); ++a)
        for (size_t b2 = a + 1; b2 < gds.size(); ++b2)
          if (gds[a].id == gds[b2].id) return false;
    }
  }
  std::vector<GKey> touched;
  // the kinematic bodies first (the grids' bodies): the level's removed ones go (with their
  // grids), the level's others placed as saved, this session's made
  for (KinematicId id : kremoved)
    if (kin_slot_of(id) > 0) remove_kinematic(id, false);
  for (const KinState& d : kds) {
    i32 ks = kin_slot_of(d.id);
    if (ks < 0) {
      next_kin_ = std::max(next_kin_, d.id);
      const KinematicId made = add_kinematic_impl(Pose{d.x, d.q}, false, d.id);
      if (made == 0) continue;
      ks = kin_slot_of(made);
    }
    restore_kinematic(static_cast<u16>(ks), d);
  }
  // then the grids: removed ones go, changed ones take their records, new ones are made
  for (GridId id : removed) {
    const i32 sl = slot_of(id);
    if (sl > 0) {
      removed_base_.push_back(id);
      remove_grid_slot(static_cast<u16>(sl), true);
    }
  }
  for (size_t q = 0; q < gds.size(); ++q) {
    GridDelta& d = gds[q];
    i32 sl = slot_of(d.id);
    if (sl < 0 && d.base) {
      // (a streamed grid not resident: its changes wait in the archive)
      const u64 key = (1ull << 63) | d.id;
      archive_record(key, write_grid_entry(d), 0);
      continue;
    }
    if (sl < 0) {
      // this session's grid: made again, with its id
      VoxelGrid g;
      g.h = d.h;
      next_grid_ = std::max(next_grid_, d.id);
      GridDesc desc;
      desc.frame = GridFrame{d.origin, d.rot};
      desc.voxel_size = d.h;
      desc.priority = d.priority;
      desc.base = false;
      desc.body = d.body;
      const GridId made = add_grid(desc, std::move(g));
      if (made == 0) continue;
      const i32 ms = slot_of(made);
      GridState& st = gs(static_cast<u16>(ms));
      if (made != d.id) {
        slots_.erase(made);
        st.id = d.id;
        slots_[d.id] = static_cast<u16>(ms);
      }
      next_grid_ = std::max(next_grid_, d.id + 1);
      sl = ms;
    } else {
      // a level's grid: where it was when saved (moved since the level placed it)
      const GridState& st = gs(static_cast<u16>(sl));
      const LatticeXf at = LatticeXf::make(d.origin, d.rot);
      if (at.off.x != st.local.off.x || at.off.y != st.local.off.y || at.off.z != st.local.off.z || at.q.x != st.local.q.x ||
          at.q.y != st.local.q.y || at.q.z != st.local.q.z || at.q.w != st.local.q.w)
        set_grid_frame(d.id, GridFrame{d.origin, d.rot});
    }
    VoxelGrid& G = vg(static_cast<u16>(sl));
    for (const auto& r : d.recs) {
      u64 key = 0;
      G.apply_record(r, &key);
      touched.push_back(GKey{static_cast<u16>(sl), key});
      G.note_voxels_modified(key);
    }
    refresh_grid_box(static_cast<u16>(sl));
    grids_changed();
  }
  if (source_) {
    for (auto& [k, r] : recs) {
      if (generated_.count(k)) {
        u64 key = 0;
        const Chunk* c0 = grid_.chunk(unkey3(k));
        const std::vector<Vox> before = c0 && !c0->uniform ? c0->v : std::vector<Vox>{};
        const Vox before_value = c0 && c0->uniform ? c0->value : kAir;
        if (!grid_.apply_record(r, &key)) return false;
        touched.push_back(GKey{0, key});
        // (voxels or bonds changed by the delta: a player's; only layers: not)
        const Chunk* c1 = grid_.chunk(unkey3(key));
        bool same = c1 && c1->broken.empty() && c1->jbroken.empty();
        if (same && !before.empty()) same = !c1->uniform && c1->v == before;
        else if (same) same = c1->uniform ? c1->value == before_value : std::all_of(c1->v.begin(), c1->v.end(), [&](Vox x) { return x == before_value; });
        if (!same) grid_.note_voxels_modified(key);
      } else {
        archive_record(k, r, region_of(k));  // (as if seen now; a bounded archive may forget the oldest to take it)
      }
    }
  } else {
    for (auto& [k, r] : recs) {
      u64 key = 0;
      if (!grid_.apply_record(r, &key)) return false;  // (checked above: never)
      touched.push_back(GKey{0, key});
    }
  }
  // restored chunks: their structures are extracted again when something happens there
  for (const GKey& k : touched) {
    if (!live(k.grid)) continue;
    mark_owners_stale(k.grid, k.chunk);
    const IVec3 cc = unkey3(k.chunk);
    const Chunk* ch = vg(k.grid).chunk(cc);
    if (ch && ch->free_count() > 0) {
      // a restored piece may stand on nothing now: check it
      const IVec3 b{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
      for (int i = 0; i < kChunkVox; i += 97) {
        const IVec3 l = local_of(i);
        const IVec3 p{b[0] + l[0], b[1] + l[1], b[2] + l[2]};
        if (vox_free(vg(k.grid).get(p))) seeds_.push_back(GVox{p, k.grid});
      }
    }
  }
  return true;
}

void World::enable_streaming(std::shared_ptr<const ChunkSource> src, const StreamConfig& sc) {
  source_ = std::move(src);
  stream_ = sc;
  stream_.load_radius = std::isfinite(stream_.load_radius) ? std::clamp(stream_.load_radius, 0.0, 1e5) : 96.0;
  stream_.evict_radius = std::isfinite(stream_.evict_radius) ? std::clamp(stream_.evict_radius, stream_.load_radius, 1e5)
                                                             : stream_.load_radius + 32.0;
  stream_.chunks_per_tick = std::max(1, stream_.chunks_per_tick);
  stream_.archive_mb = std::isfinite(stream_.archive_mb) ? std::clamp(stream_.archive_mb, 0.0, 1e6) : 64.0;
  stream_.forget_after_s = std::isfinite(stream_.forget_after_s) ? std::max(0.0, stream_.forget_after_s) : 0.0;
  generated_.clear();
  column_count_.clear();
  region_resident_.clear();
  // (a bounded archive: its arena, once; an unbounded one grows)
  archive_->reset(source_ ? static_cast<size_t>(stream_.archive_mb * 1048576.0) : 0);
  if (!source_) return;
  const IVec3 lo = source_->chunk_lo(), hi = source_->chunk_hi();
  grid_.lo = {lo[0] * kChunk, lo[1] * kChunk, lo[2] * kChunk};
  grid_.hi = {hi[0] * kChunk, hi[1] * kChunk, hi[2] * kChunk};
}

void World::set_focus(const std::vector<V3>& points) {
  focus_.clear();
  for (const V3& p : points)
    if (in_range(p)) focus_.push_back(p);
  if (!source_ || focus_.empty() || focus_set_) return;
  // the first focus: the world around it at once (a player must not fall through)
  focus_set_ = true;
  const i32 r = static_cast<i32>(std::ceil(24.0 / grid_.h));
  for (const V3& p : focus_) {
    const IVec3 c = voxel_of(p, grid_.h);
    ensure_chunks({c[0] - r, c[1] - r, grid_.lo[2]}, {c[0] + r, c[1] + r, grid_.hi[2]});
  }
}

void World::ensure_resident(const IVec3& lo, const IVec3& hi) {
  if (!source_) return;
  // (bounded: at most the world's extent)
  IVec3 a = lo, b = hi;
  for (int q = 0; q < 3; ++q) {
    a[q] = std::max(a[q], grid_.lo[q]);
    b[q] = std::min(b[q], grid_.hi[q]);
    if (a[q] >= b[q]) return;
  }
  ensure_chunks(a, b);
}

bool World::generate_chunk(u64 key) {
  if (!source_ || generated_.count(key)) return false;
  const IVec3 cc = unkey3(key);
  const IVec3 lo = source_->chunk_lo(), hi = source_->chunk_hi();
  if (cc[0] < lo[0] || cc[1] < lo[1] || cc[2] < lo[2] || cc[0] >= hi[0] || cc[1] >= hi[1] || cc[2] >= hi[2]) return false;
  std::vector<Vox> v;
  bool any = source_->generate(cc, v);
  if (any && v.size() != size_t(kChunkVox)) any = false;  // (a source that did not fill it: air)
  if (any)
    for (Vox& x : v)
      if (!vox_valid(x)) x = kAir;  // (invalid values from the source: air)
  insert_generated(key, any, std::move(v));
  return true;
}

void World::insert_generated(u64 key, bool any, std::vector<Vox>&& v) {
  struct GridsAfter {  // (the source's grids at home here come once the chunk is in)
    World& w;
    u64 k;
    ~GridsAfter() { w.generate_grids(k); }
  } grids_after{*this, key};
  const IVec3 cc = unkey3(key);
  generated_.insert(key);
  sys_generated_.push_back(key);
  ++column_count_[key3(cc[0], cc[1], 0)];
  ++region_resident_[region_of(key)];
  ++st_.generated_total;
  bool changed = false;
  if (any) {
    grid_.insert_chunk(cc, std::move(v));
    changed = true;
  } else {
    grid_.release_buffer(std::move(v));
  }
  // (the source's layers: water of a lake, ...)
  for (int L = 0; L < static_cast<int>(layer_specs_.size()); ++L) {
    std::vector<u8> lv;
    if (!source_->generate_layer(cc, layer_specs_[size_t(L)].name, lv) || lv.size() != size_t(kChunkVox)) continue;
    const IVec3 b{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
    const bool tracked = grid_.tracking();
    grid_.track_changes(false);  // (generated: not a change)
    for (int i = 0; i < kChunkVox; ++i)
      if (lv[size_t(i)]) {
        const IVec3 l = local_of(i);
        grid_.set_layer(L, {b[0] + l[0], b[1] + l[1], b[2] + l[2]}, lv[size_t(i)]);
      }
    grid_.track_changes(tracked);
    changed = true;
  }
  if (archive_->has(key)) {
    // its changes back. Whether they touched its voxels or bonds - or only persistent layers
    // (water, burn marks): then it is the generator's chunk still, to be designed when first
    // touched (a chunk changed by play was designed before it was changed)
    std::vector<Vox> generated;
    if (const Chunk* g0 = grid_.chunk(cc); any && g0 && !g0->uniform) generated = g0->v;
    const Vox generated_value = grid_.chunk(cc) && grid_.chunk(cc)->uniform ? grid_.chunk(cc)->value : kAir;
    grid_.track_changes(true);
    changed = grid_.apply_record(archive_->get(key)) || changed;  // (records were checked when archived or loaded)
    archive_->erase(key);
    const Chunk* ch = grid_.chunk(cc);
    bool same = ch && ch->broken.empty() && ch->jbroken.empty();
    if (same && !generated.empty()) same = !ch->uniform && ch->v == generated;
    else if (same) same = ch->uniform ? ch->value == generated_value : std::all_of(ch->v.begin(), ch->v.end(), [&](Vox x) { return x == generated_value; });
    if (!same) grid_.note_voxels_modified(key);
    else if (ch->free_count() > 0) gs(0).undesigned.insert(key);
  } else if (any) {
    // (fresh from the generator: designed when first touched; an archived chunk was designed
    // before it was changed)
    const Chunk* ch = grid_.chunk(cc);
    if (ch && ch->free_count() > 0) gs(0).undesigned.insert(key);
  }
  // (the static grids resident here that own their overlaps with the world grid displace its
  // voxels: as the level, not a change)
  if (any && oriented_ > 0) {
    const f64 h = grid_.h;
    const V3 clo{h * (cc[0] * kChunk - 0.5), h * (cc[1] * kChunk - 0.5), h * (cc[2] * kChunk - 0.5)};
    const V3 chi{clo.x + h * kChunk, clo.y + h * kChunk, clo.z + h * kChunk};
    const IVec3 vlo{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk}, vhi{vlo[0] + kChunk - 1, vlo[1] + kChunk - 1, vlo[2] + kChunk - 1};
    for (size_t o = 1; o < grids_.size(); ++o) {
      if (!grids_[o] || !grids_[o]->any || grids_[o]->body != 0) continue;
      const GridState& os = *grids_[o];
      if (os.hi.x < clo.x || os.lo.x > chi.x || os.hi.y < clo.y || os.lo.y > chi.y || os.hi.z < clo.z || os.lo.z > chi.z) continue;
      if (owns(static_cast<u16>(o), 0) && displace(static_cast<u16>(o), 0, &vlo, &vhi, false) > 0) changed = true;
    }
  }
  if (!changed) return;
  grid_.mark_dirty(cc);
  for (int d = 0; d < 3; ++d)
    for (int s = -1; s <= 1; s += 2) {
      IVec3 q = cc;
      q[d] += s;
      grid_.mark_dirty(q);
    }
  // structures that held this chunk as a frontier see its fragments now: they reach into it
  // (it is their changed chunk; their nodes in the neighbouring chunks stay as they are)
  for (int d = 0; d < 3; ++d)
    for (int s = -1; s <= 1; s += 2) {
      IVec3 q = cc;
      q[d] += s;
      mark_owners_stale(0, key3(q[0], q[1], q[2]), GKey{0, key});
    }
  // (oriented grids that held this chunk as the unknown world see it now)
  if (oriented_ > 0)
    for (u16 o : near_grids(0, key)) {
      const V3 lo{grid_.h * (cc[0] * kChunk - 1.5), grid_.h * (cc[1] * kChunk - 1.5), grid_.h * (cc[2] * kChunk - 1.5)};
      const V3 hi{grid_.h * ((cc[0] + 1) * kChunk + 0.5), grid_.h * ((cc[1] + 1) * kChunk + 0.5), grid_.h * ((cc[2] + 1) * kChunk + 0.5)};
      const GridState& st = gs(o);
      for (const auto& [k, own] : st.owner) {
        (void)own;
        const IVec3 oc = unkey3(k);
        // (its chunks whose box meets this chunk's: stale, to patch with the world chunk)
        const f64 h = st.g.h;
        V3 a{INFINITY, INFINITY, INFINITY}, b{-INFINITY, -INFINITY, -INFINITY};
        for (int c = 0; c < 8; ++c) {
          const V3 w = st.xf.to(V3{h * ((c & 1) ? (oc[0] + 1) * kChunk : oc[0] * kChunk), h * ((c & 2) ? (oc[1] + 1) * kChunk : oc[1] * kChunk),
                                    h * ((c & 4) ? (oc[2] + 1) * kChunk : oc[2] * kChunk)});
          for (int q = 0; q < 3; ++q) {
            a[q] = std::min(a[q], w[q]);
            b[q] = std::max(b[q], w[q]);
          }
        }
        if (b.x < lo.x || a.x > hi.x || b.y < lo.y || a.y > hi.y || b.z < lo.z || a.z > hi.z) continue;
        mark_owners_stale(o, k, GKey{0, key});
      }
    }
}

void World::generate_grids(u64 key) {
  if (!source_) return;
  for (const SourceGrid& sg : source_->grids(unkey3(key))) {
    if (sg.id == 0 || slot_of(sg.id) >= 0) continue;  // (here already)
    VoxelGrid g;
    g.h = sg.voxel_size > 0.0 ? sg.voxel_size : grid_.h;
    if (!source_->generate_grid(sg.id, g)) continue;
    // (generated: not a change)
    GridDesc d;
    d.frame = GridFrame{sg.origin, sg.rot};
    d.voxel_size = g.h;
    d.priority = sg.priority;
    d.base = true;
    const GridId id = add_grid_impl(d, std::move(g), sg.id, key, false);
    if (id == 0) continue;
    const u16 sl = static_cast<u16>(slot_of(id));
    GridState& st = gs(sl);
    const u64 ak = (1ull << 63) | id;
    if (archive_->has(ak)) {
      // its changes back (a grid changed by play was designed before it was changed)
      std::vector<u64> touched;
      apply_grid_entry(sl, archive_->get(ak), &touched);
      archive_->erase(ak);
    } else {
      // fresh from the source: designed when first touched
      for (const auto& [k, c] : st.g.chunks())
        if (c.free_count() > 0) st.undesigned.insert(k);
    }
  }
}

void World::evict_grid(u16 g, u64 home_region) {
  GridState& st = gs(g);
  if (!st.g.modified_chunks().empty()) archive_record((1ull << 63) | st.id, grid_entry(g), home_region);
  remove_grid_slot(g, true);
}

std::vector<u8> World::grid_entry(u16 g) const {
  const GridState& st = gs(g);
  GridDelta d;
  d.id = st.id;
  d.base = st.base;
  d.h = st.g.h;
  d.priority = st.priority;
  d.body = st.body == 0 ? 0u : kins_[st.body]->id;
  d.origin = st.local.identity ? V3{} : st.local.off;
  d.rot = st.local.identity ? Quat{} : st.local.q;
  std::vector<u64> keys;
  if (st.base) {
    keys = st.g.modified_chunks();
  } else {
    for (const auto& [k, c] : st.g.chunks()) keys.push_back(k);
  }
  std::sort(keys.begin(), keys.end());
  for (u64 k : keys) d.recs.push_back(st.g.chunk_record(k));
  return write_grid_entry(d);
}

bool World::apply_grid_entry(u16 g, const std::vector<u8>& e, std::vector<u64>* touched) {
  Rd in{e};
  GridDelta d;
  if (!read_grid_entry(in, kGridsVersion, grid_.h, &d)) return false;
  VoxelGrid& G = vg(g);
  for (const auto& r : d.recs) {
    u64 key = 0;
    if (!G.apply_record(r, &key)) return false;
    G.note_voxels_modified(key);
    if (touched) touched->push_back(key);
  }
  refresh_grid_box(g);
  // (placed anew while it was out of range: where it was)
  const GridState& st = gs(g);
  const LatticeXf at = LatticeXf::make(d.origin, d.rot);
  if (at.off.x != st.local.off.x || at.off.y != st.local.off.y || at.off.z != st.local.off.z || at.q.x != st.local.q.x ||
      at.q.y != st.local.q.y || at.q.z != st.local.q.z || at.q.w != st.local.q.w)
    set_grid_frame(d.id, GridFrame{d.origin, d.rot});
  return true;
}

void World::evict_chunk(u64 k) {
  const IVec3 cc = unkey3(k);
  // the grids at home in it go with it
  if (const auto ht = home_grids_.find(k); ht != home_grids_.end()) {
    const std::vector<u16> home = ht->second;
    for (u16 g : home)
      if (live(g)) evict_grid(g, region_of(k));
  }
  // registered structures reaching into it are dropped, not patched: extracted again when
  // something happens to them, they are held where they reach into chunks not resident
  if (const auto ot = gs(0).owner.find(k); ot != gs(0).owner.end()) {
    std::vector<i64> ids;
    for (i64 id : ot->second)
      if (id) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (i64 id : ids) drop_structure(id);
  }
  gs(0).undesigned.erase(k);
  const u64 region = region_of(k);
  if (grid_.is_modified(k)) archive_record(k, grid_.chunk_record(k), region);
  sys_evicted_.push_back(k);
  archive_->seen(region, st_.ticks);
  if (const auto rt = region_resident_.find(region); rt != region_resident_.end() && --rt->second <= 0) region_resident_.erase(rt);
  const bool resident = grid_.chunk(cc) != nullptr;
  mark_owners_stale(0, k);
  gs(0).owner.erase(k);
  gs(0).frags.erase(k);
  grid_.remove_chunk(cc);
  generated_.erase(k);
  if (--column_count_[key3(cc[0], cc[1], 0)] <= 0) column_count_.erase(key3(cc[0], cc[1], 0));
  ++st_.evicted_total;
  if (resident) evicted_chunks_.push_back(k);
}

void World::unload_sleepers(const std::vector<u64>& chunks, const std::function<void(const Body&, const std::function<void(u64)>&)>& chunks_of) {
  if (chunks.empty()) return;
  const std::unordered_set<u64> going(chunks.begin(), chunks.end());
  std::vector<i64> ids;
  for (const auto& bp : rigid_.bodies) {
    if (!bp->asleep) continue;
    bool hit = false;
    chunks_of(*bp, [&](u64 k) { hit = hit || going.count(k) > 0; });
    if (hit) ids.push_back(bp->id);
  }
  remove_bodies(std::move(ids), PieceEnd::Unloaded);
}

u64 World::region_of(u64 chunk_key) const { return source_ ? source_->region(unkey3(chunk_key)) : 0; }

void World::archive_record(u64 key, const std::vector<u8>& rec, u64 region) {
  if (!archive_->fits(rec.size())) forget_regions(rec.size(), region);
  if (!archive_->put(key, region, rec, st_.ticks)) {
    // (no region to forget but this chunk's own, which is partly resident: its changes go)
    ++st_.forgotten_chunks;
  }
}

bool World::forget_regions(size_t need, u64 keep) {
  // the regions out of range, least recently seen first (ties: by key)
  std::vector<std::pair<i64, u64>> cand;
  for (const auto& [r, info] : archive_->regions())
    if (r != keep && !region_resident_.count(r)) cand.push_back({info.seen, r});
  std::sort(cand.begin(), cand.end());
  for (const auto& [seen, r] : cand) {
    if (archive_->fits(need)) break;
    forget_region(r);
  }
  return archive_->fits(need);
}

void World::forget_region(u64 region) {
  const std::vector<u64> keys = archive_->forget(region);
  if (keys.empty()) return;
  ++st_.forgotten_regions;
  st_.forgotten_chunks += static_cast<i64>(keys.size());
  // (the region's chunks come back from the source as generated, designed when first touched)
  WorldEvent ev;
  ev.kind = WorldEvent::Kind::Forgotten;
  ev.id = static_cast<i64>(region);
  V3 c;
  i32 nc = 0;
  for (u64 k : keys) {
    if (k >> 63) continue;  // (a grid's record)
    const IVec3 cc = unkey3(k);
    c += V3{(cc[0] + 0.5) * kChunk * grid_.h, (cc[1] + 0.5) * kChunk * grid_.h, (cc[2] + 0.5) * kChunk * grid_.h};
    ++nc;
  }
  ev.pos = nc ? c * (1.0 / static_cast<f64>(nc)) : c;
  ev.voxels = static_cast<i32>(keys.size());
  events_.push_back(ev);
}

void World::forget_stale_regions() {
  if (stream_.forget_after_s <= 0.0) return;
  const i64 ttl = static_cast<i64>(std::ceil(stream_.forget_after_s / cfg_.dt));
  std::vector<u64> stale;
  for (const auto& [r, info] : archive_->regions())
    if (!region_resident_.count(r) && st_.ticks - info.seen > ttl) stale.push_back(r);
  std::sort(stale.begin(), stale.end());
  for (u64 r : stale) forget_region(r);
}

void World::ensure_chunks(const IVec3& vlo, const IVec3& vhi) {
  if (!source_) return;
  for (i32 x = vlo[0] >> kChunkBits; x <= (vhi[0] - 1) >> kChunkBits; ++x)
    for (i32 y = vlo[1] >> kChunkBits; y <= (vhi[1] - 1) >> kChunkBits; ++y)
      for (i32 z = vlo[2] >> kChunkBits; z <= (vhi[2] - 1) >> kChunkBits; ++z) generate_chunk(key3(x, y, z));
}

f64 World::focus_distance(const IVec3& cc) const {
  f64 best = INFINITY;
  const f64 cx = (cc[0] + 0.5) * kChunk * grid_.h - 0.5 * grid_.h, cy = (cc[1] + 0.5) * kChunk * grid_.h - 0.5 * grid_.h;
  for (const V3& f : focus_) {
    const f64 dx = cx - f.x, dy = cy - f.y;
    best = std::min(best, std::sqrt(dx * dx + dy * dy));
  }
  return best;
}

bool World::chunk_resident(const IVec3& cc) const {
  if (!source_) return true;
  const IVec3 lo = source_->chunk_lo(), hi = source_->chunk_hi();
  for (int q = 0; q < 3; ++q)
    if (cc[q] < lo[q] || cc[q] >= hi[q]) return true;  // outside the world: air
  return generated_.count(key3(cc[0], cc[1], cc[2])) > 0;
}

int World::stream_update() {
  if (!source_ || focus_.empty()) return 0;  // (no focus yet: nothing is loaded or evicted)
  int generated = 0;
  const auto t0 = Clock::now();
  const f64 cs = grid_.h * kChunk;
  const IVec3 lo = source_->chunk_lo(), hi = source_->chunk_hi();
  auto hdist = [&](const IVec3& cc) { return focus_distance(cc); };
  const i32 rl = static_cast<i32>(std::ceil(stream_.load_radius / cs)) + 1;
  std::vector<std::pair<f64, u64>> want;
  std::unordered_set<u64> columns;
  for (const V3& f : focus_) {
    const i32 cx = static_cast<i32>(std::floor((f.x / grid_.h + 0.5) / kChunk));
    const i32 cy = static_cast<i32>(std::floor((f.y / grid_.h + 0.5) / kChunk));
    for (i32 x = std::max(lo[0], cx - rl); x <= std::min(hi[0] - 1, cx + rl); ++x)
      for (i32 y = std::max(lo[1], cy - rl); y <= std::min(hi[1] - 1, cy + rl); ++y) {
        if (!columns.insert(key3(x, y, 0)).second) continue;
        const f64 d = hdist({x, y, 0});
        if (d > stream_.load_radius) continue;
        const auto cit = column_count_.find(key3(x, y, 0));
        if (cit != column_count_.end() && cit->second >= hi[2] - lo[2]) continue;
        for (i32 z = lo[2]; z < hi[2]; ++z) {
          const u64 k = key3(x, y, z);
          if (!generated_.count(k)) want.push_back({d, k});
        }
      }
  }
  std::sort(want.begin(), want.end());
  int budget = stream_.chunks_per_tick, empty_budget = 16 * stream_.chunks_per_tick;
  std::vector<std::vector<Vox>> vox;
  std::vector<u8> any;
  for (size_t i = 0; i < want.size() && budget > 0 && empty_budget > 0;) {
    const size_t n = std::min(want.size() - i, size_t(2 * budget + 8));
    vox.resize(n);
    for (auto& v : vox)
      if (v.capacity() < size_t(kChunkVox)) v = grid_.acquire_buffer(kAir);  // (recycled arrays)
    any.assign(n, 0);
    const ChunkSource* src = source_.get();
    parallel_for(static_cast<i64>(n), 1, [&](i64 b0, i64 e0) {
      for (i64 j = b0; j < e0; ++j) any[size_t(j)] = src->generate(unkey3(want[i + size_t(j)].second), vox[size_t(j)]) ? 1 : 0;
    });
    for (size_t j = 0; j < n && budget > 0 && empty_budget > 0; ++j) {
      const u64 k = want[i + j].second;
      insert_generated(k, any[j] != 0, std::move(vox[j]));
      vox[j] = {};
      ++generated;
      if (grid_.chunk(unkey3(k))) --budget;
      else --empty_budget;
    }
    i += n;
  }
  for (auto& v : vox)
    if (v.capacity() > 0) grid_.release_buffer(std::move(v));  // (generated beyond the budget: dropped)
  // evict far from the focus points (never under a piece, which would fall through, or a
  // structure being solved)
  // (a moving piece keeps the chunks around it; sleeping rubble does not: it is unloaded with
  // them, or it would keep regions it lies in resident, and their changes remembered, forever)
  auto chunks_of_body = [&](const Body& bd, const std::function<void(u64)>& f) {
    const IVec3 a = voxel_of(bd.box_lo, grid_.h), b = voxel_of(bd.box_hi, grid_.h);
    for (i32 x = (a[0] >> kChunkBits) - 1; x <= (b[0] >> kChunkBits) + 1; ++x)
      for (i32 y = (a[1] >> kChunkBits) - 1; y <= (b[1] >> kChunkBits) + 1; ++y)
        for (i32 z = (a[2] >> kChunkBits) - 1; z <= (b[2] >> kChunkBits) + 1; ++z) f(key3(x, y, z));
  };
  std::unordered_set<u64> busy;
  for (const auto& bp : rigid_.bodies)
    if (!bp->asleep) chunks_of_body(*bp, [&](u64 k) { busy.insert(k); });
  for (const auto& s : structures_)
    if (s->solving)
      for (const FragKey& f : s->frags) {
        if (f.idx < 0) continue;
        if (f.grid == 0) busy.insert(f.chunk);
        else if (live(f.grid) && gs(f.grid).home != ~0ull) busy.insert(gs(f.grid).home);
      }
  // (a streamed grid stays while a piece moves over it)
  for (size_t g = 1; g < grids_.size(); ++g) {
    if (!grids_[g] || grids_[g]->home == ~0ull || !grids_[g]->any) continue;
    const GridState& st = *grids_[g];
    for (const auto& bp : rigid_.bodies)
      if (!bp->asleep && !(bp->box_hi.x < st.lo.x || bp->box_lo.x > st.hi.x || bp->box_hi.y < st.lo.y || bp->box_lo.y > st.hi.y ||
                           bp->box_hi.z < st.lo.z || bp->box_lo.z > st.hi.z)) {
        busy.insert(st.home);
        break;
      }
  }
  std::vector<u64> keys(generated_.begin(), generated_.end());
  std::sort(keys.begin(), keys.end());
  std::vector<u64> out;
  for (u64 k : keys) {
    const IVec3 cc = unkey3(k);
    if (hdist(cc) <= stream_.evict_radius || busy.count(k)) continue;
    out.push_back(k);
  }
  unload_sleepers(out, chunks_of_body);
  for (u64 k : out) evict_chunk(k);
  if (stream_.max_resident_mb > 0.0 && st_.ticks % 30 == 0) {
    const i64 budget_b = static_cast<i64>(stream_.max_resident_mb * 1048576.0);
    i64 bytes = grid_.memory_bytes();
    if (bytes > budget_b) {
      std::vector<std::pair<f64, u64>> far;
      for (u64 k : generated_) {
        const IVec3 cc = unkey3(k);
        if (!grid_.chunk(cc)) continue;
        const f64 d = hdist(cc);
        if (d > stream_.load_radius && !busy.count(k)) far.push_back({-d, k});
      }
      std::sort(far.begin(), far.end());
      std::vector<u64> go;
      for (const auto& [nd, k] : far) {
        if (bytes <= budget_b) break;
        const Chunk* ch = grid_.chunk(unkey3(k));
        bytes -= ch ? static_cast<i64>(sizeof(Chunk) + ch->v.size() + ch->broken.size() + ch->strength.size()) : 0;
        go.push_back(k);
      }
      unload_sleepers(go, chunks_of_body);
      for (u64 k : go) {
        evict_chunk(k);
        ++st_.budget_evicted;
      }
    }
  }
  if (st_.ticks % 60 == 0) forget_stale_regions();
  st_.stream_ms = ms_since(t0);
  return generated;
}

void World::add_loads_to(const Structure& s, std::vector<f64>& F) const {
  for (const auto& [group, list] : loads_)
    for (const VoxelLoad& l : list) {
      const i32 g = slot_of(l.grid);
      if (g < 0) continue;
      const IVec3 cc = chunk_of(l.voxel);
      const u64 k = key3(cc[0], cc[1], cc[2]);
      const auto& frags = gs(static_cast<u16>(g)).frags;
      const auto ft = frags.find(k);
      if (ft == frags.end()) continue;
      const i32 f = ft->second.at(chunk_index(l.voxel));
      if (f < 0) continue;
      const i32 i = s.node(FragKey{k, f, static_cast<u16>(g)});
      if (i < 0) continue;
      V3 p = voxel_centre(GVox{l.voxel, static_cast<u16>(g)}), Fl = l.force;
      to_body(s.body, &Fl, &p);
      const V3 M = cross(p - s.P.nodes[size_t(i)].c, Fl);
      f64* a = &F[6 * size_t(i)];
      a[0] += Fl.x;
      a[1] += Fl.y;
      a[2] += Fl.z;
      a[3] += M.x;
      a[4] += M.y;
      a[5] += M.z;
    }
}

bool World::modified() const {
  if (!grid_.modified_chunks().empty() || archive_->size() > 0 || !removed_base_.empty()) return true;
  for (size_t g = 1; g < grids_.size(); ++g)
    if (grids_[g] && (!grids_[g]->base || !grids_[g]->g.modified_chunks().empty())) return true;
  return false;
}

// ---------------------------------------------------------------------------------------------
// Output

std::vector<u64> World::take_changed_chunks() {
  finish_tick_changes();  // (changes since the last tick: the systems hear of them at the next)
  std::vector<u64> keys;
  if (host_dirty_all_) {
    for (const auto& [k, c] : grid_.chunks()) keys.push_back(k);
    host_dirty_all_ = false;
  } else {
    keys.assign(host_dirty_.begin(), host_dirty_.end());
  }
  std::unordered_set<u64>().swap(host_dirty_);
  std::sort(keys.begin(), keys.end());
  return keys;
}

std::vector<GridChunk> World::take_changed_grid_chunks() {
  finish_tick_changes();
  std::vector<GridChunk> out;
  out.swap(grid_dirty_);
  auto key = [](const GridChunk& c) { return std::make_pair(c.grid, key3(c.chunk[0], c.chunk[1], c.chunk[2])); };
  std::sort(out.begin(), out.end(), [&](const GridChunk& a, const GridChunk& b) { return key(a) < key(b); });
  out.erase(std::unique(out.begin(), out.end(), [&](const GridChunk& a, const GridChunk& b) { return key(a) == key(b); }), out.end());
  // (chunks of grids gone since: the host dropped the grid)
  out.erase(std::remove_if(out.begin(), out.end(), [&](const GridChunk& c) { return slot_of(c.grid) < 0; }), out.end());
  return out;
}

std::vector<u64> World::take_evicted_chunks() {
  std::vector<u64> out;
  out.swap(evicted_chunks_);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool World::debug_field(GridId grid, const IVec3& cc, DebugField field, std::vector<u8>* out) {
  const i32 gi = slot_of(grid);
  if (gi < 0) {
    out->assign(kChunkVox, 0);
    return false;
  }
  const u16 g = static_cast<u16>(gi);
  const u64 k = key3(cc[0], cc[1], cc[2]);
  const Chunk* ch = vg(g).chunk(cc);
  FragChunk* fcp = frag_chunk_if(g, k);
  // (a chunk whose fragments are not current reads as none: the diagnostics never rebuild
  // them, which would change when the structures learn of changes)
  if (!ch || !fcp || fcp->vox_version != ch->vox_version) {
    out->assign(kChunkVox, 0);
    return ch != nullptr;
  }
  FragChunk& fc = *fcp;
  // per fragment of the chunk, then per voxel
  std::vector<u8> v(fc.frags.size(), 0);
  std::unordered_map<i64, std::vector<f32>> node_phi;
  for (i32 f = 0; f < static_cast<i32>(fc.frags.size()); ++f) {
    if (field == DebugField::Fragment) {
      v[size_t(f)] = static_cast<u8>(1 + mix64(frag_ident_of(FragKey{k, f, g}, fc.frags[size_t(f)].first)) % 254);
      continue;
    }
    Structure* s = structure(owner_of(FragKey{k, f, g}));
    if (!s || s->phi.size() != s->P.bonds.size()) continue;
    auto it = node_phi.find(s->id);
    if (it == node_phi.end()) {
      std::vector<f32> np(s->P.nodes.size(), 0.0f);
      for (size_t b = 0; b < s->P.bonds.size(); ++b) {
        const SBond& B = s->P.bonds[b];
        np[size_t(B.a)] = std::max(np[size_t(B.a)], s->phi[b]);
        if (B.b >= 0) np[size_t(B.b)] = std::max(np[size_t(B.b)], s->phi[b]);
      }
      it = node_phi.emplace(s->id, std::move(np)).first;
    }
    const i32 nd = s->node(FragKey{k, f, g});
    if (nd >= 0) v[size_t(f)] = static_cast<u8>(std::lround(255.0 * std::clamp(static_cast<f64>(it->second[size_t(nd)]), 0.0, 1.0)));
  }
  out->assign(kChunkVox, 0);
  for (int i = 0; i < kChunkVox; ++i) {
    const i32 f = fc.at(i);
    if (f >= 0 && f < static_cast<i32>(v.size())) (*out)[size_t(i)] = v[size_t(f)];
  }
  return true;
}

std::vector<WorldEvent> World::take_events() {
  std::vector<WorldEvent> out;
  out.swap(events_);
  return out;
}

WorldStats World::stats() const {
  WorldStats s = st_;
  s.voxels = grid_.solid_count();
  s.chunks = static_cast<i64>(grid_.chunks().size());
  i64 mem = grid_.memory_bytes();
  for (size_t g = 1; g < grids_.size(); ++g)
    if (grids_[g]) {
      s.voxels += grids_[g]->g.solid_count();
      s.chunks += static_cast<i64>(grids_[g]->g.chunks().size());
      mem += grids_[g]->g.memory_bytes();
    }
  s.grids = oriented_;
  s.memory_mb = f64(mem) / (1024.0 * 1024.0);
  s.structures = static_cast<i32>(structures_.size());
  s.resident_chunks = static_cast<i64>(generated_.size());
  s.archived_chunks = static_cast<i64>(archive_->size());
  s.archive_used_mb = static_cast<f64>(archive_->used_bytes()) / 1048576.0;
  s.archive_capacity_mb = static_cast<f64>(archive_->capacity()) / 1048576.0;
  s.bodies = static_cast<i32>(rigid_.bodies.size());
  s.awake = rigid_.awake_count();
  s.contacts = static_cast<i32>(rigid_.contacts().size());
  return s;
}

MemoryReport World::memory() const {
  MemoryReport m;
  m.grid = grid_.memory_bytes() + hash_bytes(grid_.chunks()) + grid_.bookkeeping_bytes();
  m.chunks = static_cast<i32>(grid_.chunks().size());
  for (size_t g = 0; g < grids_.size(); ++g) {
    if (!grids_[g]) continue;
    const GridState& st = *grids_[g];
    if (g > 0) {
      m.grid += st.g.memory_bytes() + hash_bytes(st.g.chunks()) + st.g.bookkeeping_bytes() + static_cast<i64>(sizeof(GridState));
      m.chunks += static_cast<i32>(st.g.chunks().size());
    }
    for (const auto& [k, fc] : st.frags) m.fragments += fc.memory_bytes();
    m.fragments += hash_bytes(st.frags);
    for (const auto& [k, o] : st.owner) m.fragments += vec_bytes(o);
    m.fragments += hash_bytes(st.owner);
    for (const auto& [k, v] : st.near) m.caches += vec_bytes(v);
    m.caches += hash_bytes(st.near) + hash_bytes(st.undesigned);
    m.fragment_chunks += static_cast<i32>(st.frags.size());
  }
  for (const auto& sp : structures_) m.structures += structure_bytes(*sp);
  m.structure_count = static_cast<i32>(structures_.size());
  for (const auto& bp : rigid_.bodies) m.pieces += body_bytes(*bp);
  m.pieces += vec_bytes(rigid_.contacts()) + static_cast<i64>(rigid_.bodies.capacity() * sizeof(void*));
  m.piece_count = static_cast<i32>(rigid_.bodies.size());
  m.archive = archive_->memory_bytes() + hash_bytes(region_resident_);
  m.archived_chunks = static_cast<i32>(archive_->size());
  m.caches = hash_bytes(warm_u_) + hash_bytes(judged_) + hash_bytes(dead_loads_);
  for (const auto& [id, l] : dead_loads_) m.caches += vec_bytes(l);
  m.caches += hash_bytes(generated_) + hash_bytes(column_count_) + hash_bytes(home_grids_);
  m.queues = vec_bytes(events_) + vec_bytes(evicted_chunks_) + vec_bytes(queue_) + vec_bytes(seeds_) + grid_.dirty_bytes() + vec_bytes(grid_dirty_) +
             hash_bytes(host_dirty_) + vec_bytes(sys_changed_) + vec_bytes(sys_generated_) + vec_bytes(sys_evicted_);
  for (const auto& [g, l] : loads_) m.caches += vec_bytes(l) + 48;
  for (const auto& sys : systems_) m.systems += sys->memory_bytes();
  return m;
}

u64 World::state_hash() const {
  // (the content, whatever its representation: a uniform chunk and a mixed one of equal voxels
  // hash alike, an all-air chunk like no chunk at all)
  u64 hsh = 1469598103934665603ull;
  auto mix = [&](u64 v) { hsh = (hsh ^ v) * 1099511628211ull; };
  auto hash_grid = [&](const VoxelGrid& G) {
    std::vector<u64> keys;
    for (const auto& [k, c] : G.chunks()) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    for (u64 k : keys) {
      const Chunk& c = G.chunks().at(k);
      bool uni = c.uniform;
      Vox v0 = c.uniform ? c.value : (c.v.empty() ? kAir : c.v[0]);
      if (!uni) uni = std::all_of(c.v.begin(), c.v.end(), [&](Vox v) { return v == v0; });
      const bool any_broken = std::any_of(c.broken.begin(), c.broken.end(), [](u8 b) { return b != 0; });
      if (uni && v0 == kAir && !any_broken && !c.has_layers() && c.jbroken.empty()) continue;
      mix(k);
      if (uni) {
        mix(v0);
      } else {
        for (Vox v : c.v) mix(v);
      }
      if (any_broken)
        for (u8 b : c.broken) mix(b);
      for (int L = 0; L < kMaxLayers; ++L)
        if (!c.layer[size_t(L)].empty()) {
          mix(0x1A7E0000ull + static_cast<u64>(L));
          for (u8 v : c.layer[size_t(L)]) mix(v);
        }
      if (!c.jbroken.empty()) {
        mix(0x7B0E0000ull + c.jbroken.size());
        for (u32 j : c.jbroken) mix(j);
      }
    }
  };
  hash_grid(grid_);
  // the oriented grids (a world of the world grid alone hashes as it always did)
  auto bits = [](f64 x) {
    u64 u;
    std::memcpy(&u, &x, sizeof u);
    return u;
  };
  for (GridId id : grids()) {
    const GridState& st = gs(static_cast<u16>(slot_of(id)));
    mix(0x6F7269656E746564ull ^ id);
    for (f64 x : {st.xf.off.x, st.xf.off.y, st.xf.off.z, st.xf.q.x, st.xf.q.y, st.xf.q.z, st.xf.q.w}) mix(bits(x));
    if (st.g.h != grid_.h) mix(bits(st.g.h));
    if (st.priority != 0) mix(0x5052494Full ^ static_cast<u64>(static_cast<u32>(st.priority)));
    if (st.body != 0) mix(0x424F4459ull ^ kins_[st.body]->id);
    hash_grid(st.g);
  }
  std::vector<GridId> removed = removed_base_;
  std::sort(removed.begin(), removed.end());
  for (GridId id : removed) mix(0x52454D4F564544ull ^ id);
  // the kinematic bodies: where they are and how they move
  for (KinematicId id : kinematics()) {
    const KinState& K = *kins_[size_t(kin_slot_of(id))];
    mix(0x4B494E4Eull ^ id);
    for (f64 x : {K.x.x, K.x.y, K.x.z, K.q.x, K.q.y, K.q.z, K.q.w, K.v.x, K.v.y, K.v.z, K.w.x, K.w.y, K.w.z}) mix(bits(x));
  }
  return hsh;
}

u64 World::session_hash() const {
  u64 hsh = state_hash();
  auto mix = [&](u64 v) { hsh = (hsh ^ v) * 1099511628211ull; };
  auto bits = [](f64 x) {
    u64 u;
    std::memcpy(&u, &x, sizeof u);
    return u;
  };
  for (const auto& bp : rigid_.bodies) {
    const Body& b = *bp;
    mix(static_cast<u64>(b.id));
    mix(bits(b.x.x));
    mix(bits(b.x.y));
    mix(bits(b.x.z));
    mix(bits(b.q.x));
    mix(bits(b.q.y));
    mix(bits(b.q.z));
    mix(bits(b.q.w));
  }
  // the joints: which there are, what their ends hold on to, what they carry
  for (size_t k = 0; k < rigid_.joints.size(); ++k) {
    const Joint& j = rigid_.joints[k];
    mix(0x4A4F494Eull ^ j.id);
    mix(static_cast<u64>(jrecs_[k].a.piece));
    mix(static_cast<u64>(jrecs_[k].b.piece));
    mix(bits(j.force.x));
    mix(bits(j.force.y));
    mix(bits(j.force.z));
    mix(bits(j.value));
  }
  for (const auto& sys : systems_) mix(sys->state_hash());
  return hsh;
}

// ---------------------------------------------------------------------------------------------
// Queries

namespace {

// Amanatides-Woo walk over unit voxels from o (voxel units, voxel p spans p - 1/2 .. p + 1/2)
// along unit d up to tlim; calls solid(p) per voxel. Returns t and the entry face axis.
template <class Solid>
bool dda(const V3& o_m, const V3& d, f64 h, f64 max_dist, Solid&& solid, f64* t_out, int* face_out, int step_out[3],
         IVec3* vox_out) {
  f64 o[3] = {o_m.x / h + 0.5, o_m.y / h + 0.5, o_m.z / h + 0.5};
  const f64 dd[3] = {d.x, d.y, d.z};
  i32 v[3];
  int step[3];
  f64 tmax[3], tdelta[3];
  for (int a = 0; a < 3; ++a) {
    v[a] = static_cast<i32>(std::floor(o[a]));
    step[a] = dd[a] > 0 ? 1 : (dd[a] < 0 ? -1 : 0);
    if (step[a] != 0) {
      const f64 next = step[a] > 0 ? (v[a] + 1 - o[a]) : (o[a] - v[a]);
      tdelta[a] = 1.0 / std::abs(dd[a]);
      tmax[a] = next * tdelta[a];
    } else {
      tdelta[a] = tmax[a] = INFINITY;
    }
  }
  const f64 tlim = max_dist / h;
  int face = -1;
  f64 t = 0.0;
  for (int it = 0; it < 100000 && t <= tlim; ++it) {
    if (solid(IVec3{v[0], v[1], v[2]})) {
      *t_out = t * h;
      *face_out = face;
      for (int a = 0; a < 3; ++a) step_out[a] = step[a];
      *vox_out = {v[0], v[1], v[2]};
      return true;
    }
    const int a = (tmax[0] < tmax[1]) ? (tmax[0] < tmax[2] ? 0 : 2) : (tmax[1] < tmax[2] ? 1 : 2);
    t = tmax[a];
    tmax[a] += tdelta[a];
    v[a] += step[a];
    face = a;
  }
  return false;
}

}  // namespace

namespace {

// The ray o + t d (unit d) against a box: its [t0, t1] inside, clipped to [0, tmax].
bool clip_box(const V3& o, const V3& d, const V3& lo, const V3& hi, f64 tmax, f64* t0, f64* t1) {
  f64 a = 0.0, b = tmax;
  for (int q = 0; q < 3; ++q) {
    if (std::abs(d[q]) < 1e-300) {
      if (o[q] < lo[q] || o[q] > hi[q]) return false;
      continue;
    }
    f64 u = (lo[q] - o[q]) / d[q], v = (hi[q] - o[q]) / d[q];
    if (u > v) std::swap(u, v);
    a = std::max(a, u);
    b = std::min(b, v);
    if (a > b) return false;
  }
  *t0 = a;
  *t1 = b;
  return true;
}

// Swept separating axes: when the box (centre ca, half extents ea) moving by V over t in [0, 1]
// first touches the cube (centre cb, axes u, half side hb). False: it does not within the move,
// or they overlap already (a box moves out of what it is in). n: the touched face's normal,
// pointing at the box.
bool sweep_box_cube(const V3& ca, const V3& ea, const V3& V, const V3& cb, const V3 u[3], f64 hb, f64* t, V3* n) {
  const V3 e[3] = {V3{1, 0, 0}, V3{0, 1, 0}, V3{0, 0, 1}};
  V3 axes[15];
  int na = 0;
  for (int i = 0; i < 3; ++i) axes[na++] = e[i];
  for (int i = 0; i < 3; ++i) axes[na++] = u[i];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      const V3 c = cross(e[i], u[j]);
      if (norm2(c) > 1e-12) axes[na++] = c;
    }
  f64 tin = -INFINITY, tout = INFINITY;
  V3 nin;
  const V3 D = ca - cb;
  for (int k = 0; k < na; ++k) {
    const V3& L = axes[k];
    const f64 R = ea.x * std::abs(L.x) + ea.y * std::abs(L.y) + ea.z * std::abs(L.z) +
                  hb * (std::abs(dot(L, u[0])) + std::abs(dot(L, u[1])) + std::abs(dot(L, u[2])));
    const f64 d0 = dot(L, D), vl = dot(L, V);
    if (std::abs(vl) < 1e-15) {
      if (std::abs(d0) > R) return false;
      continue;
    }
    f64 a = (-R - d0) / vl, b = (R - d0) / vl;
    if (a > b) std::swap(a, b);
    if (a > tin) {
      tin = a;
      nin = normalized(L) * (d0 >= 0.0 ? 1.0 : -1.0);
    }
    tout = std::min(tout, b);
    if (tin > tout) return false;
  }
  if (tin > 1.0 || tout < 0.0 || tin < -1e-9) return false;
  *t = std::max(0.0, tin);
  *n = nin;
  return true;
}

}  // namespace

RayHit World::raycast(const V3& o, const V3& dir, f64 max_dist) const {
  RayHit hit;
  const f64 h = grid_.h;
  V3 d = dir;
  if (!in_range(o) || !finite3(d) || !(max_dist > 0.0)) return hit;
  max_dist = std::min(max_dist, 1e4);
  const f64 len = norm(d);
  if (len <= 0.0) return hit;
  d *= 1.0 / len;
  f64 t;
  int face, step[3];
  IVec3 v;
  if (dda(o, d, h, max_dist, [&](const IVec3& p) { return vox_solid(grid_.get(p)); }, &t, &face, step, &v)) {
    hit.hit = true;
    hit.distance = t;
    hit.pos = o + d * t;
    hit.normal = V3{0, 0, 0};
    if (face >= 0) hit.normal[face] = -step[face];
    hit.material = static_cast<int>(vox_mat(grid_.get(v)));
    hit.voxel = v;
  }
  const f64 best = hit.hit ? hit.distance : max_dist;
  f64 bt = best;
  // oriented grids: the ray in each grid's lattice, within its box
  for (size_t g = 1; g < grids_.size(); ++g) {
    if (!grids_[g] || !grids_[g]->any) continue;
    const GridState& st = *grids_[g];
    const f64 hg = st.g.h;
    const V3 m{hg, hg, hg};
    f64 t0, t1;
    if (!clip_box(o, d, st.lo - m, st.hi + m, bt, &t0, &t1)) continue;
    const V3 ol = st.xf.from(o + d * t0);
    const V3 dl = st.xf.dir_from(d);
    if (dda(ol, dl, hg, t1 - t0 + hg, [&](const IVec3& p) { return vox_solid(st.g.get(p)); }, &t, &face, step, &v)) {
      const f64 tt = t0 + t;
      if (tt < bt) {
        bt = tt;
        hit.hit = true;
        hit.distance = tt;
        hit.pos = o + d * tt;
        V3 n{0, 0, 0};
        if (face >= 0) n[face] = -step[face];
        hit.normal = st.xf.dir_to(n);
        hit.material = static_cast<int>(vox_mat(st.g.get(v)));
        hit.voxel = v;
        hit.piece = 0;
        hit.grid = st.id;
        hit.shape = 0;
      }
    }
  }
  // rigid pieces: the ray in each piece's shapes' lattices
  for (const auto& bp : rigid_.bodies) {
    const Body& b = *bp;
    const V3 oc = b.x - o;
    const f64 tc = dot(oc, d);
    const f64 r = b.radius + h;
    if (norm2(oc) - tc * tc > r * r || tc + r < 0.0 || tc - r > bt) continue;
    const V3 os = b.to_shape(o);
    const V3 ds = rotate_inv(b.q, d);
    // start near the piece (the walk is bounded)
    const f64 t0 = std::max(0.0, tc - r);
    for (size_t k = 0; k < b.shapes.size(); ++k) {
      const BodyShape& S = b.shapes[k];
      const V3 ol = S.xf.from(os + ds * t0);
      const V3 dl = S.xf.dir_from(ds);
      if (dda(ol, dl, S.h, 2.0 * r, [&](const IVec3& p) { return vox_solid(S.get(p)); }, &t, &face, step, &v)) {
        const f64 tt = t0 + t;
        if (tt < bt) {
          bt = tt;
          hit.hit = true;
          hit.distance = tt;
          hit.pos = o + d * tt;
          V3 n{0, 0, 0};
          if (face >= 0) n[face] = -step[face];
          hit.normal = rotate(b.q, S.xf.dir_to(n));
          hit.material = static_cast<int>(vox_mat(S.get(v)));
          hit.voxel = v;
          hit.piece = b.id;
          hit.grid = 0;
          hit.shape = static_cast<i32>(k);
        }
      }
    }
  }
  return hit;
}

CollideResult World::collide(const V3& mn, const V3& mx, const V3& mv) const {
  CollideResult res;
  const f64 h = grid_.h;
  const f64 eps = 1e-4;
  // (bounded work: a box of at most 16 m a side, a move of at most 16 m per axis)
  if (!in_range(mn) || !in_range(mx) || !finite3(mv)) return res;
  for (int a = 0; a < 3; ++a)
    if (mx[a] < mn[a] || mx[a] - mn[a] > 16.0) return res;
  std::array<f64, 3> lo = {mn.x, mn.y, mn.z}, hi = {mx.x, mx.y, mx.z};
  const std::array<f64, 3> move = {std::clamp(mv.x, -16.0, 16.0), std::clamp(mv.y, -16.0, 16.0), std::clamp(mv.z, -16.0, 16.0)};
  auto vidx = [&](f64 x) { return static_cast<i32>(std::floor(x / h + 0.5)); };
  for (int a = 0; a < 3; ++a) {
    f64 dm = move[size_t(a)];
    if (dm == 0.0) continue;
    const int b = (a + 1) % 3, c = (a + 2) % 3;
    const i32 b0 = vidx(lo[size_t(b)] + eps), b1 = vidx(hi[size_t(b)] - eps);
    const i32 c0 = vidx(lo[size_t(c)] + eps), c1 = vidx(hi[size_t(c)] - eps);
    const f64 lead = dm > 0 ? hi[size_t(a)] : lo[size_t(a)];
    const f64 target = lead + dm;
    i32 layer = dm > 0 ? vidx(lead - eps) + 1 : vidx(lead + eps) - 1;
    const i32 last = dm > 0 ? vidx(target - eps) : vidx(target + eps);
    bool blocked = false;
    for (; dm > 0 ? layer <= last : layer >= last; layer += dm > 0 ? 1 : -1) {
      for (i32 ib = b0; ib <= b1 && !blocked; ++ib)
        for (i32 ic = c0; ic <= c1 && !blocked; ++ic) {
          i32 p[3];
          p[a] = layer;
          p[b] = ib;
          p[c] = ic;
          if (vox_solid(grid_.get(p[0], p[1], p[2]))) blocked = true;
        }
      if (blocked) break;
    }
    if (blocked) {
      const f64 face = dm > 0 ? (layer - 0.5) * h : (layer + 0.5) * h;
      dm = dm > 0 ? std::max(0.0, face - lead - eps) : std::min(0.0, face - lead + eps);
      if (a == 2 && move[2] < 0) res.on_ground = true;
    }
    // (the oriented grids' voxels: cubes at an angle)
    if (oriented_ > 0 && dm != 0.0) {
      V3 L{0, 0, 0};
      L[a] = dm;
      const SweepHit sh = sweep_grids(V3{lo[0], lo[1], lo[2]}, V3{hi[0], hi[1], hi[2]}, L, false);
      if (sh.hit) {
        const f64 free = sh.t * dm;
        dm = dm > 0 ? std::max(0.0, free - eps) : std::min(0.0, free + eps);
        if (a == 2 && move[2] < 0) {
          res.on_ground = true;
          res.ground = sh.grid;
        }
      }
    }
    lo[size_t(a)] += dm;
    hi[size_t(a)] += dm;
    res.move[a] = dm;
  }
  // (what it stands on moves it: its velocity under the base's centre)
  if (res.ground != 0) res.ground_velocity = grid_velocity(res.ground, V3{0.5 * (lo[0] + hi[0]), 0.5 * (lo[1] + hi[1]), lo[2]});
  return res;
}

SweepHit World::sweep(const V3& mn, const V3& mx, const V3& mv) const { return sweep_grids(mn, mx, mv, true); }

SweepHit World::sweep_grids(const V3& mn, const V3& mx, const V3& mv, bool world_grid) const {
  SweepHit res;
  if (!in_range(mn) || !in_range(mx) || !finite3(mv)) return res;
  for (int a = 0; a < 3; ++a)
    if (mx[a] < mn[a] || mx[a] - mn[a] > 16.0) return res;
  V3 V;
  for (int a = 0; a < 3; ++a) V[a] = std::clamp(mv[a], -16.0, 16.0);
  if (norm2(V) == 0.0) return res;
  const V3 ca = (mn + mx) * 0.5, ea = (mx - mn) * 0.5;
  // the swept box
  V3 slo, shi;
  for (int a = 0; a < 3; ++a) {
    slo[a] = std::min(mn[a], mn[a] + V[a]);
    shi[a] = std::max(mx[a], mx[a] + V[a]);
  }
  f64 best = 1.0;
  bool any = false;
  V3 bn;
  GridId bg = 0;
  for (size_t gi = world_grid ? 0 : 1; gi < grids_.size(); ++gi) {
    if (!grids_[gi]) continue;
    const GridState& st = *grids_[gi];
    const VoxelGrid& G = vg(static_cast<u16>(gi));
    const f64 h = G.h;
    if (gi > 0 && (!st.any || shi.x < st.lo.x - h || slo.x > st.hi.x + h || shi.y < st.lo.y - h || slo.y > st.hi.y + h ||
                   shi.z < st.lo.z - h || slo.z > st.hi.z + h))
      continue;
    // the swept box's voxels in the grid's lattice
    V3 llo{INFINITY, INFINITY, INFINITY}, lhi{-INFINITY, -INFINITY, -INFINITY};
    for (int c = 0; c < 8; ++c) {
      const V3 w{(c & 1) ? shi.x : slo.x, (c & 2) ? shi.y : slo.y, (c & 4) ? shi.z : slo.z};
      const V3 l = st.xf.from(w);
      for (int a = 0; a < 3; ++a) {
        llo[a] = std::min(llo[a], l[a]);
        lhi[a] = std::max(lhi[a], l[a]);
      }
    }
    const IVec3 vlo = voxel_of(llo, h), vhi = voxel_of(lhi, h);
    const V3 u[3] = {st.xf.dir_to(V3{1, 0, 0}), st.xf.dir_to(V3{0, 1, 0}), st.xf.dir_to(V3{0, 0, 1})};
    for (i32 x = vlo[0] - 1; x <= vhi[0] + 1; ++x)
      for (i32 y = vlo[1] - 1; y <= vhi[1] + 1; ++y)
        for (i32 z = vlo[2] - 1; z <= vhi[2] + 1; ++z) {
          if (!vox_solid(G.get(x, y, z))) continue;
          const V3 cb = st.xf.to(V3{h * x, h * y, h * z});
          f64 t;
          V3 n;
          if (!sweep_box_cube(ca, ea, V, cb, u, 0.5 * h, &t, &n)) continue;
          if (t < best || !any) {
            best = t;
            bn = n;
            bg = st.id;
            any = true;
          }
        }
  }
  if (!any) return res;
  res.hit = true;
  res.t = best;
  res.normal = bn;
  res.grid = bg;
  // (the surface's motion where the box touches it: its face towards the surface)
  const V3 c = ca + V * best;
  res.velocity = grid_velocity(bg, c - bn * (std::abs(bn.x) * ea.x + std::abs(bn.y) * ea.y + std::abs(bn.z) * ea.z));
  return res;
}

void World::debug_voxel(const IVec3& p) {
  const IVec3 cc = chunk_of(p);
  FragChunk& fc = frag_chunk(0, cc);
  const i32 g = fc.at(chunk_index(p));
  const Vox v = grid_.get(p);
  std::printf("voxel (%d %d %d): solid %d mat %d free %d, fragment %d", p[0], p[1], p[2], vox_solid(v) ? 1 : 0, int(vox_mat(v)),
              vox_free(v) ? 1 : 0, g);
  if (g >= 0) {
    const FragKey f{key3(cc[0], cc[1], cc[2]), g, 0};
    std::printf(" (%d voxels, owner %lld)", fc.frags[size_t(g)].count, static_cast<long long>(owner_of(f)));
  }
  std::printf("\n  neighbours:");
  for (int a = 0; a < 3; ++a)
    for (int sg = -1; sg <= 1; sg += 2) {
      IVec3 q = p;
      q[a] += sg;
      const bool br = sg > 0 ? grid_.broken(p, a) : grid_.broken(q, a);
      std::printf(" %c%c:%s%s", sg > 0 ? '+' : '-', "xyz"[a], vox_solid(grid_.get(q)) ? "solid" : "air", br ? "(broken)" : "");
    }
  std::printf("\n");
}

bool World::touches_undesigned(const Structure& s) const {
  bool any = false;
  for (const auto& gp : grids_) any = any || (gp && !gp->undesigned.empty());
  if (!any) return false;
  for (const FragKey& f : s.frags)
    if (f.idx >= 0 && live(f.grid) && gs(f.grid).undesigned.count(f.chunk)) return true;
  return false;
}

void World::design_structure(Structure& s, bool dry) {
  // self-weight only (the design state), to a tight tolerance
  auto designed = [&] {
    if (dry) return;
    for (const FragKey& f : s.frags)
      if (f.idx >= 0) gs(f.grid).undesigned.erase(f.chunk);
  };
  if (!s.P.assembled()) {
    StressOptions so;
    so.rtol = 1e-5;
    if (!s.P.assemble(so)) {
      designed();  // (given up: never tried again, or extract and design would recurse)
      return;
    }
  }
  const size_t n = s.P.nodes.size();
  std::vector<f64> F(6 * n, 0.0);
  if (s.body == 0) {
    for (size_t i = 0; i < n; ++i) F[6 * i + 2] = -s.weight[i];
  } else {
    // (a kinematic body's: its weight as it stands, in its frame)
    const V3 g = rotate_inv(kins_[s.body]->q, V3{0.0, 0.0, -cfg_.rigid.gravity});
    for (size_t i = 0; i < n; ++i) {
      F[6 * i + 0] = g.x * s.P.nodes[i].mass;
      F[6 * i + 1] = g.y * s.P.nodes[i].mass;
      F[6 * i + 2] = g.z * s.P.nodes[i].mass;
    }
  }
  add_loads_to(s, F);  // (a dam is designed for its water)
  std::vector<f64> u(6 * n, 0.0);
  PcgResult r = s.P.solve(F, u, 1e-5, 2000, false);
  if (!r.converged && !r.breakdown) {
    // (a near-mechanism a block-Jacobi preconditioned solve stagnates on: with the multigrid)
    StressOptions so;
    so.rtol = 1e-5;
    so.amg_min_nodes = 0;
    std::fill(u.begin(), u.end(), 0.0);
    if (s.P.assemble(so)) r = s.P.solve(F, u, 1e-5, 2000, false);
  }
  if (r.breakdown || !std::isfinite(r.rel_res)) {
    designed();
    return;
  }
  const f64 target = cfg_.design_utilization;
  static const bool dbg = diag("SVX_DEBUG_DESIGN");
  f64 maxphi = 0.0;
  i32 over = 0;
  for (i32 b = 0; b < static_cast<i32>(s.P.bonds.size()); ++b) {
    const SBond& B = s.P.bonds[size_t(b)];
    if (B.broken) continue;
    const BondLoad L = s.P.bond_load(b, u);
    if (!dry) judged_[s.bid[size_t(b)]] = {node_chunk(s, B.a), L};  // (the reference state of sudden changes, as the bake's)
    const f64 phi = bond_utilization(B, L, 1.0);
    maxphi = std::max(maxphi, phi);
    if (phi <= target) continue;
    ++over;
    if (dry) continue;
    const u8 cls = class_for(B.strength * phi / target);
    for (int side = 0; side < 2; ++side) {
      const i32 nd = side == 0 ? B.a : B.b;
      if (nd < 0) continue;
      design_node(s, nd, cls, &st_.strengthened_voxels);
    }
  }
  if (dbg) {
    f64 W = 0.0;
    for (size_t i = 0; i < n; ++i) W += s.weight[i];
    std::printf("  [design state s%lld] weight %.4g N, bonds %zu\n", static_cast<long long>(s.id), W, s.P.bonds.size());
  }
  if (dbg)
    std::printf("  [design%s] s%lld: %zu nodes (%d truncated), pcg %d rel %.1e conv %d, max phi %.2f, %d bonds over target\n", dry ? " check" : "",
                static_cast<long long>(s.id), n, s.truncated ? 1 : 0, r.iters, r.rel_res, r.converged ? 1 : 0, maxphi, over);
  if (dry) return;
  for (size_t i = 0; i < n; ++i) {
    std::array<f32, 6> w;
    for (int q = 0; q < 6; ++q) w[size_t(q)] = static_cast<f32>(u[6 * i + size_t(q)]);
    warm_u_[s.ident[i]] = {node_chunk(s, static_cast<i32>(i)), w};
  }
  designed();
}

void World::design_near(const V3& c, f64 r) {
  // structures around an event, designed before it happens (streamed worlds, grids added later)
  for (size_t gi = 0; gi < grids_.size(); ++gi) {
    if (!grids_[gi] || grids_[gi]->undesigned.empty()) continue;
    const u16 g = static_cast<u16>(gi);
    const VoxelGrid& G = vg(g);
    const f64 h = G.h;
    const auto& und = gs(g).undesigned;
    const V3 cl = g == 0 ? c : xf_of(g).from(c);
    const IVec3 lo = voxel_of(cl - V3{r, r, r}, h), hi = voxel_of(cl + V3{r, r, r}, h);
    const i32 step = std::max<i32>(1, static_cast<i32>(r / (4.0 * h)));
    for (i32 x = lo[0]; x <= hi[0]; x += step)
      for (i32 y = lo[1]; y <= hi[1]; y += step)
        for (i32 z = lo[2]; z <= hi[2]; z += step) {
          const IVec3 p{x, y, z};
          if (!und.count(key3(p[0] >> kChunkBits, p[1] >> kChunkBits, p[2] >> kChunkBits))) continue;
          if (!vox_free(G.get(p))) continue;
          FragKey f;
          if (!frag_at(GVox{p, g}, &f) || owner_of(f) != 0) continue;
          extract(f);  // (designs it)
          if (!live(g) || gs(g).undesigned.empty()) break;
        }
  }
}

}  // namespace svx
