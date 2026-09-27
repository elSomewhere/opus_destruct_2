#include "svx/env/env.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace svx {

void Environment::attach(World& w, const EnvConfig& c) {
  // (in this order: the water moves first, the fire sees it; the fire's flames of this tick
  // are the smoke's sources)
  if (c.water) {
    water_ = std::make_shared<WaterSystem>(c.water_config);
    w.add_system(water_);
  }
  if (c.fire) {
    fire_ = std::make_shared<FireSystem>(c.fire_config);
    w.add_system(fire_);
  }
  if (c.smoke) {
    smoke_ = std::make_shared<SmokeSystem>(c.smoke_config);
    smoke_->follow(fire_);
    w.add_system(smoke_);
  }
}

namespace {

struct Param {
  EnvParamInfo info;
  f64 (*get)(const Environment&);
  void (*set)(Environment&, f64);
};

// (one setting of a system's config: read it, change it, configure the system)
#define SVX_FIRE(n, field, lo, hi)                                                        \
  {{"fire." n, lo, hi},                                                                   \
   [](const Environment& e) { return e.fire() ? static_cast<f64>(e.fire()->config().field) : std::nan(""); }, \
   [](Environment& e, f64 v) {                                                            \
     if (!e.fire()) return;                                                               \
     FireConfig c = e.fire()->config();                                                   \
     c.field = static_cast<decltype(c.field)>(v);                                         \
     e.fire()->configure(c);                                                              \
   }}
#define SVX_FIRE_BOOL(n, field)                                                           \
  {{"fire." n, 0, 1},                                                                     \
   [](const Environment& e) { return e.fire() ? (e.fire()->config().field ? 1.0 : 0.0) : std::nan(""); }, \
   [](Environment& e, f64 v) {                                                            \
     if (!e.fire()) return;                                                               \
     FireConfig c = e.fire()->config();                                                   \
     c.field = v != 0.0;                                                                  \
     e.fire()->configure(c);                                                              \
   }}
#define SVX_MAT(n, mat, field, lo, hi)                                                    \
  {{"fire." n, lo, hi},                                                                   \
   [](const Environment& e) { return e.fire() ? e.fire()->fire_material(MaterialId::mat).field : std::nan(""); }, \
   [](Environment& e, f64 v) {                                                            \
     if (!e.fire()) return;                                                               \
     FireMaterial m = e.fire()->fire_material(MaterialId::mat);                           \
     m.field = v;                                                                         \
     e.fire()->set_material(MaterialId::mat, m);                                          \
   }}
#define SVX_SMOKE(n, field, lo, hi)                                                       \
  {{"smoke." n, lo, hi},                                                                  \
   [](const Environment& e) { return e.smoke() ? static_cast<f64>(e.smoke()->config().field) : std::nan(""); }, \
   [](Environment& e, f64 v) {                                                            \
     if (!e.smoke()) return;                                                              \
     SmokeConfig c = e.smoke()->config();                                                 \
     c.field = static_cast<decltype(c.field)>(v);                                         \
     e.smoke()->configure(c);                                                             \
   }}
#define SVX_WATER(n, field, lo, hi)                                                       \
  {{"water." n, lo, hi},                                                                  \
   [](const Environment& e) { return e.water() ? static_cast<f64>(e.water()->config().field) : std::nan(""); }, \
   [](Environment& e, f64 v) {                                                            \
     if (!e.water()) return;                                                              \
     WaterConfig c = e.water()->config();                                                 \
     c.field = static_cast<decltype(c.field)>(v);                                         \
     e.water()->configure(c);                                                             \
   }}
#define SVX_WATER_BOOL(n, field)                                                          \
  {{"water." n, 0, 1},                                                                    \
   [](const Environment& e) { return e.water() ? (e.water()->config().field ? 1.0 : 0.0) : std::nan(""); }, \
   [](Environment& e, f64 v) {                                                            \
     if (!e.water()) return;                                                              \
     WaterConfig c = e.water()->config();                                                 \
     c.field = v != 0.0;                                                                  \
     e.water()->configure(c);                                                             \
   }}

const Param kParams[] = {
    SVX_FIRE_BOOL("enabled", enabled),
    SVX_FIRE("step_s", step_s, 0.0, 0.25),
    SVX_FIRE("flame_reach", flame_reach, 0.0, 2.0),
    SVX_FIRE("max_hot", max_hot, 0, 1e6),
    SVX_FIRE("ignition_jitter", ignition_jitter, 0.0, 0.9),
    SVX_FIRE("burn_jitter", burn_jitter, 0.0, 0.9),
    SVX_FIRE("quench_c", quench_c, 0.0, 1020.0),
    SVX_FIRE("damage_quantum", damage_quantum, 1, 64),
    SVX_FIRE("piece_batch_steps", piece_batch_steps, 1, 100),
    SVX_FIRE("glow_c", glow_c, 0.0, 1021.0),
    SVX_MAT("wood.ignition_c", Wood, ignition_c, 100.0, 1000.0),
    SVX_MAT("wood.burn_s", Wood, burn_s, 1.0, 600.0),
    SVX_MAT("wood.flame_c", Wood, flame_c, 300.0, 1020.0),
    SVX_MAT("wood.char_damage", Wood, char_damage, 0.0, 1.0),
    SVX_MAT("steel.weaken_c", Steel, weaken_c, 0.0, 1020.0),
    SVX_MAT("steel.gone_c", Steel, gone_c, 0.0, 2000.0),
    SVX_MAT("concrete.weaken_c", Concrete, weaken_c, 0.0, 1020.0),
    SVX_MAT("concrete.gone_c", Concrete, gone_c, 0.0, 3000.0),
    {{"smoke.enabled", 0, 1},
     [](const Environment& e) { return e.smoke() ? (e.smoke()->config().enabled ? 1.0 : 0.0) : std::nan(""); },
     [](Environment& e, f64 v) {
       if (!e.smoke()) return;
       SmokeConfig c = e.smoke()->config();
       c.enabled = v != 0.0;
       e.smoke()->configure(c);
     }},
    SVX_SMOKE("step_s", step_s, 0.0, 0.5),
    SVX_SMOKE("rise", rise, 0.0, 5.0),
    SVX_SMOKE("diffuse", diffuse, 0.0, 5.0),
    SVX_SMOKE("lifetime", lifetime, 0.5, 300.0),
    SVX_SMOKE("wind_x", wind.x, -20.0, 20.0),
    SVX_SMOKE("wind_y", wind.y, -20.0, 20.0),
    SVX_SMOKE("wind_z", wind.z, -5.0, 5.0),
    SVX_SMOKE("per_flame", per_flame, 0.0, 2.0),
    SVX_SMOKE("ceiling_m", ceiling_m, 0.0, 200.0),
    SVX_SMOKE("max_blocks", max_blocks, 0, 16384),
    SVX_WATER_BOOL("enabled", enabled),
    SVX_WATER("step_s", step_s, 0.0, 0.1),
    SVX_WATER("fall", fall, 1, 16),
    SVX_WATER("min_spread", min_spread, 1, 128),
    SVX_WATER("min_amount", min_amount, 1, 64),
    SVX_WATER("max_active", max_active, 1000, 1e6),
    SVX_WATER("density", density, 100.0, 5000.0),
    SVX_WATER_BOOL("loads", loads),
    SVX_WATER("load_s", load_s, 0.0, 5.0),
    SVX_WATER("max_loads", max_loads, 0, 1e6),
    SVX_WATER_BOOL("buoyancy", buoyancy),
    SVX_WATER("drag", drag, 0.0, 20.0),
    SVX_WATER("samples", samples, 8, 4096),
};
#undef SVX_FIRE
#undef SVX_FIRE_BOOL
#undef SVX_MAT
#undef SVX_SMOKE
#undef SVX_WATER
#undef SVX_WATER_BOOL
constexpr i32 kCount = static_cast<i32>(sizeof(kParams) / sizeof(kParams[0]));

}  // namespace

i32 env_param_count() { return kCount; }

const EnvParamInfo* env_param(i32 i) { return i >= 0 && i < kCount ? &kParams[i].info : nullptr; }

i32 env_param_index(const char* name) {
  if (!name) return -1;
  for (i32 i = 0; i < kCount; ++i)
    if (std::strcmp(kParams[i].info.name, name) == 0) return i;
  return -1;
}

bool Environment::set(i32 i, f64 value) {
  if (i < 0 || i >= kCount || !std::isfinite(value)) return false;
  if (std::isnan(kParams[i].get(*this))) return false;  // (its system is not there)
  // (within the parameter's range first: integer fields take no value beyond theirs)
  kParams[i].set(*this, std::clamp(value, kParams[i].info.min, kParams[i].info.max));
  return true;
}

f64 Environment::get(i32 i) const { return i >= 0 && i < kCount ? kParams[i].get(*this) : std::nan(""); }

}  // namespace svx
