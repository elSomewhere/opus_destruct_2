// svx_city tests — the underground (the subway's stations, the sewers' cell plans and what the
// feature sources draw of them) is a pure function of (world, key): the same planned forward and
// backward, with the smallest caches (an entry a shard) and from four threads at once
// (docs/CITY.md §6).
#include <doctest.h>

#include <atomic>
#include <thread>

#include "city_records.hpp"
#include "network/arterials.hpp"
#include "records.hpp"
#include "underground/sewers.hpp"
#include "underground/subway.hpp"
#include "underground_records.hpp"
#include "world/World.hpp"

using namespace svx::city;

namespace {

const test::WorldCase& city_world(const char* key) {
  static const std::vector<test::WorldCase> all = test::city_worlds();
  for (const test::WorldCase& w : all)
    if (w.key == key) return w;
  SVX_FAIL("no such world");
}

struct Node {
  int axis;
  double i, j;
};

// Everything a cell's sewers are, as text.
std::string plan_text(const SewerPlan& p) {
  rec::Out out;
  test::plan_lines(out, p, true);
  return out.text();
}

std::string station_text(const std::shared_ptr<const Station>& s) {
  if (!s) return "-";
  rec::Out out;
  test::station_lines(out, *s, true);
  return out.text();
}

// What the feature sources draw of a chunk round a sewer node (its chamber, at LOD 0 and 1).
std::string chunk_text(const World& w, const SewerNode& n) {
  const StreetLevel street_level = test::terrain_street_level(w);
  auto raster = [&](ChunkBuffer& ch, const SewerColumns* tile) {
    sewer_rasterize(w, ch, tile);
    subway_rasterize(w, ch);
  };
  std::string s = test::chunk_line(w, street_level, 0, n.x + 3, n.y - 5, n.z, raster).str();
  s += test::chunk_line(w, street_level, 1, n.x + n.qx * 12, n.y + n.qy * 12, n.zr, raster).str();
  return s;
}

}  // namespace

TEST_CASE("city underground: stations and sewer plans are the same in any order, any cache and from several threads") {
  for (const char* key : {"cities", "wrapWorld:small", "island:large", "angledInfiniteCity"}) {
    const test::WorldCase& ws = city_world(key);
    // the cells (the first of the cell network stage's: round the spawn, the towns ...) and the
    // nodes round the spawn
    std::vector<test::Cell> cells;
    std::vector<Node> nodes;
    {
      const std::unique_ptr<World> probe = test::underground_world(ws.overrides);
      rec::Samples r(5);
      cells = test::cells_of(*probe, r);
      if (cells.size() > 12) cells.resize(12);
      const CellIJ c0 = probe->arterials->cell_at(0, 0);
      for (int axis = 0; axis < 2; ++axis)
        for (double i = -2; i <= 2; i += 1)
          for (double j = -2; j <= 2; j += 1) nodes.push_back({axis, (axis == 0 ? c0.i : c0.j) + i, (axis == 0 ? c0.j : c0.i) + j});
    }
    // a fresh world, forward through its caches
    const std::unique_ptr<World> a = test::underground_world(ws.overrides);
    std::vector<std::string> want_plans;
    std::vector<std::string> want_stations;
    for (const test::Cell& c : cells) want_plans.push_back(plan_text(*a->sewers->cell_plan(c[0], c[1])));
    for (const Node& n : nodes) want_stations.push_back(a->subway ? station_text(a->subway->station(n.axis, n.i, n.j)) : "-");
    std::vector<std::shared_ptr<const SewerPlan>> held;
    std::vector<const SewerNode*> picks;
    std::vector<std::string> want_chunks;
    for (const test::Cell& c : cells) {
      std::shared_ptr<const SewerPlan> p = a->sewers->cell_plan(c[0], c[1]);
      if (!p->nodes.empty() && picks.size() < 3) {
        picks.push_back(&p->nodes[p->nodes.size() / 2]);
        want_chunks.push_back(chunk_text(*a, *picks.back()));
        held.push_back(std::move(p));
      }
    }
    // made again, past the caches
    size_t again = 0;
    for (size_t k = 0; k < cells.size(); ++k) again += plan_text(*a->sewers->plan_cell(cells[k][0], cells[k][1])) == want_plans[k] ? 0 : 1;
    if (a->subway)
      for (size_t k = 0; k < nodes.size(); ++k) {
        const Node& n = nodes[k];
        const bool has = a->subway->has_station(n.axis, n.i, n.j);
        again += station_text(has ? a->subway->build_station(n.axis, n.i, n.j) : nullptr) == want_stations[k] ? 0 : 1;
      }
    CHECK_MESSAGE(again == 0, std::string(key), ": made again, differing: ", again);
    // a fresh world with the smallest caches (an entry a shard: a few plans and stations), backwards
    const std::unique_ptr<World> b = test::underground_world(ws.overrides, 2, 1);
    size_t differ = 0;
    for (size_t k = cells.size(); k-- > 0;) differ += plan_text(*b->sewers->cell_plan(cells[k][0], cells[k][1])) == want_plans[k] ? 0 : 1;
    for (size_t k = nodes.size(); k-- > 0;) differ += (b->subway ? station_text(b->subway->station(nodes[k].axis, nodes[k].i, nodes[k].j)) : "-") == want_stations[k] ? 0 : 1;
    for (size_t k = picks.size(); k-- > 0;) differ += chunk_text(*b, *picks[k]) == want_chunks[k] ? 0 : 1;
    CHECK_MESSAGE(differ == 0, std::string(key), ": differing backwards with tiny caches: ", differ);
    // fresh worlds, four threads at once, each in an order of its own: with the caches as they
    // come, and (two worlds) with small ones
    const bool small = std::string(key) == "cities" || std::string(key) == "island:large";
    for (const size_t cache : {size_t{256}, size_t{8}}) {
      if (cache == 8 && !small) continue;
      const std::unique_ptr<World> c = test::underground_world(ws.overrides, cache, cache * 2);
      std::atomic<size_t> bad{0};
      std::vector<std::thread> threads;
      for (size_t t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
          for (size_t k = 0; k < cells.size(); ++k) {
            const size_t q = (k * 7 + t * 13 + (t & 1 ? cells.size() - 1 - k : 0)) % cells.size();
            if (plan_text(*c->sewers->cell_plan(cells[q][0], cells[q][1])) != want_plans[q]) bad.fetch_add(1);
          }
          for (size_t k = 0; k < nodes.size(); ++k) {
            const size_t q = (k * 5 + t * 29 + (t & 1 ? nodes.size() - 1 - k : 0)) % nodes.size();
            const std::string got = c->subway ? station_text(c->subway->station(nodes[q].axis, nodes[q].i, nodes[q].j)) : "-";
            if (got != want_stations[q]) bad.fetch_add(1);
          }
          if (cache == 8) return;
          for (size_t k = 0; k < picks.size(); ++k) {
            const size_t q = (k + t) % picks.size();
            if (chunk_text(*c, *picks[q]) != want_chunks[q]) bad.fetch_add(1);
          }
        });
      for (std::thread& th : threads) th.join();
      CHECK_MESSAGE(bad.load() == 0, std::string(key), ": differing on 4 threads (cache ", cache, "): ", bad.load());
    }
  }
}

TEST_CASE("city underground: a created World has the subway (unless config.subway.enabled is false) and the sewers") {
  const std::shared_ptr<World> city = create_world(city_world("cities").overrides);
  CHECK(city->subway != nullptr);
  CHECK(city->sewers != nullptr);
  // (small places - islands - go without a subway)
  const std::shared_ptr<World> island = create_world(city_world("island:small").overrides);
  CHECK(island->subway == nullptr);
  CHECK(island->sewers != nullptr);
  // a World of World.js has no underground, and no openings
  const World bare(city_world("cities").overrides);
  CHECK(!bare.blocks_surface(0, 0));
}
