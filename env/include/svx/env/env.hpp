// structvox env — the environment systems of a World, together (docs/ENV.md).
//
// Each system is a WorldSystem of its own (fire.hpp, smoke.hpp, ...) and can be used alone;
// Environment makes the usual set, adds them to a world and links them: fire feeds smoke,
// water puts out fire. Hosts that want other combinations add the systems themselves.
#pragma once

#include <memory>

#include "svx/env/fire.hpp"
#include "svx/env/smoke.hpp"

namespace svx {

struct EnvConfig {
  bool fire = true, smoke = true;
  FireConfig fire_config;
  SmokeConfig smoke_config;
};

class Environment {
 public:
  // Adds the configured systems to w (once, before its first load).
  void attach(World& w, const EnvConfig& c = {});
  FireSystem* fire() const { return fire_.get(); }
  SmokeSystem* smoke() const { return smoke_.get(); }

 private:
  std::shared_ptr<FireSystem> fire_;
  std::shared_ptr<SmokeSystem> smoke_;
};

}  // namespace svx
