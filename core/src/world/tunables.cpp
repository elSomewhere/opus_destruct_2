// structvox core — the world's tunables by name (svx/world/tunables.hpp).
#include "svx/world/tunables.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace svx {

namespace {

// WorldParams first (runtime knobs), then WorldConfig's fields.
constexpr TunableInfo kParams[] = {{"fragility", false}, {"impact", false}, {"dif", false}, {"paused", false}, {"debug_fields", false}};
constexpr i32 kNumParams = static_cast<i32>(sizeof(kParams) / sizeof(kParams[0]));

struct Field {
  const char* name;
  bool setup;
  f64* (*cfg_f64)(WorldConfig&);
  i32* (*cfg_i32)(WorldConfig&);
  i64* (*cfg_i64)(WorldConfig&);
  bool* (*cfg_bool)(WorldConfig&);
};
#define SVX_T_F64(n, m) {n, false, [](WorldConfig& c) -> f64* { return &c.m; }, nullptr, nullptr, nullptr}
#define SVX_T_I32(n, m) {n, false, nullptr, [](WorldConfig& c) -> i32* { return &c.m; }, nullptr, nullptr}
#define SVX_T_INT(n, m) {n, false, nullptr, [](WorldConfig& c) -> i32* { return reinterpret_cast<i32*>(&c.m); }, nullptr, nullptr}
#define SVX_T_I64(n, m) {n, false, nullptr, nullptr, [](WorldConfig& c) -> i64* { return &c.m; }, nullptr}
#define SVX_T_BOOL(n, m) {n, false, nullptr, nullptr, nullptr, [](WorldConfig& c) -> bool* { return &c.m; }}
static_assert(sizeof(int) == sizeof(i32), "int fields");
const Field kFields[] = {
    {"dt", true, [](WorldConfig& c) -> f64* { return &c.dt; }, nullptr, nullptr, nullptr},
    SVX_T_F64("stress_rtol", stress_rtol),
    SVX_T_I64("stress_work", stress_work),
    SVX_T_I32("structure_max_nodes", structure_max_nodes),
    SVX_T_I32("cluster_nodes", cluster_nodes),
    SVX_T_I32("body_cluster_nodes", body_cluster_nodes),
    SVX_T_F64("structure_max_radius", structure_max_radius),
    SVX_T_I32("max_breaks_per_round", max_breaks_per_round),
    SVX_T_F64("break_band", break_band),
    SVX_T_I32("max_rounds", max_rounds),
    SVX_T_I32("solve_restarts", solve_restarts),
    SVX_T_I32("idle_drop_ticks", idle_drop_ticks),
    SVX_T_F64("load_trigger", load_trigger),
    SVX_T_F64("load_trigger_abs", load_trigger_abs),
    SVX_T_F64("dead_load_ema", dead_load_ema),
    SVX_T_I32("max_bodies", max_bodies),
    SVX_T_I32("body_stress_maxit", body_stress_maxit),
    SVX_T_F64("body_stress_rtol", body_stress_rtol),
    SVX_T_F64("body_trigger", body_trigger),
    SVX_T_F64("body_impact_speed", body_impact_speed),
    SVX_T_F64("small_impact_speed", small_impact_speed),
    SVX_T_F64("small_piece_mass", small_piece_mass),
    SVX_T_I32("min_fracture_frags", min_fracture_frags),
    SVX_T_I32("big_piece_voxels", big_piece_voxels),
    SVX_T_I32("impact_rounds", impact_rounds),
    SVX_T_F64("impact_chip_fraction", impact_chip_fraction),
    SVX_T_F64("impact_round_fraction", impact_round_fraction),
    SVX_T_F64("crush_energy", crush_energy),
    SVX_T_BOOL("pulverize", pulverize),
    SVX_T_BOOL("spread_contacts", spread_contacts),
    SVX_T_F64("fracture_energy", fracture_energy),
    SVX_T_F64("impact_wave_speed", impact_wave_speed),
    SVX_T_I32("body_check_ticks", body_check_ticks),
    SVX_T_I32("rollback_part_voxels", rollback_part_voxels),
    SVX_T_I32("min_body_voxels", min_body_voxels),
    SVX_T_F64("blast_shatter", blast_shatter),
    SVX_T_F64("blast_reach", blast_reach),
    SVX_T_F64("blast_kinetic", blast_kinetic),
    SVX_T_F64("blast_max_speed", blast_max_speed),
    SVX_T_F64("max_event_radius", max_event_radius),
    SVX_T_F64("design_utilization", design_utilization),
    {"junction_samples", true, nullptr, [](WorldConfig& c) -> i32* { return &c.junction_samples; }, nullptr, nullptr},
    {"junction_reach", true, [](WorldConfig& c) -> f64* { return &c.junction_reach; }, nullptr, nullptr, nullptr},
    SVX_T_I32("crack_events_per_tick", crack_events_per_tick),
    SVX_T_I32("impact_events_per_tick", impact_events_per_tick),
    SVX_T_F64("impact_event_energy", impact_event_energy),
    {"frag.min_voxels", true, nullptr, [](WorldConfig& c) -> i32* { return &c.frag.min_voxels; }, nullptr, nullptr},
    {"frag.noise_scale", true, [](WorldConfig& c) -> f64* { return &c.frag.noise_scale; }, nullptr, nullptr, nullptr},
    SVX_T_F64("rigid.gravity", rigid.gravity),
    SVX_T_INT("rigid.substeps", rigid.substeps),
    SVX_T_INT("rigid.iterations", rigid.iterations),
    SVX_T_INT("rigid.position_iterations", rigid.position_iterations),
    SVX_T_INT("rigid.busy_bodies", rigid.busy_bodies),
    SVX_T_F64("rigid.busy_speed", rigid.busy_speed),
    SVX_T_INT("rigid.busy_iterations", rigid.busy_iterations),
    SVX_T_F64("rigid.restitution", rigid.restitution),
    SVX_T_F64("rigid.bounce_speed", rigid.bounce_speed),
    SVX_T_F64("rigid.friction", rigid.friction),
    SVX_T_F64("rigid.slop", rigid.slop),
    SVX_T_F64("rigid.baumgarte", rigid.baumgarte),
    SVX_T_F64("rigid.max_correction", rigid.max_correction),
    SVX_T_F64("rigid.max_speed", rigid.max_speed),
    SVX_T_F64("rigid.rest_damping", rigid.rest_damping),
    SVX_T_F64("rigid.rest_speed", rigid.rest_speed),
    SVX_T_F64("rigid.rest_radius", rigid.rest_radius),
    SVX_T_F64("rigid.linear_damping", rigid.linear_damping),
    SVX_T_F64("rigid.angular_damping", rigid.angular_damping),
    SVX_T_F64("rigid.sleep_speed", rigid.sleep_speed),
    SVX_T_INT("rigid.sleep_substeps", rigid.sleep_substeps),
    SVX_T_INT("rigid.max_points", rigid.max_points),
    SVX_T_INT("rigid.manifold", rigid.manifold),
    SVX_T_F64("rigid.manifold_per_m", rigid.manifold_per_m),
    SVX_T_F64("rigid.kill_depth", rigid.kill_depth),
    SVX_T_BOOL("rigid.speculative", rigid.speculative),
    SVX_T_INT("rigid.speculative_contacts", rigid.speculative_contacts),
    SVX_T_F64("rigid.joint_baumgarte", rigid.joint_baumgarte),
    SVX_T_F64("rigid.joint_slop", rigid.joint_slop),
    SVX_T_F64("rigid.joint_warm", rigid.joint_warm),
    SVX_T_F64("memory.fragment_cache_mb", memory.fragment_cache_mb),
    SVX_T_F64("memory.structure_mb", memory.structure_mb),
    SVX_T_F64("memory.piece_mb", memory.piece_mb),
    SVX_T_F64("memory.cache_mb", memory.cache_mb),
    SVX_T_I32("memory.max_events", memory.max_events),
};
#undef SVX_T_F64
#undef SVX_T_I32
#undef SVX_T_INT
#undef SVX_T_I64
#undef SVX_T_BOOL



constexpr i32 kNumFields = static_cast<i32>(sizeof(kFields) / sizeof(kFields[0]));
TunableInfo g_info[kNumFields];
bool g_info_ready = [] {
  for (i32 i = 0; i < kNumFields; ++i) g_info[i] = {kFields[i].name, kFields[i].setup};
  return true;
}();

}  // namespace

i32 tunable_count() { return kNumParams + kNumFields; }

const TunableInfo* tunable(i32 i) {
  if (i < 0 || i >= tunable_count()) return nullptr;
  return i < kNumParams ? &kParams[i] : &g_info[i - kNumParams];
}

i32 tunable_index(const char* name) {
  if (!name) return -1;
  for (i32 i = 0; i < tunable_count(); ++i)
    if (std::strcmp(tunable(i)->name, name) == 0) return i;
  return -1;
}

bool set_tunable(World& w, i32 i, f64 value) {
  if (i < 0 || i >= tunable_count() || !std::isfinite(value)) return false;
  if (i < kNumParams) {
    WorldParams p = w.params();
    switch (i) {
      case 0: p.fragility = value; break;
      case 1: p.impact = value; break;
      case 2: p.dif = value; break;
      case 3: p.paused = value != 0.0; break;
      default: p.debug_fields = value != 0.0; break;
    }
    w.set_params(p);
    return true;
  }
  const Field& f = kFields[i - kNumParams];
  WorldConfig c = w.config();
  if (f.cfg_f64) *f.cfg_f64(c) = value;
  if (f.cfg_i32) *f.cfg_i32(c) = static_cast<i32>(std::clamp(value, -2e9, 2e9));
  if (f.cfg_i64) *f.cfg_i64(c) = static_cast<i64>(std::clamp(value, -9e18, 9e18));
  if (f.cfg_bool) *f.cfg_bool(c) = value != 0.0;
  w.configure(c);
  return true;
}

f64 get_tunable(const World& w, i32 i) {
  if (i < 0 || i >= tunable_count()) return std::nan("");
  if (i < kNumParams) {
    const WorldParams& p = w.params();
    switch (i) {
      case 0: return p.fragility;
      case 1: return p.impact;
      case 2: return p.dif;
      case 3: return p.paused ? 1.0 : 0.0;
      default: return p.debug_fields ? 1.0 : 0.0;
    }
  }
  const Field& f = kFields[i - kNumParams];
  WorldConfig c = w.config();
  if (f.cfg_f64) return *f.cfg_f64(c);
  if (f.cfg_i32) return *f.cfg_i32(c);
  if (f.cfg_i64) return static_cast<f64>(*f.cfg_i64(c));
  if (f.cfg_bool) return *f.cfg_bool(c) ? 1.0 : 0.0;
  return std::nan("");
}

}  // namespace svx
