// svx_city tests — U-shaped stairs (voxel_city buildings/interior/stairs.js) and the planners'
// pure helpers (common.js) against the reference (stage "stairs" of tools/procgen_ref).
#include <doctest.h>

#include <string>
#include <vector>

#include "buildings/interior/common.hpp"
#include "buildings/interior/stairs.hpp"
#include "records.hpp"

using namespace svx::city;
using rec::Line;

TEST_CASE("city stairs: stairs and the planners' helpers conform to the reference (stage stairs)") {
  rec::Samples r(5);
  rec::Out out;
  for (double H = 6; H <= 70; H += 1) {
    const StairDims d = stair_dims(H);
    const StairDims d2 = stair_dims(H, 5 + std::fmod(H, 4), 6 + std::fmod(H, 5));
    out << (Line() << "dims" << H << d.W << d.L << d.lane << d.landing << d2.W << d2.L << d2.lane << d2.landing);
  }
  for (int i = 0; i < 300; ++i) {
    const char axis = r() < 0.5 ? 'u' : 'v';
    const double dir = r() < 0.5 ? 1 : -1;
    const bool lane_low = r() < 0.5;
    const double lr = r();
    const double lv = r();
    const double lane = lr < 0.7 ? 8 : 5 + std::floor(lv * 6);
    const double landing = lr < 0.7 ? 9 : 6 + std::floor(lv * 5);
    const double story_h = 14 + std::floor(r() * 30);
    const StairDims dims = stair_dims(story_h, lane, landing);
    const double x0 = std::floor((r() - 0.5) * 200);
    const double y0 = std::floor((r() - 0.5) * 200);
    const Rect rect = axis == 'v' ? Rect{x0, y0, x0 + dims.W - 1, y0 + dims.L - 1} : Rect{x0, y0, x0 + dims.L - 1, y0 + dims.W - 1};
    const double f0 = std::floor(r() * 3) - 1;
    const double f1 = f0 + 1 + std::floor(r() * 4);
    const bool open = r() < 0.3;
    MakeStairOpts o;
    o.rect = rect;
    o.axis = axis;
    o.dir = dir;
    o.f0 = f0;
    o.f1 = f1;
    if (i % 7 != 0) {
      o.lane_low = lane_low;
      o.lane = lane;
      o.landing = landing;
      o.open = open;
    }
    Stair st = make_stair(o);
    out << (Line() << "st" << i << st.rect.x0 << st.rect.y0 << st.rect.x1 << st.rect.y1 << st.axis << st.dir << st.lane_low << st.lane << st.landing << st.f0
                   << st.f1 << st.L << st.W << st.open);
    const Rect nl = near_landing(st);
    out << (Line() << "nl" << i << nl.x0 << nl.y0 << nl.x1 << nl.y1);
    std::string loc, slab;
    for (int k = 0; k < 30; ++k) {
      const double u = rect.x0 - 2 + std::floor(r() * (rect.x1 - rect.x0 + 5));
      const double v = rect.y0 - 2 + std::floor(r() * (rect.y1 - rect.y0 + 5));
      const auto p = stair_local(st, u, v);
      if (k) loc += ' ';
      loc += p ? js::cat(p->s, ",", p->t) : "-";
      for (double f = f0 - 1; f <= f1 + 1; f += 1) slab += stair_slab_open(st, f, u, v) ? '1' : '0';
    }
    out << (Line() << "loc" << i << loc << slab);
    st.flights = std::vector<StairFlight>{};
    for (double f = st.f0; f < st.f1; f += 1) st.flights->push_back({f, f * (story_h + 2) + 3, story_h + ((js::to_int32(f) & 1) ? 2 : 0)});
    Line l;
    l << "sb" << i;
    for (const CanonBox& b : stair_boxes(st, {11, 12, 13, 14})) l << b.x0 << b.y0 << b.z0 << b.x1 << b.y1 << b.z1 << b.m;
    out << l;
  }
  // common.js
  for (int i = 0; i < 400; ++i) {
    const double a0 = std::floor((r() - 0.5) * 300);
    const double a1 = a0 + std::floor(r() * 400);
    const double target = 4 + std::floor(r() * 80);
    const double min_w = 2 + std::floor(r() * 30);
    const double jr = r();
    Rng rng(std::floor(r() * 4294967296.0));
    const auto pieces = jr < 0.5 ? split_length(a0, a1, target, min_w, rng) : split_length(a0, a1, target, min_w, rng, jr);
    std::string ps;
    for (size_t k = 0; k < pieces.size(); ++k) ps += js::cat(k ? "," : "", pieces[k][0], ":", pieces[k][1]);
    out << (Line() << "split" << a0 << a1 << target << min_w << jr << ps << rng.next());
  }
  for (int i = 0; i < 300; ++i) {
    const double a0 = std::floor((r() - 0.5) * 100);
    const double a1 = a0 + std::floor(r() * 200);
    const double n = std::floor(r() * 6);
    std::vector<std::array<double, 2>> blocked;
    for (double k = 0; k < n; k += 1) {
      const double b0 = a0 - 20 + std::floor(r() * 240);
      blocked.push_back({b0, b0 + std::floor(r() * 30)});
    }
    const auto free = free_intervals(a0, a1, blocked);
    std::string bs, fs;
    for (size_t k = 0; k < blocked.size(); ++k) bs += js::cat(k ? "," : "", blocked[k][0], ":", blocked[k][1]);
    for (size_t k = 0; k < free.size(); ++k) fs += js::cat(k ? "," : "", free[k][0], ":", free[k][1]);
    out << (Line() << "free" << a0 << a1 << bs << fs);
  }
  for (int i = 0; i < 300; ++i) {
    Rect rect;
    rect.x0 = std::floor(r() * 100);
    rect.y0 = std::floor(r() * 100);
    rect.x1 = rect.x0 + std::floor(r() * 20);
    rect.y1 = rect.y0 + std::floor(r() * 20);
    std::vector<Rect> list;
    const double n = std::floor(r() * 5);
    for (double k = 0; k < n; k += 1) {
      const double x0 = std::floor(r() * 120);
      const double y0 = std::floor(r() * 120);
      const double x1 = x0 + std::floor(r() * 15);
      const double y1 = y0 + std::floor(r() * 15);
      list.push_back({x0, y0, x1, y1});
    }
    out << (Line() << "ovl" << rect.x0 << rect.y0 << rect.x1 << rect.y1 << n << rects_overlap_any(rect, list));
  }
  CHECK(rec::record("stairs", out.text()) == rec::recorded_digest("stairs"));
}
