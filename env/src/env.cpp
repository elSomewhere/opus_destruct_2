#include "svx/env/env.hpp"

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

}  // namespace svx
