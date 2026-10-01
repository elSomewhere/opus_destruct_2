// svx_city tests — the export's material classes (voxel_city svx/materials.js) against the
// reference (stage "svxmat" of tools/procgen_ref).
#include <doctest.h>

#include "records.hpp"
#include "svx/materials.hpp"
#include "voxel/materials.hpp"

using namespace svx::city;
using rec::Line;

namespace {
// (-1: JS's undefined)
Line& opt(Line& l, int v) {
  if (v < 0)
    l << rec::kUndef;
  else
    l << v;
  return l;
}
}  // namespace

TEST_CASE("city svx materials: the classes are the reference's (stage svxmat)") {
  rec::Out out;
  out << (Line() << "base" << kCityBase << svx_classes().size());
  for (const SvxClass& c : svx_classes()) out << (Line() << "class" << c.name << c.id << c.own.has_value() << svx_class(c.name)->id);
  for (int id = 0; id < kMaterialCount; ++id) {
    const SvxClassify& e = svx_classify(id);
    Line l;
    l << "m" << id << material_info(id).name << e.air << e.liquid << e.flora;
    opt(l, e.s);
    opt(l, e.g);
    opt(l, e.s_look);
    opt(l, e.g_look);
    opt(l, e.flora_idx);
    out << l;
  }
  for (const int id : svx_look_classes()) out << (Line() << "looks" << id << svx_looks(id));
  out << (Line() << "flora" << svx_flora());
  for (const SvxClass& c : svx_classes()) out << (Line() << "vox" << c.id << vox(c.id, false) << vox(c.id, true));
  std::string text = out.text();
  text += svx_materials().json();
  text += '\n';
  CHECK(rec::record("svxmat", text) == rec::recorded_digest("svxmat"));
  CHECK(svx_class("nothing") == nullptr);
  CHECK(svx_classify(MAT::WATER).liquid);
  CHECK(svx_classify(MAT::AIR).air);
}
