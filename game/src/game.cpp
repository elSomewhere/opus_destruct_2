// structvox game — the harness around the World: viewer, commands, output for the front end.
// Movers: movers.cpp.
#include "svx/game/game.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>

#include "svx/base/parallel.hpp"
#include "svx/game/replay.hpp"

namespace svx {

namespace {

using Clock = std::chrono::steady_clock;
inline f64 ms_since(Clock::time_point t0) { return std::chrono::duration<f64, std::milli>(Clock::now() - t0).count(); }

inline u64 mix64(u64 x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// A face's light darkened by its voxel's charring (burn 0..255).
inline u8 char_light(u8 light, u8 burn) {
  if (!burn) return light;
  const u32 k = 255u - (burn * 190u) / 255u;
  return static_cast<u8>((light * k + 127u) / 255u);
}

}  // namespace

Game::Game() { env_.attach(world_); }
Game::~Game() = default;
Game::Game(Game&&) = default;
Game& Game::operator=(Game&&) = default;

void Game::set_params(const GameParams& p) {
  if (log_) {
    Command c;
    c.tick = world_.ticks();
    c.type = Command::Type::Params;
    c.a = {p.fragility, p.impact, p.dif, 0.0, static_cast<f64>(p.debug_view), p.paused ? 1.0 : 0.0};
    log_->push(c);
  }
  if (p.debug_view != par_.debug_view)
    for (const auto& [k, c] : world_.grid().chunks()) remesh_.push_back(k);
  par_ = p;
  WorldParams wp;
  wp.fragility = p.fragility;
  wp.impact = p.impact;
  wp.dif = p.dif;
  wp.paused = p.paused;
  wp.debug_fields = p.debug_view == 1;
  world_.set_params(wp);
}

void Game::load(VoxelGrid&& g, const V3& spawn_pos, const V3& spawn_dir) {
  movers_.clear();
  mover_cols_.clear();
  shots_.clear();
  // the level's triggers and texture providers belong to it (they may point into its data)
  use_resolver = nullptr;
  walk_resolver = nullptr;
  shot_resolver = nullptr;
  mesh_base_ = MeshOptions{};
  viewer_set_ = false;
  // the old level's chunks and far tiles leave the front end
  for (const auto& [k, c] : world_.grid().chunks()) removed_chunks_.push_back(k);
  for (u64 k : far_sent_) {
    const IVec3 t = unkey3(k);
    far_removed_.push_back({t[0], t[1]});
  }
  far_sent_.clear();
  far_out_.clear();
  source_.reset();
  world_.load(std::move(g));
  world_.take_events();  // (the old level's pieces: gone with it)
  views_.clear();
  fading_.clear();
  events_.clear();
  remesh_.clear();
  charred_.clear();
  spawn_pos_ = spawn_pos;
  spawn_dir_ = spawn_dir;
  viewer_ = spawn_pos;
}

void Game::load_streaming(std::shared_ptr<const GameSource> src, f64 h, const StreamConfig& sc, const FarConfig& far) {
  VoxelGrid g;
  g.h = h;
  load(std::move(g), src->spawn_pos(), src->spawn_dir());
  source_ = src;
  stream_ = sc;
  far_ = far;
  world_.enable_streaming(src, sc);
  world_.set_focus(viewer_);
}

bool Game::load_delta(const std::vector<u8>& bytes) {
  if (!world_.load_delta(bytes)) return false;
  // (the delta's chunks hold the movers as they were when it was saved)
  for (Mover& m : movers_) set_mover_rows(m, m.rows);
  return true;
}

void Game::set_viewer(const V3& pos) {
  if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return;
  if (log_) log_->push({world_.ticks(), Command::Type::Viewer, {pos.x, pos.y, pos.z, 0.0, 0.0, 0.0}});
  if (viewer_set_ && walk_resolver && !movers_.empty())
    for (const MoverTrigger& t : walk_resolver(viewer_, pos)) activate_mover(t.mover, t.move);
  viewer_ = pos;
  viewer_set_ = true;
  world_.set_focus(pos);
}

void Game::carve(const V3& pos, f64 radius) {
  if (log_) log_->push({world_.ticks(), Command::Type::Carve, {pos.x, pos.y, pos.z, radius, 0.0, 0.0}});
  if (shot_resolver && !movers_.empty())
    for (const MoverTrigger& t : shot_resolver(pos)) activate_mover(t.mover, t.move);
  if (!movers_.empty()) shots_.push_back({pos, radius});
  world_.carve(pos, radius);
}

void Game::blast(const V3& pos, f64 radius, f64 energy) {
  if (log_) log_->push({world_.ticks(), Command::Type::Blast, {pos.x, pos.y, pos.z, radius, energy, 0.0}});
  if (!movers_.empty()) shots_.push_back({pos, radius});
  world_.blast(pos, radius, energy);
  // (dust: into the smoke)
  if (env_.smoke() && std::isfinite(energy) && energy > 0.0) env_.smoke()->emit_sphere(pos, 1.5 * radius, std::min(40.0, 6.0 * energy / 1e6));
}

void Game::ignite(const V3& pos, f64 radius) {
  if (log_) log_->push({world_.ticks(), Command::Type::Ignite, {pos.x, pos.y, pos.z, radius, 0.0, 0.0}});
  if (env_.fire()) env_.fire()->ignite(world_, pos, radius);
}

void Game::extinguish(const V3& pos, f64 radius) {
  if (log_) log_->push({world_.ticks(), Command::Type::Extinguish, {pos.x, pos.y, pos.z, radius, 0.0, 0.0}});
  if (env_.fire()) env_.fire()->extinguish(world_, pos, radius);
}

std::vector<SmokePoint> Game::smoke(i32 max) const {
  std::vector<SmokePoint> out;
  if (!env_.smoke() || max <= 0) return out;
  for (const auto& c : env_.smoke()->cells(world_, max)) out.push_back({c.pos, c.density});
  return out;
}

std::vector<FlamePoint> Game::flames(i32 max) const {
  std::vector<FlamePoint> out;
  if (!env_.fire() || max <= 0) return out;
  const auto& f = env_.fire()->flames();
  const size_t n = f.size(), m = std::min(n, static_cast<size_t>(max));
  out.reserve(m);
  for (size_t k = 0; k < m; ++k) {
    const auto& fl = f[k * n / m];
    out.push_back({fl.pos, fl.heat});
  }
  return out;
}

void Game::tick() {
  // movers first: their voxels are in place when the world's commands and pieces see them
  if (!par_.paused && !movers_.empty()) step_movers();
  world_.tick();
  if (!par_.paused) check_movers_hit();
  for (Fading& f : fading_) f.t += world_.config().dt;
  fading_.erase(std::remove_if(fading_.begin(), fading_.end(), [&](const Fading& f) { return f.t >= fade_time; }), fading_.end());
  drain_world_events();
  if (source_) far_update();
  if (const FireSystem* f = env_.fire()) {
    for (u64 k : world_.take_layer_changes(f->burn_layer())) charred_.insert(k);
    char_clock_ += world_.config().dt;
    if (char_clock_ >= char_remesh_s) {
      char_clock_ = 0.0;
      remesh_.insert(remesh_.end(), charred_.begin(), charred_.end());
      charred_.clear();
    }
  }
}

void Game::drain_world_events() {
  std::vector<WorldEvent> evs = world_.take_events();
  // new pieces' meshes (concurrently), then the events in order
  std::vector<const Body*> fresh(evs.size(), nullptr);
  std::vector<size_t> todo;
  for (size_t k = 0; k < evs.size(); ++k)
    if (evs[k].kind == WorldEvent::Kind::PieceAdded) {
      fresh[k] = world_.piece(evs[k].id);
      if (fresh[k]) todo.push_back(k);
    }
  std::vector<ChunkMesh> meshes(evs.size());
  std::optional<SerialScope> serial_all;
  if (!mesh_base_.concurrent) serial_all.emplace();  // (providers with unsynchronized caches)
  parallel_for(static_cast<i64>(todo.size()), 4, [&](i64 k0, i64 k1) {
    SerialScope serial;
    for (i64 k = k0; k < k1; ++k) meshes[todo[size_t(k)]] = piece_mesh(*fresh[todo[size_t(k)]]);
  });
  for (size_t k = 0; k < evs.size(); ++k) {
    const WorldEvent& e = evs[k];
    GameEvent g;
    g.id = e.id;
    g.pos = e.pos;
    g.vel = e.vel;
    g.ang = e.ang;
    g.normal = e.normal;
    g.radius = e.radius;
    g.strength = e.strength;
    g.voxels = e.voxels;
    switch (e.kind) {
      case WorldEvent::Kind::PieceAdded:
        if (!fresh[k]) continue;  // (gone again before the harness saw it)
        views_[e.id] = {fresh[k]->x, fresh[k]->q};
        g.kind = GameEvent::Kind::Detached;
        g.pos = fresh[k]->x;
        g.mesh = std::move(meshes[k]);
        break;
      case WorldEvent::Kind::PieceRemoved: {
        const auto it = views_.find(e.id);
        if (it == views_.end()) continue;
        if (e.end == PieceEnd::Culled) fading_.push_back({e.id, 0.0, e.pos, e.rot * conj(it->second.q0)});
        views_.erase(it);
        continue;  // (the front end drops pieces missing from the poses)
      }
      case WorldEvent::Kind::Crack:
        g.kind = GameEvent::Kind::Crack;
        break;
      case WorldEvent::Kind::Impact:
        g.kind = GameEvent::Kind::Impact;
        break;
      case WorldEvent::Kind::Dust:
        g.kind = GameEvent::Kind::Dust;
        break;
      case WorldEvent::Kind::Forgotten:
        continue;  // (out of range by construction: nothing on screen changes)
    }
    events_.push_back(std::move(g));
  }
  // (a front end that stops taking events does not make the harness grow)
  const size_t cap = static_cast<size_t>(std::max(16, max_events));
  if (events_.size() > cap) events_.erase(events_.begin(), events_.begin() + static_cast<long>(events_.size() - cap));
  if (removed_chunks_.size() > 4 * cap) {
    std::sort(removed_chunks_.begin(), removed_chunks_.end());
    removed_chunks_.erase(std::unique(removed_chunks_.begin(), removed_chunks_.end()), removed_chunks_.end());
  }
}

ChunkMesh Game::piece_mesh(const Body& b) const {
  MeshOptions mo;
  mo.texels_per_metre = mesh_base_.texels_per_metre;
  mo.texture = mesh_base_.texture;
  const BodyShape& S = b.shape;
  if (par_.debug_view == 2) {
    mo.debug = [&](const IVec3& p) -> u8 {
      const i32 i = S.index(p);
      if (i < 0) return 0;
      return static_cast<u8>(1 + (mix64(static_cast<u64>(b.id) * 131 + S.frag[size_t(i)]) % 254));
    };
  }
  if (const FireSystem* f = env_.fire(); f && !S.layer[size_t(f->burn_layer())].empty()) {
    const int L = f->burn_layer();
    mo.light = [&S, L](const IVec3& p, int) -> u8 { return char_light(255, S.layer_at(L, S.index(p))); };
  }
  ChunkMesh out = mesh_shape(S, world_.voxel_size(), mo);
  // to world coordinates at the piece's pose now
  const M3 R = to_matrix(b.q);
  for (MeshVertex& v : out.vertices) {
    const V3 s{v.pos[0], v.pos[1], v.pos[2]};
    const V3 w = b.x + R * (s - b.com);
    v.pos[0] = static_cast<f32>(w.x);
    v.pos[1] = static_cast<f32>(w.y);
    v.pos[2] = static_cast<f32>(w.z);
    const V3 n = R * V3{v.normal[0] / 127.0, v.normal[1] / 127.0, v.normal[2] / 127.0};
    v.normal[0] = static_cast<i8>(std::lround(std::clamp(n.x, -1.0, 1.0) * 127.0));
    v.normal[1] = static_cast<i8>(std::lround(std::clamp(n.y, -1.0, 1.0) * 127.0));
    v.normal[2] = static_cast<i8>(std::lround(std::clamp(n.z, -1.0, 1.0) * 127.0));
  }
  return out;
}

std::vector<ChunkMesh> Game::take_meshes(const MeshOptions& base) {
  const auto t0 = Clock::now();
  mesh_base_ = base;
  std::vector<u64> keys = world_.take_changed_chunks();
  keys.insert(keys.end(), remesh_.begin(), remesh_.end());
  remesh_.clear();
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  MeshOptions mo = base;
  // debug views read the world's diagnostic fields (serially, before meshing)
  std::unordered_map<u64, std::vector<u8>> field;
  if (par_.debug_view == 1 || par_.debug_view == 2) {
    const DebugField f = par_.debug_view == 1 ? DebugField::Utilization : DebugField::Fragment;
    for (u64 k : keys) {
      std::vector<u8> v;
      if (world_.debug_field(unkey3(k), f, &v)) field.emplace(k, std::move(v));
    }
    mo.debug = [&](const IVec3& p) -> u8 {
      const IVec3 cc = chunk_of(p);
      const auto it = field.find(key3(cc[0], cc[1], cc[2]));
      return it == field.end() ? 0 : it->second[size_t(chunk_index(p))];
    };
  }
  // charred voxels are darker (the burn layer)
  const VoxelGrid& g = world_.grid();
  if (const FireSystem* f = env_.fire()) {
    const int L = f->burn_layer();
    auto base_light = mo.light;
    mo.light = [&g, L, base_light](const IVec3& p, int face) -> u8 {
      const u8 l = base_light ? base_light(p, face) : 255;
      return char_light(l, g.layer(L, p));
    };
  }
  std::vector<ChunkMesh> meshes(keys.size());
  std::optional<SerialScope> serial;
  if (!mo.concurrent) serial.emplace();
  parallel_for(static_cast<i64>(keys.size()), 1, [&](i64 b0, i64 e0) {
    for (i64 j = b0; j < e0; ++j) meshes[size_t(j)] = mesh_chunk(g, unkey3(keys[size_t(j)]), mo, false);
  });
  std::vector<ChunkMesh> out;
  for (size_t j = 0; j < keys.size(); ++j) {
    if (meshes[j].vertices.empty()) {
      removed_chunks_.push_back(keys[j]);
      continue;
    }
    out.push_back(std::move(meshes[j]));
  }
  mesh_ms_ = ms_since(t0);
  return out;
}

std::vector<u64> Game::take_removed_chunks() {
  std::vector<u64> out = world_.take_evicted_chunks();
  out.insert(out.end(), removed_chunks_.begin(), removed_chunks_.end());
  removed_chunks_.clear();
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<GameEvent> Game::take_events() {
  std::vector<GameEvent> out;
  out.swap(events_);
  return out;
}

std::vector<PiecePose> Game::pieces() const {
  std::vector<PiecePose> out;
  out.reserve(views_.size() + fading_.size());
  for (const PieceState& p : world_.pieces()) {
    const auto it = views_.find(p.id);
    if (it == views_.end()) continue;
    out.push_back({p.id, p.pos, p.rot * conj(it->second.q0), 1.0});
  }
  for (const Fading& f : fading_) out.push_back({f.id, f.pos, f.rot, std::max(0.0, 1.0 - f.t / fade_time)});
  std::sort(out.begin(), out.end(), [](const PiecePose& a, const PiecePose& b) { return a.id < b.id; });
  return out;
}

// ---------------------------------------------------------------------------------------------
// Far render tier

void Game::far_update() {
  if (!source_ || far_.radius <= stream_.evict_radius || far_.tile <= 0 || far_.factor <= 0) return;
  const f64 h = world_.voxel_size();
  const f64 tile_m = h * kChunk * far_.tile;
  const IVec3 lo = source_->chunk_lo(), hi = source_->chunk_hi();
  const i32 tlo0 = lo[0] / far_.tile - 1, thi0 = hi[0] / far_.tile + 1;
  const i32 tlo1 = lo[1] / far_.tile - 1, thi1 = hi[1] / far_.tile + 1;
  const f64 vx = viewer_.x + 0.5 * h, vy = viewer_.y + 0.5 * h;
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
    if (d < stream_.evict_radius - 16.0 || d > far_.radius + tile_m) {
      far_sent_.erase(k);
      far_removed_.push_back({t[0], t[1]});
    }
  }
  const i32 r = static_cast<i32>(std::ceil(far_.radius / tile_m)) + 1;
  const i32 cx = static_cast<i32>(std::floor(vx / tile_m)), cy = static_cast<i32>(std::floor(vy / tile_m));
  std::vector<std::pair<f64, u64>> want;
  for (i32 tx = std::max(tlo0, cx - r); tx <= std::min(thi0, cx + r); ++tx)
    for (i32 ty = std::max(tlo1, cy - r); ty <= std::min(thi1, cy + r); ++ty) {
      const f64 d = near_dist(tx, ty);
      if (d <= stream_.evict_radius || d > far_.radius) continue;
      const u64 k = key3(tx, ty, 0);
      if (!far_sent_.count(k)) want.push_back({d, k});
    }
  std::sort(want.begin(), want.end());
  int budget = far_.tiles_per_tick;
  for (const auto& [d, k] : want) {
    if (budget-- <= 0) break;
    const IVec3 t = unkey3(k);
    far_sent_.insert(k);
    ChunkMesh m = far_mesh(t[0], t[1]);
    if (!m.indices.empty()) far_out_.push_back(std::move(m));
  }
}

ChunkMesh Game::far_mesh(i32 tx, i32 ty) const {
  const i32 f = far_.factor, T = far_.tile;
  const IVec3 clo = source_->chunk_lo(), chi = source_->chunk_hi();
  const IVec3 lo{tx * T * kChunk, ty * T * kChunk, clo[2] * kChunk};
  const IVec3 n{T * kChunk / f, T * kChunk / f, (chi[2] - clo[2]) * kChunk / f};
  std::vector<Vox> occ;
  ChunkMesh m;
  if (source_->coarse(lo, n, f, occ)) m = mesh_coarse(occ, n, lo, f, world_.voxel_size());
  m.chunk = {tx, ty, 0};
  return m;
}

std::vector<ChunkMesh> Game::take_far_meshes() {
  std::vector<ChunkMesh> out;
  out.swap(far_out_);
  return out;
}

std::vector<std::array<i32, 2>> Game::take_far_removed() {
  std::vector<std::array<i32, 2>> out;
  out.swap(far_removed_);
  return out;
}

// ---------------------------------------------------------------------------------------------
// Stats, hashes

GameStats Game::stats() const {
  GameStats s;
  static_cast<WorldStats&>(s) = world_.stats();
  s.mesh_ms = mesh_ms_;
  s.movers = static_cast<i32>(movers_.size());
  if (const FireSystem* f = env_.fire()) {
    s.fire_hot = f->stats().hot;
    s.fire_burning = f->stats().burning;
    s.env_ms += f->stats().step_ms;
  }
  if (const SmokeSystem* m = env_.smoke()) {
    s.smoke_cells = m->stats().cells;
    s.smoke_blocks = m->stats().blocks;
    s.env_ms += m->stats().step_ms;
  }
  return s;
}

u64 Game::session_hash() const {
  u64 hsh = world_.session_hash();
  auto mix = [&](u64 v) { hsh = (hsh ^ v) * 1099511628211ull; };
  auto bits = [](f64 x) {
    u64 u;
    std::memcpy(&u, &x, sizeof u);
    return u;
  };
  for (const Mover& m : movers_) {
    mix(bits(m.level));
    mix(bits(m.timer));
    mix(static_cast<u64>(static_cast<u32>(m.move)) | (static_cast<u64>(m.leg) << 32) | (static_cast<u64>(m.rows) << 40) |
        (m.stopped ? 1ull << 62 : 0ull) | (m.disabled ? 1ull << 63 : 0ull));
  }
  return hsh;
}

}  // namespace svx
