#include "svx/env/env.hpp"

namespace svx {

void Environment::attach(World& w, const EnvConfig& c) {
  if (c.fire) {
    fire_ = std::make_shared<FireSystem>(c.fire_config);
    w.add_system(fire_);
  }
}

}  // namespace svx
