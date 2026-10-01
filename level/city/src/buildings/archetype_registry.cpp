// svx_city — the ARCHETYPES registry (voxel_city world/registry.js).
#include "buildings/archetype_registry.hpp"

namespace svx::city {

const Registry<Archetype>& archetype_registry() { return archetype_registry_mut(); }

Registry<Archetype>& archetype_registry_mut() {
  static Registry<Archetype> r("archetype");
  return r;
}

}  // namespace svx::city
