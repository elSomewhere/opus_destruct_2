// svx_city tests — the street and industrial prop prefabs (voxel_city city/propPrefabs.js,
// city/industry.js's INDUSTRY_PROPS) against the reference (stage "propprefabs" of
// tools/procgen_ref).
#include <doctest.h>

#include <string>
#include <vector>

#include "city/propPrefabs.hpp"
#include "prefab_records.hpp"
#include "records.hpp"

using namespace svx::city;
using rec::Line;

namespace {

// (an industrial prop always has the sizes it reads: dressing gives them)
PropOpts sample_opts(rec::Samples& r, bool sized) {
  double v[20];
  for (double& x : v) x = r();
  PropOpts o;
  if (sized || v[0] < 0.7) o.h = 8 + std::floor(v[1] * 120);
  if (v[2] < 0.5) o.reach = 3 + std::floor(v[3] * 30);
  if (v[4] < 0.3) o.pole = js::u16(1 + std::floor(v[5] * 399));
  if (sized || v[6] < 0.7) o.r = 2 + std::floor(v[7] * 24);
  if (v[8] < 0.7) o.seed = std::floor(v[9] * 4294967296.0);
  if (sized || v[10] < 0.7) o.n = 1 + std::floor(v[11] * 6);
  if (sized || v[12] < 0.7) o.hw = 8 + std::floor(v[13] * 50);
  if (sized || v[14] < 0.7) o.len = 10 + std::floor(v[15] * 300);
  if (sized || v[16] < 0.7) o.span = 40 + std::floor(v[17] * 160);
  if (v[18] < 0.2) o.plinth = v[19] < 0.5;
  return o;
}

bool is_industry_prop(const char* id) {
  for (const PropDef& d : industry_props())
    if (std::string_view(d.id) == id) return true;
  return false;
}

}  // namespace

TEST_CASE("city prop prefabs: props conform to the reference (stage propprefabs)") {
  rec::Out out;
  {
    Line keys;
    keys << "keys";
    for (const PropDef& d : props()) keys << d.id;
    out << keys;
    Line ind;
    ind << "industry";
    for (const PropDef& d : industry_props()) ind << d.id;
    out << ind;
  }
  rec::Samples r(37);
  for (const PropDef& d : props()) {
    const bool sized = is_industry_prop(d.id);
    for (int i = 0; i < 16; ++i) {
      const PropOpts o = i == 0 && !sized ? PropOpts{} : sample_opts(r, sized);
      Rng rng(std::floor(r() * 4294967296.0));
      const std::vector<PrefabBox> boxes = d.build(rng, o);
      Line l;
      l << "p" << d.id << i << irec::prop_opts(o) << static_cast<double>(boxes.size());
      for (const PrefabBox& q : boxes) l << q.a0 << q.a1 << q.b0 << q.b1 << q.z0 << q.z1 << q.m;
      l << rng.next();
      out << l;
    }
  }
  CHECK(rec::record("propprefabs", out.text()) == rec::recorded_digest("propprefabs"));
}

TEST_CASE("city prop prefabs: lookups by kind") {
  REQUIRE(prop("streetlight"));
  REQUIRE(prop("gantryCrane"));
  CHECK(prop("nothing") == nullptr);
  CHECK(props().size() == 39);
}
