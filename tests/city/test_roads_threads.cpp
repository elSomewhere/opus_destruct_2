// svx_city tests — the road network is a pure function of its roads, the terrain and the waters:
// road levels and the highways' products come out the same in any call order, after the caches
// drop what they hold (views remade, even from other road objects: docs/CITY.md §6, a road's
// identity across views) and from several threads at once.
#include <doctest.h>

#include <atomic>
#include <thread>

#include "config/presets.hpp"
#include "network/arterials.hpp"
#include "network/highways.hpp"
#include "network/roadLevel.hpp"
#include "network/roadView.hpp"
#include "road_inputs.hpp"
#include "world/World.hpp"
#include "world/caches.hpp"

using namespace svx::city;

namespace {

// A query of road levels: a segment of view (i, j) by its index there, and an arc along it; or a
// street level at a point (seg -1).
struct LevelQuery {
  int seg;
  double a, x, y;
};

// (street levels inside the cell, 200 voxels in from its edges: what stage roadlevel recorded)
std::vector<LevelQuery> level_queries(const RoadView& view, const std::string& cell, const Rect& inner) {
  std::vector<LevelQuery> q;
  rec::Samples r(71);
  for (size_t k = 0; k < view.segs.size(); ++k) {
    const RoadSeg& s = view.segs[k];
    if (s.road->cell != cell) continue;
    for (double a = 0; a <= s.len; a += 17) q.push_back({static_cast<int>(k), a, 0, 0});
    const double t = r() * s.len;
    const double lat = (r() - 0.5) * 2 * (s.hr + 20);
    const double x = s.ax + s.dx * t - s.dy * lat;
    const double y = s.ay + s.dy * t + s.dx * lat;
    q.push_back({-1, 0, js::max(inner.x0, js::min(inner.x1, x)), js::max(inner.y0, js::min(inner.y1, y))});
  }
  return q;
}

double answer(const World& w, const RoadView& view, const LevelQuery& q) {
  if (q.seg < 0) return w.street_level(q.x, q.y);
  return segment_level(w, view.segs[static_cast<size_t>(q.seg)], q.a);
}

}  // namespace

TEST_CASE("city road network: road levels are the same in any order, after the caches drop, and on several threads") {
  const auto recorded = test::load_recorded("roadlevel");
  for (const char* key : {"cities", "angledOldHarbourTown"}) {
    const Value config = preset_config(key);
    const auto rw = recorded.at(key);
    World a(config);
    test::use_recorded(a, rw);
    const std::string cell = "C0_0";
    const std::shared_ptr<const RoadView> va = a.road_view(0, 0);
    const Rect rc = a.arterials->cell_rect(0, 0);
    const std::vector<LevelQuery> qs = level_queries(*va, cell, Rect{rc.x0 + 200, rc.y0 + 200, rc.x1 - 200, rc.y1 - 200});
    REQUIRE(qs.size() > 100);
    std::vector<double> want(qs.size());
    for (size_t k = 0; k < qs.size(); ++k) want[k] = answer(a, *va, qs[k]);
    // backwards, on a fresh world (fresh roads too: every profile made in another order)
    {
      World b(config);
      test::use_recorded(b, test::load_recorded("roadlevel").at(key));
      const std::shared_ptr<const RoadView> vb = b.road_view(0, 0);
      size_t differ = 0;
      for (size_t k = qs.size(); k-- > 0;) differ += answer(b, *vb, qs[k]) == want[k] ? 0 : 1;
      CHECK_MESSAGE(differ == 0, key, ": levels differing backwards: ", differ);
    }
    // half way, the views dropped and the cell networks' roads made again (other objects): the
    // segments of the view held before ask views of the new ones
    {
      World c(config);
      test::use_recorded(c, rw);
      const std::shared_ptr<const RoadView> vc = c.road_view(0, 0);
      size_t differ = 0;
      for (size_t k = 0; k < qs.size(); ++k) {
        if (k == qs.size() / 2) {
          c.caches().road_views.clear();
          test::use_recorded(c, test::load_recorded("roadlevel").at(key));
        }
        differ += answer(c, *vc, qs[k]) == want[k] ? 0 : 1;
      }
      CHECK_MESSAGE(differ == 0, key, ": levels differing after the caches dropped: ", differ);
    }
    // four threads at once on a fresh world, each in its own order
    {
      World d(config);
      test::use_recorded(d, test::load_recorded("roadlevel").at(key));
      const std::shared_ptr<const RoadView> vd = d.road_view(0, 0);
      std::atomic<size_t> bad{0};
      std::vector<std::thread> threads;
      for (size_t t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
          for (size_t k = 0; k < qs.size(); ++k) {
            const size_t i = (k * 7 + t * 131 + (t & 1 ? qs.size() - 1 - k : 0)) % qs.size();
            if (answer(d, *vd, qs[i]) != want[i]) bad.fetch_add(1);
          }
        });
      for (std::thread& th : threads) th.join();
      CHECK_MESSAGE(bad.load() == 0, key, ": levels differing on 4 threads: ", bad.load());
    }
  }
}

TEST_CASE("city road network: the highways' products are the same whoever makes them first") {
  const auto recorded = test::load_recorded("highways");
  Value config;
  REQUIRE(Value::parse_json(R"({"seed":99})", &config));
  auto summary = [](const World& w) {
    const HighwayNetwork& hw = *w.highways;
    const HighwayEdgePtr e = hw.edge(0, -1, 0);
    std::string s = js::cat(e->id, ":", e->total, ";");
    for (const HighwayRamp& r : hw.ramps(*e)) s += js::cat("R", r.s_deck, ",", r.s_ground, ",", r.z_ground, ",", hw.ramp_piers(*e, r).size(), ";");
    for (const HighwayPier& p : hw.piers(*e)) s += js::cat("P", p.s, ",", p.cols.size(), ";");
    for (double t = 0; t < e->total; t += 397) {
      const HighwayPoint p = offset_at(*e, t, 90);
      const std::optional<double> u = hw.underside(std::floor(p.x), std::floor(p.y));
      s += js::cat(hw.covers(p.x, p.y, 4) ? "c" : "-", u ? *u : -1, ";");
    }
    return s;
  };
  World a(config);
  test::use_recorded(a, recorded.at("seed99"));
  a.highways = std::make_shared<HighwayNetwork>(a);
  const std::string want = summary(a);
  World c(config);
  test::use_recorded(c, recorded.at("seed99"));
  c.highways = std::make_shared<HighwayNetwork>(c);
  std::vector<std::string> outs(4);
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) threads.emplace_back([&, t] { outs[size_t(t)] = summary(c); });
  for (std::thread& th : threads) th.join();
  for (const std::string& o : outs) CHECK(o == want);
}
