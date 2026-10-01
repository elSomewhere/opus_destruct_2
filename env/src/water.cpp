#include "svx/env/water.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
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

// The water of a step: the grid's layer of the chunks it touches, a copy of those it changes
// (made at the first change), their voxels and residency, the last chunks remembered.
struct Fluid {
  struct Buf {
    const u8* src = nullptr;  // (the grid's values: unchanged until the step's end)
    std::vector<u8> w;        // (this step's, once changed)
    std::vector<u64> mark;    // (voxels already in the next active set)
    const Chunk* c = nullptr;
    bool resident = false;
    u8 at(i32 i) const { return !w.empty() ? w[size_t(i)] : src ? src[size_t(i)] : 0; }
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
      if (b.c && !b.c->layer[size_t(L)].empty()) b.src = b.c->layer[size_t(L)].data();
    }
    last_key = k;
    last = &it->second;
    recent[next_slot++ & 7] = {k, last};
    return it->second;
  }
  u8 get(const IVec3& p) { return buf(p).at(chunk_index(p)); }
  void set(const IVec3& p, u8 v) {
    Buf& b = buf(p);
    if (b.w.empty()) {
      if (b.src) b.w.assign(b.src, b.src + kChunkVox);
      else b.w.assign(kChunkVox, 0);
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
  // 0: closed (solid - the world grid's, or an oriented grid's there -, or not resident yet),
  // 1: open, 2: below the world (water there is gone)
  int open(const IVec3& p) {
    if (p[2] < lo_z) return 2;
    const Buf& b = buf(p);
    if (!b.resident) return 0;
    if (b.c && vox_solid(b.c->uniform ? b.c->value : b.c->v[size_t(chunk_index(p))])) return 0;
    return w.grid_solid(p) ? 0 : 1;
  }
};

inline f64 clampf(f64 v, f64 lo, f64 hi, f64 fallback) { return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; }

WaterConfig sanitized(WaterConfig c) {
  c.step_s = clampf(c.step_s, 0.0, 0.1, 1.0 / 30.0);
  c.fall = std::clamp(c.fall, 1, 16);
  c.min_amount = std::clamp<u8>(c.min_amount, 1, 64);
  c.min_spread = std::clamp<u8>(c.min_spread, c.min_amount, 128);
  c.max_active = std::clamp(c.max_active, 1, 4000000);
  c.density = clampf(c.density, 1.0, 20000.0, 1000.0);
  c.load_s = clampf(c.load_s, 0.0, 10.0, 0.5);
  c.max_loads = std::clamp(c.max_loads, 0, 1 << 22);
  c.drag = clampf(c.drag, 0.0, 20.0, 1.5);
  c.samples = std::clamp(c.samples, 8, 4096);
  return c;
}

// A full voxel of water held on every side it could go to - below and around: solids, full
// water, chunks not generated - cannot move (its step would change nothing).
bool held(const World& w, int L, const Chunk& c, const IVec3& cc, i32 i) {
  const IVec3 l{i / (kChunk * kChunk), (i / kChunk) % kChunk, i % kChunk};
  constexpr int kHold[5][3] = {{0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
  const std::vector<u8>& a = c.layer[size_t(L)];
  for (const auto& d : kHold) {
    const IVec3 q{l[0] + d[0], l[1] + d[1], l[2] + d[2]};
    const IVec3 p{cc[0] * kChunk + q[0], cc[1] * kChunk + q[1], cc[2] * kChunk + q[2]};
    if (q[0] >= 0 && q[0] < kChunk && q[1] >= 0 && q[1] < kChunk && q[2] >= 0 && q[2] < kChunk) {
      const i32 j = (q[0] * kChunk + q[1]) * kChunk + q[2];
      if (!(vox_solid(c.uniform ? c.value : c.v[size_t(j)]) || a[size_t(j)] == 255 || w.grid_solid(p))) return false;
      continue;
    }
    if (d[2] < 0 && p[2] < w.grid().lo[2] - 2 * kChunk) return false;  // (below the world: it goes)
    if (!w.chunk_resident(chunk_of(p))) continue;
    if (!(vox_solid(w.grid().get(p)) || w.layer(L, p) == 255 || w.grid_solid(p))) return false;
  }
  return true;
}

}  // namespace

WaterSystem::WaterSystem(const WaterConfig& c) : cfg_(sanitized(c)) {}

void WaterSystem::configure(const WaterConfig& c) {
  const bool loads_were = cfg_.loads;
  cfg_ = sanitized(c);
  clock_ = std::min(clock_, cfg_.step_s);
  if (cfg_.loads && !loads_were) loads_all_ = true;  // (on again: all of them anew)
  // (loads off, or another group: the world's are taken back at the next step)
}

void WaterSystem::attach(World& w) { water_ = w.add_layer({"water", true, LayerBind::Air}); }

void WaterSystem::on_load(World& w) {
  active_.clear();
  wake_.clear();
  load_dirty_.clear();
  loads_.clear();
  loads_all_ = true;
  loads_set_ = false;  // (the world dropped every load group with its old level)
  wet_.clear();
  samples_.clear();
  splashes_.clear();
  clock_ = load_clock_ = 0.0;
  st_ = Stats{};  // (a session replayed from a load: the same sweeps)
  // (the level's water presses on its structures from the start: a bake designs for it)
  if (ok() && cfg_.loads) update_loads(w);
}

void WaterSystem::loads_dirty(const World& w, u64 k) {
  // (a chunk's water, and the columns below it: their depth counts its water)
  if (!cfg_.loads) return;
  load_dirty_.push_back(k);
  IVec3 c = unkey3(k);
  for (int n = 0; n < 8; ++n) {
    --c[2];
    const Chunk* ch = w.grid().chunk(c);
    if (!ch || ch->layer[size_t(water_)].empty()) break;
    load_dirty_.push_back(key3(c[0], c[1], c[2]));
  }
}

void WaterSystem::on_generated(World& w, const std::vector<u64>& chunks) {
  // water resting against chunks not generated yet moves on once they are (and a chunk made
  // with water presses on what holds it)
  if (!ok()) return;
  std::vector<u64> near;
  for (u64 k : chunks) {
    const IVec3 c = unkey3(k);
    near.push_back(k);
    for (const auto& f : kFace) near.push_back(key3(c[0] + f[0], c[1] + f[1], c[2] + f[2]));
  }
  std::sort(near.begin(), near.end());
  near.erase(std::unique(near.begin(), near.end()), near.end());
  for (u64 k : near) {
    wake_chunk(w, k);
    const Chunk* c = w.grid().chunk(unkey3(k));
    if (c && !c->layer[size_t(water_)].empty()) loads_dirty(w, k);
  }
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
    if (a[size_t(i)] && !(a[size_t(i)] == 255 && held(w, water_, *c, cc, i))) {
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
  if (!ok()) return;
  for (u64 k : near) {
    wake_chunk(w, k);
    loads_dirty(w, k);
  }
}

u8 WaterSystem::amount(const World& w, const IVec3& p) const { return water_ < 0 ? 0 : w.layer(water_, p); }

void WaterSystem::pour(World& w, const V3& pos, f64 radius) {
  const f64 h = w.voxel_size();
  if (!cfg_.enabled || !ok() || !w.in_range(pos) || !(radius > 0.0) || !std::isfinite(radius)) return;
  const i32 R = std::min(kMaxReach, static_cast<i32>(std::ceil(std::min(radius, 1e6) / h)));
  radius = std::min(radius, h * R);
  const IVec3 c = voxel_at(pos, h);
  std::vector<LayerEdit> edits;
  for (i32 x = c[0] - R; x <= c[0] + R; ++x)
    for (i32 y = c[1] - R; y <= c[1] + R; ++y)
      for (i32 z = c[2] - R; z <= c[2] + R; ++z) {
        if (norm(V3{h * x, h * y, h * z} - pos) > radius) continue;
        if (vox_solid(w.grid().get(x, y, z)) || !w.chunk_resident(chunk_of({x, y, z})) || w.grid_solid({x, y, z})) continue;
        edits.push_back({{x, y, z}, 255});
        wake_.push_back(key3(x, y, z));
      }
  w.set_layer(water_, edits);
  // (water poured into a closed cavity may never move: its loads now)
  u64 last = ~0ull;
  for (const LayerEdit& e : edits)
    if (ckey(e.p) != last) loads_dirty(w, last = ckey(e.p));
}

void WaterSystem::drain(World& w, const V3& pos, f64 radius) {
  const f64 h = w.voxel_size();
  if (!ok() || !w.in_range(pos) || !(radius > 0.0) || !std::isfinite(radius)) return;
  const i32 R = std::min(kMaxReach, static_cast<i32>(std::ceil(std::min(radius, 1e6) / h)));
  radius = std::min(radius, h * R);
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
  u64 last = ~0ull;
  for (const LayerEdit& e : edits)
    if (ckey(e.p) != last) loads_dirty(w, last = ckey(e.p));
}

void WaterSystem::step(World& w, f64 dt) {
  const auto t0 = std::chrono::steady_clock::now();
  splashes_.clear();
  if (ok()) {
    if (cfg_.enabled) {
      const f64 s = std::max(cfg_.step_s, w.config().dt);
      clock_ = std::min(clock_ + dt, 4.0 * s);  // (a long pause does not come back as a burst)
      while (clock_ >= s - 1e-9) {
        clock_ -= s;
        flow_step(w);
      }
    }
    // loads: taken back when switched off or moved to another group
    if (loads_set_ && (!cfg_.loads || loads_group_ != cfg_.load_group)) {
      w.set_loads(loads_group_, {});
      loads_set_ = false;
      loads_.clear();
      load_dirty_.clear();
      loads_all_ = true;
      st_.loads = 0;
    }
    load_clock_ = std::min(load_clock_ + dt, cfg_.load_s);
    if (cfg_.loads && load_clock_ >= cfg_.load_s && (loads_all_ || loads_changed_ || !load_dirty_.empty())) {
      load_clock_ = 0.0;
      update_loads(w);
    }
    if (cfg_.buoyancy) float_pieces(w);
  }
  st_.step_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
  st_.total_ms += st_.step_ms;
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
    if (!b.w.empty()) ks.push_back(k);
  std::sort(ks.begin(), ks.end());
  std::vector<LayerEdit> edits;
  for (u64 k : ks) {
    const Fluid::Buf& b = f.bufs[k];
    const IVec3 cc = unkey3(k);
    const size_t before = edits.size();
    for (i32 i = 0; i < kChunkVox; ++i)
      if (b.w[size_t(i)] != (b.src ? b.src[size_t(i)] : 0))
        edits.push_back({{cc[0] * kChunk + i / (kChunk * kChunk), cc[1] * kChunk + (i / kChunk) % kChunk, cc[2] * kChunk + i % kChunk}, b.w[size_t(i)]});
    if (edits.size() > before) loads_dirty(w, k);
  }
  st_.moved = w.set_layer(water_, edits);
  st_.active = static_cast<i32>(next.size());
}

void WaterSystem::chunk_loads(const World& w, const IVec3& cc, std::vector<VoxelLoad>& out) const {
  // The pressure of the chunk's water on the free solids next to it (sideways and below).
  const Chunk* c = w.grid().chunk(cc);
  if (!c || c->layer[size_t(water_)].empty()) return;
  // (free solids it may press on: the world grid's, or an oriented grid's there)
  bool free_near = c->free_count() > 0 || w.grid_solids(cc);
  for (const auto& d : kFace) {
    if (free_near) break;
    const IVec3 nc{cc[0] + d[0], cc[1] + d[1], cc[2] + d[2]};
    const Chunk* n = w.grid().chunk(nc);
    free_near = (n && n->free_count() > 0) || w.grid_solids(nc);
  }
  if (!free_near) return;
  const f64 h = w.voxel_size(), rg = cfg_.density * kG;
  const std::vector<u8>& a = c->layer[size_t(water_)];
  for (i32 i = 0; i < kChunkVox; ++i) {
    if (!a[size_t(i)]) continue;
    const IVec3 p{cc[0] * kChunk + i / (kChunk * kChunk), cc[1] * kChunk + (i / kChunk) % kChunk, cc[2] * kChunk + i % kChunk};
    // (free solids beside or below it: those it presses on - an oriented grid's where the world
    // grid's voxel there is air)
    bool any = false;
    bool free_at[5];
    GridId gat[5] = {kWorldGrid, kWorldGrid, kWorldGrid, kWorldGrid, kWorldGrid};
    IVec3 gv[5];
    for (int s = 0; s < 5; ++s) {
      const int* d = s < 4 ? kFace[s] : kFace[5];
      const IVec3 q{p[0] + d[0], p[1] + d[1], p[2] + d[2]};
      const Vox vq = w.grid().get(q);
      free_at[s] = vox_free(vq);
      if (!vox_solid(vq) && w.grid_solid(q) && w.grid_voxel_at(V3{h * q[0], h * q[1], h * q[2]}, &gat[s], &gv[s]))
        free_at[s] = vox_free(w.grid(gat[s])->get(gv[s]));
      any = any || free_at[s];
    }
    if (!any) continue;
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
      if (!free_at[s]) continue;
      // sideways: the mean pressure over the wet part of the face; below: the column's weight
      const f64 depth = s < 4 ? h * (above + 0.5 * fill) : h * (above + fill);
      const f64 area = s < 4 ? h * h * fill : h * h;
      const f64 F = rg * depth * area;
      if (gat[s] != kWorldGrid) out.push_back({gv[s], V3{d[0] * F, d[1] * F, d[2] * F}, gat[s]});
      else out.push_back({q, V3{d[0] * F, d[1] * F, d[2] * F}});
    }
  }
}

void WaterSystem::update_loads(World& w) {
  if (!ok()) return;
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
  loads_group_ = cfg_.load_group;
  loads_set_ = true;
}

void WaterSystem::float_pieces(World& w) {
  // Buoyancy and drag on the pieces in the water: their voxels (an even sample, the same while
  // the piece keeps its shape) that are in water voxels are pushed up by the water they
  // displace and slowed.
  const f64 h = w.voxel_size(), vol = h * h * h, rho = cfg_.density, dt = w.config().dt;
  st_.floating = 0;
  std::unordered_map<i64, u8> wet;
  std::unordered_map<i64, Samples> kept;
  for (const PieceState& ps : w.pieces()) {
    const Body* b = w.piece(ps.id);
    if (!b || b->count <= 0) continue;
    // (no water near: nothing to do; a vast piece is looked at voxel by voxel)
    const V3 r{b->radius, b->radius, b->radius};
    if (!w.in_range(b->x - r) || !w.in_range(b->x + r)) continue;
    const IVec3 lo = chunk_of(voxel_at(b->x - r, h)), hi = chunk_of(voxel_at(b->x + r, h));
    bool water_near = i64(hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1) > 512;
    for (i32 x = lo[0]; x <= hi[0] && !water_near; ++x)
      for (i32 y = lo[1]; y <= hi[1] && !water_near; ++y)
        for (i32 z = lo[2]; z <= hi[2] && !water_near; ++z) {
          const Chunk* c = w.grid().chunk({x, y, z});
          water_near = c && !c->layer[size_t(water_)].empty();
        }
    if (!water_near) {
      // (the water it floated in is gone: it falls)
      if (ps.asleep && wet_.count(ps.id)) w.wake_piece(ps.id);
      continue;
    }
    // its cells, all shapes one after the other (a piece of one shape: its shape's cells)
    std::vector<size_t> off(b->shapes.size() + 1, 0);
    for (size_t k = 0; k < b->shapes.size(); ++k) off[k + 1] = off[k] + b->shapes[k].vox.size();
    auto solid_at = [&](size_t gi, size_t* k) {
      while (gi >= off[*k + 1]) ++*k;
      return vox_solid(b->shapes[*k].vox[gi - off[*k]]);
    };
    const i32 count = b->count;
    // its sample: cells picked by a hash of their index (no stripes of a shape's layout)
    Samples& smp = kept[ps.id];
    if (const auto it = samples_.find(ps.id); it != samples_.end() && it->second.count == count) smp = std::move(it->second);
    if (smp.count != count) {
      smp.count = count;
      smp.cells.clear();
      const u64 stride = static_cast<u64>(std::max<i32>(1, (count + cfg_.samples - 1) / cfg_.samples));
      size_t k = 0;
      for (i32 i = 0; i < static_cast<i32>(off.back()); ++i) {
        if (!solid_at(size_t(i), &k)) continue;
        u64 x = static_cast<u64>(i) * 0x9E3779B97F4A7C15ull;
        x ^= x >> 29;
        if (x % stride == 0) smp.cells.push_back(i);
      }
      k = 0;
      if (smp.cells.empty())
        for (i32 i = 0; i < static_cast<i32>(off.back()) && smp.cells.empty(); ++i)
          if (solid_at(size_t(i), &k)) smp.cells.push_back(i);
    }
    const f64 each = vol * static_cast<f64>(count) / static_cast<f64>(smp.cells.size());  // (the volume a sample stands for)
    f64 lift = 0.0, msub = 0.0;
    std::vector<std::pair<V3, f64>> pts;
    for (i32 gi : smp.cells) {
      size_t k = 0;
      while (size_t(gi) >= off[k + 1]) ++k;
      const IVec3 sp = b->shapes[k].voxel(static_cast<i32>(size_t(gi) - off[k]));
      const V3 X = b->lattice_to_world(k, V3{h * sp[0], h * sp[1], h * sp[2]});
      const u8 a = w.layer(water_, voxel_at(X, h));
      if (!a) continue;
      const f64 m = rho * each * (a / 255.0);  // (the water it displaces)
      lift += m * kG;
      msub += m;
      pts.push_back({X, m});
    }
    if (pts.empty()) {
      if (ps.asleep && wet_.count(ps.id)) w.wake_piece(ps.id);  // (it floated here: it falls)
      continue;
    }
    wet[ps.id] = 1;
    ++st_.floating;
    // it would float: a sleeping piece wakes
    if (ps.asleep) {
      if (lift > 1.02 * ps.mass * kG) w.wake_piece(ps.id);
      else continue;
    }
    // (drag: at most what stops it in a few ticks, never an overshoot)
    const f64 k = std::min(cfg_.drag, 0.5 / (dt * std::max(1e-9, msub / std::max(1e-9, ps.mass))));
    for (const auto& [X, m] : pts) {
      const V3 v = b->v + cross(b->w, X - b->x);
      w.apply_force(ps.id, X, V3{0.0, 0.0, m * kG} - v * (k * m));
    }
    // a hard landing in the water: a splash
    if (!wet_.count(ps.id) && ps.vel.z < -2.0) splashes_.push_back({ps.pos, static_cast<f32>(ps.mass * -ps.vel.z)});
  }
  wet_.swap(wet);
  samples_.swap(kept);
}

i64 WaterSystem::memory_bytes() const {
  i64 b = static_cast<i64>((active_.capacity() + wake_.capacity()) * sizeof(u64) + load_dirty_.capacity() * sizeof(u64) +
                           splashes_.capacity() * sizeof(Splash) + wet_.size() * 32 + sizeof(*this));
  for (const auto& [id, smp] : samples_) b += static_cast<i64>(smp.cells.capacity() * sizeof(i32) + 64);
  for (const auto& [k, l] : loads_) b += static_cast<i64>(l.capacity() * sizeof(VoxelLoad) + 64);
  return b;
}

u64 WaterSystem::state_hash() const {
  u64 hsh = 1469598103934665603ull;
  auto add = [&](u64 v) { hsh = (hsh ^ v) * 1099511628211ull; };
  for (u64 k : active_) add(k);
  add(static_cast<u64>(st_.steps));
  u64 cb;
  std::memcpy(&cb, &clock_, sizeof cb);
  add(cb);
  return hsh;
}

}  // namespace svx
