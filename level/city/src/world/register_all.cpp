// svx_city — the registries filled in the reference's module evaluation order (docs/CITY.md
// §2.2): each module's register_*() is called here, in that order, once.
#include "world/register_all.hpp"

#include <mutex>

#include "buildings/archetypes.hpp"
#include "buildings/civic.hpp"
#include "buildings/styles.hpp"
#include "city/districts.hpp"
#include "city/flavors.hpp"
#include "nature/biomes.hpp"
#include "sites/complex.hpp"
#include "sites/militaryBase.hpp"
#include "sites/mountainBase.hpp"
#include "sites/researchComplex.hpp"
#include "terrain/landforms.hpp"

namespace svx::city {

void register_all() {
  static std::once_flag once;
  std::call_once(once, [] {
    // (each ported module adds its call here, in the order of docs/CITY.md §2.2)
    register_biomes();     // BIOMES (nature/biomes.js)
    register_landforms();  // LANDFORMS (terrain/landforms.js)
    register_districts();  // DISTRICTS (city/districts.js)
    register_flavors();    // FLAVORS (city/flavors.js)
    register_styles();     // STYLES (buildings/styles.js)
    register_civic();      // ARCHETYPES and STYLES (buildings/civic.js), before archetypes.js's
    register_archetypes();      // ARCHETYPES (buildings/archetypes.js)
    register_complex_themes();  // COMPLEX_THEMES (sites/complex.js)
    register_military_base();     // DISTRICTS and SITES (sites/militaryBase.js)
    register_research_complex();  // DISTRICTS and SITES (sites/researchComplex.js)
    register_mountain_base();     // DISTRICTS and SITES (sites/mountainBase.js)
  });
}

}  // namespace svx::city
