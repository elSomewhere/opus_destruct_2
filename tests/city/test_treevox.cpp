// svx_city tests — trees rasterized into chunks (voxel_city nature/trees.js rasterizeTree) at
// LODs 0, 2, 5 and 8 against the reference (stage "treevox" of tools/procgen_ref).
#include <doctest.h>

#include <array>
#include <set>

#include "records.hpp"
#include "tree_records.hpp"

using namespace svx::city;
using rec::Line;

namespace {

constexpr int kLods[4] = {0, 2, 5, 8};
constexpr double kSnows[4] = {0, 0.25, 0.45, 1};

// FNV-1a over the values (stages/chunk.mjs digest).
uint32_t digest(const std::vector<uint16_t>& a) {
  uint32_t h = 2166136261u;
  for (const uint16_t v : a) h = (h ^ v) * 16777619u;
  return h;
}

// A look's fields: leaves, mix, bare, snow, holes, accent ("-": no look).
void look_fields(Line& l, const std::optional<TreeLook>& L) {
  if (!L) {
    l << rec::kUndef;
    return;
  }
  if (L->leaves)
    l << trec::pal_fields(L->leaves);
  else
    l << rec::kUndef;
  l << L->mix << L->bare << L->snow << L->holes;
  if (L->accent)
    l << L->accent;
  else
    l << rec::kUndef;
}

// A tree's look from r: none (and a snow cover), or a season's at a local temperature.
double sample_look(rec::Samples& r, Tree& t, const std::string& kind, const std::vector<Season>& seasons) {
  if (r() < 0.2) return kSnows[static_cast<int>(std::floor(r() * 4))];
  const Season& S = seasons[static_cast<size_t>(std::floor(r() * static_cast<double>(seasons.size())))];
  // (half of them where the seasons turn: bare or not, snow or not)
  const double tl = r() < 0.5 ? r() * 1.1 - 0.15 : 0.2 + r() * 0.5;
  t.look = S.tree_look(kind, t.seed, tl);
  return 0;
}

using Chunk3 = std::array<double, 3>;

// The chunk coordinates (at chunk span S) of points, without repeats, in order.
std::vector<Chunk3> chunks_of(double S, const std::vector<Chunk3>& pts) {
  std::vector<Chunk3> out;
  for (const Chunk3& p : pts) {
    const Chunk3 c = {std::floor(p[0] / S), std::floor(p[1] / S), std::floor(p[2] / S)};
    bool seen = false;
    for (const Chunk3& o : out) seen = seen || o == c;
    if (!seen) out.push_back(c);
  }
  return out;
}

// Every chunk (at chunk span S) a box touches, the padded buffers' one-voxel apron included.
std::vector<Chunk3> chunks_touching(int lod, const Box3& bb) {
  const double S = 32 << lod;
  const double s = 1 << lod;
  std::vector<Chunk3> out;
  for (double cz = std::floor((bb.z0 - s) / S); cz <= std::floor((bb.z1 + s) / S); cz += 1)
    for (double cy = std::floor((bb.y0 - s) / S); cy <= std::floor((bb.y1 + s) / S); cy += 1)
      for (double cx = std::floor((bb.x0 - s) / S); cx <= std::floor((bb.x1 + s) / S); cx += 1) out.push_back({cx, cy, cz});
  return out;
}

// A chunk made ready from r: tracking isolated voxels or not, isolating or not, partly filled
// first or not.
ChunkBuffer prepare(rec::Samples& r, int lod, const Chunk3& c, const Tree& t) {
  ChunkBuffer ch(lod, c[0], c[1], c[2]);
  const double q = r();
  if (q < 0.3) ch.track_isolated();
  if (q < 0.15 || q > 0.9) ch.isolating = true;
  if (r() < 0.3) {
    const double w = std::floor(r() * 24) * ch.s;
    const double m = 1 + std::floor(r() * 399);
    ch.fill_box(t.x - w, t.y - 3 * ch.s, t.z + t.h * 0.3, t.x + w, t.y + w, t.z + t.h * 0.6, js::u16(m), 0);
  }
  return ch;
}

Line chunk_line(int lod, const Chunk3& c, ChunkBuffer& ch) {
  Line l;
  l << "v" << lod << c[0] << c[1] << c[2] << ch.count_non_air() << digest(ch.data);
  if (ch.iso_data())
    l << digest(ch.iso);
  else
    l << rec::kUndef;
  return l;
}

}  // namespace

TEST_CASE("city treevox: trees rasterize into chunks as the reference's (stage treevox)") {
  const std::vector<Season> seasons = trec::seasons();
  const std::vector<std::string> kinds = trec::kinds();
  rec::Samples r(37);
  rec::Out out;
  for (const std::string& kind : kinds)
    for (int i = 0; i < 20; ++i) {
      double x = std::floor((r() - 0.5) * 60000);
      double y = std::floor((r() - 0.5) * 60000);
      const double z = std::floor(r() * 1200) - 200;
      if (r() < 0.4) {
        x = js::round(x / 8192) * 8192 + std::floor((r() - 0.5) * 60);
        y = js::round(y / 8192) * 8192 + std::floor((r() - 0.5) * 60);
      }
      if (i == 8) x += 2147483648.0;
      if (i == 9) y -= 2147483648.0 + 4096;
      const bool off = r() < 0.2;
      const double ox = off ? r() : 0;
      const double oy = off ? r() : 0;
      const double oz = off && r() < 0.5 ? 0.5 : 0;
      Tree t = trec::sample_tree(r, kind, x + ox, y + oy, z + oz);
      const double snow = sample_look(r, t, kind, seasons);
      const Box3 bb = tree_bounds(t);
      Line l;
      l << "t";
      trec::tree_fields(l, kind, t);
      l << snow;
      look_fields(l, t.look);
      trec::box_fields(l, bb);
      out << l;
      for (const int lod : kLods) {
        const double S = 32 << lod;
        const bool whole = lod >= 5 || i < 3;
        const std::vector<Chunk3> list = whole ? chunks_touching(lod, bb)
                                               : chunks_of(S, {{t.x, t.y, t.z},
                                                               {t.x, t.y, t.z + t.h * 0.7},
                                                               {bb.x0, bb.y1, t.z + t.h * 0.5},
                                                               {bb.x1, bb.y0, bb.z1},
                                                               {bb.x1 + S * 0.5, t.y, t.z}});
        for (const Chunk3& c : list) {
          ChunkBuffer ch = prepare(r, lod, c, t);
          rasterize_tree(ch, t, snow);
          out << chunk_line(lod, c, ch);
        }
      }
    }
  // groves: eight trees of any kinds close together, into shared chunks
  for (int g = 0; g < 30; ++g) {
    const double gx = std::floor((r() - 0.5) * 60000);
    const double gy = std::floor((r() - 0.5) * 60000);
    const double gz = std::floor(r() * 600);
    std::vector<Tree> trees;
    std::vector<double> snows;
    trees.reserve(8);
    for (int k = 0; k < 8; ++k) {
      const std::string& kind = kinds[static_cast<size_t>(std::floor(r() * static_cast<double>(kinds.size())))];
      const double x = gx + std::floor(r() * 160);
      const double y = gy + std::floor(r() * 160);
      const double z = gz + std::floor(r() * 24);
      trees.push_back(trec::sample_tree(r, kind, x, y, z));
      snows.push_back(sample_look(r, trees.back(), kind, seasons));
      Line l;
      l << "gt" << g;
      trec::tree_fields(l, kind, trees.back());
      l << snows.back();
      look_fields(l, trees.back().look);
      out << l;
    }
    for (const int lod : kLods) {
      const double S = 32 << lod;
      const std::vector<Chunk3> list = chunks_of(S, {{gx + 80, gy + 80, gz + 40}, {gx + 40, gy + 120, gz + 12}, {gx + 150, gy + 20, gz + 70}});
      for (const Chunk3& c : list) {
        ChunkBuffer ch = prepare(r, lod, c, trees[0]);
        for (int k = 0; k < 8; ++k) rasterize_tree(ch, trees[static_cast<size_t>(k)], snows[static_cast<size_t>(k)]);
        out << chunk_line(lod, c, ch);
      }
    }
  }
  CHECK(rec::record("treevox", out.text()) == rec::recorded_digest("treevox"));
}
