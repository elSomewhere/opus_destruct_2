// svx_city tests — lots (voxel_city city/lots.js) against the reference (stage "lots"): the blocks
// of the cell networks of every city world and synthetic blocks, by every lot mode and the cell
// plan's other lot functions; mainFrontage.
#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "city/cellNetwork.hpp"
#include "city/districts.hpp"
#include "city/lots.hpp"
#include "city_records.hpp"
#include "records.hpp"
#include "world/World.hpp"
#include "world/register_all.hpp"
#include "worlds.hpp"

using namespace svx::city;
using rec::Line;

namespace {

// districts.js's districts, in registration order (stages/lots.mjs DISTRICT_IDS)
const char* const kDistrictIds[17] = {"downtown", "midtown", "mixed", "residential", "suburban", "industrial", "heavyIndustry", "microdistrict", "projects",
                                      "port",     "harbour", "oldtown", "oldcore",     "village",  "sea",        "park",          "rural"};
const char kNSWE[4] = {'N', 'S', 'W', 'E'};

std::string ffront(const std::vector<Frontage>& fr) {
  std::string s;
  for (size_t i = 0; i < fr.size(); ++i) s += js::cat(i ? ";" : "", fr[i].side, "/", fr[i].cls);
  return s;
}

// (a flag JS sets to true or leaves undefined; a string it sets or leaves undefined)
void flag(Line& l, bool b) {
  if (b)
    l << true;
  else
    l << rec::kUndef;
}

Line lot_line(const Lot& lot) {
  Line l;
  l << "l" << test::fo(lot.id) << test::frect(lot.rect) << lot.block << lot.cell << lot.district << ffront(lot.frontages) << lot.front;
  if (lot.alley)
    l << lot.alley;
  else
    l << rec::kUndef;
  l << lot.corner;
  flag(l, lot.whole);
  flag(l, lot.farmstead);
  l << test::fo(lot.farm_role) << test::fo(lot.farm);
  flag(l, lot.village);
  flag(l, lot.micro);
  l << test::fo(lot.arch);
  flag(l, lot.cabin);
  flag(l, lot.chapel);
  l << test::fo(lot.civic);
  return l;
}

void lots_of(rec::Out& out, const std::string& tag, const Block& block, const std::vector<Lot>& lots) {
  out << (Line() << tag << block.id << lots.size());
  for (const Lot& lot : lots) out << lot_line(lot);
}

// stages/lots.mjs otherLots: microLots, rowLotsOf, wholeBlockLot, freeLot, the k-th in turn
void other_lots(rec::Out& out, const Block& block, int k, Rng& rng, rec::Samples& r) {
  const Rect& p = block.prop;
  switch (k % 4) {
    case 0:
      lots_of(out, "micro", block, micro_lots(block, rng));
      return;
    case 1: {
      const std::array<double, 2> widths[3] = {{7, 11}, {12, 26}, {30, 60}};
      const std::array<double, 2>& width = widths[static_cast<size_t>(std::floor(r() * 3))];
      lots_of(out, js::cat("row", width[0]), block, row_lots_of(block, rng, width));
      return;
    }
    case 2:
      lots_of(out, "whole", block, {whole_block_lot(block)});
      return;
    default: {
      const double x0 = p.x0 + std::floor(r() * js::max(1, p.x1 - p.x0));
      const double y0 = p.y0 + std::floor(r() * js::max(1, p.y1 - p.y0));
      const double x1 = js::min(p.x1, x0 + 8 + std::floor(r() * 300));
      const double y1 = js::min(p.y1, y0 + 8 + std::floor(r() * 300));
      const char front = kNSWE[static_cast<size_t>(std::floor(r() * 4))];
      const double e = std::floor(r() * 4);
      Lot lot = free_lot(block, Rect{x0, y0, x1, y1}, front);
      if (e == 0)
        lot.cabin = true;
      else if (e == 1)
        lot.chapel = true;
      else if (e == 2)
        lot.civic = "museum";
      lots_of(out, "free", block, {lot});
    }
  }
}

// stages/lots.mjs side(r)
RoadSide side(rec::Samples& r) {
  const char* const cls_list[10] = {"arterial", "collector", "local", "village", "pedestrian", "rural", "lane", "alley", nullptr, nullptr};
  const char* cls = cls_list[static_cast<size_t>(std::floor(r() * 10))];
  RoadSide s;
  if (!cls) return s;
  s.cls = cls;
  s.hr = 8 + std::floor(r() * 40);
  if (r() < 0.5) s.id = js::cat("S/r", std::floor(r() * 100));
  return s;
}

}  // namespace

TEST_CASE("city lots: lots conform to the reference (stage lots)") {
  register_all();
  rec::Samples r(83);
  rec::Out out;
  // the blocks of the cell networks of every world
  size_t alt = 0;
  int other = 0;
  for (const test::WorldCase& ws : test::city_worlds()) {
    const World w(ws.overrides);
    const std::vector<test::Cell> cells = test::cells_of(w, r);
    out << (Line() << "world" << ws.key << cells.size());
    for (const test::Cell& c : cells) {
      const std::shared_ptr<const CellNet> net = plan_cell_network(w, c[0], c[1]);
      out << (Line() << "cell" << net->id << net->blocks.size());
      for (size_t k = 0; k < net->blocks.size(); ++k) {
        const Block& b = net->blocks[k];
        if (k % 6 == 0) {
          Rng rng = Rng::from(w.seed, b.id, "use");
          lots_of(out, "own", b, plan_block_lots(b, district_registry().get(b.district), rng));
        } else if (k % 6 == 2) {
          const std::string id = kDistrictIds[alt % 17];
          alt += 1;
          Rng rng = Rng::from(w.seed, b.id, "alt");
          lots_of(out, id, b, plan_block_lots(b, district_registry().get(id), rng));
        } else if (k % 6 == 4) {
          Rng rng = Rng::from(w.seed, b.id, "other");
          other_lots(out, b, other, rng, r);
          other += 1;
        }
      }
    }
  }
  // synthetic blocks
  for (int k = 0; k < 400; ++k) {
    const std::string id = kDistrictIds[k % 17];
    const District& d = district_registry().get(id);
    const double big = d.lots.mode == "rural" || d.lots.mode == "village" ? 4000 : d.lots.mode == "industrial" ? 2400 : 1200;
    const double x0 = std::floor((r() - 0.5) * 200000);
    const double y0 = std::floor((r() - 0.5) * 200000);
    const double a = r();
    const double b = r();
    const double c = r();
    const double e = r();
    const double width = std::floor(a * b * big);
    const double height = std::floor(c * e * big);
    Block block;
    block.sides.N = side(r);
    block.sides.E = side(r);
    block.sides.S = side(r);
    block.sides.W = side(r);
    block.id = js::cat("S", k, "/b", k % 7);
    block.cell = js::cat("S", k);
    block.district = id;
    block.prop = {x0, y0, x0 + width, y0 + height};
    out << (Line() << "syn" << block.id << id << test::frect(block.prop) << test::fside(block.sides.N) << test::fside(block.sides.E) << test::fside(block.sides.S)
                   << test::fside(block.sides.W));
    Rng rng = Rng::from(77, block.id, "plan");
    lots_of(out, "plan", block, plan_block_lots(block, d, rng));
    for (int q = 0; q < 4; ++q) {
      Rng qrng = Rng::from(77, block.id, q);
      other_lots(out, block, q, qrng, r);
    }
  }
  // mainFrontage
  const char* const fcls[10] = {"arterial", "collector", "local", "village", "pedestrian", "rural", "lane", "alley", "highway", "street"};
  for (int k = 0; k < 300; ++k) {
    const double x0 = std::floor((r() - 0.5) * 20000);
    const double y0 = std::floor((r() - 0.5) * 20000);
    const double x1 = x0 + std::floor(r() * 600);
    const double y1 = y0 + std::floor(r() * 600);
    const Rect rect{x0, y0, x1, y1};
    const int n = static_cast<int>(std::floor(r() * 5));
    std::vector<Frontage> fr;
    for (int q = 0; q < n; ++q) {
      Frontage f;
      f.side = kNSWE[static_cast<size_t>(std::floor(r() * 4))];
      f.cls = fcls[static_cast<size_t>(std::floor(r() * 10))];
      fr.push_back(f);
    }
    const char m = main_frontage(rect, fr);
    Line l;
    l << "mf" << test::frect(rect) << ffront(fr);
    if (m)
      l << m;
    else
      l << rec::kUndef;
    out << l;
  }
  CHECK(rec::record("lots", out.text()) == rec::recorded_digest("lots"));
}

TEST_CASE("city lots: the lot record's defaults are JS's undefined") {
  register_all();
  Block block;
  block.id = "C0_0/b3";
  block.cell = "C0_0";
  block.district = "residential";
  block.prop = {0, 0, 199, 99};
  block.sides.N.cls = "local";
  block.sides.S.cls = "alley";
  const Lot whole = whole_block_lot(block);
  CHECK(whole.id == "C0_0/b3/l0");
  CHECK(whole.whole);
  CHECK(!whole.whole_rect);
  CHECK(whole.front == 'N');
  CHECK(whole.alley == 'S');
  CHECK(!whole.corner);
  CHECK(std::isnan(whole.ground_z));
  CHECK(whole.building.empty());
  CHECK(!whole.turn);
  const Lot free = free_lot(block, Rect{10, 10, 40, 40}, 'E');
  CHECK(free.id.empty());
  CHECK(free.front == 'E');
  CHECK(free.frontages.empty());
  CHECK(free.alley == 0);
}
