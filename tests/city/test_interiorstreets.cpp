// svx_city tests — the street levels of building plans (voxel_city buildings/interior/plan.js
// streetLevels) against the reference (stage "interiorstreets" of tools/procgen_ref): buildings of
// every archetype on scripted lots of three worlds, with scripted roads along their sides, their
// ground level set off the front street's; each ground-floor street door's street and sill.
#include <doctest.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "building_records.hpp"
#include "buildings/archetypes.hpp"
#include "buildings/interior/plan.hpp"
#include "buildings/styles.hpp"
#include "config/presets.hpp"
#include "core/obb.hpp"
#include "network/arterials.hpp"
#include "network/roadLevel.hpp"
#include "network/roadView.hpp"
#include "records.hpp"
#include "world/World.hpp"
#include "world/register_all.hpp"

using namespace svx::city;
using namespace svx::city::test;
using rec::Line;

namespace {

struct StreetWorld {
  const char* key;
  const char* id;
  const char* size;  // ("": the preset's default)
};
// (stages/interiorstreets.mjs STREET_WORLDS)
const StreetWorld kStreetWorlds[] = {{"cities", "cities", ""}, {"angledCities", "angledCities", ""}, {"oldHarbourTown", "oldHarbourTown", ""}};
const double kSideP[4] = {0.95, 0.5, 0.35, 0.35};

// A lot whose frame is U x V cells at (x0, y0): plain or turned (stages/interiorstreets.mjs streetLot).
Lot street_lot(rec::Samples& r, double U, double V, double x0, double y0, double k, const std::string& district_id) {
  const bool turned = r() < 0.3;
  const double fi = std::floor(r() * 4);
  const bool corner = r() < 0.3;
  Lot lot;
  lot.id = js::cat("C9_9/b", std::fmod(k, 13), "/l", k);
  lot.district = district_id;
  lot.corner = corner;
  lot.micro = false;
  if (turned) {
    const int yaw = static_cast<int>(std::floor(r() * 132));
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

XY to_world(const Frame& frame, double u, double v) { return frame.turned ? frame.point_to_world(u, v) : local_point_to_world(frame.placement, u, v); }

struct Side {
  bool has;
  double off, walk;
};

}  // namespace

TEST_CASE("city interiorstreets: street doors meet their streets as the reference's do (stage interiorstreets)") {
  register_all();
  rec::Samples r(59);
  rec::Out out;
  const std::vector<District> DS = district_list();
  std::vector<std::string> styles;
  for (const Style& s : style_registry().all()) styles.push_back(s.id);
  const auto& all = archetype_registry().all();
  for (const StreetWorld& sw : kStreetWorlds) {
    std::map<std::pair<double, double>, RoadList> cell_roads;
    World w(preset_config(sw.id, sw.size));
    w.cell_roads = [&](double i, double j) {
      auto it = cell_roads.find({i, j});
      return it == cell_roads.end() ? RoadList{} : it->second;
    };
    out << (Line() << "world" << sw.key);
    // ---- the buildings and their roads
    struct Built {
      double q;
      std::unique_ptr<Envelope> env;
    };
    std::vector<Built> built;
    for (size_t qi = 0; qi < 3 * all.size(); ++qi) {
      const double q = static_cast<double>(qi);
      const Archetype& a = all[qi % all.size()];
      double U = 0, V = 0;
      for (int t = 0; t < 60; ++t) {
        U = 40 + std::floor(r() * 300);
        V = 40 + std::floor(r() * 300);
        if (a.fits(U, V)) break;
      }
      const District& d = DS[static_cast<size_t>(std::floor(r() * static_cast<double>(DS.size())))];
      const std::string& style = styles[static_cast<size_t>(std::floor(r() * static_cast<double>(styles.size())))];
      const double ci = 2 + 3 * std::fmod(q, 10);
      const double cj = 2 + 3 * std::floor(q / 10);
      const Rect rc = w.arterials->cell_rect(ci, cj);
      const double x0 = std::floor(rc.x0 + 800 + r() * js::max(1.0, rc.x1 - rc.x0 - 1600));
      const double y0 = std::floor(rc.y0 + 800 + r() * js::max(1.0, rc.y1 - rc.y0 - 1600));
      const Lot lot = street_lot(r, U, V, x0, y0, q, d.id);
      EnvelopeExtra extra;
      extra.u = r();
      extra.core = r();
      extra.ground_z = 0;
      extra.config = &w.config;
      Rng erng(std::floor(r() * 4294967296.0));
      const std::optional<Envelope> env = plan_building_envelope_as(lot, a.id, style, d, erng, extra);
      Side sides[4];
      for (int s = 0; s < 4; ++s) {
        sides[s].has = r() < kSideP[s];
        sides[s].off = 4 + std::floor(r() * 30);
        sides[s].walk = r() < 0.3 ? 0 : 12 + std::floor(r() * 8);
      }
      std::string side_str;
      for (int s = 0; s < 4; ++s) side_str += (s ? "," : "") + (sides[s].has ? js::cat(sides[s].off, "/", sides[s].walk) : std::string("-"));
      out << (Line() << "sb" << q << a.id << U << V << d.id << style << ci << cj << side_str << env_line(env));
      if (!env) continue;
      auto e = std::make_unique<Envelope>(*env);
      const Frame& frame = envelope_frame(*e);
      const double EU = e->U, EV = e->V;
      RoadList roads;
      for (int k = 0; k < 4; ++k) {
        if (!sides[k].has) continue;
        const double o = sides[k].off;
        const double ends[4][2][2] = {{{-60, -o}, {EU + 60, -o}}, {{EU + 60, EV - 1 + o}, {-60, EV - 1 + o}}, {{-o, EV + 60}, {-o, -60}}, {{EU - 1 + o, -60}, {EU - 1 + o, EV + 60}}};
        auto road = std::make_shared<Road>();
        road->id = js::cat("S", q, "/r", k);
        road->cell = js::cat("S", q);
        road->cls = "local";
        for (int p = 0; p < 2; ++p) {
          const XY xy = to_world(frame, ends[k][p][0], ends[k][p][1]);
          road->pts.push_back({xy[0], xy[1]});
        }
        road->hc = 24;
        road->hr = 24 + sides[k].walk;
        road->corner = 6;
        road->median = 0;
        road->parking = 0;
        road->lanes = 2;
        road->lane = 24;
        road->sidewalk = sides[k].walk;
        road->shoulder = 0;
        road->ci = ci;
        road->cj = cj;
        roads.push_back(road);
      }
      cell_roads[{ci, cj}] = roads;
      built.push_back({q, std::move(e)});
    }
    // ---- the plans, each building's level off its front street's
    for (Built& b : built) {
      Envelope& env = *b.env;
      const Frame& frame = envelope_frame(env);
      const XY p = to_world(frame, env.U / 2, -6);
      const CellIJ c = w.cell_at(p[0], p[1]);
      const std::shared_ptr<const RoadView> view = w.road_view(c.i, c.j);
      const std::optional<RoadLevel> lv = road_level_at(w, *view, p[0], p[1], 24);
      const double L0 = lv ? js::round(lv->z) + (lv->sidewalk ? 1 : 0) : 0;
      const double delta = std::floor(r() * 29) - 14;
      env.ground_z = L0 - delta;
      env.base_z = env.ground_z + (env.stoop ? 4 : 1);
      const std::shared_ptr<const BuildingPlan> plan = plan_building(w, env);
      Line l;
      l << "lv" << b.q << c.i << c.j;
      if (lv)
        l << lv->z;
      else
        l << rec::kUndef;
      l << L0 << delta << env.ground_z << env.base_z;
      if (plan)
        l << static_cast<double>(plan->issues.size());
      else
        l << rec::kUndef;
      out << l;
      const PlanFloor* F0 = plan ? plan->floor_by_index(0) : nullptr;
      if (F0)
        for (const auto& d : F0->grid->doors)
          if (d->b == -1) out << (Line() << "sd" << d->id << d->kind << d->orient << d->u0 << d->u1 << d->v0 << d->v1 << fo(d->street) << fo(d->sill));
    }
  }
  CHECK(rec::record("interiorstreets", out.text()) == rec::recorded_digest("interiorstreets"));
}
