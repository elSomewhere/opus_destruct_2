// svx_city tests — polygon blocks (voxel_city city/blockPoly.js) against the reference (stage
// "blockpoly" of tools/procgen_ref).
#include <doctest.h>

#include <deque>
#include <string>
#include <vector>

#include "city/blockPoly.hpp"
#include "records.hpp"

using namespace svx::city;
using rec::Line;

namespace {

const double kDirs[10][2] = {{1, 0}, {0, 1}, {3, 4}, {-4, 3}, {20, 21}, {21, -20}, {5, -12}, {1, 1}, {-7, 24}, {0, -1}};
const char* const kCls[3] = {"street", "avenue", "alley"};

RoadSide side(rec::Samples& r) {
  const double k = std::floor(r() * 4);
  const double hr = 8 + std::floor(r() * 30);
  const std::string id = js::cat("r", std::floor(r() * 1000));
  if (k == 0) return RoadSide{};
  RoadSide s;
  s.cls = kCls[static_cast<int>(k) - 1];
  s.hr = hr;
  if (k != 3) s.id = id;
  return s;
}
std::string fo(const std::optional<std::string>& s) { return s ? (s->empty() ? "\"\"" : *s) : "-"; }
std::string fs(const RoadSide& s) { return js::cat(fo(s.cls), "/", s.hr, "/", fo(s.id)); }
std::string fpoly(const std::optional<Poly>& p) {
  if (!p) return "-";
  std::string out;
  for (size_t i = 0; i < p->size(); ++i) {
    if (i) out += ';';
    out += js::cat((*p)[i].x, ",", (*p)[i].y, ",", fs((*p)[i].side));
  }
  return out;
}
std::string fcut(const BlockCut& k) { return js::cat(k.nx, ",", k.ny, ",", k.c, ",", fo(k.cls), ",", k.hr, ",", fo(k.id)); }
bool same_side(const RoadSide& a, const RoadSide& b) { return a.cls == b.cls && a.hr == b.hr && a.id == b.id; }
bool same_poly(const Poly& a, const Poly& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].x != b[i].x || a[i].y != b[i].y || !same_side(a[i].side, b[i].side)) return false;
  return true;
}

}  // namespace

TEST_CASE("city blockPoly: polygon blocks conform to the reference (stage blockpoly)") {
  rec::Samples r(17);
  rec::Out out;
  for (int i = 0; i < 160; ++i) {
    const double x0 = std::floor((r() - 0.5) * 4000);
    const double y0 = std::floor((r() - 0.5) * 4000);
    const double w = 30 + std::floor(r() * 500);
    const double h = 30 + std::floor(r() * 500);
    const Rect rect{x0, y0, x0 + w, y0 + h};
    const RoadSide sN = side(r);
    const RoadSide sE = side(r);
    const RoadSide sS = side(r);
    const RoadSide sW = side(r);
    const bool some = r() < 0.8;
    const Poly poly = rect_poly(rect, some ? BlockSides{sN, sE, sS, sW} : BlockSides{sN, {}, {}, {}});
    out << (Line() << "rp" << i << fpoly(poly) << poly_area(poly));
    std::vector<Poly> pieces;
    std::deque<Poly> queue{poly};
    int splits = 0;
    while (!queue.empty()) {
      const Poly p = queue.front();
      queue.pop_front();
      const bool go = r() < 0.75 && splits < 6;
      const double ax = r();
      const double ay = r();
      const double* dir = kDirs[static_cast<size_t>(std::floor(r() * 10))];
      const double jitter = r() < 0.3 ? r() - 0.5 : 0;
      const RoadSide cut = side(r);
      if (!go) {
        pieces.push_back(p);
        continue;
      }
      const Rect bb = poly_bounds(p);
      const Point2 a{bb.x0 + (bb.x1 - bb.x0) * ax, bb.y0 + (bb.y1 - bb.y0) * ay};
      const double dx = dir[0] + jitter;
      const double dy = dir[1];
      const bool crosses = line_crosses(p, a, dx, dy);
      auto [L, R] = split_poly(p, a, dx, dy, cut);
      splits += 1;
      out << (Line() << "sp" << i << splits << a.x << a.y << dx << dy << crosses << fpoly(L) << fpoly(R));
      if (L) queue.push_back(*L);
      if (R) queue.push_back(*R);
      if (!L && !R) pieces.push_back(p);
    }
    for (const Poly& p : pieces) {
      const Point2 c = poly_centroid(p);
      const Rect bb = poly_bounds(p);
      const PolyBlock blk = poly_block(p);
      out << (Line() << "pc" << i << poly_area(p) << c.x << c.y << bb.x0 << bb.y0 << bb.x1 << bb.y1);
      std::string cuts;
      for (size_t k = 0; k < blk.cuts.size(); ++k) {
        if (k) cuts += ';';
        cuts += fcut(blk.cuts[k]);
      }
      out << (Line() << "blk" << i << blk.r.x0 << blk.r.y0 << blk.r.x1 << blk.r.y1 << fs(blk.s.N) << fs(blk.s.E) << fs(blk.s.S) << fs(blk.s.W) << cuts
                     << same_poly(blk.poly, p));
      std::string ins;
      for (int k = 0; k < 24; ++k) {
        const double x = blk.r.x0 - 5 + r() * (blk.r.x1 - blk.r.x0 + 10);
        const double y = blk.r.y0 - 5 + r() * (blk.r.y1 - blk.r.y0 + 10);
        ins += inside_cuts(blk.cuts, x, y) ? '1' : '0';
      }
      out << (Line() << "in" << i << ins);
      for (int k = 0; k < 8; ++k) {
        const double qx = blk.r.x0 + std::floor(r() * (blk.r.x1 - blk.r.x0 + 1));
        const double qy = blk.r.y0 + std::floor(r() * (blk.r.y1 - blk.r.y0 + 1));
        const double qx1 = qx + std::floor(r() * 120);
        const double qy1 = qy + std::floor(r() * 120);
        const Rect q{qx, qy, qx1, qy1};
        const auto t = trim_to_cuts(q, blk.cuts);
        Line l;
        l << "tr" << i << q.x0 << q.y0 << q.x1 << q.y1;
        if (t) {
          l << std::vector<double>{t->rect.x0, t->rect.y0, t->rect.x1, t->rect.y1};
          std::string tr;
          for (size_t s = 0; s < t->trimmed.size(); ++s) {
            if (s) tr += ';';
            tr += js::cat(t->trimmed[s].side, "/", fo(t->trimmed[s].cls), "/", fo(t->trimmed[s].id));
          }
          l << tr;
        } else {
          l << rec::kUndef << rec::kUndef;
        }
        out << l;
      }
    }
  }
  CHECK(rec::record("blockpoly", out.text()) == rec::recorded_digest("blockpoly"));
}
