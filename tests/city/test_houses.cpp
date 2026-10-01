// svx_city tests — the house and cabin floor planners (voxel_city buildings/interior/houses.js,
// cabins.js) against the reference (stage "houses" of tools/procgen_ref): house columns, cabin
// layouts, and house, row house, town house, cabin and church plans through the plan builder.
#include <doctest.h>

#include "building_records.hpp"
#include "buildings/archetypes.hpp"
#include "buildings/interior/cabins.hpp"
#include "buildings/interior/houses.hpp"
#include "buildings/interior/plan.hpp"
#include "buildings/styles.hpp"
#include "records.hpp"
#include "world/register_all.hpp"

using namespace svx::city;
using namespace svx::city::test;
using rec::Line;

namespace {

std::string range(const std::array<double, 2>& a) { return js::cat("[", a[0], ",", a[1], "]"); }

void plan_records(rec::Out& out, const std::string& tag, const PlanBuilder& pb) {
  for (const auto& s : pb.stairs) {
    std::string flights;
    if (s->flights)
      for (size_t k = 0; k < s->flights->size(); ++k) {
        const StairFlight& q = (*s->flights)[k];
        flights += js::cat(k ? ";" : "", q.f, "/", q.z0, "/", q.H);
      }
    out << (Line() << tag + "s" << *s->id << rect_str(s->rect) << s->axis << s->dir << s->lane_low << s->lane << s->landing << s->f0 << s->f1 << s->L << s->W << s->open
                   << flights);
  }
  for (const PlanFloor& fl : pb.floors) {
    out << (Line() << tag + "f" << fl.index << fl.z << fl.height << fl.kind);
    grid_records(out, tag, *fl.grid);
  }
}

struct Kind {
  const char* id;
  int n;
  const char* planner;
};
const Kind kKinds[] = {{"house", 100, "house"}, {"rowhouse", 100, "house"}, {"townhouse", 100, "house"}, {"cabin", 120, "cabin"},
                       {"church", 80, "church"}, {"cabin", 40, "house"},     {"wharfhouse", 30, "house"}};

}  // namespace

TEST_CASE("city houses: house and cabin plans are the reference's (stage houses)") {
  register_all();
  rec::Samples r(37);
  rec::Out out;
  // ---- house columns and cabin layouts
  for (double U = 16; U <= 260; U += 1)
    for (bool m : {false, true}) {
      const HouseColumns c = house_columns(U, m);
      out << (Line() << "hc" << U << m << range(c.stair_col) << range(c.hall) << range(c.rooms) << c.entrance_u);
    }
  for (double U = 24; U <= 120; U += 1)
    for (double V = 24; V <= 120; V += 3)
      for (bool m : {false, true}) {
        const CabinLayout L = cabin_layout(U, V, m);
        std::string rooms;
        for (size_t k = 0; k < L.rooms.size(); ++k) rooms += js::cat(k ? ";" : "", L.rooms[k].key, "/", L.rooms[k].type, "/", rect_str(L.rooms[k].rect));
        out << (Line() << "cl" << U << V << m << rooms << L.entrance_u);
      }
  // ---- plans
  const std::vector<District> DS = district_list();
  const std::vector<Value> CFG = config_list();
  std::vector<std::string> styles;
  for (const Style& s : style_registry().all()) styles.push_back(s.id);
  const std::optional<double> kMargins[4] = {std::nullopt, 0.0, 1.0, 2.0};
  double k = 0;
  for (const Kind& kind : kKinds) {
    const Archetype& a = archetype_registry().get(kind.id);
    for (int s = 0; s < kind.n; ++s) {
      double U = 0, V = 0;
      for (int t = 0; t < 60; ++t) {
        U = 40 + std::floor(r() * 360);
        V = 40 + std::floor(r() * 360);
        if (a.fits(U, V)) break;
      }
      const District& d = DS[static_cast<size_t>(std::floor(r() * static_cast<double>(DS.size())))];
      const Lot lot = scripted_lot(r, U, V, (k += 1), d.id);
      const std::string& style = styles[static_cast<size_t>(std::floor(r() * static_cast<double>(styles.size())))];
      const Value& config = CFG[static_cast<size_t>(std::floor(r() * static_cast<double>(CFG.size())))];
      EnvelopeExtra extra;
      extra.u = r();
      extra.core = r() * 1.2;
      extra.ground_z = std::floor(r() * 2000);
      extra.config = &config;
      if (r() < 0.3) extra.chapel = true;
      if (r() < 0.4) extra.dome = {"GOLD", "DOME_GREEN"};
      Rng erng(std::floor(r() * 4294967296.0));
      std::optional<Envelope> env = plan_building_envelope_as(lot, kind.id, style, d, erng, extra);
      const double ch = r();
      const double cs = r();
      const double ck = r();
      if (env && ch < 0.2) env->chamfer = Chamfer{cs < 0.5 ? 'L' : 'R', 20 * (1 + std::floor(ck * 2)), 21 * (1 + std::floor(ck * 2))};
      const std::optional<double> margin = kMargins[static_cast<int>(std::floor(r() * 4))];
      const std::string chamfer = env && env->chamfer ? js::cat(env->chamfer->side, env->chamfer->a, "/", env->chamfer->b) : std::string("-");
      out << (Line() << "h" << kind.id << kind.planner << U << V << d.id << fo(margin) << chamfer << env_line(env));
      if (!env) continue;
      Rng rng = Rng::from(config["seed"].to_number(), env->id, "interior");
      PlanBuilder pb(*env, rng);
      const std::string planner = kind.planner;
      if (planner == "cabin")
        plan_cabin(*env, rng, pb);
      else if (planner == "church")
        plan_church(*env, pb);
      else if (margin)
        plan_house(*env, rng, pb, *margin);
      else
        plan_house(*env, rng, pb);
      plan_records(out, "x", pb);
      out << (Line() << "xe" << rng.next());
    }
  }
  CHECK(rec::record("houses", out.text()) == rec::recorded_digest("houses"));
}

TEST_CASE("city houses: a two-storey house has a stair, a hall on every floor and a front door") {
  register_all();
  Value config = make_config(Value::object());
  Lot lot;
  lot.id = "C0_0/b2/l0";
  lot.rect = {0, 0, 127, 199};
  lot.front = 'N';
  lot.district = "suburban";
  District d = district_registry().get("suburban");
  EnvelopeExtra extra;
  extra.config = &config;
  for (double seed = 1; seed <= 40; seed += 1) {
    Rng erng(seed);
    const std::optional<Envelope> env = plan_building_envelope_as(lot, "house", "siding", d, erng, extra);
    REQUIRE(env);
    Rng rng(seed);
    PlanBuilder pb(*env, rng);
    plan_house(*env, rng, pb);
    REQUIRE(pb.floors.size() == static_cast<size_t>(env->floors));
    CHECK(pb.stairs.size() == (env->floors > 1 ? 1u : 0u));
    bool front_door = false;
    for (const auto& dd : pb.floors[0].grid->doors)
      if (dd->b == -1 && dd->kind == "entrance" && dd->v0 <= 2) front_door = true;
    CHECK(front_door);
    for (const PlanFloor& fl : pb.floors) CHECK(fl.grid->rooms[0]->type == "hall");
  }
}
