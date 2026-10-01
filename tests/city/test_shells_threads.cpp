// svx_city tests — the building shells and the island landmarks from several threads: an envelope's
// frame, look and roof snow cover are made on first use by whichever thread draws it first (massing,
// wings, the building source), and an island's landmarks are planned once by whichever thread asks
// first; every thread gets what one thread alone draws, in any order.
#include <doctest.h>

#include <atomic>
#include <map>
#include <thread>
#include <utility>

#include "buildings/archetypes.hpp"
#include "buildings/source.hpp"
#include "buildings/styles.hpp"
#include "buildings/wings.hpp"
#include "records.hpp"
#include "road_inputs.hpp"
#include "shell_records.hpp"
#include "voxel/chunk.hpp"
#include "world/landmarks.hpp"
#include "world/register_all.hpp"
#include "worlds.hpp"

using namespace svx::city;
using namespace svx::city::test;

namespace {

// The chunks drawn of one envelope with wings: the building source (grid mode) round its first
// wing at LOD 0 and 1, and the wing in its lattice at LOD 0 and 2.
std::vector<std::vector<uint16_t>> draw(const World& w, const std::shared_ptr<const Envelope>& env) {
  std::vector<std::vector<uint16_t>> out;
  const Wing& q = env->wings[0];
  const EnvelopeList envs = {env};
  for (int lod : {0, 1}) {
    const double E = static_cast<double>(32 << lod);
    ChunkBuffer c(lod, std::floor(((q.bounds.x0 + q.bounds.x1) / 2) / E), std::floor(((q.bounds.y0 + q.bounds.y1) / 2) / E), std::floor(((q.z0 + q.z1) / 2) / E));
    rasterize_buildings(w, envs, c);
    out.push_back(c.data);
  }
  for (int lod : {0, 2}) {
    const double E = static_cast<double>(32 << lod);
    ChunkBuffer c(lod, 0, 0, std::floor(((q.z0 + q.z1) / 2) / E));
    rasterize_wing_part(w, *env, q, c);
    out.push_back(c.data);
  }
  return out;
}

}  // namespace

TEST_CASE("city shells: massing, wings and the building source draw the same from four threads") {
  register_all();
  rec::Samples r(67);
  const std::vector<District> DS = district_list();
  std::vector<std::string> styles;
  for (const Style& s : style_registry().all()) styles.push_back(s.id);
  Value cfg;
  REQUIRE(Value::parse_json(shell_worlds()[1], &cfg));
  World w(cfg);
  CellRoads cell_roads;
  serve_cell_roads(w, cell_roads);
  const RoadSpecs specs(w.config);
  // envelopes with wings (the wings stage's sites), each kept twice: one drawn alone first, one
  // fresh for the threads
  std::vector<std::shared_ptr<const Envelope>> alone, shared;
  for (double k = 0; k < 242 && shared.size() < 24; k += 1) {
    const double i = 3 * std::fmod(k, 11) - 15;
    const double j = 3 * std::floor(k / 11) - 33;
    WingSite site = wing_site(r, w, cell_roads, i, j, k, DS, styles, specs);
    if (!site.env) continue;
    Envelope env = *site.env;
    Rng rng = Rng::from(w.seed, env.id, "wings");
    const Block* block = site.block ? &*site.block : nullptr;
    const std::vector<Wing> ws = plan_wings(w, env, &site.lot, block, *site.view, site.cell_id, rng);
    if (ws.empty()) continue;
    env.wings = ws;
    for (const Wing& q : ws)
      if (q.chamfer) env.chamfer = *q.chamfer;
    alone.push_back(std::make_shared<const Envelope>(env));
    shared.push_back(std::make_shared<const Envelope>(env));
  }
  REQUIRE(shared.size() >= 12);
  std::vector<std::vector<std::vector<uint16_t>>> expect;
  for (const auto& e : alone) expect.push_back(draw(w, e));
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([&, t] {
      for (size_t n = 0; n < shared.size(); ++n) {
        const size_t i = t % 2 ? shared.size() - 1 - n : (n * 5 + static_cast<size_t>(t)) % shared.size();
        if (draw(w, shared[i]) != expect[i]) bad += 1;
      }
    });
  for (std::thread& th : threads) th.join();
  CHECK(bad.load() == 0);
}

TEST_CASE("city landmarks: an island's landmarks are planned once, the same from four threads") {
  const Value data = read_json_file(std::string(SVX_SOURCE_DIR) + "/tools/procgen_ref/data/landmarks.json");
  for (const WorldCase& wc : all_worlds()) {
    if (wc.key != "islandTiny" && wc.key != "nordicTown:fjord") continue;
    auto answers = std::make_shared<std::map<std::pair<double, double>, bool>>();
    for (const Value& a : data[wc.key].items()) (*answers)[{a[size_t{0}].to_number(), a[size_t{1}].to_number()}] = a[size_t{2}].to_number() != 0;
    auto serve = [answers](double x, double y) {
      auto it = answers->find({x, y});
      return it != answers->end() && it->second;
    };
    // what one thread alone plans
    const std::shared_ptr<World> w1 = create_world(wc.overrides);
    w1->landmarks->free_source = serve;
    std::vector<std::string> expect;
    for (const Landmark& it : w1->landmarks->all()) expect.push_back(js::cat(it.kind, ":", it.bb.x0, ",", it.bb.y0, ",", it.bb.z0, ",", it.bb.z1));
    REQUIRE(!expect.empty());
    const Rect foot = w1->landmarks->all()[0].foot;
    // four threads asking a fresh world's at once
    const std::shared_ptr<World> w2 = create_world(wc.overrides);
    w2->landmarks->free_source = serve;
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
      threads.emplace_back([&, t] {
        if (t == 1 && !w2->landmarks->blocks(foot.x0, foot.y0)) bad += 1;
        if (t == 2 && w2->landmarks->near(foot).empty()) bad += 1;
        std::vector<std::string> got;
        for (const Landmark& it : w2->landmarks->all()) got.push_back(js::cat(it.kind, ":", it.bb.x0, ",", it.bb.y0, ",", it.bb.z0, ",", it.bb.z1));
        if (got != expect) bad += 1;
      });
    for (std::thread& th : threads) th.join();
    CHECK(bad.load() == 0);
  }
}
