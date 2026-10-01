// svx_city tests — the interior planners (voxel_city buildings/interior/plan.js, apartments.js,
// offices.js, industrial.js, garage.js, school.js, civic.js, civicPrograms.js) against the
// reference (stage "interiors" of tools/procgen_ref): planBuilding on envelopes of every archetype,
// on the envelopes planBuildingEnvelope chooses and on odd ones, the planners called directly,
// validatePlan on scripted plans, roomAtCell.
#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "building_records.hpp"
#include "buildings/archetypes.hpp"
#include "buildings/interior/plan.hpp"
#include "buildings/styles.hpp"
#include "interior_records.hpp"
#include "network/roadView.hpp"
#include "records.hpp"
#include "world/World.hpp"
#include "world/register_all.hpp"

using namespace svx::city;
using namespace svx::city::test;
using rec::Line;

namespace {

// town flavors a shop reads (and some it does not; "": undefined)
const char* const kFlavors[] = {"", "nordic", "nordicHarbour", "harbourTown", "nordicBleak", "soviet", "nordicVillage", "sovietVillage", "historic", "modern"};
const double kPitched[] = {0.08, 0.35, 0.65, 1};

// A sky door on floor 1 .. floors across a span of the building's world box (stages/interiors.mjs skyDoorOf).
SkyDoor sky_door_of(const Envelope& env, double a, double b, double c, double d, const std::string& bridge) {
  SkyDoor s;
  s.floor = 1 + std::floor(a * js::max(1.0, env.floors));
  const bool along_x = env.front == 'N' || env.front == 'S';
  const bool span_x = d < 0.85 ? along_x : !along_x;
  const double lo = span_x ? env.R.x0 : env.R.y0;
  const double hi = span_x ? env.R.x1 : env.R.y1;
  s.span_x = span_x;
  s.s0 = lo + std::floor(b * js::max(1.0, hi - lo - 24));
  s.s1 = s.s0 + 10 + std::floor(c * 24);
  s.bridge = bridge;
  return s;
}

std::string sky_str(const SkyDoor& d) { return js::cat(d.floor, "/", d.span_x ? "x" : "y", "/", d.s0, "/", d.s1, "/", d.bridge); }

// decorate(r, env, k): a chamfer, a flavor, sky doors, wings, pitched ramps, the other roof.
std::string decorate(rec::Samples& r, std::optional<Envelope>& env, double k) {
  const double ch = r();
  const double cs = r();
  const double ck = r();
  const double fl = r();
  const double sk = r();
  const double sa = r();
  const double sb = r();
  const double sc = r();
  const double sd = r();
  const double s2 = r();
  const double wg = r();
  const double wu = r();
  const double wv = r();
  const double pr = r();
  const double rf = r();
  if (!env) return "dec -";
  if (ch < 0.15) env->chamfer = Chamfer{cs < 0.5 ? 'L' : 'R', 20 * (1 + std::floor(ck * 2)), 21 * (1 + std::floor(ck * 2))};
  const char* flavor = kFlavors[static_cast<size_t>(std::floor(fl * 10))];
  if (*flavor) env->flavor = flavor;
  if (sk < 0.6) {
    env->sky_doors.push_back(sky_door_of(*env, sa, sb, sc, sd, js::cat("B", k, "a")));
    if (s2 < 0.3) env->sky_doors.push_back(sky_door_of(*env, sb, sc, sa, s2, js::cat("B", k, "b")));
  }
  if (wg < 0.3) {
    const double u = std::floor(wu * env->U);
    const double y0 = wv < 0.4 ? -30 : wv < 0.8 ? env->V + 2 : env->V + 100;
    const double y1 = wv < 0.4 ? -2 : wv < 0.8 ? env->V + 30 : env->V + 140;
    env->wings.push_back(Wing{Rect{u - 20, y0, u + 20, y1}});
  }
  if (pr < 0.5) env->pitched_ramps = true;
  if (rf < 0.1) env->roof.type = env->roof.type == "flat" ? "gable" : "flat";
  std::string sky = "-";
  if (!env->sky_doors.empty()) {
    sky.clear();
    for (size_t i = 0; i < env->sky_doors.size(); ++i) sky += (i ? "," : "") + sky_str(env->sky_doors[i]);
  }
  const std::string ch_s = env->chamfer ? js::cat(env->chamfer->side, env->chamfer->a, "/", env->chamfer->b) : std::string("-");
  const std::string wing = env->wings.empty() ? std::string("-") : js::cat(env->wings[0].canon.x0, ",", env->wings[0].canon.y0, ",", env->wings[0].canon.x1, ",", env->wings[0].canon.y1);
  return (Line() << "dec" << ch_s << fstr(env->flavor) << sky << wing << env->pitched_ramps << env->roof.type).str();
}

// The worlds of the configs (their seeds for the plans' streams), with no roads.
class Worlds {
 public:
  explicit Worlds(const std::vector<Value>& cfg) : cfg_(cfg), worlds_(cfg.size()) {}
  const World& of(size_t ci) {
    if (!worlds_[ci]) {
      worlds_[ci] = std::make_unique<World>(cfg_[ci]);
      worlds_[ci]->cell_roads = [](double, double) { return RoadList{}; };
    }
    return *worlds_[ci];
  }

 private:
  const std::vector<Value>& cfg_;
  std::vector<std::unique_ptr<World>> worlds_;
};

// The planners called directly on a stream of the stage's: the plan's size and the stream's next draw.
std::string direct(const Envelope& env, double seed) {
  Rng rng(seed);
  PlanBuilder pb(env, rng);
  if (!run_planner(env, rng, pb)) return (Line() << "direct" << "-" << rng.next()).str();
  const BuildingPlan plan = pb.build();
  double rooms = 0;
  for (const PlanFloor& fl : plan.floors) rooms += static_cast<double>(fl.grid->rooms.size());
  return (Line() << "direct" << static_cast<double>(plan.floors.size()) << rooms << static_cast<double>(plan.issues.size()) << rng.next()).str();
}

double count_of(const std::string& id) {
  static const std::vector<std::pair<std::string, double>> kCount = {{"walkup", 60},    {"midrise", 50},  {"tower", 40},  {"office", 40},   {"panelSlab", 40},
                                                                     {"panelTower", 30}, {"townhouse", 50}, {"wharfhouse", 30}, {"garage", 30}, {"school", 24},
                                                                     {"warehouse", 30}, {"factory", 20},  {"barn", 20}};
  for (const auto& c : kCount)
    if (c.first == id) return c.second;
  return 16;
}

size_t pick(rec::Samples& r, size_t n) { return static_cast<size_t>(std::floor(r() * static_cast<double>(n))); }

}  // namespace

TEST_CASE("city interiors: building plans are the reference's (stage interiors)") {
  register_all();
  rec::Samples r(53);
  rec::Out out;
  const std::vector<District> DS = district_list();
  const std::vector<Value> CFG = config_list();
  std::vector<std::string> styles;
  for (const Style& s : style_registry().all()) styles.push_back(s.id);
  Worlds worlds(CFG);
  double k = 0;
  // ---- every archetype on scripted lots
  for (const Archetype& a : archetype_registry().all()) {
    const double n = a.civic ? 14 : count_of(a.id);
    for (double s = 0; s < n; s += 1) {
      double U = 0, V = 0;
      for (int t = 0; t < 60; ++t) {
        U = 40 + std::floor(r() * 400);
        V = 40 + std::floor(r() * 400);
        if (a.fits(U, V)) break;
      }
      const District& d = DS[pick(r, DS.size())];
      const Lot lot = scripted_lot(r, U, V, (k += 1), d.id);
      const std::string& style = styles[pick(r, styles.size())];
      const size_t ci = pick(r, CFG.size());
      EnvelopeExtra extra;
      extra.u = r();
      extra.core = r() * 1.2;
      extra.ground_z = std::floor(r() * 2000);
      extra.config = &CFG[ci];
      if (r() < 0.3) extra.chapel = true;
      if (r() < 0.4) extra.dome = {"GOLD", "DOME_GREEN"};
      if (r() < 0.5) extra.pitched_civic = kPitched[pick(r, 4)];
      Rng erng(std::floor(r() * 4294967296.0));
      std::optional<Envelope> env = plan_building_envelope_as(lot, a.id, style, d, erng, extra);
      out << (Line() << "b" << a.id << U << V << d.id << style << static_cast<double>(ci) << env_line(env));
      out << (Line() << decorate(r, env, k));
      const double seed = std::floor(r() * 4294967296.0);
      if (!env) continue;
      const std::shared_ptr<const BuildingPlan> plan = plan_building(worlds.of(ci), *env);
      plan_records(out, plan.get());
      if (std::fmod(s, 4) == 0) out << (Line() << direct(*env, seed));
    }
  }
  // ---- the archetypes planBuildingEnvelope chooses
  for (int s = 0; s < 300; ++s) {
    const double U = 40 + std::floor(r() * 400);
    const double V = 40 + std::floor(r() * 400);
    const District& d = DS[pick(r, DS.size())];
    const Lot lot = scripted_lot(r, U, V, (k += 1), d.id);
    const size_t ci = pick(r, CFG.size());
    EnvelopeExtra extra;
    extra.u = r();
    extra.core = r() * 1.2;
    extra.ground_z = std::floor(r() * 2000);
    extra.config = &CFG[ci];
    Rng erng(std::floor(r() * 4294967296.0));
    const std::optional<Envelope> env = plan_building_envelope(lot, d, erng, extra);
    out << (Line() << "p" << U << V << d.id << static_cast<double>(ci) << env_line(env));
    if (!env) continue;
    plan_records(out, plan_building(worlds.of(ci), *env).get());
  }
  // ---- odd envelopes
  struct Odd {
    const char* id;
    const char* cut;
    int n;
  };
  const Odd kOdd[] = {{"walkup", "shallow", 30},      {"townhouse", "shallow", 16}, {"wharfhouse", "shallow", 10}, {"panelSlab", "shallow", 10},
                      {"garage", "shallow", 30},      {"school", "narrow", 24},     {"midrise", "narrow", 14},     {"office", "narrow", 14},
                      {"tower", "narrow", 10},        {"warehouse", "narrow", 10},  {"hospital", "narrow", 10},    {"policeStation", "narrow", 10},
                      {"supermarket", "narrow", 8},   {"cinema", "narrow", 8},      {"departmentStore", "narrow", 8}, {"house", "bogus", 4}};
  for (const Odd& o : kOdd) {
    const Archetype& a = archetype_registry().get(o.id);
    const std::string cut = o.cut;
    for (int s = 0; s < o.n; ++s) {
      double U = 0, V = 0;
      for (int t = 0; t < 60; ++t) {
        U = 40 + std::floor(r() * 400);
        V = 40 + std::floor(r() * 400);
        if (a.fits(U, V)) break;
      }
      const District& d = DS[pick(r, DS.size())];
      const Lot lot = scripted_lot(r, U, V, (k += 1), d.id);
      const std::string& style = styles[pick(r, styles.size())];
      const size_t ci = pick(r, CFG.size());
      EnvelopeExtra extra;
      extra.u = r();
      extra.core = r() * 1.2;
      extra.ground_z = std::floor(r() * 2000);
      extra.config = &CFG[ci];
      Rng erng(std::floor(r() * 4294967296.0));
      std::optional<Envelope> env = plan_building_envelope_as(lot, o.id, style, d, erng, extra);
      const double size = r();
      const double pr = r();
      if (env) {
        if (cut == "shallow") {
          const double depth = std::string(o.id) == "garage" ? 150 + std::floor(size * 180) : 30 + std::floor(size * 90);
          for (EnvTier& t : env->tiers)
            for (Rect& q : t.rects) q.y1 = js::min(q.y1, q.y0 + depth - 1);
        } else if (cut == "narrow") {
          const double width = 90 + std::floor(size * 260);
          for (EnvTier& t : env->tiers)
            for (Rect& q : t.rects) q.x1 = js::min(q.x1, q.x0 + width - 1);
        } else {
          env->archetype = "bogus";
        }
        if (pr < 0.6) env->pitched_ramps = true;
      }
      out << (Line() << "odd" << o.id << o.cut << U << V << d.id << style << static_cast<double>(ci) << size << pr << env_line(env));
      if (!env) continue;
      plan_records(out, plan_building(worlds.of(ci), *env).get());
    }
  }
  // ---- validatePlan on scripted plans
  static const char* const kTypes[] = {"office", "hall", "stair", "elevator", "shaft", "void", "mechanicalShaft", "storage"};
  static const char* const kDoorKinds[] = {"interior", "entrance", "elevator", "balcony", "window", "opening", "stair"};
  for (int c = 0; c < 400; ++c) {
    const double nR = 2 + std::floor(r() * 7);
    const double nG = 1 + std::floor(r() * 3);
    const double nS = std::floor(r() * 3);
    BuildingPlan plan;
    for (double s = 0; s < nS; s += 1) {
      auto st = std::make_shared<Stair>();
      st->id = s;
      st->f0 = std::floor(r() * 3) - 1;
      st->f1 = st->f0 + std::floor(r() * 4);
      plan.stairs.push_back(st);
    }
    std::vector<std::shared_ptr<FloorGrid>> grids;
    for (double g = 0; g < nG; g += 1) {
      auto grid = std::make_shared<FloorGrid>(64, 64, std::vector<Rect>{{0, 0, 63, 63}});
      for (double q = 0; q < nR; q += 1) {
        std::string type = kTypes[pick(r, 8)];
        const double st = std::floor(r() * js::max(1.0, nS));
        if (type == "stair" && nS == 0) type = "office";
        const double i = std::fmod(q, 3);
        const double j = std::floor(q / 3);
        RoomProps p;
        if (type == "stair") p.stair = st;
        grid->add_room(type, {{2 + i * 20, 2 + j * 20, 19 + i * 20, 19 + j * 20}}, p);
      }
      const double nD = std::floor(r() * 12);
      for (double q = 0; q < nD; q += 1) {
        auto dd = std::make_shared<Door>();
        dd->id = static_cast<double>(grid->doors.size());
        dd->a = std::floor(r() * nR);
        dd->b = r() < 0.3 ? -1 : std::floor(r() * nR);
        dd->kind = kDoorKinds[pick(r, 7)];
        grid->doors.push_back(dd);
      }
      grids.push_back(grid);
    }
    const double nF = 1 + std::floor(r() * 5);
    for (double q = 0; q < nF; q += 1) {
      PlanFloor fl;
      fl.index = std::floor(r() * 5) - 1;
      fl.z = r() < 0.2 ? 0 : fl.index * 30;
      fl.height = 30;
      fl.grid = grids[pick(r, static_cast<size_t>(nG))];
      fl.kind = "x";
      plan.floors.push_back(fl);
    }
    js::sort(plan.floors, [](const PlanFloor& p, const PlanFloor& q) { return js::or_(p.z - q.z, p.index - q.index); });
    const double nL = std::floor(r() * 3);
    for (double q = 0; q < nL; q += 1) {
      PlanLink l;
      l.fa = plan.floors[pick(r, static_cast<size_t>(nF))].index;
      l.ra = std::floor(r() * nR);
      l.fb = plan.floors[pick(r, static_cast<size_t>(nF))].index;
      l.rb = std::floor(r() * nR);
      l.ramp = q;
      plan.links.push_back(l);
    }
    for (const auto& g : grids)
      for (const auto& m : g->rooms) {
        const double lk = r();
        const double lf = plan.floors[pick(r, static_cast<size_t>(nF))].index;
        const double lr = std::floor(r() * nR);
        if (lk < 0.15) m->link_to = RoomLink{lf, lr};
      }
    validate_plan(plan);
    std::string fls;
    for (size_t q = 0; q < plan.floors.size(); ++q) {
      double gi = -1;
      for (size_t i = 0; i < grids.size(); ++i)
        if (grids[i] == plan.floors[q].grid) {
          gi = static_cast<double>(i);
          break;
        }
      fls += js::cat(q ? "," : "", plan.floors[q].index, "@", plan.floors[q].z, ":", gi);
    }
    std::string issues;
    for (size_t q = 0; q < plan.issues.size(); ++q) {
      const PlanIssue& is = plan.issues[q];
      issues += js::cat(q ? "," : "", is.floor, "/", is.room, "/", is.type, "/", is.msg);
    }
    out << (Line() << "vp" << c << nR << nG << nS << fls << (issues.empty() ? std::string("-") : issues));
  }
  // ---- roomAtCell
  const World& w0 = worlds.of(0);
  const auto& all = archetype_registry().all();
  for (size_t s = 0; s < 40; ++s) {
    const Archetype& a = all[s % all.size()];
    double U = 0, V = 0;
    for (int t = 0; t < 60; ++t) {
      U = 40 + std::floor(r() * 300);
      V = 40 + std::floor(r() * 300);
      if (a.fits(U, V)) break;
    }
    const District& d = DS[pick(r, DS.size())];
    const Lot lot = scripted_lot(r, U, V, (k += 1), d.id);
    EnvelopeExtra extra;
    extra.u = 0.5;
    extra.core = 0.5;
    extra.ground_z = 100;
    extra.config = &CFG[0];
    Rng erng(std::floor(r() * 4294967296.0));
    const std::optional<Envelope> env = plan_building_envelope_as(lot, a.id, "concrete", d, erng, extra);
    if (!env) {
      out << (Line() << "rc" << a.id << "-");
      continue;
    }
    const std::shared_ptr<const BuildingPlan> plan = plan_building(w0, *env);
    std::string cells;
    if (plan)
      for (const PlanFloor& fl : plan->floors)
        for (int q = 0; q < 12; ++q) {
          const double u = std::floor(r() * (env->U + 4)) - 2;
          const double v = std::floor(r() * (env->V + 4)) - 2;
          const RoomAtCell x = room_at_cell(fl, u, v);
          if (!cells.empty()) cells += ",";
          cells += x.room ? js::num(x.room->id) : x.door ? "D" : "-";
        }
    out << (Line() << "rc" << a.id << (cells.empty() ? std::string("-") : cells));
  }
  CHECK(rec::record("interiors", out.text()) == rec::recorded_digest("interiors"));
}
