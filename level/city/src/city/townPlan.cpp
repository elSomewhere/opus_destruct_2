// svx_city — city/townPlan.hpp (voxel_city city/townPlan.js).
#include "city/townPlan.hpp"

#include <array>
#include <memory>
#include <numeric>
#include <optional>
#include <unordered_set>

#include "buildings/civic.hpp"
#include "city/cellNetwork.hpp"
#include "city/districts.hpp"
#include "city/flavors.hpp"
#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "network/highways.hpp"
#include "terrain/terrain.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"
#include "world/island.hpp"

namespace svx::city {

namespace {

constexpr double kPi = 3.141592653589793;  // Math.PI

// Block programs a landmark kind turns into, with the districts and block sizes (m) it accepts.
struct Kind {
  std::string id;
  std::vector<std::string> districts;
  double min = 0, max = 0;
  std::optional<double> long_side;  // (JS long; undefined: min)
  bool civic = false;
};

// Civic buildings a town gets (buildings/civic): the districts a lot for one may be carved from.
// The block must hold the archetype's lot (its minimum footprint and setbacks). (CIVIC_DISTRICTS,
// in its key order.)
const std::vector<std::pair<std::string, std::vector<std::string>>>& civic_districts() {
  static const std::vector<std::pair<std::string, std::vector<std::string>>> t = {
      {"supermarket", {"residential", "suburban", "mixed", "projects", "microdistrict", "industrial"}},
      {"departmentStore", {"downtown", "midtown", "mixed", "oldcore", "oldtown"}},
      {"petrolStation", {"suburban", "residential", "industrial", "mixed", "projects", "microdistrict", "heavyIndustry"}},
      {"hospital", {"residential", "suburban", "mixed", "midtown", "microdistrict"}},
      {"polyclinic", {"microdistrict", "residential", "mixed", "projects", "suburban"}},
      {"policeStation", {"mixed", "midtown", "residential", "downtown", "oldcore", "oldtown"}},
      {"fireStation", {"mixed", "residential", "suburban", "industrial", "midtown", "oldtown"}},
      {"museum", {"oldcore", "oldtown", "downtown", "midtown", "mixed"}},
      {"artGallery", {"oldcore", "oldtown", "downtown", "midtown", "mixed"}},
      {"concertHall", {"downtown", "midtown", "mixed", "oldcore"}},
      {"houseOfCulture", {"mixed", "microdistrict", "downtown", "midtown", "oldtown"}},
      {"cinema", {"downtown", "midtown", "mixed", "oldtown", "oldcore", "microdistrict"}},
      {"library", {"mixed", "midtown", "oldcore", "oldtown", "residential"}},
      {"townHall", {"oldcore", "oldtown", "downtown", "midtown", "mixed"}},
      {"hotel", {"oldcore", "oldtown", "downtown", "midtown", "mixed", "harbour"}},
      {"marketHall", {"oldtown", "oldcore", "harbour", "mixed", "downtown"}},
      {"musicClub", {"mixed", "midtown", "oldcore", "industrial", "downtown", "oldtown"}},
  };
  return t;
}

// KINDS: the landmark kinds, then one per civic building.
const std::vector<Kind>& kinds() {
  static const std::vector<Kind> k = [] {
    std::vector<Kind> out = {
        {"square", {"oldtown", "mixed", "downtown", "midtown", "residential", "harbour"}, 22, 120, std::nullopt, false},
        {"church", {"oldtown", "mixed", "midtown", "residential", "suburban", "downtown"}, 20, 150, 30, false},
        {"wharf", {"oldtown", "mixed", "harbour"}, 20, 160, std::nullopt, false},
        {"cemetery", {"residential", "suburban", "mixed", "oldtown", "projects", "microdistrict"}, 45, 400, 60, false},
        {"park", {"oldtown", "mixed", "downtown", "midtown", "residential", "suburban", "microdistrict"}, 35, 400, std::nullopt, false},
        {"school", {"mixed", "midtown", "residential", "suburban", "microdistrict", "projects", "oldtown"}, 38, 400, 48, false},
        {"sports", {"residential", "suburban", "microdistrict", "projects", "mixed", "industrial"}, 45, 400, 70, false},
        {"allotments", {"residential", "suburban", "projects", "microdistrict", "industrial"}, 35, 400, std::nullopt, false},
        {"garages", {"projects", "microdistrict", "industrial", "residential", "suburban"}, 28, 300, std::nullopt, false},
        {"wasteland", {"industrial", "heavyIndustry", "projects", "harbour"}, 30, 400, std::nullopt, false},
    };
    for (const auto& [id, districts] : civic_districts()) {
      const CivicSpec* c = civic_spec(id);
      if (!c) SVX_FAIL("townPlan: a civic building the civic table lacks");
      const double need_w = c->w[0] + 2 * c->set_s[0];  // (c.setS?.[0] ?? 0)
      const double need_d = c->d[0] + c->set_f[0] + 1;
      out.push_back({id, districts, js::min(need_w, need_d), 9999, js::max(need_w, need_d), true});
    }
    return out;
  }();
  return k;
}

const Kind& kind_of(const std::string& id) {
  for (const Kind& k : kinds())
    if (k.id == id) return k;
  SVX_FAIL("townPlan: a landmark of no kind");
}

double rect_dist(const Rect& r, double x, double y) {
  const double dx = js::max(r.x0 - x, 0.0, x - r.x1);
  const double dy = js::max(r.y0 - y, 0.0, y - r.y1);
  return js::hypot(dx, dy);
}

bool suits(const Kind& k, const Block& block) {
  bool listed = false;
  for (const std::string& d : k.districts) listed = listed || d == block.district;
  if (!listed) return false;
  const Rect& p = block.prop;
  const double w = (p.x1 - p.x0) / 8;
  const double h = (p.y1 - p.y0) / 8;
  const double lo = js::min(w, h);
  const double hi = js::max(w, h);
  return lo >= k.min && hi <= k.max && hi >= k.long_side.value_or(k.min);
}

std::shared_ptr<TownPlan> make_town_plan(const World& world, const Settlement& s) {
  auto plan = std::make_shared<TownPlan>();
  if (s.village) return plan;
  const double seed = world.seed;
  // (hash the canonical centre: a wrapping world's laps share the plan)
  const double hx = js::round(s.cx);
  const double hy = js::round(s.cy);
  auto rnd = [&](double k) { return hash_float(seed, hx, hy, 8800 + k); };
  const double R = s.radius;
  const double Rm = R / 8;
  const IslandPlan* isl = world.fields->island.get();
  auto on_land = [&](double x, double y, double m) { return !isl || isl->coast(x / 8, y / 8) > m; };
  auto at = [&](double ang, double d) { return std::array<double, 2>{s.x + js::cos(ang) * d * R, s.y + js::sin(ang) * d * R}; };
  auto add = [&](const std::string& kind, const std::array<double, 2>& p, double pri) {
    if (on_land(p[0], p[1], 40)) plan->anchors.push_back({kind, js::round(p[0]), js::round(p[1]), pri});
  };
  // the harbour (island towns) and the industrial side (fields.industryNoise)
  std::optional<Harbour> h;
  if (isl && s.i == 0 && s.j == 0) h = isl->harbour();
  const std::optional<double> harbour_ang = h ? std::optional<double>(js::atan2(h->dy, h->dx)) : std::nullopt;
  const double ind_ang = hash_float(seed, s.i, s.j, 17) * kPi * 2;
  auto ang_gap = [](double a, double b) { return std::fabs(js::atan2(js::sin(a - b), js::cos(a - b))); };
  // a direction away from the works and the water
  auto quiet = [&](double k) {
    bool has = false;
    double best_a = 0;
    double best_score = 0;
    for (int q = 0; q < 12; ++q) {
      const double a = ((q + rnd(k)) / 12) * kPi * 2;
      const double score = ang_gap(a, ind_ang) + (harbour_ang ? ang_gap(a, *harbour_ang) : 1) + rnd(k + q + 1) * 0.6;
      if (!has || score > best_score) {
        has = true;
        best_a = a;
        best_score = score;
      }
    }
    return best_a;
  };

  // market square: at the heart, pulled towards the harbour
  if (h) {
    const double hd = js::hypot(h->x * 8 - s.x, h->y * 8 - s.y) / R;
    const double d = js::min(0.28, js::max(0.06, hd * 0.55));
    add("square", at(*harbour_ang, d), 0);
    // the wharf: gabled warehouses along the harbour front on either side of the square's axis
    for (const double side : {-1.0, 1.0}) {
      const double a = *harbour_ang + side * (0.35 + 0.25 * rnd(3 + side));
      add("wharf", at(a, js::max(0.1, hd * 0.9)), 1);
    }
  } else {
    add("square", at(rnd(1) * kPi * 2, 0.05 + 0.08 * rnd(2)), 0);
  }
  // main church: the highest of a few spots near the centre
  {
    bool has = false;
    double bx = 0, by = 0, bz = 0;
    for (int k = 0; k < 7; ++k) {
      const std::array<double, 2> p = at(rnd(10 + k) * kPi * 2, 0.12 + 0.22 * rnd(20 + k));
      if (!on_land(p[0], p[1], 80)) continue;
      const double z = world.terrain->sample(p[0], p[1]).h;
      if (!has || z > bz) {
        has = true;
        bx = p[0];
        by = p[1];
        bz = z;
      }
    }
    if (has) add("church", {bx, by}, 1);
  }
  // cemetery on the quiet edge of town
  add("cemetery", at(quiet(30), 0.78 + 0.14 * rnd(31)), 2);
  // parks and schools through the town, more in a bigger one
  const double n_park = 1 + std::floor(Rm / 900);
  for (double k = 0; k < n_park; k += 1) add("park", at(rnd(40 + k) * kPi * 2, 0.32 + 0.35 * rnd(50 + k)), 3);
  const double n_school = js::max(1.0, js::round(Rm / 800));
  for (double k = 0; k < n_school; k += 1) add("school", at(rnd(60 + k) * kPi * 2, 0.3 + 0.4 * rnd(70 + k)), 3);
  add("sports", at(quiet(80) + 1.4 + rnd(81), 0.62 + 0.25 * rnd(82)), 4);
  // allotment gardens out on the fringe, garages and a vacant lot by the works
  const double n_allot = 1 + std::floor(Rm / 1500);
  for (double k = 0; k < n_allot; k += 1) add("allotments", at(quiet(90 + k) + (rnd(95 + k) - 0.5) * 2.2, 0.86 + 0.14 * rnd(100 + k)), 5);
  add("garages", at(ind_ang + (rnd(110) - 0.5) * 1.1, 0.6 + 0.18 * rnd(111)), 5);
  add("wasteland", at(ind_ang + (rnd(120) - 0.5) * 0.9, 0.78 + 0.15 * rnd(121)), 6);

  // civicAnchors: the civic buildings of a town, by its size and flavor group
  {
    const std::string group = civic_group(flavor_of_settlement(&s).id);
    std::optional<double> sq_ang;
    for (const TownAnchor& a : plan->anchors)
      if (a.kind == "square") {
        sq_ang = js::atan2(a.y - s.y, a.x - s.x);
        break;
      }
    if (!sq_ang) sq_ang = rnd(200) * kPi * 2;
    double k = 300;
    auto some = [&](const std::string& kind, double n, double d0, double d1, double pri = 7, std::optional<double> ang = std::nullopt) {
      for (double q = 0; q < n; q += 1) {
        const double a = ang ? *ang + (rnd(k++) - 0.5) * 0.8 : rnd(k++) * kPi * 2;
        const double d = d0 + (d1 - d0) * rnd(k++);
        add(kind, at(a, d), pri);
      }
    };
    const bool soviet = group == "soviet";
    some("townHall", 1, 0.04, 0.12, 6, *sq_ang);
    some("hotel", 1 + std::floor(Rm / 1200), 0.05, 0.3);
    if (Rm >= 450) some("museum", 1 + std::floor(Rm / 2500), 0.08, 0.35);
    if (Rm >= 800) some("artGallery", 1 + std::floor(Rm / 2000), 0.1, 0.4);
    if (Rm >= 600) some(soviet ? "houseOfCulture" : "concertHall", 1 + std::floor(Rm / 2500), 0.1, 0.35);
    if (Rm >= 450) some("cinema", 1 + std::floor(Rm / 1800), 0.1, 0.45);
    if (Rm >= 450) some("library", 1 + std::floor(Rm / 2000), 0.15, 0.45);
    if (Rm >= 600) some("musicClub", 1 + std::floor(Rm / 1500), 0.15, 0.5);
    if (Rm >= 1200 || (soviet && Rm >= 500)) some("departmentStore", 1 + std::floor(Rm / 2000), 0.05, 0.22);
    if (Rm >= 400) some("policeStation", 1 + std::floor(Rm / 1800), 0.15, 0.45);
    if (Rm >= 400) some("fireStation", 1 + std::floor(Rm / 2000), 0.3, 0.65);
    if (Rm >= 900 && !soviet) some("hospital", 1 + std::floor(Rm / 2500), 0.35, 0.7, 7, quiet(310));
    if (Rm >= 450 && (soviet || Rm < 900)) some("polyclinic", 1 + std::floor(Rm / 1200), 0.3, 0.7, 7, quiet(320));
    if (harbour_ang || Rm >= 1500) some("marketHall", 1, 0.05, 0.2, 7, harbour_ang ? *harbour_ang : *sq_ang);
    some("supermarket", 1 + std::floor(Rm / 800), 0.35, 0.85, 8);
    some("petrolStation", 1 + std::floor(Rm / 900), 0.55, 0.95, 8);
  }
  return plan;
}

// The block (id) each landmark of a town lands on: the nearest suitable block within reach that
// no more important landmark took, or none. Resolved for the whole town at once (in order of
// importance).
std::vector<std::string> resolve_town(const World& world, const TownPlan& plan) {
  const Registry<District>& DISTRICTS = district_registry();
  std::vector<std::string> out(plan.anchors.size());
  std::unordered_set<std::string> claimed;
  std::vector<size_t> order(plan.anchors.size());
  std::iota(order.begin(), order.end(), size_t(0));
  js::sort(order, [&](size_t p, size_t q) { return plan.anchors[p].pri - plan.anchors[q].pri; });
  // (a civic building keeps off the blocks a highway crosses; a World without highways - World.js's
  // - crosses nothing)
  auto crossed = [&](const Block& b) -> bool {
    if (!world.highways) return false;
    for (const HighwayCorridor& c : world.highways->corridors_near(b.prop))
      if (c.hits_rect(b.prop)) return true;
    return false;
  };
  for (const size_t ai : order) {
    const TownAnchor& a = plan.anchors[ai];
    const Kind& kd = kind_of(a.kind);
    const bool civic = kd.civic;
    const double reach = vx(a.kind == "cemetery" || a.kind == "allotments" || a.kind == "school" || civic ? 240 : 150);
    std::vector<std::shared_ptr<const CellNet>> nets;  // (holding the blocks best points into)
    const Block* best = nullptr;
    double best_d = 0;
    for (const CellIJ& c : world.cells_overlapping(Rect{a.x - reach, a.y - reach, a.x + reach, a.y + reach})) {
      nets.push_back(world.cell_net(c.i, c.j));
      for (const Block& b : nets.back()->blocks) {
        if (claimed.count(b.id) || !DISTRICTS.has(b.district) || !suits(kd, b)) continue;
        if (civic && crossed(b)) continue;
        const double d = rect_dist(b.prop, a.x, a.y);
        if (d > reach) continue;
        if (!best || d < best_d || (d == best_d && b.id < best->id)) {
          best = &b;
          best_d = d;
        }
      }
    }
    out[ai] = best ? best->id : std::string();
    if (best) claimed.insert(best->id);
  }
  return out;
}

}  // namespace

const std::vector<std::string>& TownPlan::blocks(const World& world) const {
  return blocks_.get([&] { return resolve_town(world, *this); });
}

const TownPlan& town_plan(const World& world, const Settlement& s) {
  return *s.plan.get([&] { return std::shared_ptr<const TownPlan>(make_town_plan(world, s)); });
}

std::string landmark_use_of(const std::string& kind) {
  for (const Kind& k : kinds())
    if (k.id == kind) return k.civic ? "civic:" + kind : kind;
  return "";
}

std::vector<std::string> landmark_kinds() {
  std::vector<std::string> out;
  for (const Kind& k : kinds()) out.push_back(k.id);
  return out;
}

std::string landmark_use(const World& world, const std::string& block_id, const Rect& p) {
  const double pad = vx(250);
  // (a pointer into a plan: plans live on their settlement records, which live with the World)
  const TownAnchor* pick = nullptr;
  for (const Settlement* s : world.fields->settlements_in(Rect{p.x0 - pad, p.y0 - pad, p.x1 + pad, p.y1 + pad})) {
    const TownPlan& plan = town_plan(world, *s);
    const std::vector<std::string>* blocks = nullptr;
    for (size_t k = 0; k < plan.anchors.size(); ++k) {
      const TownAnchor& a = plan.anchors[k];
      if (std::fabs(a.x - (p.x0 + p.x1) / 2) > pad * 2 || std::fabs(a.y - (p.y0 + p.y1) / 2) > pad * 2) continue;
      if (!blocks) blocks = &plan.blocks(world);
      if ((*blocks)[k] != block_id) continue;
      if (!pick || a.pri < pick->pri) pick = &a;
    }
  }
  return pick ? landmark_use_of(pick->kind) : std::string();
}

std::string landmark_use(const World& world, const Block& block) { return landmark_use(world, block.id, block.prop); }

}  // namespace svx::city
