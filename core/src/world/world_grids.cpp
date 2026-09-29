// structvox — the world's grids (docs/GRIDS.md): oriented grids, their frames, and the junctions
// that bond them to each other and to the world grid.
//
// A grid is a voxel lattice placed in the world (GridFrame): its voxel p is centred at
// origin + R (h p). The world grid (slot 0) has the identity frame; its arithmetic is what it
// always was. Oriented grids are structure like the world grid's voxels: they fragment in their
// own lattice (their rubble follows their axes), and their faces bond within the grid as the
// world grid's do. Where a face meets another grid's voxels, it bonds to them through samples:
// S x S points of the face, pushed out by the junction reach, landing in a solid voxel of the
// other grid. An interface is measured by the faces of the newer of the two grids, which owns the
// space both have voxels in; a junction bond breaks sample by sample (the grid keeps the broken
// ones per voxel).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "svx/world/world.hpp"
#include "world_internal.hpp"

namespace svx {

using namespace world_detail;

namespace {

bool finite_quat(const Quat& q) { return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w); }

// The world box of a lattice box [lo, hi] (metres, in the lattice) placed by xf.
void world_box(const LatticeXf& xf, const V3& lo, const V3& hi, V3* wlo, V3* whi) {
  *wlo = V3{INFINITY, INFINITY, INFINITY};
  *whi = V3{-INFINITY, -INFINITY, -INFINITY};
  for (int c = 0; c < 8; ++c) {
    const V3 p{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
    const V3 w = xf.to(p);
    for (int a = 0; a < 3; ++a) {
      (*wlo)[a] = std::min((*wlo)[a], w[a]);
      (*whi)[a] = std::max((*whi)[a], w[a]);
    }
  }
}

inline bool inside(const V3& p, const V3& lo, const V3& hi) {
  return p.x >= lo.x && p.y >= lo.y && p.z >= lo.z && p.x <= hi.x && p.y <= hi.y && p.z <= hi.z;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Grids: identities and frames

i32 World::slot_of(GridId id) const {
  if (id == kWorldGrid) return 0;
  const auto it = slots_.find(id);
  return it == slots_.end() ? -1 : static_cast<i32>(it->second);
}

GridId World::id_of(u16 g) const { return grids_[g]->id; }

const LatticeXf& World::xf_of(u16 g) const { return grids_[g]->xf; }

f64 World::h_of(u16 g) const { return vg(g).h; }



bool World::owns(u16 a, u16 b) const {
  const GridState& A = *grids_[a];
  const GridState& B = *grids_[b];
  return A.priority != B.priority ? A.priority > B.priority : A.id > B.id;
}

i32 World::grid_priority(GridId id) const {
  const i32 s = slot_of(id);
  return s < 0 ? 0 : grids_[size_t(s)]->priority;
}

V3 World::voxel_centre(const GVox& v) const {
  const f64 h = h_of(v.grid);
  const V3 s{h * v.p[0], h * v.p[1], h * v.p[2]};
  return v.grid == 0 ? s : grids_[v.grid]->xf.to(s);
}

u64 World::frag_ident_of(const FragKey& f, i32 first) const {
  const u64 id = frag_ident(f.chunk, first);
  if (f.grid == 0) return id;
  const GridState& st = *grids_[f.grid];
  const u64 x = mix64(id ^ (static_cast<u64>(st.id) * 0xA24BAED4963EE407ull));
  return st.placement == 0 ? x : mix64(x ^ (static_cast<u64>(st.placement) * 0x9FB21C651E98DF25ull));
}

u64 World::residency_key(u16 g, u64 chunk) const { return g == 0 ? chunk : ((1ull << 63) | grids_[g]->id); }

bool World::resident_key(u64 k) const {
  if (k >> 63) return slots_.count(static_cast<GridId>(k & 0xFFFFFFFFull)) > 0;
  return generated_.count(k) > 0;
}

std::vector<GridId> World::grids() const {
  std::vector<GridId> out;
  out.reserve(slots_.size());
  for (const auto& [id, s] : slots_) out.push_back(id);
  std::sort(out.begin(), out.end());
  return out;
}

const VoxelGrid* World::grid(GridId id) const {
  const i32 s = slot_of(id);
  return s < 0 ? nullptr : &vg(static_cast<u16>(s));
}

bool World::grid_frame(GridId id, GridFrame* out) const {
  const i32 s = slot_of(id);
  if (s < 0) return false;
  const LatticeXf& x = grids_[size_t(s)]->xf;
  out->origin = x.off;
  out->rot = x.q;
  return true;
}

void World::world_chunks_of(const LatticeXf& xf, const V3& lo, const V3& hi, std::vector<u64>& out) const {
  // (the world chunks a lattice box's world box meets)
  V3 wlo, whi;
  world_box(xf, lo, hi, &wlo, &whi);
  const f64 h = grid_.h;
  auto cc = [&](f64 x) { return static_cast<i32>(std::floor(x / h + 0.5)) >> kChunkBits; };
  const i32 x0 = cc(wlo.x), x1 = cc(whi.x), y0 = cc(wlo.y), y1 = cc(whi.y), z0 = cc(wlo.z), z1 = cc(whi.z);
  if (static_cast<i64>(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 4096) return;  // (never: grids' chunks are small)
  for (i32 x = x0; x <= x1; ++x)
    for (i32 y = y0; y <= y1; ++y)
      for (i32 z = z0; z <= z1; ++z) out.push_back(key3(x, y, z));
}

const u8* World::grid_solids(const IVec3& cc) const {
  const SolidsCache* e = solids_entry(cc);
  return e && !e->bits.empty() ? e->bits.data() : nullptr;
}

u64 World::grid_solids_stamp(const IVec3& cc) const {
  const SolidsCache* e = solids_entry(cc);
  return e && !e->bits.empty() ? e->stamp : 0;
}

bool World::grid_voxel_at(const V3& X, GridId* grid, IVec3* voxel) const {
  if (oriented_ == 0) return false;
  for (size_t s = 1; s < grids_.size(); ++s) {
    if (!grids_[s] || !grids_[s]->any) continue;
    const GridState& st = *grids_[s];
    if (!inside(X, st.lo, st.hi)) continue;
    const IVec3 v = voxel_of(st.xf.from(X), st.g.h);
    if (!vox_solid(st.g.get(v))) continue;
    *grid = st.id;
    *voxel = v;
    return true;
  }
  return false;
}

const World::SolidsCache* World::solids_entry(const IVec3& cc) const {
  if (oriented_ == 0) return nullptr;
  const u64 key = key3(cc[0], cc[1], cc[2]);
  auto it = solids_.find(key);
  if (it != solids_.end()) {
    // (still what the grids there hold, where they are)
    bool fresh = it->second.epoch == grid_epoch_;
    for (const auto& f : it->second.from) {
      if (!fresh) break;
      const u16 g = static_cast<u16>(f[0]);
      fresh = g < grids_.size() && grids_[g] && grids_[g]->g.revision() == f[1] && grids_[g]->placement == f[2];
    }
    if (fresh) return &it->second;
  }
  SolidsCache c;
  c.epoch = grid_epoch_;
  c.stamp = ++solids_seq_;
  const f64 h = grid_.h;
  const V3 clo{h * (cc[0] * kChunk - 0.5), h * (cc[1] * kChunk - 0.5), h * (cc[2] * kChunk - 0.5)};
  const V3 chi{clo.x + h * kChunk, clo.y + h * kChunk, clo.z + h * kChunk};
  for (size_t s = 1; s < grids_.size(); ++s) {
    if (!grids_[s] || !grids_[s]->any) continue;
    const GridState& st = *grids_[s];
    if (st.hi.x < clo.x || st.lo.x > chi.x || st.hi.y < clo.y || st.lo.y > chi.y || st.hi.z < clo.z || st.lo.z > chi.z) continue;
    c.from.push_back({s, st.g.revision(), st.placement});
    // (the chunk's voxels whose centres lie in the grid's box)
    i32 a0[3], a1[3];
    bool none = false;
    for (int a = 0; a < 3; ++a) {
      a0[a] = std::max(cc[a] * kChunk, static_cast<i32>(std::floor(st.lo[a] / h + 0.5)));
      a1[a] = std::min(cc[a] * kChunk + kChunk - 1, static_cast<i32>(std::floor(st.hi[a] / h + 0.5)));
      none = none || a0[a] > a1[a];
    }
    if (none) continue;
    const f64 hg = st.g.h;
    for (i32 x = a0[0]; x <= a1[0]; ++x)
      for (i32 y = a0[1]; y <= a1[1]; ++y)
        for (i32 z = a0[2]; z <= a1[2]; ++z) {
          const IVec3 v = voxel_of(st.xf.from(V3{h * x, h * y, h * z}), hg);
          if (!vox_solid(st.g.get(v))) continue;
          if (c.bits.empty()) c.bits.assign(kChunkVox / 8, 0);
          const i32 i = chunk_index(IVec3{x, y, z});
          c.bits[size_t(i >> 3)] = static_cast<u8>(c.bits[size_t(i >> 3)] | (1u << (i & 7)));
        }
  }
  if (solids_.size() > 65536) solids_.clear();  // (a bound: what is needed is made again)
  return &(solids_[key] = std::move(c));
}

V3 World::grid_to_world(GridId id, const V3& lattice) const {
  const i32 s = slot_of(id);
  return s <= 0 ? lattice : grids_[size_t(s)]->xf.to(lattice);
}

V3 World::world_to_grid(GridId id, const V3& world) const {
  const i32 s = slot_of(id);
  return s <= 0 ? world : grids_[size_t(s)]->xf.from(world);
}

void World::refresh_grid_box(u16 g) {
  if (g == 0) return;
  GridState& st = gs(g);
  const VoxelGrid& G = st.g;
  IVec3 clo{INT32_MAX, INT32_MAX, INT32_MAX}, chi{INT32_MIN, INT32_MIN, INT32_MIN};
  bool any = false;
  for (const auto& [k, c] : G.chunks()) {
    if (c.uniform && !vox_solid(c.value)) continue;
    const IVec3 cc = unkey3(k);
    for (int a = 0; a < 3; ++a) {
      clo[a] = std::min(clo[a], cc[a]);
      chi[a] = std::max(chi[a], cc[a]);
    }
    any = true;
  }
  const V3 olo = st.lo, ohi = st.hi;
  const bool was = st.any;
  st.any = any;
  if (!any) {
    st.lo = st.hi = st.xf.off;
  } else {
    const f64 h = st.g.h;
    st.llo = V3{h * (clo[0] * kChunk - 0.5), h * (clo[1] * kChunk - 0.5), h * (clo[2] * kChunk - 0.5)};
    st.lhi = V3{h * ((chi[0] + 1) * kChunk - 0.5), h * ((chi[1] + 1) * kChunk - 0.5), h * ((chi[2] + 1) * kChunk - 0.5)};
    world_box(st.xf, st.llo, st.lhi, &st.lo, &st.hi);
  }
  // (a grid that grew reaches more: the candidates of junctions are found again)
  if (any != was || !inside(st.lo, olo, ohi) || !inside(st.hi, olo, ohi)) grids_changed();
}

void World::grids_changed() { ++grid_epoch_; }

const std::vector<u16>& World::near_grids(u16 g, u64 chunk) {
  static const std::vector<u16> none;
  if (oriented_ == 0) return none;
  GridState& st = gs(g);
  if (st.near_epoch != grid_epoch_) {
    st.near.clear();
    st.near_epoch = grid_epoch_;
  }
  const auto it = st.near.find(chunk);
  if (it != st.near.end()) return it->second;
  std::vector<u16> out;
  // the chunk's box in the world, grown by the reach of a sample (and a voxel of margin)
  const f64 h = st.g.h;
  const IVec3 cc = unkey3(chunk);
  const V3 lo{h * (cc[0] * kChunk - 0.5), h * (cc[1] * kChunk - 0.5), h * (cc[2] * kChunk - 0.5)};
  const V3 hi{h * ((cc[0] + 1) * kChunk - 0.5), h * ((cc[1] + 1) * kChunk - 0.5), h * ((cc[2] + 1) * kChunk - 0.5)};
  V3 wlo, whi;
  world_box(st.xf, lo, hi, &wlo, &whi);
  const f64 r = std::max(0.0, cfg_.junction_reach) + 1.5;
  if (g != 0) out.push_back(0);  // (the world grid: everywhere)
  for (size_t s = 1; s < grids_.size(); ++s) {
    if (s == g || !grids_[s] || !grids_[s]->any) continue;
    const GridState& o = *grids_[s];
    const f64 m = r * std::max(h, o.g.h);  // (a sample reaches voxels of the other grid)
    if (o.hi.x < wlo.x - m || o.lo.x > whi.x + m || o.hi.y < wlo.y - m || o.lo.y > whi.y + m || o.hi.z < wlo.z - m ||
        o.lo.z > whi.z + m)
      continue;
    out.push_back(static_cast<u16>(s));
  }
  return st.near.emplace(chunk, std::move(out)).first->second;
}

std::vector<StaticGrid> World::static_grids() const {
  std::vector<StaticGrid> out;
  StaticGrid w;
  w.g = &grid_;
  w.unbounded = true;
  w.slot = 0;
  out.push_back(w);
  for (size_t s = 1; s < grids_.size(); ++s) {
    if (!grids_[s] || !grids_[s]->any) continue;
    const GridState& st = *grids_[s];
    const f64 m = st.g.h;
    StaticGrid o;
    o.g = &st.g;
    o.xf = st.xf;
    o.lo = st.lo - V3{m, m, m};
    o.hi = st.hi + V3{m, m, m};
    o.slot = static_cast<u16>(s);
    out.push_back(o);
  }
  return out;
}

void World::tear_voxel(const GVox& v) {
  VoxelGrid& G = vg(v.grid);
  for (int a = 0; a < 3; ++a) {
    G.break_bond(v.p, a);
    IVec3 q = v.p;
    q[a] -= 1;
    G.break_bond(q, a);
  }
  // (and its junctions: kept only where there is another grid to bond to, so that a world of the
  // world grid alone is saved and hashed as it always was)
  if (oriented_ > 0)
    for (int f = 0; f < 6; ++f) G.break_junction(v.p, f, kJunctionFace);
}

// ---------------------------------------------------------------------------------------------
// Adding and removing grids

GridId World::add_grid(const GridDesc& d, VoxelGrid&& voxels) {
  if (in_tick_) return 0;
  return add_grid_impl(d, std::move(voxels), 0, ~0ull, designed_all_);
}

GridId World::add_grid_impl(const GridDesc& d, VoxelGrid&& voxels, GridId want, u64 home, bool seed) {
  const GridFrame& frame = d.frame;
  if (!in_range(frame.origin) || !finite_quat(frame.rot)) return 0;
  if (want != 0 && slot_of(want) >= 0) return 0;
  const f64 qn = frame.rot.x * frame.rot.x + frame.rot.y * frame.rot.y + frame.rot.z * frame.rot.z + frame.rot.w * frame.rot.w;
  if (!(qn > 1e-12)) return 0;
  // (a voxel size of its own: from 1/64 to 64 x the world's)
  const f64 h = d.voxel_size == 0.0 ? grid_.h : d.voxel_size;
  if (!std::isfinite(h) || !(h >= grid_.h / 64.0) || !(h <= grid_.h * 64.0)) return 0;
  // (its voxels within the key range, a margin for the events around them)
  for (const auto& [k, c] : voxels.chunks()) {
    const IVec3 cc = unkey3(k);
    for (int a = 0; a < 3; ++a)
      if (std::abs(static_cast<i64>(cc[a]) * kChunk) + kChunk >= kVoxelLimit) return 0;
  }
  // the lowest free slot (deterministic: the same slots for the same calls)
  u16 slot = 0;
  for (size_t s = 1; s < grids_.size(); ++s)
    if (!grids_[s]) {
      slot = static_cast<u16>(s);
      break;
    }
  if (slot == 0) {
    if (grids_.size() >= 0xFFFF) return 0;
    slot = static_cast<u16>(grids_.size());
    grids_.emplace_back();
  }
  auto st = std::make_unique<GridState>();
  st->id = want != 0 ? want : next_grid_++;
  st->base = d.base;
  st->priority = d.priority;
  st->home = home;
  st->xf = LatticeXf::make(frame.origin, frame.rot);
  // (never the identity: a grid placed exactly as the world grid is still its own lattice)
  if (st->xf.identity) {
    st->xf.identity = false;
    st->xf.R = M3::identity();
    st->xf.Rt = M3::identity();
  }
  voxels.h = h;
  // the world's layers, by name (a grid made with layers of its own keeps those the world has)
  voxels.adopt_layers(layer_specs_);
  voxels.sanitize();
  voxels.compact();
  st->g = std::move(voxels);
  st->g.lo = {0, 0, 0};
  st->g.hi = {0, 0, 0};
  st->g.track_changes(true);
  st->g.mark_all_dirty();
  const GridId id = st->id;
  grids_[slot] = std::move(st);
  slots_[id] = slot;
  if (home != ~0ull) home_grids_[home].push_back(slot);
  ++oriented_;
  refresh_grid_box(slot);
  grids_changed();
  // where it overlaps other grids, the lower priority's voxels go (a level's grid: as
  // part of the level, not saved; a grid of this session: a change)
  displace_overlaps(slot, !d.base);
  // what it touches is loaded differently now, and a grid of free voxels on nothing falls: its
  // structures are extracted at the next tick (one seed per fragment). Before the design pass
  // (a level being built), the pass does it; after it, the grid is designed when first touched,
  // like a streamed chunk.
  if (seed) {
    GridState& G = gs(slot);
    std::vector<u64> keys;
    for (const auto& [k, c] : G.g.chunks())
      if (c.free_count() > 0) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    for (u64 k : keys) {
      const IVec3 cc = unkey3(k);
      G.undesigned.insert(k);
      FragChunk& fc = frag_chunk(slot, cc);
      for (const FragInfo& fi : fc.frags) {
        if (fi.count <= 0) continue;
        const IVec3 l = local_of(fi.first);
        seeds_.push_back(GVox{{cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]}, slot});
      }
    }
    const V3 m{2 * h, 2 * h, 2 * h};
    rigid_.wake_box(G.lo - m, G.hi + m);
  }
  WorldEvent ev;
  ev.kind = WorldEvent::Kind::GridAdded;
  ev.id = static_cast<i64>(id);
  ev.pos = frame.origin;
  ev.rot = gs(slot).xf.q;
  events_.push_back(std::move(ev));
  return id;
}

bool World::remove_grid(GridId id) {
  if (in_tick_ || id == kWorldGrid) return false;
  const i32 s = slot_of(id);
  if (s <= 0) return false;
  const u16 g = static_cast<u16>(s);
  // What it held through junctions is extracted again: it lost those bonds (a slab on a column of
  // it falls). (Not when a streamed grid is evicted: the world there is unknown, and holds.)
  release_junctions(g);
  if (gs(g).base) removed_base_.push_back(id);
  remove_grid_slot(g, true);
  return true;
}

void World::release_junctions(u16 g) {
  JunctionScratch js;
  std::vector<u64> keys;
  for (const auto& [k, c] : vg(g).chunks())
    if (!c.uniform || vox_solid(c.value)) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  std::vector<GKey> stale;
  for (u64 k : keys) {
    const i32 nfrag = static_cast<i32>(frag_chunk(g, unkey3(k)).frags.size());
    for (i32 i = 0; i < nfrag; ++i) {
      const FragChunk* fc = frag_chunk_if(FragKey{k, i, g});
      if (!fc || fc->frags[size_t(i)].count <= 0) continue;
      each_junction(js, FragKey{k, i, g}, [&](const JSample& j, bool fwd) {
        if (fwd && j.kind == kJunctionUnknown) return;
        const GVox o = fwd ? GVox{j.o, j.og} : GVox{j.v, j.vg};
        if (o.grid == g || !vox_free(vg(o.grid).get(o.p))) return;
        seeds_.push_back(o);
        const IVec3 oc = chunk_of(o.p);
        stale.push_back(GKey{o.grid, key3(oc[0], oc[1], oc[2])});
      });
    }
  }
  std::sort(stale.begin(), stale.end());
  stale.erase(std::unique(stale.begin(), stale.end()), stale.end());
  for (const GKey& k : stale) mark_owners_stale(k.grid, k.chunk);
}

bool World::set_grid_frame(GridId id, const GridFrame& frame) {
  if (in_tick_ || id == kWorldGrid) return false;
  const i32 s = slot_of(id);
  if (s <= 0) return false;
  if (!in_range(frame.origin) || !finite_quat(frame.rot)) return false;
  const f64 qn = frame.rot.x * frame.rot.x + frame.rot.y * frame.rot.y + frame.rot.z * frame.rot.z + frame.rot.w * frame.rot.w;
  if (!(qn > 1e-12)) return false;
  const u16 g = static_cast<u16>(s);
  GridState& st = gs(g);
  const f64 h = st.g.h;
  const V3 m{2 * h, 2 * h, 2 * h};
  // Where it was: what it held lets go (its partners are extracted again), its structures go
  // (their fragments of other grids are extracted again), what rested on it wakes.
  release_junctions(g);
  {
    std::vector<i64> ids;
    for (const auto& [k, o] : st.owner)
      for (i64 sid : o)
        if (sid) ids.push_back(sid);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (i64 sid : ids)
      if (Structure* sp = structure(sid)) {
        reseed(*sp);
        drop_structure(sid);
      }
  }
  if (st.any) rigid_.wake_box(st.lo - m, st.hi + m);
  // (its broken junction samples were those of its old place: it bonds afresh)
  st.g.clear_junction_breaks();
  // (the systems of the world's lattice hear of the place it leaves)
  if (st.any && !systems_.empty()) world_chunks_of(LatticeXf{}, st.lo, st.hi, sys_changed_);
  st.xf = LatticeXf::make(frame.origin, frame.rot);
  if (st.xf.identity) {
    st.xf.identity = false;
    st.xf.R = M3::identity();
    st.xf.Rt = M3::identity();
  }
  st.moved = true;
  ++st.placement;
  st.g.mark_all_dirty();  // (hosts place its chunks anew)
  grids_changed();
  refresh_grid_box(g);
  // Where it is now: overlaps resolved (a change), its structures and what it bonds to there are
  // extracted at the next tick, what it lands on wakes.
  displace_overlaps(g, true);
  std::vector<u64> keys;
  for (const auto& [k, c] : st.g.chunks())
    if (c.free_count() > 0) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  for (u64 k : keys) {
    const IVec3 cc = unkey3(k);
    FragChunk& fc = frag_chunk(g, cc);
    for (const FragInfo& fi : fc.frags) {
      if (fi.count <= 0) continue;
      const IVec3 l = local_of(fi.first);
      seeds_.push_back(GVox{{cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]}, g});
    }
  }
  release_junctions(g);
  if (st.any) rigid_.wake_box(st.lo - m, st.hi + m);
  WorldEvent ev;
  ev.kind = WorldEvent::Kind::GridMoved;
  ev.id = static_cast<i64>(id);
  ev.pos = st.xf.off;
  ev.rot = st.xf.q;
  events_.push_back(std::move(ev));
  return true;
}

void World::displace_overlaps(u16 g, bool tracked) {
  if (!live(g)) return;
  const GridState& st = gs(g);
  if (g != 0 && !st.any) return;
  // (the grids whose boxes meet its box, in slot order: the world grid first)
  for (size_t o = 0; o < grids_.size(); ++o) {
    if (o == g || !grids_[o]) continue;
    const u16 s = static_cast<u16>(o);
    const GridState& os = gs(s);
    if (s != 0 && (!os.any || os.hi.x < st.lo.x || os.lo.x > st.hi.x || os.hi.y < st.lo.y || os.lo.y > st.hi.y ||
                   os.hi.z < st.lo.z || os.lo.z > st.hi.z))
      continue;
    if (g == 0 && s == 0) continue;
    if (owns(g, s)) displace(g, s, nullptr, nullptr, tracked);
    else displace(s, g, nullptr, nullptr, tracked);
  }
}

void World::displace_edits(u16 g, const IVec3& lo, const IVec3& hi, bool tracked) {
  const f64 h = h_of(g);
  // the edits' box in the world
  V3 blo, bhi;
  world_box(xf_of(g), V3{h * (lo[0] - 0.5), h * (lo[1] - 0.5), h * (lo[2] - 0.5)}, V3{h * (hi[0] + 0.5), h * (hi[1] + 0.5), h * (hi[2] + 0.5)},
            &blo, &bhi);
  for (size_t o = 0; o < grids_.size(); ++o) {
    if (o == g || !grids_[o]) continue;
    const u16 s = static_cast<u16>(o);
    const GridState& os = gs(s);
    if (s != 0 && (!os.any || os.hi.x < blo.x || os.lo.x > bhi.x || os.hi.y < blo.y || os.lo.y > bhi.y || os.hi.z < blo.z ||
                   os.lo.z > bhi.z))
      continue;
    if (owns(s, g)) {
      displace(s, g, &lo, &hi, tracked);  // (written where the owner is: they go again)
    } else {
      // (the other grid's voxels the new ones take the place of: the box in its lattice)
      V3 llo, lhi;
      world_box(inverse(xf_of(s)), blo, bhi, &llo, &lhi);
      const f64 hs = h_of(s);
      IVec3 slo = voxel_of(llo, hs), shi = voxel_of(lhi, hs);
      for (int a = 0; a < 3; ++a) {
        --slo[a];
        ++shi[a];
      }
      displace(g, s, &slo, &shi, tracked);
    }
  }
}

i32 World::displace(u16 keep, u16 lose, const IVec3* lo, const IVec3* hi, bool tracked) {
  if (!live(keep) || !live(lose) || keep == lose) return 0;
  const GridState& K = gs(keep);
  if (keep != 0 && !K.any) return 0;
  VoxelGrid& L = vg(lose);
  const VoxelGrid& KG = vg(keep);
  const f64 hl = L.h, hk = KG.h;
  // keep's box (the world grid's: lose's own) in lose's lattice, clamped to [lo, hi]
  IVec3 vlo{INT32_MIN / 2, INT32_MIN / 2, INT32_MIN / 2}, vhi{INT32_MAX / 2, INT32_MAX / 2, INT32_MAX / 2};
  if (keep != 0) {
    V3 blo, bhi;
    world_box(inverse(xf_of(lose)), K.lo, K.hi, &blo, &bhi);
    vlo = voxel_of(blo, hl);
    vhi = voxel_of(bhi, hl);
    for (int a = 0; a < 3; ++a) {
      --vlo[a];
      ++vhi[a];
    }
  }
  if (lo)
    for (int a = 0; a < 3; ++a) vlo[a] = std::max(vlo[a], (*lo)[a]);
  if (hi)
    for (int a = 0; a < 3; ++a) vhi[a] = std::min(vhi[a], (*hi)[a]);
  std::vector<u64> keys;
  for (const auto& [k, c] : L.chunks()) {
    if (c.uniform && !vox_solid(c.value)) continue;
    const IVec3 cc = unkey3(k);
    bool meets = true;
    for (int a = 0; a < 3; ++a) meets = meets && cc[a] * kChunk <= vhi[a] && cc[a] * kChunk + kChunk - 1 >= vlo[a];
    if (!meets) continue;
    if (lose == 0 && source_ && !chunk_resident(cc)) continue;  // (a chunk generated later is displaced then)
    keys.push_back(k);
  }
  std::sort(keys.begin(), keys.end());
  const LatticeXf& XL = xf_of(lose);
  const LatticeXf& XK = xf_of(keep);
  std::vector<GVox> removed;
  std::vector<GKey> supports;
  const bool was = L.tracking();
  if (!tracked) L.track_changes(false);
  for (u64 k : keys) {
    const IVec3 cc = unkey3(k);
    const IVec3 base{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
    const Chunk* ch = L.chunk(cc);
    for (int i = 0; i < kChunkVox && ch; ++i) {
      const Vox v = ch->uniform ? ch->value : ch->v[size_t(i)];
      if (!vox_solid(v)) continue;
      const IVec3 l = local_of(i);
      const IVec3 p{base[0] + l[0], base[1] + l[1], base[2] + l[2]};
      bool inside_box = true;
      for (int a = 0; a < 3; ++a) inside_box = inside_box && p[a] >= vlo[a] && p[a] <= vhi[a];
      if (!inside_box) continue;
      const V3 X = XL.to(V3{hl * p[0], hl * p[1], hl * p[2]});
      if (!vox_solid(KG.get(voxel_of(keep == 0 ? X : XK.from(X), hk)))) continue;
      const GVox gv{p, lose};
      if (vox_anchored(v)) support_changed(gv, &supports);
      L.set(p, kAir);
      removed.push_back(gv);
      ch = L.chunk(cc);  // (a uniform chunk is mixed now)
    }
  }
  L.track_changes(was);
  for (const GKey& k : supports) mark_owners_stale(k.grid, k.chunk);
  if (removed.empty()) return 0;
  if (lose != 0) refresh_grid_box(lose);
  seed_near(removed);
  return static_cast<i32>(removed.size());
}

void World::remove_grid_slot(u16 g, bool event) {
  GridState& st = gs(g);
  // its structures go (what they held of other grids is extracted again: it lost those bonds)
  std::vector<i64> ids;
  for (const auto& [k, o] : st.owner)
    for (i64 id : o)
      if (id) ids.push_back(id);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  for (i64 id : ids)
    if (Structure* s = structure(id)) {
      reseed(*s);
      drop_structure(id);
    }
  // (what rested on it falls; loads and seeds on it go; the systems of the world's lattice hear
  // of the place it leaves)
  const V3 m{2 * st.g.h, 2 * st.g.h, 2 * st.g.h};
  if (st.any) rigid_.wake_box(st.lo - m, st.hi + m);
  if (st.any && !systems_.empty()) world_chunks_of(LatticeXf{}, st.lo, st.hi, sys_changed_);
  for (auto it = dead_loads_.begin(); it != dead_loads_.end(); ++it)
    it->second.erase(std::remove_if(it->second.begin(), it->second.end(), [&](const DeadLoad& d) { return d.vox.grid == g; }),
                     it->second.end());
  blast_loads_.erase(std::remove_if(blast_loads_.begin(), blast_loads_.end(), [&](const BlastLoad& b) { return b.grid == g; }),
                     blast_loads_.end());
  seeds_.erase(std::remove_if(seeds_.begin(), seeds_.end(), [&](const GVox& v) { return v.grid == g; }), seeds_.end());
  grid_dirty_.erase(std::remove_if(grid_dirty_.begin(), grid_dirty_.end(), [&](const GridChunk& c) { return c.grid == st.id; }),
                    grid_dirty_.end());
  if (st.home != ~0ull) {
    auto ht = home_grids_.find(st.home);
    if (ht != home_grids_.end()) {
      ht->second.erase(std::remove(ht->second.begin(), ht->second.end(), g), ht->second.end());
      if (ht->second.empty()) home_grids_.erase(ht);
    }
  }
  const GridId id = st.id;
  if (event) {
    WorldEvent ev;
    ev.kind = WorldEvent::Kind::GridRemoved;
    ev.id = static_cast<i64>(id);
    ev.pos = st.xf.off;
    ev.rot = st.xf.q;
    events_.push_back(std::move(ev));
  }
  slots_.erase(id);
  grids_[g].reset();
  while (grids_.size() > 1 && !grids_.back()) grids_.pop_back();
  --oriented_;
  grids_changed();
}

// ---------------------------------------------------------------------------------------------
// Junction samples

int World::junction_side(u16 g, const JSample& j, bool fwd) const {
  if (fwd) return j.face;  // (its own face)
  // (the other grid's face, whose normal points at the node: out of the node is its opposite)
  const V3 n = xf_of(g).dir_from(xf_of(j.vg).dir_to(face_normal(j.face)) * -1.0);
  int a = 0;
  for (int q = 1; q < 3; ++q)
    if (std::abs(n[q]) > std::abs(n[a])) a = q;
  return a * 2 + (n[a] > 0.0 ? 1 : 0);
}

const std::vector<JSample>& World::junction_fwd(JunctionScratch& js, u16 g, u64 chunk) {
  const GKey key{g, chunk};
  const auto it = js.fwd.find(key);
  if (it != js.fwd.end()) return it->second;
  std::vector<JSample> out;
  const std::vector<u16> nb = near_grids(g, chunk);
  const VoxelGrid& G = vg(g);
  const IVec3 cc = unkey3(chunk);
  const Chunk* ch = G.chunk(cc);
  if (nb.empty() || !ch || (ch->uniform && !vox_solid(ch->value))) return js.fwd.emplace(key, std::move(out)).first->second;
  const f64 h = G.h;
  const i32 S = std::clamp(cfg_.junction_samples, 1, 7);
  // (a sample reaches junction_reach voxels of the other grid out of its face: where the other
  // grid's voxels were displaced by this one's, its surface is up to half of its voxel away)
  const f64 reach = std::clamp(cfg_.junction_reach, 0.0, 2.0);
  const LatticeXf& X = xf_of(g);
  const IVec3 base{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
  constexpr int kStride[3] = {kChunk * kChunk, kChunk, 1};
  // (per other grid: its chunk last looked into)
  struct Cache {
    IVec3 cc{INT32_MIN, 0, 0};
    const Chunk* ch = nullptr;
  };
  std::vector<Cache> cache(nb.size());
  auto at = [&](size_t n, const IVec3& p) -> Vox {
    Cache& c = cache[n];
    const IVec3 q = chunk_of(p);
    if (q != c.cc) {
      c.cc = q;
      c.ch = vg(nb[n]).chunk(q);
    }
    if (!c.ch) return kAir;
    return c.ch->uniform ? c.ch->value : c.ch->v[size_t(chunk_index(p))];
  };
  auto own = [&](int i) -> Vox { return ch->uniform ? ch->value : ch->v[size_t(i)]; };
  for (int i = 0; i < kChunkVox; ++i) {
    if (!vox_solid(own(i))) continue;
    const IVec3 l = local_of(i);
    const IVec3 p{base[0] + l[0], base[1] + l[1], base[2] + l[2]};
    for (int face = 0; face < 6; ++face) {
      const int a = face >> 1, sg = (face & 1) ? 1 : -1;
      // an exposed face: air beyond it in its own grid (a known world: an unknown neighbour holds
      // it already)
      const bool in = l[a] + sg >= 0 && l[a] + sg < kChunk;
      IVec3 q = p;
      q[a] += sg;
      if (in ? vox_solid(own(i + sg * kStride[a])) : vox_solid(G.get(q))) continue;
      if (g == 0 && !in && !chunk_resident(chunk_of(q))) continue;
      for (int sub = 0; sub < S * S; ++sub) {
        if (!ch->jbroken.empty() && G.junction_broken(p, face, sub)) continue;
        f64 pushed = -1.0;
        V3 Xw;
        for (size_t n = 0; n < nb.size(); ++n) {
          const u16 s = nb[n];
          if (const f64 push = reach * h_of(s); push != pushed) {
            Xw = X.to(junction_point(p, face, sub, S, h, push));
            pushed = push;
          }
          if (s != 0 && !inside(Xw, gs(s).lo, gs(s).hi)) continue;
          const IVec3 o = voxel_of(s == 0 ? Xw : xf_of(s).from(Xw), h_of(s));
          if (s == 0 && source_ && !chunk_resident(chunk_of(o))) {
            out.push_back({p, o, g, 0, static_cast<u8>(face), static_cast<u8>(sub), kJunctionUnknown});
            break;
          }
          if (!vox_solid(at(n, o))) continue;
          // (an interface is measured by its owner's faces alone: junction_weight)
          if (!owns(g, s)) break;
          out.push_back({p, o, g, s, static_cast<u8>(face), static_cast<u8>(sub), 0});
          break;
        }
      }
    }
  }
  return js.fwd.emplace(key, std::move(out)).first->second;
}

const std::vector<JSample>* World::junction_rev(JunctionScratch& js, const FragKey& f) {
  const GKey key{f.grid, f.chunk};
  auto it = js.rev.find(key);
  if (it == js.rev.end()) {
    FragChunk* fc = frag_chunk_if(f);
    std::vector<std::vector<JSample>> by(fc ? fc->frags.size() : 0);
    const std::vector<u16> nb = near_grids(f.grid, f.chunk);
    if (fc && !nb.empty()) {
      const f64 h = h_of(f.grid);
      const IVec3 cc = unkey3(f.chunk);
      const V3 lo{h * (cc[0] * kChunk - 0.5), h * (cc[1] * kChunk - 0.5), h * (cc[2] * kChunk - 0.5)};
      const V3 hi{h * ((cc[0] + 1) * kChunk - 0.5), h * ((cc[1] + 1) * kChunk - 0.5), h * ((cc[2] + 1) * kChunk - 0.5)};
      for (u16 s : nb) {
        // the chunk's box in the world, grown by what a sample of the other grid reaches
        V3 wlo, whi;
        world_box(xf_of(f.grid), lo, hi, &wlo, &whi);
        const f64 m = (std::clamp(cfg_.junction_reach, 0.0, 2.0) + 2.0) * std::max(h, h_of(s));
        wlo -= V3{m, m, m};
        whi += V3{m, m, m};
        V3 llo, lhi;
        // (... in the other lattice: its box there)
        LatticeXf inv = inverse(xf_of(s));
        world_box(inv, wlo, whi, &llo, &lhi);
        const IVec3 vlo = voxel_of(llo, h_of(s)), vhi = voxel_of(lhi, h_of(s));
        const VoxelGrid& O = vg(s);
        for (i32 x = vlo[0] >> kChunkBits; x <= vhi[0] >> kChunkBits; ++x)
          for (i32 y = vlo[1] >> kChunkBits; y <= vhi[1] >> kChunkBits; ++y)
            for (i32 z = vlo[2] >> kChunkBits; z <= vhi[2] >> kChunkBits; ++z) {
              const Chunk* oc = O.chunk({x, y, z});
              if (!oc || (oc->uniform && !vox_solid(oc->value))) continue;
              if (s == 0 && !chunk_resident({x, y, z})) continue;
              for (const JSample& j : junction_fwd(js, s, key3(x, y, z))) {
                if (j.og != f.grid || j.kind != 0 || chunk_of(j.o) != cc) continue;
                const i32 fi = fc->at(chunk_index(j.o));
                if (fi >= 0 && fi < static_cast<i32>(by.size())) by[size_t(fi)].push_back(j);
              }
            }
      }
    }
    it = js.rev.emplace(key, std::move(by)).first;
  }
  if (f.idx < 0 || f.idx >= static_cast<i32>(it->second.size())) return nullptr;
  return &it->second[size_t(f.idx)];
}

}  // namespace svx
