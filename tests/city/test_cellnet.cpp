// svx_city tests — the cell networks (voxel_city city/cellNetwork.js with city/streets.js and
// city/diagonals.js; World::cell_net) against the reference (stages "cellnet" on a World of
// World.js, "cellworld" on create_world's: lakes, highways, harbour grading), and their purity:
// the same networks in any order and from several threads (docs/CITY.md §6).
#include <doctest.h>

#include <atomic>
#include <thread>

#include "city/cellNetwork.hpp"
#include "city_records.hpp"
#include "network/arterials.hpp"
#include "records.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"
#include "world/island.hpp"
#include "worlds.hpp"

using namespace svx::city;
using rec::Line;

TEST_CASE("city cellNetwork: cell networks conform to the reference (stage cellnet)") {
  rec::Samples r(53);
  rec::Out out;
  for (const test::WorldCase& ws : test::city_worlds()) {
    const World w(ws.overrides);
    const ArterialGrid& A = *w.arterials;
    const std::vector<test::Cell> cells = test::cells_of(w, r);
    out << (Line() << "world" << ws.key << cells.size());
    for (const test::Cell& c : cells) test::net_lines(out, *plan_cell_network(w, c[0], c[1]));
    for (int k = 0; k < 24; ++k) {
      const int axis = r() < 0.5 ? 0 : 1;
      const double ln = std::floor((r() - 0.5) * 300);
      const double span = std::floor((r() - 0.5) * 300);
      out << test::edge_line("e", edge_info(w, axis, ln, span));
    }
    if (w.fields->island) {
      const Rect b = w.fields->island->bounds();
      for (int k = 0; k < 60; ++k) {
        const double x = js::round((b.x0 + r() * (b.x1 - b.x0)) * 8);
        const double y = js::round((b.y0 + r() * (b.y1 - b.y0)) * 8);
        const double x1 = x + std::floor(r() * 3000);
        const double y1 = y + std::floor(r() * 3000);
        const Rect rect{x, y, x1, y1};
        const double m = std::floor(r() * 12) - 2;
        const double bx = x + (r() - 0.5) * 6000;
        const double by = y + (r() - 0.5) * 6000;
        out << (Line() << "sea" << x << y << w.sea_at(x, y) << w.sea_at(x, y, m) << w.sea_hits_rect(rect) << w.sea_hits_rect(rect, m) << w.sea_share(rect)
                       << w.sea_hits_seg(x, y, bx, by) << w.sea_hits_seg(x, y, bx, by, m));
      }
    }
    (void)A;
  }
  CHECK(rec::record("cellnet", out.text()) == rec::recorded_digest("cellnet"));
}

TEST_CASE("city cellNetwork: the cell networks of create_world's worlds - lakes, highways, harbour grading - conform (stage cellworld)") {
  rec::Samples r(59);
  rec::Out out;
  for (const test::WorldCase& ws : test::city_worlds()) {
    const std::shared_ptr<World> w = create_world(ws.overrides);
    const std::vector<test::Cell> cells = test::cells_of(*w, r);
    out << (Line() << "world" << ws.key << cells.size());
    for (const test::Cell& c : cells) test::net_lines(out, *plan_cell_network(*w, c[0], c[1]));
  }
  CHECK(rec::record("cellworld", out.text()) == rec::recorded_digest("cellworld"));
}

namespace {

std::string net_text(const CellNet& net) {
  rec::Out out;
  test::net_lines(out, net);
  return out.text();
}

const test::WorldCase& city_world(const char* key) {
  static const std::vector<test::WorldCase> all = test::city_worlds();
  for (const test::WorldCase& w : all)
    if (w.key == key) return w;
  SVX_FAIL("no such world");
}

}  // namespace

TEST_CASE("city cellNetwork: a cell's network is the same in any order and from several threads") {
  // (towns, villages, an island's trunk roads and harbour, a wrapping world's laps, the angled
  // world's diagonals and crooked lanes: everything a network reads that is made on first use)
  for (const char* key : {"cities", "nordicTown:fjord", "island:medium", "wrapWorld:small", "angledOldHarbourTown", "angledNordicTown:forest"}) {
    const test::WorldCase& ws = city_world(key);
    std::vector<test::Cell> cells;
    {
      const World probe(ws.overrides);
      rec::Samples r(71);
      cells = test::cells_of(probe, r);
    }
    // fresh worlds: one planned in the order drawn (through the World's cache), one backwards
    const World a(ws.overrides);
    std::vector<std::string> want;
    for (const test::Cell& c : cells) want.push_back(net_text(*a.cell_net(c[0], c[1])));
    const World b(ws.overrides);
    size_t differ = 0;
    for (size_t k = cells.size(); k-- > 0;) differ += net_text(*plan_cell_network(b, cells[k][0], cells[k][1])) == want[k] ? 0 : 1;
    CHECK_MESSAGE(differ == 0, std::string(key), ": networks differing between orders: ", differ);
    // the cache hands out what it made; made again, the same
    size_t again = 0;
    for (size_t k = 0; k < cells.size(); ++k) {
      again += net_text(*a.cell_net(cells[k][0], cells[k][1])) == want[k] ? 0 : 1;
      again += net_text(*plan_cell_network(a, cells[k][0], cells[k][1])) == want[k] ? 0 : 1;
    }
    CHECK_MESSAGE(again == 0, std::string(key), ": networks differing when made again: ", again);
    // a fresh world, four threads at once, each in an order of its own
    const World c(ws.overrides);
    std::atomic<size_t> bad{0};
    std::vector<std::thread> threads;
    for (size_t t = 0; t < 4; ++t)
      threads.emplace_back([&, t] {
        for (size_t k = 0; k < cells.size(); ++k) {
          const size_t i = (k * 7 + t * 13 + (t & 1 ? cells.size() - 1 - k : 0)) % cells.size();
          if (net_text(*c.cell_net(cells[i][0], cells[i][1])) != want[i]) bad.fetch_add(1);
        }
      });
    for (std::thread& th : threads) th.join();
    CHECK_MESSAGE(bad.load() == 0, std::string(key), ": networks differing on 4 threads: ", bad.load());
  }
}
