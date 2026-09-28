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
#include "svx/world/tunables.hpp"

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

// The heat (units of the fire's heat layer) from which a voxel glows (texture 0xFE00 +
// material: burning wood, red-hot steel).
inline u8 glow_units(const FireSystem& f) { return static_cast<u8>(std::min(255.0, std::ceil(f.config().glow_c / FireSystem::kHeatUnit))); }

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
    for (const auto& [k, c] : world_.grid().chunks()) remesh_.insert(k);
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
  // (the old level's oriented grids: their meshes and occupancy go)
  for (const auto& [id, k] : grid_meshed_) removed_grid_chunks_.push_back(GridChunk{id, unkey3(k)});
  grid_meshed_.clear();
  grid_box_.clear();
  for (const auto& [k, bits] : grid_occ_) occ_changed_.insert(k);
  grid_occ_.clear();
  world_.load(std::move(g));
  world_.take_events();  // (the old level's pieces: gone with it)
  views_.clear();
  fading_.clear();
  events_.clear();
  remesh_.clear();
  decor_only_.clear();
  meshed_.clear();
  piece_remesh_.clear();
  charred_.clear();
  for (u64 k : wet_sent_) wet_removed_.push_back(k);
  wet_sent_.clear();
  wet_dirty_.clear();
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

void Game::pour(const V3& pos, f64 radius) {
  if (log_) log_->push({world_.ticks(), Command::Type::Pour, {pos.x, pos.y, pos.z, radius, 0.0, 0.0}});
  if (env_.water()) env_.water()->pour(world_, pos, radius);
}

std::vector<ChunkMesh> Game::take_water_meshes() {
  std::vector<ChunkMesh> out;
  const WaterSystem* ws = env_.water();
  if (!ws || wet_clock_ < water_remesh_s || wet_dirty_.empty()) return out;
  wet_clock_ = 0.0;
  std::vector<u64> keys(wet_dirty_.begin(), wet_dirty_.end());
  wet_dirty_.clear();
  std::sort(keys.begin(), keys.end());
  std::vector<ChunkMesh> meshes(keys.size());
  const VoxelGrid& g = world_.grid();
  const int L = ws->water_layer();
  parallel_for(static_cast<i64>(keys.size()), 1, [&](i64 b0, i64 e0) {
    for (i64 j = b0; j < e0; ++j) meshes[size_t(j)] = mesh_water(g, L, unkey3(keys[size_t(j)]));
  });
  for (size_t j = 0; j < keys.size(); ++j) {
    if (meshes[j].vertices.empty()) {
      if (wet_sent_.erase(keys[j])) wet_removed_.push_back(keys[j]);
      continue;
    }
    wet_sent_.insert(keys[j]);
    out.push_back(std::move(meshes[j]));
  }
  return out;
}

std::vector<u64> Game::take_water_removed() {
  std::vector<u64> out;
  // (a chunk with a water mesh sent since its removal was queued stays)
  for (u64 k : wet_removed_)
    if (!wet_sent_.count(k)) out.push_back(k);
  wet_removed_.clear();
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

void Game::heat(const V3& pos, f64 radius, f64 celsius) {
  if (log_) log_->push({world_.ticks(), Command::Type::Heat, {pos.x, pos.y, pos.z, radius, celsius, 0.0}});
  if (env_.fire()) env_.fire()->heat(world_, pos, radius, celsius);
}

void Game::drain(const V3& pos, f64 radius) {
  if (log_) log_->push({world_.ticks(), Command::Type::Drain, {pos.x, pos.y, pos.z, radius, 0.0, 0.0}});
  if (env_.water()) env_.water()->drain(world_, pos, radius);
}

bool Game::set_env(i32 index, f64 value) {
  if (!env_param(index) || !std::isfinite(value)) return false;
  if (log_) log_->push({world_.ticks(), Command::Type::EnvParam, {static_cast<f64>(index), value, 0.0, 0.0, 0.0, 0.0}});
  return env_.set(index, value);
}

bool Game::set_tunable(i32 index, f64 value) {
  if (!tunable(index) || !std::isfinite(value)) return false;
  if (log_) log_->push({world_.ticks(), Command::Type::Tunable, {static_cast<f64>(index), value, 0.0, 0.0, 0.0, 0.0}});
  return svx::set_tunable(world_, index, value);
}

bool Game::set_tunable(const char* name, f64 value) { return set_tunable(tunable_index(name), value); }

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
  if (const WaterSystem* ws = env_.water()) {
    // (a chunk's water surface also shows at its neighbours' faces)
    for (u64 k : world_.take_layer_changes(ws->water_layer())) {
      const IVec3 c = unkey3(k);
      wet_dirty_.insert(k);
      for (int d = 0; d < 6; ++d) {
        IVec3 q = c;
        q[d / 2] += (d & 1) ? -1 : 1;
        const Chunk* ch = world_.grid().chunk(q);
        if (ch && !ch->layer[size_t(ws->water_layer())].empty()) wet_dirty_.insert(key3(q[0], q[1], q[2]));
      }
    }
    wet_clock_ += world_.config().dt;
    for (const auto& s : ws->splashes()) {
      GameEvent g;
      g.kind = GameEvent::Kind::Splash;
      g.pos = s.pos;
      g.strength = s.strength;
      events_.push_back(std::move(g));
    }
  }
  if (FireSystem* f = env_.fire(); f && f->ok()) {
    // charring (burn changes) and glow (voxels crossing the glow temperature): meshed again at
    // most every char_remesh_s; pieces too (a Remesh event)
    for (u64 k : world_.take_layer_changes(f->burn_layer())) charred_.insert(k);
    world_.take_layer_changes(f->heat_layer());  // (heat itself: not shown)
    for (u64 k : f->take_glow_changes()) charred_.insert(k);
    for (i64 id : f->take_piece_changes()) piece_remesh_.push_back(id);
    char_clock_ += world_.config().dt;
    if (char_clock_ >= char_remesh_s) {
      char_clock_ = 0.0;
      remesh_.insert(charred_.begin(), charred_.end());
      charred_.clear();
      std::sort(piece_remesh_.begin(), piece_remesh_.end());
      piece_remesh_.erase(std::unique(piece_remesh_.begin(), piece_remesh_.end()), piece_remesh_.end());
      for (i64 id : piece_remesh_) {
        const Body* b = world_.piece(id);
        const auto vt = views_.find(id);
        if (!b || vt == views_.end()) continue;
        GameEvent g;
        g.kind = GameEvent::Kind::Remesh;
        g.id = id;
        g.pos = b->x;
        g.vel = b->v;
        g.ang = b->w;
        g.voxels = b->count;
        g.mesh = piece_mesh(*b);
        vt->second = {b->x, b->q};  // (its poses from now on: from this mesh's frame)
        events_.push_back(std::move(g));
      }
      piece_remesh_.clear();
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
      case WorldEvent::Kind::GridAdded:
        continue;  // (its chunks come with take_meshes)
      case WorldEvent::Kind::GridRemoved: {
        const GridId id = static_cast<GridId>(e.id);
        for (auto it = grid_meshed_.lower_bound({id, 0}); it != grid_meshed_.end() && it->first == id;) {
          removed_grid_chunks_.push_back(GridChunk{id, unkey3(it->second)});
          it = grid_meshed_.erase(it);
        }
        if (const auto bt = grid_box_.find(id); bt != grid_box_.end()) {
          std::vector<u64> wc;
          grid_box_chunks(bt->second.first, bt->second.second, &wc);
          grid_box_.erase(bt);
          grids_occupancy(wc);
        }
        continue;
      }
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
  // each of its shapes (one per grid it came from), in world coordinates at its pose now
  ChunkMesh out;
  for (size_t k = 0; k < b.shapes.size(); ++k) {
    ChunkMesh m = shape_mesh(b, k);
    const u32 base = static_cast<u32>(out.vertices.size());
    out.vertices.insert(out.vertices.end(), m.vertices.begin(), m.vertices.end());
    for (u32 i : m.indices) out.indices.push_back(base + i);
  }
  return out;
}

ChunkMesh Game::shape_mesh(const Body& b, size_t k) const {
  MeshOptions mo;
  mo.texels_per_metre = mesh_base_.texels_per_metre;
  mo.texture = mesh_base_.texture;
  const BodyShape& S = b.shapes[k];
  if (par_.debug_view == 2) {
    mo.debug = [&](const IVec3& p) -> u8 {
      const i32 i = S.index(p);
      if (i < 0) return 0;
      return static_cast<u8>(1 + (mix64(static_cast<u64>(b.id) * 131 + S.frag[size_t(i)]) % 254));
    };
  }
  if (const FireSystem* f = env_.fire(); f && f->ok() && !S.layer[size_t(f->burn_layer())].empty()) {
    const int L = f->burn_layer();
    mo.light = [&S, L](const IVec3& p, int) -> u8 { return char_light(255, S.layer_at(L, S.index(p))); };
  }
  if (const FireSystem* f = env_.fire(); f && f->ok() && !S.layer[size_t(f->heat_layer())].empty()) {
    const int H = f->heat_layer();
    const u8 glow = glow_units(*f);
    auto base_tex = mo.texture;
    mo.texture = [&S, H, glow, base_tex](const IVec3& p, int face) -> u16 {
      const i32 i = S.index(p);
      if (i >= 0 && S.layer_at(H, i) >= glow) return static_cast<u16>(0xFE00 + static_cast<u16>(vox_mat(S.vox[size_t(i)])));
      return base_tex ? base_tex(p, face) : static_cast<u16>(0xFF00 + static_cast<u16>(vox_mat(S.get(p))));
    };
  }
  ChunkMesh out = mesh_shape(S, world_.voxel_size(), mo);
  // to world coordinates at the piece's pose now (through the shape's place in the piece)
  const M3 R = S.xf.identity ? to_matrix(b.q) : to_matrix(b.q) * S.xf.R;
  const V3 off = to_matrix(b.q) * (S.xf.off - b.com);
  for (MeshVertex& v : out.vertices) {
    const V3 s{v.pos[0], v.pos[1], v.pos[2]};
    const V3 w = S.xf.identity ? b.x + R * (s - b.com) : b.x + off + R * s;
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
  // (chunks meshed again for their decoration only: their voxels did not change)
  decor_only_.clear();
  {
    std::unordered_set<u64> voxel_changed(keys.begin(), keys.end());
    for (u64 k : remesh_)
      if (!voxel_changed.count(k)) decor_only_.insert(k);
  }
  if (const WaterSystem* ws = env_.water())
    for (u64 k : keys) {  // (new chunks with water, water uncovered or covered by voxels)
      const Chunk* ch = world_.grid().chunk(unkey3(k));
      if ((ch && !ch->layer[size_t(ws->water_layer())].empty()) || wet_sent_.count(k)) wet_dirty_.insert(k);
    }
  keys.insert(keys.end(), remesh_.begin(), remesh_.end());
  remesh_.clear();
  if (par_.debug_view != 0) decor_only_.clear();  // (a debug view change: all of it)
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
  if (const FireSystem* f = env_.fire(); f && f->ok()) {
    const int L = f->burn_layer();
    auto base_light = mo.light;
    mo.light = [&g, L, base_light](const IVec3& p, int face) -> u8 {
      const u8 l = base_light ? base_light(p, face) : 255;
      return char_light(l, g.layer(L, p));
    };
    // (and burning or red-hot voxels glow)
    const int H = f->heat_layer();
    const u8 glow = glow_units(*f);
    auto base_tex = mo.texture;
    mo.texture = [&g, H, glow, base_tex](const IVec3& p, int face) -> u16 {
      if (g.layer(H, p) >= glow) return static_cast<u16>(0xFE00 + static_cast<u16>(vox_mat(g.get(p))));
      return base_tex ? base_tex(p, face) : static_cast<u16>(0xFF00 + static_cast<u16>(vox_mat(g.get(p))));
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
      meshed_.erase(keys[j]);
      continue;
    }
    meshed_.insert(keys[j]);  // (a removal queued before - an old level's - is void)
    out.push_back(std::move(meshes[j]));
  }
  // The oriented grids' chunks: meshed in their lattice, placed in the world by their frames
  // (docs/GRIDS.md). Debug views read the grids' fields; materials give their colours.
  const std::vector<GridChunk> gcs = world_.take_changed_grid_chunks();
  if (!gcs.empty()) {
    std::vector<ChunkMesh> gm(gcs.size());
    std::vector<std::vector<u8>> gfield(gcs.size());
    if (par_.debug_view == 1 || par_.debug_view == 2) {
      const DebugField f = par_.debug_view == 1 ? DebugField::Utilization : DebugField::Fragment;
      for (size_t j = 0; j < gcs.size(); ++j) world_.debug_field(gcs[j].grid, gcs[j].chunk, f, &gfield[j]);
    }
    parallel_for(static_cast<i64>(gcs.size()), 1, [&](i64 b0, i64 e0) {
      for (i64 j = b0; j < e0; ++j) {
        const GridChunk& gc = gcs[size_t(j)];
        const VoxelGrid* G = world_.grid(gc.grid);
        GridFrame fr;
        if (!G || !world_.grid_frame(gc.grid, &fr)) continue;
        MeshOptions mg;
        mg.texels_per_metre = base.texels_per_metre;
        const std::vector<u8>& field = gfield[size_t(j)];
        if (!field.empty()) mg.debug = [&field](const IVec3& p) -> u8 { return field[size_t(chunk_index(p))]; };
        ChunkMesh m = mesh_chunk(*G, gc.chunk, mg, false);
        const M3 R = to_matrix(fr.rot);
        for (MeshVertex& v : m.vertices) {
          const V3 w = fr.origin + R * V3{v.pos[0], v.pos[1], v.pos[2]};
          v.pos[0] = static_cast<f32>(w.x);
          v.pos[1] = static_cast<f32>(w.y);
          v.pos[2] = static_cast<f32>(w.z);
          const V3 n = R * V3{v.normal[0] / 127.0, v.normal[1] / 127.0, v.normal[2] / 127.0};
          v.normal[0] = static_cast<i8>(std::lround(std::clamp(n.x, -1.0, 1.0) * 127.0));
          v.normal[1] = static_cast<i8>(std::lround(std::clamp(n.y, -1.0, 1.0) * 127.0));
          v.normal[2] = static_cast<i8>(std::lround(std::clamp(n.z, -1.0, 1.0) * 127.0));
        }
        m.chunk = gc.chunk;
        m.grid = gc.grid;
        gm[size_t(j)] = std::move(m);
      }
    });
    std::vector<u64> occ;
    for (size_t j = 0; j < gcs.size(); ++j) {
      const GridChunk& gc = gcs[j];
      const std::pair<GridId, u64> key{gc.grid, key3(gc.chunk[0], gc.chunk[1], gc.chunk[2])};
      // (the chunk's world box: the grid's box grows with it, its occupancy is felt there)
      const f64 h = world_.voxel_size();
      V3 lo{INFINITY, INFINITY, INFINITY}, hi{-INFINITY, -INFINITY, -INFINITY};
      for (int c = 0; c < 8; ++c) {
        const V3 p{h * (gc.chunk[0] * kChunk + ((c & 1) ? kChunk : 0) - 0.5), h * (gc.chunk[1] * kChunk + ((c & 2) ? kChunk : 0) - 0.5),
                   h * (gc.chunk[2] * kChunk + ((c & 4) ? kChunk : 0) - 0.5)};
        const V3 w = world_.grid_to_world(gc.grid, p);
        for (int a = 0; a < 3; ++a) {
          lo[a] = std::min(lo[a], w[a]);
          hi[a] = std::max(hi[a], w[a]);
        }
      }
      auto& box = grid_box_.try_emplace(gc.grid, lo, hi).first->second;
      for (int a = 0; a < 3; ++a) {
        box.first[a] = std::min(box.first[a], lo[a]);
        box.second[a] = std::max(box.second[a], hi[a]);
      }
      grid_box_chunks(lo, hi, &occ);
      if (gm[j].vertices.empty()) {
        if (grid_meshed_.erase(key)) removed_grid_chunks_.push_back(gc);
        continue;
      }
      grid_meshed_.insert(key);
      out.push_back(std::move(gm[j]));
    }
    grids_occupancy(occ);
  }
  mesh_ms_ = ms_since(t0);
  return out;
}

std::vector<GridChunk> Game::take_removed_grid_chunks() {
  std::vector<GridChunk> out;
  out.swap(removed_grid_chunks_);
  // (a chunk meshed again since its removal was queued stays)
  out.erase(std::remove_if(out.begin(), out.end(),
                           [&](const GridChunk& c) { return grid_meshed_.count({c.grid, key3(c.chunk[0], c.chunk[1], c.chunk[2])}) > 0; }),
            out.end());
  return out;
}

void Game::grid_box_chunks(const V3& lo, const V3& hi, std::vector<u64>* out) const {
  const f64 h = world_.voxel_size();
  auto vc = [&](f64 x) { return static_cast<i32>(std::floor(x / h + 0.5)) >> kChunkBits; };
  const i32 x0 = vc(lo.x), x1 = vc(hi.x), y0 = vc(lo.y), y1 = vc(hi.y), z0 = vc(lo.z), z1 = vc(hi.z);
  if (static_cast<i64>(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 1 << 20) return;  // (never: grids are bounded)
  for (i32 x = x0; x <= x1; ++x)
    for (i32 y = y0; y <= y1; ++y)
      for (i32 z = z0; z <= z1; ++z) out->push_back(key3(x, y, z));
}

void Game::grids_occupancy(const std::vector<u64>& world_chunks) {
  // Each world chunk's overlay again: its voxels whose centres lie in a grid's solid voxel (so a
  // turned wall has no holes a centre-to-centre splat would leave).
  std::vector<u64> wc = world_chunks;
  std::sort(wc.begin(), wc.end());
  wc.erase(std::unique(wc.begin(), wc.end()), wc.end());
  if (wc.empty()) return;
  const f64 h = world_.voxel_size();
  struct Placed {
    const VoxelGrid* g;
    V3 origin;
    M3 Rt;
    V3 lo, hi;  // the solid chunks' world box
  };
  std::vector<Placed> placed;
  for (GridId id : world_.grids()) {
    const VoxelGrid* G = world_.grid(id);
    GridFrame fr;
    if (!G || !world_.grid_frame(id, &fr)) continue;
    const M3 R = to_matrix(fr.rot);
    Placed pl{G, fr.origin, transpose(R), {INFINITY, INFINITY, INFINITY}, {-INFINITY, -INFINITY, -INFINITY}};
    for (const auto& [k, c] : G->chunks()) {
      if (c.uniform && !vox_solid(c.value)) continue;
      const IVec3 cc = unkey3(k);
      for (int q = 0; q < 8; ++q) {
        const V3 p{h * (cc[0] * kChunk + ((q & 1) ? kChunk : 0) - 0.5), h * (cc[1] * kChunk + ((q & 2) ? kChunk : 0) - 0.5),
                   h * (cc[2] * kChunk + ((q & 4) ? kChunk : 0) - 0.5)};
        const V3 w = fr.origin + R * p;
        for (int a = 0; a < 3; ++a) {
          pl.lo[a] = std::min(pl.lo[a], w[a]);
          pl.hi[a] = std::max(pl.hi[a], w[a]);
        }
      }
    }
    if (pl.lo.x <= pl.hi.x) placed.push_back(pl);
  }
  for (u64 k : wc) {
    const IVec3 cc = unkey3(k);
    const V3 clo{h * (cc[0] * kChunk - 0.5), h * (cc[1] * kChunk - 0.5), h * (cc[2] * kChunk - 0.5)};
    const V3 chi{clo.x + h * kChunk, clo.y + h * kChunk, clo.z + h * kChunk};
    std::vector<u8> bits;
    for (const Placed& pl : placed) {
      // (the chunk's voxels whose centres lie in the grid's box)
      i32 a0[3], a1[3];
      bool none = false;
      for (int a = 0; a < 3; ++a) {
        a0[a] = std::max(cc[a] * kChunk, static_cast<i32>(std::floor(pl.lo[a] / h + 0.5)));
        a1[a] = std::min(cc[a] * kChunk + kChunk - 1, static_cast<i32>(std::floor(pl.hi[a] / h + 0.5)));
        none = none || a0[a] > a1[a];
      }
      if (none) continue;
      const Chunk* gch = nullptr;
      IVec3 gcc{0, 0, 0};
      bool have = false;
      for (i32 x = a0[0]; x <= a1[0]; ++x)
        for (i32 y = a0[1]; y <= a1[1]; ++y)
          for (i32 z = a0[2]; z <= a1[2]; ++z) {
            const V3 q = pl.Rt * (V3{h * x, h * y, h * z} - pl.origin);
            const IVec3 v{static_cast<i32>(std::floor(q.x / h + 0.5)), static_cast<i32>(std::floor(q.y / h + 0.5)),
                          static_cast<i32>(std::floor(q.z / h + 0.5))};
            const IVec3 vc = chunk_of(v);
            if (!have || vc != gcc) {
              gch = pl.g->chunk(vc);
              gcc = vc;
              have = true;
            }
            if (!gch || !vox_solid(gch->uniform ? gch->value : gch->v[size_t(chunk_index(v))])) continue;
            if (bits.empty()) bits.assign(kChunkVox / 8, 0);
            const int bi = chunk_index(IVec3{x, y, z});
            bits[size_t(bi >> 3)] = static_cast<u8>(bits[size_t(bi >> 3)] | (1u << (bi & 7)));
          }
    }
    const auto ot = grid_occ_.find(k);
    if (bits.empty()) {
      if (ot != grid_occ_.end()) {
        grid_occ_.erase(ot);
        occ_changed_.insert(k);
      }
      continue;
    }
    if (ot != grid_occ_.end() && ot->second == bits) continue;
    grid_occ_[k] = std::move(bits);
    occ_changed_.insert(k);
  }
}

int Game::chunk_occupancy(const IVec3& cc, u8* bits) const {
  const Chunk* ch = world_.grid().chunk(cc);
  const auto ot = grid_occ_.find(key3(cc[0], cc[1], cc[2]));
  if (ot == grid_occ_.end()) {
    // the world grid's alone
    if (!ch) return 0;
    if (ch->uniform) return vox_solid(ch->value) ? 1 : 0;
    std::fill(bits, bits + kChunkVox / 8, u8{0});
    int any = 0, all = 1;
    for (int v = 0; v < kChunkVox; ++v) {
      if (vox_solid(ch->v[size_t(v)])) {
        bits[v >> 3] = static_cast<u8>(bits[v >> 3] | (1u << (v & 7)));
        any = 1;
      } else {
        all = 0;
      }
    }
    return all ? 1 : any ? 2 : 0;
  }
  // with the grids' overlay
  std::fill(bits, bits + kChunkVox / 8, u8{0});
  int any = 0, all = 1;
  for (int v = 0; v < kChunkVox; ++v) {
    const bool solid = (ch && vox_solid(ch->uniform ? ch->value : ch->v[size_t(v)])) || ((ot->second[size_t(v >> 3)] >> (v & 7)) & 1);
    if (solid) {
      bits[v >> 3] = static_cast<u8>(bits[v >> 3] | (1u << (v & 7)));
      any = 1;
    } else {
      all = 0;
    }
  }
  return all ? 1 : any ? 2 : 0;
}

std::vector<u64> Game::take_occupancy_changed() {
  std::vector<u64> out(occ_changed_.begin(), occ_changed_.end());
  occ_changed_.clear();
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<u64> Game::take_removed_chunks() {
  std::vector<u64> out = world_.take_evicted_chunks();
  for (u64 k : out) {
    wet_dirty_.erase(k);
    meshed_.erase(k);
    if (wet_sent_.erase(k)) wet_removed_.push_back(k);
  }
  // (a chunk meshed since its removal was queued - the old level's key, the new level's chunk -
  // stays)
  for (u64 k : removed_chunks_)
    if (!meshed_.count(k)) out.push_back(k);
  removed_chunks_.clear();
  meshed_.clear();
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
  if (const WaterSystem* w = env_.water()) {
    s.water_active = w->stats().active;
    s.water_loads = w->stats().loads;
    s.floating = w->stats().floating;
    s.env_ms += w->stats().step_ms;
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
