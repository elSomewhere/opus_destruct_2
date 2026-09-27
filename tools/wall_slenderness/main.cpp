// svx_wall_slenderness — Phase 0 buckling margin check for the XPBD-look compliance knob.
//
// Softening the physical stiffness by S_p lowers every Euler load by S_p while self-weight
// stays the same (plan §B3; red-team O3). For each shell column that stands free (no ceiling
// slab bracing its top) we estimate the self-weight buckling ratio of the wall strip through
// it, treated as a cantilever column of height H and thickness t:
//     H_cr = (7.837 E t^2 / (12 rho g))^(1/3),   ratio(S_p) = S_p (H / H_cr)^3
// and report which S_p keeps ratio <= 0.3 for (nearly) all free-standing columns.
// The strip model is conservative (walls are plates with braced ends).
//
// usage: svx_wall_slenderness --wad FILE --map NAME [--material concrete|masonry]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "svx/doom/slenderness.hpp"
#include "svx/doom/voxelize.hpp"
#include "svx/doom/wad.hpp"
#include "svx/mech/material.hpp"
#include "svx/world/columns.hpp"

using namespace svx;


int main(int argc, char** argv) {
  std::string wadp, mapn, matn = "concrete";
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    const char* v = i + 1 < argc ? argv[i + 1] : "";
    if (k == "--wad") wadp = v, ++i;
    else if (k == "--map") mapn = v, ++i;
    else if (k == "--material") matn = v, ++i;
  }
  doom::Wad wad;
  std::string err;
  if (wadp.empty() || mapn.empty() || !wad.load(wadp, &err)) {
    std::fprintf(stderr, "usage: svx_wall_slenderness --wad FILE --map NAME [--material concrete|masonry] %s\n", err.c_str());
    return 2;
  }
  doom::Map map;
  if (!wad.read_map(mapn, &map, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  ColumnGrid g;
  doom::VoxelizeStats vs;
  if (!doom::voxelize(map, doom::VoxelizeOptions{}, &g, &vs, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  bool ok = false;
  const Material& M = material(material_from_name(matn.c_str(), &ok));
  const doom::SlendernessReport r = doom::wall_slenderness(g, M, 0.125);
  std::printf("%s %s [%s]: vertical members %lld free-standing, %lld braced | free: H p50=%.2fm max=%.2fm\n",
              wadp.substr(wadp.find_last_of('/') + 1).c_str(), mapn.c_str(), matn.c_str(), (long long)r.free_members,
              (long long)r.braced_members, r.height_p50, r.height_max);
  std::printf("  max S_p with ratio<=0.3: %.1f (p99), %.1f (p99.9), %.1f (all)\n", r.max_compliance_p99,
              r.max_compliance_p999, r.max_compliance_all);
  return 0;
}
