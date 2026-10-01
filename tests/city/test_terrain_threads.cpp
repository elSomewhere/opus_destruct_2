// svx_city tests — the world base on several threads at once: one World's fields, terrain and
// island plan queried concurrently give the single-threaded results (once every settlement's base
// height is made: the first sample that makes one reads what that left in its thread's context, as
// the reference's samples do in theirs).
#include <doctest.h>

#include <atomic>
#include <thread>

#include "records.hpp"
#include "terrain/terrain.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"
#include "world/island.hpp"
#include "worlds.hpp"

using namespace svx::city;

namespace {

struct Got {
  double h, natural, u, rugged, coast, mountain, stream_d;
};

Got got(const TerrainSample& s) { return {s.h, s.natural, s.u, s.rugged, s.coast, s.mountain, s.stream ? s.stream->d : -1}; }

bool same(const Got& a, const Got& b) {
  return a.h == b.h && a.natural == b.natural && a.u == b.u && a.rugged == b.rugged && a.coast == b.coast && a.mountain == b.mountain &&
         a.stream_d == b.stream_d;
}

}  // namespace

TEST_CASE("city world base: one World queried from several threads at once") {
  for (const char* key : {"cities", "nordicTown:fjord", "wrapWorld:small"}) {
    const test::WorldSpec* spec = nullptr;
    for (const test::WorldSpec& ws : test::worlds())
      if (std::string(ws.key) == key) spec = &ws;
    REQUIRE(spec);
    const World w(test::world_overrides(*spec));
    rec::Samples r(41);
    std::vector<test::Point> pts = test::sample_points(w, r);
    pts.resize(std::min<size_t>(pts.size(), 1200));
    // make every base height (the reference's first touches), then the single-threaded answers
    for (const test::Point& p : pts) w.terrain->sample(p[0], p[1]);
    std::vector<Got> want;
    for (const test::Point& p : pts) want.push_back(got(w.terrain->sample(p[0], p[1])));
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
      threads.emplace_back([&, t] {
        for (size_t k = 0; k < pts.size(); ++k) {
          const size_t i = (k * 7 + static_cast<size_t>(t) * 131) % pts.size();
          if (!same(got(w.terrain->sample(pts[i][0], pts[i][1])), want[i])) bad.fetch_add(1);
        }
      });
    for (std::thread& th : threads) th.join();
    CHECK_MESSAGE(bad.load() == 0, key);
  }
  // lazy products raced on a fresh world: the island's places, harbour and trunk roads, and the
  // lattice settlements
  for (const char* key : {"nordicTown:fjord", "cities"}) {
    const test::WorldSpec* spec = nullptr;
    for (const test::WorldSpec& ws : test::worlds())
      if (std::string(ws.key) == key) spec = &ws;
    const World a(test::world_overrides(*spec));
    const World b(test::world_overrides(*spec));
    auto summary = [](const World& w) {
      std::string s;
      for (const Settlement* t : w.fields->settlements_in({-200000, -200000, 200000, 200000})) s += t->id + js::cat(":", t->x, ",", t->t, ";");
      for (const Settlement* v : w.fields->villages_in({-100000, -100000, 100000, 100000})) s += v->id + js::cat(":", v->y, ",", v->m, ";");
      if (w.fields->island) {
        const auto& h = w.fields->island->harbour();
        s += h ? js::cat("H", h->x, ",", h->y) : std::string("H-");
        for (const auto& e : w.fields->island->trunk_edges(w).items()) s += js::cat(e[0], ":", e[1], ":", e[2], ";");
      }
      return s;
    };
    const std::string want = summary(a);
    std::vector<std::string> outs(4);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([&, t] { outs[size_t(t)] = summary(b); });
    for (std::thread& th : threads) th.join();
    for (const std::string& o : outs) CHECK(o == want);
  }
}
