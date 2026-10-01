// svx_city tests — tree models part by part (voxel_city nature/trees.js treeModel) against the
// reference (stage "treemodels" of tools/procgen_ref).
#include <doctest.h>

#include "records.hpp"
#include "tree_records.hpp"

using namespace svx::city;
using rec::Line;

TEST_CASE("city treemodels: every part of the models is the reference's (stage treemodels)") {
  rec::Out out;
  rec::Samples r(31);
  for (const std::string& kind : trec::kinds())
    for (int i = 0; i < 24; ++i) {
      const double x = std::floor((r() - 0.5) * 100000);
      const double y = std::floor((r() - 0.5) * 100000);
      const double z = std::floor(r() * 2400) - 400;
      const bool off = r() < 0.15;
      const double ox = off ? r() : 0;
      const double oy = off ? r() : 0;
      const Tree t = trec::sample_tree(r, kind, x + ox, y - oy, off ? z + 0.5 : z);
      const TreeModel& m = tree_model(t);
      const Box3 bb = tree_bounds(t);
      Line l;
      l << "t";
      trec::tree_fields(l, kind, t);
      l << static_cast<double>(m.parts.size());
      trec::model_bounds(l, m);
      l << trec::pal_fields(m.pal);
      trec::box_fields(l, bb);
      out << l;
      for (const TreePart& p : m.parts) out << trec::part_line(p);
    }
  CHECK(rec::record("treemodels", out.text()) == rec::recorded_digest("treemodels"));
}
