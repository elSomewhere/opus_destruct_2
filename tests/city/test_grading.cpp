// svx_city tests — site grading (voxel_city city/grading.js) against the reference (stage
// "grading"): levelLot and SiteGrading.at over scripted cells of lots and envelopes.
#include <doctest.h>

#include <deque>
#include <map>

#include "buildings/archetypes.hpp"
#include "buildings/styles.hpp"
#include "city/grading.hpp"
#include "records.hpp"
#include "shell_records.hpp"
#include "world/register_all.hpp"

using namespace svx::city;
using namespace svx::city::test;
using rec::Line;

namespace {

const char* const kKinds[] = {"house", "school", "wharfhouse", "petrolStation", "supermarket", "walkup", "townHall", "museum", "library", "cabin", "garage", "midrise",
                              "barn", "warehouse", "church", "hotel"};

// stages/grading.mjs clusterLot
Lot cluster_lot(rec::Samples& r, double ox, double oy, const Archetype& a, double k, const District& d) {
  double U = 0, V = 0;
  for (int t = 0; t < 60; ++t) {
    U = 60 + std::floor(r() * 500);
    V = 60 + std::floor(r() * 500);
    if (a.fits(U, V)) break;
  }
  const bool turned = r() < 0.3;
  const double x0 = ox + std::floor(r() * 1600);
  const double y0 = oy + std::floor(r() * 1600);
  const double fi = std::floor(r() * 4);
  const int yaw = static_cast<int>(std::floor(r() * 132));
  const bool corner = r() < 0.3;
  Lot lot;
  lot.id = js::cat("C0_0/b", std::fmod(k, 3), "/l", k);
  lot.district = d.id;
  lot.corner = corner;
  lot.micro = false;
  lot.block = js::cat("C0_0/b", std::fmod(k, 3));
  if (turned) {
    lot.front = nominal_front(yaw);
    Turn t;
    t.yaw = yaw;
    t.origin = {x0, y0};
    t.ou = 0;
    t.ov = 0;
    t.U = U;
    t.V = V;
    lot.turn = t;
    lot.rect = lot_frame_of(lot.turn, Rect{}, lot.front).R;
  } else {
    lot.front = "NESW"[static_cast<int>(fi)];
    const bool ns = lot.front == 'N' || lot.front == 'S';
    lot.rect = {x0, y0, x0 + (ns ? U : V) - 1, y0 + (ns ? V : U) - 1};
  }
  return lot;
}

}  // namespace

TEST_CASE("city grading: level lots and graded ground are the reference's (stage grading)") {
  register_all();
  rec::Out out;
  rec::Samples r(73);
  const std::vector<District> DS = district_list();
  std::vector<std::string> styles;
  for (const Style& s : style_registry().all()) styles.push_back(s.id);
  for (int cell = 0; cell < 40; ++cell) {
    const World& w = shell_world(static_cast<size_t>(cell % 3));
    const double ox = std::floor((r() - 0.5) * 100000);
    const double oy = std::floor((r() - 0.5) * 100000);
    const double n = 4 + std::floor(r() * 8);
    std::vector<Lot> lots;
    std::deque<std::optional<Envelope>> envs;
    std::map<std::string, const Envelope*> buildings;
    for (double k = 0; k < n; k += 1) {
      const Archetype& a = archetype_registry().get(kKinds[static_cast<size_t>(std::floor(r() * 16))]);
      const District& d = DS[static_cast<size_t>(std::floor(r() * static_cast<double>(DS.size())))];
      Lot lot = cluster_lot(r, ox, oy, a, k, d);
      lot.ground_z = 400 + std::floor(r() * 200);
      lot.building.clear();
      const std::string& style = styles[static_cast<size_t>(std::floor(r() * static_cast<double>(styles.size())))];
      EnvelopeExtra extra;
      extra.u = r();
      extra.core = r();
      extra.ground_z = lot.ground_z;
      extra.config = &w.config;
      Rng rng(std::floor(r() * 4294967296.0));
      envs.push_back(plan_building_envelope_as(lot, a.id, style, d, rng, extra));
      const std::optional<Envelope>& env = envs.back();
      const bool miss = r() < 0.08;
      const bool hw = r() < 0.06;
      const bool ug = r() < 0.05;
      if (env) {
        lot.building = env->id;
        if (!miss) buildings[env->id] = &*env;
      }
      lot.under_highway = hw;
      lot.underground = ug;
      out << (Line() << "lot" << lot.id << lot.rect.x0 << lot.rect.y0 << lot.rect.x1 << lot.rect.y1 << lot.ground_z << lot.block
                     << (lot.building.empty() ? std::string("-") : lot.building) << miss << hw << ug << level_lot(lot, env ? &*env : nullptr) << env_line(env));
      lots.push_back(std::move(lot));
    }
    const SiteGrading grading(lots, [&](const std::string& id) -> const Envelope* {
      auto it = buildings.find(id);
      return it == buildings.end() ? nullptr : it->second;
    });
    for (size_t k = 0; k < lots.size(); ++k) {
      const Lot& lot = lots[k];
      const Rect b = envs[k] ? envs[k]->bounds : lot.rect;
      for (int q = 0; q < 40; ++q) {
        const double x = std::floor(b.x0 - 200 + r() * (b.x1 - b.x0 + 400));
        const double y = std::floor(b.y0 - 200 + r() * (b.y1 - b.y0 + 400));
        const bool frac = r() < 0.3;
        const double dz = (r() - 0.5) * 240;
        const double base = lot.ground_z + (frac ? dz : std::floor(dz));
        const std::string block_id = r() < 0.8 ? lot.block : std::string("C0_0/b9");
        const double lt = r();
        const Lot& other = lots[static_cast<size_t>(std::floor(r() * static_cast<double>(lots.size())))];
        const Lot* on = lt < 0.3 ? &lot : lt < 0.4 ? &other : nullptr;
        out << (Line() << "at" << x << y << base << block_id << (on ? on->id : std::string("-")) << grading.at(x, y, base, block_id, on));
      }
    }
  }
  CHECK(rec::record("grading", out.text()) == rec::recorded_digest("grading"));
}
