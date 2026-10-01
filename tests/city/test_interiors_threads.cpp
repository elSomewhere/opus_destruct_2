// svx_city tests — a building's plan is a pure function of its envelope and the world's roads:
// World::building_plan (the World's cache of plans) gives the plans planBuilding makes, in any
// order, from several threads at once (their road levels too, from road objects of their own),
// and again after the cache dropped them.
#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "building_records.hpp"
#include "buildings/archetypes.hpp"
#include "buildings/interior/plan.hpp"
#include "config/presets.hpp"
#include "core/obb.hpp"
#include "interior_records.hpp"
#include "network/arterials.hpp"
#include "network/roadView.hpp"
#include "world/World.hpp"
#include "world/register_all.hpp"

using namespace svx::city;
using namespace svx::city::test;

namespace {

// Envelopes of every archetype, each in a cell of its own (2 + 3k, 2 + 3m), on plain and turned
// lots, their ground a little off the street level (the street doors get streets and sills).
std::vector<std::shared_ptr<const Envelope>> make_envelopes(const World& w, int n) {
  const std::vector<District> DS = district_list();
  const auto& all = archetype_registry().all();
  rec::Samples r(97);
  std::vector<std::shared_ptr<const Envelope>> out;
  for (int q = 0; q < n; ++q) {
    const Archetype& a = all[static_cast<size_t>(q) % all.size()];
    double U = 0, V = 0;
    for (int t = 0; t < 60; ++t) {
      U = 40 + std::floor(r() * 300);
      V = 40 + std::floor(r() * 300);
      if (a.fits(U, V)) break;
    }
    const District& d = DS[static_cast<size_t>(std::floor(r() * static_cast<double>(DS.size())))];
    const Rect rc = w.arterials->cell_rect(2 + 3 * (q % 10), 2 + 3 * (q / 10));
    Lot lot;
    lot.id = js::cat("C9_9/b1/l", q);
    lot.district = d.id;
    const double x0 = std::floor(rc.x0 + 800 + r() * 1500);
    const double y0 = std::floor(rc.y0 + 800 + r() * 1500);
    if (r() < 0.3) {
      Turn t;
      t.yaw = static_cast<int>(std::floor(r() * 132));
      t.origin = {x0, y0};
      t.U = U;
      t.V = V;
      lot.front = nominal_front(t.yaw);
      lot.turn = t;
      lot.rect = lot_frame_of(lot.turn, Rect{}, lot.front).R;
    } else {
      lot.front = "NESW"[static_cast<int>(std::floor(r() * 4))];
      const bool ns = lot.front == 'N' || lot.front == 'S';
      lot.rect = {x0, y0, x0 + (ns ? U : V) - 1, y0 + (ns ? V : U) - 1};
    }
    EnvelopeExtra extra;
    extra.u = r();
    extra.core = r();
    extra.ground_z = 0;
    extra.config = &w.config;
    Rng erng(std::floor(r() * 4294967296.0));
    std::optional<Envelope> env = plan_building_envelope_as(lot, a.id, "brick", d, erng, extra);
    if (!env) continue;
    // (the terrain under the lot's middle, give or take: a street there is a few voxels off)
    env->ground_z = 0;
    env->base_z = env->ground_z + (env->stoop ? 4 : 1);
    out.push_back(std::make_shared<const Envelope>(std::move(*env)));
  }
  return out;
}

// Roads along the front and one side of every envelope, new road objects for every call (a world of
// its own: their profiles made by that world's threads).
std::map<std::pair<double, double>, RoadList> make_roads(const World& w, const std::vector<std::shared_ptr<const Envelope>>& envs) {
  std::map<std::pair<double, double>, RoadList> cells;
  for (size_t k = 0; k < envs.size(); ++k) {
    const Envelope& env = *envs[k];
    const Frame& frame = envelope_frame(env);
    auto at = [&](double u, double v) { return frame.turned ? frame.point_to_world(u, v) : local_point_to_world(frame.placement, u, v); };
    const XY mid = at(env.U / 2, env.V / 2);
    const CellIJ c = w.cell_at(mid[0], mid[1]);
    RoadList roads;
    const double ends[2][2][2] = {{{-60, -12}, {env.U + 60, -12}}, {{-14, env.V + 60}, {-14, -60}}};
    for (int s = 0; s < 2; ++s) {
      auto road = std::make_shared<Road>();
      road->id = js::cat("T", k, "/r", s);
      road->cell = js::cat("T", k);
      road->cls = "local";
      for (int p = 0; p < 2; ++p) {
        const XY xy = at(ends[s][p][0], ends[s][p][1]);
        road->pts.push_back({xy[0], xy[1]});
      }
      road->hc = 24;
      road->hr = 40;
      road->corner = 6;
      road->lanes = 2;
      road->lane = 24;
      road->sidewalk = 16;
      road->ci = c.i;
      road->cj = c.j;
      roads.push_back(road);
    }
    cells[{c.i, c.j}] = roads;
  }
  return cells;
}

std::string records_of(const BuildingPlan* plan) {
  rec::Out out;
  plan_records(out, plan);
  return out.text();
}

}  // namespace

TEST_CASE("city interiors: building plans are the same from the World's cache, in any order, on four threads, after it drops them") {
  register_all();
  const Value config = preset_config("cities");
  World base(config);
  const std::vector<std::shared_ptr<const Envelope>> envs = make_envelopes(base, 120);
  REQUIRE(envs.size() > 80);

  // ---- planBuilding in order, on a world of its own
  std::vector<std::string> want;
  bool streets = false;
  {
    const auto cells = make_roads(base, envs);
    World w(config);
    w.cell_roads = [&](double i, double j) {
      auto it = cells.find({i, j});
      return it == cells.end() ? RoadList{} : it->second;
    };
    for (const auto& env : envs) {
      const std::shared_ptr<const BuildingPlan> plan = plan_building(w, *env);
      want.push_back(records_of(plan.get()));
      if (const PlanFloor* F0 = plan ? plan->floor_by_index(0) : nullptr)
        for (const auto& d : F0->grid->doors)
          if (d->street) streets = true;
    }
  }
  CHECK(streets);

  // ---- World::building_plan from four threads, each in its own order, on a fresh world
  const auto cells = make_roads(base, envs);
  World w(config);
  w.cell_roads = [&](double i, double j) {
    auto it = cells.find({i, j});
    return it == cells.end() ? RoadList{} : it->second;
  };
  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([&, t] {
      std::vector<size_t> order(envs.size());
      for (size_t i = 0; i < order.size(); ++i) order[i] = i;
      Rng rng(1000 + t);
      if (t == 1) std::reverse(order.begin(), order.end());
      if (t >= 2) order = rng.shuffle(order);
      for (size_t i : order) {
        const std::shared_ptr<const BuildingPlan> plan = w.building_plan(*envs[i]);
        if (records_of(plan.get()) != want[i]) mismatches.fetch_add(1);
      }
    });
  for (auto& th : threads) th.join();
  CHECK(mismatches.load() == 0);
  // (cached: the same plan again)
  for (size_t i = 0; i < envs.size(); i += 7) CHECK(w.building_plan(*envs[i]).get() == w.building_plan(*envs[i]).get());

  // ---- after the cache dropped them (more plans than it keeps), made again alike
  const std::vector<std::shared_ptr<const Envelope>> more = make_envelopes(base, 420);
  std::vector<std::shared_ptr<Envelope>> others;
  for (size_t k = 0; k < more.size(); ++k) {
    auto e = std::make_shared<Envelope>(*more[k]);
    e->id = js::cat(e->id, "/other", k);  // (keys of their own)
    others.push_back(e);
  }
  for (const auto& e : others) w.building_plan(*e);
  int again = 0;
  for (size_t i = 0; i < envs.size(); ++i)
    if (records_of(w.building_plan(*envs[i]).get()) != want[i]) again += 1;
  CHECK(again == 0);
}
