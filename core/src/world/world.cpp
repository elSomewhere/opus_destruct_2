// structvox — the world: fragments, structures, loads, commands, tick (docs/CORE.md).
// Pieces and their fracture: world_pieces.cpp. Streaming, persistence, queries: world_io.cpp.
#include "svx/world/world.hpp"

#include <algorithm>
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

World::World() : archive_(std::make_unique<ChangeArchive>()) {
  add_layer({"damage", true, LayerBind::Solid});  // (kDamageLayer)
}
World::~World() = default;
World::World(World&&) noexcept = default;
World& World::operator=(World&&) noexcept = default;

void World::configure(const WorldConfig& c) {
  cfg_ = c;
  // (guards: a zero or negative knob would stall or divide by zero)
  if (!(cfg_.dt > 0.0)) cfg_.dt = 1.0 / 60.0;
  cfg_.rigid.substeps = std::max(1, cfg_.rigid.substeps);
  cfg_.max_bodies = std::max(0, cfg_.max_bodies);
  cfg_.cluster_nodes = std::max(64, cfg_.cluster_nodes);
  cfg_.body_cluster_nodes = std::max(16, cfg_.body_cluster_nodes);
  cfg_.max_breaks_per_round = std::max(1, cfg_.max_breaks_per_round);
  rigid_.par = cfg_.rigid;
}

void World::set_params(const WorldParams& p) {
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

void World::load(VoxelGrid&& g) {
  if (in_tick_) return;  // (from inside a tick: refused)
  queue_.clear();
  std::vector<i64> ids;
  for (const auto& b : rigid_.bodies) ids.push_back(b->id);
  remove_bodies(ids, PieceEnd::Removed);
  pending_add_.clear();
  pending_retire_.clear();
  rigid_ = RigidWorld{};
  rigid_.par = cfg_.rigid;
  next_id_ = 1;  // (ids take part in contact ordering: a replay from load() must see the same ones)
  // the old grid's chunks are gone (changed to air), the new ones changed
  std::vector<u64> old_keys;
  for (const auto& [k, c] : grid_.chunks()) old_keys.push_back(k);
  structures_.clear();
  frags_.clear();
  owner_.clear();
  seeds_.clear();
  warm_u_.clear();
  judged_.clear();
  designed_all_ = false;
  dead_loads_.clear();
  blast_loads_.clear();
  undesigned_.clear();
  // the grid's layers are the world's (by name: a grid made with layers of its own keeps them)
  for (const LayerSpec& spec : g.layers())
    if (std::none_of(layer_specs_.begin(), layer_specs_.end(), [&](const LayerSpec& l) { return l.name == spec.name; }) &&
        static_cast<int>(layer_specs_.size()) < kMaxLayers)
      layer_specs_.push_back(spec);
  g.adopt_layers(layer_specs_);
  g.sanitize();
  grid_ = std::move(g);
  for (u64 k : old_keys) grid_.mark_dirty(unkey3(k));
  grid_.mark_all_dirty();
  loads_.clear();
  host_dirty_.clear();
  host_dirty_all_ = false;
  sys_changed_.clear();
  sys_generated_.clear();
  sys_evicted_.clear();
  source_.reset();
  generated_.clear();
  column_count_.clear();
  region_resident_.clear();
  archive_->reset(0);
  focus_.clear();
  focus_set_ = false;
  const std::vector<WorldEvent> keep = std::move(events_);
  st_ = WorldStats{};
  design_ = DesignReport{};
  events_ = keep;
  grid_.track_changes(true);
  for (auto& sys : systems_) sys->on_load(*this);
}

// ---------------------------------------------------------------------------------------------
// Fragments

void World::mark_owners_stale(u64 key, u64 changed) {
  // the structures owning fragments of chunk `key`: stale, with `changed` (default: key) to patch
  if (changed == ~0ull) changed = key;
  const auto ot = owner_.find(key);
  if (ot == owner_.end()) return;
  for (i64 id : ot->second)
    if (id)
      if (Structure* s = structure(id)) {
        s->stale = true;
        if (std::find(s->changed.begin(), s->changed.end(), changed) == s->changed.end()) s->changed.push_back(changed);
      }
}

World::Structure* World::structure(i64 id) {
  auto it = std::lower_bound(structures_.begin(), structures_.end(), id,
                             [](const std::unique_ptr<Structure>& x, i64 v) { return x->id < v; });
  return (it != structures_.end() && (*it)->id == id) ? it->get() : nullptr;
}

FragChunk& World::frag_chunk(const IVec3& cc) {
  const u64 key = key3(cc[0], cc[1], cc[2]);
  const Chunk* ch = grid_.chunk(cc);
  if (!ch) {
    const auto it = frags_.find(key);
    if (it != frags_.end()) {
      mark_owners_stale(key);  // (evicted / emptied)
      owner_.erase(key);
      frags_.erase(it);
    }
    empty_frags_ = FragChunk{};
    return empty_frags_;
  }
  auto it = frags_.find(key);
  if (it != frags_.end() && it->second.vox_version == ch->vox_version) {
    it->second.used = st_.ticks;
    return it->second;
  }
  return adopt_fragments(key, fragment_chunk(grid_, cc, cfg_.frag));
}

FragChunk& World::adopt_fragments(u64 key, FragChunk&& nf) {
  const auto it = frags_.find(key);
  if (it != frags_.end()) {
    const FragChunk& of = it->second;
    bool same = of.id == nf.id && of.frags.size() == nf.frags.size();
    for (size_t f = 0; f < of.frags.size() && same; ++f) same = of.frags[f].count == nf.frags[f].count;
    if (same) {
      it->second.vox_version = nf.vox_version;  // (e.g. a mover changed anchored voxels only)
      return it->second;
    }
  }
  // re-fragmented: the structures holding its old fragments are stale
  mark_owners_stale(key);
  owner_[key].assign(nf.frags.size(), 0);
  FragChunk& slot = frags_[key];
  slot = std::move(nf);
  slot.used = st_.ticks;
  return slot;
}

void World::prefragment(const IVec3& seed, f64 max_radius) {
  // The chunks a structure walk from here can reach - face-connected chunks holding free
  // voxels, within its reach - fragmented at once, in parallel (fragment_chunk only reads the
  // grid): the walk then finds them cached. The fragments are the ones the walk would make
  // chunk by chunk.
  constexpr size_t kMaxFlood = 4096, kMin = 4;
  const i32 R = static_cast<i32>(std::ceil(max_radius / (grid_.h * kChunk))) + 1;
  std::vector<IVec3> queue{seed}, todo;
  std::unordered_set<u64> seen{key3(seed[0], seed[1], seed[2])};
  for (size_t i = 0; i < queue.size() && queue.size() < kMaxFlood; ++i) {
    const IVec3 cc = queue[i];
    const Chunk* ch = grid_.chunk(cc);
    if (!ch || ch->free_count() == 0) continue;
    // (only chunks never fragmented: a stale cache is rebuilt when the walk comes to it, which
    // is also when the structures holding its old fragments learn of it)
    if (!frags_.count(key3(cc[0], cc[1], cc[2]))) todo.push_back(cc);
    for (int d = 0; d < 6; ++d) {
      IVec3 q = cc;
      q[d / 2] += (d & 1) ? -1 : 1;
      if (std::abs(q[0] - seed[0]) > R || std::abs(q[1] - seed[1]) > R || std::abs(q[2] - seed[2]) > R) continue;
      if (seen.insert(key3(q[0], q[1], q[2])).second) queue.push_back(q);
    }
  }
  if (todo.size() < kMin) return;  // (a few: the walk does them as well)
  std::vector<FragChunk> out(todo.size());
  parallel_for(static_cast<i64>(todo.size()), 1, [&](i64 a, i64 b) {
    for (i64 j = a; j < b; ++j) out[size_t(j)] = fragment_chunk(grid_, todo[size_t(j)], cfg_.frag);
  });
  for (size_t j = 0; j < todo.size(); ++j) adopt_fragments(key3(todo[j][0], todo[j][1], todo[j][2]), std::move(out[j]));
}

FragChunk* World::frag_chunk_if(u64 key) {
  const auto it = frags_.find(key);
  if (it == frags_.end()) return nullptr;
  it->second.used = st_.ticks;
  return &it->second;
}

bool World::frag_at(const IVec3& p, FragKey* out) {
  const Vox v = grid_.get(p);
  if (!vox_free(v)) return false;
  const IVec3 cc = chunk_of(p);
  FragChunk& fc = frag_chunk(cc);
  const i32 f = fc.at(chunk_index(p));
  if (f < 0) return false;
  out->chunk = key3(cc[0], cc[1], cc[2]);
  out->idx = f;
  return true;
}

i64 World::owner_of(const FragKey& f) const {
  const auto it = owner_.find(f.chunk);
  return (it != owner_.end() && f.idx >= 0 && f.idx < static_cast<i32>(it->second.size())) ? it->second[size_t(f.idx)] : 0;
}

void World::voxels_of(const FragKey& f, std::vector<IVec3>& out) {
  FragChunk* fc = frag_chunk_if(f.chunk);
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

u8 World::frag_class(const FragKey& f) {
  FragChunk* fc = frag_chunk_if(f.chunk);
  if (!fc) return 0;
  const Chunk* ch = grid_.chunk(unkey3(f.chunk));
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

World::Structure* World::extract(const FragKey& seed, i32 max_nodes, f64 max_radius, bool detach_free) {
  if (max_nodes <= 0) max_nodes = cfg_.structure_max_nodes;
  if (max_radius <= 0) max_radius = cfg_.structure_max_radius;
  prefragment(unkey3(seed.chunk), max_radius);
  auto s = std::make_unique<Structure>();
  s->id = next_id_++;
  std::vector<FragKey> members;
  auto nodemap_slot = [&](const FragKey& f) -> i32& {
    auto& v = s->nodemap[f.chunk];
    if (v.empty()) {
      FragChunk* fc = frag_chunk_if(f.chunk);
      v.assign(fc ? fc->frags.size() : size_t(f.idx + 1), -1);
    }
    if (f.idx >= static_cast<i32>(v.size())) v.resize(size_t(f.idx) + 1, -1);
    return v[size_t(f.idx)];
  };
  FragChunk& sfc = frag_chunk(unkey3(seed.chunk));
  const V3 seed_pos = sfc.frags[size_t(seed.idx)].com;
  nodemap_slot(seed) = 0;
  members.push_back(seed);
  std::vector<SecAcc> accs;
  std::unordered_map<u64, i32> acc_index;
  auto acc_for = [&](i32 a, i32 b, int axis, int sign) -> SecAcc& {
    const u64 k = acc_key(a, b, axis, sign);
    auto it = acc_index.find(k);
    if (it != acc_index.end()) return accs[size_t(it->second)];
    acc_index.emplace(k, static_cast<i32>(accs.size()));
    accs.emplace_back();
    SecAcc& A = accs.back();
    A.a = b >= 0 ? std::min(a, b) : a;
    A.b = b >= 0 ? std::max(a, b) : b;
    A.axis = static_cast<u8>(axis);
    A.sign = static_cast<i8>(sign);
    return A;
  };
  bool any_support = false;
  bool touched_unloaded = false;  // (held by chunks not generated yet)
  constexpr int kStride[3] = {kChunk * kChunk, kChunk, 1};
  for (size_t qi = 0; qi < members.size(); ++qi) {
    const FragKey F = members[qi];
    const i32 nF = static_cast<i32>(qi);
    const IVec3 cc = unkey3(F.chunk);
    FragChunk* fcp = frag_chunk_if(F.chunk);
    const Chunk* ch = grid_.chunk(cc);
    if (!fcp || !ch) continue;
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
          const Vox vq = inside ? (ch->uniform ? ch->value : ch->v[size_t(qi2)]) : grid_.get(q);
          // a neighbour in a chunk not resident (not generated yet, or evicted) holds it: the
          // world there is unknown, and is not air
          const bool unloaded = !inside && !chunk_resident(chunk_of(q));
          if (!unloaded) {
            if (!vox_solid(vq)) continue;
            const bool face_broken = sg > 0 ? ((brk_p >> a) & 1) != 0 : grid_.broken(q, a);
            if (face_broken) continue;
          }
          const IVec3 lower = sg > 0 ? p : q;
          if (unloaded) touched_unloaded = true;
          if (vox_anchored(vq) || unloaded) {
            SecAcc& A = acc_for(nF, -1, a, sg);
            A.mb = unloaded ? vox_mat(ch->uniform ? ch->value : ch->v[size_t(li)]) : vox_mat(vq);
            A.add(lower, a);
            any_support = true;
            continue;
          }
          FragKey G;
          if (inside) {
            const i32 g = fcp->at(qi2);
            if (g < 0 || g == F.idx) continue;
            G = {F.chunk, g};
          } else {
            const IVec3 qc = chunk_of(q);
            FragChunk& fq = frag_chunk(qc);
            const i32 g = fq.at(chunk_index(q));
            if (g < 0) continue;
            G = {key3(qc[0], qc[1], qc[2]), g};
          }
          i32& slot = nodemap_slot(G);
          if (slot == -1) {
            FragChunk* gfc = frag_chunk_if(G.chunk);
            const V3 gc = gfc->frags[size_t(G.idx)].com;
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
          if (slot >= 0) {
            if (sg > 0) acc_for(nF, slot, a, 1).add(lower, a, nF < slot ? 1 : -1);
          } else {
            SecAcc& A = acc_for(nF, -1, a, sg);
            A.mb = vox_mat(vq);
            A.add(lower, a);
            any_support = true;
          }
        }
    }
  }
  st_.extractions++;
  st_.extracted_nodes += static_cast<i64>(members.size());
  if (!any_support) {
    if (detach_free) {
      make_body_from_world(members, V3{}, V3{});  // a free piece: it falls
    } else {
      // (bake) a piece of the source world that cannot stand: removed, not a gameplay change
      std::vector<IVec3> vox;
      for (const FragKey& f : members) voxels_of(f, vox);
      const bool tracked = grid_.tracking();
      grid_.track_changes(false);
      for (const IVec3& p : vox) grid_.set(p, kAir);
      grid_.track_changes(tracked);
      design_.floating_voxels += static_cast<i64>(vox.size());
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
    // and extracted again with their new strengths. (A structure damaged before it was designed
    // is left as it is.)
    const i64 id = out->id;
    static const bool dbgd = diag("SVX_DEBUG_DESIGN");
    if (dbgd)
      std::printf("  [first touch] s%lld: %zu nodes, unloaded %d, ensuring %d, pristine %d\n", static_cast<long long>(id), out->P.nodes.size(),
                  touched_unloaded ? 1 : 0, ensuring_, pristine(*out) ? 1 : 0);
    if (touched_unloaded && ensuring_ < 8) {
      IVec3 lo{INT32_MAX, INT32_MAX, INT32_MAX}, hi{INT32_MIN, INT32_MIN, INT32_MIN};
      for (const FragKey& f : out->frags) {
        if (f.idx < 0) continue;
        const IVec3 cc = unkey3(f.chunk);
        for (int a = 0; a < 3; ++a) {
          lo[a] = std::min(lo[a], (cc[a] - 1) * kChunk);
          hi[a] = std::max(hi[a], (cc[a] + 2) * kChunk - 1);
        }
      }
      drop_structure(id);
      ++ensuring_;
      ensure_chunks(lo, hi);
      Structure* again = extract(seed, max_nodes, max_radius, detach_free);
      --ensuring_;
      return again;
    }
    if (pristine(*out)) {
      design_structure(*out);
      drop_structure(id);
      Structure* again = extract(seed, max_nodes, max_radius, detach_free);
      static const bool dbg = diag("SVX_DEBUG_DESIGN");
      if (dbg && again) design_structure(*again, true);
      return again;
    }
    for (const FragKey& f : out->frags)
      if (f.idx >= 0) undesigned_.erase(f.chunk);
  }
  return out;
}

bool World::pristine(const Structure& s) const {
  for (const SBond& B : s.P.bonds)
    if (B.broken) return false;
  // (changed voxels or bonds: a player's; water, burn marks - persistent layers - do not count)
  for (const FragKey& f : s.frags)
    if (f.idx >= 0 && grid_.voxels_modified(f.chunk)) return false;
  return true;
}

i32 World::cluster_cell(i64 fragments, i32 limit) const {
  if (limit <= 0) limit = cfg_.cluster_nodes;
  if (fragments <= limit) return 0;
  return fragments <= 6 * static_cast<i64>(limit) ? 8 : 16;
}

void World::append_nodes(Structure& s, const std::vector<FragKey>& frags, const std::vector<SecAcc>& fine, i32 cell,
                          std::vector<i64>* superseded) {
  const f64 h = grid_.h;
  const i32 m = static_cast<i32>(frags.size());
  const i32 n0 = static_cast<i32>(s.P.nodes.size());
  s.cell = cell;
  // clusters: fragments in one chunk-local cell, connected inside it
  std::vector<u64> key(static_cast<size_t>(m));
  for (i32 f = 0; f < m; ++f) {
    if (cell <= 0) {
      key[size_t(f)] = static_cast<u64>(f);
      continue;
    }
    const FragInfo& fi = frag_chunk_if(frags[size_t(f)].chunk)->frags[size_t(frags[size_t(f)].idx)];
    const IVec3 l = local_of(fi.first);
    key[size_t(f)] = mix64(frags[size_t(f)].chunk ^ (static_cast<u64>((l[0] / cell) * 64 + (l[1] / cell) * 8 + l[2] / cell) << 58));
  }
  std::vector<std::pair<i32, i32>> links;
  for (const SecAcc& A : fine)
    if (A.a >= 0 && A.a < m && A.b >= 0 && A.b < m) links.push_back({A.a, A.b});
  std::vector<i32> fnode;
  const i32 K = cluster_items(key, links, &fnode);
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
      const FragInfo& fi = frag_chunk_if(fk.chunk)->frags[size_t(fk.idx)];
      mass += fi.mass;
      com += fi.com * fi.mass;
      if (fi.mass > heavy) {
        heavy = fi.mass;
        mat = fi.mat;
      }
      strength = std::min(strength, class_mult(frag_class(fk)));
      s.frags.push_back(fk);
      auto& nm = s.nodemap[fk.chunk];
      const size_t nf = frag_chunk_if(fk.chunk)->frags.size();
      if (nm.size() < nf) nm.resize(nf, -1);
      nm[size_t(fk.idx)] = nd;
      auto& ov = owner_[fk.chunk];
      if (ov.size() < nf) ov.resize(nf, 0);
      if (ov[size_t(fk.idx)] && ov[size_t(fk.idx)] != s.id && superseded) superseded->push_back(ov[size_t(fk.idx)]);
      ov[size_t(fk.idx)] = s.id;
    }
    s.fstart.push_back(static_cast<i32>(s.frags.size()));
    node.c = mass > 0 ? com * (1.0 / mass) : com;
    node.mass = mass;
    s.P.nodes.push_back(node);
    const FragKey& f0 = frags[size_t(members[size_t(c)].front())];
    const FragInfo& fi0 = frag_chunk_if(f0.chunk)->frags[size_t(f0.idx)];
    s.ident.push_back(frag_ident(f0.chunk, fi0.first));
    const IVec3 cc = unkey3(f0.chunk);
    const IVec3 l = local_of(fi0.first);
    s.vox0.push_back({cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]});
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
  const std::vector<SecAcc> merged = merge_accs(fine, [&](i32 e) { return e >= kExisting ? e - kExisting : n0 + fnode[size_t(e)]; });
  for (const SecAcc& A0 : merged) {
    SecAcc A = A0;
    // (the bond's ends in the order of their identities: the same bond gets the same identity
    // and local frame whatever order an extraction found its nodes in - its reference load for
    // sudden changes is found again, and in the same frame)
    if (A.b >= 0 && s.ident[size_t(A.a)] > s.ident[size_t(A.b)]) {
      std::swap(A.a, A.b);
      for (i8& g : A.fsg) g = static_cast<i8>(-g);
    }
    const i32 ia = A.a, ib = A.b;
    if (ib >= 0) {
      A.mb = s.nmat[size_t(ib)];
      A.strength_b = s.nstrength[size_t(ib)];
    } else {
      A.strength_b = 1e9;  // (the anchored side never governs)
    }
    const V3* cb = ib >= 0 ? &s.P.nodes[size_t(ib)].c : nullptr;
    SBond B = A.finish(h, s.P.nodes[size_t(ia)].c, cb, s.nmat[size_t(ia)], s.nstrength[size_t(ia)]);
    section_strengths(A.faces.data(), A.fax.data(), A.faces.size(), [&](const IVec3& p) { return voxel_at(p); }, B);
    B.tag = static_cast<i32>(s.P.bonds.size());
    s.P.bonds.push_back(B);
    for (size_t k = 0; k < A.faces.size(); ++k) {
      s.face_p.push_back(A.faces[k]);
      s.face_axis.push_back(A.fax[k]);
    }
    s.face_start.push_back(static_cast<i32>(s.face_p.size()));
    s.bid.push_back(bond_ident(s.ident[size_t(ia)], ib >= 0 ? s.ident[size_t(ib)] : 0, ib >= 0 ? 0 : A.axis, ib >= 0 ? 1 : A.sign));
    s.phi.push_back(0.0f);
  }
}

void World::drop_structure(i64 id) {
  Structure* s = structure(id);
  if (!s) return;
  for (const FragKey& f : s->frags) {
    if (f.idx < 0) continue;
    auto it = owner_.find(f.chunk);
    if (it == owner_.end() || f.idx >= static_cast<i32>(it->second.size())) continue;
    if (it->second[size_t(f.idx)] == id) it->second[size_t(f.idx)] = 0;
  }
  s->dead = true;
  structures_.erase(std::remove_if(structures_.begin(), structures_.end(), [](const std::unique_ptr<Structure>& x) { return x->dead; }),
                    structures_.end());
}

void World::refresh_structures() {
  struct FreshScope {  // (outside a refresh, no structure counts as fresh)
    i64& f;
    ~FreshScope() { f = INT64_MAX; }
  } fresh_scope{fresh_from_};
  fresh_from_ = next_id_;  // (structures made from here on are this refresh's: extract())
  static const bool prof = diag("SVX_PROFILE");
  for (int round = 0; round < 3; ++round) {
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
            if (f.idx >= 0) undesigned_.erase(f.chunk);
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
    std::vector<IVec3> seeds;
    seeds.swap(seeds_);
    for (const IVec3& p : seeds) {
      FragKey f;
      if (!frag_at(p, &f)) continue;
      // owned: its structure is current (stale ones were patched above) or was extracted just now
      if (owner_of(f) != 0) continue;
      const auto tx = Clock::now();
      const i64 prev_owner = owner_of(f);
      Structure* xs = extract(f);
      if (prof && xs)
        std::printf("  [prof] extract s%lld from (%d %d %d) (owner was %lld, round %d): %zu nodes %zu bonds %.1f ms\n",
                    static_cast<long long>(xs->id), p[0], p[1], p[2], static_cast<long long>(prev_owner), round, xs->P.nodes.size(),
                    xs->P.bonds.size(), ms_since(tx));
    }
    bool any_stale = false;
    for (auto& s : structures_) any_stale = any_stale || s->stale;
    if (!any_stale && seeds_.empty()) break;
  }
}

// ---------------------------------------------------------------------------------------------
// Structures: loads, solves, judging

std::vector<f64> World::load_vector(const Structure& s) const {
  const size_t n = s.P.nodes.size();
  std::vector<f64> f(6 * n, 0.0);
  for (size_t i = 0; i < n; ++i) {
    f[6 * i + 2] = -s.weight[i];
    for (int q = 0; q < 6; ++q) f[6 * i + q] += s.ext_solved[6 * i + q];
  }
  return f;
}

void World::step_structures() {
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
      if (!s.solving && !s.stale && s.u.size() == 6 * s.P.nodes.size()) {
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
      StressOptions so;
      so.rtol = cfg_.stress_rtol;
      if (!s.P.assemble(so)) {
        s.solving = false;
        continue;
      }
      budget -= 30 * s.P.matrix_blocks();  // (assembly and hierarchy: counted as work)
      if (prof)
        std::printf("  [prof] assemble s%lld: %zu nodes %lld blocks: %.1f ms\n", static_cast<long long>(s.id), s.P.nodes.size(),
                    static_cast<long long>(s.P.matrix_blocks()), ms_since(tp));
    }
    if (!s.P.running()) {
      s.P.begin(load_vector(s), s.u);
      budget -= 2 * s.P.matrix_blocks();
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
      // stale operator) re-assemble from scratch and restart
      s.P.invalidate();
      s.run_iters = 0;
      detach_unsupported(s);
      continue;
    }
    if (!r.converged && s.run_iters + r.iters > 120) {
      // a stale preconditioner (after many breaks): rebuild it, and restart from here
      s.P.current(s.u);
      s.P.invalidate();
      s.run_iters = 0;
      continue;
    }
    if (r.converged) {
      s.P.current(s.u);
      s.P.stop();
      ++st_.solves;
      // a stale preconditioner (many changes since it was built): rebuild it for the next solve
      if (s.P.running() == false && s.run_iters + r.iters > 60) s.P.invalidate();
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

u64 World::node_chunk(const Structure& s, i32 node) const {
  const IVec3 cc = chunk_of(s.vox0[size_t(node)]);
  return key3(cc[0], cc[1], cc[2]);
}

void World::prune_caches() {
  // The reference loads of judged bonds and the warm starts of fragments outlive their
  // structures (a structure extracted again starts from them). Those of chunks no longer
  // resident go; beyond the budget, only the registered structures' are kept.
  if (source_) {
    for (auto it = judged_.begin(); it != judged_.end();)
      it = generated_.count(it->second.chunk) ? std::next(it) : judged_.erase(it);
    for (auto it = warm_u_.begin(); it != warm_u_.end();)
      it = generated_.count(it->second.chunk) ? std::next(it) : warm_u_.erase(it);
  }
  const i64 budget = static_cast<i64>(cfg_.memory.cache_mb * 1048576.0);
  if (hash_bytes(judged_) + hash_bytes(warm_u_) <= budget) return;
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
  if (hash_bytes(judged_) + hash_bytes(warm_u_) > budget) std::unordered_map<u64, WarmStart>().swap(warm_u_);  // (only a speed-up)
}

void World::judge(Structure& s) {
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
    if (dbgj && s.rounds == 0 && bond_utilization(B, L, par_.fragility) < 1.0 && it != judged_.end() && s.shock && dif != 1.0 &&
        bond_utilization(B, lerp_load(it->second.load, L, dif), par_.fragility) >= 1.0)
      std::printf("      (dif) bond %d: static phi %.2f, old N %.0f M %.0f %.0f V %.0f %.0f, new N %.0f M %.0f %.0f V %.0f %.0f\n", b,
                  bond_utilization(B, L, par_.fragility), it->second.load.N, it->second.load.M1, it->second.load.M2, it->second.load.V1,
                  it->second.load.V2, L.N, L.M1, L.M2, L.V1, L.V2);
    if (dbgj && s.rounds == 0 && it == judged_.end() && bond_utilization(B, L, par_.fragility) >= 1.0)
      std::printf("      (no baseline) bond %d: static phi %.2f\n", b, bond_utilization(B, L, par_.fragility));
    if (s.shock && dif != 1.0 && it != judged_.end()) Le = lerp_load(it->second.load, L, dif);
    const f64 phi = bond_utilization(B, Le, par_.fragility);
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
      if (f.idx >= 0) grid_.mark_dirty(unkey3(f.chunk));
  if (over.empty() || s.rounds >= cfg_.max_rounds) {
    s.solving = more;  // (a waiting load case, or the steady state after an impact: solve again)
    s.shock = false;
    if (over.empty()) s.rounds = 0;  // (the cascade is over: the next one gets its own rounds)
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
    for (size_t k = 0; k < std::min<size_t>(4, over.size()); ++k) {
      const SBond& B = s.P.bonds[size_t(over[k].second)];
      const BondLoad L = s.P.bond_load(over[k].second, s.u);
      FailMode mode;
      bond_utilization(B, L, par_.fragility, &mode);
      std::printf("      phi %.2f at (%.2f %.2f %.2f) n (%.2f %.2f %.2f) faces %d area %.4f sup %d mode %d N %.0f V %.0f %.0f T %.0f M %.0f %.0f\n",
                  over[k].first, B.p.x, B.p.y, B.p.z, B.n.x, B.n.y, B.n.z, B.faces, B.area, B.b < 0 ? 1 : 0, static_cast<int>(mode), L.N,
                  L.V1, L.V2, L.T, L.M1, L.M2);
    }
  }
  // a round breaks the worst bonds: those near the maximum, and at least the worst quarter of all
  // that are over strength (a heavily overloaded structure fails in few rounds)
  const size_t quota = std::max<size_t>(1, (over.size() + 3) / 4);
  i32 count = 0;
  for (size_t k = 0; k < over.size(); ++k) {
    const f64 phi = over[k].first;
    const i32 b = over[k].second;
    if ((phi < thr && k >= quota) || count >= cfg_.max_breaks_per_round) break;
    s.P.remove_bond(b);
    const SBond& B = s.P.bonds[size_t(b)];
    for (i32 k = s.face_start[size_t(b)]; k < s.face_start[size_t(b) + 1]; ++k) grid_.break_bond(s.face_p[size_t(k)], s.face_axis[size_t(k)]);
    ++count;
    ++st_.bonds_broken;
    crack_event(B.p, B.n, phi);
  }
  s.shock = true;
  ++s.rounds;
  detach_unsupported(s);
}

void World::crack_event(const V3& p, const V3& n, f64 phi) {
  if (crack_budget_ <= 0) return;
  --crack_budget_;
  WorldEvent ev;
  ev.kind = WorldEvent::Kind::Crack;
  ev.pos = p;
  ev.normal = n;
  ev.strength = phi;
  events_.push_back(std::move(ev));
}

void World::dust_event(const V3& p, const V3& v, i32 voxels, bool crushed) {
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

void World::detach_unsupported(Structure& s) {
  const i32 n = static_cast<i32>(s.P.nodes.size());
  std::vector<i32> comp;
  std::vector<u8> supported(size_t(n), 0);
  for (const SBond& B : s.P.bonds)
    if (!B.broken && B.b < 0) supported[size_t(B.a)] = 1;
  const i32 ncomp = graph_components(n, s.P.bonds, supported, &comp);
  std::vector<std::vector<i32>> pieces(static_cast<size_t>(ncomp));
  for (i32 i = 0; i < n; ++i)
    if (comp[size_t(i)] > 0 && !s.P.nodes[size_t(i)].gone) pieces[size_t(comp[size_t(i)])].push_back(i);
  std::vector<i32> retire;
  const i64 sid = s.id;
  for (i32 c = 1; c < ncomp; ++c) {
    if (pieces[size_t(c)].empty()) continue;
    std::vector<FragKey> frags;
    for (i32 i : pieces[size_t(c)]) {
      for (i32 k = s.fstart[size_t(i)]; k < s.fstart[size_t(i) + 1]; ++k)
        if (s.frags[size_t(k)].idx >= 0) frags.push_back(s.frags[size_t(k)]);
      retire.push_back(i);
    }
    make_body_from_world(frags, V3{}, V3{});
  }
  if (retire.empty()) return;
  Structure* sp = structure(sid);
  if (!sp) return;
  retire_structure_nodes(*sp, retire);
}

void World::retire_structure_nodes(Structure& s, const std::vector<i32>& list) {
  s.P.retire_nodes(list);
  for (i32 i : list) {
    for (i32 k = s.fstart[size_t(i)]; k < s.fstart[size_t(i) + 1]; ++k) {
      const FragKey f = s.frags[size_t(k)];
      if (f.idx < 0) continue;
      auto it = s.nodemap.find(f.chunk);
      if (it != s.nodemap.end() && f.idx < static_cast<i32>(it->second.size()) && it->second[size_t(f.idx)] == i)
        it->second[size_t(f.idx)] = -1;
      if (owner_of(f) == s.id) owner_[f.chunk][size_t(f.idx)] = 0;
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

void World::reseed(const Structure& s) {
  // A structure dropped to be extracted again seeds a voxel of each live node and of each of its
  // fragments (where the fragment cache is current): a fragment that cracks cut off inside a
  // cluster is then extracted on its own (and falls) instead of floating, unowned.
  for (size_t i = 0; i < s.vox0.size(); ++i)
    if (!s.P.nodes[i].gone) seeds_.push_back(s.vox0[i]);
  for (const FragKey& f : s.frags) {
    if (f.idx < 0) continue;
    FragChunk* fc = frag_chunk_if(f.chunk);
    const IVec3 cc = unkey3(f.chunk);
    const Chunk* ch = grid_.chunk(cc);
    if (!fc || !ch || fc->vox_version != ch->vox_version || f.idx >= static_cast<i32>(fc->frags.size())) continue;
    for (i32 k = fc->vox_start[size_t(f.idx)]; k < fc->vox_start[size_t(f.idx) + 1]; ++k) {
      const i32 li = fc->vox[size_t(k)];
      if (fc->id[size_t(li)] != static_cast<u16>(f.idx + 1)) continue;
      const IVec3 l = local_of(li);
      seeds_.push_back({cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]});
      break;
    }
  }
}

bool World::patch_structure(Structure& s) {
  std::vector<u64> changed = s.changed;
  std::sort(changed.begin(), changed.end());
  s.changed.clear();
  s.stale = false;
  auto is_changed = [&](u64 k) { return std::binary_search(changed.begin(), changed.end(), k); };
  // 1. nodes in re-fragmented chunks retire (clusters are chunk-local; the chunks' fragment
  // numbering and ownership were reset when they were rebuilt)
  for (u64 k : changed) s.nodemap.erase(k);
  std::vector<i32> retire;
  for (i32 i = 0; i < static_cast<i32>(s.P.nodes.size()); ++i) {
    if (s.P.nodes[size_t(i)].gone) continue;
    bool hit = false;
    for (i32 k = s.fstart[size_t(i)]; k < s.fstart[size_t(i) + 1] && !hit; ++k)
      hit = s.frags[size_t(k)].idx >= 0 && is_changed(s.frags[size_t(k)].chunk);
    if (hit) retire.push_back(i);
  }
  s.P.retire_nodes(retire);
  for (i32 i : retire) {
    for (i32 k = s.fstart[size_t(i)]; k < s.fstart[size_t(i) + 1]; ++k) {
      FragKey& f = s.frags[size_t(k)];
      if (f.idx < 0) continue;
      if (!is_changed(f.chunk)) {
        // (a cluster reaching out of the chunk: its other fragments are released as well)
        auto it = s.nodemap.find(f.chunk);
        if (it != s.nodemap.end() && f.idx < static_cast<i32>(it->second.size())) it->second[size_t(f.idx)] = -1;
      }
      // (a chunk re-fragmented reset its owners; one whose supports changed kept them)
      if (owner_of(f) == s.id) owner_[f.chunk][size_t(f.idx)] = 0;
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
  std::unordered_map<u64, std::vector<i32>> member_of;  // chunk -> member index per fragment (-1)
  for (u64 k : changed) {
    FragChunk& fc = frag_chunk(unkey3(k));
    member_of[k].assign(fc.frags.size(), -1);
  }
  // fragments released above (outside the changed chunks) are candidates too
  auto touches = [&](const FragKey& F) {
    std::vector<IVec3> vox;
    voxels_of(F, vox);
    for (const IVec3& p : vox)
      for (int a = 0; a < 3; ++a)
        for (int sg = -1; sg <= 1; sg += 2) {
          IVec3 q = p;
          q[a] += sg;
          if (!vox_free(grid_.get(q))) continue;
          if (sg > 0 ? grid_.broken(p, a) : grid_.broken(q, a)) continue;
          FragKey G;
          if (!frag_at(q, &G)) continue;
          if (s.node(G) >= 0 && !s.P.nodes[size_t(s.node(G))].gone) return true;
        }
    return false;
  };
  auto member_slot = [&](const FragKey& G) -> i32* {
    auto it = member_of.find(G.chunk);
    if (it == member_of.end()) return nullptr;
    if (G.idx >= static_cast<i32>(it->second.size())) it->second.resize(size_t(G.idx) + 1, -1);
    return &it->second[size_t(G.idx)];
  };
  std::vector<FragKey> stack;
  for (u64 k : changed) {
    FragChunk* fc = frag_chunk_if(k);
    if (!fc) continue;  // (an empty or evicted chunk)
    for (i32 f = 0; f < static_cast<i32>(fc->frags.size()); ++f) {
      if (fc->frags[size_t(f)].count <= 0 || owner_of({k, f})) continue;
      if (member_of[k][size_t(f)] >= 0 || !touches({k, f})) continue;
      member_of[k][size_t(f)] = static_cast<i32>(members.size());
      members.push_back({k, f});
      stack.push_back({k, f});
      while (!stack.empty()) {  // flood through the changed chunks
        const FragKey F = stack.back();
        stack.pop_back();
        std::vector<IVec3> vox;
        voxels_of(F, vox);
        for (const IVec3& p : vox)
          for (int a = 0; a < 3; ++a)
            for (int sg = -1; sg <= 1; sg += 2) {
              IVec3 q = p;
              q[a] += sg;
              const IVec3 qc = chunk_of(q);
              if (!is_changed(key3(qc[0], qc[1], qc[2])) || !vox_free(grid_.get(q))) continue;
              if (sg > 0 ? grid_.broken(p, a) : grid_.broken(q, a)) continue;
              FragKey G;
              if (!frag_at(q, &G) || (G.chunk == F.chunk && G.idx == F.idx) || owner_of(G)) continue;
              i32* slot = member_slot(G);
              if (!slot || *slot >= 0) continue;
              *slot = static_cast<i32>(members.size());
              members.push_back(G);
              stack.push_back(G);
            }
      }
    }
  }
  // 3. their bonds: to each other through + faces (counted once), to the structure's other
  // nodes through faces of either side, to supports
  std::vector<SecAcc> fine;
  std::unordered_map<u64, i32> index;
  auto acc_for = [&](i32 a, i32 b, int axis, int sign) -> SecAcc& {
    const u64 k = acc_key(a, b, axis, sign);
    auto it = index.find(k);
    if (it != index.end()) return fine[size_t(it->second)];
    index.emplace(k, static_cast<i32>(fine.size()));
    fine.emplace_back();
    SecAcc& A = fine.back();
    A.a = b >= 0 ? std::min(a, b) : a;
    A.b = b >= 0 ? std::max(a, b) : b;
    A.axis = static_cast<u8>(axis);
    A.sign = static_cast<i8>(sign);
    return A;
  };
  for (size_t mi = 0; mi < members.size(); ++mi) {
    const FragKey F = members[mi];
    const i32 eF = static_cast<i32>(mi);
    std::vector<IVec3> vox;
    voxels_of(F, vox);
    for (const IVec3& p : vox)
      for (int a = 0; a < 3; ++a)
        for (int sg = -1; sg <= 1; sg += 2) {
          IVec3 q = p;
          q[a] += sg;
          const Vox vq = grid_.get(q);
          const bool unloaded = !chunk_resident(chunk_of(q));  // (holds it, as in extract)
          if (!unloaded) {
            if (!vox_solid(vq)) continue;
            if (sg > 0 ? grid_.broken(p, a) : grid_.broken(q, a)) continue;
          }
          const IVec3 lower = sg > 0 ? p : q;
          if (vox_anchored(vq) || unloaded) {
            SecAcc& A = acc_for(eF, -1, a, sg);
            A.mb = unloaded ? vox_mat(grid_.get(p)) : vox_mat(vq);
            A.add(lower, a);
            continue;
          }
          FragKey G;
          if (!frag_at(q, &G) || (G.chunk == F.chunk && G.idx == F.idx)) continue;
          i32 eG = -1;
          if (i32* slot = member_slot(G); slot && *slot >= 0) {
            if (sg < 0) continue;  // (a member: counted from its + face)
            eG = *slot;
          } else {
            const i32 nd = s.node(G);
            if (nd >= 0 && !s.P.nodes[size_t(nd)].gone) eG = kExisting + nd;
          }
          if (eG < 0) {
            // a fragment of no structure here (another one's, a frontier): held fixed
            SecAcc& A = acc_for(eF, -1, a, sg);
            A.mb = vox_mat(vq);
            A.add(lower, a);
            continue;
          }
          const i32 low = sg > 0 ? eF : eG;
          acc_for(eF, eG, a, 1).add(lower, a, low == std::min(eF, eG) ? 1 : -1);
        }
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
void World::structure_loads(f64 dt_sub) {
  const auto& cs = rigid_.contacts();
  const f64 imp = par_.impact;
  for (const Contact& c : cs) {
    if (c.b >= 0) continue;
    const V3 F = c.impulse() * (-imp / dt_sub);  // on the world
    if (!finite3(F)) continue;  // (one such force would never decay out of the loads)
    FragKey f;
    if (!frag_at(c.wvox, &f)) continue;
    const i64 o = owner_of(f);
    Structure* s = o ? structure(o) : nullptr;
    if (!s) {
      if (norm(F) > 4.0 * cfg_.load_trigger_abs) seeds_.push_back(c.wvox);
      continue;
    }
    const i32 i = s->node(f);
    if (i < 0) continue;
    const V3 M = cross(c.p - s->P.nodes[size_t(i)].c, F);
    f64* a = &s->acc[6 * size_t(i)];
    a[0] += F.x;
    a[1] += F.y;
    a[2] += F.z;
    a[3] += M.x;
    a[4] += M.y;
    a[5] += M.z;
    const f64 mag = norm(F);
    if (mag > s->peak_mag[size_t(i)]) {
      s->peak_mag[size_t(i)] = mag;
      f64* pk = &s->peak[6 * size_t(i)];
      pk[0] = F.x;
      pk[1] = F.y;
      pk[2] = F.z;
      pk[3] = M.x;
      pk[4] = M.y;
      pk[5] = M.z;
    }
  }
  // dead loads: bodies falling asleep keep their last contact forces on the world
  for (size_t bi = 0; bi < rigid_.bodies.size(); ++bi) {
    Body& b = *rigid_.bodies[bi];
    if (b.asleep && !b.was_asleep) {
      auto& dl = dead_loads_[b.id];
      dl.clear();
      for (const Contact& c : cs)
        if (c.a == static_cast<i32>(bi) && c.b < 0 && finite3(c.impulse())) {
          dl.push_back({c.wvox, c.p, c.impulse() * (-1.0 / dt_sub)});
          seeds_.push_back(c.wvox);
        }
    } else if (!b.asleep && b.was_asleep) {
      dead_loads_.erase(b.id);
    }
    b.was_asleep = b.asleep;
  }
}

void World::finish_loads(int substeps) {
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
      if (!frag_at(d.vox, &f)) continue;
      Structure* s = structure(owner_of(f));
      if (!s) continue;
      const i32 i = s->node(f);
      if (i < 0) continue;
      const V3 M = cross(d.p - s->P.nodes[size_t(i)].c, d.F);
      f64* a = &s->acc[6 * size_t(i)];
      a[0] += d.F.x;
      a[1] += d.F.y;
      a[2] += d.F.z;
      a[3] += M.x;
      a[4] += M.y;
      a[5] += M.z;
    }
  for (auto& sp : structures_) {
    Structure& s = *sp;
    if (s.stale) continue;
    const size_t n = s.P.nodes.size();
    bool trigger = false, impact = false;
    for (size_t i = 0; i < n; ++i) {
      f64* e = &s.ext[6 * i];
      const f64* a = &s.acc[6 * i];
      for (int q = 0; q < 6; ++q) e[q] += ema * (a[q] - e[q]);
      const f64 thr = cfg_.load_trigger * s.weight[i] + cfg_.load_trigger_abs;
      if (a[0] == 0.0 && a[1] == 0.0 && a[2] == 0.0 && std::abs(e[0]) + std::abs(e[1]) + std::abs(e[2]) < 0.01 * thr)
        for (int q = 0; q < 6; ++q) e[q] = 0.0;  // (unloaded)
      const f64* es = &s.ext_solved[6 * i];
      const f64 d = std::sqrt((e[0] - es[0]) * (e[0] - es[0]) + (e[1] - es[1]) * (e[1] - es[1]) + (e[2] - es[2]) * (e[2] - es[2]));
      if (d > thr) trigger = true;
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

f64 World::probe_utilization(const IVec3& voxel, i32* over) {
  FragKey f;
  if (!frag_at(voxel, &f)) return -1.0;
  Structure* s = structure(owner_of(f));
  if (!s) s = extract(f);
  if (!s) return -1.0;
  const std::vector<f64> F = load_vector(*s);
  if (!s->P.assembled()) {
    StressOptions so;
    so.rtol = cfg_.stress_rtol;
    s->P.assemble(so);
  }
  s->P.solve(F, s->u, 1e-8, 5000, true);
  f64 mx = 0.0;
  i32 cnt = 0;
  i32 worst = -1;
  for (i32 b = 0; b < static_cast<i32>(s->P.bonds.size()); ++b) {
    if (s->P.bonds[size_t(b)].broken) continue;
    const f64 phi = bond_utilization(s->P.bonds[size_t(b)], s->P.bond_load(b, s->u), par_.fragility);
    if (phi > mx) worst = b;
    mx = std::max(mx, phi);
    cnt += phi >= 1.0 ? 1 : 0;
  }
  if (worst >= 0 && diag("SVX_DEBUG_PROBE")) {
    const SBond& B = s->P.bonds[size_t(worst)];
    FailMode mode;
    bond_utilization(B, s->P.bond_load(worst, s->u), par_.fragility, &mode);
    std::printf("  [probe] worst bond at (%.2f %.2f %.2f) n (%.1f %.1f %.1f) mode %d faces %d support %d sectioned %d ft %.2g fb %.2g\n", B.p.x,
                B.p.y, B.p.z, B.n.x, B.n.y, B.n.z, static_cast<int>(mode), B.faces, B.b < 0 ? 1 : 0, B.sectioned ? 1 : 0, B.ft, B.fb);
  }
  if (over) *over = cnt;
  return mx;
}

// ---------------------------------------------------------------------------------------------
// Events

bool World::in_range(const V3& p) const {
  // finite, and within the range of voxel keys (+-2^20 voxels) with room for an event's radius
  const f64 lim = grid_.h * static_cast<f64>(kVoxelLimit);
  return finite3(p) && std::abs(p.x) < lim && std::abs(p.y) < lim && std::abs(p.z) < lim;
}

void World::carve(const V3& pos, f64 radius) {
  if (!in_range(pos) || !std::isfinite(radius) || radius <= 0.0) return;
  PendingEvent e;
  e.pos = pos;
  e.radius = std::min(radius, cfg_.max_event_radius);
  queue_.push_back(e);
}

void World::blast(const V3& pos, f64 radius, f64 energy) {
  if (!in_range(pos) || !std::isfinite(radius) || radius <= 0.0 || !std::isfinite(energy)) return;
  PendingEvent e;
  e.blast = true;
  e.pos = pos;
  e.radius = std::min(radius, cfg_.max_event_radius);
  e.energy = std::clamp(energy, 0.0, 1e15);
  queue_.push_back(e);
}

i32 World::set_voxels(const std::vector<VoxelEdit>& in, u32 flags) {
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
  if (source_) ensure_chunks({lo[0] - 1, lo[1] - 1, lo[2] - 1}, {hi[0] + 2, hi[1] + 2, hi[2] + 2});
  const bool tracked = grid_.tracking();
  if (flags & kEditUntracked) grid_.track_changes(false);
  std::vector<IVec3> changed, unbonded;
  std::vector<u64> supports;
  for (const VoxelEdit& e : edits) {
    const Vox cur = grid_.get(e.p);
    if (cur == e.v && !(flags & kEditIsolated)) continue;
    if (cur != e.v) {
      if (vox_anchored(cur)) support_changed(e.p, &supports);  // (a support of a structure goes)
      grid_.set(e.p, e.v);
      changed.push_back(e.p);
      // what the old voxel held may lose its support (an isolated voxel held nothing)
      if (vox_solid(cur) && (!(flags & kEditIsolated) || vox_free(cur))) unbonded.push_back(e.p);
      if (vox_free(e.v)) seeds_.push_back(e.p);  // a new free voxel joins or makes a structure
    }
    if ((flags & kEditIsolated) && vox_solid(e.v))
      for (int a = 0; a < 3; ++a) {
        grid_.break_bond(e.p, a);
        IVec3 q = e.p;
        q[a] -= 1;
        grid_.break_bond(q, a);
      }
    else if (cur != e.v && vox_anchored(e.v))
      support_changed(e.p, &supports);  // (a new support)
  }
  grid_.track_changes(tracked);
  for (u64 k : supports) mark_owners_stale(k);
  if (changed.empty()) return 0;
  seed_near(unbonded);
  const f64 h = grid_.h;
  const V3 m{2 * h, 2 * h, 2 * h};
  rigid_.wake_box(V3{h * lo[0], h * lo[1], h * lo[2]} - m, V3{h * hi[0], h * hi[1], h * hi[2]} + m);
  return static_cast<i32>(changed.size());
}

bool World::apply_impulse(i64 id, const V3& point, const V3& J) {
  Body* b = rigid_.find(id);
  if (!b || !in_range(point) || !finite3(J)) return false;
  rigid_.wake(*b);
  b->v += J * b->inv_mass;
  b->w += b->inv_inertia_world() * cross(point - b->x, J);
  return true;
}

bool World::remove_piece(i64 id) {
  if (!rigid_.find(id)) return false;
  remove_bodies({id}, PieceEnd::Removed);
  return true;
}

void World::support_changed(const IVec3& p, std::vector<u64>* chunks) {
  // the structures holding a free neighbour bonded to p: their supports change (the fragments
  // of free voxels stay as they are, so nothing else would tell them)
  for (int a = 0; a < 3; ++a)
    for (int sg = -1; sg <= 1; sg += 2) {
      IVec3 q = p;
      q[a] += sg;
      if (!vox_free(grid_.get(q))) continue;
      if (!(sg > 0 ? grid_.bond(p, a) : grid_.bond(q, a))) continue;
      const IVec3 cc = chunk_of(q);
      const u64 k = key3(cc[0], cc[1], cc[2]);
      if (std::find(chunks->begin(), chunks->end(), k) == chunks->end()) chunks->push_back(k);
    }
}

void World::carve_world(const V3& c, f64 r, std::vector<IVec3>* removed) {
  const f64 h = grid_.h;
  const i32 R = static_cast<i32>(std::ceil(r / h)) + 1;
  const IVec3 cv = voxel_of(c, h);
  std::vector<u64> supports;
  for (i32 x = cv[0] - R; x <= cv[0] + R; ++x)
    for (i32 y = cv[1] - R; y <= cv[1] + R; ++y)
      for (i32 z = cv[2] - R; z <= cv[2] + R; ++z) {
        const V3 p{h * x, h * y, h * z};
        // a slightly jagged crater edge (deterministic per voxel)
        const f64 jag = 1.0 + 0.18 * (unit01(mix64(key3(x, y, z))) - 0.5);
        if (norm(p - c) > r * jag) continue;
        const IVec3 q{x, y, z};
        const Vox v = grid_.get(q);
        if (!vox_solid(v)) continue;
        const Material& M = material(vox_mat(v));
        if (M.indestructible || M.ductile) continue;  // (bullets and craters bend steel and bars; they do not remove it)
        if (vox_anchored(v)) support_changed(q, &supports);
        grid_.set(q, kAir);
        removed->push_back(q);
      }
  for (u64 k : supports) mark_owners_stale(k);
}

void World::seed_near(const std::vector<IVec3>& removed) {
  for (const IVec3& p : removed)
    for (int a = 0; a < 3; ++a)
      for (int s = -1; s <= 1; s += 2) {
        IVec3 q = p;
        q[a] += s;
        if (vox_free(grid_.get(q))) seeds_.push_back(q);
      }
}

void World::process(const PendingEvent& e) {
  ++st_.events;
  design_near(e.pos, (e.blast ? cfg_.blast_reach : 1.0) * e.radius + 1.0);
  std::vector<IVec3> removed;
  carve_world(e.pos, e.radius, &removed);
  carve_bodies(e.pos, e.radius);
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

void World::blast_world(const PendingEvent& e) {
  const f64 h = grid_.h;
  const f64 rs = cfg_.blast_shatter * e.radius, rl = cfg_.blast_reach * e.radius;
  // fragments within the load radius
  const i32 R = static_cast<i32>(std::ceil(rl / h)) + 1;
  const IVec3 cv = voxel_of(e.pos, h);
  std::vector<std::pair<FragKey, f64>> near;  // fragment, distance of its centre
  for (i32 cx = (cv[0] - R) >> kChunkBits; cx <= (cv[0] + R) >> kChunkBits; ++cx)
    for (i32 cy = (cv[1] - R) >> kChunkBits; cy <= (cv[1] + R) >> kChunkBits; ++cy)
      for (i32 cz = (cv[2] - R) >> kChunkBits; cz <= (cv[2] + R) >> kChunkBits; ++cz) {
        const IVec3 cc{cx, cy, cz};
        if (!grid_.chunk(cc)) continue;
        FragChunk& fc = frag_chunk(cc);
        const u64 key = key3(cx, cy, cz);
        for (i32 f = 0; f < static_cast<i32>(fc.frags.size()); ++f) {
          const FragInfo& fi = fc.frags[size_t(f)];
          if (fi.count <= 0) continue;
          const f64 d = norm(fi.com - e.pos);
          if (d <= rl) near.push_back({FragKey{key, f}, d});
        }
      }
  if (near.empty()) return;
  // shatter: loose pieces near the crater (each its own body) thrown outwards
  auto falloff = [&](f64 d) { return std::min(1.0, (e.radius * e.radius) / std::max(1e-6, d * d)); };
  f64 m_shatter = 0.0, m_loaded = 0.0;
  for (const auto& [f, d] : near) {
    const f64 m = frag_chunk_if(f.chunk)->frags[size_t(f.idx)].mass;
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
  std::vector<std::pair<FragKey, f64>> shatter;
  for (const auto& fd : near)
    if (fd.second <= rs) shatter.push_back(fd);
  for (const auto& [f, d0] : shatter) {
    FragChunk* fc = frag_chunk_if(f.chunk);
    if (!fc || fc->frags[size_t(f.idx)].count <= 0) continue;
    const FragInfo fi = fc->frags[size_t(f.idx)];
    const f64 d = std::max(1e-3, norm(fi.com - e.pos));
    const V3 dir = (fi.com - e.pos) * (1.0 / d);
    const u64 hs = mix64(frag_ident(f.chunk, fi.first) ^ static_cast<u64>(st_.ticks));
    const V3 spin{(unit01(hs) - 0.5) * 12.0, (unit01(mix64(hs ^ 1)) - 0.5) * 12.0, (unit01(mix64(hs ^ 2)) - 0.5) * 12.0};
    // its faces are torn (the piece keeps none of the world's bonds); the structure holding it
    // is patched (the fragment cache is updated in place: nothing else would tell it)
    if (owner_of(f)) mark_owners_stale(f.chunk);
    std::vector<IVec3> vox;
    voxels_of(f, vox);
    for (const IVec3& p : vox)
      for (int a = 0; a < 3; ++a) {
        grid_.break_bond(p, a);
        IVec3 q = p;
        q[a] -= 1;
        grid_.break_bond(q, a);
      }
    make_body_from_world({f}, dir * speed(d), spin);
    seed_near(vox);
  }
  // load: the blast's impulse on the structures around it (an impact load case)
  for (const auto& [f, d] : near) {
    if (d <= rs) continue;
    FragChunk* fc = frag_chunk_if(f.chunk);
    if (!fc || fc->frags[size_t(f.idx)].count <= 0) continue;
    const FragInfo& fi = fc->frags[size_t(f.idx)];
    const V3 dir = (fi.com - e.pos) * (1.0 / std::max(1e-3, d));
    blast_loads_.push_back({fi.first, f.chunk, dir * (par_.impact * fi.mass * speed(d) / cfg_.dt), fi.com});
    const IVec3 cc = unkey3(f.chunk);
    const IVec3 l = local_of(fi.first);
    seeds_.push_back({cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]});
  }
}

void World::apply_blast_loads() {
  if (blast_loads_.empty()) return;
  for (const BlastLoad& bl : blast_loads_) {
    const IVec3 cc = unkey3(bl.chunk);
    const IVec3 l = local_of(bl.first);
    FragKey f;
    if (!frag_at({cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]}, &f)) continue;
    Structure* s = structure(owner_of(f));
    if (!s) continue;
    const i32 i = s->node(f);
    if (i < 0) continue;
    f64* pk = &s->peak[6 * size_t(i)];
    pk[0] += bl.F.x;
    pk[1] += bl.F.y;
    pk[2] += bl.F.z;
    if (s->fstart[size_t(i) + 1] - s->fstart[size_t(i)] > 1) {
      // (a cluster of fragments: the force acts at its fragment, off the node's centre)
      const V3 M = cross(bl.at - s->P.nodes[size_t(i)].c, bl.F);
      pk[3] += M.x;
      pk[4] += M.y;
      pk[5] += M.z;
    }
    s->peak_mag[size_t(i)] = std::sqrt(pk[0] * pk[0] + pk[1] * pk[1] + pk[2] * pk[2]);
    s->shock = true;
  }
  blast_loads_.clear();
}

// ---------------------------------------------------------------------------------------------
// Memory budgets

void World::enforce_budgets() {
  trim_output();  // (every tick: cheap unless over)
  if (st_.ticks % 30 != 0) return;
  trim_fragment_caches();
  trim_structures();
  prune_caches();
}

void World::trim_fragment_caches() {
  // Fragment caches are derived from the grid: those of chunks no structure holds are dropped,
  // least recently used first, beyond the budget (rebuilt, identical, when needed again).
  const i64 budget = static_cast<i64>(cfg_.memory.fragment_cache_mb * 1048576.0);
  i64 bytes = 0;
  for (const auto& [k, fc] : frags_) bytes += fc.memory_bytes();
  if (bytes <= budget) return;
  std::vector<std::pair<i64, u64>> cand;  // (last use, chunk)
  for (const auto& [k, fc] : frags_) {
    const auto ot = owner_.find(k);
    bool held = false;
    if (ot != owner_.end())
      for (i64 id : ot->second) held = held || id != 0;
    if (!held) cand.push_back({fc.used, k});
  }
  std::sort(cand.begin(), cand.end());
  for (const auto& [used, k] : cand) {
    if (bytes <= budget) break;
    const auto it = frags_.find(k);
    bytes -= it->second.memory_bytes();
    frags_.erase(it);
    owner_.erase(k);
    ++st_.dropped_fragment_caches;
  }
}

i64 World::structure_bytes(const Structure& s) const {
  i64 b = sizeof(Structure) + s.P.memory_bytes() + vec_bytes(s.fstart) + vec_bytes(s.frags) + vec_bytes(s.ident) + vec_bytes(s.nmat) +
          vec_bytes(s.nstrength) + vec_bytes(s.vox0) + vec_bytes(s.weight) + vec_bytes(s.face_start) + vec_bytes(s.face_p) +
          vec_bytes(s.face_axis) + vec_bytes(s.bid) + vec_bytes(s.phi) + vec_bytes(s.u) + vec_bytes(s.ext) + vec_bytes(s.ext_solved) +
          vec_bytes(s.acc) + vec_bytes(s.peak) + vec_bytes(s.pending) + vec_bytes(s.peak_mag) + vec_bytes(s.changed) +
          hash_bytes(s.nodemap);
  for (const auto& [k, v] : s.nodemap) b += vec_bytes(v);
  return b;
}

void World::trim_structures() {
  // Registered structures are extracted again when something happens to them: beyond the
  // budget the idle ones go, the longest idle first (never one being solved).
  const i64 budget = static_cast<i64>(cfg_.memory.structure_mb * 1048576.0);
  i64 bytes = 0;
  std::vector<std::tuple<i32, i64, i64>> cand;  // (-idle, id, bytes)
  for (const auto& sp : structures_) {
    const i64 b = structure_bytes(*sp);
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

void World::trim_output() {
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
  if (evicted_chunks_.size() > cap) evicted_chunks_.erase(evicted_chunks_.begin(), evicted_chunks_.begin() + static_cast<long>(evicted_chunks_.size() - cap / 2));
}

// ---------------------------------------------------------------------------------------------
// Tick

void World::tick() {
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
  // (the violent part of a collapse, or a large pile settling: one substep a tick)
  const bool busy = rigid_.busy() || rigid_.contacts().size() > cfg_.rigid.busy_contacts;
  const int ns = busy ? 1 : std::max(1, cfg_.rigid.substeps);
  const f64 dts = cfg_.dt / ns;
  for (int k = 0; k < ns; ++k) {
    rigid_.substep(dts, grid_, [this](f64 dt) { return fracture_hook(dt); });
    structure_loads(dts);
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
  const f64 floor_z = grid_.h * grid_.lo[2] - cfg_.rigid.kill_depth;
  std::vector<i64> out;
  for (const auto& bp : rigid_.bodies) {
    const Body& b = *bp;
    const bool finite = finite3(b.x) && finite3(b.v) && finite3(b.w) && std::isfinite(b.q.w);
    if (!finite || b.x.z < floor_z || b.shape.count <= 0) out.push_back(b.id);
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
  grid_.compact_changed();
  enforce_budgets();
  announce_bodies();
  st_.tick_ms = ms_since(t0);
  st_.upkeep_ms = std::max(0.0, st_.tick_ms - st_.stream_ms - st_.event_ms - st_.rigid_ms - loads_ms - st_.structural_ms - st_.systems_ms);
}

}  // namespace svx
