// svx_city tests — trees (voxel_city nature/trees.js) against the reference: their tables and
// many sampled models (stage "trees" of tools/procgen_ref), and the API's lookups.
#include <doctest.h>

#include "records.hpp"
#include "tree_records.hpp"

using namespace svx::city;
using rec::Line;

TEST_CASE("city trees: tables, bounds and models are the reference's (stage trees)") {
  rec::Out out;
  for (const TreeKindSpec& k : tree_kinds()) out << (Line() << "k" << std::string(k.name) << k.h[0] << k.h[1] << k.r[0] << k.r[1] << tree_evergreen(k.kind));
  rec::Samples r(29);
  for (const std::string& kind : trec::kinds())
    for (int i = 0; i < 300; ++i) {
      const double x = std::floor((r() - 0.5) * 100000);
      const double y = std::floor((r() - 0.5) * 100000);
      const double z = std::floor(r() * 2400) - 400;
      // (off the voxel grid now and then)
      const bool off = r() < 0.15;
      const double ox = off ? r() : 0;
      const double oy = off ? r() : 0;
      const Tree t = trec::sample_tree(r, kind, x + ox, y - oy, off ? z + 0.5 : z);
      const TreeModel& m = tree_model(t);
      const Box3 bb = tree_bounds(t);
      uint32_t h = 2166136261u;
      for (const TreePart& p : m.parts) h = trec::fnv(trec::part_line(p).str() + "\n", h);
      Line l;
      l << "t";
      trec::tree_fields(l, kind, t);
      l << static_cast<double>(m.parts.size());
      trec::model_bounds(l, m);
      l << trec::pal_fields(m.pal);
      trec::box_fields(l, bb);
      l << h;
      out << l;
    }
  CHECK(rec::record("trees", out.text()) == rec::recorded_digest("trees"));
}

TEST_CASE("city trees: kinds, lookups and the cached model") {
  CHECK(tree_kinds().size() == 28);
  CHECK(tree_kind("oak") == TreeKind::Oak);
  CHECK(tree_kind("shrubDry") == TreeKind::ShrubDry);
  CHECK(tree_kind("sapling") == TreeKind::Unknown);
  CHECK(tree_kind_name(TreeKind::Larch) == "larch");
  CHECK(tree_kind_name(TreeKind::Unknown).empty());
  REQUIRE(tree_kind_spec("snag"));
  CHECK(tree_kind_spec("snag")->h[1] == 12);
  CHECK(tree_kind_spec("nothing") == nullptr);
  CHECK(&tree_kind_spec(TreeKind::Palm) == tree_kind_spec("palm"));
  CHECK(tree_evergreen(TreeKind::Acacia));
  CHECK(!tree_evergreen(TreeKind::Larch));
  CHECK(!tree_evergreen(TreeKind::Snag));  // (world/season's own list holds it)
  Tree t;
  t.x = 100;
  t.y = -40;
  t.z = 12;
  t.h = 96;
  t.r = 28;
  t.kind = TreeKind::Oak;
  t.seed = 12345;
  const TreeModel& m = tree_model(t);
  CHECK(&tree_model(t) == &m);  // (made once, kept on the tree)
  const TreeModel fresh = build_tree_model(t);
  REQUIRE(fresh.parts.size() == m.parts.size());
  CHECK(fresh.bb.x0 == m.bb.x0);
  CHECK(fresh.bb.z1 == m.bb.z1);
  // foliage first, limbs after
  size_t first_limb = m.parts.size();
  for (size_t i = 0; i < m.parts.size(); ++i)
    if (m.parts[i].k == TreePartKind::Limb && first_limb == m.parts.size()) first_limb = i;
  for (size_t i = first_limb; i < m.parts.size(); ++i) CHECK(m.parts[i].k == TreePartKind::Limb);
  // a copy of a tree makes its model again (the same)
  const Tree u = t;
  CHECK(&tree_model(u) != &m);
  CHECK(tree_model(u).parts.size() == m.parts.size());
  // a kind without a model: its foot
  Tree k = t;
  k.kind = TreeKind::Unknown;
  const Box3 b = tree_bounds(k);
  CHECK((b.x0 == 100 && b.x1 == 100 && b.z0 == 12 && b.z1 == 12));
  CHECK(tree_model(k).parts.empty());
  CHECK(tree_model(k).pal == tree_model(t).pal);  // (the oak's palette)
}
