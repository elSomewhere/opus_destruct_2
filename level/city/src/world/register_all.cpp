// svx_city — the registries filled in the reference's module evaluation order (docs/CITY.md
// §2.2): each module's register_*() is called here, in that order, once.
#include "world/register_all.hpp"

#include <mutex>

#include "nature/biomes.hpp"

namespace svx::city {

void register_all() {
  static std::once_flag once;
  std::call_once(once, [] {
    // (each ported module adds its call here, in the order of docs/CITY.md §2.2)
    register_biomes();     // BIOMES (nature/biomes.js)
  });
}

}  // namespace svx::city
