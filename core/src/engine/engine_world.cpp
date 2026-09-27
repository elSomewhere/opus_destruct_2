// structvox v2 — world side of the engine: streaming, far tiles, persistence, queries, meshing,
// stats, hashes and the design pass (bake).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_set>

#include "svx/base/parallel.hpp"
#include "svx/engine/engine.hpp"
#include "svx/engine/engine_internal.hpp"

namespace svx {

using namespace engine_detail;

namespace {
using Clock = std::chrono::steady_clock;
inline f64 ms_since(Clock::time_point t0) { return std::chrono::duration<f64, std::milli>(Clock::now() - t0).count(); }
}  // namespace

// ---------------------------------------------------------------------------------------------
// Design pass

bool Engine::bake(f64* ms) {
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
      Structure* s = extract(f, 4000000, 1e9, false);
      if (!s) continue;
      for (const FragKey& m : s->refs) mark(m);
      StressOptions so;
      so.rtol = 1e-6;
      if (!s->P.assemble(so)) {
        drop_structure(s->id);
        continue;
      }
      std::fill(s->ext_solved.begin(), s->ext_solved.end(), 0.0);
      const std::vector<f64> F = load_vector(*s);
      static const bool dbg = std::getenv("SVX_DEBUG") != nullptr;
      if (std::getenv("SVX_BENCH_SOLVE")) {
        // solver benchmark: setup, a cold solve to the game tolerance, per-iteration cost
        const int reps = std::atoi(std::getenv("SVX_BENCH_SOLVE"));
        for (int rep = 0; rep < std::max(1, reps); ++rep) {
          const auto t0 = Clock::now();
          s->P.invalidate();
          StressOptions so2;
          s->P.assemble(so2);
          const f64 setup = ms_since(t0);
          std::vector<f64> u0;
          const auto t1 = Clock::now();
          const PcgResult r1 = s->P.solve(F, u0, cfg_.stress_rtol, 1000, false);
          const f64 solve = ms_since(t1);
          std::printf("  bench: %zu nodes: setup %.1f ms, solve to %.0e: %d its %.1f ms (%.2f ms/it)\n", s->P.nodes.size(), setup,
                      cfg_.stress_rtol, r1.iters, solve, solve / std::max(1, r1.iters));
        }
      }
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
          voxels_of(s->refs[size_t(nd)], vox);
          for (const IVec3& p : vox)
            if (grid_.strength(p) < cls) {
              grid_.set_strength(p, cls);
              ++design_.strengthened_voxels;
            }
        }
      }
      for (size_t i = 0; i < s->refs.size(); ++i) {
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

std::vector<u8> Engine::save_delta() const {
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

bool Engine::load_delta(const std::vector<u8>& bytes) {
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
  for (Mover& m : movers_) set_mover_rows(m, m.rows);
  return true;
}

void Engine::enable_streaming(std::unique_ptr<ChunkSource> src, const StreamConfig& sc) {
  source_ = std::move(src);
  stream_ = sc;
  generated_.clear();
  column_count_.clear();
  archive_.clear();
  grid_.lo = {source_->chunk_lo()[0] * kChunk, source_->chunk_lo()[1] * kChunk, source_->chunk_lo()[2] * kChunk};
  grid_.hi = {source_->chunk_hi()[0] * kChunk, source_->chunk_hi()[1] * kChunk, source_->chunk_hi()[2] * kChunk};
  viewer_ = source_->spawn_pos();
  const i32 r = static_cast<i32>(std::ceil(24.0 / grid_.h));
  const IVec3 c = voxel_of(to_v3(viewer_), grid_.h);
  ensure_chunks({c[0] - r, c[1] - r, grid_.lo[2]}, {c[0] + r, c[1] + r, grid_.hi[2]});
}

bool Engine::generate_chunk(u64 key) {
  if (!source_ || generated_.count(key)) return false;
  const IVec3 cc = unkey3(key);
  const IVec3 lo = source_->chunk_lo(), hi = source_->chunk_hi();
  if (cc[0] < lo[0] || cc[1] < lo[1] || cc[2] < lo[2] || cc[0] >= hi[0] || cc[1] >= hi[1] || cc[2] >= hi[2]) return false;
  std::vector<Vox> v;
  const bool any = source_->generate(cc, v);
  insert_generated(key, any, std::move(v));
  return true;
}

void Engine::insert_generated(u64 key, bool any, std::vector<Vox>&& v) {
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
  }
  if (!changed) return;
  grid_.mark_dirty(cc);
  for (int d = 0; d < 3; ++d)
    for (int s = -1; s <= 1; s += 2) {
      IVec3 q = cc;
      q[d] += s;
      grid_.mark_dirty(q);
    }
  // structures that held this chunk as a frontier see its fragments now
  for (int d = 0; d < 3; ++d)
    for (int s = -1; s <= 1; s += 2) {
      IVec3 q = cc;
      q[d] += s;
      mark_owners_stale(key3(q[0], q[1], q[2]));
    }
}

void Engine::evict_chunk(u64 k) {
  const IVec3 cc = unkey3(k);
  if (grid_.is_modified(k)) archive_[k] = std::make_shared<const std::vector<u8>>(grid_.chunk_record(k));
  const bool resident = grid_.chunk(cc) != nullptr;
  mark_owners_stale(k);
  owner_.erase(k);
  frags_.erase(k);
  grid_.remove_chunk(cc);
  generated_.erase(k);
  if (--column_count_[key3(cc[0], cc[1], 0)] <= 0) column_count_.erase(key3(cc[0], cc[1], 0));
  ++st_.evicted_total;
  if (resident) removed_chunks_.push_back(k);
}

void Engine::ensure_chunks(const IVec3& vlo, const IVec3& vhi) {
  if (!source_) return;
  for (i32 x = vlo[0] >> kChunkBits; x <= (vhi[0] - 1) >> kChunkBits; ++x)
    for (i32 y = vlo[1] >> kChunkBits; y <= (vhi[1] - 1) >> kChunkBits; ++y)
      for (i32 z = vlo[2] >> kChunkBits; z <= (vhi[2] - 1) >> kChunkBits; ++z) generate_chunk(key3(x, y, z));
}

bool Engine::chunk_resident(const IVec3& cc) const {
  if (!source_) return true;
  const IVec3 lo = source_->chunk_lo(), hi = source_->chunk_hi();
  for (int q = 0; q < 3; ++q)
    if (cc[q] < lo[q] || cc[q] >= hi[q]) return true;  // outside the world: air
  return generated_.count(key3(cc[0], cc[1], cc[2])) > 0;
}

int Engine::stream_update() {
  if (!source_) return 0;
  int generated = 0;
  const auto t0 = Clock::now();
  const f64 cs = grid_.h * kChunk;
  const IVec3 lo = source_->chunk_lo(), hi = source_->chunk_hi();
  const f64 vx = viewer_[0] / grid_.h + 0.5, vy = viewer_[1] / grid_.h + 0.5;
  auto hdist = [&](const IVec3& cc) {
    const f64 dx = (cc[0] + 0.5) * kChunk - vx, dy = (cc[1] + 0.5) * kChunk - vy;
    return std::sqrt(dx * dx + dy * dy) * grid_.h;
  };
  const i32 rl = static_cast<i32>(std::ceil(stream_.load_radius / cs)) + 1;
  const i32 cx = static_cast<i32>(std::floor(vx / kChunk)), cy = static_cast<i32>(std::floor(vy / kChunk));
  std::vector<std::pair<f64, u64>> want;
  for (i32 x = std::max(lo[0], cx - rl); x <= std::min(hi[0] - 1, cx + rl); ++x)
    for (i32 y = std::max(lo[1], cy - rl); y <= std::min(hi[1] - 1, cy + rl); ++y) {
      const f64 d = hdist({x, y, 0});
      if (d > stream_.load_radius) continue;
      const auto cit = column_count_.find(key3(x, y, 0));
      if (cit != column_count_.end() && cit->second >= hi[2] - lo[2]) continue;
      for (i32 z = lo[2]; z < hi[2]; ++z) {
        const u64 k = key3(x, y, z);
        if (!generated_.count(k)) want.push_back({d, k});
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
      for (const FragKey& f : s->refs) busy.insert(f.chunk);
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
  far_update();
  st_.stream_ms = ms_since(t0);
  return generated;
}

void Engine::far_update() {
  if (stream_.far_radius <= stream_.evict_radius || stream_.far_tile <= 0) return;
  const f64 tile_m = grid_.h * kChunk * stream_.far_tile;
  const IVec3 lo = source_->chunk_lo(), hi = source_->chunk_hi();
  const i32 tlo0 = lo[0] / stream_.far_tile - 1, thi0 = hi[0] / stream_.far_tile + 1;
  const i32 tlo1 = lo[1] / stream_.far_tile - 1, thi1 = hi[1] / stream_.far_tile + 1;
  const f64 vx = viewer_[0] + 0.5 * grid_.h, vy = viewer_[1] + 0.5 * grid_.h;
  auto near_dist = [&](i32 tx, i32 ty) {
    const f64 x0 = tx * tile_m, x1 = x0 + tile_m, y0 = ty * tile_m, y1 = y0 + tile_m;
    const f64 dx = vx < x0 ? x0 - vx : vx > x1 ? vx - x1 : 0.0;
    const f64 dy = vy < y0 ? y0 - vy : vy > y1 ? vy - y1 : 0.0;
    return std::sqrt(dx * dx + dy * dy);
  };
  std::vector<u64> keys(far_sent_.begin(), far_sent_.end());
  std::sort(keys.begin(), keys.end());
  for (u64 k : keys) {
    const IVec3 t = unkey3(k);
    const f64 d = near_dist(t[0], t[1]);
    if (d < stream_.evict_radius - 16.0 || d > stream_.far_radius + tile_m) {
      far_sent_.erase(k);
      far_removed_.push_back({t[0], t[1]});
    }
  }
  const i32 r = static_cast<i32>(std::ceil(stream_.far_radius / tile_m)) + 1;
  const i32 cx = static_cast<i32>(std::floor(vx / tile_m)), cy = static_cast<i32>(std::floor(vy / tile_m));
  std::vector<std::pair<f64, u64>> want;
  for (i32 tx = std::max(tlo0, cx - r); tx <= std::min(thi0, cx + r); ++tx)
    for (i32 ty = std::max(tlo1, cy - r); ty <= std::min(thi1, cy + r); ++ty) {
      const f64 d = near_dist(tx, ty);
      if (d <= stream_.evict_radius || d > stream_.far_radius) continue;
      const u64 k = key3(tx, ty, 0);
      if (!far_sent_.count(k)) want.push_back({d, k});
    }
  std::sort(want.begin(), want.end());
  int budget = stream_.far_tiles_per_tick;
  for (const auto& [d, k] : want) {
    if (budget-- <= 0) break;
    const IVec3 t = unkey3(k);
    far_sent_.insert(k);
    ChunkMesh m = far_mesh(t[0], t[1]);
    if (!m.indices.empty()) far_out_.push_back(std::move(m));
  }
}

ChunkMesh Engine::far_mesh(i32 tx, i32 ty) const {
  ChunkMesh m;
  m.chunk = {tx, ty, 0};
  const i32 f = stream_.far_factor, T = stream_.far_tile;
  const IVec3 clo = source_->chunk_lo(), chi = source_->chunk_hi();
  const IVec3 lo{tx * T * kChunk, ty * T * kChunk, clo[2] * kChunk};
  const i32 n[3] = {T * kChunk / f, T * kChunk / f, (chi[2] - clo[2]) * kChunk / f};
  std::vector<Vox> occ;
  if (!source_->coarse(lo, {n[0], n[1], n[2]}, f, occ)) return m;
  auto at = [&](i32 x, i32 y, i32 z) -> Vox {
    if (z >= n[2]) return kAir;
    if (x < 0 || y < 0 || z < 0 || x >= n[0] || y >= n[1]) return make_vox(MaterialId::Rock, true);
    return occ[(size_t(x) * n[1] + y) * n[2] + z];
  };
  const f64 h = grid_.h;
  std::vector<i32> mask;
  for (int face = 0; face < 6; ++face) {
    const int a = face >> 1, s = (face & 1) ? 1 : -1;
    const int b = (a + 1) % 3, c = (a + 2) % 3;
    const i32 nb = n[b], nc = n[c];
    mask.assign(size_t(nb) * nc, 0);
    for (i32 k = 0; k < n[a]; ++k) {
      for (i32 u = 0; u < nb; ++u)
        for (i32 w = 0; w < nc; ++w) {
          i32 p[3];
          p[a] = k;
          p[b] = u;
          p[c] = w;
          const Vox v = at(p[0], p[1], p[2]);
          p[a] += s;
          mask[size_t(u) * nc + w] = vox_solid(v) && !vox_solid(at(p[0], p[1], p[2])) ? 1 + static_cast<i32>(vox_mat(v)) : 0;
        }
      for (i32 u = 0; u < nb; ++u)
        for (i32 w = 0; w < nc;) {
          const i32 id = mask[size_t(u) * nc + w];
          if (!id) {
            ++w;
            continue;
          }
          i32 ww = 1;
          while (w + ww < nc && mask[size_t(u) * nc + w + ww] == id) ++ww;
          i32 hh = 1;
          for (bool grow = true; grow && u + hh < nb;) {
            for (i32 q = 0; q < ww; ++q)
              if (mask[size_t(u + hh) * nc + w + q] != id) {
                grow = false;
                break;
              }
            if (grow) ++hh;
          }
          for (i32 du = 0; du < hh; ++du)
            for (i32 q = 0; q < ww; ++q) mask[size_t(u + du) * nc + w + q] = 0;
          f64 base[3];
          base[a] = h * (lo[a] + (k + (s > 0 ? 1 : 0)) * f - 0.5);
          base[b] = h * (lo[b] + u * f - 0.5);
          base[c] = h * (lo[c] + w * f - 0.5);
          const f64 eb = h * hh * f, ec = h * ww * f;
          const u32 v0 = static_cast<u32>(m.vertices.size());
          const f64 cr[4][2] = {{0, 0}, {eb, 0}, {eb, ec}, {0, ec}};
          for (const auto& q : cr) {
            MeshVertex mv{};
            f64 pp[3] = {base[0], base[1], base[2]};
            pp[b] += q[0];
            pp[c] += q[1];
            for (int kq = 0; kq < 3; ++kq) mv.pos[kq] = static_cast<f32>(pp[kq]);
            mv.normal[a] = static_cast<i8>(127 * s);
            mv.normal[3] = 127;
            mv.uv[0] = static_cast<f32>(pp[b] * 32.0);
            mv.uv[1] = static_cast<f32>(pp[c] * 32.0);
            mv.texture = static_cast<u16>(0xFF00 + (id - 1));
            mv.light = 255;
            m.vertices.push_back(mv);
          }
          if (s > 0) {
            for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(v0 + i);
          } else {
            for (u32 i : {0u, 2u, 1u, 0u, 3u, 2u}) m.indices.push_back(v0 + i);
          }
          w += ww;
        }
    }
  }
  return m;
}

std::vector<ChunkMesh> Engine::take_far_meshes() {
  std::vector<ChunkMesh> out;
  out.swap(far_out_);
  return out;
}

std::vector<std::array<i32, 2>> Engine::take_far_removed() {
  std::vector<std::array<i32, 2>> out;
  out.swap(far_removed_);
  return out;
}

// ---------------------------------------------------------------------------------------------
// Output

std::vector<ChunkMesh> Engine::take_meshes(const MeshOptions& base) {
  const auto t0 = Clock::now();
  mesh_base_ = base;
  std::vector<u64> keys = grid_.take_dirty();
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  MeshOptions mo = base;
  // debug views read the fragment caches (made current before meshing, serially)
  std::unordered_map<u64, std::vector<u8>> frag_value;
  if (par_.debug_view != 0) {
    std::unordered_map<i64, std::vector<f32>> node_phi;
    for (u64 k : keys) {
      const IVec3 cc = unkey3(k);
      if (!grid_.chunk(cc)) continue;
      FragChunk& fc = frag_chunk(cc);
      std::vector<u8> v(fc.frags.size(), 0);
      for (i32 f = 0; f < static_cast<i32>(fc.frags.size()); ++f) {
        if (par_.debug_view == 2) {
          v[size_t(f)] = static_cast<u8>(1 + mix64(frag_ident(k, fc.frags[size_t(f)].first)) % 254);
          continue;
        }
        Structure* s = structure(owner_of({k, f}));
        if (!s) continue;
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
      frag_value[k] = std::move(v);
    }
    mo.debug = [&](const IVec3& p) -> u8 {
      const IVec3 cc = chunk_of(p);
      const u64 k = key3(cc[0], cc[1], cc[2]);
      const auto it = frag_value.find(k);
      const auto ft = frags_.find(k);
      if (it == frag_value.end() || ft == frags_.end()) return 0;
      const i32 f = ft->second.at(chunk_index(p));
      return f >= 0 && f < static_cast<i32>(it->second.size()) ? it->second[size_t(f)] : 0;
    };
  }
  std::vector<ChunkMesh> meshes(keys.size());
  std::optional<SerialScope> serial;
  if (!mo.concurrent) serial.emplace();
  parallel_for(static_cast<i64>(keys.size()), 1, [&](i64 b0, i64 e0) {
    for (i64 j = b0; j < e0; ++j) meshes[size_t(j)] = mesh_chunk(grid_, unkey3(keys[size_t(j)]), mo, false);
  });
  std::vector<ChunkMesh> out;
  for (size_t j = 0; j < keys.size(); ++j) {
    if (meshes[j].vertices.empty()) {
      removed_chunks_.push_back(keys[j]);
      continue;
    }
    out.push_back(std::move(meshes[j]));
  }
  st_.mesh_ms = ms_since(t0);
  return out;
}

std::vector<u64> Engine::take_removed_chunks() {
  std::vector<u64> out;
  out.swap(removed_chunks_);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<EngineEvent> Engine::take_events() {
  std::vector<EngineEvent> out;
  out.swap(events_);
  return out;
}

EngineStats Engine::stats() const {
  EngineStats s = st_;
  s.voxels = grid_.solid_count();
  s.chunks = static_cast<i64>(grid_.chunks().size());
  s.memory_mb = f64(grid_.memory_bytes()) / (1024.0 * 1024.0);
  s.structures = static_cast<i32>(structures_.size());
  s.resident_chunks = static_cast<i64>(generated_.size());
  s.archived_chunks = static_cast<i64>(archive_.size());
  s.bodies = static_cast<i32>(rigid_.bodies.size());
  s.awake = rigid_.awake_count();
  s.contacts = static_cast<i32>(rigid_.contacts().size());
  s.movers = static_cast<i32>(movers_.size());
  return s;
}

u64 Engine::state_hash() const {
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

u64 Engine::session_hash() const {
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
  for (const Mover& m : movers_) {
    mix(bits(m.level));
    mix(bits(m.timer));
    mix(static_cast<u64>(static_cast<u32>(m.move)) | (static_cast<u64>(m.leg) << 32) | (static_cast<u64>(m.rows) << 40) |
        (m.stopped ? 1ull << 62 : 0ull) | (m.disabled ? 1ull << 63 : 0ull));
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

RayHit Engine::raycast(const std::array<f64, 3>& origin, const std::array<f64, 3>& dir, f64 max_dist) const {
  RayHit hit;
  const f64 h = grid_.h;
  const V3 o = to_v3(origin);
  V3 d = to_v3(dir);
  const f64 len = norm(d);
  if (len <= 0.0) return hit;
  d *= 1.0 / len;
  f64 t;
  int face, step[3];
  IVec3 v;
  if (dda(o, d, h, max_dist, [&](const IVec3& p) { return vox_solid(grid_.get(p)); }, &t, &face, step, &v)) {
    hit.hit = true;
    hit.distance = t;
    hit.pos = to_arr(o + d * t);
    hit.normal = {0, 0, 0};
    if (face >= 0) hit.normal[size_t(face)] = -step[face];
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
        hit.pos = to_arr(o + d * tt);
        V3 n{0, 0, 0};
        if (face >= 0) n[face] = -step[face];
        hit.normal = to_arr(rotate(b.q, n));
        hit.material = static_cast<int>(vox_mat(b.shape.get(v)));
        hit.voxel = v;
        hit.body = b.id;
      }
    }
  }
  return hit;
}

CollideResult Engine::collide(const std::array<f64, 3>& mn, const std::array<f64, 3>& mx, const std::array<f64, 3>& move) const {
  CollideResult res;
  const f64 h = grid_.h;
  const f64 eps = 1e-4;
  std::array<f64, 3> lo = mn, hi = mx;
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
    res.move[size_t(a)] = dm;
  }
  return res;
}

}  // namespace svx
