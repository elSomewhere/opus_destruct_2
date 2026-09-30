// structvox game — the harness around the World: viewer, commands, output for the front end.
// Movers: movers.cpp.
#include "svx/game/game.hpp"

#include "pedestrians.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>

#include "svx/base/diag.hpp"
#include "svx/base/parallel.hpp"
#include "svx/base/rotation.hpp"
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

Game::Game() {
  env_.attach(world_);
  paint_layer_ = world_.add_layer({"paint", true, LayerBind::Solid});
}
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
  drops_.clear();
  grid_charred_.clear();
  grid_remesh_.clear();
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
  // (the old level's oriented grids: their meshes and places go)
  for (const auto& [id, k] : grid_meshed_) removed_grid_chunks_.push_back(GridChunk{id, unkey3(k)});
  grid_meshed_.clear();
  for (const auto& [id, v] : grid_sent_) removed_grids_.push_back(id);
  grid_sent_.clear();
  world_.load(std::move(g));
  world_.take_events();  // (the old level's pieces: gone with it)
  paint_layer_ = world_.add_layer({"paint", true, LayerBind::Solid});
  vehicles_.clear();
  next_vehicle_ = 1;
  player_vehicle_ = 0;
  player_input_ = VehicleInput{};
  parked_spots_.clear();
  traffic_clock_ = 0.0;
  if (Pedestrians* p = people()) p->clear();
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
  // (a world with roads has traffic: its vehicles' models made now, not at the first of each)
  if (src->roads())
    for (int k = 0; k < static_cast<int>(VehicleKind::Count); ++k) (void)vehicle_model(static_cast<VehicleKind>(k));
}

bool Game::load_delta(const std::vector<u8>& bytes) {
  if (!world_.load_delta(bytes)) return false;
  // (its vehicles: from their wheels)
  vehicles_.clear();
  player_vehicle_ = 0;
  player_input_ = VehicleInput{};
  sync_vehicles();
  // (the delta's chunks hold the movers as they were when it was saved)
  for (Mover& m : movers_) set_mover_rows(m, m.rows);
  // (a session that was played had its drops: they are among its pieces)
  if (world_.time() > 0.0) drops_.clear();
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
  if (Pedestrians* p = people()) p->blast(pos, radius, energy);
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
  // what the level drops in when play starts
  if (!drops_.empty()) {
    for (Drop& d : drops_) world_.add_grid(d.desc, std::move(d.voxels));
    drops_.clear();
  }
  // movers first: their voxels are in place when the world's commands and pieces see them
  if (!par_.paused && !movers_.empty()) step_movers();
  using PClock = std::chrono::steady_clock;
  const auto p0 = PClock::now();
  if (!par_.paused) vehicles_before_tick();
  if (!par_.paused) pedestrians_before_tick();
  const auto p1 = PClock::now();
  world_.tick();
  const auto p2 = PClock::now();
  if (!par_.paused) check_movers_hit();
  if (!par_.paused) vehicles_after_tick();
  if (!par_.paused) pedestrians_after_tick();
  const auto p3 = PClock::now();
  for (Fading& f : fading_) f.t += world_.config().dt;
  fading_.erase(std::remove_if(fading_.begin(), fading_.end(), [&](const Fading& f) { return f.t >= fade_time; }), fading_.end());
  drain_world_events();
  const auto p4 = PClock::now();
  if (source_) far_update();
  const auto p5 = PClock::now();
  // (SVX_PROFILE_GAME: the phases of the ticks over 40 ms)
  static const bool gprof = diag("SVX_PROFILE_GAME");
  if (gprof) {
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    if (ms(p0, p5) > 40.0) {
      const WorldStats& w = world_.stats();
      std::printf("  [game] %.0f ms: before %.1f world %.1f (stream %.1f events %.1f rigid %.1f loads %.1f struct %.1f systems %.1f upkeep %.1f) after %.1f drain %.1f far %.1f\n",
                  ms(p0, p5), ms(p0, p1), ms(p1, p2), w.stream_ms, w.event_ms, w.rigid_ms, w.loads_ms, w.structural_ms, w.systems_ms, w.upkeep_ms, ms(p2, p3),
                  ms(p3, p4), ms(p4, p5));
    }
  }
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
    // (the oriented grids' likewise)
    for (GridId gid : world_.grids()) {
      for (u64 k : world_.take_layer_changes(gid, f->burn_layer())) grid_charred_.insert({gid, k});
      world_.take_layer_changes(gid, f->heat_layer());
    }
    for (const GridChunk& c : f->take_grid_glow_changes()) grid_charred_.insert({c.grid, key3(c.chunk[0], c.chunk[1], c.chunk[2])});
    char_clock_ += world_.config().dt;
    if (char_clock_ >= char_remesh_s) {
      char_clock_ = 0.0;
      remesh_.insert(charred_.begin(), charred_.end());
      charred_.clear();
      grid_remesh_.insert(grid_charred_.begin(), grid_charred_.end());
      grid_charred_.clear();
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
        g.occupancy = piece_occupancy(*b);
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
  // (a vehicle's chassis: its body's paint - its mesh is its kind's, re-tinted)
  std::vector<Paint> body(evs.size(), Paint::None);
  for (size_t k : todo)
    for (const auto& [vid, v] : vehicles_)
      if (v.chassis == evs[k].id) body[k] = v.spec.paint;
  std::optional<SerialScope> serial_all;
  if (!mesh_base_.concurrent) serial_all.emplace();  // (providers with unsynchronized caches)
  parallel_for(static_cast<i64>(todo.size()), 4, [&](i64 k0, i64 k1) {
    SerialScope serial;
    for (i64 k = k0; k < k1; ++k) meshes[todo[size_t(k)]] = piece_mesh(*fresh[todo[size_t(k)]], true, body[todo[size_t(k)]]);
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
    g.material = e.material;
    switch (e.kind) {
      case WorldEvent::Kind::PieceAdded:
        if (!fresh[k]) continue;  // (gone again before the harness saw it)
        views_[e.id] = {fresh[k]->x, fresh[k]->q};
        g.kind = GameEvent::Kind::Detached;
        g.pos = fresh[k]->x;
        g.mesh = std::move(meshes[k]);
        g.occupancy = piece_occupancy(*fresh[k]);
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
      case WorldEvent::Kind::JointBroken:
        continue;  // (joints are drawn from their state each frame: it is gone from it)
      case WorldEvent::Kind::GridMoved:
        continue;  // (its new place comes with take_grid_views)
      case WorldEvent::Kind::WheelDetached:
        continue;  // (wheels are drawn from their state each frame; the piece it became comes as a Detached)
      case WorldEvent::Kind::ArticulationAdded:
      case WorldEvent::Kind::ArticulationRemoved:
        continue;  // (the characters' bodies: their system draws them from its own state)
      case WorldEvent::Kind::PieceReshaped: {
        // (crumpled in place: its new mesh, drawn from its pose now on)
        for (auto& [vid, v] : vehicles_)
          if (v.chassis == e.id) ++v.reshapes;
        const Body* b = world_.piece(e.id);
        const auto vt = views_.find(e.id);
        if (!b || vt == views_.end()) continue;
        g.kind = GameEvent::Kind::Remesh;
        g.pos = b->x;
        g.vel = b->v;
        g.ang = b->w;
        g.voxels = b->count;
        g.mesh = piece_mesh(*b);
        g.occupancy = piece_occupancy(*b);
        vt->second = {b->x, b->q};
        break;
      }
      case WorldEvent::Kind::GridRemoved: {
        const GridId id = static_cast<GridId>(e.id);
        for (auto it = grid_meshed_.lower_bound({id, 0}); it != grid_meshed_.end() && it->first == id;) {
          removed_grid_chunks_.push_back(GridChunk{id, unkey3(it->second)});
          it = grid_meshed_.erase(it);
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

ChunkMesh Game::piece_mesh(const Body& b, bool fresh, Paint body) const {
  // each of its shapes (one per grid it came from), in world coordinates at its pose now
  ChunkMesh out;
  for (size_t k = 0; k < b.shapes.size(); ++k) {
    ChunkMesh m = shape_mesh(b, k, fresh, body);
    const u32 base = static_cast<u32>(out.vertices.size());
    out.vertices.insert(out.vertices.end(), m.vertices.begin(), m.vertices.end());
    for (u32 i : m.indices) out.indices.push_back(base + i);
  }
  return out;
}

ChunkMesh Game::shape_mesh(const Body& b, size_t k, bool fresh, Paint body) const {
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
  if (paint_layer_ >= 0 && !S.layer[size_t(paint_layer_)].empty()) {
    // (painted: a car's body, its trim; glowing, it shows its material's glow)
    const int P = paint_layer_;
    auto inner = mo.texture;
    mo.texture = [&S, P, inner](const IVec3& p, int face) -> u16 {
      const u8 c = S.layer_at(P, S.index(p));
      if (c) return static_cast<u16>(kPaintTexture + c);
      return inner ? inner(p, face) : static_cast<u16>(0xFF00 + static_cast<u16>(vox_mat(S.get(p))));
    };
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
  // (a large new piece - a vehicle dropped in - as one meshed before: its voxels and paint the
  // same, nothing burning or glowing, no debug colours)
  bool memo = fresh && S.count >= 1500 && par_.debug_view != 2 && mesh_base_.concurrent && !mesh_base_.light && !mesh_base_.texture;
  if (const FireSystem* f = env_.fire(); memo && f && f->ok())
    memo = S.layer[size_t(f->burn_layer())].empty() && S.layer[size_t(f->heat_layer())].empty();
  static const std::vector<u8> no_paint;
  const std::vector<u8>& paint0 = paint_layer_ >= 0 ? S.layer[size_t(paint_layer_)] : no_paint;
  // (a vehicle's body paint as its kind's placeholder: one mesh per kind, re-tinted)
  constexpr u8 kPlaceholder = static_cast<u8>(Paint::Red);
  const u8 bp = static_cast<u8>(body);
  std::vector<u8> normalized;
  if (memo && bp != 0 && bp != kPlaceholder) {
    normalized = paint0;
    for (u8& x : normalized)
      if (x == bp) x = kPlaceholder;
  }
  const std::vector<u8>& paint = normalized.empty() ? paint0 : normalized;
  const u16 tex_body = static_cast<u16>(kPaintTexture + bp), tex_placeholder = static_cast<u16>(kPaintTexture + kPlaceholder);
  auto retint = [](ChunkMesh& m, u16 from, u16 to) {
    if (from == to) return;
    for (MeshVertex& v : m.vertices)
      if (v.texture == from) v.texture = to;
  };
  u64 key = 0;
  ChunkMesh out;
  bool found = false;
  if (memo) {
    key = 0xCBF29CE484222325ull ^ static_cast<u64>(std::llround(S.h * 1e6));
    for (int a = 0; a < 3; ++a) key = (key ^ static_cast<u64>(static_cast<u32>(S.lo[a]) * 31u + static_cast<u32>(S.dim[a]))) * 0x100000001B3ull;
    for (Vox x : S.vox) key = (key ^ x) * 0x100000001B3ull;
    for (u8 x : paint) key = (key ^ x) * 0x100000001B3ull;
    std::lock_guard<std::mutex> lock(*shape_memo_mu_);
    const auto it = shape_memo_.find(key);
    if (it != shape_memo_.end() && it->second.lo == S.lo && it->second.dim == S.dim && it->second.h == S.h && it->second.vox == S.vox &&
        it->second.paint == paint) {
      out = it->second.mesh;
      found = true;
    }
  }
  if (found && bp != 0) retint(out, tex_placeholder, tex_body);
  if (!found) {
    out = mesh_shape(S, S.h > 0.0 ? S.h : world_.voxel_size(), mo);  // (its own voxel size: a car's are 6.25 cm)
    if (memo) {
      std::lock_guard<std::mutex> lock(*shape_memo_mu_);
      if (shape_memo_.size() >= 48) shape_memo_.clear();  // (a bound: the kinds and paints seen lately)
      ShapeMeshMemo& m = shape_memo_[key];
      m.lo = S.lo;
      m.dim = S.dim;
      m.h = S.h;
      m.vox = S.vox;
      m.paint = paint;
      m.mesh = out;
      if (bp != 0) retint(m.mesh, tex_body, tex_placeholder);
    }
  }
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
  // painted voxels (the paint layer: road markings, facades)
  const VoxelGrid& g = world_.grid();
  if (paint_layer_ >= 0) {
    const int P = paint_layer_;
    auto inner = mo.texture;
    mo.texture = [&g, P, inner](const IVec3& p, int face) -> u16 {
      const u8 c = g.layer(P, p);
      if (c) return static_cast<u16>(kPaintTexture + c);
      return inner ? inner(p, face) : static_cast<u16>(0xFF00 + static_cast<u16>(vox_mat(g.get(p))));
    };
  }
  // charred voxels are darker (the burn layer)
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
  // The oriented grids' chunks: meshed in their lattice (placed in the world by their views,
  // docs/GRIDS.md). Debug views read the grids' fields; materials give their colours, charring
  // darkens them and burning or red-hot voxels glow (as the world's).
  std::vector<GridChunk> gcs = world_.take_changed_grid_chunks();
  for (const auto& [gid, k] : grid_remesh_) gcs.push_back(GridChunk{gid, unkey3(k)});
  grid_remesh_.clear();
  std::sort(gcs.begin(), gcs.end(), [](const GridChunk& a, const GridChunk& b) {
    return a.grid != b.grid ? a.grid < b.grid : key3(a.chunk[0], a.chunk[1], a.chunk[2]) < key3(b.chunk[0], b.chunk[1], b.chunk[2]);
  });
  gcs.erase(std::unique(gcs.begin(), gcs.end(), [](const GridChunk& a, const GridChunk& b) { return a.grid == b.grid && a.chunk == b.chunk; }),
            gcs.end());
  const FireSystem* fire = env_.fire();
  const bool fire_on = fire && fire->ok();
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
        if (!G) continue;
        MeshOptions mg;
        mg.texels_per_metre = base.texels_per_metre;
        const std::vector<u8>& field = gfield[size_t(j)];
        if (!field.empty()) mg.debug = [&field](const IVec3& p) -> u8 { return field[size_t(chunk_index(p))]; };
        const int P = paint_layer_;
        if (fire_on) {
          const int L = fire->burn_layer(), Hl = fire->heat_layer();
          const u8 glow = glow_units(*fire);
          mg.light = [G, L](const IVec3& p, int) -> u8 { return char_light(255, G->layer(L, p)); };
          mg.texture = [G, Hl, glow, P](const IVec3& p, int) -> u16 {
            const u16 m = static_cast<u16>(vox_mat(G->get(p)));
            if (G->layer(Hl, p) >= glow) return static_cast<u16>(0xFE00 + m);
            const u8 c = P >= 0 ? G->layer(P, p) : 0;
            return c ? static_cast<u16>(kPaintTexture + c) : static_cast<u16>(0xFF00 + m);
          };
        } else if (P >= 0) {
          mg.texture = [G, P](const IVec3& p, int) -> u16 {
            const u8 c = G->layer(P, p);
            return c ? static_cast<u16>(kPaintTexture + c) : static_cast<u16>(0xFF00 + static_cast<u16>(vox_mat(G->get(p))));
          };
        }
        ChunkMesh m = mesh_chunk(*G, gc.chunk, mg, false);
        m.chunk = gc.chunk;
        m.grid = gc.grid;
        gm[size_t(j)] = std::move(m);
      }
    });
    for (size_t j = 0; j < gcs.size(); ++j) {
      const GridChunk& gc = gcs[j];
      const std::pair<GridId, u64> key{gc.grid, key3(gc.chunk[0], gc.chunk[1], gc.chunk[2])};
      if (gm[j].vertices.empty()) {
        if (grid_meshed_.erase(key)) removed_grid_chunks_.push_back(gc);
        continue;
      }
      grid_meshed_.insert(key);
      out.push_back(std::move(gm[j]));
    }
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

int Game::chunk_occupancy(const IVec3& cc, u8* bits) const { return grid_chunk_occupancy(kWorldGrid, cc, bits); }

int Game::grid_chunk_occupancy(GridId grid, const IVec3& cc, u8* bits) const {
  const VoxelGrid* G = world_.grid(grid);
  const Chunk* ch = G ? G->chunk(cc) : nullptr;
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

GridView Game::grid_view(GridId id) const {
  GridView v;
  v.id = id;
  GridFrame f;
  if (!world_.grid_frame(id, &f)) return v;
  v.origin = f.origin;
  v.rot = f.rot;
  if (const VoxelGrid* G = world_.grid(id)) v.voxel_size = G->h;
  return v;
}

std::vector<GridView> Game::take_grid_views() {
  // (the grids now against the places the front end has: the new ones and the moved)
  std::vector<GridView> out;
  const std::vector<GridId> ids = world_.grids();
  for (GridId id : ids) {
    const GridView v = grid_view(id);
    const auto it = grid_sent_.find(id);
    auto eq = [](const V3& a, const V3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
    const bool same = it != grid_sent_.end() && eq(it->second.origin, v.origin) && it->second.rot.x == v.rot.x && it->second.rot.y == v.rot.y &&
                      it->second.rot.z == v.rot.z && it->second.rot.w == v.rot.w && it->second.voxel_size == v.voxel_size;
    if (same) continue;
    grid_sent_[id] = v;
    out.push_back(v);
  }
  for (auto it = grid_sent_.begin(); it != grid_sent_.end();) {
    if (!std::binary_search(ids.begin(), ids.end(), it->first)) {
      removed_grids_.push_back(it->first);
      it = grid_sent_.erase(it);
    } else {
      ++it;
    }
  }
  return out;
}

std::vector<GridId> Game::take_removed_grids() {
  std::vector<GridId> out;
  out.swap(removed_grids_);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<JointView> Game::joint_views() const {
  std::vector<JointView> out;
  for (JointId id : world_.joints()) {
    JointState s;
    if (!world_.joint(id, &s)) continue;
    out.push_back(JointView{id, s.type, s.a, s.b});
  }
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

std::vector<u8> piece_occupancy(const Body& b) {
  std::vector<u8> out;
  auto put32 = [&](u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
  };
  auto putf = [&](f64 x) {
    u64 u;
    std::memcpy(&u, &x, 8);
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>(u >> (8 * i)));
  };
  put32(static_cast<u32>(b.shapes.size()));
  for (size_t k = 0; k < b.shapes.size(); ++k) {
    const BodyShape& S = b.shapes[k];
    const V3 origin = b.lattice_to_world(k, V3{});
    const Quat q = b.lattice_rot(k);
    for (f64 x : {origin.x, origin.y, origin.z, q.x, q.y, q.z, q.w, S.h}) putf(x);
    for (int a = 0; a < 3; ++a) put32(static_cast<u32>(S.lo[size_t(a)]));
    for (int a = 0; a < 3; ++a) put32(static_cast<u32>(S.dim[size_t(a)]));
    const size_t base = out.size();
    out.resize(base + (S.vox.size() + 7) / 8, 0);
    for (size_t i = 0; i < S.vox.size(); ++i)
      if (vox_solid(S.vox[i])) out[base + (i >> 3)] = static_cast<u8>(out[base + (i >> 3)] | (1u << (i & 7)));
  }
  return out;
}

std::vector<PiecePose> Game::pieces() const {
  std::vector<PiecePose> out;
  out.reserve(views_.size() + fading_.size());
  for (const PieceState& p : world_.pieces()) {
    const auto it = views_.find(p.id);
    if (it == views_.end()) continue;
    out.push_back({p.id, p.pos, p.rot * conj(it->second.q0), 1.0, p.vel, p.ang});
  }
  for (const Fading& f : fading_) out.push_back({f.id, f.pos, f.rot, std::max(0.0, 1.0 - f.t / fade_time), V3{}, V3{}});
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
  bool any = source_->coarse(lo, n, f, occ);
  if (!any) occ.assign(size_t(n[0]) * size_t(n[1]) * size_t(n[2]), kAir);
  // The source's oriented grids there (docs/GRIDS.md): each voxel's centre makes the coarse cell
  // it falls in solid (their homes: chunks of the tile, or of its surroundings for the grids that
  // reach into it).
  const f64 h = world_.voxel_size();
  constexpr i32 kMargin = 4;  // chunks
  std::vector<u32> seen;
  for (i32 cx = tx * T - kMargin; cx < (tx + 1) * T + kMargin; ++cx)
    for (i32 cy = ty * T - kMargin; cy < (ty + 1) * T + kMargin; ++cy)
      for (i32 cz = clo[2]; cz < chi[2]; ++cz)
        for (const SourceGrid& sg : source_->grids({cx, cy, cz})) {
          if (std::find(seen.begin(), seen.end(), sg.id) != seen.end()) continue;
          seen.push_back(sg.id);
          VoxelGrid gv;
          if (!source_->generate_grid(sg.id, gv)) continue;
          const f64 hg = sg.voxel_size > 0.0 ? sg.voxel_size : h;
          const M3 R = to_matrix(sg.rot);
          for (const auto& [k, ch] : gv.chunks()) {
            if (ch.uniform && !vox_solid(ch.value)) continue;
            const IVec3 gc = unkey3(k);
            for (int i = 0; i < kChunkVox; ++i) {
              const Vox v = ch.uniform ? ch.value : ch.v[size_t(i)];
              if (!vox_solid(v)) continue;
              const IVec3 l{i / (kChunk * kChunk), (i / kChunk) % kChunk, i % kChunk};
              const V3 X = sg.origin + R * V3{hg * (gc[0] * kChunk + l[0]), hg * (gc[1] * kChunk + l[1]), hg * (gc[2] * kChunk + l[2])};
              i32 c[3];
              bool in = true;
              for (int a = 0; a < 3; ++a) {
                const i32 w = static_cast<i32>(std::floor(X[a] / h + 0.5)) - lo[a];
                c[a] = w >= 0 ? w / f : -1;
                in = in && c[a] >= 0 && c[a] < n[a];
              }
              if (!in) continue;
              occ[(size_t(c[0]) * size_t(n[1]) + size_t(c[1])) * size_t(n[2]) + size_t(c[2])] = static_cast<Vox>(v & ~kAnchorBit);
              any = true;
            }
          }
        }
  if (any) m = mesh_coarse(occ, n, lo, f, h);
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
  for (const auto& [id, v] : vehicles_) {
    mix(id);
    mix(bits(v.rpm));
    mix(bits(v.steer));
    mix(static_cast<u64>(static_cast<u32>(v.gear)) | (v.wreck ? 1ull << 40 : 0ull) | (static_cast<u64>(v.flags) << 48));
    mix(v.lane);
    mix(bits(v.input.throttle));
    mix(bits(v.input.brake));
  }
  mix(player_vehicle_);
  for (const Mover& m : movers_) {
    mix(bits(m.level));
    mix(bits(m.timer));
    mix(static_cast<u64>(static_cast<u32>(m.move)) | (static_cast<u64>(m.leg) << 32) | (static_cast<u64>(m.rows) << 40) |
        (m.stopped ? 1ull << 62 : 0ull) | (m.disabled ? 1ull << 63 : 0ull));
  }
  return hsh;
}

}  // namespace svx
