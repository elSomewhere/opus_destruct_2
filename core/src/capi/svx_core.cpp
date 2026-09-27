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
#include "svx/world/world.hpp"

using namespace svx;

struct svxc_world {
  World w;
  f64 h = 0.125;
  std::vector<u8> delta;
  std::vector<WorldEvent> events;
  std::vector<PieceState> pieces;
  std::vector<i32> changed, evicted;
  std::vector<u8> piece_vox;
};

namespace {

// A streamed world's generator: the host's callback.
class CallbackSource final : public ChunkSource {
 public:
  CallbackSource(svxc_generate_fn fn, void* user, const IVec3& lo, const IVec3& hi) : fn_(fn), user_(user), lo_(lo), hi_(hi) {}
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
};

// Tunables by name: a field of WorldConfig (config) or WorldParams (params).
struct Field {
  const char* name;
  f64* (*cfg_f64)(WorldConfig&);
  i32* (*cfg_i32)(WorldConfig&);
  i64* (*cfg_i64)(WorldConfig&);
  bool* (*cfg_bool)(WorldConfig&);
};
#define SVXC_F64(n, m) {n, [](WorldConfig& c) -> f64* { return &c.m; }, nullptr, nullptr, nullptr}
#define SVXC_I32(n, m) {n, nullptr, [](WorldConfig& c) -> i32* { return &c.m; }, nullptr, nullptr}
#define SVXC_INT(n, m) {n, nullptr, [](WorldConfig& c) -> i32* { return reinterpret_cast<i32*>(&c.m); }, nullptr, nullptr}
#define SVXC_I64(n, m) {n, nullptr, nullptr, [](WorldConfig& c) -> i64* { return &c.m; }, nullptr}
#define SVXC_BOOL(n, m) {n, nullptr, nullptr, nullptr, [](WorldConfig& c) -> bool* { return &c.m; }}
static_assert(sizeof(int) == sizeof(i32), "int fields");
const Field kFields[] = {
    SVXC_F64("dt", dt),
    SVXC_F64("stress_rtol", stress_rtol),
    SVXC_I64("stress_work", stress_work),
    SVXC_I32("structure_max_nodes", structure_max_nodes),
    SVXC_I32("cluster_nodes", cluster_nodes),
    SVXC_I32("body_cluster_nodes", body_cluster_nodes),
    SVXC_F64("structure_max_radius", structure_max_radius),
    SVXC_I32("max_breaks_per_round", max_breaks_per_round),
    SVXC_F64("break_band", break_band),
    SVXC_I32("max_rounds", max_rounds),
    SVXC_I32("idle_drop_ticks", idle_drop_ticks),
    SVXC_F64("load_trigger", load_trigger),
    SVXC_F64("load_trigger_abs", load_trigger_abs),
    SVXC_F64("dead_load_ema", dead_load_ema),
    SVXC_I32("max_bodies", max_bodies),
    SVXC_I32("body_stress_maxit", body_stress_maxit),
    SVXC_F64("body_stress_rtol", body_stress_rtol),
    SVXC_F64("body_trigger", body_trigger),
    SVXC_F64("body_impact_speed", body_impact_speed),
    SVXC_F64("small_impact_speed", small_impact_speed),
    SVXC_F64("small_piece_mass", small_piece_mass),
    SVXC_I32("min_fracture_frags", min_fracture_frags),
    SVXC_I32("big_piece_voxels", big_piece_voxels),
    SVXC_I32("impact_rounds", impact_rounds),
    SVXC_F64("impact_chip_fraction", impact_chip_fraction),
    SVXC_F64("impact_round_fraction", impact_round_fraction),
    SVXC_F64("crush_energy", crush_energy),
    SVXC_BOOL("pulverize", pulverize),
    SVXC_BOOL("spread_contacts", spread_contacts),
    SVXC_F64("fracture_energy", fracture_energy),
    SVXC_F64("impact_wave_speed", impact_wave_speed),
    SVXC_I32("body_check_ticks", body_check_ticks),
    SVXC_I32("rollback_part_voxels", rollback_part_voxels),
    SVXC_I32("min_body_voxels", min_body_voxels),
    SVXC_F64("blast_shatter", blast_shatter),
    SVXC_F64("blast_reach", blast_reach),
    SVXC_F64("blast_kinetic", blast_kinetic),
    SVXC_F64("blast_max_speed", blast_max_speed),
    SVXC_F64("max_event_radius", max_event_radius),
    SVXC_F64("design_utilization", design_utilization),
    SVXC_I32("crack_events_per_tick", crack_events_per_tick),
    SVXC_I32("impact_events_per_tick", impact_events_per_tick),
    SVXC_F64("impact_event_energy", impact_event_energy),
    SVXC_I32("frag.min_voxels", frag.min_voxels),
    SVXC_F64("frag.noise_scale", frag.noise_scale),
    SVXC_F64("rigid.gravity", rigid.gravity),
    SVXC_INT("rigid.substeps", rigid.substeps),
    SVXC_INT("rigid.iterations", rigid.iterations),
    SVXC_INT("rigid.position_iterations", rigid.position_iterations),
    SVXC_INT("rigid.busy_bodies", rigid.busy_bodies),
    SVXC_F64("rigid.busy_speed", rigid.busy_speed),
    SVXC_INT("rigid.busy_iterations", rigid.busy_iterations),
    SVXC_F64("rigid.restitution", rigid.restitution),
    SVXC_F64("rigid.bounce_speed", rigid.bounce_speed),
    SVXC_F64("rigid.friction", rigid.friction),
    SVXC_F64("rigid.slop", rigid.slop),
    SVXC_F64("rigid.baumgarte", rigid.baumgarte),
    SVXC_F64("rigid.max_correction", rigid.max_correction),
    SVXC_F64("rigid.max_speed", rigid.max_speed),
    SVXC_F64("rigid.rest_damping", rigid.rest_damping),
    SVXC_F64("rigid.rest_speed", rigid.rest_speed),
    SVXC_F64("rigid.rest_radius", rigid.rest_radius),
    SVXC_F64("rigid.linear_damping", rigid.linear_damping),
    SVXC_F64("rigid.angular_damping", rigid.angular_damping),
    SVXC_F64("rigid.sleep_speed", rigid.sleep_speed),
    SVXC_INT("rigid.sleep_substeps", rigid.sleep_substeps),
    SVXC_INT("rigid.max_points", rigid.max_points),
    SVXC_INT("rigid.manifold", rigid.manifold),
    SVXC_F64("rigid.manifold_per_m", rigid.manifold_per_m),
    SVXC_F64("rigid.kill_depth", rigid.kill_depth),
};
#undef SVXC_F64
#undef SVXC_I32
#undef SVXC_INT
#undef SVXC_I64
#undef SVXC_BOOL

const Field* field(const char* name) {
  if (!name) return nullptr;
  for (const Field& f : kFields)
    if (std::strcmp(f.name, name) == 0) return &f;
  return nullptr;
}

f64* param_field(WorldParams& p, const char* name) {
  if (!name) return nullptr;
  if (!std::strcmp(name, "fragility")) return &p.fragility;
  if (!std::strcmp(name, "impact")) return &p.impact;
  if (!std::strcmp(name, "dif")) return &p.dif;
  return nullptr;
}

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
  WorldParams p = w->w.params();
  if (f64* f = param_field(p, name)) {
    *f = value;
    w->w.set_params(p);
    return 0;
  }
  if (name && !std::strcmp(name, "paused")) {
    p.paused = value != 0.0;
    w->w.set_params(p);
    return 0;
  }
  if (name && !std::strcmp(name, "debug_fields")) {
    p.debug_fields = value != 0.0;
    w->w.set_params(p);
    return 0;
  }
  const Field* f = field(name);
  if (!f) return -1;
  WorldConfig c = w->w.config();
  if (f->cfg_f64) *f->cfg_f64(c) = value;
  if (f->cfg_i32) *f->cfg_i32(c) = static_cast<i32>(std::clamp(value, -2e9, 2e9));
  if (f->cfg_i64) *f->cfg_i64(c) = static_cast<i64>(std::clamp(value, -9e18, 9e18));
  if (f->cfg_bool) *f->cfg_bool(c) = value != 0.0;
  w->w.configure(c);
  return 0;
}

double svxc_get(svxc_world* w, const char* name) {
  if (!w) return NAN;
  WorldParams p = w->w.params();
  if (f64* f = param_field(p, name)) return *f;
  if (name && !std::strcmp(name, "paused")) return p.paused ? 1.0 : 0.0;
  if (name && !std::strcmp(name, "debug_fields")) return p.debug_fields ? 1.0 : 0.0;
  const Field* f = field(name);
  if (!f) return NAN;
  WorldConfig c = w->w.config();
  if (f->cfg_f64) return *f->cfg_f64(c);
  if (f->cfg_i32) return *f->cfg_i32(c);
  if (f->cfg_i64) return static_cast<double>(*f->cfg_i64(c));
  if (f->cfg_bool) return *f->cfg_bool(c) ? 1.0 : 0.0;
  return NAN;
}

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

void svxc_enable_streaming(svxc_world* w, svxc_generate_fn fn, void* user, const int lo[3], const int hi[3], double load_radius,
                           double evict_radius, int chunks_per_tick, double max_resident_mb) {
  if (!w || !fn || !lo || !hi) return;
  VoxelGrid g;
  g.h = w->h;
  w->w.load(std::move(g));
  StreamConfig sc;
  sc.load_radius = load_radius;
  sc.evict_radius = evict_radius;
  sc.chunks_per_tick = chunks_per_tick;
  sc.max_resident_mb = std::isfinite(max_resident_mb) ? std::max(0.0, max_resident_mb) : 0.0;
  w->w.enable_streaming(std::make_shared<CallbackSource>(fn, user, IVec3{lo[0], lo[1], lo[2]}, IVec3{hi[0], hi[1], hi[2]}), sc);
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
}

uint64_t svxc_state_hash(svxc_world* w) { return w ? w->w.state_hash() : 0; }
uint64_t svxc_session_hash(svxc_world* w) { return w ? w->w.session_hash() : 0; }

}  // extern "C"
