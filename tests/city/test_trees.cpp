// svx_city tests — trees (voxel_city nature/trees.js) against the reference: their tables and
// many sampled models (stage "trees" of tools/procgen_ref), the API's lookups, and chunks that
// come out the same in any order and from several threads.
#include <doctest.h>

#include <array>
#include <atomic>
#include <thread>

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

namespace {

// A wood of every kind (wild or not, with looks of every season), close together.
std::vector<Tree> mixed_wood() {
  const std::vector<Season> seasons = trec::seasons();
  const std::vector<std::string> kinds = trec::kinds();
  rec::Samples r(47);
  std::vector<Tree> trees;
  for (int k = 0; k < 64; ++k) {
    const std::string& kind = kinds[static_cast<size_t>(k) % kinds.size()];
    const double x = std::floor(r() * 240);
    const double y = std::floor(r() * 240);
    Tree t = trec::sample_tree(r, kind, x, y, 40 + std::floor(r() * 8));
    t.look = seasons[static_cast<size_t>(k) % seasons.size()].tree_look(kind, t.seed, r() * 0.8);
    trees.push_back(t);
  }
  return trees;
}

struct ChunkKey {
  int lod;
  double cx, cy, cz;
};

uint32_t chunk_digest(const ChunkKey& c, const std::vector<Tree>& trees, const std::vector<Box3>& bounds) {
  ChunkBuffer ch(c.lod, c.cx, c.cy, c.cz);
  for (size_t i = 0; i < trees.size(); ++i) {
    const Box3& b = bounds[i];
    if (ch.touches(b.x0, b.y0, b.z0, b.x1, b.y1, b.z1)) rasterize_tree(ch, trees[i]);
  }
  uint32_t h = 2166136261u;
  for (const uint16_t v : ch.data) h = (h ^ v) * 16777619u;
  return h;
}

}  // namespace

TEST_CASE("city trees: chunks are the same in any order and from several threads") {
  const std::vector<Tree> proto = mixed_wood();
  std::vector<Box3> bounds;
  for (const Tree& t : proto) bounds.push_back(tree_bounds(t));
  // every chunk any tree touches at LODs 0, 1 and 2, the LODs interleaved
  std::vector<ChunkKey> keys;
  for (const int lod : {0, 1, 2}) {
    const double S = 32 << lod;
    for (double cz = std::floor((40 - 2 - (1 << lod)) / S); cz <= std::floor(300 / S); cz += 1)
      for (double cy = std::floor(-80 / S); cy <= std::floor(320 / S); cy += 1)
        for (double cx = std::floor(-80 / S); cx <= std::floor(320 / S); cx += 1) keys.push_back({lod, cx, cy, cz});
  }
  for (size_t i = 0; i + 1 < keys.size(); i += 3) std::swap(keys[i], keys[keys.size() - 1 - i / 3]);
  std::vector<uint32_t> want(keys.size());
  for (size_t i = 0; i < keys.size(); ++i) want[i] = chunk_digest(keys[i], proto, bounds);
  // backwards, the trees' models made afresh (a copy of a tree makes its own)
  {
    const std::vector<Tree> fresh = proto;
    size_t differ = 0;
    for (size_t i = keys.size(); i-- > 0;) differ += chunk_digest(keys[i], fresh, bounds) == want[i] ? 0 : 1;
    CHECK_MESSAGE(differ == 0, "chunks differing backwards: ", differ);
  }
  // four threads at once over fresh trees (racing to make their models), each in its own order
  const std::vector<Tree> fresh = proto;
  std::atomic<size_t> bad{0};
  std::vector<std::thread> threads;
  for (size_t t = 0; t < 4; ++t)
    threads.emplace_back([&, t] {
      for (size_t k = 0; k < keys.size(); ++k) {
        const size_t i = (k * 7 + t * 131 + (t & 1 ? keys.size() - 1 - k : 0)) % keys.size();
        if (chunk_digest(keys[i], fresh, bounds) != want[i]) bad.fetch_add(1);
      }
    });
  for (std::thread& th : threads) th.join();
  CHECK_MESSAGE(bad.load() == 0, "chunks differing on 4 threads: ", bad.load());
  CHECK(keys.size() > 300);
}
