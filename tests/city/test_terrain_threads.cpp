// svx_city tests — the world base is a pure function of (world, position, arguments): terrain
// samples, macro fields and the island's lazy products come out the same in any call order, on
// fresh or used worlds, and from several threads at once (docs/CITY.md §6: unlike the reference,
// whose shared terrain context lets a nested call change the outer sample).
#include <doctest.h>

#include <algorithm>
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
  double h, natural, u, rugged, coast, mountain, grade, stream_d;
  bool operator==(const Got& o) const {
    return h == o.h && natural == o.natural && u == o.u && rugged == o.rugged && coast == o.coast && mountain == o.mountain && grade == o.grade &&
           stream_d == o.stream_d;
  }
};

Got got(const TerrainSample& s) { return {s.h, s.natural, s.u, s.rugged, s.coast, s.mountain, s.grade, s.stream ? s.stream->d : -1}; }

const test::WorldSpec& spec(const char* key) {
  for (const test::WorldSpec& ws : test::worlds())
    if (std::string(ws.key) == key) return ws;
  SVX_FAIL("no such world");
}

// The points a sampling pass visits, in the order of `order` (indices into pts).
std::vector<Got> sample_in(const World& w, const std::vector<test::Point>& pts, const std::vector<size_t>& order) {
  std::vector<Got> out(pts.size());
  for (size_t i : order) out[i] = got(w.terrain->sample(pts[i][0], pts[i][1]));
  return out;
}

}  // namespace

TEST_CASE("city world base: terrain samples are the same in any order and from several threads") {
  for (const char* key : {"cities", "island:small", "nordicTown:fjord", "wrapWorld:small"}) {
    const test::WorldSpec& ws = spec(key);
    std::vector<test::Point> pts;
    {
      const World probe(test::world_overrides(ws));
      rec::Samples r(43);
      pts = test::sample_points(probe, r);
    }
    std::vector<size_t> forward(pts.size()), backward(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) forward[i] = i, backward[i] = pts.size() - 1 - i;
    // fresh worlds: every settlement's base height is made by whichever sample first needs it
    const World a(test::world_overrides(ws));
    const World b(test::world_overrides(ws));
    const std::vector<Got> want = sample_in(a, pts, forward);
    const std::vector<Got> rev = sample_in(b, pts, backward);
    size_t differ = 0;
    for (size_t i = 0; i < pts.size(); ++i) differ += want[i] == rev[i] ? 0 : 1;
    CHECK_MESSAGE(differ == 0, std::string(key), ": samples differing between orders: ", differ);
    // a used world gives what it gave
    size_t again = 0;
    for (size_t i = 0; i < pts.size(); ++i) again += got(a.terrain->sample(pts[i][0], pts[i][1])) == want[i] ? 0 : 1;
    CHECK_MESSAGE(again == 0, std::string(key), ": samples differing on a used world: ", again);
    // a fresh world sampled from four threads at once, each in its own order
    const World c(test::world_overrides(ws));
    std::atomic<size_t> bad{0};
    std::vector<std::thread> threads;
    for (size_t t = 0; t < 4; ++t)
      threads.emplace_back([&, t] {
        for (size_t k = 0; k < pts.size(); ++k) {
          const size_t i = (k * 7 + t * 131 + (t & 1 ? pts.size() - 1 - k : 0)) % pts.size();
          if (!(got(c.terrain->sample(pts[i][0], pts[i][1])) == want[i])) bad.fetch_add(1);
        }
      });
    for (std::thread& th : threads) th.join();
    CHECK_MESSAGE(bad.load() == 0, std::string(key), ": samples differing on 4 threads: ", bad.load());
  }
}

TEST_CASE("city world base: lazy products are the same whoever makes them first") {
  for (const char* key : {"nordicTown:fjord", "island:medium", "cities", "wrapWorld:small"}) {
    const test::WorldSpec& ws = spec(key);
    auto summary = [](const World& w) {
      std::string s;
      for (const Settlement* t : w.fields->settlements_in({-200000, -200000, 200000, 200000}))
        s += t->id + js::cat(":", t->x, ",", t->t, ",", w.terrain->settlement_base(*t), ";");
      for (const Settlement* v : w.fields->villages_in({-100000, -100000, 100000, 100000}))
        s += v->id + js::cat(":", v->y, ",", v->m, ",", w.terrain->settlement_base(*v), ";");
      if (w.fields->island) {
        const auto& h = w.fields->island->harbour();
        s += h ? js::cat("H", h->x, ",", h->y) : std::string("H-");
        for (const auto& e : w.fields->island->trunk_edges(w).items()) s += js::cat(e[0], ":", e[1], ":", e[2], ";");
      }
      return s;
    };
    // the trunk roads before anything else, or after samples made the base heights
    const World a(test::world_overrides(ws));
    const std::string want = summary(a);
    const World b(test::world_overrides(ws));
    for (int k = -20; k <= 20; ++k) b.terrain->sample(k * 997.0, k * -613.0);
    CHECK(summary(b) == want);
    // raced from four threads on a fresh world
    const World c(test::world_overrides(ws));
    std::vector<std::string> outs(4);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([&, t] { outs[size_t(t)] = summary(c); });
    for (std::thread& th : threads) th.join();
    for (const std::string& o : outs) CHECK(o == want);
  }
}
