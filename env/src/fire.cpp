#include "svx/env/fire.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace svx {

namespace {

constexpr f64 kUnit = FireSystem::kHeatUnit;
constexpr f64 kMaxC = 255.0 * kUnit;  // (the hottest a voxel can be)

inline u64 mix(u64 x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}
inline f64 u01(u64 h) { return static_cast<f64>(h >> 11) * (1.0 / 9007199254740992.0); }
// Heat lost to the air: a fraction of the excess, and at least a little (the last degrees go).
inline f64 cooling(const FireMaterial& m, f64 T, f64 dt) { return std::max(m.cool * T, 4.0) * dt; }
inline u64 vkey(const IVec3& p) { return key3(p[0], p[1], p[2]); }
inline u64 ckey(const IVec3& p) {
  const IVec3 c = chunk_of(p);
  return key3(c[0], c[1], c[2]);
}
inline IVec3 voxel_at(const V3& X, f64 h) {
  return {static_cast<i32>(std::floor(X.x / h + 0.5)), static_cast<i32>(std::floor(X.y / h + 0.5)), static_cast<i32>(std::floor(X.z / h + 0.5))};
}
inline f64 clampf(f64 v, f64 lo, f64 hi, f64 fallback) { return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; }
inline u8 units(f64 celsius) { return static_cast<u8>(std::clamp(celsius / kUnit, 0.0, 255.0)); }

// The solids a flame heats (offsets from the burning voxel, z up) and how much: up the wall
// above it, less to the sides, hardly below.
struct Reach {
  i8 dx, dy, dz;
  f32 w;
};
constexpr Reach kFlame[] = {
    {0, 0, 1, 1.0f},    {0, 0, 2, 0.6f},    {0, 0, 3, 0.3f},    {1, 0, 1, 0.5f},    {-1, 0, 1, 0.5f},  {0, 1, 1, 0.5f},
    {0, -1, 1, 0.5f},   {1, 1, 1, 0.3f},    {1, -1, 1, 0.3f},   {-1, 1, 1, 0.3f},   {-1, -1, 1, 0.3f}, {1, 0, 2, 0.25f},
    {-1, 0, 2, 0.25f},  {0, 1, 2, 0.25f},   {0, -1, 2, 0.25f},  {1, 0, 0, 0.33f},   {-1, 0, 0, 0.33f}, {0, 1, 0, 0.33f},
    {0, -1, 0, 0.33f},  {2, 0, 0, 0.12f},   {-2, 0, 0, 0.12f},  {0, 2, 0, 0.12f},   {0, -2, 0, 0.12f}, {0, 0, -1, 0.05f},
};
constexpr i32 kFlameReach = 3;  // voxels: the farthest a flame reaches
constexpr int kFace[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

// Grid reads with the last chunk remembered (fire steps read the same few chunks many times).
struct Look {
  const VoxelGrid& g;
  u64 key = ~0ull;
  const Chunk* c = nullptr;
  const Chunk* at(const IVec3& p) {
    const u64 k = ckey(p);
    if (k != key) {
      key = k;
      c = g.chunk(chunk_of(p));
    }
    return c;
  }
  Vox vox(const IVec3& p) {
    const Chunk* ch = at(p);
    if (!ch) return kAir;
    return ch->uniform ? ch->value : ch->v[size_t(chunk_index(p))];
  }
  u8 layer(int L, const IVec3& p) {
    if (L < 0) return 0;
    const Chunk* ch = at(p);
    return ch && !ch->layer[size_t(L)].empty() ? ch->layer[size_t(L)][size_t(chunk_index(p))] : 0;
  }
  bool exposed(const IVec3& p) {  // (a face in the air: it can burn, a flame can reach it)
    for (const auto& f : kFace)
      if (!vox_solid(vox({p[0] + f[0], p[1] + f[1], p[2] + f[2]}))) return true;
    return false;
  }
  bool wet(int L, const IVec3& p) {  // (water in or next to it)
    if (L < 0) return false;
    if (layer(L, p)) return true;
    for (const auto& f : kFace)
      if (layer(L, {p[0] + f[0], p[1] + f[1], p[2] + f[2]})) return true;
    return false;
  }
};

FireMaterial sanitized(FireMaterial m) {
  m.ignition_c = clampf(m.ignition_c, kUnit, kMaxC, 300.0);
  m.burn_s = clampf(m.burn_s, 0.5, 1e6, 40.0);
  m.flame_c = clampf(m.flame_c, 0.0, kMaxC, 900.0);
  m.conduct = clampf(m.conduct, 0.0, 2.0, 0.05);  // (explicit steps: stable up to 1 / (6 dt))
  m.cool = clampf(m.cool, 0.0, 5.0, 0.05);
  m.char_damage = clampf(m.char_damage, 0.0, 1.0, 0.9);
  m.weaken_c = clampf(m.weaken_c, 0.0, 1e6, 0.0);
  m.gone_c = clampf(m.gone_c, m.weaken_c, 1e6, m.weaken_c);
  return m;
}

FireConfig sanitized(FireConfig c) {
  c.step_s = clampf(c.step_s, 0.0, 0.25, 0.1);
  c.flame_reach = clampf(c.flame_reach, 0.0, 2.0, 0.15);
  c.max_hot = std::clamp(c.max_hot, 0, 4000000);
  c.ignition_jitter = clampf(c.ignition_jitter, 0.0, 0.9, 0.15);
  c.burn_jitter = clampf(c.burn_jitter, 0.0, 0.9, 0.35);
  c.quench_c = clampf(c.quench_c, 0.0, kMaxC, 60.0);
  c.damage_quantum = std::clamp(c.damage_quantum, 1, 64);
  c.piece_batch_steps = std::clamp(c.piece_batch_steps, 1, 1000);
  c.glow_c = clampf(c.glow_c, 0.0, kMaxC + 1.0, 520.0);
  return c;
}

}  // namespace

FireSystem::FireSystem(const FireConfig& c) : cfg_(sanitized(c)) {
  FireMaterial inert;
  inert.conduct = 0.02;
  inert.cool = 0.05;
  mats_.fill(inert);
  FireMaterial wood;
  wood.combustible = true;
  wood.ignition_c = 300.0;
  wood.burn_s = 40.0;
  wood.flame_c = 900.0;
  wood.conduct = 0.05;
  wood.cool = 0.06;
  wood.char_damage = 0.9;
  mats_[size_t(MaterialId::Wood)] = wood;
  FireMaterial steel;  // (conducts; weakens from 400 degC, little left at 1000)
  steel.conduct = 0.8;
  steel.cool = 0.03;
  steel.weaken_c = 400.0;
  steel.gone_c = 1000.0;
  mats_[size_t(MaterialId::Steel)] = steel;
  FireMaterial bar = steel;  // (inside concrete: slower to heat)
  bar.conduct = 0.3;
  bar.weaken_c = 450.0;
  bar.gone_c = 1100.0;
  mats_[size_t(MaterialId::Rebar)] = bar;
  FireMaterial conc;  // (spalls and loses strength slowly past 600 degC)
  conc.conduct = 0.02;
  conc.cool = 0.04;
  conc.weaken_c = 600.0;
  conc.gone_c = 1400.0;
  mats_[size_t(MaterialId::Concrete)] = conc;
  mats_[size_t(MaterialId::Rc)] = conc;
  FireMaterial masonry = conc;
  masonry.weaken_c = 900.0;
  masonry.gone_c = 1800.0;
  mats_[size_t(MaterialId::Masonry)] = masonry;
  mats_[size_t(MaterialId::Stone)] = masonry;
  FireMaterial glass;  // (cracks in the heat)
  glass.conduct = 0.05;
  glass.cool = 0.05;
  glass.weaken_c = 250.0;
  glass.gone_c = 700.0;
  mats_[size_t(MaterialId::Glass)] = glass;
}

void FireSystem::configure(const FireConfig& c) {
  cfg_ = sanitized(c);
  clock_ = std::min(clock_, cfg_.step_s);
}

void FireSystem::set_material(MaterialId id, const FireMaterial& m) { mats_[static_cast<size_t>(id) & 0x7F] = sanitized(m); }

u8 FireSystem::glow_units() const { return static_cast<u8>(std::min(255.0, std::ceil(cfg_.glow_c / kUnit))); }

void FireSystem::attach(World& w) {
  heat_ = w.add_layer({"heat", false, LayerBind::Solid});
  burn_ = w.add_layer({"burn", true, LayerBind::Solid});
  water_ = w.layer_index("water");
}

void FireSystem::on_load(World& w) {
  hot_.clear();
  flames_.clear();
  glow_changes_.clear();
  piece_changes_.clear();
  clock_ = 0.0;
  steps_ = 0;  // (a session replayed from a load: the same dithering)
  st_ = Stats{};
  water_ = w.layer_index("water");
  if (!ok()) return;
  // (heat the level came with)
  std::vector<u64> chunks;
  for (const auto& [k, c] : w.grid().chunks())
    if (!c.layer[size_t(heat_)].empty()) chunks.push_back(k);
  track_heat(w, chunks);
}

void FireSystem::on_generated(World& w, const std::vector<u64>& chunks) {
  if (ok()) track_heat(w, chunks);  // (heat a source made)
}

void FireSystem::track_heat(const World& w, const std::vector<u64>& chunks) {
  size_t n0 = hot_.size();
  for (u64 k : chunks) {
    const Chunk* c = w.grid().chunk(unkey3(k));
    if (!c || c->layer[size_t(heat_)].empty()) continue;
    const IVec3 cc = unkey3(k);
    const std::vector<u8>& a = c->layer[size_t(heat_)];
    for (i32 i = 0; i < kChunkVox; ++i)
      if (a[size_t(i)]) hot_.push_back(key3(cc[0] * kChunk + i / (kChunk * kChunk), cc[1] * kChunk + (i / kChunk) % kChunk, cc[2] * kChunk + i % kChunk));
  }
  if (hot_.size() == n0) return;
  std::sort(hot_.begin(), hot_.end());
  hot_.erase(std::unique(hot_.begin(), hot_.end()), hot_.end());
}

void FireSystem::on_evicted(World& w, const std::vector<u64>& chunks) {
  (void)w;
  const std::unordered_set<u64> gone(chunks.begin(), chunks.end());
  hot_.erase(std::remove_if(hot_.begin(), hot_.end(), [&](u64 k) { return gone.count(ckey(unkey3(k))) > 0; }), hot_.end());
  flames_.erase(std::remove_if(flames_.begin(), flames_.end(), [&](const Flame& f) { return f.piece == 0 && gone.count(ckey(voxel_at(f.pos, w.voxel_size()))); }),
                flames_.end());
}

f64 FireSystem::ignition(const FireMaterial& m, u64 key) const {
  return m.ignition_c * (1.0 + cfg_.ignition_jitter * (2.0 * u01(mix(key ^ cfg_.seed ^ 0x1617ull)) - 1.0));
}

f64 FireSystem::burn_time(const FireMaterial& m, u64 key) const {
  return std::max(0.5, m.burn_s * (1.0 + cfg_.burn_jitter * (2.0 * u01(mix(key ^ cfg_.seed ^ 0x2718ull)) - 1.0)));
}

u8 FireSystem::dither(f64 v, u64 key) const {
  if (!(v > 0.0)) return 0;
  if (v >= 255.0) return 255;
  const f64 fl = std::floor(v);
  return static_cast<u8>(fl + (u01(mix(key ^ static_cast<u64>(steps_) * 0x9E37ull)) < v - fl ? 1.0 : 0.0));
}

void FireSystem::step(World& w, f64 dt) {
  const auto t0 = std::chrono::steady_clock::now();
  if (cfg_.enabled && ok()) {
    const f64 s = std::max(cfg_.step_s, w.config().dt);
    // (a long pause does not come back as a burst of steps)
    clock_ = std::min(clock_ + dt, 4.0 * s);
    while (clock_ >= s - 1e-9) {
      clock_ -= s;
      fire_step(w, s);
    }
  }
  st_.step_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
  st_.total_ms += st_.step_ms;
}

void FireSystem::fire_step(World& w, f64 dt) {
  ++steps_;
  ++st_.steps;
  water_ = w.layer_index("water");
  flames_.clear();
  Look g{w.grid()};
  const f64 h = w.voxel_size();
  const u8 quantum = static_cast<u8>(cfg_.damage_quantum);
  std::unordered_map<u64, f64> D;  // temperature changes (degC)
  D.reserve(hot_.size() * 4);
  std::vector<LayerEdit> burn_edits, damage_edits;
  std::vector<VoxelEdit> gone;
  std::vector<u64> burning_keys;
  st_.burning = 0;
  auto temp = [&](const IVec3& p) { return g.layer(heat_, p) * kUnit; };
  // (damage rises only, and is written in steps of the quantum: the structures are judged again
  // at each write)
  auto damage = [&](const IVec3& p, f64 d01) {
    const u8 d = static_cast<u8>(std::lround(254.0 * std::clamp(d01, 0.0, 1.0)));
    const u8 cur = g.layer(World::kDamageLayer, p);
    if (d > cur && (d - cur >= quantum || d == 254)) damage_edits.push_back({p, d});
  };
  for (u64 key : hot_) {
    const IVec3 p = unkey3(key);
    const Vox v = g.vox(p);
    if (!vox_solid(v)) continue;
    const FireMaterial& m = mats_[size_t(vox_mat(v)) & 0x7F];
    const f64 T = temp(p);
    if (g.wet(water_, p)) {  // (quenched)
      if (T > cfg_.quench_c) D[key] += cfg_.quench_c - T;
      continue;
    }
    const u8 burnt = g.layer(burn_, p);
    const bool burning = m.combustible && T >= ignition(m, key) && g.exposed(p);
    f64 Tsrc = T;
    f64 d01 = m.weaken_c > 0.0 && T > m.weaken_c ? (T - m.weaken_c) / std::max(1.0, m.gone_c - m.weaken_c) : 0.0;
    if (burning) {
      ++st_.burning;
      burning_keys.push_back(key);
      if (!burnt) ++st_.ignited;  // (burn is 1 at least from the first step)
      D[key] += m.flame_c - T;     // (it holds its flame)
      Tsrc = m.flame_c;
      const u32 b = std::max<u32>(1, burnt + dither(255.0 * dt / burn_time(m, key), key));
      if (b >= 255) {
        gone.push_back({p, kAir});
        ++st_.burnt_out;
      } else {
        burn_edits.push_back({p, static_cast<u8>(b)});
        d01 = std::max(d01, m.char_damage * b / 255.0);
      }
      flames_.push_back({V3{h * p[0], h * p[1], h * p[2]}, static_cast<f32>(m.flame_c), 0});
      // the flame heats the exposed solids it licks
      for (const Reach& r : kFlame) {
        const IVec3 q{p[0] + r.dx, p[1] + r.dy, p[2] + r.dz};
        if (!vox_solid(g.vox(q))) continue;
        const f64 Tq = temp(q);
        if (Tq >= m.flame_c || !g.exposed(q)) continue;
        D[vkey(q)] += cfg_.flame_reach * r.w * dt * (m.flame_c - Tq);
      }
    } else {
      D[key] -= std::min(T, cooling(m, T, dt));
    }
    // conduction to cooler solid neighbours
    for (const auto& f : kFace) {
      const IVec3 q{p[0] + f[0], p[1] + f[1], p[2] + f[2]};
      const Vox vq = g.vox(q);
      if (!vox_solid(vq)) continue;
      const f64 Tq = temp(q);
      if (Tq >= Tsrc) continue;
      const f64 k = 0.5 * std::min(m.conduct, mats_[size_t(vox_mat(vq)) & 0x7F].conduct) * dt * (Tsrc - Tq);
      D[vkey(q)] += k;
      if (!burning) D[key] -= k;
    }
    if (d01 > 0.0 && !(burning && gone.size() && gone.back().p == p)) damage(p, d01);
  }
  // pieces: their own fires, and heat across between them and the world
  std::vector<V3> heat_world;
  step_pieces(w, dt, heat_world);
  for (size_t i = 0; i + 1 < heat_world.size(); i += 2) {
    const IVec3 p = voxel_at(heat_world[i], h);
    if (!in_voxel_range(p) || !vox_solid(g.vox(p))) continue;
    const f64 Tflame = heat_world[i + 1].x, reach = heat_world[i + 1].y;
    const f64 Tq = temp(p);
    if (Tq < Tflame) D[vkey(p)] += reach * dt * (Tflame - Tq);
  }
  // new temperatures (in key order: the same on every platform)
  std::vector<u64> keys;
  keys.reserve(D.size());
  for (const auto& [k, d] : D) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  std::vector<u64> all;
  all.reserve(keys.size() + hot_.size());
  std::merge(keys.begin(), keys.end(), hot_.begin(), hot_.end(), std::back_inserter(all));
  all.erase(std::unique(all.begin(), all.end()), all.end());
  std::sort(gone.begin(), gone.end(), [](const VoxelEdit& a, const VoxelEdit& b) { return vkey(a.p) < vkey(b.p); });
  auto burnt_away = [&](u64 k) {
    const auto it = std::lower_bound(gone.begin(), gone.end(), k, [](const VoxelEdit& e, u64 v) { return vkey(e.p) < v; });
    return it != gone.end() && vkey(it->p) == k;
  };
  const u8 glow = glow_units();
  std::vector<LayerEdit> heat_edits;
  std::vector<std::pair<u8, u64>> next;
  next.reserve(all.size());
  for (u64 k : all) {
    const IVec3 p = unkey3(k);
    const auto it = D.find(k);
    const u8 old = g.layer(heat_, p);
    const f64 T = old * kUnit + (it == D.end() ? 0.0 : it->second);
    const bool solid = vox_solid(g.vox(p)) && !burnt_away(k);
    const u8 u = solid ? dither(std::max(0.0, T) / kUnit, k) : 0;
    if (u != old) {
      heat_edits.push_back({p, u});
      if ((old >= glow) != (u >= glow)) glow_changes_.push_back(ckey(p));
    }
    if (u) next.push_back({u, k});
  }
  // (budget: beyond max_hot, the coolest go - never a burning one before a cooler one; ties by
  // a hash of the voxel, not its place)
  if (static_cast<i64>(next.size()) > cfg_.max_hot) {
    auto rank = [&](const std::pair<u8, u64>& e) {
      const bool b = std::binary_search(burning_keys.begin(), burning_keys.end(), e.second);
      return std::make_tuple(b ? 1 : 0, e.first, mix(e.second));
    };
    const size_t keep = static_cast<size_t>(cfg_.max_hot);
    std::nth_element(next.begin(), next.begin() + static_cast<long>(keep), next.end(), [&](const auto& a, const auto& b) { return rank(a) > rank(b); });
    for (size_t i = keep; i < next.size(); ++i) heat_edits.push_back({unkey3(next[i].second), 0});
    st_.dropped += static_cast<i64>(next.size() - keep);
    next.resize(keep);
  }
  hot_.clear();
  for (const auto& [u, k] : next) hot_.push_back(k);
  std::sort(hot_.begin(), hot_.end());
  st_.hot = static_cast<i32>(hot_.size());
  std::sort(heat_edits.begin(), heat_edits.end(), [](const LayerEdit& a, const LayerEdit& b) { return vkey(a.p) < vkey(b.p); });
  w.set_layer(heat_, heat_edits);
  w.set_layer(burn_, burn_edits);
  w.set_layer(World::kDamageLayer, damage_edits);
  // burnt out: gone, chunk by chunk (an edit's reach - chunks generated, pieces woken - is its
  // chunk's; their heat, burn and damage go with them: layers bound to the voxel)
  std::sort(gone.begin(), gone.end(), [](const VoxelEdit& a, const VoxelEdit& b) { return ckey(a.p) < ckey(b.p) || (ckey(a.p) == ckey(b.p) && vkey(a.p) < vkey(b.p)); });
  for (size_t i = 0; i < gone.size();) {
    size_t j = i + 1;
    while (j < gone.size() && ckey(gone[j].p) == ckey(gone[i].p)) ++j;
    w.set_voxels(std::vector<VoxelEdit>(gone.begin() + static_cast<long>(i), gone.begin() + static_cast<long>(j)));
    i = j;
  }
  if (glow_changes_.size() > 65536) glow_changes_.clear();  // (nobody takes them)
}

void FireSystem::step_pieces(World& w, f64 dt, std::vector<V3>& heat_world) {
  // Fire on pieces works as in the world, in each piece's shape (its own neighbours; "up" is the
  // shape axis nearest the world's). Burning world voxels heat the pieces in their flames, and
  // burning pieces the world voxels in theirs (heat_world: pairs of point, (flame degC, reach)).
  // A piece's shape voxels keep the coordinates they had in the world: the same voxel has the
  // same jitter, in the world or on a piece.
  const f64 h = w.voxel_size();
  const u8 quantum = static_cast<u8>(cfg_.damage_quantum);
  const u8 glow = glow_units();
  st_.burning_pieces = 0;
  Look look{w.grid()};
  // pieces near the world's flames (by chunk; their boxes grown by a flame's reach)
  std::unordered_map<u64, std::vector<i64>> near;
  if (!flames_.empty())
    for (const PieceState& ps : w.pieces()) {
      const Body* b = w.piece(ps.id);
      const f64 pad = h * (kFlameReach + 1);
      const V3 lo_w = b->box_lo - V3{pad, pad, pad}, hi_w = b->box_hi + V3{pad, pad, pad};
      if (!w.in_range(lo_w) || !w.in_range(hi_w)) continue;
      const IVec3 lo = chunk_of(voxel_at(lo_w, h)), hi = chunk_of(voxel_at(hi_w, h));
      if (i64(hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1) > 512) continue;  // (a vast piece: its own heat only)
      for (i32 x = lo[0]; x <= hi[0]; ++x)
        for (i32 y = lo[1]; y <= hi[1]; ++y)
          for (i32 z = lo[2]; z <= hi[2]; ++z) near[key3(x, y, z)].push_back(ps.id);
    }
  std::unordered_map<i64, std::unordered_map<i32, f64>> pD;  // piece -> cell -> degC
  for (const Flame& f : flames_) {
    const IVec3 p = voxel_at(f.pos, h);
    const auto it = near.find(ckey(p));
    if (it == near.end()) continue;
    for (i64 id : it->second) {
      const Body* b = w.piece(id);
      if (norm(f.pos - b->x) > b->radius + h * (kFlameReach + 2)) continue;
      for (const Reach& r : kFlame) {
        const V3 X{h * (p[0] + r.dx), h * (p[1] + r.dy), h * (p[2] + r.dz)};
        const i32 i = b->shape.index(voxel_at(b->to_shape(X), h));
        if (i < 0 || !vox_solid(b->shape.vox[size_t(i)])) continue;
        const f64 Tq = b->shape.layer_at(heat_, i) * kUnit;
        if (Tq < f.heat) pD[id][i] += cfg_.flame_reach * r.w * dt * (f.heat - Tq);
      }
    }
  }
  // pieces with heat of their own
  std::vector<i64> ids;
  for (const PieceState& ps : w.pieces()) {
    const Body* b = w.piece(ps.id);
    if (!b->shape.layer[size_t(heat_)].empty() || pD.count(ps.id)) ids.push_back(ps.id);
  }
  for (i64 id : ids) {
    const Body* b = w.piece(id);
    const BodyShape& S = b->shape;
    auto& D = pD[id];
    std::vector<LayerEdit> burn_edits, damage_edits;
    std::vector<IVec3> gone;
    bool changed = false;  // (its charring or glow: its mesh)
    // "up" in the shape: the axis nearest the world's
    const V3 up = rotate_inv(b->q, V3{0, 0, 1});
    int ua = 0;
    for (int a = 1; a < 3; ++a)
      if (std::abs(up[a]) > std::abs(up[ua])) ua = a;
    const int us = up[ua] >= 0 ? 1 : -1;
    auto cell_solid = [&](const IVec3& q) { return vox_solid(S.get(q)); };
    auto exposed = [&](const IVec3& q) {
      for (const auto& f : kFace)
        if (!cell_solid({q[0] + f[0], q[1] + f[1], q[2] + f[2]})) return true;
      return false;
    };
    auto damage = [&](const IVec3& sp, i32 i, f64 d01) {
      const u8 d = static_cast<u8>(std::lround(254.0 * std::clamp(d01, 0.0, 1.0)));
      const u8 cur = S.layer_at(World::kDamageLayer, i);
      if (d > cur && (d - cur >= quantum || d == 254)) damage_edits.push_back({sp, d});
    };
    bool any_burning = false;
    const std::vector<u8>& heat = S.layer[size_t(heat_)];
    for (i32 i = 0; i < static_cast<i32>(heat.size()); ++i) {
      if (!heat[size_t(i)] || !vox_solid(S.vox[size_t(i)])) continue;
      const IVec3 sp = S.voxel(i);
      if (S.layer_at(burn_, i) == 255) {  // (ash: it goes with the piece's next batch)
        gone.push_back(sp);
        continue;
      }
      const FireMaterial& m = mats_[size_t(vox_mat(S.vox[size_t(i)])) & 0x7F];
      const f64 T = heat[size_t(i)] * kUnit;
      const V3 X = b->to_world(V3{h * sp[0], h * sp[1], h * sp[2]});
      if (w.in_range(X) && look.wet(water_, voxel_at(X, h))) {
        if (T > cfg_.quench_c) D[i] += cfg_.quench_c - T;
        continue;
      }
      const u64 key = vkey(sp);
      const bool burning = m.combustible && T >= ignition(m, key) && exposed(sp);
      f64 Tsrc = T;
      f64 d01 = m.weaken_c > 0.0 && T > m.weaken_c ? (T - m.weaken_c) / std::max(1.0, m.gone_c - m.weaken_c) : 0.0;
      if (burning) {
        any_burning = true;
        const u8 burnt = S.layer_at(burn_, i);
        if (!burnt) ++st_.ignited;
        D[i] += m.flame_c - T;
        Tsrc = m.flame_c;
        const u32 bt = std::max<u32>(1, burnt + dither(255.0 * dt / burn_time(m, key), key));
        if (bt >= 255) {
          burn_edits.push_back({sp, 255});
          ++st_.burnt_out;
        } else {
          burn_edits.push_back({sp, static_cast<u8>(bt)});
          d01 = std::max(d01, m.char_damage * bt / 255.0);
        }
        changed = true;
        flames_.push_back({X, static_cast<f32>(m.flame_c), id});
        // its flame: the shape's cells above it, and the world's voxels there
        for (const Reach& r : kFlame) {
          int o[3];
          o[ua] = us * r.dz;
          o[(ua + 1) % 3] = r.dx;
          o[(ua + 2) % 3] = r.dy;
          const IVec3 q{sp[0] + o[0], sp[1] + o[1], sp[2] + o[2]};
          const i32 j = S.index(q);
          if (j < 0 || !vox_solid(S.vox[size_t(j)])) continue;
          const f64 Tq = S.layer_at(heat_, j) * kUnit;
          if (Tq >= m.flame_c || !exposed(q)) continue;
          D[j] += cfg_.flame_reach * r.w * dt * (m.flame_c - Tq);
        }
        for (const Reach& r : kFlame) {
          heat_world.push_back(V3{X.x + h * r.dx, X.y + h * r.dy, X.z + h * r.dz});
          heat_world.push_back(V3{m.flame_c, cfg_.flame_reach * r.w, 0.0});
        }
      } else {
        D[i] -= std::min(T, cooling(m, T, dt));
      }
      for (const auto& f : kFace) {
        const IVec3 q{sp[0] + f[0], sp[1] + f[1], sp[2] + f[2]};
        const i32 j = S.index(q);
        if (j < 0 || !vox_solid(S.vox[size_t(j)])) continue;
        const f64 Tq = S.layer_at(heat_, j) * kUnit;
        if (Tq >= Tsrc) continue;
        const f64 k = 0.5 * std::min(m.conduct, mats_[size_t(vox_mat(S.vox[size_t(j)])) & 0x7F].conduct) * dt * (Tsrc - Tq);
        D[j] += k;
        if (!burning) D[i] -= k;
      }
      if (d01 > 0.0) damage(sp, i, d01);
    }
    if (any_burning) ++st_.burning_pieces;
    std::vector<std::pair<i32, f64>> cells(D.begin(), D.end());
    std::sort(cells.begin(), cells.end());
    std::vector<LayerEdit> heat_edits;
    for (const auto& [i, d] : cells) {
      const IVec3 sp = S.voxel(i);
      const u8 old = S.layer_at(heat_, i);
      const u8 u = dither(std::max(0.0, old * kUnit + d) / kUnit, vkey(sp) ^ 0x51ull);
      if (u == old) continue;
      heat_edits.push_back({sp, u});
      changed = changed || (old >= glow) != (u >= glow);
    }
    if (changed) piece_changes_.push_back(id);
    w.set_piece_layer(id, heat_, heat_edits);
    w.set_piece_layer(id, burn_, burn_edits);
    if (!damage_edits.empty()) w.set_piece_layer(id, World::kDamageLayer, damage_edits);
    // (burnt-out voxels leave a piece in batches: each removal announces its parts again, with
    // new meshes)
    if (!gone.empty() && (steps_ + id) % cfg_.piece_batch_steps == 0) w.remove_piece_voxels(id, gone, false);  // (last: the piece may split)
  }
  if (piece_changes_.size() > 65536) piece_changes_.clear();  // (nobody takes them)
}

void FireSystem::admit(std::vector<std::pair<f64, u64>>& cand) {
  // (nearest first, while the budget has room)
  std::sort(cand.begin(), cand.end());
  const size_t room = hot_.size() >= static_cast<size_t>(cfg_.max_hot) ? 0 : static_cast<size_t>(cfg_.max_hot) - hot_.size();
  if (cand.size() > room) {
    st_.dropped += static_cast<i64>(cand.size() - room);
    cand.resize(room);
  }
  for (const auto& [d, k] : cand) hot_.push_back(k);
  std::sort(hot_.begin(), hot_.end());
  hot_.erase(std::unique(hot_.begin(), hot_.end()), hot_.end());
}

void FireSystem::ignite(World& w, const V3& pos, f64 radius) {
  heat_sphere(w, pos, radius, 0.0, true);  // (combustibles to their flame, the rest to 300 degC)
}

void FireSystem::heat(World& w, const V3& pos, f64 radius, f64 celsius) {
  if (!std::isfinite(celsius)) return;
  heat_sphere(w, pos, radius, std::max(0.0, celsius), false);
}

void FireSystem::heat_sphere(World& w, const V3& pos, f64 radius, f64 celsius, bool own) {
  if (!cfg_.enabled || !ok() || !w.in_range(pos) || !(radius > 0.0) || !std::isfinite(radius)) return;
  const f64 h = w.voxel_size();
  const i32 R = std::min(kMaxReach, static_cast<i32>(std::ceil(std::min(radius, 1e6) / h)));
  radius = std::min(radius, h * R);
  const IVec3 c = voxel_at(pos, h);
  auto target = [&](Vox v) {
    if (!own) return units(celsius);
    const FireMaterial& m = mats_[size_t(vox_mat(v)) & 0x7F];
    return units(m.combustible ? m.flame_c : 300.0);
  };
  std::vector<LayerEdit> edits;
  std::vector<std::pair<f64, u64>> cand;
  for (i32 x = c[0] - R; x <= c[0] + R; ++x)
    for (i32 y = c[1] - R; y <= c[1] + R; ++y)
      for (i32 z = c[2] - R; z <= c[2] + R; ++z) {
        const f64 d = norm(V3{h * x, h * y, h * z} - pos);
        if (d > radius) continue;
        const Vox v = w.grid().get(x, y, z);
        if (!vox_solid(v)) continue;
        const u8 u = target(v);
        if (w.layer(heat_, {x, y, z}) < u) edits.push_back({{x, y, z}, u});
        cand.push_back({d, key3(x, y, z)});  // (tracked even if it was that hot already)
      }
  // (within the budget: the nearest; the others keep no heat)
  admit(cand);
  std::unordered_set<u64> in(cand.size());
  for (const auto& [d, k] : cand) in.insert(k);
  edits.erase(std::remove_if(edits.begin(), edits.end(), [&](const LayerEdit& e) { return !in.count(vkey(e.p)); }), edits.end());
  w.set_layer(heat_, edits);
  // pieces in the sphere
  for (const PieceState& ps : w.pieces()) {
    const Body* b = w.piece(ps.id);
    if (norm(ps.pos - pos) > radius + b->radius) continue;
    std::vector<LayerEdit> pe;
    for (i32 i = 0; i < static_cast<i32>(b->shape.vox.size()); ++i) {
      if (!vox_solid(b->shape.vox[size_t(i)])) continue;
      const IVec3 sp = b->shape.voxel(i);
      if (norm(b->to_world(V3{h * sp[0], h * sp[1], h * sp[2]}) - pos) > radius) continue;
      const u8 u = target(b->shape.vox[size_t(i)]);
      if (b->shape.layer_at(heat_, i) < u) pe.push_back({sp, u});
    }
    w.set_piece_layer(ps.id, heat_, pe);
  }
}

void FireSystem::extinguish(World& w, const V3& pos, f64 radius) {
  if (!cfg_.enabled || !ok() || !w.in_range(pos) || !(radius > 0.0) || !std::isfinite(radius)) return;
  const f64 h = w.voxel_size();
  radius = std::min(radius, h * kMaxReach);
  const u8 u = units(cfg_.quench_c);
  std::vector<LayerEdit> edits;
  for (u64 k : hot_) {
    const IVec3 p = unkey3(k);
    if (norm(V3{h * p[0], h * p[1], h * p[2]} - pos) > radius) continue;
    if (w.layer(heat_, p) > u) edits.push_back({p, u});
  }
  w.set_layer(heat_, edits);
  for (const PieceState& ps : w.pieces()) {
    const Body* b = w.piece(ps.id);
    if (b->shape.layer[size_t(heat_)].empty() || norm(ps.pos - pos) > radius + b->radius) continue;
    std::vector<LayerEdit> pe;
    for (i32 i = 0; i < static_cast<i32>(b->shape.vox.size()); ++i) {
      if (b->shape.layer_at(heat_, i) <= u) continue;
      const IVec3 sp = b->shape.voxel(i);
      if (norm(b->to_world(V3{h * sp[0], h * sp[1], h * sp[2]}) - pos) > radius) continue;  // (the sphere's cells only)
      pe.push_back({sp, u});
    }
    w.set_piece_layer(ps.id, heat_, pe);
  }
}

f64 FireSystem::temperature(const World& w, const IVec3& p) const { return ok() ? w.layer(heat_, p) * kUnit : 0.0; }

f64 FireSystem::burnt(const World& w, const IVec3& p) const { return ok() ? w.layer(burn_, p) / 255.0 : 0.0; }

std::vector<u64> FireSystem::take_glow_changes() {
  std::vector<u64> out;
  out.swap(glow_changes_);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<i64> FireSystem::take_piece_changes() {
  std::vector<i64> out;
  out.swap(piece_changes_);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

i64 FireSystem::memory_bytes() const {
  return static_cast<i64>(hot_.capacity() * sizeof(u64) + flames_.capacity() * sizeof(Flame) + glow_changes_.capacity() * sizeof(u64) +
                          piece_changes_.capacity() * sizeof(i64) + sizeof(*this));
}

u64 FireSystem::state_hash() const {
  u64 hsh = 1469598103934665603ull;
  auto add = [&](u64 v) { hsh = (hsh ^ v) * 1099511628211ull; };
  for (u64 k : hot_) add(k);
  add(static_cast<u64>(steps_));
  u64 cb;
  std::memcpy(&cb, &clock_, sizeof cb);
  add(cb);
  add(static_cast<u64>(st_.burnt_out));
  return hsh;
}

}  // namespace svx
