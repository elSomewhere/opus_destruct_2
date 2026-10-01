// svx_city tests — underground complexes (voxel_city sites/complex.js) against the reference
// (stage "complex" of tools/procgen_ref): the themes, corridor rects, room shapes, stair shafts
// and their boxes, complexes planned from specs in the site kinds' shapes and random ones, their
// box lists, and their structures rasterized into ground-filled chunks.
#include <doctest.h>

#include <string>
#include <vector>

#include "complexes.hpp"
#include "records.hpp"
#include "sites/complex.hpp"
#include "sites/kit.hpp"
#include "world/register_all.hpp"

using namespace svx::city;
using rec::Line;

TEST_CASE("city complex: complexes conform to the reference (stage complex)") {
  register_all();
  rec::Samples r(41);
  rec::Out out;
  for (const ComplexTheme& t : complex_themes().all()) {
    std::string weights, big;
    for (size_t i = 0; i < t.weights.size(); ++i) weights += js::cat(i ? "," : "", t.weights[i].first, ":", t.weights[i].second);
    for (double k = 0; k < 4; k += 1) big += js::cat(k > 0 ? "," : "", t.big(k, false), ",", t.big(k, true));
    out << (Line() << "theme" << t.id << weights << big << t.frame << t.accent << t.shapes);
  }
  out << (Line() << "gap" << kLevelGap);
  for (int i = 0; i < 300; ++i) {
    const double x0 = std::floor((r() - 0.5) * 2000);
    const double y0 = std::floor((r() - 0.5) * 2000);
    const double x1 = r() < 0.2 ? x0 : std::floor((r() - 0.5) * 2000);
    const double y1 = r() < 0.2 ? y0 : std::floor((r() - 0.5) * 2000);
    const double w = r() < 0.5 ? 24 : 1 + std::floor(r() * 60);
    const Rect q = seg_rect(x0, y0, x1, y1, w);
    out << (Line() << "seg" << x0 << y0 << x1 << y1 << w << q.x0 << q.y0 << q.x1 << q.y1);
  }
  const char* shapes[5] = {"", "rect", "octagon", "round", "cross"};
  for (int i = 0; i < 400; ++i) {
    ComplexRoom q;
    q.x0 = std::floor((r() - 0.5) * 4000);
    q.y0 = std::floor((r() - 0.5) * 4000);
    const bool big = r() < 0.7;
    q.x1 = q.x0 + std::floor(r() * (big ? 300 : 12));
    q.y1 = q.y0 + std::floor(r() * (big ? 300 : 12));
    q.shape = shapes[i % 5];
    Line l;
    l << "shape" << q.x0 << q.y0 << q.x1 << q.y1;
    if (q.shape.empty())
      l << rec::kUndef;
    else
      l << q.shape;
    l << test::rect_list(shape_rects(q));
    out << l;
  }
  for (int i = 0; i < 120; ++i) {
    const double n = 1 + std::floor(r() * 5);
    std::vector<double> zs;
    for (double k = 0; k < n; k += 1) zs.push_back(std::floor((r() - 0.5) * 600));
    const double cx = std::floor((r() - 0.5) * 4000);
    const double cy = std::floor((r() - 0.5) * 4000);
    const Rect rect{cx, cy, cx + 16, cy + 45};
    const double dir = r() < 0.5 ? -1 : 1;
    const double t = r();
    const ComplexShaft sh = t < 0.3 ? mk_shaft(rect, zs) : mk_shaft(rect, zs, dir, t < 0.65);
    std::string zl, levels, flights;
    for (size_t k = 0; k < zs.size(); ++k) zl += js::cat(k ? "," : "", zs[k]);
    for (size_t k = 0; k < sh.levels.size(); ++k) levels += js::cat(k ? "," : "", sh.levels[k]);
    for (size_t k = 0; k < sh.st.flights->size(); ++k) {
      const StairFlight& fl = (*sh.st.flights)[k];
      flights += js::cat(k ? "," : "", fl.f, "/", fl.z0, "/", fl.H);
    }
    out << (Line() << "mk" << zl << sh.dir << sh.open_top << sh.z_low << sh.z_high << levels << sh.st.f0 << sh.st.f1 << (flights.empty() ? std::string("none") : flights));
    std::vector<SiteBox> det;
    emit_shaft(det, sh);
    test::box_lines(out, "D", det);
  }
  std::vector<int> kinds = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
  for (int k = 0; k < 24; ++k) kinds.push_back(3);
  for (int k = 0; k < 4; ++k) kinds.push_back(4);
  for (int k = 0; k < 6; ++k) kinds.push_back(5);
  for (size_t n = 0; n < kinds.size(); ++n) {
    const test::DrawnSpec d = test::draw_complex_spec(r, kinds[n]);
    const double top = d.top;
    const double seed = std::floor(r() * 4294967296.0);
    Rng rng(seed);
    const Complex cx = plan_complex(rng, d.spec);
    out << (Line() << "complex" << n << kinds[n] << seed << top);
    test::complex_lines(out, cx);
    BoxLists lists;
    emit_complex(lists, cx, rng);
    out << (Line() << "rng" << rng.next());
    // (every box of one complex of each kind, digests of 32 boxes elsewhere)
    const bool full = n == 0 || n == 10 || n == 18 || n == 28 || n == 52;
    test::box_lines(out, "S", lists.shells, full);
    test::box_lines(out, "C", lists.carves, full);
    test::box_lines(out, "D", lists.details, full);
    const SiteStructure st = finish_structure(std::move(lists));
    {
      Line l;
      l << "bb" << st.boxes.size();
      if (st.bb)
        l << std::vector<double>{st.bb->x0, st.bb->y0, st.bb->z0, st.bb->x1, st.bb->y1, st.bb->z1};
      else
        l << rec::kUndef;
      out << l;
    }
    // chunks round points of the complex (and one at the top of its first shaft, at LOD 3)
    const auto pts = test::complex_points(cx);
    std::vector<int> lods = {0, 0, 0, 0, 0, 0, 2, 2, 3, 5};
    if (!cx.shafts.empty()) lods.push_back(-3);
    for (const int l : lods) {
      const int lod = l < 0 ? -l : l;
      std::array<double, 3> p;
      if (l < 0)
        p = {cx.shafts[0].rect.x0 + 8, cx.shafts[0].rect.y0 + 20, cx.shafts[0].z_high};
      else
        p = pts[static_cast<size_t>(std::floor(r() * static_cast<double>(pts.size())))];
      const double e = static_cast<double>(32 << lod);
      const double ccx = std::floor((p[0] + std::floor((r() - 0.5) * e * 0.5)) / e);
      const double ccy = std::floor((p[1] + std::floor((r() - 0.5) * e * 0.5)) / e);
      const double ccz = std::floor((p[2] + std::floor((r() - 0.5) * e * 0.5)) / e);
      ChunkBuffer ch = test::ground_chunk(lod, ccx, ccy, ccz, top - 1);
      rasterize_structure(st, ch, lod >= 3 ? top - 1 - 16 : -js::kInf);
      const int count = ch.count_non_air();
      out << (Line() << "ch" << lod << ccx << ccy << ccz << count << test::chunk_digest(ch.data));
    }
  }
  CHECK(rec::record("complex", out.text()) == rec::recorded_digest("complex"));
}

TEST_CASE("city complex: a complex's shafts link its levels and its rooms stay inside their sector") {
  register_all();
  rec::Samples r(5);
  for (int n = 0; n < 12; ++n) {
    const test::DrawnSpec d = test::draw_complex_spec(r, n % 4);
    Rng rng(1000 + n);
    const Complex cx = plan_complex(rng, d.spec);
    REQUIRE(cx.sectors.size() == d.spec.sectors.size());
    for (const ComplexSector& s : cx.sectors) {
      for (const size_t l : s.levels) {
        const ComplexLevel& lv = cx.levels[l];
        REQUIRE(!lv.rooms.empty());
        CHECK(lv.rooms[0].type == "hall");  // the shaft's anteroom stays
        for (const ComplexRoom& q : lv.rooms) {
          if (q.fixed) continue;
          CHECK(q.x0 >= s.bounds.x0 - 8);
          CHECK(q.y0 >= s.bounds.y0 - 8);
        }
      }
    }
    for (const ComplexShaft& sh : cx.shafts) {
      REQUIRE(sh.st.flights);
      double rise = 0;
      for (const StairFlight& fl : *sh.st.flights) rise += fl.H;
      CHECK(rise == sh.z_high - sh.z_low);
    }
  }
}
