// structvox env — the environment systems of a World, together (docs/ENV.md).
//
// Each system is a WorldSystem of its own (fire.hpp, smoke.hpp, water.hpp) and can be used alone;
// Environment makes the usual set, adds them to a world and links them: fire feeds smoke,
// water puts out fire. Hosts that want other combinations add the systems themselves.
#pragma once

#include <memory>

#include "svx/env/fire.hpp"
#include "svx/env/smoke.hpp"
#include "svx/env/water.hpp"

namespace svx {

struct EnvConfig {
  bool fire = true, smoke = true, water = true;
  FireConfig fire_config;
  SmokeConfig smoke_config;
  WaterConfig water_config;
};

// The environment's settings by name - "fire.flame_reach", "smoke.wind_x", "water.loads",
// "fire.wood.burn_s", ...: every field of the systems' configs, and the fire properties of the
// main materials - for hosts, settings UIs and command logs (an index is stable within a build;
// values are brought into their ranges).
struct EnvParamInfo {
  const char* name;
  f64 min, max;  // (the range a UI offers; the systems clamp to their own)
};
i32 env_param_count();
const EnvParamInfo* env_param(i32 index);  // nullptr: out of range
i32 env_param_index(const char* name);     // -1: unknown

class Environment {
 public:
  // Adds the configured systems to w (once, before its first load).
  void attach(World& w, const EnvConfig& c = {});
  FireSystem* fire() { return fire_.get(); }
  SmokeSystem* smoke() { return smoke_.get(); }
  WaterSystem* water() { return water_.get(); }
  const FireSystem* fire() const { return fire_.get(); }
  const SmokeSystem* smoke() const { return smoke_.get(); }
  const WaterSystem* water() const { return water_.get(); }

  // Settings by name or index (env_param_*): false / NaN for an unknown one, or a system that
  // is not there.
  bool set(i32 index, f64 value);
  f64 get(i32 index) const;
  bool set(const char* name, f64 value) { return set(env_param_index(name), value); }
  f64 get(const char* name) const { return get(env_param_index(name)); }

 private:
  std::shared_ptr<FireSystem> fire_;
  std::shared_ptr<SmokeSystem> smoke_;
  std::shared_ptr<WaterSystem> water_;
};

}  // namespace svx
