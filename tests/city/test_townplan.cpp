// svx_city tests — town plans (voxel_city city/townPlan.js): the landmarks of towns, the blocks
// they land on and the landmark use of blocks, against the reference (stage "townplan"), and
// their purity (the same plans and uses whichever is asked first, from several threads).
#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <thread>

#include "city/cellNetwork.hpp"
#include "city/townPlan.hpp"
#include "city_records.hpp"
#include "network/arterials.hpp"
#include "records.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"
#include "worlds.hpp"

using namespace svx::city;
using rec::Line;

namespace {

// The places a world's plans are made for (stages/townplan.mjs placesOf).
std::vector<const Settlement*> places_of(const World& w) {
  const MacroFields& F = *w.fields;
  std::vector<const Settlement*> out;
  const Settlement* spawn = F.settlement(0, 0);
  if (spawn) out.push_back(spawn);
  int more = 0;
  for (const Settlement* s : F.settlements_in({-100000, -100000, 100000, 100000}))
    if (s != spawn && more < 2) {
      out.push_back(s);
      more += 1;
    }
  if (F.wrap.on)
    if (const Settlement* s = F.settlement(F.n_town, 0)) out.push_back(s);
  const std::vector<const Settlement*> villages = F.villages_in({-60000, -60000, 60000, 60000});
  if (!villages.empty()) out.push_back(villages[0]);
  return out;
}

// A town's records: its plan, the blocks its landmarks land on, and the uses of the blocks of the
// cells round its landmarks and of three more (drawn from r).
void town_lines(rec::Out& out, const World& w, const std::string& key, const Settlement& s, rec::Samples& r) {
  constexpr double kPi = 3.141592653589793;
  const TownPlan& plan = town_plan(w, s);
  out << (Line() << "town" << key << s.id << s.x << s.y << s.radius << plan.anchors.size());
  if (plan.anchors.empty()) return;
  const std::vector<std::string>& blocks = plan.blocks(w);
  for (size_t k = 0; k < plan.anchors.size(); ++k) {
    const TownAnchor& a = plan.anchors[k];
    out << (Line() << "a" << a.kind << a.x << a.y << a.pri << test::fo(blocks[k]));
  }
  std::vector<CellIJ> cells;
  std::set<std::pair<double, double>> seen;
  auto at = [&](double x, double y) {
    const CellIJ c = w.arterials->cell_at(js::round(x), js::round(y));
    if (seen.insert({c.i, c.j}).second) cells.push_back(c);
  };
  for (const TownAnchor& a : plan.anchors) at(a.x, a.y);
  for (int k = 0; k < 3; ++k) {
    const double ang = r() * 2 * kPi;
    const double d = r() * 1.1 * s.radius;
    at(s.x + js::cos(ang) * d, s.y + js::sin(ang) * d);
  }
  for (const CellIJ& c : cells) {
    const std::shared_ptr<const CellNet> net = w.cell_net(c.i, c.j);
    std::string uses;
    for (size_t k = 0; k < net->blocks.size(); ++k) {
      const Block& b = net->blocks[k];
      const Rect& p = b.prop;
      const Rect cut{p.x0 + 12, p.y0 + 4, p.x1 - 6, p.y1 - 10};
      uses += js::cat(k ? "," : "", test::fo(landmark_use(w, b)), "/", test::fo(landmark_use(w, b.id, cut)));
    }
    out << (Line() << "uses" << net->id << c.i << c.j << uses);
  }
}

}  // namespace

TEST_CASE("city townPlan: town plans conform to the reference (stage townplan)") {
  rec::Samples r(67);
  rec::Out out;
  std::vector<std::string> kinds = landmark_kinds();
  kinds.push_back("nope");
  for (const std::string& kind : kinds) out << (Line() << "use" << kind << test::fo(landmark_use_of(kind)));
  for (const test::WorldCase& ws : test::city_worlds()) {
    const World w(ws.overrides);
    for (const Settlement* s : places_of(w)) town_lines(out, w, ws.key, *s, r);
  }
  CHECK(rec::record("townplan", out.text()) == rec::recorded_digest("townplan"));
}

TEST_CASE("city townPlan: town plans of create_world's worlds - lakes, highways, harbour grading - conform (stage townworld)") {
  rec::Samples r(71);
  rec::Out out;
  for (const test::WorldCase& ws : test::city_worlds()) {
    const std::shared_ptr<World> w = create_world(ws.overrides);
    for (const Settlement* s : places_of(*w)) town_lines(out, *w, ws.key, *s, r);
  }
  CHECK(rec::record("townworld", out.text()) == rec::recorded_digest("townworld"));
}

TEST_CASE("city townPlan: plans and landmark uses are the same whichever is asked first, from several threads") {
  for (const char* key : {"nordicTown:fjord", "island:medium", "wrapWorld:small", "oldHarbourTown"}) {
    const std::vector<test::WorldCase> all = test::city_worlds();
    const test::WorldCase* ws = nullptr;
    for (const test::WorldCase& c : all)
      if (c.key == key) ws = &c;
    REQUIRE(ws != nullptr);
    auto text = [&](const World& w, bool reverse) {
      std::vector<const Settlement*> places = places_of(w);
      if (reverse) std::reverse(places.begin(), places.end());
      std::vector<std::string> parts;
      for (const Settlement* s : places) {
        rec::Out out;
        rec::Samples r(5);
        town_lines(out, w, key, *s, r);
        parts.push_back(out.text());
      }
      if (reverse) std::reverse(parts.begin(), parts.end());
      std::string all_text;
      for (const std::string& p : parts) all_text += p;
      return all_text;
    };
    const World a(ws->overrides);
    const std::string want = text(a, false);
    // a fresh world, the places asked backwards: other towns' plans and blocks made first
    const World b(ws->overrides);
    CHECK_MESSAGE(text(b, true) == want, std::string(key), ": plans differing between orders");
    // landmark uses first (each resolving the towns round it), then the plans
    const World c(ws->overrides);
    for (const Settlement* s : places_of(c)) {
      const std::shared_ptr<const CellNet> net = c.cell_net(c.cell_at(s->x, s->y).i, c.cell_at(s->x, s->y).j);
      for (const Block& blk : net->blocks) (void)landmark_use(c, blk);
    }
    CHECK_MESSAGE(text(c, false) == want, std::string(key), ": plans differing after landmark uses");
    // four threads at once on a fresh world
    const World d(ws->overrides);
    std::vector<std::string> outs(4);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([&, t] { outs[size_t(t)] = text(d, (t & 1) != 0); });
    for (std::thread& th : threads) th.join();
    for (const std::string& o : outs) CHECK_MESSAGE(o == want, std::string(key), ": plans differing on 4 threads");
  }
}
