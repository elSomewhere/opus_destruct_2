// structvox — the world's design pass (bake), persistence, streaming, output, queries, stats and
// hashes.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_set>

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
  std::vector<u64> keys;
  for (const auto& [k, ch] : grid_.chunks())
    if (ch.free_count() > 0) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  // fragments already designed, by identity (indices shift when floating pieces are removed)
  std::unordered_set<u64> seen;
  auto ident_of = [&](const FragKey& f) -> u64 {
    FragChunk* fc = frag_chunk_if(f.chunk);
    return fc ? frag_ident(f.chunk, fc->frags[size_t(f.idx)].first) : 0;
  };
  auto mark = [&](const FragKey& f) { seen.insert(ident_of(f)); };
  auto is_seen = [&](const FragKey& f) { return seen.count(ident_of(f)) > 0; };
  for (u64 key : keys) {
    const IVec3 cc = unkey3(key);
    for (i32 fi = 0;; ++fi) {
      FragChunk& fc = frag_chunk(cc);
      if (fi >= static_cast<i32>(fc.frags.size())) break;
      if (fc.frags[size_t(fi)].count <= 0) continue;
      const FragKey f{key, fi};
      if (is_seen(f)) continue;
      const Chunk* ch0 = grid_.chunk(cc);
      const u32 ver = ch0 ? ch0->vox_version : 0;
      Structure* s = extract(f, 4000000, 1e9, false);
      static const bool bdbg = std::getenv("SVX_DEBUG_BAKE") != nullptr;
      if (bdbg && s && s->P.nodes.size() > 1000)
        std::printf("  [bake] chunk (%d %d %d) fragment %d: structure of %zu nodes\n", cc[0], cc[1], cc[2], fi, s->P.nodes.size());
      if (!s) {
        // (a floating piece was removed: the chunk's fragments are renumbered, scan it again; the
        // ones seen are skipped by identity)
        const Chunk* ch1 = grid_.chunk(cc);
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
      const std::vector<f64> F = load_vector(*s);
      static const bool dbg = std::getenv("SVX_DEBUG") != nullptr;
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
        judged_[s->bid[size_t(b)]] = L;
        const f64 phi = bond_utilization(B, L, 1.0);
        design_.max_utilization = std::max(design_.max_utilization, phi);
        if (phi <= target) continue;
        const u8 cls = class_for(B.strength * phi / target);
        for (int side = 0; side < 2; ++side) {
          const i32 nd = side == 0 ? B.a : B.b;
          if (nd < 0) continue;
          std::vector<IVec3> vox;
          for (i32 k = s->fstart[size_t(nd)]; k < s->fstart[size_t(nd) + 1]; ++k)
            if (s->frags[size_t(k)].idx >= 0) voxels_of(s->frags[size_t(k)], vox);
          for (const IVec3& p : vox)
            if (grid_.strength(p) < cls) {
              grid_.set_strength(p, cls);
              ++design_.strengthened_voxels;
            }
        }
      }
      for (size_t i = 0; i < s->P.nodes.size(); ++i) {
        std::array<f32, 6> w;
        for (int q = 0; q < 6; ++q) w[size_t(q)] = static_cast<f32>(s->u[6 * i + q]);
        warm_u_[s->ident[i]] = w;
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
  return true;
}

// ---------------------------------------------------------------------------------------------
// Persistence and streaming

std::vector<u8> World::save_delta() const {
  if (!source_) return grid_.save_delta();
  std::vector<u64> keys = grid_.modified_chunks();
  for (const auto& [k, r] : archive_) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  std::vector<std::vector<u8>> recs;
  for (u64 k : keys) {
    const auto it = archive_.find(k);
    recs.push_back(it != archive_.end() && !grid_.is_modified(k) ? *it->second : grid_.chunk_record(k));
  }
  return VoxelGrid::pack_delta(recs);
}

bool World::load_delta(const std::vector<u8>& bytes) {
  std::vector<u64> touched;
  if (source_) {
    std::vector<std::pair<u64, std::vector<u8>>> recs;
    if (!VoxelGrid::unpack_delta(bytes, &recs)) return false;
    for (auto& [k, r] : recs) {
      if (generated_.count(k)) {
        u64 key = 0;
        if (!grid_.apply_record(r, &key)) return false;
        touched.push_back(key);
      } else {
        archive_[k] = std::make_shared<const std::vector<u8>>(std::move(r));
      }
    }
  } else if (!grid_.load_delta(bytes, &touched)) {
    return false;
  }
  // restored chunks: their structures are extracted again when something happens there
  for (u64 k : touched) {
    mark_owners_stale(k);
    const IVec3 cc = unkey3(k);
    const Chunk* ch = grid_.chunk(cc);
    if (ch && ch->free_count() > 0) {
      // a restored piece may stand on nothing now: check it
      const IVec3 b{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
      for (int i = 0; i < kChunkVox; i += 97) {
        const IVec3 l = local_of(i);
        const IVec3 p{b[0] + l[0], b[1] + l[1], b[2] + l[2]};
        if (vox_free(grid_.get(p))) seeds_.push_back(p);
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
  generated_.clear();
  column_count_.clear();
  archive_.clear();
  if (!source_) return;
  const IVec3 lo = source_->chunk_lo(), hi = source_->chunk_hi();
  grid_.lo = {lo[0] * kChunk, lo[1] * kChunk, lo[2] * kChunk};
  grid_.hi = {hi[0] * kChunk, hi[1] * kChunk, hi[2] * kChunk};
}

void World::set_focus(const std::vector<V3>& points) {
  focus_.clear();
  for (const V3& p : points)
    if (finite3(p)) focus_.push_back(p);
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
  const bool any = source_->generate(cc, v);
  insert_generated(key, any, std::move(v));
  return true;
}

void World::insert_generated(u64 key, bool any, std::vector<Vox>&& v) {
  const IVec3 cc = unkey3(key);
  generated_.insert(key);
  ++column_count_[key3(cc[0], cc[1], 0)];
  ++st_.generated_total;
  bool changed = false;
  if (any) {
    grid_.insert_chunk(cc, std::move(v));
    changed = true;
  }
  const auto ait = archive_.find(key);
  if (ait != archive_.end()) {
    grid_.track_changes(true);
    grid_.apply_record(*ait->second);
    archive_.erase(ait);
    changed = true;
  } else if (any) {
    // (fresh from the generator: designed when first touched; an archived chunk was designed
    // before it was changed)
    const Chunk* ch = grid_.chunk(cc);
    if (ch && ch->free_count() > 0) undesigned_.insert(key);
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
      mark_owners_stale(key3(q[0], q[1], q[2]), key);
    }
}

void World::evict_chunk(u64 k) {
  const IVec3 cc = unkey3(k);
  undesigned_.erase(k);
  if (grid_.is_modified(k)) archive_[k] = std::make_shared<const std::vector<u8>>(grid_.chunk_record(k));
  const bool resident = grid_.chunk(cc) != nullptr;
  mark_owners_stale(k);
  owner_.erase(k);
  frags_.erase(k);
  grid_.remove_chunk(cc);
  generated_.erase(k);
  if (--column_count_[key3(cc[0], cc[1], 0)] <= 0) column_count_.erase(key3(cc[0], cc[1], 0));
  ++st_.evicted_total;
  if (resident) evicted_chunks_.push_back(k);
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
    vox.assign(n, {});
    any.assign(n, 0);
    const ChunkSource* src = source_.get();
    parallel_for(static_cast<i64>(n), 1, [&](i64 b0, i64 e0) {
      for (i64 j = b0; j < e0; ++j) any[size_t(j)] = src->generate(unkey3(want[i + size_t(j)].second), vox[size_t(j)]) ? 1 : 0;
    });
    for (size_t j = 0; j < n && budget > 0 && empty_budget > 0; ++j) {
      const u64 k = want[i + j].second;
      insert_generated(k, any[j] != 0, std::move(vox[j]));
      ++generated;
      if (grid_.chunk(unkey3(k))) --budget;
      else --empty_budget;
    }
    i += n;
  }
  // evict behind the viewer (never under a moving piece or a structure being solved)
  std::unordered_set<u64> busy;
  for (const auto& bp : rigid_.bodies) {
    if (bp->asleep) continue;
    const IVec3 a = voxel_of(bp->box_lo, grid_.h), b = voxel_of(bp->box_hi, grid_.h);
    for (i32 x = (a[0] >> kChunkBits) - 1; x <= (b[0] >> kChunkBits) + 1; ++x)
      for (i32 y = (a[1] >> kChunkBits) - 1; y <= (b[1] >> kChunkBits) + 1; ++y)
        for (i32 z = (a[2] >> kChunkBits) - 1; z <= (b[2] >> kChunkBits) + 1; ++z) busy.insert(key3(x, y, z));
  }
  for (const auto& s : structures_)
    if (s->solving)
      for (const FragKey& f : s->frags)
        if (f.idx >= 0) busy.insert(f.chunk);
  std::vector<u64> keys(generated_.begin(), generated_.end());
  std::sort(keys.begin(), keys.end());
  for (u64 k : keys) {
    const IVec3 cc = unkey3(k);
    if (hdist(cc) <= stream_.evict_radius || busy.count(k)) continue;
    evict_chunk(k);
  }
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
      for (const auto& [nd, k] : far) {
        if (bytes <= budget_b) break;
        const Chunk* ch = grid_.chunk(unkey3(k));
        bytes -= ch ? static_cast<i64>(sizeof(Chunk) + ch->v.size() + ch->broken.size() + ch->strength.size()) : 0;
        evict_chunk(k);
        ++st_.budget_evicted;
      }
    }
  }
  st_.stream_ms = ms_since(t0);
  return generated;
}

bool World::modified() const { return !grid_.modified_chunks().empty() || !archive_.empty(); }

// ---------------------------------------------------------------------------------------------
// Output

std::vector<u64> World::take_changed_chunks() {
  std::vector<u64> keys = grid_.take_dirty();
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return keys;
}

std::vector<u64> World::take_evicted_chunks() {
  std::vector<u64> out;
  out.swap(evicted_chunks_);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool World::debug_field(const IVec3& cc, DebugField field, std::vector<u8>* out) {
  const u64 k = key3(cc[0], cc[1], cc[2]);
  if (!grid_.chunk(cc)) return false;
  FragChunk& fc = frag_chunk(cc);
  // per fragment of the chunk, then per voxel
  std::vector<u8> v(fc.frags.size(), 0);
  std::unordered_map<i64, std::vector<f32>> node_phi;
  for (i32 f = 0; f < static_cast<i32>(fc.frags.size()); ++f) {
    if (field == DebugField::Fragment) {
      v[size_t(f)] = static_cast<u8>(1 + mix64(frag_ident(k, fc.frags[size_t(f)].first)) % 254);
      continue;
    }
    Structure* s = structure(owner_of({k, f}));
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
    const i32 nd = s->node({k, f});
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
  s.memory_mb = f64(grid_.memory_bytes()) / (1024.0 * 1024.0);
  s.structures = static_cast<i32>(structures_.size());
  s.resident_chunks = static_cast<i64>(generated_.size());
  s.archived_chunks = static_cast<i64>(archive_.size());
  s.bodies = static_cast<i32>(rigid_.bodies.size());
  s.awake = rigid_.awake_count();
  s.contacts = static_cast<i32>(rigid_.contacts().size());
  return s;
}

u64 World::state_hash() const {
  std::vector<u64> keys;
  for (const auto& [k, c] : grid_.chunks()) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  u64 hsh = 1469598103934665603ull;
  auto mix = [&](u64 v) { hsh = (hsh ^ v) * 1099511628211ull; };
  for (u64 k : keys) {
    const Chunk& c = grid_.chunks().at(k);
    mix(k);
    if (c.uniform) {
      mix(c.value);
    } else {
      for (Vox v : c.v) mix(v);
    }
    for (u8 b : c.broken) mix(b);
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

RayHit World::raycast(const V3& o, const V3& dir, f64 max_dist) const {
  RayHit hit;
  const f64 h = grid_.h;
  V3 d = dir;
  if (!finite3(o) || !finite3(d) || !(max_dist > 0.0)) return hit;
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
  // rigid pieces: the ray in each piece's shape frame
  const f64 best = hit.hit ? hit.distance : max_dist;
  f64 bt = best;
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
    if (dda(os + ds * t0, ds, h, 2.0 * r, [&](const IVec3& p) { return vox_solid(b.shape.get(p)); }, &t, &face, step, &v)) {
      const f64 tt = t0 + t;
      if (tt < bt) {
        bt = tt;
        hit.hit = true;
        hit.distance = tt;
        hit.pos = o + d * tt;
        V3 n{0, 0, 0};
        if (face >= 0) n[face] = -step[face];
        hit.normal = rotate(b.q, n);
        hit.material = static_cast<int>(vox_mat(b.shape.get(v)));
        hit.voxel = v;
        hit.piece = b.id;
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
  if (!finite3(mn) || !finite3(mx) || !finite3(mv)) return res;
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
    lo[size_t(a)] += dm;
    hi[size_t(a)] += dm;
    res.move[a] = dm;
  }
  return res;
}

void World::debug_voxel(const IVec3& p) {
  const IVec3 cc = chunk_of(p);
  FragChunk& fc = frag_chunk(cc);
  const i32 g = fc.at(chunk_index(p));
  const Vox v = grid_.get(p);
  std::printf("voxel (%d %d %d): solid %d mat %d free %d, fragment %d", p[0], p[1], p[2], vox_solid(v) ? 1 : 0, int(vox_mat(v)),
              vox_free(v) ? 1 : 0, g);
  if (g >= 0) {
    const FragKey f{key3(cc[0], cc[1], cc[2]), g};
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
  if (undesigned_.empty()) return false;
  for (const FragKey& f : s.frags)
    if (f.idx >= 0 && undesigned_.count(f.chunk)) return true;
  return false;
}

void World::design_structure(Structure& s, bool dry) {
  // self-weight only (the design state), to a tight tolerance
  if (!s.P.assembled()) {
    StressOptions so;
    so.rtol = 1e-5;
    if (!s.P.assemble(so)) return;
  }
  const size_t n = s.P.nodes.size();
  std::vector<f64> F(6 * n, 0.0);
  for (size_t i = 0; i < n; ++i) F[6 * i + 2] = -s.weight[i];
  std::vector<f64> u(6 * n, 0.0);
  const PcgResult r = s.P.solve(F, u, 1e-5, 2000, false);
  const f64 target = cfg_.design_utilization;
  static const bool dbg = std::getenv("SVX_DEBUG_DESIGN") != nullptr;
  f64 maxphi = 0.0;
  i32 over = 0;
  for (i32 b = 0; b < static_cast<i32>(s.P.bonds.size()); ++b) {
    const SBond& B = s.P.bonds[size_t(b)];
    if (B.broken) continue;
    const BondLoad L = s.P.bond_load(b, u);
    if (!dry) judged_[s.bid[size_t(b)]] = L;  // (the reference state of sudden changes, as the bake's)
    const f64 phi = bond_utilization(B, L, 1.0);
    maxphi = std::max(maxphi, phi);
    if (phi <= target) continue;
    ++over;
    if (dry) continue;
    const u8 cls = class_for(B.strength * phi / target);
    for (int side = 0; side < 2; ++side) {
      const i32 nd = side == 0 ? B.a : B.b;
      if (nd < 0) continue;
      std::vector<IVec3> vox;
      for (i32 k = s.fstart[size_t(nd)]; k < s.fstart[size_t(nd) + 1]; ++k)
        if (s.frags[size_t(k)].idx >= 0) voxels_of(s.frags[size_t(k)], vox);
      for (const IVec3& p : vox)
        if (grid_.strength(p) < cls) {
          grid_.set_strength(p, cls);
          ++st_.strengthened_voxels;
        }
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
    warm_u_[s.ident[i]] = w;
  }
  for (const FragKey& f : s.frags)
    if (f.idx >= 0) undesigned_.erase(f.chunk);
}

void World::design_near(const V3& c, f64 r) {
  // structures around an event, designed before it happens (streamed worlds)
  if (undesigned_.empty()) return;
  const f64 h = grid_.h;
  const IVec3 lo = voxel_of(c - V3{r, r, r}, h), hi = voxel_of(c + V3{r, r, r}, h);
  const i32 step = std::max<i32>(1, static_cast<i32>(r / (4.0 * h)));
  for (i32 x = lo[0]; x <= hi[0]; x += step)
    for (i32 y = lo[1]; y <= hi[1]; y += step)
      for (i32 z = lo[2]; z <= hi[2]; z += step) {
        const IVec3 p{x, y, z};
        if (!undesigned_.count(key3(p[0] >> kChunkBits, p[1] >> kChunkBits, p[2] >> kChunkBits))) continue;
        if (!vox_free(grid_.get(p))) continue;
        FragKey f;
        if (!frag_at(p, &f) || owner_of(f) != 0) continue;
        extract(f);  // (designs it)
      }
}

}  // namespace svx
