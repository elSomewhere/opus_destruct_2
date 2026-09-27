#include "svx/env/smoke.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "svx/env/fire.hpp"

namespace svx {

namespace {

constexpr f32 kGone = 1e-3f;  // thinner than this: clear air

inline i32 floor_div(i32 a, i32 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline IVec3 cell_chunk(const IVec3& c) {
  return {floor_div(c[0], SmokeSystem::kSide), floor_div(c[1], SmokeSystem::kSide), floor_div(c[2], SmokeSystem::kSide)};
}
inline i32 cell_index(const IVec3& c) {
  constexpr i32 S = SmokeSystem::kSide;
  const i32 x = c[0] - floor_div(c[0], S) * S, y = c[1] - floor_div(c[1], S) * S, z = c[2] - floor_div(c[2], S) * S;
  return (x * S + y) * S + z;
}
// the cell holding world point p (voxel p's centre is at h p)
inline IVec3 cell_at(const V3& p, f64 h) {
  const f64 cm = h * SmokeSystem::kCell;
  return {static_cast<i32>(std::floor((p.x + 0.5 * h) / cm)), static_cast<i32>(std::floor((p.y + 0.5 * h) / cm)),
          static_cast<i32>(std::floor((p.z + 0.5 * h) / cm))};
}
inline V3 cell_centre(const IVec3& c, f64 h) {
  const f64 o = 0.5 * (SmokeSystem::kCell - 1);
  return {h * (c[0] * SmokeSystem::kCell + o), h * (c[1] * SmokeSystem::kCell + o), h * (c[2] * SmokeSystem::kCell + o)};
}

}  // namespace

namespace {

inline f64 clampf(f64 v, f64 lo, f64 hi, f64 fallback) { return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; }

SmokeConfig sanitized(SmokeConfig c) {
  c.step_s = clampf(c.step_s, 0.0, 0.5, 0.1);
  c.rise = clampf(c.rise, 0.0, 20.0, 0.8);
  c.diffuse = clampf(c.diffuse, 0.0, 5.0, 0.6);
  c.lifetime = clampf(c.lifetime, 0.5, 3600.0, 20.0);
  for (int a = 0; a < 3; ++a) c.wind[a] = clampf(c.wind[a], -50.0, 50.0, 0.0);
  c.per_flame = clampf(c.per_flame, 0.0, 10.0, 0.12);
  c.ceiling_m = clampf(c.ceiling_m, 0.0, 1000.0, 24.0);
  c.max_blocks = std::clamp(c.max_blocks, 0, 1 << 20);
  return c;
}

// e^-x for small x, from + - * / only (the same on every platform): a [2/2] Pade approximant,
// exact to 1e-12 below x = 0.05, applied in halvings beyond.
f64 exp_neg(f64 x) {
  int k = 0;
  while (x > 0.05 && k < 60) {
    x *= 0.5;
    ++k;
  }
  const f64 x2 = x * x / 12.0;
  f64 r = (1.0 - 0.5 * x + x2) / (1.0 + 0.5 * x + x2);
  for (int i = 0; i < k; ++i) r *= r;
  return r;
}

}  // namespace

SmokeSystem::SmokeSystem(const SmokeConfig& c) : cfg_(sanitized(c)) {}

void SmokeSystem::configure(const SmokeConfig& c) {
  cfg_ = sanitized(c);
  clock_ = std::min(clock_, cfg_.step_s);
}

void SmokeSystem::emit(const V3& pos, f64 amount) { emit_sphere(pos, 0.0, amount); }

void SmokeSystem::emit_sphere(const V3& pos, f64 radius, f64 amount) {
  if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) || !std::isfinite(radius) || !(amount > 0.0) ||
      !std::isfinite(amount))
    return;
  if (pending_.size() < kMaxPending) pending_.push_back({pos, std::clamp(radius, 0.0, kMaxEmitRadius), std::min(amount, 1e4)});
}

void SmokeSystem::on_load(World& w) {
  (void)w;
  blocks_.clear();
  solid_.clear();
  pending_.clear();
  clock_ = 0.0;
  st_ = Stats{};
}

void SmokeSystem::on_evicted(World& w, const std::vector<u64>& chunks) {
  (void)w;
  for (u64 k : chunks) {
    blocks_.erase(k);
    solid_.erase(k);
  }
}

void SmokeSystem::on_voxels_changed(World& w, const std::vector<u64>& chunks) {
  (void)w;
  for (u64 k : chunks) {
    const auto it = solid_.find(k);
    if (it != solid_.end()) it->second.stale = true;
  }
}

const SmokeSystem::Solid& SmokeSystem::solid(const World& w, const IVec3& cc) {
  static const Solid kClosed = [] {
    Solid s;
    s.s.fill(1);
    s.stale = false;
    return s;
  }();
  if (!w.chunk_resident(cc)) return kClosed;
  const u64 k = key3(cc[0], cc[1], cc[2]);
  Solid& m = solid_[k];  // (never cleared within a step: the references stay valid; prune_solid after it)
  if (!m.stale) return m;
  m.stale = false;
  const Chunk* ch = w.grid().chunk(cc);
  if (!ch || ch->uniform) {
    m.s.fill(ch && vox_solid(ch->value) ? 1 : 0);
  } else {
    // A cell is closed if it is mostly solid, or holds a wall: a voxel plane across it (along
    // any axis) nearly all solid. (Thin walls, floors and roofs hold smoke; windows let it by.)
    std::vector<u8> n(size_t(kCells) * 3 * kCell, 0);  // cell, axis, plane -> solid voxels
    std::array<u8, kCells> total{};
    for (i32 x = 0; x < kChunk; ++x)
      for (i32 y = 0; y < kChunk; ++y)
        for (i32 z = 0; z < kChunk; ++z) {
          if (!vox_solid(ch->v[size_t((x * kChunk + y) * kChunk + z)])) continue;
          const size_t c = size_t(((x / kCell) * kSide + y / kCell) * kSide + z / kCell);
          ++total[c];
          ++n[(c * 3 + 0) * kCell + size_t(x % kCell)];
          ++n[(c * 3 + 1) * kCell + size_t(y % kCell)];
          ++n[(c * 3 + 2) * kCell + size_t(z % kCell)];
        }
    constexpr i32 kPlane = kCell * kCell, kWall = kPlane * 3 / 4;
    for (i32 i = 0; i < kCells; ++i) {
      bool closed = total[size_t(i)] * 2 > kPlane * kCell;
      for (size_t q = 0; q < 3 * kCell && !closed; ++q) closed = n[size_t(i) * 3 * kCell + q] >= kWall;
      m.s[size_t(i)] = closed ? 1 : 0;
    }
  }
  // (smoke in cells that became solid is gone)
  const auto bt = blocks_.find(k);
  if (bt != blocks_.end())
    for (i32 i = 0; i < kCells; ++i)
      if (m.s[size_t(i)]) bt->second.d[size_t(i)] = 0.0f;
  return m;
}

SmokeSystem::Block* SmokeSystem::block(const IVec3& cc, bool create) {
  const u64 k = key3(cc[0], cc[1], cc[2]);
  const auto it = blocks_.find(k);
  if (it != blocks_.end()) return &it->second;
  return create ? &blocks_[k] : nullptr;
}

void SmokeSystem::add(World& w, const IVec3& c, f64 amount) {
  const IVec3 cc = cell_chunk(c);
  const i32 i = cell_index(c);
  if (solid(w, cc).s[size_t(i)]) return;
  // (new blocks within twice the budget: the rest waits for the step's trim)
  Block* b = block(cc, static_cast<i32>(blocks_.size()) < 2 * cfg_.max_blocks);
  if (!b) {
    ++st_.dropped;
    return;
  }
  b->d[size_t(i)] = std::min(8.0f, b->d[size_t(i)] + static_cast<f32>(amount));
}

void SmokeSystem::add_at(World& w, const V3& pos, f64 amount) {
  // The cell holding pos, or - a closed cell (a flame on a wall, a floor) - the first open one
  // next to it: up, the sides, down.
  const IVec3 c = cell_at(pos, w.voxel_size());
  constexpr int kOrder[7][3] = {{0, 0, 0}, {0, 0, 1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, -1}};
  for (const auto& o : kOrder) {
    const IVec3 q{c[0] + o[0], c[1] + o[1], c[2] + o[2]};
    if (solid(w, cell_chunk(q)).s[size_t(cell_index(q))]) continue;
    add(w, q, amount);
    return;
  }
}

void SmokeSystem::prune_solid() {
  // (the masks of chunks with a block or next to one; the rest go once the cache has grown)
  if (solid_.size() <= static_cast<size_t>(8 * std::max<size_t>(64, blocks_.size()))) return;
  std::unordered_map<u64, Solid> keep;
  for (const auto& [k, b] : blocks_) {
    const IVec3 c = unkey3(k);
    for (int d = -1; d < 6; ++d) {
      IVec3 q = c;
      if (d >= 0) q[d / 2] += (d & 1) ? -1 : 1;
      const u64 qk = key3(q[0], q[1], q[2]);
      const auto it = solid_.find(qk);
      if (it != solid_.end()) keep.emplace(qk, it->second);
    }
  }
  solid_.swap(keep);
}

f64 SmokeSystem::density(const World& w, const V3& pos) const {
  if (!w.in_range(pos)) return 0.0;
  const IVec3 c = cell_at(pos, w.voxel_size());
  const IVec3 cc = cell_chunk(c);
  const auto it = blocks_.find(key3(cc[0], cc[1], cc[2]));
  return it == blocks_.end() ? 0.0 : it->second.d[size_t(cell_index(c))];
}

void SmokeSystem::step(World& w, f64 dt) {
  const auto t0 = std::chrono::steady_clock::now();
  if (cfg_.enabled) {
    const f64 s = std::max(cfg_.step_s, w.config().dt);
    clock_ = std::min(clock_ + dt, 4.0 * s);  // (a long pause does not come back as a burst)
    while (clock_ >= s - 1e-9) {
      clock_ -= s;
      smoke_step(w, s);
    }
  }
  st_.step_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
  st_.total_ms += st_.step_ms;
}

void SmokeSystem::smoke_step(World& w, f64 dt) {
  ++st_.steps;
  const f64 h = w.voxel_size(), cm = h * kCell;
  // sources
  if (fire_)
    for (const auto& f : fire_->flames()) add_at(w, f.pos + V3{0.0, 0.0, h}, cfg_.per_flame * dt);
  for (const Emit& e : pending_) {
    if (!w.in_range(e.pos)) continue;
    const i32 r = static_cast<i32>(std::ceil(e.radius / cm));
    const IVec3 c0 = cell_at(e.pos, h);
    std::vector<IVec3> in;
    for (i32 x = -r; x <= r; ++x)
      for (i32 y = -r; y <= r; ++y)
        for (i32 z = -r; z <= r; ++z) {
          const IVec3 c{c0[0] + x, c0[1] + y, c0[2] + z};
          if (norm(cell_centre(c, h) - e.pos) <= e.radius + 0.5 * cm) in.push_back(c);
        }
    if (in.empty()) {  // (a point: its cell, or an open one next to it)
      add_at(w, e.pos, e.amount);
      continue;
    }
    for (const IVec3& c : in) add(w, c, e.amount / static_cast<f64>(in.size()));
  }
  pending_.clear();
  // transport
  const f32 up = static_cast<f32>(std::min(0.45, cfg_.rise * dt / cm));
  const f32 dif = static_cast<f32>(std::min(0.12, cfg_.diffuse * dt));
  f32 wf[3];
  for (int a = 0; a < 3; ++a) wf[a] = static_cast<f32>(std::min(0.3, std::abs(cfg_.wind[a]) * dt / cm));
  const f32 decay = static_cast<f32>(exp_neg(dt / cfg_.lifetime));
  const i32 top = static_cast<i32>(std::floor(std::min(1e8, (w.grid().hi[2] * h + cfg_.ceiling_m) / cm)));
  for (auto& [k, b] : blocks_) {
    solid(w, unkey3(k));  // (refreshed if stale)
    b.next.fill(0.0f);
  }
  std::vector<u64> keys;
  keys.reserve(blocks_.size());
  for (const auto& [k, b] : blocks_) keys.push_back(k);
  constexpr int kDir[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (u64 k : keys) {
    Block& b = blocks_.find(k)->second;
    const IVec3 cc = unkey3(k);
    const Solid* own = &solid(w, cc);
    // (the six neighbouring blocks' masks and blocks, looked up when a cell first needs them)
    const Solid* nmask[6] = {};
    Block* nblk[6] = {};
    for (i32 i = 0; i < kCells; ++i) {
      const f32 d = b.d[size_t(i)];
      if (d <= 0.0f) continue;
      const i32 lx = i / (kSide * kSide), ly = (i / kSide) % kSide, lz = i % kSide;
      const IVec3 c{cc[0] * kSide + lx, cc[1] * kSide + ly, cc[2] * kSide + lz};
      struct Flow {
        f32* to;
        f32 f;
      } flows[6];
      int n = 0;
      f32 total = 0.0f;
      // the six neighbours: open (their next density), and how thick they are
      f32* to[6] = {};
      f32 dq[6] = {};
      bool open[6] = {};
      for (int dn = 0; dn < 6; ++dn) {
        if (c[2] + kDir[dn][2] > top) {
          open[dn] = dn == 4;  // (up and away)
          continue;
        }
        const i32 qx = lx + kDir[dn][0], qy = ly + kDir[dn][1], qz = lz + kDir[dn][2];
        const bool inside = qx >= 0 && qx < kSide && qy >= 0 && qy < kSide && qz >= 0 && qz < kSide;
        const i32 j = ((qx & (kSide - 1)) * kSide + (qy & (kSide - 1))) * kSide + (qz & (kSide - 1));
        Block* nb;
        if (inside) {
          if (own->s[size_t(j)]) continue;
          nb = &b;
        } else {
          if (!nmask[dn]) {
            const IVec3 qc{cc[0] + kDir[dn][0], cc[1] + kDir[dn][1], cc[2] + kDir[dn][2]};
            nmask[dn] = &solid(w, qc);
          }
          if (nmask[dn]->s[size_t(j)]) continue;
          if (!nblk[dn]) nblk[dn] = block({cc[0] + kDir[dn][0], cc[1] + kDir[dn][1], cc[2] + kDir[dn][2]}, true);
          nb = nblk[dn];
        }
        open[dn] = true;
        to[dn] = &nb->next[size_t(j)];
        dq[dn] = nb->d[size_t(j)];
      }
      // buoyancy: up, or along the ceiling towards thinner smoke (a ceiling jet)
      f32 jet[6] = {};
      if (open[4]) {
        jet[4] = up * d;
      } else {
        f32 lower = 0.0f;
        for (int dn = 0; dn < 4; ++dn)
          if (open[dn] && dq[dn] < d) lower += d - dq[dn];
        if (lower > 0.0f)
          for (int dn = 0; dn < 4; ++dn)
            if (open[dn] && dq[dn] < d) jet[dn] = up * d * (d - dq[dn]) / lower;
      }
      for (int dn = 0; dn < 6; ++dn) {
        if (!open[dn]) continue;
        f32 f = jet[dn];
        if (dq[dn] < d) f += dif * (d - dq[dn]) * 0.5f;
        const int a = dn / 2;
        if (wf[a] > 0.0f && ((dn & 1) == 0) == (cfg_.wind[a] > 0.0)) f += wf[a] * d;
        if (f <= 0.0f) continue;
        flows[n++] = {to[dn], f};
        total += f;
      }
      const f32 scale = total > d ? d / total : 1.0f;
      f32 kept = d;
      for (int q = 0; q < n; ++q) {
        const f32 f = flows[q].f * scale;
        kept -= f;
        if (flows[q].to) *flows[q].to += f;
      }
      b.next[size_t(i)] += std::max(0.0f, kept);
    }
  }
  // dissipation; empty blocks go
  st_.cells = 0;
  for (auto it = blocks_.begin(); it != blocks_.end();) {
    Block& b = it->second;
    bool any = false;
    for (i32 i = 0; i < kCells; ++i) {
      f32 v = b.next[size_t(i)] * decay;
      if (v < kGone) v = 0.0f;
      b.d[size_t(i)] = v;
      if (v > 0.0f) {
        any = true;
        ++st_.cells;
      }
    }
    it = any ? std::next(it) : blocks_.erase(it);
  }
  // budget: the thinnest blocks go
  if (static_cast<i32>(blocks_.size()) > cfg_.max_blocks) {
    std::vector<std::pair<f32, u64>> sums;
    for (const auto& [k, b] : blocks_) {
      f32 s = 0.0f;
      for (f32 v : b.d) s += v;
      sums.push_back({s, k});
    }
    std::sort(sums.begin(), sums.end());
    const size_t drop = blocks_.size() - static_cast<size_t>(std::max(0, cfg_.max_blocks));
    for (size_t q = 0; q < drop; ++q) blocks_.erase(sums[q].second);
    st_.dropped += static_cast<i64>(drop);
  }
  st_.blocks = static_cast<i32>(blocks_.size());
  prune_solid();
}

std::vector<SmokeSystem::Cell> SmokeSystem::cells(const World& w, i32 max, f32 min_density) const {
  std::vector<Cell> out;
  if (max <= 0) return out;
  const f64 h = w.voxel_size();
  for (const auto& [k, b] : blocks_) {
    const IVec3 cc = unkey3(k);
    for (i32 i = 0; i < kCells; ++i) {
      const f32 d = b.d[size_t(i)];
      if (d < min_density) continue;
      const IVec3 c{cc[0] * kSide + i / (kSide * kSide), cc[1] * kSide + (i / kSide) % kSide, cc[2] * kSide + i % kSide};
      out.push_back({cell_centre(c, h), d});
    }
  }
  if (static_cast<i32>(out.size()) > max) {
    std::nth_element(out.begin(), out.begin() + max, out.end(), [](const Cell& a, const Cell& b) { return a.density > b.density; });
    out.resize(static_cast<size_t>(max));
  }
  return out;
}

i64 SmokeSystem::memory_bytes() const {
  // (map nodes: the block and about four pointers)
  return static_cast<i64>(blocks_.size() * (sizeof(Block) + 48) + solid_.size() * (sizeof(Solid) + 32) +
                          pending_.capacity() * sizeof(Emit) + sizeof(*this));
}

u64 SmokeSystem::state_hash() const {
  u64 hsh = 1469598103934665603ull;
  for (const auto& [k, b] : blocks_) {
    hsh = (hsh ^ k) * 1099511628211ull;
    for (i32 i = 0; i < kCells; ++i) {
      if (b.d[size_t(i)] == 0.0f) continue;
      u32 bits;
      std::memcpy(&bits, &b.d[size_t(i)], 4);
      hsh = (hsh ^ (static_cast<u64>(i) << 32 | bits)) * 1099511628211ull;
    }
  }
  return hsh;
}

}  // namespace svx
