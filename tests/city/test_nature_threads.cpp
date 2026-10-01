// svx_city tests — the nature of a created world (land cover, rivers, lakes and their port lakes,
// the harbour grading of the terrain, the water predicates, caves) is a pure function of (world,
// position, arguments): the same in any call order, with any lake cache size and from several
// threads at once (docs/CITY.md §6: unlike the reference, where the first terrain sample near a
// harbour town plans its port lake inside the shared terrain context).
#include <doctest.h>

#include <atomic>
#include <thread>

#include "core/hash.hpp"
#include "nature/caves.hpp"
#include "nature/lakes.hpp"
#include "nature/landcover.hpp"
#include "nature/rivers.hpp"
#include "records.hpp"
#include "terrain/terrain.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"
#include "worlds.hpp"

using namespace svx::city;

namespace {

const test::WorldSpec& spec(const char* key) {
  for (const test::WorldSpec& ws : test::worlds())
    if (std::string(ws.key) == key) return ws;
  SVX_FAIL("no such world");
}

// Everything the nature answers at a point, as text.
std::string got(const World& w, double x, double y) {
  std::string s;
  const TerrainSample ts = w.terrain->sample(x, y);
  s += js::cat(ts.h, ",", ts.rugged, ",", ts.coast, ";");
  if (const auto ri = w.rivers->at(x, y)) s += js::cat("r", ri->d, ",", ri->half, ",", ri->water, ",", ri->bed, ";");
  s += js::cat(w.rivers->channel_gap(x, y), ";");
  if (const auto lk = w.lakes->at(x, y)) s += js::cat("l", lk->lake->id, ",", lk->k, ",", lk->bed, ",", lk->lake->level, ";");
  s += js::cat(w.is_wet(x, y), w.open_water_at(x, y), w.sea_at(x, y, 10), ";");
  if (const auto sh = w.shore_near(x, y, 4000)) s += js::cat("s", sh->dist, ",", sh->nx, ",", sh->lake ? sh->lake->id : std::string("sea"), ";");
  const Rect r{x - 200, y - 150, x + 300, y + 250};
  s += js::cat(w.water_hits_rect(r, 4), w.sea_share(r), ";");
  const LandCover& lc = *w.land_cover;
  const Surface su = lc.surface(x, y, js::round(ts.h), 0.2, ts.u);
  s += js::cat(su.top, ",", su.sub, ",", su.pond, ",", su.bump, ",", lc.forest_density(x, y, ts.u), ",", lc.is_farmland(x, y, ts.u), ";");
  s += js::cat(w.caves->region(x, y), ",", w.caves->field(x, y, ts.h - 40, 5));
  return s;
}

std::vector<std::string> run(const World& w, const std::vector<test::Point>& pts, const std::vector<size_t>& order) {
  std::vector<std::string> out(pts.size());
  for (size_t i : order) out[i] = got(w, pts[i][0], pts[i][1]);
  return out;
}

// The port lakes of the towns round the spawn.
std::string ports(const World& w) {
  std::string s;
  for (const Settlement* t : w.fields->settlements_in({-96000, -96000, 96000, 96000})) {
    const std::shared_ptr<const Lake> L = w.lakes->port_lake_of(*t);
    s += t->id + ":" + (L ? L->id : std::string("-")) + ";";
  }
  return s;
}

}  // namespace

TEST_CASE("city nature: water, land cover and caves are the same in any order, any lake cache and from several threads") {
  for (const char* key : {"cities", "wrapWorld:small", "island:large", "nordicTown:fjord"}) {
    const Value overrides = test::world_overrides(spec(key));
    // the points: round the spawn, round the harbour towns and round the lakes (planned on a probe)
    std::vector<test::Point> pts;
    {
      const std::shared_ptr<World> probe = create_world(overrides);
      rec::Samples r(97);
      for (int k = 0; k < 80; ++k) {
        const double x = js::round((r() - 0.5) * 192000);
        const double y = js::round((r() - 0.5) * 192000);
        pts.push_back({x, y});
      }
      for (const Settlement* t : probe->fields->settlements_in({-96000, -96000, 96000, 96000}))
        if (const std::shared_ptr<const Lake> L = probe->lakes->port_lake_of(*t))
          for (int k = 0; k < 24; ++k) {
            const double f = r() * 1.2;
            const double j = (r() - 0.5) * t->radius;
            pts.push_back({js::round(t->x + (L->x - t->x) * f + j), js::round(t->y + (L->y - t->y) * f - j)});
          }
      for (const std::shared_ptr<const Lake>& L : probe->lakes->near({-96000, -96000, 96000, 96000}))
        for (int k = 0; k < 3; ++k) {
          const double a = r() * 6.283185307179586;
          const double d = r() * 1.6 * L->r0;
          pts.push_back({js::round(L->x + js::cos(a) * d), js::round(L->y + js::sin(a) * d)});
        }
    }
    std::vector<size_t> forward(pts.size()), backward(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) forward[i] = i, backward[i] = pts.size() - 1 - i;
    // fresh worlds: whichever query comes first plans the port lakes, lakes and base heights
    const std::shared_ptr<World> a = create_world(overrides);
    const std::shared_ptr<World> b = create_world(overrides);
    const std::vector<std::string> want = run(*a, pts, forward);
    const std::vector<std::string> rev = run(*b, pts, backward);
    size_t differ = 0;
    for (size_t i = 0; i < pts.size(); ++i) differ += want[i] == rev[i] ? 0 : 1;
    CHECK_MESSAGE(differ == 0, key, ": points differing between orders: ", differ, " of ", pts.size());
    CHECK(ports(*a) == ports(*b));
    // a lake cache of 16 cells (an at() reads 25): lakes made again and again (every fifth point,
    // from the last)
    const std::shared_ptr<World> c = create_world(overrides);
    c->lakes = std::make_shared<Lakes>(*c, 16);
    size_t differ_tiny = 0;
    for (size_t k = 0; k < pts.size(); k += 5) {
      const size_t i = pts.size() - 1 - k;
      differ_tiny += got(*c, pts[i][0], pts[i][1]) == want[i] ? 0 : 1;
    }
    CHECK_MESSAGE(differ_tiny == 0, key, ": points differing with a small lake cache: ", differ_tiny);
    CHECK(ports(*c) == ports(*a));
    // a fresh world queried from four threads at once, each in its own order
    const std::shared_ptr<World> d = create_world(overrides);
    std::atomic<size_t> bad{0};
    std::vector<std::thread> threads;
    for (size_t t = 0; t < 4; ++t)
      threads.emplace_back([&, t] {
        for (size_t k = 0; k < pts.size(); ++k) {
          const size_t i = (k * 7 + t * 131 + (t & 1 ? pts.size() - 1 - k : 0)) % pts.size();
          if (got(*d, pts[i][0], pts[i][1]) != want[i]) bad.fetch_add(1);
        }
      });
    for (std::thread& th : threads) th.join();
    CHECK_MESSAGE(bad.load() == 0, key, ": points differing on 4 threads: ", bad.load());
    CHECK(ports(*d) == ports(*a));
  }
}
