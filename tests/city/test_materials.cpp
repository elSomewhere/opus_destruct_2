// svx_city tests — the voxel material palette (voxel_city voxel/materials.js) against the
// reference (stage "materials").
#include <doctest.h>

#include "records.hpp"
#include "voxel/materials.hpp"

using namespace svx::city;
using rec::Line;

TEST_CASE("city materials: the palette is the reference's (stage materials)") {
  rec::Out out;
  for (int id = 0; id < kMaterialCount; ++id) {
    const MaterialInfo& m = material_info(id);
    out << (Line() << m.id << m.name << std::vector<double>{double(m.rgb[0]), double(m.rgb[1]), double(m.rgb[2])} << m.noise << m.transparent << m.opacity
                   << m.emissive << m.solid << m.climb << m.glow << material_id(m.name) << (is_transparent(id) ? 1 : 0) << (is_solid(id) ? 1 : 0)
                   << (is_climb(id) ? 1 : 0));
  }
  CHECK(rec::record("materials", out.text()) == rec::recorded_digest("materials"));
  CHECK(material_id("NOT_A_MATERIAL") == -1);
  CHECK(MAT::GRASS == material_id("GRASS"));
}
