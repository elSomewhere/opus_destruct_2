// structvox — the world: fragments, structures, loads, commands, tick (docs/CORE.md).
// Pieces and their fracture: world_pieces.cpp. Streaming, persistence, queries: world_io.cpp.
#include "svx/world/world.hpp"

#include <algorithm>
#include <map>
#include <tuple>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "svx/base/diag.hpp"
#include "svx/base/mem.hpp"
#include "svx/base/parallel.hpp"
#include "archive.hpp"
#include "world_internal.hpp"

namespace svx {

using namespace world_detail;

namespace {
using Clock = std::chrono::steady_clock;
inline f64 ms_since(Clock::time_point t0) { return std::chrono::duration<f64, std::milli>(Clock::now() - t0).count(); }
}  // namespace

World::Impl::Impl(World& self) : self_(&self) {
  strm_.archive = std::make_unique<ChangeArchive>();
  grids_.push_back(std::make_unique<GridState>());  // (the world grid's)
  add_layer({"damage", true, LayerBind::Solid});  // (kDamageLayer)
}
World::Impl::~Impl() = default;

void World::Impl::configure(const WorldConfig& c) {
  cfg_ = c;
  // (guards: a zero or negative knob would stall or divide by zero)
  if (!(cfg_.dt > 0.0)) cfg_.dt = 1.0 / 60.0;
  cfg_.rigid.substeps = std::max(1, cfg_.rigid.substeps);
  cfg_.rigid.mixed_substeps = std::clamp(cfg_.rigid.mixed_substeps, 0, 64);
  cfg_.max_bodies = std::max(0, cfg_.max_bodies);
  cfg_.cluster_nodes = std::max(64, cfg_.cluster_nodes);
  cfg_.body_cluster_nodes = std::max(16, cfg_.body_cluster_nodes);
  cfg_.max_breaks_per_round = std::max(1, cfg_.max_breaks_per_round);
  rigid_.par = cfg_.rigid;
}

void World::Impl::set_params(const WorldParams& p) {
  WorldParams q = p;
  // (non-finite or non-positive knobs are clamped: they would poison every solve)
  auto clamp = [](f64 v, f64 lo, f64 hi, f64 def) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; };
  q.fragility = clamp(q.fragility, 1e-3, 1e3, 1.0);
  q.impact = clamp(q.impact, 0.0, 1e3, 1.0);
  q.dif = clamp(q.dif, 1.0, 10.0, 1.5);
  const bool weaker = q.fragility != par_.fragility;
  par_ = q;
  if (weaker) {
    // every registered structure is judged again under the new strengths
    for (auto& s : structures_) s->solving = true;
  }
}

void World::Impl::load(VoxelGrid&& g) {
  if (in_tick_) return;  // (from inside a tick: refused)
  queue_.clear();
  clear_articulations();
  std::vector<i64> ids;
  for (const auto& b : rigid_.bodies) ids.push_back(b->id);
  remove_bodies(ids, PieceEnd::Removed);
  pw_.pending_add.clear();
  pw_.pending_retire.clear();
  rigid_ = RigidWorld{};
  rigid_.par = cfg_.rigid;
  next_id_ = 1;  // (ids take part in contact ordering: a replay from load() must see the same ones)
  // the old grid's chunks are gone (changed to air), the new ones changed
  std::vector<u64> old_keys;
  for (const auto& [k, c] : grid_.chunks()) old_keys.push_back(k);
  structures_.clear();
  // the oriented grids go too (a level adds its own again)
  for (size_t g = 1; g < grids_.size(); ++g) {
    if (!grids_[g]) continue;
    WorldEvent ev;
    ev.kind = WorldEvent::Kind::GridRemoved;
    ev.id = static_cast<i64>(grids_[g]->id);
    ev.pos = grids_[g]->xf.off;
    ev.rot = grids_[g]->xf.q;
    events_.push_back(std::move(ev));
  }
  grids_.clear();
  grids_.push_back(std::make_unique<GridState>());
  slots_.clear();
  next_grid_ = 1;
  att_.joints.clear();
  rigid_.joints.clear();
  att_.next_joint = 1;
  att_.wheels.clear();
  rigid_.wheels.clear();
  att_.next_wheel = 1;
  steps_ = 0;
  oriented_ = 0;
  ++grid_epoch_;
  solids_.clear();
  grid_dirty_.clear();
  removed_base_.clear();
  strm_.home_grids.clear();
  statics_.clear();
  seeds_.clear();
  warm_u_.clear();
  judged_.clear();
  designed_all_ = false;
  dead_loads_.clear();
  blast_loads_.clear();
  // the grid's layers are the world's (by name: a grid made with layers of its own keeps them)
  for (const LayerSpec& spec : g.layers())
    if (std::none_of(ext_.layers.begin(), ext_.layers.end(), [&](const LayerSpec& l) { return l.name == spec.name; }) &&
        static_cast<int>(ext_.layers.size()) < kMaxLayers)
      ext_.layers.push_back(spec);
  g.adopt_layers(ext_.layers);
  g.sanitize();
  grid_ = std::move(g);
  for (u64 k : old_keys) grid_.mark_dirty(unkey3(k));
  grid_.mark_all_dirty();
  ext_.loads.clear();
  ext_.host_dirty.clear();
  ext_.host_dirty_all = false;
  ext_.sys_changed.clear();
  ext_.sys_generated.clear();
  ext_.sys_evicted.clear();
  strm_.source.reset();
  strm_.generated.clear();
  strm_.column_count.clear();
  strm_.region_resident.clear();
  reset_archive(0);
  strm_.evicted_chunks.clear();  // (the last level's: the host has dropped its chunks)
  touching_.clear();
  strm_.focus.clear();
  strm_.focus_set = false;
  strm_.evict_scan_tick = -1000000;
  strm_.evict_scan_focus.clear();
  const std::vector<WorldEvent> keep = std::move(events_);
  st_ = WorldStats{};
  design_ = DesignReport{};
  events_ = keep;
  grid_.track_changes(true);
  // (by index over the systems there are now, each held while it runs: one may add another)
  for (size_t i = 0, n = ext_.systems.size(); i < n; ++i) {
    const std::shared_ptr<WorldSystem> s = ext_.systems[i];
    s->on_load(*self_);
  }
}

// ---------------------------------------------------------------------------------------------
// Fragments

void World::Impl::mark_owners_stale(u16 g, u64 key, GKey changed) {
  // the structures owning fragments of chunk `key` of grid g: stale, with `changed` (default:
  // that chunk) to patch
  if (changed.grid == 0xFFFF) changed = GKey{g, key};
  const auto& owner = gs(g).owner;
  const auto ot = owner.find(key);
  if (ot == owner.end()) return;
  for (i64 id : ot->second)
    if (id)
      if (Structure* s = structure(id)) {
        s->stale = true;
        if (std::find(s->changed.begin(), s->changed.end(), changed) == s->changed.end()) s->changed.push_back(changed);
      }
}

World::Impl::Structure* World::Impl::structure(i64 id) {
  auto it = std::lower_bound(structures_.begin(), structures_.end(), id,
                             [](const std::unique_ptr<Structure>& x, i64 v) { return x->id < v; });
  return (it != structures_.end() && (*it)->id == id) ? it->get() : nullptr;
}

FragChunk& World::Impl::frag_chunk(u16 g, const IVec3& cc) {
  const u64 key = key3(cc[0], cc[1], cc[2]);
  const Chunk* ch = vg(g).chunk(cc);
  auto& frags = gs(g).frags;
  if (!ch) {
    const auto it = frags.find(key);
    if (it != frags.end()) {
      mark_owners_stale(g, key);  // (evicted / emptied)
      gs(g).owner.erase(key);
      frags.erase(it);
    }
    empty_frags_ = FragChunk{};
    return empty_frags_;
  }
  auto it = frags.find(key);
  if (it != frags.end() && it->second.vox_version == ch->vox_version) {
    it->second.used = st_.ticks;
    return it->second;
  }
  // (a session grid's chunk - a vehicle, an object dropped in - as one fragmented before)
  if (g != 0 && !gs(g).base && !ch->uniform && ch->free_count() > 0) {
    const FragParams par = frag_params(g);
    u64 hsh = key3(cc[0], cc[1], cc[2]) ^ 0x9E3779B97F4A7C15ull;
    for (Vox x : ch->v) hsh = (hsh ^ x) * 0x100000001B3ull;
    for (u8 x : ch->broken) hsh = (hsh ^ x) * 0x100000001B3ull;
    hsh ^= static_cast<u64>(std::llround(par.scale * 65536.0));
    const auto mt = frag_memo_.find(hsh);
    const FragParams& mp = mt != frag_memo_.end() ? mt->second.par : par;
    const bool same_par = mp.min_voxels == par.min_voxels && mp.jitter_lo == par.jitter_lo && mp.jitter_span == par.jitter_span &&
                          mp.noise_scale == par.noise_scale && mp.salt == par.salt && mp.mats == par.mats;
    if (mt != frag_memo_.end() && same_par && mt->second.cc == cc && mt->second.scale == par.scale && mt->second.v == ch->v &&
        mt->second.broken == ch->broken) {
      FragChunk copy = mt->second.frags;
      copy.vox_version = ch->vox_version;
      return adopt_fragments(g, key, std::move(copy));
    }
    FragChunk nf = fragment_chunk(vg(g), cc, par);
    if (frag_memo_.size() >= 96) frag_memo_.clear();  // (a bound: a few dozen kinds of chunk)
    FragMemo& m = frag_memo_[hsh];
    m.cc = cc;
    m.scale = par.scale;
    m.par = par;
    m.v = ch->v;
    m.broken = ch->broken;
    m.frags = nf;
    return adopt_fragments(g, key, std::move(nf));
  }
  return adopt_fragments(g, key, fragment_chunk(vg(g), cc, frag_params(g)));
}

FragChunk& World::Impl::adopt_fragments(u16 g, u64 key, FragChunk&& nf) {
  auto& frags = gs(g).frags;
  const auto it = frags.find(key);
  if (it != frags.end()) {
    const FragChunk& of = it->second;
    bool same = of.id == nf.id && of.frags.size() == nf.frags.size();
    for (size_t f = 0; f < of.frags.size() && same; ++f) same = of.frags[f].count == nf.frags[f].count;
    if (same) {
      it->second.vox_version = nf.vox_version;  // (e.g. a mover changed anchored voxels only)
      return it->second;
    }
  }
  // re-fragmented: the structures holding its old fragments are stale
  mark_owners_stale(g, key);
  gs(g).owner[key].assign(nf.frags.size(), 0);
  FragChunk& slot = frags[key];
  slot = std::move(nf);
  slot.used = st_.ticks;
  return slot;
}

void World::Impl::prefragment(u16 g, const IVec3& seed, f64 max_radius) {
  // The chunks a structure walk from here can reach - face-connected chunks holding free
  // voxels, within its reach - fragmented at once, in parallel (fragment_chunk only reads the
  // grid): the walk then finds them cached. The fragments are the ones the walk would make
  // chunk by chunk.
  constexpr size_t kMaxFlood = 4096, kMin = 4;
  const i32 R = static_cast<i32>(std::ceil(max_radius / (h_of(g) * kChunk))) + 1;
  std::vector<IVec3> queue{seed}, todo;
  std::unordered_set<u64> seen{key3(seed[0], seed[1], seed[2])};
  const VoxelGrid& G = vg(g);
  const auto& frags = gs(g).frags;
  // (a walk crosses into a neighbouring chunk only where free voxels meet across their shared
  // face: a dense city's next building - apart across a street, a gap, the anchored ground - is
  // not fragmented for a walk that never gets there)
  auto free_at = [](const Chunk* c, const IVec3& l) {
    const Vox v = c->uniform ? c->value : c->v[size_t((l[0] * kChunk + l[1]) * kChunk + l[2])];
    return vox_free(v);
  };
  auto meet = [&](const Chunk* a, const Chunk* b, int d) {
    const int ax = d / 2, u = (ax + 1) % 3, w = (ax + 2) % 3;
    IVec3 la{0, 0, 0}, lb{0, 0, 0};
    la[ax] = (d & 1) ? 0 : kChunk - 1;
    lb[ax] = (d & 1) ? kChunk - 1 : 0;
    for (i32 i = 0; i < kChunk; ++i)
      for (i32 j = 0; j < kChunk; ++j) {
        la[u] = lb[u] = i;
        la[w] = lb[w] = j;
        if (free_at(a, la) && free_at(b, lb)) return true;
      }
    return false;
  };
  for (size_t i = 0; i < queue.size() && queue.size() < kMaxFlood; ++i) {
    const IVec3 cc = queue[i];
    const Chunk* ch = G.chunk(cc);
    if (!ch || ch->free_count() == 0) continue;
    // (only chunks never fragmented: a stale cache is rebuilt when the walk comes to it, which
    // is also when the structures holding its old fragments learn of it)
    if (!frags.count(key3(cc[0], cc[1], cc[2]))) todo.push_back(cc);
    for (int d = 0; d < 6; ++d) {
      IVec3 q = cc;
      q[d / 2] += (d & 1) ? -1 : 1;
      if (std::abs(q[0] - seed[0]) > R || std::abs(q[1] - seed[1]) > R || std::abs(q[2] - seed[2]) > R) continue;
      if (seen.count(key3(q[0], q[1], q[2]))) continue;
      const Chunk* qc = G.chunk(q);
      if (!qc || qc->free_count() == 0 || !meet(ch, qc, d)) continue;
      seen.insert(key3(q[0], q[1], q[2]));
      queue.push_back(q);
    }
  }
  if (todo.size() < kMin) return;  // (a few: the walk does them as well)
  std::vector<FragChunk> out(todo.size());
  const FragParams fp = frag_params(g);
  parallel_for(static_cast<i64>(todo.size()), 1, [&](i64 a, i64 b) {
    for (i64 j = a; j < b; ++j) out[size_t(j)] = fragment_chunk(G, todo[size_t(j)], fp);
  });
  for (size_t j = 0; j < todo.size(); ++j) adopt_fragments(g, key3(todo[j][0], todo[j][1], todo[j][2]), std::move(out[j]));
}

FragParams World::Impl::frag_params(u16 g) const {
  FragParams p = cfg_.frag;
  p.mats = mats_.get();
  const f64 h = h_of(g);
  if (h != grid_.h) p.scale = grid_.h / h;  // (the world grid's rubble, in metres)
  return p;
}

FragChunk* World::Impl::frag_chunk_if(u16 g, u64 key) {
  auto& frags = gs(g).frags;
  const auto it = frags.find(key);
  if (it == frags.end()) return nullptr;
  it->second.used = st_.ticks;
  return &it->second;
}

bool World::Impl::frag_at(const GVox& v, FragKey* out) {
  const Vox x = vg(v.grid).get(v.p);
  if (!vox_free(x)) return false;
  const IVec3 cc = chunk_of(v.p);
  FragChunk& fc = frag_chunk(v.grid, cc);
  const i32 f = fc.at(chunk_index(v.p));
  if (f < 0) return false;
  out->chunk = key3(cc[0], cc[1], cc[2]);
  out->idx = f;
  out->grid = v.grid;
  return true;
}

i64 World::Impl::owner_of(const FragKey& f) const {
  const auto& owner = gs(f.grid).owner;
  const auto it = owner.find(f.chunk);
  return (it != owner.end() && f.idx >= 0 && f.idx < static_cast<i32>(it->second.size())) ? it->second[size_t(f.idx)] : 0;
}

V3 World::Impl::frag_com(const FragKey& f) {
  FragChunk* fc = frag_chunk_if(f);
  if (!fc || f.idx < 0 || f.idx >= static_cast<i32>(fc->frags.size())) return V3{};
  const V3& c = fc->frags[size_t(f.idx)].com;
  return f.grid == 0 ? c : xf_of(f.grid).to(c);
}

void World::Impl::voxels_of(const FragKey& f, std::vector<IVec3>& out) {
  FragChunk* fc = frag_chunk_if(f);
  if (!fc || f.idx < 0 || f.idx >= static_cast<i32>(fc->frags.size())) return;
  const IVec3 cc = unkey3(f.chunk);
  const IVec3 base{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
  for (i32 k = fc->vox_start[size_t(f.idx)]; k < fc->vox_start[size_t(f.idx) + 1]; ++k) {
    const i32 i = fc->vox[size_t(k)];
    if (fc->id[size_t(i)] != static_cast<u16>(f.idx + 1)) continue;  // (removed since)
    const IVec3 l = local_of(i);
    out.push_back({base[0] + l[0], base[1] + l[1], base[2] + l[2]});
  }
}

u8 World::Impl::frag_class(const FragKey& f) {
  FragChunk* fc = frag_chunk_if(f);
  if (!fc) return 0;
  const Chunk* ch = vg(f.grid).chunk(unkey3(f.chunk));
  if (!ch || ch->strength.empty()) return 0;
  u8 c = 255;
  for (i32 k = fc->vox_start[size_t(f.idx)]; k < fc->vox_start[size_t(f.idx) + 1]; ++k) {
    const i32 i = fc->vox[size_t(k)];
    if (fc->id[size_t(i)] != static_cast<u16>(f.idx + 1)) continue;
    c = std::min(c, ch->strength[size_t(i)]);
  }
  return c == 255 ? 0 : c;
}

// ---------------------------------------------------------------------------------------------
// Structures: extraction

World::Impl::Structure* World::Impl::extract(const FragKey& seed, i32 max_nodes, f64 max_radius, bool detach_free) {
  if (max_nodes <= 0) max_nodes = cfg_.structure_max_nodes;
  if (max_radius <= 0) max_radius = cfg_.structure_max_radius;
  prefragment(seed.grid, unkey3(seed.chunk), max_radius);
  auto s = std::make_unique<Structure>();
  s->id = next_id_++;
  s->P.mats = mats_.get();
  std::vector<FragKey> members;
  auto nodemap_slot = [&](const FragKey& f) -> i32& {
    auto& v = s->nodemap[GKey{f.grid, f.chunk}];
    if (v.empty()) {
      FragChunk* fc = frag_chunk_if(f);
      v.assign(fc ? fc->frags.size() : size_t(f.idx + 1), -1);
    }
    if (f.idx >= static_cast<i32>(v.size())) v.resize(size_t(f.idx) + 1, -1);
    return v[size_t(f.idx)];
  };
  frag_chunk(seed.grid, unkey3(seed.chunk));
  const V3 seed_pos = frag_com(seed);
  s->centre = seed_pos;
  s->reach = max_radius;
  nodemap_slot(seed) = 0;
  members.push_back(seed);
  std::vector<SecAcc> accs;
  std::unordered_map<u64, i32> acc_index;
  auto acc_for = [&](i32 a, i32 b, int axis, int sign, u16 grid) -> SecAcc& {
    const u64 k = acc_key(a, b, axis, sign);
    auto it = acc_index.find(k);
    if (it != acc_index.end()) return accs[size_t(it->second)];
    acc_index.emplace(k, static_cast<i32>(accs.size()));
    accs.emplace_back();
    SecAcc& A = accs.back();
    A.a = b >= 0 ? std::min(a, b) : a;
    A.b = b >= 0 ? std::max(a, b) : b;
    A.axis = static_cast<u32>(axis);
    A.sign = static_cast<i8>(sign);
    A.grid = grid;
    return A;
  };
  // A fragment met through a face or a junction: a member (its slot), or held fixed (-2)
  auto meet = [&](const FragKey& G) -> i32 {
    i32& slot = nodemap_slot(G);
    if (slot == -1) {
      const V3 gc = frag_com(G);
      // Another registered structure's fragment: the extraction takes that structure over
      // whole (past the radius, which would cut it in two: frontiers next to what happens
      // are artificial supports), up to the node limit. What it cannot take is extracted
      // again (append_nodes: superseded) - but a structure made in this refresh is a
      // frontier: in a world too large for one extraction, the two would otherwise take
      // each other over in turn, every tick.
      const i64 other = owner_of(G);
      const bool owned = other != 0 && other != s->id && structure(other) != nullptr;
      const bool fresh = owned && other >= fresh_from_;
      const bool room = static_cast<i32>(members.size()) < max_nodes;
      if (!fresh && room && (owned || norm(gc - seed_pos) <= max_radius)) {
        slot = static_cast<i32>(members.size());
        members.push_back(G);
      } else {
        slot = -2;  // frontier: held fixed
        s->truncated = true;
      }
    }
    return slot;
  };
  bool any_support = false;
  bool touched_unloaded = false;  // (held by chunks not generated yet)
  IVec3 ulo{INT32_MAX, INT32_MAX, INT32_MAX}, uhi{INT32_MIN, INT32_MIN, INT32_MIN};  // (such chunks reached by junctions)
  JunctionScratch js;
  constexpr int kStride[3] = {kChunk * kChunk, kChunk, 1};
  for (size_t qi = 0; qi < members.size(); ++qi) {
    const FragKey F = members[qi];
    const i32 nF = static_cast<i32>(qi);
    const u16 g = F.grid;
    const VoxelGrid& G = vg(g);
    const IVec3 cc = unkey3(F.chunk);
    FragChunk* fcp = frag_chunk_if(F);
    const Chunk* ch = G.chunk(cc);
    if (!fcp || !ch || F.idx < 0 || F.idx >= static_cast<i32>(fcp->frags.size())) continue;
    const IVec3 base{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
    for (i32 k = fcp->vox_start[size_t(F.idx)]; k < fcp->vox_start[size_t(F.idx) + 1]; ++k) {
      const i32 li = fcp->vox[size_t(k)];
      if (fcp->id[size_t(li)] != static_cast<u16>(F.idx + 1)) continue;
      const IVec3 l = local_of(li);
      const IVec3 p{base[0] + l[0], base[1] + l[1], base[2] + l[2]};
      const u8 brk_p = ch->broken.empty() ? 0 : ch->broken[size_t(li)];
      for (int a = 0; a < 3; ++a)
        for (int sg = -1; sg <= 1; sg += 2) {
          IVec3 q = p;
          q[a] += sg;
          const bool inside = l[a] + sg >= 0 && l[a] + sg < kChunk;
          const int qi2 = li + sg * kStride[a];
          const Vox vq = inside ? (ch->uniform ? ch->value : ch->v[size_t(qi2)]) : G.get(q);
          // a neighbour in a chunk not resident (not generated yet, or evicted) holds it: the
          // world there is unknown, and is not air
          const bool unloaded = g == 0 && !inside && !chunk_resident(chunk_of(q));
          if (!unloaded) {
            if (!vox_solid(vq)) continue;
            const bool face_broken = sg > 0 ? ((brk_p >> a) & 1) != 0 : G.broken(q, a);
            if (face_broken) continue;
          }
          const IVec3 lower = sg > 0 ? p : q;
          if (unloaded) touched_unloaded = true;
          if (vox_anchored(vq) || unloaded) {
            SecAcc& A = acc_for(nF, -1, a, sg, g);
            A.mb = unloaded ? vox_mat(ch->uniform ? ch->value : ch->v[size_t(li)]) : vox_mat(vq);
            A.add(lower, a);
            any_support = true;
            continue;
          }
          FragKey Gk;
          Gk.grid = g;
          if (inside) {
            const i32 g2 = fcp->at(qi2);
            if (g2 < 0 || g2 == F.idx) continue;
            Gk.chunk = F.chunk;
            Gk.idx = g2;
          } else {
            const IVec3 qc = chunk_of(q);
            FragChunk& fq = frag_chunk(g, qc);
            const i32 g2 = fq.at(chunk_index(q));
            if (g2 < 0) continue;
            Gk.chunk = key3(qc[0], qc[1], qc[2]);
            Gk.idx = g2;
          }
          const i32 slot = meet(Gk);
          if (slot >= 0) {
            if (sg > 0) acc_for(nF, slot, a, 1, g).add(lower, a, nF < slot ? 1 : -1);
          } else {
            SecAcc& A = acc_for(nF, -1, a, sg, g);
            A.mb = vox_mat(vq);
            A.add(lower, a);
            any_support = true;
          }
        }
    }
    // Its junctions: bonds to the voxels of other grids its faces meet, and those whose faces
    // meet it. A bond counts both sides' samples half each: a member's own samples come with its
    // walk, the samples of what is not walked (supports, fragments held fixed) with this one's.
    each_junction(js, F, [&](const JSample& j, bool fwd) {
      if (fwd && j.kind == kJunctionUnknown) {
        // (reaching into the unknown world: it holds it)
        SecAcc& A = acc_for(nF, -1, 3 + j.face, 1, g);
        A.mb = vox_mat(G.get(j.v));
        A.add_sample(j, 1, 1.0f);
        any_support = true;
        touched_unloaded = true;
        const IVec3 oc = chunk_of(j.o);
        for (int a = 0; a < 3; ++a) {
          ulo[a] = std::min(ulo[a], oc[a]);
          uhi[a] = std::max(uhi[a], oc[a]);
        }
        return;
      }
      const GVox other = fwd ? GVox{j.o, j.og} : GVox{j.v, j.vg};
      const Vox vo = vg(other.grid).get(other.p);
      if (!vox_solid(vo)) return;
      if (vox_anchored(vo)) {
        SecAcc& A = acc_for(nF, -1, 3 + 6 * other.grid + junction_side(g, j, fwd), 1, g);
        A.mb = vox_mat(vo);
        A.add_sample(j, fwd ? 1 : -1, junction_weight(j));
        any_support = true;
        return;
      }
      FragKey Gk;
      if (!frag_at(other, &Gk)) return;
      const i32 slot = meet(Gk);
      if (slot >= 0) {
        if (fwd) acc_for(nF, slot, 0, 1, g).add_sample(j, nF < slot ? 1 : -1, junction_weight(j));
      } else {
        SecAcc& A = acc_for(nF, -1, 3 + 6 * other.grid + junction_side(g, j, fwd), 1, g);
        A.mb = vox_mat(vo);
        A.add_sample(j, fwd ? 1 : -1, junction_weight(j));
        any_support = true;
      }
    });
  }
  st_.extractions++;
  st_.extracted_nodes += static_cast<i64>(members.size());
  if (!any_support) {
    if (detach_free) {
      // a free piece: it falls
      make_body_from_world(members, V3{}, V3{});
    } else if (jointed(members)) {
      // (bake) a free part a joint holds (a door on its hinge, a ball on a crane's rope): it
      // comes loose as a piece in the first tick, and hangs on the joint
      for (const FragKey& f : members) {
        FragChunk* fc = frag_chunk_if(f);
        if (!fc || f.idx < 0 || f.idx >= static_cast<i32>(fc->frags.size()) || fc->frags[size_t(f.idx)].count <= 0) continue;
        const IVec3 cc = unkey3(f.chunk), l = local_of(fc->frags[size_t(f.idx)].first);
        seeds_.push_back(GVox{{cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]}, f.grid});
      }
    } else {
      // (bake) a piece of the source world that cannot stand: removed, not a gameplay change
      for (const FragKey& f : members) {
        std::vector<IVec3> vox;
        voxels_of(f, vox);
        VoxelGrid& Gf = vg(f.grid);
        const bool tracked = Gf.tracking();
        Gf.track_changes(false);
        for (const IVec3& p : vox) Gf.set(p, kAir);
        Gf.track_changes(tracked);
        design_.floating_voxels += static_cast<i64>(vox.size());
      }
    }
    return nullptr;
  }
  std::vector<i64> superseded;
  append_nodes(*s, members, accs, cluster_cell(static_cast<i64>(members.size())), &superseded);
  std::sort(superseded.begin(), superseded.end());
  superseded.erase(std::unique(superseded.begin(), superseded.end()), superseded.end());
  for (i64 id : superseded)
    if (Structure* o = structure(id)) {
      // (the fragments the new structure did not take lose their owner and are extracted again)
      reseed(*o);
      drop_structure(id);
    }
  structures_.push_back(std::move(s));  // (ids ascend: the list stays sorted)
  Structure* out = structures_.back().get();
  if (detach_free && touches_undesigned(*out)) {
    // A streamed structure touched for the first time. It is designed whole: first the chunks
    // around it are generated (it must not stand on chunks that are not there yet); then, if it
    // is still intact, it is solved under its own weight and its overloaded members strengthened,
    // its bonds taking their new strengths. (A structure damaged before it was designed is left
    // as it is.)
    const i64 id = out->id;
    // (extracted again from the seed's voxel: what is generated meanwhile may fragment the seed's
    // chunk again - a grid at home there displacing its voxels - and the seed is no more if that
    // voxel is gone or taken)
    GVox seed_vox;
    bool seed_known = false;
    if (FragChunk* fc = frag_chunk_if(seed); fc && seed.idx >= 0 && seed.idx < static_cast<i32>(fc->frags.size()) && fc->frags[size_t(seed.idx)].count > 0) {
      const IVec3 cc = unkey3(seed.chunk), l = local_of(fc->frags[size_t(seed.idx)].first);
      seed_vox = GVox{{cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]}, seed.grid};
      seed_known = true;
    }
    auto extract_again = [&]() -> Structure* {
      FragKey again;
      if (!seed_known || !frag_at(seed_vox, &again) || owner_of(again) != 0) return nullptr;
      return extract(again, max_nodes, max_radius, detach_free);
    };
    static const bool dbgd = diag("SVX_DEBUG_DESIGN");
    if (dbgd)
      std::printf("  [first touch] s%lld: %zu nodes, unloaded %d, ensuring %d, pristine %d\n", static_cast<long long>(id), out->P.nodes.size(),
                  touched_unloaded ? 1 : 0, ensuring_, pristine(*out) ? 1 : 0);
    if (touched_unloaded && ensuring_ < 8) {
      IVec3 lo{INT32_MAX, INT32_MAX, INT32_MAX}, hi{INT32_MIN, INT32_MIN, INT32_MIN};
      for (const FragKey& f : out->frags) {
        if (f.idx < 0 || f.grid != 0) continue;
        const IVec3 cc = unkey3(f.chunk);
        for (int a = 0; a < 3; ++a) {
          lo[a] = std::min(lo[a], (cc[a] - 1) * kChunk);
          hi[a] = std::max(hi[a], (cc[a] + 2) * kChunk - 1);
        }
      }
      if (ulo[0] <= uhi[0])
        for (int a = 0; a < 3; ++a) {
          lo[a] = std::min(lo[a], (ulo[a] - 1) * kChunk);
          hi[a] = std::max(hi[a], (uhi[a] + 2) * kChunk - 1);
        }
      drop_structure(id);
      ++ensuring_;
      if (lo[0] <= hi[0]) ensure_chunks(lo, hi);
      Structure* again = extract_again();
      --ensuring_;
      return again;
    }
    if (pristine(*out)) {
      static const bool dbg = diag("SVX_DEBUG_DESIGN");
      design_structure(*out);
      if (cfg_.design_in_place) {
        // (designed in place: extracted again, it would have the same nodes and bonds - and it
        // starts from the design's solution)
        if (dbg) design_structure(*out, true);
        return out;
      }
      // (the reference's way: extracted again with the new strengths, a structure of a new id
      // that starts its solve afresh)
      drop_structure(id);
      Structure* again = extract_again();
      if (dbg && again) design_structure(*again, true);
      return again;
    }
    for (const FragKey& f : out->frags)
      if (f.idx >= 0) gs(f.grid).undesigned.erase(f.chunk);
  }
  return out;
}

bool World::Impl::pristine(const Structure& s) const {
  for (const SBond& B : s.P.bonds)
    if (B.broken) return false;
  // (changed voxels or bonds: a player's; water, burn marks - persistent layers - do not count)
  for (const FragKey& f : s.frags)
    if (f.idx >= 0 && vg(f.grid).voxels_modified(f.chunk)) return false;
  return true;
}

i32 World::Impl::cluster_cell(i64 fragments, i32 limit) const {
  if (limit <= 0) limit = cfg_.cluster_nodes;
  if (fragments <= limit) return 0;
  return fragments <= 6 * static_cast<i64>(limit) ? 8 : 16;
}

void World::Impl::append_nodes(Structure& s, const std::vector<FragKey>& frags, const std::vector<SecAcc>& fine, i32 cell,
                          std::vector<i64>* superseded) {
  const i32 m = static_cast<i32>(frags.size());
  const i32 n0 = static_cast<i32>(s.P.nodes.size());
  s.cell = cell;
  // clusters: fragments in one chunk-local cell (of one grid), connected inside it
  std::vector<u64> key(static_cast<size_t>(m));
  for (i32 f = 0; f < m; ++f) {
    if (cell <= 0) {
      key[size_t(f)] = static_cast<u64>(f);
      continue;
    }
    const FragInfo& fi = frag_chunk_if(frags[size_t(f)])->frags[size_t(frags[size_t(f)].idx)];
    const IVec3 l = local_of(fi.first);
    // (the cell in its chunk, two bits an axis - the reference's packing lost the x cell above the
    // key's six bits: cells a chunk long in x. WorldConfig::cluster_cubes)
    const u64 c = cfg_.cluster_cubes ? static_cast<u64>(((l[0] / cell) << 4) | ((l[1] / cell) << 2) | (l[2] / cell))
                                     : static_cast<u64>((l[0] / cell) * 64 + (l[1] / cell) * 8 + l[2] / cell);
    key[size_t(f)] = mix64(frags[size_t(f)].chunk ^ (c << 58));
  }
  std::vector<std::pair<i32, i32>> links;
  for (const SecAcc& A : fine)
    if (A.a >= 0 && A.a < m && A.b >= 0 && A.b < m) links.push_back({A.a, A.b});
  std::vector<u16> group;
  if (oriented_ > 0) {
    group.resize(size_t(m));
    for (i32 f = 0; f < m; ++f) group[size_t(f)] = frags[size_t(f)].grid;
  }
  std::vector<i32> fnode;
  const i32 K = cluster_items(key, links, &fnode, group.empty() ? nullptr : &group);
  // nodes
  std::vector<std::vector<i32>> members(static_cast<size_t>(K));
  for (i32 f = 0; f < m; ++f) members[size_t(fnode[size_t(f)])].push_back(f);
  for (i32 c = 0; c < K; ++c) {
    const i32 nd = n0 + c;
    SNode node;
    f64 mass = 0.0, heavy = -1.0, strength = 1e30;
    V3 com;
    MaterialId mat = MaterialId::Concrete;
    for (i32 f : members[size_t(c)]) {
      const FragKey& fk = frags[size_t(f)];
      const FragInfo& fi = frag_chunk_if(fk)->frags[size_t(fk.idx)];
      mass += fi.mass;
      com += (fk.grid == 0 ? fi.com : xf_of(fk.grid).to(fi.com)) * fi.mass;
      if (fi.mass > heavy) {
        heavy = fi.mass;
        mat = fi.mat;
      }
      strength = std::min(strength, class_mult(frag_class(fk)));
      s.frags.push_back(fk);
      auto& nm = s.nodemap[GKey{fk.grid, fk.chunk}];
      const size_t nf = frag_chunk_if(fk)->frags.size();
      if (nm.size() < nf) nm.resize(nf, -1);
      nm[size_t(fk.idx)] = nd;
      auto& ov = gs(fk.grid).owner[fk.chunk];
      if (ov.size() < nf) ov.resize(nf, 0);
      if (ov[size_t(fk.idx)] && ov[size_t(fk.idx)] != s.id && superseded) superseded->push_back(ov[size_t(fk.idx)]);
      ov[size_t(fk.idx)] = s.id;
    }
    s.fstart.push_back(static_cast<i32>(s.frags.size()));
    node.c = mass > 0 ? com * (1.0 / mass) : com;
    node.mass = mass;
    s.P.nodes.push_back(node);
    const FragKey& f0 = frags[size_t(members[size_t(c)].front())];
    const FragInfo& fi0 = frag_chunk_if(f0)->frags[size_t(f0.idx)];
    s.ident.push_back(frag_ident_of(f0, fi0.first));
    const IVec3 cc = unkey3(f0.chunk);
    const IVec3 l = local_of(fi0.first);
    s.vox0.push_back(GVox{{cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]}, f0.grid});
    s.weight.push_back(mass * cfg_.rigid.gravity);
    s.nmat.push_back(mat);
    s.nstrength.push_back(strength);
    std::array<f32, 6> w{0, 0, 0, 0, 0, 0};
    const auto it = warm_u_.find(s.ident.back());
    if (it != warm_u_.end()) w = it->second.u;
    for (int q = 0; q < 6; ++q) {
      s.u.push_back(w[size_t(q)]);
      s.ext.push_back(0.0);
      s.ext_solved.push_back(0.0);
      s.acc.push_back(0.0);
      s.peak.push_back(0.0);
      if (s.pending_impact) s.pending.push_back(0.0);
    }
    s.peak_mag.push_back(0.0);
  }
  // bonds
  const i32 S = std::clamp(cfg_.junction_samples, 1, 7);
  auto xf = [this](u16 g) -> const LatticeXf& { return xf_of(g); };
  auto hx = [this](u16 g) { return h_of(g); };
  auto at = [this](u16 g, const IVec3& p) { return voxel_at(GVox{p, g}); };
  const std::vector<SecAcc> merged = merge_accs(fine, [&](i32 e) { return e >= kExisting ? e - kExisting : n0 + fnode[size_t(e)]; });
  for (const SecAcc& A0 : merged) {
    SecAcc A = A0;
    // (the bond's ends in the order of their identities: the same bond gets the same identity
    // and local frame whatever order an extraction found its nodes in - its reference load for
    // sudden changes is found again, and in the same frame)
    if (A.b >= 0 && s.ident[size_t(A.a)] > s.ident[size_t(A.b)]) {
      std::swap(A.a, A.b);
      for (i8& g : A.fsg) g = static_cast<i8>(-g);
      for (i8& g : A.jsg) g = static_cast<i8>(-g);
    }
    const i32 ia = A.a, ib = A.b;
    if (ib >= 0) {
      A.mb = s.nmat[size_t(ib)];
      A.strength_b = s.nstrength[size_t(ib)];
    } else {
      A.strength_b = 1e9;  // (the anchored side never governs)
    }
    const V3* cb = ib >= 0 ? &s.P.nodes[size_t(ib)].c : nullptr;
    SBond B = A.finish(hx, xf, S, s.P.nodes[size_t(ia)].c, cb, s.nmat[size_t(ia)], s.nstrength[size_t(ia)]);
    if (A.js.empty())
      section_strengths(mats(), A.faces.data(), A.fax.data(), A.faces.size(), [&](const IVec3& p) { return voxel_at(GVox{p, A.grid}); }, B);
    else
      section_strengths_general(mats(), A.grid, A.faces.data(), A.fax.data(), A.faces.size(), A.js.data(), A.js.size(), at, B, S);
    B.tag = static_cast<i32>(s.P.bonds.size());
    s.P.bonds.push_back(B);
    for (size_t k = 0; k < A.faces.size(); ++k) {
      s.face_p.push_back(A.faces[k]);
      s.face_axis.push_back(A.fax[k]);
    }
    s.face_start.push_back(static_cast<i32>(s.face_p.size()));
    s.bgrid.push_back(A.grid);
    s.jref.insert(s.jref.end(), A.js.begin(), A.js.end());
    s.jstart.push_back(static_cast<i32>(s.jref.size()));
    s.bid.push_back(bond_ident(s.ident[size_t(ia)], ib >= 0 ? s.ident[size_t(ib)] : 0, ib >= 0 ? 0 : A.axis, ib >= 0 ? 1 : A.sign));
    s.phi.push_back(0.0f);
  }
}

void World::Impl::drop_structure(i64 id) {
  Structure* s = structure(id);
  if (!s) return;
  for (const FragKey& f : s->frags) {
    if (f.idx < 0 || !live(f.grid)) continue;
    auto& owner = gs(f.grid).owner;
    auto it = owner.find(f.chunk);
    if (it == owner.end() || f.idx >= static_cast<i32>(it->second.size())) continue;
    if (it->second[size_t(f.idx)] == id) it->second[size_t(f.idx)] = 0;
  }
  s->dead = true;
  structures_.erase(std::remove_if(structures_.begin(), structures_.end(), [](const std::unique_ptr<Structure>& x) { return x->dead; }),
                    structures_.end());
}

void World::Impl::refresh_structures() {
  struct FreshScope {  // (outside a refresh, no structure counts as fresh)
    i64& f;
    ~FreshScope() { f = INT64_MAX; }
  } fresh_scope{fresh_from_};
  fresh_from_ = next_id_;  // (structures made from here on are this refresh's: extract())
  static const bool prof = diag("SVX_PROFILE");
  for (int round = 0; round < 3; ++round) {
    // (the seeds' chunks first: fragments rebuilt where voxels were cut - the structures that
    // held the old ones go stale, and are patched below, not taken over and extracted again
    // whole: a hole in a tower is a few chunks' work, not the tower's. Unless a structure cut
    // short by its reach would then have its frontier - an artificial support - near what
    // happened: that one is extracted again about it, as a seed's. WorldConfig::patch_cut_structures)
    for (const GVox& p : seeds_) {
      if (!cfg_.patch_cut_structures) break;
      if (!live(p.grid) || !vox_free(vg(p.grid).get(p.p))) continue;
      const IVec3 cc = chunk_of(p.p);
      const FragChunk* fc = frag_chunk_if(p.grid, key3(cc[0], cc[1], cc[2]));
      if (!fc || fc->id.empty()) continue;  // (never fragmented: no structure holds it)
      const u16 id = fc->id[size_t(chunk_index(p.p))];
      if (!id) continue;
      const Structure* o = structure(owner_of(FragKey{key3(cc[0], cc[1], cc[2]), id - 1, p.grid}));
      if (!o || (o->truncated && norm(voxel_centre(p) - o->centre) > 0.5 * o->reach)) continue;
      frag_chunk(p.grid, cc);
    }
    std::vector<i64> stale;
    for (auto& s : structures_)
      if (s->stale) stale.push_back(s->id);
    for (i64 id : stale) {
      Structure* s = structure(id);
      if (!s) continue;
      const auto tpt = Clock::now();
      const size_t nch = s->changed.size();
      if (patch_structure(*s)) {
        s = structure(id);  // (the patch may have detached all of it, or enough to drop it)
        if (!s) continue;
        if (touches_undesigned(*s)) {
          static const bool dbgd = diag("SVX_DEBUG_DESIGN");
          if (dbgd) std::printf("  [patch touch] s%lld: pristine %d\n", static_cast<long long>(id), pristine(*s) ? 1 : 0);
          // (new chunks streamed in under it: designed with them if still intact, else as is)
          if (pristine(*s)) {
            design_structure(*s);
            reseed(*s);
            drop_structure(id);
            continue;
          }
          for (const FragKey& f : s->frags)
            if (f.idx >= 0) gs(f.grid).undesigned.erase(f.chunk);
        }
        if (prof)
          std::printf("  [prof] patch s%lld: %zu changed chunks, %zu nodes (%d gone): %.1f ms\n", static_cast<long long>(id), nch,
                      s->P.nodes.size(), s->gone, ms_since(tpt));
        continue;
      }
      if (prof) std::printf("  [prof] patch s%lld failed: re-extract\n", static_cast<long long>(id));
      reseed(*s);
      drop_structure(id);
    }
    if (seeds_.empty()) break;
    std::vector<GVox> seeds;
    seeds.swap(seeds_);
    for (const GVox& p : seeds) {
      if (!live(p.grid)) continue;
      FragKey f;
      if (!frag_at(p, &f)) continue;
      // owned: its structure is current (stale ones were patched above) or was extracted just now
      if (owner_of(f) != 0) continue;
      const auto tx = Clock::now();
      const i64 prev_owner = owner_of(f);
      Structure* xs = extract(f);
      if (prof && xs)
        std::printf("  [prof] extract s%lld from (%d %d %d) (owner was %lld, round %d): %zu nodes %zu bonds %.1f ms\n",
                    static_cast<long long>(xs->id), p.p[0], p.p[1], p.p[2], static_cast<long long>(prev_owner), round, xs->P.nodes.size(),
                    xs->P.bonds.size(), ms_since(tx));
    }
    bool any_stale = false;
    for (auto& s : structures_) any_stale = any_stale || s->stale;
    if (!any_stale && seeds_.empty()) break;
  }
}

// ---------------------------------------------------------------------------------------------
// Structures: loads, solves, judging

std::vector<f64> World::Impl::load_vector(const Structure& s) const {
  const size_t n = s.P.nodes.size();
  std::vector<f64> f(6 * n, 0.0);
  for (size_t i = 0; i < n; ++i) {
    f[6 * i + 2] = -s.weight[i];
    for (int q = 0; q < 6; ++q) f[6 * i + q] += s.ext_solved[6 * i + q];
  }
  return f;
}

StressOptions World::Impl::solver_options() const {
  // (what the configuration asks of every stress solve: WorldConfig::true_solve_work,
  // coarsen_dense_levels, reaggregate_levels)
  StressOptions so;
  so.amg.dense_work_per_unknown = !cfg_.true_solve_work;
  so.amg.coarsen_dense = cfg_.coarsen_dense_levels;
  so.amg.reaggregate = cfg_.reaggregate_levels;
  return so;
}

void World::Impl::step_structures() {
  static const bool prof = diag("SVX_PROFILE");
  i64 budget = cfg_.stress_work;
  st_.solving = 0;
  st_.solve_nodes = 0;
  // (judging may detach pieces and drop structures: walk by id)
  std::vector<i64> ids;
  for (auto& s : structures_) ids.push_back(s->id);
  for (i64 id : ids) {
    Structure* sp = structure(id);
    if (!sp) continue;
    Structure& s = *sp;
    if (s.rejudge) {
      // (its strengths changed - damage - and its loads did not: judged again as it stands; the
      // judgement reads the bonds and the solution only, not the preconditioner)
      s.rejudge = false;
      if (!s.solving && !s.stale && s.converged && s.u.size() == 6 * s.P.nodes.size()) {
        s.shock = false;
        judge(s);
        continue;
      }
    }
    if (!s.solving || s.stale) {
      ++s.idle;
      continue;
    }
    if (prof && diag("SVX_PROFILE_ALL"))
      std::printf("  [prof] step s%lld: assembled %d running %d budget %lld\n", static_cast<long long>(s.id), s.P.assembled() ? 1 : 0,
                  s.P.running() ? 1 : 0, static_cast<long long>(budget));
    s.idle = 0;
    ++st_.solving;
    st_.solve_nodes += static_cast<i64>(s.P.nodes.size());
    if (budget <= 0) continue;
    static const bool prof = diag("SVX_PROFILE");
    const auto tp = Clock::now();
    if (!s.P.assembled()) {
      StressOptions so = solver_options();
      so.rtol = cfg_.stress_rtol;
      if (s.multigrid) so.amg_min_nodes = 0;
      if (!s.P.assemble(so)) {
        s.solving = false;
        continue;
      }
      // (assembly and hierarchy: counted as work - at their cost, WorldConfig::true_solve_work)
      budget -= cfg_.true_solve_work ? s.P.assembly_work() : 30 * s.P.matrix_blocks();
      if (prof)
        std::printf("  [prof] assemble s%lld: %zu nodes %lld blocks: %.1f ms\n", static_cast<long long>(s.id), s.P.nodes.size(),
                    static_cast<long long>(s.P.matrix_blocks()), ms_since(tp));
    }
    if (!s.P.running()) {
      s.converged = false;
      s.P.begin(load_vector(s), s.u);
      budget -= cfg_.true_solve_work ? s.P.work_per_iteration() : 2 * s.P.matrix_blocks();
    }
    const i64 per = std::max<i64>(1, s.P.work_per_iteration());
    const int maxit = static_cast<int>(std::clamp<i64>(budget / per, 1, 400));
    const auto tq = Clock::now();
    const PcgResult r = s.P.iterate(maxit, cfg_.stress_rtol);
    if (prof && (r.converged || s.run_iters % 20 == 0 || diag("SVX_PROFILE_ALL")))
      std::printf("  [prof] solve s%lld: %d its (run %d, maxit %d, rel %.2e%s, appended %d) %.1f ms (%.2f ms/it, work/it %lld)\n",
                  static_cast<long long>(s.id), r.iters, s.run_iters + r.iters, maxit, r.rel_res, r.converged ? " conv" : "",
                  s.P.appended(), ms_since(tq), ms_since(tq) / std::max(1, r.iters), static_cast<long long>(per));
    budget -= per * std::max(1, r.iters);
    st_.pcg_iters += r.iters;
    if (r.breakdown) {
      // (not positive definite: a mechanism the connectivity pass has not split off yet, or a
      // stale operator) re-assemble from scratch and restart - from what is finite of its start
      // and loads: a value that is not would break every solve down again, tick after tick
      s.P.invalidate();
      s.run_iters = 0;
      for (std::vector<f64>* v : {&s.u, &s.ext, &s.ext_solved})
        for (f64& x : *v)
          if (!std::isfinite(x)) x = 0.0;
      detach_unsupported(s);
      continue;
    }
    // (a preconditioner rebuilt is another only if it is stale - built before bonds broke - or a
    // small structure's block-Jacobi one, which becomes its multigrid: a current one would be
    // built the same again, its solve restarts on it. WorldConfig::rebuild_stale_only)
    const bool rebuild = !cfg_.rebuild_stale_only || !s.P.preconditioner_current() || s.P.block_jacobi();
    const bool diverging = !(r.rel_res < 10.0);
    if (!r.converged && (s.run_iters + r.iters > 120 || (cfg_.restart_diverging_solves && diverging && rebuild))) {
      // a stale preconditioner (after many breaks; diverging: a crash's breaks at once): rebuild
      // it, and restart from here. A small
      // structure's block-Jacobi one that did not converge meets a near-mechanism (a frame left
      // hanging by one face, a member held by a sliver of junction): its multigrid from now on
      // (rigid-body coarse spaces, the coarsest level solved exactly). What converges in neither
      // is no longer solved - never judged, it would take its iterations every tick for good.
      // (A diverged iterate is no start for the next solve - nor what a judgement of it as it
      // stands would read: that starts from where this one did.)
      if (!cfg_.rebuild_stale_only || !diverging) s.P.current(s.u);
      if (rebuild) {
        s.P.invalidate();
        s.multigrid = true;
      } else {
        s.P.stop();
        // (diverged on the operator it would have again: from where it started, it would go the
        // same way - it starts from rest)
        if (cfg_.rebuild_stale_only && diverging) std::fill(s.u.begin(), s.u.end(), 0.0);
      }
      s.run_iters = 0;
      if (++s.restarts >= cfg_.solve_restarts) {
        s.P.stop();
        s.solving = false;
        s.restarts = 0;
        ++st_.solves_abandoned;
      }
      continue;
    }
    if (r.converged) {
      s.restarts = 0;
      s.P.current(s.u);
      s.P.stop();
      s.converged = true;
      ++st_.solves;
      s.solved_at = st_.ticks;
      // a stale preconditioner (many changes since it was built): rebuild it for the next solve
      if (s.P.running() == false && s.run_iters + r.iters > 60 && rebuild) s.P.invalidate();
      s.run_iters = 0;
      judge(s);
    } else {
      s.run_iters += r.iters;
    }
  }
  // idle structures without loads are dropped after a while (their warm start is kept)
  std::vector<i64> drop;
  for (auto& s : structures_) {
    if (s->solving || s->idle < cfg_.idle_drop_ticks) continue;
    bool loaded = false;
    for (f64 v : s->ext)
      if (v != 0.0) {
        loaded = true;
        break;
      }
    if (!loaded) drop.push_back(s->id);
  }
  for (i64 id : drop) drop_structure(id);
}

u64 World::Impl::node_chunk(const Structure& s, i32 node) const {
  const GVox& v = s.vox0[size_t(node)];
  const IVec3 cc = chunk_of(v.p);
  return residency_key(v.grid, key3(cc[0], cc[1], cc[2]));
}

void World::Impl::prune_caches() {
  // The reference loads of judged bonds and the warm starts of fragments outlive their
  // structures (a structure extracted again starts from them). Those of chunks no longer
  // resident go; beyond the budget, only the registered structures' are kept.
  if (strm_.source) {
    for (auto it = judged_.begin(); it != judged_.end();)
      it = resident_key(it->second.chunk) ? std::next(it) : judged_.erase(it);
    for (auto it = warm_u_.begin(); it != warm_u_.end();)
      it = resident_key(it->second.chunk) ? std::next(it) : warm_u_.erase(it);
  }
  const i64 budget = static_cast<i64>(cfg_.memory.cache_mb * 1048576.0);
  if (hash_bytes(judged_, Bytes::Used) + hash_bytes(warm_u_, Bytes::Used) <= budget) return;
  std::unordered_map<u64, Judged> keep_j;
  std::unordered_map<u64, WarmStart> keep_w;
  for (const auto& s : structures_) {
    for (u64 b : s->bid)
      if (const auto it = judged_.find(b); it != judged_.end()) keep_j.emplace(b, it->second);
    for (u64 id : s->ident)
      if (const auto it = warm_u_.find(id); it != warm_u_.end()) keep_w.emplace(id, it->second);
  }
  judged_.swap(keep_j);
  warm_u_.swap(keep_w);
  if (hash_bytes(judged_, Bytes::Used) + hash_bytes(warm_u_, Bytes::Used) > budget) std::unordered_map<u64, WarmStart>().swap(warm_u_);  // (only a speed-up)
}

void World::Impl::judge(Structure& s) {
  const f64 dif = par_.dif;
  // (an impact load case is judged, but it is no state to measure the next change from: the
  // reference loads stay the steady state's, and the steady state is solved again after it)
  const bool transient = s.transient;
  std::vector<std::pair<f64, i32>> over;
  f64 maxphi = 0.0;
  const i32 nb = static_cast<i32>(s.P.bonds.size());
  static const bool dbgj = diag("SVX_DEBUG_JUDGE");
  for (i32 b = 0; b < nb; ++b) {
    const SBond& B = s.P.bonds[size_t(b)];
    if (B.broken) continue;
    const BondLoad L = s.P.bond_load(b, s.u);
    BondLoad Le = L;
    auto it = judged_.find(s.bid[size_t(b)]);
    if (dbgj && s.rounds == 0 && bond_utilization(B, L, par_.fragility, mats()) < 1.0 && it != judged_.end() && s.shock && dif != 1.0 &&
        bond_utilization(B, lerp_load(it->second.load, L, dif), par_.fragility, mats()) >= 1.0)
      std::printf("      (dif) bond %d: static phi %.2f, old N %.0f M %.0f %.0f V %.0f %.0f, new N %.0f M %.0f %.0f V %.0f %.0f\n", b,
                  bond_utilization(B, L, par_.fragility, mats()), it->second.load.N, it->second.load.M1, it->second.load.M2, it->second.load.V1,
                  it->second.load.V2, L.N, L.M1, L.M2, L.V1, L.V2);
    if (dbgj && s.rounds == 0 && it == judged_.end() && bond_utilization(B, L, par_.fragility, mats()) >= 1.0)
      std::printf("      (no baseline) bond %d: static phi %.2f\n", b, bond_utilization(B, L, par_.fragility, mats()));
    if (s.shock && dif != 1.0 && it != judged_.end()) Le = lerp_load(it->second.load, L, dif);
    const f64 phi = bond_utilization(B, Le, par_.fragility, mats());
    if (!transient) {
      if (it != judged_.end()) it->second.load = L;
      else judged_.emplace(s.bid[size_t(b)], Judged{node_chunk(s, B.a), L});
    }
    s.phi[size_t(b)] = static_cast<f32>(phi);
    maxphi = std::max(maxphi, phi);
    if (phi >= 1.0) over.push_back({phi, b});
  }
  st_.max_utilization = maxphi;
  // keep the warm start
  for (size_t i = 0; i < s.P.nodes.size(); ++i) {
    if (s.P.nodes[i].gone) continue;
    std::array<f32, 6> w;
    for (int q = 0; q < 6; ++q) w[size_t(q)] = static_cast<f32>(s.u[6 * i + q]);
    warm_u_[s.ident[i]] = {node_chunk(s, static_cast<i32>(i)), w};
  }
  s.ext_solved = s.pending_impact ? s.pending : s.ext;
  s.transient = s.pending_impact;
  const bool more = s.pending_impact || s.reload || transient;
  s.pending_impact = false;
  s.reload = false;
  if (par_.debug_fields)
    for (const FragKey& f : s.frags)
      if (f.idx >= 0) vg(f.grid).mark_dirty(unkey3(f.chunk));
  if (over.empty() || s.rounds >= cfg_.max_rounds) {
    s.solving = more;  // (a waiting load case, or the steady state after an impact: solve again)
    s.shock = false;
    if (over.empty()) {
      s.rounds = 0;  // (the cascade is over: the next one gets its own rounds)
      s.blast.clear();
    }
    return;
  }
  std::sort(over.begin(), over.end(), [](const auto& x, const auto& y) { return x.first > y.first || (x.first == y.first && x.second < y.second); });
  const f64 thr = std::max(1.0, cfg_.break_band * over.front().first);
  static const bool dbg = diag("SVX_DEBUG_JUDGE");
  if (dbg) {
    {
      f64 W = 0.0, E = 0.0;
      for (size_t i = 0; i < s.P.nodes.size(); ++i) {
        W += s.weight[i];
        E += s.ext_solved[6 * i + 2];
      }
      std::printf("  [judge state s%lld] nodes %zu (gone %d), weight %.4g N, ext z %.4g N, bonds %zu\n", static_cast<long long>(s.id),
                  s.P.nodes.size(), s.gone, W, E, s.P.bonds.size());
    }
    std::printf("  [judge t%lld s%lld] round %d: %zu over 1 (of %d), max %.2f, thr %.2f, shock %d\n", static_cast<long long>(st_.ticks),
                static_cast<long long>(s.id), s.rounds, over.size(), nb, over.front().first, thr, s.shock ? 1 : 0);
    if (diag("SVX_DEBUG_JUDGE_ALL"))
      for (i32 b = 0; b < nb; ++b) {
        const SBond& B = s.P.bonds[size_t(b)];
        if (B.broken) continue;
        const BondLoad L = s.P.bond_load(b, s.u);
        if (std::abs(L.N) + std::hypot(L.V1, L.V2) < 1.5e6) continue;
        std::printf("      [big] b%d at (%.2f %.2f %.2f) n (%.2f %.2f %.2f) faces %d area %.4f sup %d junction %d (%d samples) la %.3f lb %.3f N %.0f V %.0f %.0f M %.0f %.0f\n", b,
                    B.p.x, B.p.y, B.p.z, B.n.x, B.n.y, B.n.z, B.faces, B.area, B.b < 0 ? 1 : 0, s.jstart[size_t(b) + 1] > s.jstart[size_t(b)] ? 1 : 0,
                    s.jstart[size_t(b) + 1] - s.jstart[size_t(b)], B.la, B.lb, L.N, L.V1, L.V2, L.M1, L.M2);
      }
    for (size_t k = 0; k < std::min<size_t>(4, over.size()); ++k) {
      const SBond& B = s.P.bonds[size_t(over[k].second)];
      const BondLoad L = s.P.bond_load(over[k].second, s.u);
      FailMode mode;
      bond_utilization(B, L, par_.fragility, mats(), &mode);
      std::printf("      phi %.2f at (%.2f %.2f %.2f) n (%.2f %.2f %.2f) faces %d area %.4f sup %d mode %d N %.0f V %.0f %.0f T %.0f M %.0f %.0f\n",
                  over[k].first, B.p.x, B.p.y, B.p.z, B.n.x, B.n.y, B.n.z, B.faces, B.area, B.b < 0 ? 1 : 0, static_cast<int>(mode), L.N,
                  L.V1, L.V2, L.T, L.M1, L.M2);
    }
  }
  // a round breaks the worst bonds: those near the maximum, and at least the worst quarter of all
  // that are over strength (a heavily overloaded structure fails in few rounds)
  const size_t quota = std::max<size_t>(1, (over.size() + 3) / 4);
  i32 count = 0;
  std::vector<HingeCut> hinges;
  for (size_t k = 0; k < over.size(); ++k) {
    const f64 phi = over[k].first;
    const i32 b = over[k].second;
    if ((phi < thr && k >= quota) || count >= cfg_.max_breaks_per_round) break;
    // (a ductile section bent past its strength: a plastic hinge, if a part comes loose there)
    HingeCut hc;
    if (cfg_.plastic_hinges && plastic_hinge(s, b, &hc)) hinges.push_back(hc);
    s.P.remove_bond(b);
    const SBond& B = s.P.bonds[size_t(b)];
    break_structure_bond(s, b);
    ++count;
    ++st_.bonds_broken;
    crack_event(B.p, B.n, phi);
  }
  s.shock = true;
  s.solving = true;  // (what the broken bonds carried goes elsewhere: solved again - after a re-judgement too)
  ++s.rounds;
  detach_unsupported(s, hinges.empty() ? nullptr : &hinges);
}

bool World::Impl::plastic_hinge(const Structure& s, i32 bi, HingeCut* out) const {
  const SBond& B = s.P.bonds[size_t(bi)];
  const auto rot = [&](i32 i) { return i < 0 ? V3{} : V3{s.u[6 * size_t(i) + 3], s.u[6 * size_t(i) + 4], s.u[6 * size_t(i) + 5]}; };
  if (!hinge_of(B, s.P.bond_load(bi, s.u), rot(B.a), rot(B.b), out)) return false;
  out->grid = s.vox0[size_t(B.a)].grid;
  return true;
}

bool World::Impl::hinge_of(const SBond& B, const BondLoad& L, const V3& rot_a, const V3& rot_b, HingeCut* out) const {
  const MaterialTable& M = mats();
  if (!M[B.ma].ductile || !M[B.mb].ductile) return false;
  const BondStrength S = bond_strength(B, par_.fragility, M);
  const f64 A = std::max(B.area, 1e-12);
  const f64 sN = L.N / A;
  // (1 / the section's moduli about t1 and t2, as bond_utilization weighs them)
  const f64 w1 = B.s2 > 0 ? B.c2 / B.s2 : 0.0, w2 = B.s1 > 0 ? B.c1 / B.s1 : 0.0;
  const f64 sb = std::abs(L.M1) * w1 + std::abs(L.M2) * w2;
  // bending governs: it is most of what failed the section (not a pull, a shear or a twist)
  const f64 fb = std::max(S.fb, 1e-9), ft = std::max(S.ft, 1e-9), fc = std::max(S.fc, 1e-9);
  const f64 phi_b = sb / fb;
  const f64 phi_n = sN >= 0.0 ? sN / ft : -sN / fc;
  const f64 J = std::max(B.s1 + B.s2, 1e-18);
  const f64 tau = 1.5 * std::sqrt(L.V1 * L.V1 + L.V2 * L.V2) / A + std::abs(L.T) * B.rmax / J;
  const f64 phi_s = tau / std::max(S.coh + S.mu * std::max(0.0, -sN), 1e-9);
  if (!(phi_b > 0.0) || phi_b < 1.5 * phi_n || phi_b < 1.5 * phi_s) return false;
  // the way it turns: the part beyond turned on from where the solve has it (b's rotation less
  // a's; a support does not turn), square to the bond
  V3 w = (B.b < 0 ? V3{} : rot_b) - rot_a;
  w -= B.n * dot(w, B.n);
  const V3 m = B.t1 * L.M1 + B.t2 * L.M2;
  HingeCut h;
  h.a = B.a;
  h.b = B.b;
  h.n = B.n;
  h.p = B.p;
  if (norm2(w) > 1e-30) {
    h.axis = normalized(w);
    // (its pivot: the section's compression edge - where b, turning about it, would press into
    // a - so the parts fold about it rather than grind into each other)
    V3 y = cross(h.axis, B.n);
    const f64 yl = norm(y);
    if (yl > 1e-9) {
      y *= 1.0 / yl;
      const f64 c = std::abs(dot(y, B.t1)) * B.c1 + std::abs(dot(y, B.t2)) * B.c2;
      h.p = B.p - y * (0.9 * c);
    }
  } else if (norm2(m) > 1e-30) {
    h.axis = normalized(m);
  } else {
    return false;
  }
  const f64 a1 = std::abs(dot(h.axis, B.t1)), a2 = std::abs(dot(h.axis, B.t2));
  h.mp = std::max(0.0, cfg_.hinge_shape) * fb / std::max(a1 * w1 + a2 * w2, 1e-12);
  h.pull = ft * A;
  *out = h;
  return true;
}

void World::Impl::break_structure_bond(Structure& s, i32 b) {
  VoxelGrid& G = vg(s.bgrid[size_t(b)]);
  for (i32 k = s.face_start[size_t(b)]; k < s.face_start[size_t(b) + 1]; ++k) G.break_bond(s.face_p[size_t(k)], s.face_axis[size_t(k)]);
  // (a junction breaks sample by sample, each at the voxel whose face it samples)
  for (i32 k = s.jstart[size_t(b)]; k < s.jstart[size_t(b) + 1]; ++k) {
    const JSample& j = s.jref[size_t(k)];
    if (live(j.vg)) vg(j.vg).break_junction(j.v, j.face, j.sub);
  }
}

void World::Impl::crack_event(const V3& p, const V3& n, f64 phi) {
  if (crack_budget_ <= 0) return;
  --crack_budget_;
  WorldEvent ev;
  ev.kind = WorldEvent::Kind::Crack;
  ev.pos = p;
  ev.normal = n;
  ev.strength = phi;
  events_.push_back(std::move(ev));
}

void World::Impl::dust_event(const V3& p, const V3& v, i32 voxels, bool crushed) {
  if (crack_budget_ <= 0) return;
  --crack_budget_;
  WorldEvent ev;
  ev.kind = WorldEvent::Kind::Dust;
  ev.pos = p;
  ev.vel = v;
  ev.voxels = voxels;
  ev.strength = crushed ? 1.0 : 0.0;
  ev.radius = 0.5 * grid_.h * std::cbrt(static_cast<f64>(voxels));
  events_.push_back(std::move(ev));
}

void World::Impl::detach_unsupported(Structure& s, const std::vector<HingeCut>* hinges) {
  const i32 n = static_cast<i32>(s.P.nodes.size());
  std::vector<i32> comp;
  std::vector<u8> supported(size_t(n), 0);
  for (const SBond& B : s.P.bonds)
    if (!B.broken && B.b < 0) supported[size_t(B.a)] = 1;
  const i32 ncomp = graph_components(n, s.P.bonds, supported, &comp);
  std::vector<std::vector<i32>> pieces(static_cast<size_t>(ncomp));
  for (i32 i = 0; i < n; ++i)
    if (comp[size_t(i)] > 0 && !s.P.nodes[size_t(i)].gone) pieces[size_t(comp[size_t(i)])].push_back(i);
  // plastic hinges between what comes loose and what holds it (or between two loose parts): one
  // per pair, its sections' moments summed, at their moment-weighted pivot. Anchored on the
  // voxels either side of the section, before they become pieces (the joint goes with them).
  if (hinges && ncomp > 1) {
    struct Pair {
      f64 mp = 0.0, pull = 0.0;
      V3 p, axis, n;
      u16 grid = 0;
    };
    std::map<std::pair<i32, i32>, Pair> pairs;
    for (const HingeCut& h : *hinges) {
      const i32 ca = h.a >= 0 && h.a < n ? comp[size_t(h.a)] : 0;
      const i32 cb = h.b >= 0 && h.b < n ? comp[size_t(h.b)] : 0;
      if (ca == cb) continue;  // (both still held, or one part: nothing turns there)
      // (oriented from the lower component to the higher)
      const bool flip = ca > cb;
      Pair& P = pairs[{std::min(ca, cb), std::max(ca, cb)}];
      const V3 ax = P.mp > 0.0 && dot(P.axis, h.axis) < 0.0 ? h.axis * -1.0 : h.axis;
      P.p += h.p * h.mp;
      P.axis += ax * h.mp;
      P.n += (flip ? h.n * -1.0 : h.n) * h.mp;
      P.mp += h.mp;
      P.pull += h.pull;
      P.grid = h.grid;
    }
    for (auto& [key, P] : pairs) {
      if (!(P.mp > 0.0) || norm2(P.axis) < 1e-24 || !live(P.grid)) continue;
      const V3 at = P.p * (1.0 / P.mp);
      const V3 nn = norm2(P.n) > 1e-24 ? normalized(P.n) : V3{0, 0, 1};
      const f64 hh = h_of(P.grid);
      JointDesc d;
      d.type = JointType::Hinge;
      d.axis = normalized(P.axis);
      d.a.kind = d.b.kind = JointAnchor::Kind::Grid;
      d.a.id = d.b.id = gs(P.grid).id;
      d.a.point = at - nn * (0.5 * hh);  // (the lower component's side)
      d.b.point = at + nn * (0.5 * hh);
      d.drive.kind = JointDrive::Kind::Speed;
      d.drive.speed = 0.0;
      d.drive.max = P.mp;
      d.break_force = P.pull;
      d.break_angle = std::max(0.0, cfg_.hinge_rotation);
      d.collide = false;
      if (add_joint_impl(d, 0) != 0) ++st_.plastic_hinges;
    }
  }
  std::vector<i32> retire;
  const i64 sid = s.id;
  // (a blast's momentum, while its cascade lasts)
  if (!s.blast.empty() && static_cast<f64>(st_.ticks - s.blast_tick) * cfg_.dt > 1.0) s.blast.clear();
  std::vector<Structure::BlastHit> hits;
  for (i32 c = 1; c < ncomp; ++c) {
    if (pieces[size_t(c)].empty()) continue;
    std::vector<FragKey> frags;
    for (i32 i : pieces[size_t(c)]) {
      for (i32 k = s.fstart[size_t(i)]; k < s.fstart[size_t(i) + 1]; ++k)
        if (s.frags[size_t(k)].idx >= 0) frags.push_back(s.frags[size_t(k)]);
      retire.push_back(i);
    }
    hits.clear();
    for (const Structure::BlastHit& bh : s.blast)
      if (std::find(frags.begin(), frags.end(), bh.f) != frags.end()) hits.push_back(bh);
    Body* b = make_body_from_world(frags, V3{}, V3{});
    if (b && !hits.empty()) {
      const M3 Iinv = b->inv_inertia_world();
      for (const Structure::BlastHit& bh : hits) {
        b->v += bh.J * b->inv_mass;
        b->w += Iinv * cross(bh.at - b->x, bh.J);
      }
      b->v_pre = b->v;
      b->w_pre = b->w;
    }
  }
  if (retire.empty()) return;
  Structure* sp = structure(sid);
  if (!sp) return;
  retire_structure_nodes(*sp, retire);
}

void World::Impl::retire_structure_nodes(Structure& s, const std::vector<i32>& list) {
  s.P.retire_nodes(list);
  for (i32 i : list) {
    for (i32 k = s.fstart[size_t(i)]; k < s.fstart[size_t(i) + 1]; ++k) {
      const FragKey f = s.frags[size_t(k)];
      if (f.idx < 0) continue;
      auto it = s.nodemap.find(GKey{f.grid, f.chunk});
      if (it != s.nodemap.end() && f.idx < static_cast<i32>(it->second.size()) && it->second[size_t(f.idx)] == i)
        it->second[size_t(f.idx)] = -1;
      if (owner_of(f) == s.id) gs(f.grid).owner[f.chunk][size_t(f.idx)] = 0;
      s.frags[size_t(k)] = FragKey{};
    }
    for (int q = 0; q < 6; ++q) {
      s.ext[6 * size_t(i) + q] = 0.0;
      s.ext_solved[6 * size_t(i) + q] = 0.0;
      s.acc[6 * size_t(i) + q] = 0.0;
      s.peak[6 * size_t(i) + q] = 0.0;
      s.u[6 * size_t(i) + q] = 0.0;
      if (s.pending_impact) s.pending[6 * size_t(i) + q] = 0.0;
    }
    s.weight[size_t(i)] = 0.0;
    s.peak_mag[size_t(i)] = 0.0;
    ++s.gone;
  }
  i32 active = static_cast<i32>(s.P.nodes.size()) - s.gone;
  if (active <= 0) {
    drop_structure(s.id);
    return;
  }
  // much retired: extract it again (compact); a stale preconditioner: rebuild it
  if (s.gone > std::max<i32>(64, active / 2)) {
    reseed(s);
    const i64 id = s.id;
    drop_structure(id);
  }
}

void World::Impl::reseed(const Structure& s) {
  // A structure dropped to be extracted again seeds a voxel of each live node and of each of its
  // fragments (where the fragment cache is current): a fragment that cracks cut off inside a
  // cluster is then extracted on its own (and falls) instead of floating, unowned.
  for (size_t i = 0; i < s.vox0.size(); ++i)
    if (!s.P.nodes[i].gone && live(s.vox0[i].grid)) seeds_.push_back(s.vox0[i]);
  for (const FragKey& f : s.frags) {
    if (f.idx < 0 || !live(f.grid)) continue;
    FragChunk* fc = frag_chunk_if(f);
    const IVec3 cc = unkey3(f.chunk);
    const Chunk* ch = vg(f.grid).chunk(cc);
    if (!fc || !ch || fc->vox_version != ch->vox_version || f.idx >= static_cast<i32>(fc->frags.size())) continue;
    for (i32 k = fc->vox_start[size_t(f.idx)]; k < fc->vox_start[size_t(f.idx) + 1]; ++k) {
      const i32 li = fc->vox[size_t(k)];
      if (fc->id[size_t(li)] != static_cast<u16>(f.idx + 1)) continue;
      const IVec3 l = local_of(li);
      seeds_.push_back(GVox{{cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]}, f.grid});
      break;
    }
  }
}

bool World::Impl::patch_structure(Structure& s) {
  std::vector<GKey> changed = s.changed;
  std::sort(changed.begin(), changed.end());
  s.changed.clear();
  s.stale = false;
  auto is_changed = [&](const GKey& k) { return std::binary_search(changed.begin(), changed.end(), k); };
  for (const GKey& k : changed)
    if (!live(k.grid)) return false;  // (a grid gone: extract again)
  // 1. nodes in re-fragmented chunks retire (clusters are chunk-local; the chunks' fragment
  // numbering and ownership were reset when they were rebuilt)
  for (const GKey& k : changed) s.nodemap.erase(k);
  std::vector<i32> retire;
  for (i32 i = 0; i < static_cast<i32>(s.P.nodes.size()); ++i) {
    if (s.P.nodes[size_t(i)].gone) continue;
    bool hit = false;
    for (i32 k = s.fstart[size_t(i)]; k < s.fstart[size_t(i) + 1] && !hit; ++k)
      hit = s.frags[size_t(k)].idx >= 0 && is_changed(GKey{s.frags[size_t(k)].grid, s.frags[size_t(k)].chunk});
    if (hit) retire.push_back(i);
  }
  s.P.retire_nodes(retire);
  for (i32 i : retire) {
    for (i32 k = s.fstart[size_t(i)]; k < s.fstart[size_t(i) + 1]; ++k) {
      FragKey& f = s.frags[size_t(k)];
      if (f.idx < 0) continue;
      if (!is_changed(GKey{f.grid, f.chunk})) {
        // (a cluster reaching out of the chunk: its other fragments are released as well)
        auto it = s.nodemap.find(GKey{f.grid, f.chunk});
        if (it != s.nodemap.end() && f.idx < static_cast<i32>(it->second.size())) it->second[size_t(f.idx)] = -1;
      }
      // (a chunk re-fragmented reset its owners; one whose supports changed kept them)
      if (owner_of(f) == s.id) gs(f.grid).owner[f.chunk][size_t(f.idx)] = 0;
      f = FragKey{};
    }
    for (int q = 0; q < 6; ++q) {
      s.ext[6 * size_t(i) + q] = 0.0;
      s.ext_solved[6 * size_t(i) + q] = 0.0;
      s.acc[6 * size_t(i) + q] = 0.0;
      s.peak[6 * size_t(i) + q] = 0.0;
      s.u[6 * size_t(i) + q] = 0.0;
      if (s.pending_impact) s.pending[6 * size_t(i) + q] = 0.0;
    }
    s.weight[size_t(i)] = 0.0;
    s.peak_mag[size_t(i)] = 0.0;
    ++s.gone;
  }
  const i32 n0 = static_cast<i32>(s.P.nodes.size());
  if (n0 - s.gone <= 0 || s.gone > std::max<i32>(64, (n0 - s.gone) / 2)) return false;  // (mostly changed: extract again)
  // 2. the fragments of those chunks that touch the structure, and what they reach there
  std::vector<FragKey> members;
  std::unordered_map<GKey, std::vector<i32>, GKeyHash> member_of;  // chunk -> member index per fragment (-1)
  for (const GKey& k : changed) {
    FragChunk& fc = frag_chunk(k.grid, unkey3(k.chunk));
    member_of[k].assign(fc.frags.size(), -1);
  }
  JunctionScratch js;
  // fragments released above (outside the changed chunks) are candidates too
  auto touches = [&](const FragKey& F) {
    std::vector<IVec3> vox;
    voxels_of(F, vox);
    const VoxelGrid& G = vg(F.grid);
    for (const IVec3& p : vox)
      for (int a = 0; a < 3; ++a)
        for (int sg = -1; sg <= 1; sg += 2) {
          IVec3 q = p;
          q[a] += sg;
          if (!vox_free(G.get(q))) continue;
          if (sg > 0 ? G.broken(p, a) : G.broken(q, a)) continue;
          FragKey Gk;
          if (!frag_at(GVox{q, F.grid}, &Gk)) continue;
          if (s.node(Gk) >= 0 && !s.P.nodes[size_t(s.node(Gk))].gone) return true;
        }
    bool hit = false;
    each_junction(js, F, [&](const JSample& j, bool fwd) {
      if (hit || (fwd && j.kind == kJunctionUnknown)) return;
      FragKey Gk;
      if (!frag_at(fwd ? GVox{j.o, j.og} : GVox{j.v, j.vg}, &Gk)) return;
      const i32 nd = s.node(Gk);
      hit = nd >= 0 && !s.P.nodes[size_t(nd)].gone;
    });
    return hit;
  };
  auto member_slot = [&](const FragKey& G) -> i32* {
    auto it = member_of.find(GKey{G.grid, G.chunk});
    if (it == member_of.end()) return nullptr;
    if (G.idx >= static_cast<i32>(it->second.size())) it->second.resize(size_t(G.idx) + 1, -1);
    return &it->second[size_t(G.idx)];
  };
  std::vector<FragKey> stack;
  for (const GKey& k : changed) {
    FragChunk* fc = frag_chunk_if(k.grid, k.chunk);
    if (!fc) continue;  // (an empty or evicted chunk)
    for (i32 f = 0; f < static_cast<i32>(fc->frags.size()); ++f) {
      const FragKey F0{k.chunk, f, k.grid};
      if (fc->frags[size_t(f)].count <= 0 || owner_of(F0)) continue;
      if (member_of[k][size_t(f)] >= 0 || !touches(F0)) continue;
      member_of[k][size_t(f)] = static_cast<i32>(members.size());
      members.push_back(F0);
      stack.push_back(F0);
      while (!stack.empty()) {  // flood through the changed chunks
        const FragKey F = stack.back();
        stack.pop_back();
        std::vector<IVec3> vox;
        voxels_of(F, vox);
        const VoxelGrid& G = vg(F.grid);
        for (const IVec3& p : vox)
          for (int a = 0; a < 3; ++a)
            for (int sg = -1; sg <= 1; sg += 2) {
              IVec3 q = p;
              q[a] += sg;
              const IVec3 qc = chunk_of(q);
              if (!is_changed(GKey{F.grid, key3(qc[0], qc[1], qc[2])}) || !vox_free(G.get(q))) continue;
              if (sg > 0 ? G.broken(p, a) : G.broken(q, a)) continue;
              FragKey Gk;
              if (!frag_at(GVox{q, F.grid}, &Gk) || (Gk.chunk == F.chunk && Gk.idx == F.idx) || owner_of(Gk)) continue;
              i32* slot = member_slot(Gk);
              if (!slot || *slot >= 0) continue;
              *slot = static_cast<i32>(members.size());
              members.push_back(Gk);
              stack.push_back(Gk);
            }
        each_junction(js, F, [&](const JSample& j, bool fwd) {
          if (fwd && j.kind == kJunctionUnknown) return;
          FragKey Gk;
          if (!frag_at(fwd ? GVox{j.o, j.og} : GVox{j.v, j.vg}, &Gk) || owner_of(Gk)) return;
          if (!is_changed(GKey{Gk.grid, Gk.chunk})) return;
          i32* slot = member_slot(Gk);
          if (!slot || *slot >= 0) return;
          *slot = static_cast<i32>(members.size());
          members.push_back(Gk);
          stack.push_back(Gk);
        });
      }
    }
  }
  // 3. their bonds: to each other through + faces (counted once), to the structure's other
  // nodes through faces of either side, to supports; and their junctions (a member's own
  // samples with its walk; both sides' where the other side is not walked)
  std::vector<SecAcc> fine;
  std::unordered_map<u64, i32> index;
  auto acc_for = [&](i32 a, i32 b, int axis, int sign, u16 grid) -> SecAcc& {
    const u64 k = acc_key(a, b, axis, sign);
    auto it = index.find(k);
    if (it != index.end()) return fine[size_t(it->second)];
    index.emplace(k, static_cast<i32>(fine.size()));
    fine.emplace_back();
    SecAcc& A = fine.back();
    A.a = b >= 0 ? std::min(a, b) : a;
    A.b = b >= 0 ? std::max(a, b) : b;
    A.axis = static_cast<u32>(axis);
    A.sign = static_cast<i8>(sign);
    A.grid = grid;
    return A;
  };
  for (size_t mi = 0; mi < members.size(); ++mi) {
    const FragKey F = members[mi];
    const i32 eF = static_cast<i32>(mi);
    const VoxelGrid& G = vg(F.grid);
    std::vector<IVec3> vox;
    voxels_of(F, vox);
    for (const IVec3& p : vox)
      for (int a = 0; a < 3; ++a)
        for (int sg = -1; sg <= 1; sg += 2) {
          IVec3 q = p;
          q[a] += sg;
          const Vox vq = G.get(q);
          const bool unloaded = F.grid == 0 && !chunk_resident(chunk_of(q));  // (holds it, as in extract)
          if (!unloaded) {
            if (!vox_solid(vq)) continue;
            if (sg > 0 ? G.broken(p, a) : G.broken(q, a)) continue;
          }
          const IVec3 lower = sg > 0 ? p : q;
          if (vox_anchored(vq) || unloaded) {
            SecAcc& A = acc_for(eF, -1, a, sg, F.grid);
            A.mb = unloaded ? vox_mat(G.get(p)) : vox_mat(vq);
            A.add(lower, a);
            continue;
          }
          FragKey Gk;
          if (!frag_at(GVox{q, F.grid}, &Gk) || (Gk.chunk == F.chunk && Gk.idx == F.idx)) continue;
          i32 eG = -1;
          if (i32* slot = member_slot(Gk); slot && *slot >= 0) {
            if (sg < 0) continue;  // (a member: counted from its + face)
            eG = *slot;
          } else {
            const i32 nd = s.node(Gk);
            if (nd >= 0 && !s.P.nodes[size_t(nd)].gone) eG = kExisting + nd;
          }
          if (eG < 0) {
            // a fragment of no structure here (another one's, a frontier): held fixed
            SecAcc& A = acc_for(eF, -1, a, sg, F.grid);
            A.mb = vox_mat(vq);
            A.add(lower, a);
            continue;
          }
          const i32 low = sg > 0 ? eF : eG;
          acc_for(eF, eG, a, 1, F.grid).add(lower, a, low == std::min(eF, eG) ? 1 : -1);
        }
    each_junction(js, F, [&](const JSample& j, bool fwd) {
      if (fwd && j.kind == kJunctionUnknown) {
        SecAcc& A = acc_for(eF, -1, 3 + j.face, 1, F.grid);
        A.mb = vox_mat(G.get(j.v));
        A.add_sample(j, 1, 1.0f);
        return;
      }
      const GVox other = fwd ? GVox{j.o, j.og} : GVox{j.v, j.vg};
      const Vox vo = vg(other.grid).get(other.p);
      if (!vox_solid(vo)) return;
      i32 eG = -1;
      bool walked = false;
      FragKey Gk;
      if (vox_free(vo) && frag_at(other, &Gk)) {
        if (i32* slot = member_slot(Gk); slot && *slot >= 0) {
          eG = *slot;
          walked = true;
        } else {
          const i32 nd = s.node(Gk);
          if (nd >= 0 && !s.P.nodes[size_t(nd)].gone) eG = kExisting + nd;
        }
      }
      if (eG < 0) {
        // (a support, or a fragment held fixed)
        SecAcc& A = acc_for(eF, -1, 3 + 6 * other.grid + junction_side(F.grid, j, fwd), 1, F.grid);
        A.mb = vox_mat(vo);
        A.add_sample(j, fwd ? 1 : -1, junction_weight(j));
        return;
      }
      if (walked && !fwd) return;  // (a member's own samples come with its walk)
      acc_for(eF, eG, 0, 1, F.grid).add_sample(j, (fwd ? 1 : -1) * (eF < eG ? 1 : -1), junction_weight(j));
    });
  }
  append_nodes(s, members, fine, s.cell, nullptr);
  // 4. the new nodes start from their neighbours' motion; the solver keeps its preconditioner
  // (appended nodes: their own small multigrid) unless much changed
  s.P.extend_warm_start(s.u, n0);
  const i32 active = static_cast<i32>(s.P.nodes.size()) - s.gone;
  const i32 added = static_cast<i32>(s.P.nodes.size()) - n0;
  (void)active;
  (void)added;
  s.P.invalidate();  // (a fresh preconditioner: a stale one converges far slower than it saves)
  s.solving = true;
  s.shock = true;
  detach_unsupported(s);
  return true;
}

// Contact loads of bodies on world fragments (after each substep).
void World::Impl::structure_loads(f64 dt_sub) {
  const auto& cs = rigid_.contacts();
  const f64 imp = par_.impact;
  for (const Contact& c : cs) {
    if (c.b >= 0) continue;
    const V3 F = c.impulse() * (-imp / dt_sub);  // on the world
    if (!finite3(F)) continue;  // (one such force would never decay out of the loads)
    if (!live(c.grid)) continue;
    const GVox wv{c.wvox, c.grid};
    FragKey f;
    if (!frag_at(wv, &f)) continue;
    const i64 o = owner_of(f);
    Structure* s = o ? structure(o) : nullptr;
    if (!s) {
      if (norm(F) > 4.0 * cfg_.load_trigger_abs) seeds_.push_back(wv);
      continue;
    }
    const i32 i = s->node(f);
    if (i < 0) continue;
    const V3 Fl = F, pl = c.p;
    const V3 M = cross(pl - s->P.nodes[size_t(i)].c, Fl);
    f64* a = &s->acc[6 * size_t(i)];
    a[0] += Fl.x;
    a[1] += Fl.y;
    a[2] += Fl.z;
    a[3] += M.x;
    a[4] += M.y;
    a[5] += M.z;
    const f64 mag = norm(Fl);
    if (mag > s->peak_mag[size_t(i)]) {
      s->peak_mag[size_t(i)] = mag;
      f64* pk = &s->peak[6 * size_t(i)];
      pk[0] = Fl.x;
      pk[1] = Fl.y;
      pk[2] = Fl.z;
      pk[3] = M.x;
      pk[4] = M.y;
      pk[5] = M.z;
    }
  }
  if (!att_.joints.empty()) joint_structure_loads(dt_sub);
  if (!att_.wheels.empty()) wheel_structure_loads(dt_sub);
  // dead loads: bodies falling asleep keep their last contact forces on the world (what they
  // hang on keeps its joint's pull: a joint of sleeping bodies carries what it did; a parked car
  // keeps loading the bridge its wheels stand on)
  for (size_t bi = 0; bi < rigid_.bodies.size(); ++bi) {
    Body& b = *rigid_.bodies[bi];
    if (b.asleep && !b.was_asleep) {
      auto& dl = dead_loads_[b.id];
      dl.clear();
      for (const Contact& c : cs)
        if (c.a == static_cast<i32>(bi) && c.b < 0 && finite3(c.impulse()) && live(c.grid)) {
          dl.push_back({GVox{c.wvox, c.grid}, c.p, c.impulse() * (-1.0 / dt_sub)});
          seeds_.push_back(GVox{c.wvox, c.grid});
        }
      for (const Wheel& w : rigid_.wheels)
        if (w.body == b.id && !w.broken && w.contact && w.ground_body == 0 && live(w.ground_grid) && finite3(w.force)) {
          const GVox gv{IVec3{w.ground_voxel[0], w.ground_voxel[1], w.ground_voxel[2]}, w.ground_grid};
          dl.push_back({gv, w.point, w.force * -1.0});
          seeds_.push_back(gv);
        }
      // (asleep, it needs no fracture solver until it is checked again: its operator and
      // multigrid, most of its memory, go - assembled afresh from its bonds at its next check)
      if (cfg_.release_solvers && b.graph) b.graph->P.release();
    } else if (!b.asleep && b.was_asleep) {
      dead_loads_.erase(b.id);
    }
    b.was_asleep = b.asleep;
  }
}

void World::Impl::finish_loads(int substeps) {
  const f64 ema = cfg_.dead_load_ema;
  for (auto& s : structures_)
    for (f64& v : s->acc) v /= std::max(1, substeps);
  // dead loads of sleeping bodies (in body order: the sums are the same on every platform)
  std::vector<i64> sleepers;
  sleepers.reserve(dead_loads_.size());
  for (const auto& [id, list] : dead_loads_) sleepers.push_back(id);
  std::sort(sleepers.begin(), sleepers.end());
  add_external_loads();
  for (i64 id : sleepers)
    for (const DeadLoad& d : dead_loads_.at(id)) {
      FragKey f;
      if (!live(d.vox.grid) || !frag_at(d.vox, &f)) continue;
      Structure* s = structure(owner_of(f));
      if (!s) continue;
      const i32 i = s->node(f);
      if (i < 0) continue;
      const V3 Fl = d.F, pl = d.p;
      const V3 M = cross(pl - s->P.nodes[size_t(i)].c, Fl);
      f64* a = &s->acc[6 * size_t(i)];
      a[0] += Fl.x;
      a[1] += Fl.y;
      a[2] += Fl.z;
      a[3] += M.x;
      a[4] += M.y;
      a[5] += M.z;
    }
  for (auto& sp : structures_) {
    Structure& s = *sp;
    if (s.stale) continue;
    const size_t n = s.P.nodes.size();
    bool trigger = false, impact = false;
    f64 worst = 0.0;  // (the largest change, x its trigger)
    for (size_t i = 0; i < n; ++i) {
      f64* e = &s.ext[6 * i];
      const f64* a = &s.acc[6 * i];
      for (int q = 0; q < 6; ++q) {
        e[q] += ema * (a[q] - e[q]);
        if (!std::isfinite(e[q])) e[q] = std::isfinite(a[q]) ? a[q] : 0.0;  // (an overflow is not kept)
      }
      const f64 thr = cfg_.load_trigger * s.weight[i] + cfg_.load_trigger_abs;
      if (a[0] == 0.0 && a[1] == 0.0 && a[2] == 0.0 && std::abs(e[0]) + std::abs(e[1]) + std::abs(e[2]) < 0.01 * thr)
        for (int q = 0; q < 6; ++q) e[q] = 0.0;  // (unloaded)
      const f64* es = &s.ext_solved[6 * i];
      const f64 d = std::sqrt((e[0] - es[0]) * (e[0] - es[0]) + (e[1] - es[1]) * (e[1] - es[1]) + (e[2] - es[2]) * (e[2] - es[2]));
      if (d > thr) trigger = true;
      worst = std::max(worst, d / thr);
      const f64 steady = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
      if (s.peak_mag[i] > 2.0 * steady + thr) impact = true;
    }
    // A new load case waits for a running solve (restarting it every tick would never converge):
    // the largest pending impact is kept, and the next solve takes it.
    if (impact) {
      if (!s.pending_impact) s.pending = s.ext;
      for (size_t i = 0; i < n; ++i)
        if (s.peak_mag[i] > 0.0) {
          f64* pd = &s.pending[6 * i];
          const f64 old = std::sqrt((pd[0] - s.ext[6 * i]) * (pd[0] - s.ext[6 * i]) + (pd[1] - s.ext[6 * i + 1]) * (pd[1] - s.ext[6 * i + 1]) +
                                    (pd[2] - s.ext[6 * i + 2]) * (pd[2] - s.ext[6 * i + 2]));
          if (!s.pending_impact || s.peak_mag[i] > old)
            for (int q = 0; q < 6; ++q) pd[q] = s.ext[6 * i + q] + s.peak[6 * i + q];
        }
      s.pending_impact = true;
      ++st_.impacts;
    }
    // (a creeping load on a large structure - wheels rolling over a bridge - is solved again at most every
    // load_trigger_gap ticks: its wheels cross a fragment every few ticks, and a solve of the
    // whole bridge each time is wasted work; what it breaks, it breaks a tenth of a second later)
    if (trigger && !impact && worst < 4.0 && s.rounds == 0 && static_cast<i32>(n) >= cfg_.load_trigger_gap_nodes &&
        st_.ticks - s.solved_at < cfg_.load_trigger_gap)
      trigger = false;
    if ((impact || trigger) && !s.P.running()) {
      s.ext_solved = s.pending_impact ? s.pending : s.ext;
      s.transient = s.pending_impact;
      s.pending_impact = false;
      s.rounds = 0;  // (a new load case: a cascade of its own)
      s.solving = true;
    } else if (trigger) {
      s.reload = true;  // (the running solve finishes first)
    }
    std::fill(s.acc.begin(), s.acc.end(), 0.0);
    std::fill(s.peak.begin(), s.peak.end(), 0.0);
    std::fill(s.peak_mag.begin(), s.peak_mag.end(), 0.0);
  }
}

f64 World::Impl::probe_utilization(GridId grid, const IVec3& voxel, i32* over) {
  const i32 g = slot_of(grid);
  if (g < 0 || !in_voxel_range(voxel)) return -1.0;
  FragKey f;
  if (!frag_at(GVox{voxel, static_cast<u16>(g)}, &f)) return -1.0;
  Structure* s = structure(owner_of(f));
  if (!s) s = extract(f);
  if (!s) return -1.0;
  const std::vector<f64> F = load_vector(*s);
  if (!s->P.assembled()) {
    StressOptions so = solver_options();
    so.rtol = cfg_.stress_rtol;
    s->P.assemble(so);
  }
  s->P.solve(F, s->u, 1e-8, 5000, true);
  f64 mx = 0.0;
  i32 cnt = 0;
  i32 worst = -1;
  for (i32 b = 0; b < static_cast<i32>(s->P.bonds.size()); ++b) {
    if (s->P.bonds[size_t(b)].broken) continue;
    const f64 phi = bond_utilization(s->P.bonds[size_t(b)], s->P.bond_load(b, s->u), par_.fragility, mats());
    if (phi > mx) worst = b;
    mx = std::max(mx, phi);
    cnt += phi >= 1.0 ? 1 : 0;
  }
  if (worst >= 0 && diag("SVX_DEBUG_PROBE")) {
    const SBond& B = s->P.bonds[size_t(worst)];
    FailMode mode;
    bond_utilization(B, s->P.bond_load(worst, s->u), par_.fragility, mats(), &mode);
    const BondLoad L = s->P.bond_load(worst, s->u);
    std::printf("  [probe] worst bond at (%.2f %.2f %.2f) n (%.1f %.1f %.1f) mode %d faces %d support %d sectioned %d ft %.2g fb %.2g\n", B.p.x,
                B.p.y, B.p.z, B.n.x, B.n.y, B.n.z, static_cast<int>(mode), B.faces, B.b < 0 ? 1 : 0, B.sectioned ? 1 : 0, B.ft, B.fb);
    std::printf("          area %.4f s1 %.3g s2 %.3g c1 %.3f c2 %.3f la %.3f lb %.3f | N %.0f V %.0f %.0f T %.0f M %.0f %.0f | node a %.0f kg\n", B.area,
                B.s1, B.s2, B.c1, B.c2, B.la, B.lb, L.N, L.V1, L.V2, L.T, L.M1, L.M2, s->P.nodes[size_t(B.a)].mass);
  }
  if (over) *over = cnt;
  return mx;
}

// ---------------------------------------------------------------------------------------------
// Events

bool World::Impl::in_range(const V3& p) const {
  // finite, and within the range of voxel keys (+-2^20 voxels) with room for an event's radius
  const f64 lim = grid_.h * static_cast<f64>(kVoxelLimit);
  return finite3(p) && std::abs(p.x) < lim && std::abs(p.y) < lim && std::abs(p.z) < lim;
}

void World::Impl::carve(const V3& pos, f64 radius) {
  if (!in_range(pos) || !std::isfinite(radius) || radius <= 0.0) return;
  PendingEvent e;
  e.pos = pos;
  e.radius = std::min(radius, cfg_.max_event_radius);
  e.energy = -1.0;  // (a cut)
  queue_.push_back(e);
}

void World::Impl::shoot(const V3& pos, f64 radius, f64 energy) {
  if (!in_range(pos) || !std::isfinite(radius) || radius <= 0.0 || !std::isfinite(energy) || energy < 0.0) return;
  PendingEvent e;
  e.pos = pos;
  e.radius = std::min(radius, cfg_.max_event_radius);
  e.energy = std::min(energy, 1e15);
  queue_.push_back(e);
}

bool World::Impl::penetrates(const Material& M, f64 energy, f64 r, f64 d) const {
  if (M.indestructible) return false;
  if (!cfg_.impact_penetration) return !M.ductile;  // (the reference: carves and craters leave steel and bars)
  if (energy < 0.0 || M.penetration <= 0.0) return true;  // (a cut; or brittle material: any impact)
  // The impact's energy density: its energy over the sphere's volume, concentrated at its centre
  // (1.5 x there, 0.5 x at its edge: a bullet's hole is deepest where it strikes).
  constexpr f64 kSphere = 4.18879020478639098461;  // (4 pi / 3)
  const f64 base = energy / std::max(1e-9, kSphere * r * r * r);
  return base * std::max(0.5, 1.5 - d / std::max(1e-9, r)) >= M.penetration;
}

void World::Impl::blast(const V3& pos, f64 radius, f64 energy) {
  if (!in_range(pos) || !std::isfinite(radius) || radius <= 0.0 || !std::isfinite(energy)) return;
  PendingEvent e;
  e.blast = true;
  e.pos = pos;
  e.radius = std::min(radius, cfg_.max_event_radius);
  e.energy = std::clamp(energy, 0.0, 1e15);
  queue_.push_back(e);
}

i32 World::Impl::set_voxels(const std::vector<VoxelEdit>& in, u32 flags) { return world_set_voxels(0, in, flags); }

i32 World::Impl::set_voxels(GridId grid, const std::vector<VoxelEdit>& in, u32 flags) {
  const i32 g = slot_of(grid);
  if (g < 0) return 0;
  return world_set_voxels(static_cast<u16>(g), in, flags);
}

i32 World::Impl::world_set_voxels(u16 g, const std::vector<VoxelEdit>& in, u32 flags) {
  // (voxels within the key range, valid values)
  std::vector<VoxelEdit> edits;
  edits.reserve(in.size());
  for (const VoxelEdit& e : in)
    if (in_voxel_range(e.p) && vox_valid(e.v)) edits.push_back(e);
  if (edits.empty()) return 0;
  IVec3 lo{INT32_MAX, INT32_MAX, INT32_MAX}, hi{INT32_MIN, INT32_MIN, INT32_MIN};
  for (const VoxelEdit& e : edits)
    for (int a = 0; a < 3; ++a) {
      lo[a] = std::min(lo[a], e.p[a]);
      hi[a] = std::max(hi[a], e.p[a]);
    }
  if (strm_.source && g == 0) {
    // (the chunks of the edits and of their neighbours, in key order - not the box about them all:
    // edits far apart must not make the world between them resident)
    std::vector<u64> keys;
    for (const VoxelEdit& e : edits)
      for (i32 x = (e.p[0] - 1) >> kChunkBits; x <= (e.p[0] + 1) >> kChunkBits; ++x)
        for (i32 y = (e.p[1] - 1) >> kChunkBits; y <= (e.p[1] + 1) >> kChunkBits; ++y)
          for (i32 z = (e.p[2] - 1) >> kChunkBits; z <= (e.p[2] + 1) >> kChunkBits; ++z) keys.push_back(key3(x, y, z));
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    for (u64 k : keys) generate_chunk(k);
  }
  VoxelGrid& G = vg(g);
  const bool tracked = G.tracking();
  if (flags & kEditUntracked) G.track_changes(false);
  std::vector<GVox> changed, unbonded;
  std::vector<GKey> supports;
  const size_t chunks_before = G.chunks().size();
  for (const VoxelEdit& e : edits) {
    const Vox cur = G.get(e.p);
    if (cur == e.v && !(flags & kEditIsolated)) continue;
    const GVox gv{e.p, g};
    if (cur != e.v) {
      if (vox_anchored(cur)) support_changed(gv, &supports);  // (a support of a structure goes)
      G.set(e.p, e.v);
      changed.push_back(gv);
      // what the old voxel held may lose its support (an isolated voxel held nothing)
      if (vox_solid(cur) && (!(flags & kEditIsolated) || vox_free(cur))) unbonded.push_back(gv);
      if (vox_free(e.v)) seeds_.push_back(gv);  // a new free voxel joins or makes a structure
    }
    if ((flags & kEditIsolated) && vox_solid(e.v))
      tear_voxel(gv);
    else if (cur != e.v && vox_anchored(e.v))
      support_changed(gv, &supports);  // (a new support)
  }
  G.track_changes(tracked);
  for (const GKey& k : supports) mark_owners_stale(k.grid, k.chunk);
  if (changed.empty()) return 0;
  if (g != 0 && G.chunks().size() != chunks_before) refresh_grid_box(g);
  seed_near(unbonded);
  // (solid voxels written where they overlap other grids of the body: the lower priority's go)
  if (oriented_ > 0) {
    bool solid = false;
    for (const GVox& c : changed) solid = solid || vox_solid(G.get(c.p));
    if (solid) displace_edits(g, lo, hi, !(flags & kEditUntracked));
  }
  const f64 h = G.h;
  const V3 m{2 * h, 2 * h, 2 * h};
  if (g == 0) {
    rigid_.wake_box(V3{h * lo[0], h * lo[1], h * lo[2]} - m, V3{h * hi[0], h * hi[1], h * hi[2]} + m);
  } else {
    // (the edits' box in the world)
    V3 wlo{INFINITY, INFINITY, INFINITY}, whi{-INFINITY, -INFINITY, -INFINITY};
    for (int c = 0; c < 8; ++c) {
      const V3 w = xf_of(g).to(V3{h * ((c & 1) ? hi[0] : lo[0]), h * ((c & 2) ? hi[1] : lo[1]), h * ((c & 4) ? hi[2] : lo[2])});
      for (int a = 0; a < 3; ++a) {
        wlo[a] = std::min(wlo[a], w[a]);
        whi[a] = std::max(whi[a], w[a]);
      }
    }
    rigid_.wake_box(wlo - m, whi + m);
  }
  return static_cast<i32>(changed.size());
}

bool World::Impl::apply_impulse(i64 id, const V3& point, const V3& J) {
  Body* b = rigid_.find(id);
  if (!b || !in_range(point) || !finite3(J)) return false;
  rigid_.wake(*b);
  b->v += J * b->inv_mass;
  b->w += b->inv_inertia_world() * cross(point - b->x, J);
  return true;
}

bool World::Impl::set_piece_keep(i64 id, bool keep) {
  Body* b = rigid_.find(id);
  if (!b) return false;
  b->keep = keep;
  return true;
}

bool World::Impl::remove_piece(i64 id) {
  const Body* b = rigid_.find(id);
  if (!b || b->is_link()) return false;  // (a link goes with its articulation: remove_articulation)
  remove_bodies({id}, PieceEnd::Removed);
  return true;
}

void World::Impl::support_changed(const GVox& v, std::vector<GKey>* chunks) {
  // the structures holding a free neighbour bonded to v: their supports change (the fragments
  // of free voxels stay as they are, so nothing else would tell them)
  const VoxelGrid& G = vg(v.grid);
  auto note = [&](u16 g, const IVec3& q) {
    const IVec3 cc = chunk_of(q);
    const GKey k{g, key3(cc[0], cc[1], cc[2])};
    if (std::find(chunks->begin(), chunks->end(), k) == chunks->end()) chunks->push_back(k);
  };
  for (int a = 0; a < 3; ++a)
    for (int sg = -1; sg <= 1; sg += 2) {
      IVec3 q = v.p;
      q[a] += sg;
      if (!vox_free(G.get(q))) continue;
      if (!(sg > 0 ? G.bond(v.p, a) : G.bond(q, a))) continue;
      note(v.grid, q);
    }
  // (and those of other grids it may hold through junctions: the chunks around it there)
  if (oriented_ > 0) {
    const f64 hv = G.h;
    const V3 X = xf_of(v.grid).to(V3{hv * v.p[0], hv * v.p[1], hv * v.p[2]});
    const IVec3 cc = chunk_of(v.p);
    for (u16 o : near_grids(v.grid, key3(cc[0], cc[1], cc[2]))) {
      const f64 r = (1.0 + std::clamp(cfg_.junction_reach, 0.0, 2.0)) * std::max(hv, h_of(o));
      const V3 L = o == 0 ? X : xf_of(o).from(X);
      for (int c = 0; c < 8; ++c) {
        const V3 d{(c & 1) ? r : -r, (c & 2) ? r : -r, (c & 4) ? r : -r};
        note(o, voxel_of(L + d, h_of(o)));
      }
    }
  }
}

void World::Impl::carve_world(const V3& c, f64 r, f64 energy, std::vector<GVox>* removed) {
  std::vector<GKey> supports;
  // every grid the sphere reaches: the world grid, and the oriented ones it overlaps
  for (size_t gi = 0; gi < grids_.size(); ++gi) {
    if (!grids_[gi]) continue;
    const u16 g = static_cast<u16>(gi);
    if (g != 0) {
      const GridState& st = gs(g);
      if (!st.any || c.x + r < st.lo.x || c.x - r > st.hi.x || c.y + r < st.lo.y || c.y - r > st.hi.y || c.z + r < st.lo.z ||
          c.z - r > st.hi.z)
        continue;
    }
    VoxelGrid& G = vg(g);
    const f64 h = G.h;
    const i32 R = static_cast<i32>(std::ceil(r / h)) + 1;
    const V3 cl = g == 0 ? c : xf_of(g).from(c);  // (the centre in the grid)
    const IVec3 cv = voxel_of(cl, h);
    const u64 salt = g == 0 ? 0 : mix64(static_cast<u64>(id_of(g)));
    for (i32 x = cv[0] - R; x <= cv[0] + R; ++x)
      for (i32 y = cv[1] - R; y <= cv[1] + R; ++y)
        for (i32 z = cv[2] - R; z <= cv[2] + R; ++z) {
          const V3 p{h * x, h * y, h * z};
          // a slightly jagged crater edge (deterministic per voxel)
          const f64 jag = 1.0 + 0.18 * (unit01(mix64(key3(x, y, z) ^ salt)) - 0.5);
          const f64 d = norm(p - cl);
          if (d > r * jag) continue;
          const IVec3 q{x, y, z};
          const Vox v = G.get(q);
          if (!vox_solid(v)) continue;
          // (an impact removes what it penetrates: bullets hole sheet and brick, bend a section's
          // steel and a column's bars; a rocket holes a section; nothing but a cut goes through plate)
          if (!penetrates(mats()[vox_mat(v)], energy, r, d)) continue;
          if (vox_anchored(v)) support_changed(GVox{q, g}, &supports);
          G.set(q, kAir);
          removed->push_back(GVox{q, g});
        }
  }
  for (const GKey& k : supports) mark_owners_stale(k.grid, k.chunk);
}

void World::Impl::seed_near(const std::vector<GVox>& removed) {
  for (const GVox& v : removed) {
    const VoxelGrid& G = vg(v.grid);
    for (int a = 0; a < 3; ++a)
      for (int s = -1; s <= 1; s += 2) {
        IVec3 q = v.p;
        q[a] += s;
        if (vox_free(G.get(q))) seeds_.push_back(GVox{q, v.grid});
      }
  }
  // (what other grids held through junctions to it: their voxels around it; and the chunk it
  // was in learns of it now - its fragments rebuilt, its owners stale - since the seeds may all
  // be in other grids)
  if (oriented_ > 0) {
    GKey last{0xFFFF, 0};
    for (const GVox& v : removed) {
      const IVec3 cc = chunk_of(v.p);
      const GKey k{v.grid, key3(cc[0], cc[1], cc[2])};
      if (k == last) continue;
      last = k;
      frag_chunk(v.grid, cc);
    }
    for (const GVox& v : removed) {
      const f64 hv = h_of(v.grid);
      const V3 X = xf_of(v.grid).to(V3{hv * v.p[0], hv * v.p[1], hv * v.p[2]});
      const IVec3 cc = chunk_of(v.p);
      for (u16 o : near_grids(v.grid, key3(cc[0], cc[1], cc[2]))) {
        const f64 r = (0.5 + std::clamp(cfg_.junction_reach, 0.0, 2.0)) * std::max(hv, h_of(o));
        seed_fragments_near(o, o == 0 ? X : xf_of(o).from(X), r);
      }
    }
  }
}

void World::Impl::seed_fragments_near(u16 g, const V3& centre, f64 r) {
  // the fragments of grid g whose voxels' box meets the box of half side r around centre (in g's
  // lattice, metres): one seed each (a thin member of a finer grid is not stepped over)
  const f64 h = h_of(g);
  const IVec3 vlo = voxel_of(centre - V3{r, r, r}, h), vhi = voxel_of(centre + V3{r, r, r}, h);
  const VoxelGrid& G = vg(g);
  for (i32 cx = vlo[0] >> kChunkBits; cx <= vhi[0] >> kChunkBits; ++cx)
    for (i32 cy = vlo[1] >> kChunkBits; cy <= vhi[1] >> kChunkBits; ++cy)
      for (i32 cz = vlo[2] >> kChunkBits; cz <= vhi[2] >> kChunkBits; ++cz) {
        const IVec3 cc{cx, cy, cz};
        const Chunk* ch = G.chunk(cc);
        if (!ch || ch->free_count() == 0) continue;
        if (g == 0 && !chunk_resident(cc)) continue;
        const FragChunk& fc = frag_chunk(g, cc);
        const IVec3 base{cx * kChunk, cy * kChunk, cz * kChunk};
        for (const FragInfo& fi : fc.frags) {
          if (fi.count <= 0) continue;
          bool meets = true;
          for (int a = 0; a < 3; ++a)
            meets = meets && base[a] + fi.lo[size_t(a)] <= vhi[a] && base[a] + fi.hi[size_t(a)] >= vlo[a];
          if (!meets) continue;
          const IVec3 l = local_of(fi.first);
          seeds_.push_back(GVox{{base[0] + l[0], base[1] + l[1], base[2] + l[2]}, g});
        }
      }
}

void World::Impl::recheck_vacated(u16 g, const BodyShape& S) {
  // (the neighbours outside the shape - the piece's own cells are gone from the grid - that are
  // free voxels of the grid; seeded unless their fragment is known to belong to a structure)
  const VoxelGrid& G = vg(g);
  IVec3 cached{INT32_MIN, 0, 0};
  const Chunk* ch = nullptr;
  const FragChunk* fc = nullptr;
  u64 ck = 0;
  u64 last_ck = ~0ull;  // (the fragment seeded last: runs of voxels of one fragment seed it once)
  i32 last_f = -1;
  const i32 cells = static_cast<i32>(S.vox.size());
  for (i32 i = 0; i < cells; ++i) {
    if (!vox_solid(S.vox[size_t(i)])) continue;
    const IVec3 p = S.voxel(i);
    for (i32 dx = -1; dx <= 1; ++dx)
      for (i32 dy = -1; dy <= 1; ++dy)
        for (i32 dz = -1; dz <= 1; ++dz) {
          if (dx == 0 && dy == 0 && dz == 0) continue;
          const IVec3 q{p[0] + dx, p[1] + dy, p[2] + dz};
          if (vox_solid(S.get(q))) continue;
          const IVec3 cc = chunk_of(q);
          if (cc != cached) {
            cached = cc;
            ch = G.chunk(cc);
            ck = key3(cc[0], cc[1], cc[2]);
            const auto it = gs(g).frags.find(ck);
            fc = it != gs(g).frags.end() && ch && it->second.vox_version == ch->vox_version ? &it->second : nullptr;
          }
          if (!ch) continue;
          const i32 li = chunk_index(q);
          if (!vox_free(ch->uniform ? ch->value : ch->v[size_t(li)])) continue;
          if (fc) {
            const i32 f = fc->at(li);
            if (f < 0) continue;
            if (owner_of(FragKey{ck, f, g}) != 0) continue;  // (a structure's: it holds on)
            if (ck == last_ck && f == last_f) continue;
            last_ck = ck;
            last_f = f;
          }
          seeds_.push_back(GVox{q, g});
        }
  }
}

void World::Impl::process(const PendingEvent& e) {
  ++st_.events;
  design_near(e.pos, (e.blast ? cfg_.blast_reach : 1.0) * e.radius + 1.0);
  std::vector<GVox> removed;
  // (a blast craters as an impact of its energy; a carve is a cut or a shot)
  carve_world(e.pos, e.radius, e.energy, &removed);
  carve_bodies(e.pos, e.radius, e.energy);
  if (e.blast) {
    blast_world(e);
    blast_bodies(e);
    WorldEvent ev;
    ev.kind = WorldEvent::Kind::Impact;
    ev.pos = e.pos;
    ev.radius = e.radius;
    ev.strength = e.energy;
    events_.push_back(std::move(ev));
  }
  seed_near(removed);
  const f64 h = grid_.h;
  const f64 reach = (e.blast ? cfg_.blast_reach : 1.0) * e.radius + 2 * h;
  rigid_.wake_box(e.pos - V3{reach, reach, reach}, e.pos + V3{reach, reach, reach});
}

void World::Impl::tear_fragment(const FragKey& f, const std::vector<IVec3>& vox) {
  // A fragment torn out of its grid whole (a blast's shard, what a punch knocks out): its faces
  // with the rest are torn - its own between its voxels too without
  // WorldConfig::shards_hold_together (the reference's: a shard of loose voxels, all dust at the
  // next cut)
  VoxelGrid& G = vg(f.grid);
  const FragChunk* fc = cfg_.shards_hold_together ? frag_chunk_if(f) : nullptr;
  if (fc && fc->id.size() != size_t(kChunkVox)) fc = nullptr;
  const IVec3 cc = unkey3(f.chunk);
  const u16 own = static_cast<u16>(f.idx + 1);
  auto mine = [&](const IVec3& q) { return fc && chunk_of(q) == cc && fc->id[size_t(chunk_index(q))] == own; };
  for (const IVec3& p : vox)
    for (int a = 0; a < 3; ++a) {
      IVec3 q = p;
      q[a] += 1;
      if (!mine(q)) G.break_bond(p, a);  // (its +a face)
      q[a] -= 2;
      if (!mine(q)) G.break_bond(q, a);  // (its -a face: q's +a face)
    }
}

void World::Impl::blast_world(const PendingEvent& e) {
  const f64 rs = cfg_.blast_shatter * e.radius, rl = cfg_.blast_reach * e.radius;
  // fragments within the load radius, grid by grid
  std::vector<std::pair<FragKey, f64>> near;  // fragment, distance of its centre
  for (size_t gi = 0; gi < grids_.size(); ++gi) {
    if (!grids_[gi]) continue;
    const u16 g = static_cast<u16>(gi);
    if (g != 0) {
      const GridState& st = gs(g);
      if (!st.any || e.pos.x + rl < st.lo.x || e.pos.x - rl > st.hi.x || e.pos.y + rl < st.lo.y || e.pos.y - rl > st.hi.y ||
          e.pos.z + rl < st.lo.z || e.pos.z - rl > st.hi.z)
        continue;
    }
    const f64 h = h_of(g);
    const i32 R = static_cast<i32>(std::ceil(rl / h)) + 1;
    const IVec3 cv = voxel_of(g == 0 ? e.pos : xf_of(g).from(e.pos), h);
    for (i32 cx = (cv[0] - R) >> kChunkBits; cx <= (cv[0] + R) >> kChunkBits; ++cx)
      for (i32 cy = (cv[1] - R) >> kChunkBits; cy <= (cv[1] + R) >> kChunkBits; ++cy)
        for (i32 cz = (cv[2] - R) >> kChunkBits; cz <= (cv[2] + R) >> kChunkBits; ++cz) {
          const IVec3 cc{cx, cy, cz};
          if (!vg(g).chunk(cc)) continue;
          FragChunk& fc = frag_chunk(g, cc);
          const u64 key = key3(cx, cy, cz);
          for (i32 f = 0; f < static_cast<i32>(fc.frags.size()); ++f) {
            const FragInfo& fi = fc.frags[size_t(f)];
            if (fi.count <= 0) continue;
            const f64 d = norm((g == 0 ? fi.com : xf_of(g).to(fi.com)) - e.pos);
            if (d <= rl) near.push_back({FragKey{key, f, g}, d});
          }
        }
  }
  if (near.empty()) return;
  // shatter: loose pieces near the crater (each its own body) thrown outwards
  auto falloff = [&](f64 d) { return std::min(1.0, (e.radius * e.radius) / std::max(1e-6, d * d)); };
  f64 m_shatter = 0.0, m_loaded = 0.0;
  for (const auto& [f, d] : near) {
    const f64 m = frag_chunk_if(f)->frags[size_t(f.idx)].mass;
    if (d <= rs) m_shatter += m;
    else m_loaded += m * falloff(d) * falloff(d);
  }
  // (the kinetic energy goes to the shattered mass; with little or none of it - a blast in the
  // air, on the anchored ground - to the loaded mass around it, as its falloff has it: never more
  // than the blast has. A blast on a solid shatters far more than its falloff loads.)
  const f64 m_kinetic = std::max(m_shatter, m_loaded);
  const f64 vmax = m_kinetic > 0 ? std::min(cfg_.blast_max_speed, std::sqrt(2.0 * cfg_.blast_kinetic * e.energy / m_kinetic))
                                 : cfg_.blast_max_speed;
  auto speed = [&](f64 d) { return vmax * falloff(d); };
  if (diag("SVX_DEBUG_BLAST"))
    std::printf("  [blast t%lld] at (%.2f %.2f %.2f) r %.2f: %zu fragments near, shatter %.0f kg, loaded %.0f kg, vmax %.1f m/s\n",
                static_cast<long long>(st_.ticks), e.pos.x, e.pos.y, e.pos.z, e.radius, near.size(), m_shatter, m_loaded, vmax);
  std::vector<std::pair<FragKey, f64>> shatter;
  for (const auto& fd : near)
    if (fd.second <= rs) shatter.push_back(fd);
  for (const auto& [f, d0] : shatter) {
    FragChunk* fc = frag_chunk_if(f);
    if (!fc || fc->frags[size_t(f.idx)].count <= 0) continue;
    const FragInfo fi = fc->frags[size_t(f.idx)];
    const V3 com = f.grid == 0 ? fi.com : xf_of(f.grid).to(fi.com);
    const f64 d = std::max(1e-3, norm(com - e.pos));
    const V3 dir = (com - e.pos) * (1.0 / d);
    const u64 hs = mix64(frag_ident_of(f, fi.first) ^ static_cast<u64>(st_.ticks));
    const V3 spin{(unit01(hs) - 0.5) * 12.0, (unit01(mix64(hs ^ 1)) - 0.5) * 12.0, (unit01(mix64(hs ^ 2)) - 0.5) * 12.0};
    // its faces are torn (the piece keeps none of the world's bonds); the structure holding it
    // is patched (the fragment cache is updated in place: nothing else would tell it)
    if (owner_of(f)) mark_owners_stale(f.grid, f.chunk);
    std::vector<IVec3> vox;
    voxels_of(f, vox);
    tear_fragment(f, vox);
    make_body_from_world({f}, dir * speed(d), spin);
    std::vector<GVox> gv;
    gv.reserve(vox.size());
    for (const IVec3& p : vox) gv.push_back(GVox{p, f.grid});
    seed_near(gv);
  }
  // load: the blast's impulse on the structures around it (an impact load case)
  for (const auto& [f, d] : near) {
    if (d <= rs) continue;
    FragChunk* fc = frag_chunk_if(f);
    if (!fc || fc->frags[size_t(f.idx)].count <= 0) continue;
    const FragInfo& fi = fc->frags[size_t(f.idx)];
    const V3 com = f.grid == 0 ? fi.com : xf_of(f.grid).to(fi.com);
    const V3 dir = (com - e.pos) * (1.0 / std::max(1e-3, d));
    blast_loads_.push_back({fi.first, f.chunk, f.grid, dir * (par_.impact * fi.mass * speed(d) / cfg_.dt), com, dir * (fi.mass * speed(d))});
    const IVec3 cc = unkey3(f.chunk);
    const IVec3 l = local_of(fi.first);
    seeds_.push_back(GVox{{cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]}, f.grid});
  }
}

void World::Impl::apply_blast_loads() {
  if (blast_loads_.empty()) return;
  for (const BlastLoad& bl : blast_loads_) {
    const IVec3 cc = unkey3(bl.chunk);
    const IVec3 l = local_of(bl.first);
    FragKey f;
    if (!live(bl.grid) || !frag_at(GVox{{cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]}, bl.grid}, &f)) continue;
    Structure* s = structure(owner_of(f));
    if (!s) continue;
    const i32 i = s->node(f);
    if (i < 0) continue;
    const V3 Fl = bl.F, at = bl.at;
    f64* pk = &s->peak[6 * size_t(i)];
    pk[0] += Fl.x;
    pk[1] += Fl.y;
    pk[2] += Fl.z;
    if (s->fstart[size_t(i) + 1] - s->fstart[size_t(i)] > 1) {
      // (a cluster of fragments: the force acts at its fragment, off the node's centre)
      const V3 M = cross(at - s->P.nodes[size_t(i)].c, Fl);
      pk[3] += M.x;
      pk[4] += M.y;
      pk[5] += M.z;
    }
    s->peak_mag[size_t(i)] = std::sqrt(pk[0] * pk[0] + pk[1] * pk[1] + pk[2] * pk[2]);
    s->shock = true;
    // (what its load breaks off moves off with the momentum the blast gave it)
    if (s->blast_tick != st_.ticks) s->blast.clear();
    s->blast.push_back({f, bl.J, at});
    s->blast_tick = st_.ticks;
  }
  blast_loads_.clear();
}

// ---------------------------------------------------------------------------------------------
// Memory budgets

void World::Impl::enforce_budgets() {
  trim_output();  // (every tick: cheap unless over)
  if (st_.ticks % 30 != 0) return;
  trim_fragment_caches();
  trim_structures();
  prune_caches();
}

void World::Impl::trim_fragment_caches() {
  // Fragment caches are derived from the grid: those of chunks no structure holds are dropped,
  // least recently used first, beyond the budget (rebuilt, identical, when needed again).
  // (The budgets decide by what is used - Bytes::Used: the same on every platform.)
  const i64 budget = static_cast<i64>(cfg_.memory.fragment_cache_mb * 1048576.0);
  i64 bytes = 0;
  for (const auto& gp : grids_)
    if (gp)
      for (const auto& [k, fc] : gp->frags) bytes += fc.memory_bytes(Bytes::Used);
  if (bytes <= budget) return;
  std::vector<std::tuple<i64, u16, u64>> cand;  // (last use, grid, chunk)
  for (size_t g = 0; g < grids_.size(); ++g) {
    if (!grids_[g]) continue;
    const GridState& st = *grids_[g];
    for (const auto& [k, fc] : st.frags) {
      const auto ot = st.owner.find(k);
      bool held = false;
      if (ot != st.owner.end())
        for (i64 id : ot->second) held = held || id != 0;
      if (!held) cand.push_back({fc.used, static_cast<u16>(g), k});
    }
  }
  std::sort(cand.begin(), cand.end());
  for (const auto& [used, g, k] : cand) {
    if (bytes <= budget) break;
    GridState& st = gs(g);
    const auto it = st.frags.find(k);
    bytes -= it->second.memory_bytes(Bytes::Used);
    st.frags.erase(it);
    st.owner.erase(k);
    ++st_.dropped_fragment_caches;
  }
}

i64 World::Impl::structure_bytes(const Structure& s, Bytes k) const {
  i64 b = record_bytes<Structure>(k, 2048) + s.P.memory_bytes(k) + vec_bytes(s.fstart, k) + vec_bytes(s.frags, k) +
          vec_bytes(s.ident, k) + vec_bytes(s.nmat, k) + vec_bytes(s.nstrength, k) + vec_bytes(s.vox0, k) + vec_bytes(s.weight, k) +
          vec_bytes(s.face_start, k) + vec_bytes(s.face_p, k) + vec_bytes(s.face_axis, k) + vec_bytes(s.bgrid, k) + vec_bytes(s.jstart, k) +
          vec_bytes(s.jref, k) + vec_bytes(s.bid, k) + vec_bytes(s.phi, k) + vec_bytes(s.u, k) + vec_bytes(s.ext, k) + vec_bytes(s.ext_solved, k) +
          vec_bytes(s.acc, k) + vec_bytes(s.peak, k) + vec_bytes(s.pending, k) + vec_bytes(s.peak_mag, k) + vec_bytes(s.changed, k);
  // (its node maps: one per chunk it reaches)
  b += k == Bytes::Held ? hash_bytes(s.nodemap) : static_cast<i64>(s.nodemap.size()) * 64;
  for (const auto& [key, v] : s.nodemap) b += vec_bytes(v, k);
  return b;
}

void World::Impl::trim_structures() {
  // Registered structures are extracted again when something happens to them: beyond the
  // budget the idle ones go, the longest idle first (never one being solved).
  const i64 budget = static_cast<i64>(cfg_.memory.structure_mb * 1048576.0);
  i64 bytes = 0;
  std::vector<std::tuple<i32, i64, i64>> cand;  // (-idle, id, bytes)
  for (const auto& sp : structures_) {
    const i64 b = structure_bytes(*sp, Bytes::Used);
    bytes += b;
    if (!sp->solving && !sp->stale) cand.push_back({-sp->idle, sp->id, b});
  }
  if (bytes <= budget) return;
  std::sort(cand.begin(), cand.end());
  for (const auto& [nidle, id, b] : cand) {
    if (bytes <= budget) break;
    drop_structure(id);
    bytes -= b;
    ++st_.dropped_structures;
  }
}

void World::Impl::trim_output() {
  // Output the host does not take is bounded: beyond max_events the oldest cosmetic events go
  // (then the oldest of any kind); evicted chunk keys beyond as many are dropped oldest first.
  const size_t cap = static_cast<size_t>(std::max(64, cfg_.memory.max_events));
  if (events_.size() > cap) {
    const size_t excess = events_.size() - cap;
    size_t cosmetic = 0;
    std::vector<WorldEvent> kept;
    kept.reserve(cap);
    for (WorldEvent& e : events_) {
      const bool cos = e.kind == WorldEvent::Kind::Crack || e.kind == WorldEvent::Kind::Dust || e.kind == WorldEvent::Kind::Impact;
      if (cos && cosmetic < excess) {
        ++cosmetic;
        continue;
      }
      kept.push_back(std::move(e));
    }
    if (kept.size() > cap) kept.erase(kept.begin(), kept.begin() + static_cast<long>(kept.size() - cap));
    st_.dropped_events += static_cast<i64>(events_.size() - kept.size());
    events_.swap(kept);
  }
  if (strm_.evicted_chunks.size() > cap) strm_.evicted_chunks.erase(strm_.evicted_chunks.begin(), strm_.evicted_chunks.begin() + static_cast<long>(strm_.evicted_chunks.size() - cap / 2));
}

// ---------------------------------------------------------------------------------------------
// Tick

void World::Impl::tick() {
  if (in_tick_) return;  // (a system or callback ticking the world from inside its tick)
  in_tick_ = true;
  struct Done {
    bool& f;
    ~Done() { f = false; }
  } done{in_tick_};
  const auto t0 = Clock::now();
  ++st_.ticks;
  stream_update();
  if (par_.paused) {
    // (paused: forces set meanwhile do not pile up, and the systems still hear of streaming)
    for (auto& bp : rigid_.bodies) {
      bp->force = V3{};
      bp->torque = V3{};
    }
    step_systems(false);
    st_.tick_ms = ms_since(t0);
    return;
  }
  const f64 clock0 = time();
  ++steps_;
  crack_budget_ = cfg_.crack_events_per_tick;
  impact_budget_ = cfg_.impact_events_per_tick;
  std::vector<PendingEvent> q;
  q.swap(queue_);
  const auto te = Clock::now();
  for (const PendingEvent& e : q) process(e);
  refresh_structures();
  st_.event_ms = ms_since(te);
  // rigid bodies (with fracture) and their loads on the structures
  const auto tr = Clock::now();
  rigid_.par = cfg_.rigid;
  rigid_.mats = mats_.get();
  // the systems' drives for this tick (what their hosts changed since, too), then the
  // articulations' into the solver
  if (!ext_.systems.empty()) {
    systems_phase_ = true;
    for (size_t i = 0, n = ext_.systems.size(); i < n; ++i) {
      const std::shared_ptr<WorldSystem> s = ext_.systems[i];
      s->pre_step(*self_, cfg_.dt);
    }
    systems_phase_ = false;
  }
  rigid_.begin_tick();
  if (!arts_.empty()) apply_articulation_controls();
  // (the violent part of a collapse, or a large pile settling: one substep a tick - decided once
  // for the tick's substeps and held until the collapse is under two thirds of both thresholds,
  // RigidParams::busy_hold)
  bool busy;
  if (cfg_.rigid.busy_hold) {
    const f64 k = busy_ ? 2.0 / 3.0 : 1.0;
    const i32 bodies = static_cast<i32>(k * cfg_.rigid.busy_bodies);
    busy = rigid_.fast_bodies(bodies) > bodies || static_cast<f64>(rigid_.piece_contacts()) > k * static_cast<f64>(cfg_.rigid.busy_contacts);
    busy_ = busy;
    rigid_.hold_busy(true, busy);
  } else {
    busy = rigid_.busy() || static_cast<i64>(rigid_.piece_contacts()) > cfg_.rigid.busy_contacts;
    rigid_.hold_busy(false, busy);
  }
  int ns = busy ? 1 : std::max(1, cfg_.rigid.substeps);
  // (an articulation solved with the pieces - it touches an awake one: the tick in finer substeps
  // where quality asks for it, RigidParams::mixed_substeps - and the articulations on their own in
  // as many fewer fine steps, at their rate)
  rigid_.link_steps = 0;
  if (cfg_.rigid.mixed_substeps > ns && !arts_.empty() && rigid_.links_mixed(cfg_.dt / ns)) {
    const int m = cfg_.rigid.mixed_substeps;
    rigid_.link_steps = std::max(1, (std::max(1, cfg_.rigid.link_substeps) * ns + m - 1) / m);
    ns = m;
  }
  st_.substeps = ns;
  const f64 dts = cfg_.dt / ns;
  statics_ = static_grids();
  for (int k = 0; k < ns; ++k) {
    if (!att_.joints.empty()) {
      update_joint_ends();  // (their ends where the bodies are now)
      reap_joints();
      rigid_.time = clock0 + static_cast<f64>(k + 1) * dts;  // (the drives' clock)
    }
    if (!att_.wheels.empty()) {
      update_wheel_mounts();  // (their mounts where the carriers are now)
      reap_wheels();
    }
    rigid_.substep(dts, statics_, [this](f64 dt) { return fracture_hook(dt); });
    if (k + 1 == ns && strm_.source) {
      // (what touches what at the step's end, by id: streaming groups what rests on what by it
      // next tick, when the body list - which the contacts index - has changed)
      touching_.clear();
      for (const Contact& c : rigid_.contacts())
        if (c.b >= 0) touching_.push_back({rigid_.bodies[size_t(c.a)]->id, rigid_.bodies[size_t(c.b)]->id});
    }
    structure_loads(dts);
    crumple(dts);  // (what crumpled in this substep's collisions folds)
    if (!att_.joints.empty()) reap_joints();
    if (!att_.wheels.empty()) reap_wheels();
  }
  st_.rigid_ms = ms_since(tr);
  const auto tl = Clock::now();
  refresh_structures();  // (bodies landing on structures not registered yet)
  apply_blast_loads();
  finish_loads(ns);
  const f64 loads_ms = ms_since(tl);
  st_.loads_ms = loads_ms;
  const auto ts = Clock::now();
  step_structures();
  st_.structural_ms = ms_since(ts);
  static const bool tprof = diag("SVX_PROFILE_TICK");
  if (tprof && st_.ticks % 30 == 0)
    std::printf("  [tick] events %.2f rigid %.2f loads %.2f structures %.2f ms\n", st_.event_ms, st_.rigid_ms, loads_ms, st_.structural_ms);
  // bodies that left the world (or whose state is no longer finite: never expected, but one
  // such piece would poison every contact solve it takes part in)
  f64 floor_z = grid_.h * grid_.lo[2] - cfg_.rigid.kill_depth;
  for (size_t g = 1; g < grids_.size(); ++g)
    if (grids_[g] && grids_[g]->any) floor_z = std::min(floor_z, grids_[g]->lo.z - cfg_.rigid.kill_depth);
  if (!arts_.empty()) articulations_out_of_world(floor_z);
  std::vector<i64> out;
  for (const auto& bp : rigid_.bodies) {
    const Body& b = *bp;
    if (b.link) continue;  // (an articulation's: it goes whole, above)
    const bool finite = finite3(b.x) && finite3(b.v) && finite3(b.w) && std::isfinite(b.q.w);
    if (!finite || b.x.z < floor_z || b.count <= 0) out.push_back(b.id);
  }
  if (!out.empty()) remove_bodies(out, PieceEnd::OutOfWorld);
  limit_bodies();
  for (auto& bp : rigid_.bodies) {  // (forces apply for one tick)
    bp->force = V3{};
    bp->torque = V3{};
  }
  const auto tsys = Clock::now();
  step_systems();
  st_.systems_ms = ms_since(tsys);
  flush_body_changes();
  if (solids_.size() > 65536) solids_.clear();  // (the grids' solids in the world's chunks: made again as needed)
  grid_.compact_changed();
  for (size_t g = 1; g < grids_.size(); ++g) {
    if (!grids_[g]) continue;
    grids_[g]->g.compact_changed();
    // (a grid of this session whose voxels are all gone - it fell as pieces, it was blown
    // away - goes; a level's grid stays, empty: that is its change)
    if (!grids_[g]->base && grids_[g]->g.solid_count() == 0) remove_grid_slot(static_cast<u16>(g), true);
  }
  enforce_budgets();
  announce_bodies();
  // (pieces reshaped in place this tick - crumpled - once each: their hosts mesh them again)
  if (!pw_.reshaped.empty()) {
    std::sort(pw_.reshaped.begin(), pw_.reshaped.end());
    pw_.reshaped.erase(std::unique(pw_.reshaped.begin(), pw_.reshaped.end()), pw_.reshaped.end());
    for (i64 id : pw_.reshaped) {
      const Body* b = rigid_.find(id);
      if (!b || !b->announced) continue;
      WorldEvent ev;
      ev.kind = WorldEvent::Kind::PieceReshaped;
      ev.id = id;
      ev.pos = b->x;
      ev.rot = b->q;
      ev.vel = b->v;
      ev.ang = b->w;
      ev.voxels = b->count;
      events_.push_back(std::move(ev));
    }
    pw_.reshaped.clear();
  }
  st_.tick_ms = ms_since(t0);
  st_.upkeep_ms = std::max(0.0, st_.tick_ms - st_.stream_ms - st_.event_ms - st_.rigid_ms - loads_ms - st_.structural_ms - st_.systems_ms);
}

}  // namespace svx
