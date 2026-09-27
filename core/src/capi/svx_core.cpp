// structvox core — the C API (svx/svx_core.h) over svx::World.
#include "svx/svx_core.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "svx/base/parallel.hpp"
#include "svx/material/material.hpp"
#include "svx/world/tunables.hpp"
#include "svx/world/world.hpp"

using namespace svx;

struct svxc_world {
  World w;
  f64 h = 0.125;
  std::vector<u8> delta;
  std::vector<WorldEvent> events;
  std::vector<PieceState> pieces;
  std::vector<i32> changed, evicted, layer_changed;
  std::vector<u8> piece_vox;
};

namespace {

// A streamed world's generator: the host's callback.
class CallbackSource final : public ChunkSource {
 public:
  CallbackSource(svxc_generate_fn fn, void* user, const IVec3& lo, const IVec3& hi, i32 region)
      : fn_(fn), user_(user), lo_(lo), hi_(hi), region_(std::max(1, region)) {}
  u64 region(const IVec3& c) const override {
    auto fdiv = [&](i32 a) { return a >= 0 ? a / region_ : -((-a + region_ - 1) / region_); };
    return key3(fdiv(c[0]), fdiv(c[1]), 0);
  }
  bool generate(const IVec3& c, std::vector<Vox>& out) const override {
    out.assign(kChunkVox, kAir);
    return fn_ && fn_(user_, c[0], c[1], c[2], out.data()) != 0;
  }
  IVec3 chunk_lo() const override { return lo_; }
  IVec3 chunk_hi() const override { return hi_; }

 private:
  svxc_generate_fn fn_;
  void* user_;
  IVec3 lo_, hi_;
  i32 region_;
};

void put3(double* out, const V3& v) {
  out[0] = v.x;
  out[1] = v.y;
  out[2] = v.z;
}
void put4(double* out, const Quat& q) {
  out[0] = q.x;
  out[1] = q.y;
  out[2] = q.z;
  out[3] = q.w;
}

}  // namespace

namespace {

std::vector<LayerEdit> layer_edits(const int32_t* xyz, const uint8_t* values, int n) {
  std::vector<LayerEdit> e(static_cast<size_t>(std::max(0, n)));
  for (int i = 0; i < n; ++i) e[size_t(i)] = {{xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]}, values[i]};
  return e;
}

// A host's system: its callbacks.
class CallbackSystem final : public WorldSystem {
 public:
  CallbackSystem(svxc_world* owner, const svxc_system& s) : owner_(owner), s_(s), name_(s.name ? s.name : "host") {}
  const char* name() const override { return name_.c_str(); }
  void on_load(World&) override {
    if (s_.on_load) s_.on_load(s_.user, owner_);
  }
  void on_generated(World&, const std::vector<u64>& c) override { chunks(SVXC_CHUNKS_GENERATED, c); }
  void on_evicted(World&, const std::vector<u64>& c) override { chunks(SVXC_CHUNKS_EVICTED, c); }
  void on_voxels_changed(World&, const std::vector<u64>& c) override { chunks(SVXC_CHUNKS_CHANGED, c); }
  void step(World&, f64 dt) override { s_.step(s_.user, owner_, dt); }
  i64 memory_bytes() const override { return s_.memory_bytes ? s_.memory_bytes(s_.user) : 0; }
  u64 state_hash() const override { return s_.state_hash ? s_.state_hash(s_.user) : 0; }

 private:
  void chunks(int kind, const std::vector<u64>& c) {
    if (!s_.on_chunks || c.empty()) return;
    std::vector<int32_t> xyz;
    xyz.reserve(3 * c.size());
    for (u64 k : c) {
      const IVec3 p = unkey3(k);
      xyz.insert(xyz.end(), {p[0], p[1], p[2]});
    }
    s_.on_chunks(s_.user, owner_, kind, xyz.data(), static_cast<int>(c.size()));
  }
  svxc_world* owner_;
  svxc_system s_;
  std::string name_;
};

}  // namespace

extern "C" {

// ---- materials

int svxc_material_set(int id, const svxc_material* m) {
  if (!m) return -1;
  Material M;
  M.name = m->name ? m->name : "";
  M.E = m->E;
  M.G = m->G;
  M.rho = m->rho;
  M.ft = m->ft;
  M.fb = m->fb;
  M.fc = m->fc;
  M.cohesion = m->cohesion;
  M.friction = m->friction;
  M.Gf = m->Gf;
  M.frag_x = m->frag[0];
  M.frag_y = m->frag[1];
  M.frag_z = m->frag[2];
  M.frag_noise = m->frag_noise;
  M.indestructible = m->indestructible != 0;
  if (id >= 0) {
    if (id >= kMaxMaterials) return -1;
    set_material(static_cast<MaterialId>(id), M);
    return id;
  }
  MaterialId out;
  return register_material(M, &out) ? static_cast<int>(out) : -1;
}

int svxc_material_get(int id, svxc_material* out) {
  if (!out || id < 0 || id >= kMaxMaterials || !material_registered(static_cast<MaterialId>(id))) return 0;
  const Material& M = material(static_cast<MaterialId>(id));
  out->name = M.name.c_str();
  out->E = M.E;
  out->G = M.G;
  out->rho = M.rho;
  out->ft = M.ft;
  out->fb = M.fb;
  out->fc = M.fc;
  out->cohesion = M.cohesion;
  out->friction = M.friction;
  out->Gf = M.Gf;
  out->frag[0] = M.frag_x;
  out->frag[1] = M.frag_y;
  out->frag[2] = M.frag_z;
  out->frag_noise = M.frag_noise;
  out->indestructible = M.indestructible ? 1 : 0;
  return 1;
}

int svxc_material_find(const char* name) {
  bool ok = false;
  const MaterialId id = material_from_name(name, &ok);
  return ok ? static_cast<int>(id) : -1;
}

void svxc_materials_reset(void) { reset_materials(); }

// ---- worlds

svxc_world* svxc_create(double voxel_size) {
  auto* w = new svxc_world;
  w->h = std::isfinite(voxel_size) && voxel_size >= 0.01 && voxel_size <= 10.0 ? voxel_size : 0.125;
  VoxelGrid g;
  g.h = w->h;
  w->w.load(std::move(g));
  return w;
}

void svxc_destroy(svxc_world* w) { delete w; }

void svxc_set_threads(int threads) { set_num_threads(std::max(1, threads)); }

int svxc_set(svxc_world* w, const char* name, double value) {
  if (!w || !std::isfinite(value)) return -1;
  return set_tunable(w->w, name, value) ? 0 : -1;
}

double svxc_get(svxc_world* w, const char* name) { return w ? get_tunable(w->w, name) : NAN; }

void svxc_load_box(svxc_world* w, const uint8_t* vox, int nx, int ny, int nz, int ox, int oy, int oz) {
  if (!w) return;
  VoxelGrid g;
  g.h = w->h;
  constexpr i32 kLim = 1 << 19;  // (voxel coordinates well within the key range)
  const bool ok = vox && nx > 0 && ny > 0 && nz > 0 && std::abs(ox) < kLim && std::abs(oy) < kLim && std::abs(oz) < kLim &&
                  nx < kLim && ny < kLim && nz < kLim;
  if (ok) {
    for (i32 x = 0; x < nx; ++x)
      for (i32 y = 0; y < ny; ++y) {
        const u8* col = vox + (size_t(x) * size_t(ny) + size_t(y)) * size_t(nz);
        // runs of equal voxels per column
        for (i32 z = 0; z < nz;) {
          i32 e = z + 1;
          while (e < nz && col[e] == col[z]) ++e;
          if (col[z] != kAir) g.fill_column(ox + x, oy + y, oz + z, oz + e, col[z]);
          z = e;
        }
      }
    g.compact();
    g.lo = {ox, oy, oz};
    g.hi = {ox + nx, oy + ny, oz + nz};
  }
  w->w.load(std::move(g));
}

int svxc_bake(svxc_world* w) { return w && w->w.bake() ? 1 : 0; }

svxc_stream svxc_stream_defaults(void) {
  const StreamConfig d;
  svxc_stream c;
  c.load_radius = d.load_radius;
  c.evict_radius = d.evict_radius;
  c.chunks_per_tick = d.chunks_per_tick;
  c.max_resident_mb = d.max_resident_mb;
  c.archive_mb = d.archive_mb;
  c.forget_after_s = d.forget_after_s;
  c.region_chunks = 8;
  return c;
}

void svxc_enable_streaming(svxc_world* w, svxc_generate_fn fn, void* user, const int lo[3], const int hi[3], const svxc_stream* cfg) {
  if (!w || !fn || !lo || !hi) return;
  const svxc_stream c = cfg ? *cfg : svxc_stream_defaults();
  VoxelGrid g;
  g.h = w->h;
  w->w.load(std::move(g));
  StreamConfig sc;
  sc.load_radius = c.load_radius;
  sc.evict_radius = c.evict_radius;
  sc.chunks_per_tick = c.chunks_per_tick;
  sc.max_resident_mb = std::isfinite(c.max_resident_mb) ? std::max(0.0, c.max_resident_mb) : 0.0;
  sc.archive_mb = c.archive_mb;
  sc.forget_after_s = c.forget_after_s;
  w->w.enable_streaming(
      std::make_shared<CallbackSource>(fn, user, IVec3{lo[0], lo[1], lo[2]}, IVec3{hi[0], hi[1], hi[2]}, c.region_chunks), sc);
}

void svxc_set_focus(svxc_world* w, const double* xyz, int count) {
  if (!w || (!xyz && count > 0)) return;
  std::vector<V3> pts;
  for (int i = 0; i < count; ++i) pts.push_back({xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]});
  w->w.set_focus(pts);
}

const uint8_t* svxc_save_delta(svxc_world* w, size_t* size) {
  if (!w) return nullptr;
  w->delta = w->w.save_delta();
  if (size) *size = w->delta.size();
  return w->delta.data();
}

int svxc_load_delta(svxc_world* w, const uint8_t* data, size_t size) {
  if (!w || (!data && size > 0)) return -1;
  return w->w.load_delta(std::vector<u8>(data, data + size)) ? 0 : -1;
}

int svxc_modified(svxc_world* w) { return w && w->w.modified() ? 1 : 0; }

// ---- commands

void svxc_carve(svxc_world* w, double x, double y, double z, double radius) {
  if (w) w->w.carve({x, y, z}, radius);
}

void svxc_blast(svxc_world* w, double x, double y, double z, double radius, double energy) {
  if (w) w->w.blast({x, y, z}, radius, energy);
}

int svxc_set_voxels(svxc_world* w, const int32_t* xyz, const uint8_t* values, int n, unsigned flags) {
  if (!w || n <= 0 || !xyz || !values) return 0;
  std::vector<VoxelEdit> edits(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) edits[size_t(i)] = {{xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]}, values[i]};
  u32 f = 0;
  if (flags & SVXC_EDIT_UNTRACKED) f |= kEditUntracked;
  if (flags & SVXC_EDIT_ISOLATED) f |= kEditIsolated;
  return w->w.set_voxels(edits, f);
}

int svxc_apply_impulse(svxc_world* w, int64_t piece, const double point[3], const double impulse[3]) {
  if (!w || !point || !impulse) return 0;
  return w->w.apply_impulse(piece, {point[0], point[1], point[2]}, {impulse[0], impulse[1], impulse[2]}) ? 1 : 0;
}

int svxc_remove_piece(svxc_world* w, int64_t piece) { return w && w->w.remove_piece(piece) ? 1 : 0; }

int svxc_add_layer(svxc_world* w, const char* name, int persistent, int bind) {
  if (!w || !name || !*name || bind < SVXC_BIND_PLACE || bind > SVXC_BIND_AIR) return -1;
  return w->w.add_layer({name, persistent != 0, static_cast<LayerBind>(bind)});
}

int svxc_layer_index(svxc_world* w, const char* name) { return w && name ? w->w.layer_index(name) : -1; }

int svxc_set_layer(svxc_world* w, int layer, const int32_t* xyz, const uint8_t* values, int n) {
  if (!w || n <= 0 || !xyz || !values) return 0;
  return w->w.set_layer(layer, layer_edits(xyz, values, n));
}

uint8_t svxc_layer(svxc_world* w, int layer, int x, int y, int z) { return w ? w->w.layer(layer, {x, y, z}) : 0; }

int svxc_set_piece_layer(svxc_world* w, int64_t piece, int layer, const int32_t* xyz, const uint8_t* values, int n) {
  if (!w || n <= 0 || !xyz || !values) return 0;
  return w->w.set_piece_layer(piece, layer, layer_edits(xyz, values, n));
}

uint8_t svxc_piece_layer(svxc_world* w, int64_t piece, int layer, int x, int y, int z) {
  return w ? w->w.piece_layer(piece, layer, {x, y, z}) : 0;
}

int svxc_remove_piece_voxels(svxc_world* w, int64_t piece, const int32_t* xyz, int n, int dust) {
  if (!w || n <= 0 || !xyz) return 0;
  std::vector<IVec3> v(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) v[size_t(i)] = {xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]};
  return w->w.remove_piece_voxels(piece, v, dust != 0) ? 1 : 0;
}

void svxc_set_loads(svxc_world* w, uint64_t group, const int32_t* xyz, const double* forces, int n) {
  if (!w || (n > 0 && (!xyz || !forces))) return;
  std::vector<VoxelLoad> loads;
  if (n > 0)
    for (int i = 0; i < n; ++i)
      loads.push_back({{xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]}, {forces[3 * i], forces[3 * i + 1], forces[3 * i + 2]}});
  w->w.set_loads(group, std::move(loads));
}

void svxc_apply_force(svxc_world* w, int64_t piece, const double point[3], const double force[3]) {
  if (w && point && force) w->w.apply_force(piece, {point[0], point[1], point[2]}, {force[0], force[1], force[2]});
}

void svxc_wake_piece(svxc_world* w, int64_t piece) {
  if (w) w->w.wake_piece(piece);
}

void svxc_add_system(svxc_world* w, const char* name, svxc_step_fn step, void* user) {
  svxc_system s{};
  s.name = name;
  s.user = user;
  s.step = step;
  svxc_add_system_ex(w, &s);
}

int svxc_add_system_ex(svxc_world* w, const svxc_system* s) {
  if (!w || !s || !s->step) return 0;
  w->w.add_system(std::make_shared<CallbackSystem>(w, *s));
  return 1;
}

int svxc_chunk_layer(svxc_world* w, int layer, int cx, int cy, int cz, uint8_t* out) {
  if (!out) return 0;
  std::fill(out, out + kChunkVox, u8{0});
  if (!w || layer < 0 || layer >= kMaxLayers) return 0;
  const Chunk* c = w->w.grid().chunk({cx, cy, cz});
  if (!c || c->layer[size_t(layer)].empty()) return 0;
  std::copy(c->layer[size_t(layer)].begin(), c->layer[size_t(layer)].end(), out);
  return 1;
}


void svxc_tick(svxc_world* w) {
  if (w) w->w.tick();
}

// ---- output

int svxc_poll_events(svxc_world* w) {
  if (!w) return 0;
  w->events = w->w.take_events();
  return static_cast<int>(w->events.size());
}

int svxc_event_at(svxc_world* w, int i, svxc_event* out) {
  if (!w || !out || i < 0 || static_cast<size_t>(i) >= w->events.size()) return 0;
  const WorldEvent& e = w->events[size_t(i)];
  *out = svxc_event{};
  out->kind = static_cast<int>(e.kind);
  out->end = static_cast<int>(e.end);
  out->id = e.id;
  out->parent = e.parent;
  put3(out->pos, e.pos);
  put3(out->vel, e.vel);
  put3(out->ang, e.ang);
  put3(out->normal, e.normal);
  put4(out->rot, e.rot);
  out->radius = e.radius;
  out->strength = e.strength;
  out->voxels = e.voxels;
  return 1;
}

int svxc_poll_pieces(svxc_world* w) {
  if (!w) return 0;
  w->pieces = w->w.pieces();
  return static_cast<int>(w->pieces.size());
}

int svxc_piece_at(svxc_world* w, int i, svxc_piece* out) {
  if (!w || !out || i < 0 || static_cast<size_t>(i) >= w->pieces.size()) return 0;
  const PieceState& p = w->pieces[size_t(i)];
  *out = svxc_piece{};
  out->id = p.id;
  put3(out->pos, p.pos);
  put4(out->rot, p.rot);
  put3(out->vel, p.vel);
  put3(out->ang, p.ang);
  if (const Body* b = w->w.piece(p.id)) put3(out->com, b->com);
  out->mass = p.mass;
  out->voxels = p.voxels;
  out->asleep = p.asleep ? 1 : 0;
  return 1;
}

const uint8_t* svxc_piece_voxels(svxc_world* w, int64_t id, int lo[3], int dim[3]) {
  if (!w) return nullptr;
  const Body* b = w->w.piece(id);
  if (!b) return nullptr;
  const BodyShape& S = b->shape;
  if (lo)
    for (int a = 0; a < 3; ++a) lo[a] = S.lo[a];
  if (dim)
    for (int a = 0; a < 3; ++a) dim[a] = S.dim[a];
  w->piece_vox = S.vox;
  return w->piece_vox.data();
}

namespace {
int poll_chunks(std::vector<u64>&& keys, std::vector<i32>& buf, const int32_t** out) {
  buf.clear();
  for (u64 k : keys) {
    const IVec3 c = unkey3(k);
    buf.insert(buf.end(), {c[0], c[1], c[2]});
  }
  if (out) *out = buf.data();
  return static_cast<int>(keys.size());
}
}  // namespace

int svxc_poll_changed_chunks(svxc_world* w, const int32_t** chunks) {
  return w ? poll_chunks(w->w.take_changed_chunks(), w->changed, chunks) : 0;
}

int svxc_poll_evicted_chunks(svxc_world* w, const int32_t** chunks) {
  return w ? poll_chunks(w->w.take_evicted_chunks(), w->evicted, chunks) : 0;
}

int svxc_poll_layer_changes(svxc_world* w, int layer, const int32_t** chunks) {
  if (!w || layer < 0 || layer >= kMaxLayers) {
    if (chunks) *chunks = nullptr;
    return 0;
  }
  return poll_chunks(w->w.take_layer_changes(layer), w->layer_changed, chunks);
}

int svxc_chunk_voxels(svxc_world* w, int cx, int cy, int cz, uint8_t* out) {
  if (!w || !out) return 0;
  const Chunk* c = w->w.grid().chunk({cx, cy, cz});
  if (!c) return 0;
  if (c->uniform) {
    std::fill(out, out + kChunkVox, c->value);
    return 1;
  }
  std::copy(c->v.begin(), c->v.end(), out);
  return 2;
}

// ---- queries

svxc_hit svxc_raycast(svxc_world* w, const double origin[3], const double dir[3], double max_dist) {
  svxc_hit out{};
  if (!w || !origin || !dir) return out;
  const RayHit h = w->w.raycast({origin[0], origin[1], origin[2]}, {dir[0], dir[1], dir[2]}, max_dist);
  out.hit = h.hit ? 1 : 0;
  put3(out.pos, h.pos);
  put3(out.normal, h.normal);
  out.distance = h.distance;
  out.material = h.material;
  for (int a = 0; a < 3; ++a) out.voxel[a] = h.voxel[a];
  out.piece = h.piece;
  return out;
}

void svxc_collide(svxc_world* w, const double mn[3], const double mx[3], const double move[3], double out[4]) {
  if (!out) return;
  out[0] = out[1] = out[2] = out[3] = 0.0;
  if (!w || !mn || !mx || !move) return;
  const CollideResult r = w->w.collide({mn[0], mn[1], mn[2]}, {mx[0], mx[1], mx[2]}, {move[0], move[1], move[2]});
  put3(out, r.move);
  out[3] = r.on_ground ? 1.0 : 0.0;
}

void svxc_get_stats(svxc_world* w, svxc_stats* out) {
  if (!out) return;
  *out = svxc_stats{};
  if (!w) return;
  const WorldStats s = w->w.stats();
  out->tick_ms = s.tick_ms;
  out->structural_ms = s.structural_ms;
  out->rigid_ms = s.rigid_ms;
  out->stream_ms = s.stream_ms;
  out->memory_mb = s.memory_mb;
  out->ticks = s.ticks;
  out->voxels = s.voxels;
  out->structures = s.structures;
  out->pieces = s.bodies;
  out->awake = s.awake;
  out->contacts = s.contacts;
  out->bonds_broken = s.bonds_broken;
  out->detached_pieces = s.detached_pieces;
  out->pulverized_voxels = s.pulverized_voxels;
  out->resident_chunks = s.resident_chunks;
  out->archived_chunks = s.archived_chunks;
  out->forgotten_regions = s.forgotten_regions;
  out->forgotten_chunks = s.forgotten_chunks;
  out->culled_pieces = s.culled_pieces;
  out->dropped_events = s.dropped_events;
  out->archive_used_mb = s.archive_used_mb;
  out->archive_capacity_mb = s.archive_capacity_mb;
}

void svxc_get_memory(svxc_world* w, svxc_memory* out) {
  if (!out) return;
  *out = svxc_memory{};
  if (!w) return;
  const MemoryReport m = w->w.memory();
  out->grid = m.grid;
  out->fragments = m.fragments;
  out->structures = m.structures;
  out->pieces = m.pieces;
  out->archive = m.archive;
  out->caches = m.caches;
  out->queues = m.queues;
  out->total = m.total();
  out->systems = m.systems;
}

uint64_t svxc_state_hash(svxc_world* w) { return w ? w->w.state_hash() : 0; }
uint64_t svxc_session_hash(svxc_world* w) { return w ? w->w.session_hash() : 0; }

}  // extern "C"
