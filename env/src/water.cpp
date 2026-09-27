#include "svx/env/water.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <unordered_set>

namespace svx {

namespace {

constexpr f64 kG = 9.81;
constexpr int kFace[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
constexpr int kSide[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};

inline u64 vkey(const IVec3& p) { return key3(p[0], p[1], p[2]); }
inline u64 ckey(const IVec3& p) {
  const IVec3 c = chunk_of(p);
  return key3(c[0], c[1], c[2]);
}
inline IVec3 voxel_at(const V3& X, f64 h) {
  return {static_cast<i32>(std::floor(X.x / h + 0.5)), static_cast<i32>(std::floor(X.y / h + 0.5)), static_cast<i32>(std::floor(X.z / h + 0.5))};
}

// The water of a step: dense copies of the chunks it touches (the grid's layer, with this
// step's changes), their voxels and residency, the last chunk remembered.
struct Fluid {
  struct Buf {
    std::vector<u8> w, before;
    std::vector<u64> mark;  // (voxels already in the next active set)
    const Chunk* c = nullptr;
    bool resident = false, touched = false;
  };
  const World& w;
  int L;
  i32 lo_z;
  std::unordered_map<u64, Buf> bufs;
  u64 last_key = ~0ull;
  Buf* last = nullptr;
  std::array<std::pair<u64, Buf*>, 8> recent{};  // (the chunks used last: a scan crosses a few)
  u32 next_slot = 0;

  Buf& buf(const IVec3& p) {
    const u64 k = ckey(p);
    if (k == last_key) return *last;
    for (const auto& [rk, rb] : recent)
      if (rb && rk == k) {
        last_key = k;
        last = rb;
        return *rb;
      }
    auto it = bufs.find(k);
    if (it == bufs.end()) {
      it = bufs.emplace(k, Buf{}).first;
      Buf& b = it->second;
      const IVec3 cc = chunk_of(p);
      b.c = w.grid().chunk(cc);
      b.resident = w.chunk_resident(cc);
      if (b.c && !b.c->layer[size_t(L)].empty()) b.w = b.c->layer[size_t(L)];
      else b.w.assign(kChunkVox, 0);
    }
    last_key = k;
    last = &it->second;
    recent[next_slot++ & 7] = {k, last};
    return it->second;
  }
  u8 get(const IVec3& p) { return buf(p).w[size_t(chunk_index(p))]; }
  void set(const IVec3& p, u8 v) {
    Buf& b = buf(p);
    if (!b.touched) {
      b.touched = true;
      b.before = b.w;
    }
    b.w[size_t(chunk_index(p))] = v;
  }
  // Marks p (true the first time).
  bool mark(const IVec3& p) {
    Buf& b = buf(p);
    if (b.mark.empty()) b.mark.assign(kChunkVox / 64, 0);
    const i32 i = chunk_index(p);
    u64& m = b.mark[size_t(i >> 6)];
    const u64 bit = u64(1) << (i & 63);
    if (m & bit) return false;
    m |= bit;
    return true;
  }
  // 0: closed (solid, or not resident yet), 1: open, 2: below the world (water there is gone)
  int open(const IVec3& p) {
    if (p[2] < lo_z) return 2;
    const Buf& b = buf(p);
    if (!b.resident) return 0;
    if (!b.c) return 1;
    return vox_solid(b.c->uniform ? b.c->value : b.c->v[size_t(chunk_index(p))]) ? 0 : 1;
  }
};

}  // namespace

void WaterSystem::attach(World& w) { water_ = w.add_layer({"water", true}); }

void WaterSystem::on_load(World& w) {
  (void)w;
  active_.clear();
  wake_.clear();
  load_dirty_.clear();
  loads_.clear();
  loads_all_ = true;
  wet_.clear();
  splashes_.clear();
  clock_ = load_clock_ = 0.0;
  // (the level's water presses on its structures from the start: a bake designs for it)
  if (cfg_.loads) update_loads(w);
}

void WaterSystem::on_evicted(World& w, const std::vector<u64>& chunks) {
  const std::unordered_set<u64> gone(chunks.begin(), chunks.end());
  auto out = [&](u64 k) { return gone.count(ckey(unkey3(k))) > 0; };
  active_.erase(std::remove_if(active_.begin(), active_.end(), out), active_.end());
  wake_.erase(std::remove_if(wake_.begin(), wake_.end(), out), wake_.end());
  for (u64 k : chunks) loads_changed_ |= loads_.erase(k) > 0;  // (the loads are set again without them)
  (void)w;
}

void WaterSystem::wake_chunk(const World& w, u64 k) {
  const Chunk* c = w.grid().chunk(unkey3(k));
  if (!c || water_ < 0 || c->layer[size_t(water_)].empty()) return;
  const IVec3 cc = unkey3(k);
  const std::vector<u8>& a = c->layer[size_t(water_)];
  for (i32 i = 0; i < kChunkVox; ++i)
    if (a[size_t(i)]) {
      const IVec3 l{i / (kChunk * kChunk), (i / kChunk) % kChunk, i % kChunk};
      wake_.push_back(key3(cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]));
    }
}

void WaterSystem::on_voxels_changed(World& w, const std::vector<u64>& chunks) {
  // water near changed voxels moves again (a wall shot away), and the loads change
  std::vector<u64> near;
  for (u64 k : chunks) {
    const IVec3 c = unkey3(k);
    near.push_back(k);
    for (const auto& f : kFace) near.push_back(key3(c[0] + f[0], c[1] + f[1], c[2] + f[2]));
  }
  std::sort(near.begin(), near.end());
  near.erase(std::unique(near.begin(), near.end()), near.end());
  for (u64 k : near) wake_chunk(w, k);
  load_dirty_.insert(load_dirty_.end(), near.begin(), near.end());
}

u8 WaterSystem::amount(const World& w, const IVec3& p) const { return water_ < 0 ? 0 : w.layer(water_, p); }

void WaterSystem::pour(World& w, const V3& pos, f64 radius) {
  const f64 h = w.voxel_size();
  if (water_ < 0 || !(radius > 0.0) || !std::isfinite(pos.x + pos.y + pos.z)) return;
  radius = std::min(radius, 4.0);
  const i32 R = static_cast<i32>(std::ceil(radius / h));
  const IVec3 c = voxel_at(pos, h);
  std::vector<LayerEdit> edits;
  for (i32 x = c[0] - R; x <= c[0] + R; ++x)
    for (i32 y = c[1] - R; y <= c[1] + R; ++y)
      for (i32 z = c[2] - R; z <= c[2] + R; ++z) {
        if (norm(V3{h * x, h * y, h * z} - pos) > radius) continue;
        if (vox_solid(w.grid().get(x, y, z)) || !w.chunk_resident(chunk_of({x, y, z}))) continue;
        edits.push_back({{x, y, z}, 255});
        wake_.push_back(key3(x, y, z));
      }
  w.set_layer(water_, edits);
}

void WaterSystem::drain(World& w, const V3& pos, f64 radius) {
  const f64 h = w.voxel_size();
  if (water_ < 0 || !(radius > 0.0) || !std::isfinite(pos.x + pos.y + pos.z)) return;
  radius = std::min(radius, 8.0);
  const i32 R = static_cast<i32>(std::ceil(radius / h));
  const IVec3 c = voxel_at(pos, h);
  std::vector<LayerEdit> edits;
  for (i32 x = c[0] - R; x <= c[0] + R; ++x)
    for (i32 y = c[1] - R; y <= c[1] + R; ++y)
      for (i32 z = c[2] - R; z <= c[2] + R; ++z) {
        if (norm(V3{h * x, h * y, h * z} - pos) > radius || !w.layer(water_, {x, y, z})) continue;
        edits.push_back({{x, y, z}, 0});
        for (const auto& f : kFace)
          if (w.layer(water_, {x + f[0], y + f[1], z + f[2]})) wake_.push_back(key3(x + f[0], y + f[1], z + f[2]));
      }
  w.set_layer(water_, edits);
  for (const LayerEdit& e : edits) load_dirty_.push_back(ckey(e.p));
}

void WaterSystem::step(World& w, f64 dt) {
  if (water_ < 0) return;
  const auto t0 = std::chrono::steady_clock::now();
  clock_ += dt;
  const f64 s = std::max(cfg_.step_s, w.config().dt);
  while (clock_ >= s - 1e-9) {
    clock_ -= s;
    flow_step(w);
  }
  load_clock_ += dt;
  if (cfg_.loads && load_clock_ >= cfg_.load_s && (loads_all_ || loads_changed_ || !load_dirty_.empty())) {
    load_clock_ = 0.0;
    update_loads(w);
  }
  splashes_.clear();
  if (cfg_.buoyancy) float_pieces(w);
  st_.step_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void WaterSystem::flow_step(World& w) {
  ++st_.steps;
  // (the active set is unique; the woken may repeat it)
  if (!wake_.empty()) {
    active_.insert(active_.end(), wake_.begin(), wake_.end());
    wake_.clear();
    std::sort(active_.begin(), active_.end());
    active_.erase(std::unique(active_.begin(), active_.end()), active_.end());
  }
  // this step's share (a rotating window when there is too much)
  std::vector<u64> todo, later;
  const size_t cap = static_cast<size_t>(std::max(1, cfg_.max_active));
  if (active_.size() > cap) {
    const size_t start = static_cast<size_t>(st_.steps * static_cast<i64>(cap)) % active_.size();
    for (size_t q = 0; q < active_.size(); ++q) ((q + active_.size() - start) % active_.size() < cap ? todo : later).push_back(active_[q]);
  } else {
    todo.swap(active_);
  }
  active_.clear();
  // bottom up; sideways in alternating directions (no drift): sorted as keys z, x, y (x and y
  // mirrored on odd steps)
  const bool flip = st_.steps & 1;
  for (u64& k : todo) {
    const IVec3 p = unkey3(k);
    const i32 x = flip ? -p[0] : p[0], y = flip ? -p[1] : p[1];
    k = key3(p[2], x, y);
  }
  std::sort(todo.begin(), todo.end());
  for (u64& k : todo) {
    const IVec3 q = unkey3(k);
    k = key3(flip ? -q[1] : q[1], flip ? -q[2] : q[2], q[0]);
  }
  Fluid f{w, water_, w.grid().lo[2] - 2 * kChunk, {}};
  std::vector<IVec3> changed;
  const int rot = static_cast<int>(st_.steps & 3);
  for (u64 key : todo) {
    const IVec3 p = unkey3(key);
    const u8 a = f.get(p);
    if (!a) continue;
    const int op = f.open(p);
    if (op == 0 && !w.chunk_resident(chunk_of(p))) continue;  // (waits for its chunk)
    if (op != 1) {  // (a solid came where the water was, or it is below the world)
      f.set(p, 0);
      changed.push_back(p);
      continue;
    }
    if (a < cfg_.min_amount) {  // (dried up)
      f.set(p, 0);
      changed.push_back(p);
      continue;
    }
    // fall: as a packet through empty voxels, then into the water below as far as it takes
    IVec3 cur = p;
    u32 amt = a;
    bool out = false;
    for (int k = 0; k < cfg_.fall; ++k) {
      const IVec3 b{cur[0], cur[1], cur[2] - 1};
      const int o = f.open(b);
      if (o == 2) {
        out = true;
        break;
      }
      if (o == 0 || f.get(b) != 0) break;
      cur = b;
    }
    if (out) {  // (off the world)
      f.set(p, 0);
      changed.push_back(p);
      continue;
    }
    if (cur != p) {
      f.set(p, 0);
      changed.push_back(p);
    }
    const IVec3 below{cur[0], cur[1], cur[2] - 1};
    const int ob = f.open(below);
    if (ob == 1) {
      const u8 ab = f.get(below);
      if (ab < 255) {
        const u32 t = std::min<u32>(amt, 255u - ab);
        f.set(below, static_cast<u8>(ab + t));
        changed.push_back(below);
        amt -= t;
      }
    }
    if (cur != p || amt != a) {
      f.set(cur, static_cast<u8>(amt));
      changed.push_back(cur);
    }
    if (amt == 0) continue;
    // spread on a floor or on water: level with the lower open neighbours
    const bool supported = ob != 1 || f.get(below) == 255;
    if (!supported || amt < cfg_.min_spread) continue;
    IVec3 low[4];
    u8 an[4];
    int n = 0;
    for (int s = 0; s < 4; ++s) {
      const int* d = kSide[(s + rot) & 3];
      const IVec3 q{cur[0] + d[0], cur[1] + d[1], cur[2]};
      if (f.open(q) != 1) continue;
      const u8 v = f.get(q);
      if (v + 1u >= amt) continue;
      // (kept in ascending order: the lowest are levelled with first)
      int k = n++;
      for (; k > 0 && an[k - 1] > v; --k) {
        low[k] = low[k - 1];
        an[k] = an[k - 1];
      }
      low[k] = q;
      an[k] = v;
    }
    // the level of this voxel and its lowest neighbours, taking them while they are below it
    u32 sum = amt;
    int m = 0;
    while (m < n && an[m] * static_cast<u32>(m + 1) < sum) sum += an[m++];
    if (!m) continue;
    const u32 level = sum / static_cast<u32>(m + 1);
    u32 given = 0;
    for (int s = 0; s < m; ++s) {
      if (level <= an[s]) continue;
      f.set(low[s], static_cast<u8>(level));
      changed.push_back(low[s]);
      given += level - an[s];
    }
    if (given) {
      f.set(cur, static_cast<u8>(amt - given));
      changed.push_back(cur);
    }
  }
  // what moves next: the changed water and the water next to it (read before the buffers go)
  std::vector<u64>& next = active_;
  next = std::move(later);
  for (u64 k : next) f.mark(unkey3(k));  // (the ones waiting their turn)
  for (const IVec3& p : changed) {
    if (f.get(p) && f.mark(p)) next.push_back(vkey(p));
    for (const auto& d : kFace) {
      const IVec3 q{p[0] + d[0], p[1] + d[1], p[2] + d[2]};
      if (f.get(q) && f.mark(q)) next.push_back(vkey(q));
    }
  }
  // the step's water, into the layer (chunk by chunk, in key order)
  std::vector<u64> ks;
  for (const auto& [k, b] : f.bufs)
    if (b.touched) ks.push_back(k);
  std::sort(ks.begin(), ks.end());
  std::vector<LayerEdit> edits;
  for (u64 k : ks) {
    const Fluid::Buf& b = f.bufs[k];
    const IVec3 cc = unkey3(k);
    const size_t before = edits.size();
    for (i32 i = 0; i < kChunkVox; ++i)
      if (b.w[size_t(i)] != b.before[size_t(i)])
        edits.push_back({{cc[0] * kChunk + i / (kChunk * kChunk), cc[1] * kChunk + (i / kChunk) % kChunk, cc[2] * kChunk + i % kChunk}, b.w[size_t(i)]});
    if (cfg_.loads && edits.size() > before) load_dirty_.push_back(k);
  }
  st_.moved = w.set_layer(water_, edits);
  st_.active = static_cast<i32>(next.size());
}

void WaterSystem::chunk_loads(const World& w, const IVec3& cc, std::vector<VoxelLoad>& out) const {
  // The pressure of the chunk's water on the free solids next to it (sideways and below).
  const Chunk* c = w.grid().chunk(cc);
  if (!c || c->layer[size_t(water_)].empty()) return;
  bool free_near = c->free_count() > 0;
  for (const auto& d : kFace) {
    if (free_near) break;
    const Chunk* n = w.grid().chunk({cc[0] + d[0], cc[1] + d[1], cc[2] + d[2]});
    free_near = n && n->free_count() > 0;
  }
  if (!free_near) return;
  const f64 h = w.voxel_size(), rg = cfg_.density * kG;
  const std::vector<u8>& a = c->layer[size_t(water_)];
  for (i32 i = 0; i < kChunkVox; ++i) {
    if (!a[size_t(i)]) continue;
    const IVec3 p{cc[0] * kChunk + i / (kChunk * kChunk), cc[1] * kChunk + (i / kChunk) % kChunk, cc[2] * kChunk + i % kChunk};
    // (the water above it: its depth)
    f64 above = 0.0;
    for (i32 z = p[2] + 1, k = 0; k < 256; ++z, ++k) {
      const u8 v = w.layer(water_, {p[0], p[1], z});
      if (!v) break;
      above += v / 255.0;
    }
    const f64 fill = a[size_t(i)] / 255.0;
    for (int s = 0; s < 5; ++s) {
      const int* d = s < 4 ? kFace[s] : kFace[5];
      const IVec3 q{p[0] + d[0], p[1] + d[1], p[2] + d[2]};
      if (!vox_free(w.grid().get(q))) continue;
      // sideways: the mean pressure over the wet part of the face; below: the column's weight
      const f64 depth = s < 4 ? h * (above + 0.5 * fill) : h * (above + fill);
      const f64 area = s < 4 ? h * h * fill : h * h;
      const f64 F = rg * depth * area;
      out.push_back({q, V3{d[0] * F, d[1] * F, d[2] * F}});
    }
  }
}

void WaterSystem::update_loads(World& w) {
  std::vector<u64> dirty;
  if (loads_all_) {
    loads_.clear();
    for (const auto& [k, c] : w.grid().chunks())
      if (!c.layer[size_t(water_)].empty()) dirty.push_back(k);
    loads_all_ = false;
  } else {
    dirty.swap(load_dirty_);
  }
  load_dirty_.clear();
  std::sort(dirty.begin(), dirty.end());
  dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
  bool any = loads_changed_;
  loads_changed_ = false;
  for (u64 k : dirty) {
    std::vector<VoxelLoad> l;
    chunk_loads(w, unkey3(k), l);
    const auto it = loads_.find(k);
    if (l.empty()) {
      if (it != loads_.end()) {
        loads_.erase(it);
        any = true;
      }
      continue;
    }
    if (it != loads_.end() && it->second.size() == l.size()) {
      bool same = true;
      for (size_t q = 0; q < l.size() && same; ++q)
        same = l[q].voxel == it->second[q].voxel && std::abs(l[q].force.x - it->second[q].force.x) + std::abs(l[q].force.y - it->second[q].force.y) +
                                                            std::abs(l[q].force.z - it->second[q].force.z) <
                                                        1.0;
      if (same) continue;
    }
    loads_[k] = std::move(l);
    any = true;
  }
  if (!any) return;
  std::vector<u64> keys;
  for (const auto& [k, l] : loads_) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  std::vector<VoxelLoad> all;
  for (u64 k : keys) all.insert(all.end(), loads_[k].begin(), loads_[k].end());
  if (static_cast<i32>(all.size()) > cfg_.max_loads) {
    std::stable_sort(all.begin(), all.end(), [](const VoxelLoad& a, const VoxelLoad& b) { return norm2(a.force) > norm2(b.force); });
    all.resize(static_cast<size_t>(std::max(0, cfg_.max_loads)));
  }
  st_.loads = static_cast<i32>(all.size());
  w.set_loads(cfg_.load_group, std::move(all));
}

void WaterSystem::float_pieces(World& w) {
  // Buoyancy and drag on the pieces in the water: their voxels (a sample) that are in water
  // voxels are pushed up by the water they displace and slowed.
  const f64 h = w.voxel_size(), vol = h * h * h, rho = cfg_.density;
  st_.floating = 0;
  std::unordered_map<i64, u8> wet;
  for (const PieceState& ps : w.pieces()) {
    const Body* b = w.piece(ps.id);
    if (!b || b->shape.count <= 0) continue;
    // (no water near: nothing to do)
    const IVec3 lo = chunk_of(voxel_at(b->box_lo, h)), hi = chunk_of(voxel_at(b->box_hi, h));
    if ((hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1) > 27) continue;
    bool water_near = false;
    for (i32 x = lo[0]; x <= hi[0] && !water_near; ++x)
      for (i32 y = lo[1]; y <= hi[1] && !water_near; ++y)
        for (i32 z = lo[2]; z <= hi[2] && !water_near; ++z) {
          const Chunk* c = w.grid().chunk({x, y, z});
          water_near = c && !c->layer[size_t(water_)].empty();
        }
    if (!water_near) continue;
    const BodyShape& S = b->shape;
    const i32 stride = std::max<i32>(1, (S.count + cfg_.samples - 1) / std::max(1, cfg_.samples));
    f64 lift = 0.0;
    i32 seen = 0, in = 0;
    std::vector<std::pair<V3, f64>> pts;
    for (i32 i = 0; i < static_cast<i32>(S.vox.size()); ++i) {
      if (!vox_solid(S.vox[size_t(i)])) continue;
      if (seen++ % stride) continue;
      const IVec3 sp = S.voxel(i);
      const V3 X = b->to_world(V3{h * sp[0], h * sp[1], h * sp[2]});
      const u8 a = w.layer(water_, voxel_at(X, h));
      if (!a) continue;
      ++in;
      const f64 m = rho * vol * stride * (a / 255.0);  // (the water it displaces)
      lift += m * kG;
      pts.push_back({X, m});
    }
    if (!in) continue;
    wet[ps.id] = 1;
    ++st_.floating;
    // it would float: a sleeping piece wakes
    if (ps.asleep) {
      if (lift > 1.02 * ps.mass * kG) w.wake_piece(ps.id);
      else continue;
    }
    for (const auto& [X, m] : pts) {
      const V3 v = b->v + cross(b->w, X - b->x);
      w.apply_force(ps.id, X, V3{0.0, 0.0, m * kG} - v * (cfg_.drag * m));
    }
    // a hard landing in the water: a splash
    if (!wet_.count(ps.id) && ps.vel.z < -2.0) splashes_.push_back({ps.pos, static_cast<f32>(ps.mass * -ps.vel.z)});
  }
  wet_.swap(wet);
}

i64 WaterSystem::memory_bytes() const {
  i64 b = static_cast<i64>((active_.capacity() + wake_.capacity()) * sizeof(u64) + load_dirty_.capacity() * sizeof(u64) +
                           splashes_.capacity() * sizeof(Splash) + wet_.size() * 32 + sizeof(*this));
  for (const auto& [k, l] : loads_) b += static_cast<i64>(l.capacity() * sizeof(VoxelLoad) + 64);
  return b;
}

u64 WaterSystem::state_hash() const {
  u64 hsh = 1469598103934665603ull;
  for (u64 k : active_) hsh = (hsh ^ k) * 1099511628211ull;
  return hsh ^ static_cast<u64>(st_.steps);
}

}  // namespace svx
