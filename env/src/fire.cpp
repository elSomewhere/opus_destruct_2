#include "svx/env/fire.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>

namespace svx {

namespace {

constexpr f64 kUnit = 4.0;  // degC per heat unit

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
};

}  // namespace

FireSystem::FireSystem(const FireConfig& c) : cfg_(c) {
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

void FireSystem::attach(World& w) {
  heat_ = w.add_layer({"heat", false});
  burn_ = w.add_layer({"burn", true});
}

void FireSystem::on_load(World& w) {
  (void)w;
  hot_.clear();
  flames_.clear();
  clock_ = 0.0;
}

void FireSystem::on_evicted(World& w, const std::vector<u64>& chunks) {
  (void)w;
  const std::unordered_set<u64> gone(chunks.begin(), chunks.end());
  hot_.erase(std::remove_if(hot_.begin(), hot_.end(), [&](u64 k) { return gone.count(ckey(unkey3(k))) > 0; }), hot_.end());
}

f64 FireSystem::ignition(const FireMaterial& m, u64 key) const {
  return m.ignition_c * (1.0 + cfg_.ignition_jitter * (2.0 * u01(mix(key ^ 0x1617ull)) - 1.0));
}

f64 FireSystem::burn_time(const FireMaterial& m, u64 key) const {
  return std::max(0.5, m.burn_s * (1.0 + cfg_.burn_jitter * (2.0 * u01(mix(key ^ 0x2718ull)) - 1.0)));
}

u8 FireSystem::dither(f64 v, u64 key) const {
  if (!(v > 0.0)) return 0;
  if (v >= 255.0) return 255;
  const f64 fl = std::floor(v);
  return static_cast<u8>(fl + (u01(mix(key ^ static_cast<u64>(steps_) * 0x9E37ull)) < v - fl ? 1.0 : 0.0));
}

bool FireSystem::wet(const World& w, const IVec3& p) const {
  if (water_ < 0) return false;
  if (w.layer(water_, p)) return true;
  for (const auto& f : kFace)
    if (w.layer(water_, {p[0] + f[0], p[1] + f[1], p[2] + f[2]})) return true;
  return false;
}

void FireSystem::step(World& w, f64 dt) {
  clock_ += dt;
  const f64 s = std::max(cfg_.step_s, w.config().dt);
  while (clock_ >= s - 1e-9) {
    clock_ -= s;
    fire_step(w, s);
  }
}

void FireSystem::fire_step(World& w, f64 dt) {
  const auto t0 = std::chrono::steady_clock::now();
  ++steps_;
  water_ = w.layer_index("water");
  flames_.clear();
  Look g{w.grid()};
  const f64 h = w.voxel_size();
  std::unordered_map<u64, f64> D;  // temperature changes (degC)
  std::vector<LayerEdit> burn_edits, damage_edits;
  std::vector<VoxelEdit> gone;
  st_.burning = 0;
  auto temp = [&](const IVec3& p) { return g.layer(heat_, p) * kUnit; };
  auto weaken = [&](const FireMaterial& m, const IVec3& p, f64 T) {
    if (m.weaken_c <= 0.0 || T <= m.weaken_c) return;
    const f64 t = std::clamp((T - m.weaken_c) / std::max(1.0, m.gone_c - m.weaken_c), 0.0, 1.0);
    const u8 d = static_cast<u8>(std::lround(254.0 * t));
    if (d > g.layer(World::kDamageLayer, p)) damage_edits.push_back({p, d});
  };
  for (u64 key : hot_) {
    const IVec3 p = unkey3(key);
    const Vox v = g.vox(p);
    if (!vox_solid(v)) continue;
    const FireMaterial& m = mats_[size_t(vox_mat(v))];
    const f64 T = temp(p);
    if (wet(w, p)) {  // (quenched)
      if (T > cfg_.quench_c) D[key] += cfg_.quench_c - T;
      continue;
    }
    const u8 burnt = g.layer(burn_, p);
    const bool burning = m.combustible && T >= ignition(m, key) && g.exposed(p);
    f64 Tsrc = T;
    if (burning) {
      ++st_.burning;
      if (!burnt) ++st_.ignited;
      D[key] += m.flame_c - T;  // (it holds its flame)
      Tsrc = m.flame_c;
      const u32 b = burnt + dither(255.0 * dt / burn_time(m, key), key);
      if (b >= 255) {
        gone.push_back({p, kAir});
        ++st_.burnt_out;
      } else {
        burn_edits.push_back({p, static_cast<u8>(b)});
        const u8 d = static_cast<u8>(std::lround(254.0 * m.char_damage * b / 255.0));
        if (d > g.layer(World::kDamageLayer, p)) damage_edits.push_back({p, d});
      }
      flames_.push_back({V3{h * p[0], h * p[1], h * p[2]}, static_cast<f32>(m.flame_c), 0});
      // the flame heats the solids it licks
      for (const Reach& r : kFlame) {
        const IVec3 q{p[0] + r.dx, p[1] + r.dy, p[2] + r.dz};
        const Vox vq = g.vox(q);
        if (!vox_solid(vq)) continue;
        if (!g.exposed(q)) continue;
        const f64 Tq = temp(q);
        if (Tq < m.flame_c) D[vkey(q)] += cfg_.flame_reach * r.w * dt * (m.flame_c - Tq);
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
      const f64 k = 0.5 * std::min(m.conduct, mats_[size_t(vox_mat(vq))].conduct) * dt * (Tsrc - Tq);
      D[vkey(q)] += k;
      if (!burning) D[key] -= k;
    }
    weaken(m, p, T);
  }
  // pieces: their own fires, and heat across between them and the world
  std::vector<V3> heat_world;
  step_pieces(w, dt, heat_world);
  for (size_t i = 0; i + 1 < heat_world.size(); i += 2) {
    const IVec3 p{static_cast<i32>(std::floor(heat_world[i].x / h + 0.5)), static_cast<i32>(std::floor(heat_world[i].y / h + 0.5)),
                  static_cast<i32>(std::floor(heat_world[i].z / h + 0.5))};
    if (!vox_solid(g.vox(p))) continue;
    const f64 Tflame = heat_world[i + 1].x, reach = heat_world[i + 1].y;
    const f64 Tq = temp(p);
    if (Tq < Tflame) D[vkey(p)] += reach * dt * (Tflame - Tq);
  }
  // new temperatures (in key order: the same on every platform)
  std::vector<u64> keys;
  keys.reserve(D.size() + hot_.size());
  for (const auto& [k, d] : D) keys.push_back(k);
  keys.insert(keys.end(), hot_.begin(), hot_.end());
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  std::vector<LayerEdit> heat_edits;
  std::vector<std::pair<u8, u64>> next;
  std::unordered_set<u64> burnt_away;
  for (const VoxelEdit& e : gone) burnt_away.insert(vkey(e.p));
  for (u64 k : keys) {
    const IVec3 p = unkey3(k);
    const auto it = D.find(k);
    const f64 T = temp(p) + (it == D.end() ? 0.0 : it->second);
    const bool solid = vox_solid(g.vox(p)) && !burnt_away.count(k);
    const u8 u = solid ? dither(std::max(0.0, T) / kUnit, k) : 0;
    if (u != g.layer(heat_, p)) heat_edits.push_back({p, u});
    if (u) next.push_back({u, k});
  }
  // (budget: beyond max_hot, the coolest are let go)
  if (static_cast<i64>(next.size()) > cfg_.max_hot) {
    std::stable_sort(next.begin(), next.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = static_cast<size_t>(cfg_.max_hot); i < next.size(); ++i) heat_edits.push_back({unkey3(next[i].second), 0});
    st_.dropped += static_cast<i64>(next.size()) - cfg_.max_hot;
    next.resize(static_cast<size_t>(cfg_.max_hot));
  }
  hot_.clear();
  for (const auto& [u, k] : next) hot_.push_back(k);
  std::sort(hot_.begin(), hot_.end());
  st_.hot = static_cast<i32>(hot_.size());
  w.set_layer(heat_, heat_edits);
  w.set_layer(burn_, burn_edits);
  w.set_layer(World::kDamageLayer, damage_edits);
  if (!gone.empty()) {
    std::vector<LayerEdit> clear;
    for (const VoxelEdit& e : gone) clear.push_back({e.p, 0});
    w.set_voxels(gone);
    w.set_layer(burn_, clear);
    w.set_layer(World::kDamageLayer, clear);
  }
  st_.step_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void FireSystem::step_pieces(World& w, f64 dt, std::vector<V3>& heat_world) {
  // Fire on pieces works as in the world, in each piece's shape (its own neighbours; "up" is the
  // shape axis nearest the world's). Burning world voxels heat the pieces in their flames, and
  // burning pieces the world voxels in theirs (heat_world: pairs of point, (flame degC, reach)).
  const f64 h = w.voxel_size();
  st_.burning_pieces = 0;
  // pieces near the world's flames (by chunk)
  std::unordered_map<u64, std::vector<i64>> near;
  if (!flames_.empty())
    for (const PieceState& ps : w.pieces()) {
      const Body* b = w.piece(ps.id);
      const IVec3 lo = chunk_of({static_cast<i32>(std::floor(b->box_lo.x / h)), static_cast<i32>(std::floor(b->box_lo.y / h)),
                                 static_cast<i32>(std::floor(b->box_lo.z / h))});
      const IVec3 hi = chunk_of({static_cast<i32>(std::ceil(b->box_hi.x / h)), static_cast<i32>(std::ceil(b->box_hi.y / h)),
                                 static_cast<i32>(std::ceil(b->box_hi.z / h))});
      if ((hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1) > 64) continue;  // (a huge piece: its own heat only)
      for (i32 x = lo[0]; x <= hi[0]; ++x)
        for (i32 y = lo[1]; y <= hi[1]; ++y)
          for (i32 z = lo[2]; z <= hi[2]; ++z) near[key3(x, y, z)].push_back(ps.id);
    }
  std::unordered_map<i64, std::vector<LayerEdit>> piece_heat;  // heat added to pieces from the world (as deltas)
  std::unordered_map<i64, std::unordered_map<i32, f64>> pD;    // piece -> cell -> degC
  for (const Flame& f : flames_) {
    const IVec3 p{static_cast<i32>(std::lround(f.pos.x / h)), static_cast<i32>(std::lround(f.pos.y / h)),
                  static_cast<i32>(std::lround(f.pos.z / h))};
    const auto it = near.find(ckey(p));
    if (it == near.end()) continue;
    for (i64 id : it->second) {
      const Body* b = w.piece(id);
      for (const Reach& r : kFlame) {
        const V3 X{h * (p[0] + r.dx), h * (p[1] + r.dy), h * (p[2] + r.dz)};
        const V3 sp = b->to_shape(X);
        const IVec3 sv{static_cast<i32>(std::floor(sp.x / h + 0.5)), static_cast<i32>(std::floor(sp.y / h + 0.5)),
                       static_cast<i32>(std::floor(sp.z / h + 0.5))};
        const i32 i = b->shape.index(sv);
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
    bool any_burning = false;
    const std::vector<u8>& heat = S.layer[size_t(heat_)];
    for (i32 i = 0; i < static_cast<i32>(heat.size()); ++i) {
      if (!heat[size_t(i)] || !vox_solid(S.vox[size_t(i)])) continue;
      const IVec3 sp = S.voxel(i);
      if (S.layer_at(burn_, i) == 255) {  // (ash: it goes with the piece's next batch)
        gone.push_back(sp);
        continue;
      }
      const FireMaterial& m = mats_[size_t(vox_mat(S.vox[size_t(i)]))];
      const f64 T = heat[size_t(i)] * kUnit;
      const V3 X = b->to_world(V3{h * sp[0], h * sp[1], h * sp[2]});
      const IVec3 wp{static_cast<i32>(std::floor(X.x / h + 0.5)), static_cast<i32>(std::floor(X.y / h + 0.5)),
                     static_cast<i32>(std::floor(X.z / h + 0.5))};
      if (wet(w, wp)) {
        if (T > cfg_.quench_c) D[i] += cfg_.quench_c - T;
        continue;
      }
      const u64 key = mix(static_cast<u64>(id) * 0x9E3779B97F4A7C15ull ^ static_cast<u64>(i));
      const bool burning = m.combustible && T >= ignition(m, key) && exposed(sp);
      f64 Tsrc = T;
      if (burning) {
        any_burning = true;
        D[i] += m.flame_c - T;
        Tsrc = m.flame_c;
        const u32 bt = S.layer_at(burn_, i) + dither(255.0 * dt / burn_time(m, key), key);
        if (bt >= 255) {
          burn_edits.push_back({sp, 255});
          ++st_.burnt_out;
        } else {
          burn_edits.push_back({sp, static_cast<u8>(bt)});
          const u8 d = static_cast<u8>(std::lround(254.0 * m.char_damage * bt / 255.0));
          if (d > S.layer_at(World::kDamageLayer, i)) damage_edits.push_back({sp, d});
        }
        flames_.push_back({X, static_cast<f32>(m.flame_c), id});
        // its flame: the shape's cells above it, and the world's voxels there
        for (const Reach& r : kFlame) {
          int o[3];
          o[ua] = us * r.dz;
          o[(ua + 1) % 3] = r.dx;
          o[(ua + 2) % 3] = r.dy;
          const IVec3 q{sp[0] + o[0], sp[1] + o[1], sp[2] + o[2]};
          const i32 j = S.index(q);
          if (j >= 0 && vox_solid(S.vox[size_t(j)])) {
            if (!exposed(q)) continue;
            const f64 Tq = S.layer_at(heat_, j) * kUnit;
            if (Tq < m.flame_c) D[j] += cfg_.flame_reach * r.w * dt * (m.flame_c - Tq);
          }
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
        const f64 k = 0.5 * std::min(m.conduct, mats_[size_t(vox_mat(S.vox[size_t(j)]))].conduct) * dt * (Tsrc - Tq);
        D[j] += k;
        if (!burning) D[i] -= k;
      }
      if (m.weaken_c > 0.0 && T > m.weaken_c) {
        const f64 t = std::clamp((T - m.weaken_c) / std::max(1.0, m.gone_c - m.weaken_c), 0.0, 1.0);
        const u8 d = static_cast<u8>(std::lround(254.0 * t));
        if (d > S.layer_at(World::kDamageLayer, i)) damage_edits.push_back({sp, d});
      }
    }
    if (any_burning) ++st_.burning_pieces;
    std::vector<std::pair<i32, f64>> cells(D.begin(), D.end());
    std::sort(cells.begin(), cells.end());
    std::vector<LayerEdit> heat_edits;
    for (const auto& [i, d] : cells) {
      const IVec3 sp = S.voxel(i);
      const f64 T = S.layer_at(heat_, i) * kUnit + d;
      const u64 key = mix(static_cast<u64>(id) * 0x51ull ^ static_cast<u64>(i));
      heat_edits.push_back({sp, dither(std::max(0.0, T) / kUnit, key)});
    }
    w.set_piece_layer(id, heat_, heat_edits);
    w.set_piece_layer(id, burn_, burn_edits);
    if (!damage_edits.empty()) w.set_piece_layer(id, World::kDamageLayer, damage_edits);
    // (burnt-out voxels leave a piece in batches, about once a second: each removal announces
    // its parts again, with new meshes)
    if (!gone.empty() && (steps_ + id) % 10 == 0) w.remove_piece_voxels(id, gone, false);  // (last: the piece may split)
  }
}

void FireSystem::ignite(World& w, const V3& pos, f64 radius) {
  const f64 h = w.voxel_size();
  if (!(radius > 0.0) || !std::isfinite(pos.x + pos.y + pos.z)) return;
  radius = std::min(radius, 8.0);
  const i32 R = static_cast<i32>(std::ceil(radius / h));
  const IVec3 c{static_cast<i32>(std::floor(pos.x / h + 0.5)), static_cast<i32>(std::floor(pos.y / h + 0.5)),
                static_cast<i32>(std::floor(pos.z / h + 0.5))};
  std::vector<LayerEdit> edits;
  for (i32 x = c[0] - R; x <= c[0] + R; ++x)
    for (i32 y = c[1] - R; y <= c[1] + R; ++y)
      for (i32 z = c[2] - R; z <= c[2] + R; ++z) {
        if (norm(V3{h * x, h * y, h * z} - pos) > radius) continue;
        const Vox v = w.grid().get(x, y, z);
        if (!vox_solid(v)) continue;
        const FireMaterial& m = mats_[size_t(vox_mat(v))];
        const u8 u = static_cast<u8>(std::min(255.0, (m.combustible ? m.flame_c : 300.0) / kUnit));
        if (w.layer(heat_, {x, y, z}) >= u) continue;  // (it only heats)
        edits.push_back({{x, y, z}, u});
        hot_.push_back(key3(x, y, z));
      }
  w.set_layer(heat_, edits);
  std::sort(hot_.begin(), hot_.end());
  hot_.erase(std::unique(hot_.begin(), hot_.end()), hot_.end());
  // pieces in the sphere
  for (const PieceState& ps : w.pieces()) {
    const Body* b = w.piece(ps.id);
    if (norm(ps.pos - pos) > radius + b->radius) continue;
    std::vector<LayerEdit> pe;
    for (i32 i = 0; i < static_cast<i32>(b->shape.vox.size()); ++i) {
      if (!vox_solid(b->shape.vox[size_t(i)])) continue;
      const IVec3 sp = b->shape.voxel(i);
      if (norm(b->to_world(V3{h * sp[0], h * sp[1], h * sp[2]}) - pos) > radius) continue;
      const FireMaterial& m = mats_[size_t(vox_mat(b->shape.vox[size_t(i)]))];
      const u8 u = static_cast<u8>(std::min(255.0, (m.combustible ? m.flame_c : 300.0) / kUnit));
      if (b->shape.layer_at(heat_, i) < u) pe.push_back({sp, u});
    }
    w.set_piece_layer(ps.id, heat_, pe);
  }
}

void FireSystem::heat(World& w, const V3& pos, f64 radius, f64 celsius) {
  const f64 h = w.voxel_size();
  if (!(radius > 0.0) || !std::isfinite(pos.x + pos.y + pos.z + celsius)) return;
  radius = std::min(radius, 8.0);
  const u8 u = static_cast<u8>(std::clamp(celsius / kUnit, 0.0, 255.0));
  const i32 R = static_cast<i32>(std::ceil(radius / h));
  const IVec3 c{static_cast<i32>(std::floor(pos.x / h + 0.5)), static_cast<i32>(std::floor(pos.y / h + 0.5)),
                static_cast<i32>(std::floor(pos.z / h + 0.5))};
  std::vector<LayerEdit> edits;
  for (i32 x = c[0] - R; x <= c[0] + R; ++x)
    for (i32 y = c[1] - R; y <= c[1] + R; ++y)
      for (i32 z = c[2] - R; z <= c[2] + R; ++z) {
        if (norm(V3{h * x, h * y, h * z} - pos) > radius) continue;
        if (!vox_solid(w.grid().get(x, y, z)) || w.layer(heat_, {x, y, z}) >= u) continue;
        edits.push_back({{x, y, z}, u});
        hot_.push_back(key3(x, y, z));
      }
  w.set_layer(heat_, edits);
  std::sort(hot_.begin(), hot_.end());
  hot_.erase(std::unique(hot_.begin(), hot_.end()), hot_.end());
}

void FireSystem::extinguish(World& w, const V3& pos, f64 radius) {
  const f64 h = w.voxel_size();
  if (!(radius > 0.0) || !std::isfinite(pos.x + pos.y + pos.z)) return;
  radius = std::min(radius, 8.0);
  const u8 u = static_cast<u8>(cfg_.quench_c / kUnit);
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
    for (i32 i = 0; i < static_cast<i32>(b->shape.vox.size()); ++i)
      if (b->shape.layer_at(heat_, i) > u) pe.push_back({b->shape.voxel(i), u});
    w.set_piece_layer(ps.id, heat_, pe);
  }
}

i64 FireSystem::memory_bytes() const {
  return static_cast<i64>(hot_.capacity() * sizeof(u64) + flames_.capacity() * sizeof(Flame) + sizeof(*this));
}

u64 FireSystem::state_hash() const {
  u64 hsh = 1469598103934665603ull;
  for (u64 k : hot_) hsh = (hsh ^ k) * 1099511628211ull;
  return hsh ^ static_cast<u64>(st_.burnt_out);
}

}  // namespace svx
