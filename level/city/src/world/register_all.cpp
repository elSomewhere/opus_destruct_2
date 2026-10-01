// svx_city — the registries filled in the reference's module evaluation order (docs/CITY.md
// §2.2): each module's register_*() is called here, in that order, once.
#include "world/register_all.hpp"

#include <mutex>

#include "buildings/civic.hpp"
#include "buildings/styles.hpp"
#include "city/districts.hpp"
#include "city/flavors.hpp"

namespace svx::city {

void register_all() {
  static std::once_flag once;
  std::call_once(once, [] {
    // (each ported module adds its call here, in the order of docs/CITY.md §2.2)
    register_districts();  // DISTRICTS (city/districts.js)
    register_flavors();    // FLAVORS (city/flavors.js)
    register_styles();     // STYLES (buildings/styles.js)
    register_civic();      // ARCHETYPES and STYLES (buildings/civic.js), before archetypes.js's
  });
}

}  // namespace svx::city
