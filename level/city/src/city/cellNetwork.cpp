// svx_city — city/cellNetwork.hpp (voxel_city city/cellNetwork.js) and World::cell_net
// (world/World.js).
#include "city/cellNetwork.hpp"

#include <array>
#include <unordered_map>
#include <utility>

#include "city/diagonals.hpp"
#include "city/districts.hpp"
#include "city/flavors.hpp"
#include "city/streets.hpp"
#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "core/noise.hpp"
#include "network/arterials.hpp"
#include "terrain/terrain.hpp"
#include "world/World.hpp"
#include "world/caches.hpp"
#include "world/fields.hpp"
#include "world/island.hpp"

namespace svx::city {

namespace {

constexpr double kPi = 3.141592653589793;  // Math.PI

// STRIP_BY_DISTRICT[district] ?? "pits": the verge of a district's local streets.
std::string strip_by_district(const std::string& d) {
  if (d == "residential" || d == "suburban" || d == "village" || d == "microdistrict" || d == "projects") return "grass";
  if (d == "industrial" || d == "heavyIndustry" || d == "port") return "none";
  return "pits";  // (downtown, midtown, mixed; and every other district)
}

// Angled streets on (ANGLED_WORLD_PLAN.md S1)?
bool angled_roads(const Value& config) {
  const Value& a = config["world"]["angles"];
  if (!a["enabled"].truthy()) return false;
  const Value& roads = a["features"]["roads"];
  return !(roads.is_bool() && !roads.truthy());
}

// Wobble of country roads (x the axis-aligned world's) and of village streets through the angled
// world (ANGLED_WOBBLE).
constexpr double kAngledWobbleRural = 1.7;
constexpr double kAngledWobbleVillage = 0.45;
constexpr double kAngledWobbleMain = 0.35;

// Urbanization a diagonal boulevard needs somewhere along its piece of a cell.
constexpr double kDiagonalU = 0.3;

FlavorSettlement flavor_settlement(const Settlement& s) {
  FlavorSettlement f;
  f.flavor = s.flavor;
  // (settlement.i === 0 && settlement.j === 0: a village's i is a string, never 0)
  f.origin_cell = !s.village && s.i == 0 && s.j == 0;
  f.island = s.island;
  f.village = s.village;
  f.t = s.t;
  f.m = s.m;
  f.style = s.style;
  return f;
}

// flavor.mainRoad ?? config.city.mainRoad ?? "arterial"
std::string main_road(const Flavor* flavor, const Value& config) {
  if (flavor && !flavor->main_road.empty()) return flavor->main_road;
  const Value& m = config["city"]["mainRoad"];
  return m.is_nullish() ? std::string("arterial") : m.to_string();
}

template <class T>
const T* find_key(const ByDistrict<T>& map, const std::string& key) {
  for (const auto& kv : map)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

bool truthy_opt(const std::optional<double>& v) { return v && js::truthy(*v); }

// Country roads stay off terrain they could not climb: no road where the ground along the edge
// rises more than ~14% between samples (mountain flanks, canyon walls, ravines).
bool too_steep(const World& world, int axis, double fixed, double s0, double s1) {
  const double mid = (s0 + s1) / 2;
  if (world.fields->mountainness(axis == 0 ? fixed : mid, axis == 0 ? mid : fixed) > 0.45) return true;
  const double n = 6;
  const double step = (s1 - s0) / n;
  bool has_prev = false;
  double prev = 0;
  for (double k = 0; k <= n; k += 1) {
    const double s = s0 + k * step;
    const double h = world.terrain->sample(axis == 0 ? fixed : s, axis == 0 ? s : fixed).h;
    if (has_prev && std::fabs(h - prev) > 0.14 * step) return true;
    prev = h;
    has_prev = true;
  }
  return false;
}

// A road bending gently off its line (fixed) between s0 and s1: points every ~24 m, displaced by
// noise tapering to nothing at both ends. (The reference keeps the noise in a module variable per
// seed: here it is made per call, the same noise.)
std::vector<RoadPt> wobble(const World& world, int axis, double line, double fixed, double s0, double s1, double scale = 1, double s_off = 0) {
  const SimplexNoise noise(static_cast<double>(js::to_int32(world.seed) ^ 0x51f15e));
  const double amp = vx(world.config["city"]["ruralRoadWobble"].to_number()) * scale;
  const double len = s1 - s0;
  const double n = js::max(4.0, js::round(len / vx(24)));
  std::vector<RoadPt> pts;
  for (double k = 0; k <= n; k += 1) {
    const double t = k / n;
    const double s = s0 + len * t;
    const double taper = js::pow(js::sin(kPi * t), 0.7);
    const double off = amp * taper * noise.fbm2(line * 7.13 + axis * 31.1, (s - s_off) / vx(420), 3);
    const double f = js::round(fixed + off);
    pts.push_back(axis == 0 ? RoadPt{f, js::round(s)} : RoadPt{js::round(s), f});
  }
  return pts;
}

// Does a sub-cell centre lie on a harbour front? Near a big lake's shore, or on an island near the
// main town's harbour (one small port, not the whole waterfront).
bool port_near(const World& world, double x, double y) {
  // world.lakes?.shoreNear(x, y, vx(420)): nature/lakes is a stage of its own; a World without
  // lakes (World.js's, the port's until createWorld installs them) skips it. A World with lakes
  // needs this wired to them first.
  if (world.lakes) SVX_FAIL("cellNetwork: port_near does not ask the lakes yet (nature/lakes)");
  const IslandPlan* isl = world.fields->island.get();
  if (!isl) return false;
  const std::optional<Harbour>& h = isl->harbour();
  if (!h) return false;
  // (a small town's harbour is a few quays, not half the town)
  const double town_r = isl->sites[0].radius;
  const double reach = js::min(420 + (0.18 * isl->population) / 100, town_r * 0.5);
  if (js::hypot(x / 8 - h->x, y / 8 - h->y) > reach) return false;
  return isl->coast(x / 8, y / 8) < js::min(380.0, town_r * 0.32);
}

EdgeInfo edge_info_with(const World& world, const RoadSpecs& specs, int axis, double line, double span) {
  const ArterialGrid& A = *world.arterials;
  const MacroFields& F = *world.fields;
  const Value& config = world.config;
  const double seed = world.seed;
  const double fixed = A.line(axis, line);
  const double s0 = A.line(1 - axis, span);
  const double s1 = A.line(1 - axis, span + 1);
  // (a wrapping world hashes the canonical line and span: every lap agrees)
  const double cl = A.canon(line);
  const double cs = A.canon(span);
  const double s_off = A.line(1 - axis, span) - A.line_at(1 - axis, cs);
  const double mid = (s0 + s1) / 2;
  const double px = axis == 0 ? fixed : mid;
  const double py = axis == 0 ? mid : fixed;
  const Urban um = F.urban(px, py);
  const double ua = um.u;
  const double ub = F.urban(axis == 0 ? fixed : s0, axis == 0 ? s0 : fixed).u;
  const double uc = F.urban(axis == 0 ? fixed : s1, axis == 0 ? s1 : fixed).u;
  const double u = js::max(ua, (ub + uc) / 2);
  // through a village the country road becomes its main street (sidewalks, a gentle bend)
  const bool village = um.settlement && um.settlement->village;
  // a town's flavor may build narrow, gently bending main streets (old northern towns)
  const Flavor* flavor = um.settlement && !village ? &flavor_of_settlement(um.settlement) : nullptr;
  const double chance = config["city"]["ruralRoadChance"].num(0.6);
  std::string cls;  // ("": null)
  if (village) {
    cls = u > 0.12 ? "village" : u > 0.04 || hash_float(seed, cl * 2 + axis, cs, 991) < chance ? "rural" : "";
  } else if (u > 0.2) {
    // (small places build two-lane main streets instead of avenues: city.mainRoad)
    cls = main_road(flavor, config);
    // not every line is an avenue: away from the centre about half the lines (never those with
    // a subway under them) are two-lane collectors, so the grid reads as a few main roads with
    // streets between them
    const Value& subway = config["subway"];
    const bool sub = !(subway["enabled"].is_bool() && !subway["enabled"].truthy()) && std::fmod(std::fabs(line + axis * 7), subway["lineEvery"].num(2)) == 0;
    if (cls == "arterial" && !sub && um.core < 0.3 && config["world"]["mode"].to_string() != "infiniteCity" && hash_float(seed, cl * 2 + axis, 0, 994) < 0.55)
      cls = "collector";
  } else if (u > 0.06) {
    // (the lanes out of a small island town: not every edge round it)
    cls = !F.island || hash_float(seed, cl * 2 + axis, cs, 992) < 0.5 ? "rural" : "";
  } else if (hash_float(seed, cl * 2 + axis, cs, 991) < chance && !too_steep(world, axis, fixed, s0, s1)) {
    cls = "rural";
  }
  if (F.island) {
    // island: trunk roads join every place; nothing runs out over the sea
    if (cls.empty() && F.island->trunk_edges(world).has(axis, line, span)) cls = "rural";
    if (!cls.empty() && (axis == 0 ? world.sea_hits_seg(fixed, s0, fixed, s1, 4) : world.sea_hits_seg(s0, fixed, s1, fixed, 4))) cls = "";
  }
  EdgeInfo e;
  e.axis = axis;
  e.line = line;
  e.span = span;
  e.fixed = fixed;
  e.s0 = s0;
  e.s1 = s1;
  e.cls = cls;
  if (!cls.empty())
    if (const RoadSpec* sp = specs.get(cls)) e.spec = *sp;
  if (axis == 0)
    e.pts = {{fixed, s0}, {fixed, s1}};
  else
    e.pts = {{s0, fixed}, {s1, fixed}};
  // (the angled world: country and village roads wander more freely)
  const bool free = angled_roads(config);
  if (cls == "rural")
    e.pts = wobble(world, axis, cl, fixed, s0, s1, (village ? 0.25 : 1) * (free ? kAngledWobbleRural : 1), s_off);
  else if (village)
    e.pts = wobble(world, axis, cl, fixed, s0, s1, free ? kAngledWobbleVillage : 0.2, s_off);
  else if (!cls.empty() && flavor && truthy_opt(flavor->main_road_wobble))
    e.pts = wobble(world, axis, cl, fixed, s0, s1, *flavor->main_road_wobble, s_off);
  // old-town main streets are cobbled
  if (!cls.empty() && cls != "rural" && flavor && truthy_opt(flavor->cobble_within) && F.settlement_distance(*um.settlement, px, py) < *flavor->cobble_within)
    e.paving = "cobble";
  // how far the road bends off its line: blocks beside it keep that much further back
  double wob = 0;
  for (const RoadPt& p : e.pts) wob = js::max(wob, std::fabs((axis == 0 ? p.x : p.y) - fixed));
  e.hr = e.spec ? e.spec->hr : 0;
  e.wob = wob;
  e.u = u;
  return e;
}

RoadSide road_side(const Road& r) { return RoadSide{r.cls, r.hr, r.id}; }

// (a bending edge road: its blocks keep clear of the whole bend)
RoadSide edge_side(const EdgeInfo& e) {
  RoadSide s;
  if (!e.cls.empty()) s.cls = e.cls;
  s.hr = e.hr + (!e.cls.empty() ? e.wob : 0);
  return s;
}

}  // namespace

const Flavor& flavor_of_settlement(const Settlement* s) {
  if (!s) return flavor_of(nullptr);
  const FlavorSettlement f = flavor_settlement(*s);
  return flavor_of(&f);
}

EdgeInfo edge_info(const World& world, int axis, double line, double span) {
  const RoadSpecs specs = road_specs(world.config);
  return edge_info_with(world, specs, axis, line, span);
}

std::shared_ptr<const CellNet> plan_cell_network(const World& world, double i, double j) {
  const ArterialGrid& A = *world.arterials;
  const MacroFields& F = *world.fields;
  const Value& config = world.config;
  const double seed = world.seed;
  const Registry<District>& DISTRICTS = district_registry();
  // a wrapping world: the cell's canonical id seeds everything in it
  const std::string id = js::cat("C", A.canon(i), "_", A.canon(j));
  // (and positions that seed anything are taken relative to the canonical cell)
  const double lap_x = A.line(0, i) - A.line_at(0, A.canon(i));
  const double lap_y = A.line(1, j) - A.line_at(1, A.canon(j));
  const Rect rect = A.cell_rect(i, j);
  const RoadSpecs specs = road_specs(config);
  Rng rng = Rng::from(seed, id, "network");
  auto net = std::make_shared<CellNet>();
  std::vector<std::shared_ptr<Road>> roads;
  std::vector<Block>& blocks = net->blocks;
  std::vector<SubCell>& subcells = net->subcells;

  const bool angled = angled_roads(config);
  double road_count = 0;
  // (island: a road over the sea keeps its record and id but is not built)
  auto make_road = [&](const std::string& cls, std::vector<RoadPt> pts, bool build) -> std::shared_ptr<Road> {
    const RoadSpec* spec = specs.get(cls);
    if (!spec) SVX_FAIL("cellNetwork: a road of a class config.roads has no spec of");
    auto road = std::make_shared<Road>();
    road->id = js::cat(id, "/r", road_count);
    road_count += 1;
    road->cell = id;
    road->cls = cls;
    road->pts = std::move(pts);
    road->hc = spec->hc;
    road->hr = spec->hr;
    road->corner = spec->corner;
    road->median = spec->median;
    road->parking = spec->parking;
    road->lanes = spec->lanes;
    road->lane = spec->lane;
    road->sidewalk = spec->sidewalk;
    road->shoulder = spec->shoulder;
    // (the angled world: the cell that owns it, whose road view sees every road it meets, roadLevel)
    if (angled) road->home = std::array<double, 2>{i, j};
    road->ci = i;
    road->cj = j;
    if (build) roads.push_back(road);
    return road;
  };

  CellEdges& edges = net->edges;
  edges.W = edge_info_with(world, specs, 0, i, j);
  edges.E = edge_info_with(world, specs, 0, i + 1, j);
  edges.N = edge_info_with(world, specs, 1, j, i);
  edges.S = edge_info_with(world, specs, 1, j + 1, i);
  for (const EdgeInfo* e : {&edges.W, &edges.N}) {
    if (e->cls.empty()) continue;
    std::shared_ptr<Road> r = make_road(e->cls, e->pts, true);
    r->arterial_edge = e == &edges.W ? "W" : "N";
    if (!e->paving.empty()) r->paving = e->paving;
  }

  // diagonal boulevards (city/diagonals): the pieces of the global lines crossing this cell,
  // built where they run through a town
  struct Diagonal {
    DiagonalPiece d;
    std::shared_ptr<Road> road;
    double dx, dy;
  };
  std::vector<Diagonal> diagonals;
  if (diagonals_on(config)) {
    for (const DiagonalPiece& d : diagonal_pieces(config, rect)) {
      std::optional<Urban> best;
      for (int q = 0; q <= 4; ++q) {
        Urban ur = F.urban(d.a.x + ((d.b.x - d.a.x) * q) / 4, d.a.y + ((d.b.y - d.a.y) * q) / 4);
        if (!best || ur.u > best->u) best = std::move(ur);
      }
      if (best->u < kDiagonalU || !best->settlement || best->settlement->village || world.sea_hits_seg(d.a.x, d.a.y, d.b.x, d.b.y, 4)) continue;
      const Flavor& flavor = flavor_of_settlement(best->settlement);
      const double mx = (d.a.x + d.b.x) / 2;
      const double my = (d.a.y + d.b.y) / 2;
      const bool cobble = truthy_opt(flavor.cobble_within) && F.settlement_distance(*best->settlement, mx, my) < *flavor.cobble_within;
      std::shared_ptr<Road> road = make_road(main_road(&flavor, config), {{d.a.x, d.a.y}, {d.b.x, d.b.y}}, true);
      road->diagonal = RoadDiagonal{d.fam->f, d.k};
      if (cobble) road->paving = "cobble";
      diagonals.push_back({d, road, d.b.x - d.a.x, d.b.y - d.a.y});
    }
  }

  // collectors split urban cells into up to four sub-cells
  const double cx = (rect.x0 + rect.x1) / 2;
  const double cy = (rect.y0 + rect.y1) / 2;
  const Urban center_urban = F.urban(cx, cy);
  const double w = rect.x1 - rect.x0;
  const double h = rect.y1 - rect.y0;
  std::vector<double> xs{rect.x0};
  std::vector<double> ys{rect.y0};
  const double thr = config["city"]["collectorUrbanThreshold"].to_number();
  std::optional<double> col_x;
  std::optional<double> col_y;
  // villages keep their organic streets: no collector grid (nor do towns whose flavor says so)
  if (center_urban.u > thr && !(center_urban.settlement && center_urban.settlement->village) && flavor_of_settlement(center_urban.settlement).collectors) {
    if (w > vx(380)) {
      col_x = js::round((rect.x0 + w * (0.5 + (rng.next() - 0.5) * 0.24)) / 8) * 8;
      xs.push_back(*col_x);
    }
    if (h > vx(380)) {
      col_y = js::round((rect.y0 + h * (0.5 + (rng.next() - 0.5) * 0.24)) / 8) * 8;
      ys.push_back(*col_y);
    }
  }
  // a village centred in this cell gets its main street through the centre (and a cross street
  // when it is large): the houses gather around it
  std::string col_cls = "collector";
  if (!col_x && !col_y) {
    for (const Settlement* v : F.nearest_villages(cx, cy)) {
      const double m = vx(70);
      if (v->x < rect.x0 + m || v->x > rect.x1 - m || v->y < rect.y0 + m || v->y > rect.y1 - m) continue;
      const bool along_y = hash_float(seed, v->cx, v->cy, 993) < 0.5;
      const bool big = v->radius > vx(420);
      if (along_y || big) {
        col_x = js::round(v->x / 8) * 8;
        xs.push_back(*col_x);
      }
      if (!along_y || big) {
        col_y = js::round(v->y / 8) * 8;
        ys.push_back(*col_y);
      }
      col_cls = "village";
      break;
    }
  }
  xs.push_back(rect.x1);
  ys.push_back(rect.y1);
  // (island: a collector that would run out over the sea is not built; its sub-cells keep it as
  // their side) (the angled world: a village's own main street bends gently through it)
  auto bend = [&](double ax, double ay, double bx, double by) {
    return ax == bx ? wobble(world, 0, A.canon(i) * 131 + A.canon(j) * 7 + 5000, ax, ay, by, kAngledWobbleMain, lap_y)
                    : wobble(world, 1, A.canon(i) * 131 + A.canon(j) * 7 + 6000, ay, ax, bx, kAngledWobbleMain, lap_x);
  };
  auto collector = [&](double ax, double ay, double bx, double by) {
    std::vector<RoadPt> pts = angled && col_cls == "village" ? bend(ax, ay, bx, by) : std::vector<RoadPt>{{ax, ay}, {bx, by}};
    return make_road(col_cls, std::move(pts), !world.sea_hits_seg(ax, ay, bx, by, 3));
  };

  // how the collectors divide a town cell: a plain cross, a cross whose north-south street jogs
  // where it meets the other, a T (one collector stops at the other), or a single collector; not
  // every cell is four equal squares
  struct Sub {
    Rect rect;
    BlockSides sides;
  };
  std::vector<Sub> subs;
  std::shared_ptr<Road> col_x_road;
  std::shared_ptr<Road> col_y_road;
  const double layout_pick = col_x && col_y && col_cls == "collector" ? rng.next() : -1;
  if (layout_pick >= 0.3) {
    col_y_road = collector(rect.x0, *col_y, rect.x1, *col_y);
    BlockSides side;
    side.W = edge_side(edges.W);
    side.E = edge_side(edges.E);
    side.N = edge_side(edges.N);
    side.S = edge_side(edges.S);
    auto row = [&](double y0, double y1, std::optional<double> x, const std::shared_ptr<Road>& road, bool top) {
      const RoadSide n = top ? side.N : road_side(*col_y_road);
      const RoadSide s = top ? road_side(*col_y_road) : side.S;
      if (!x) {
        subs.push_back({{rect.x0, y0, rect.x1, y1}, BlockSides{n, side.E, s, side.W}});
      } else {
        subs.push_back({{rect.x0, y0, *x, y1}, BlockSides{n, road_side(*road), s, side.W}});
        subs.push_back({{*x, y0, rect.x1, y1}, BlockSides{n, side.E, s, road_side(*road)}});
      }
    };
    if (layout_pick < 0.62) {
      // a jog: the street continues 30-90 m to one side
      const double m = vx(30 + 60 * rng.next());
      const double sg = rng.chance(0.5) ? 1 : -1;
      const double off = m * sg;
      const double xb = js::round(js::max(rect.x0 + w * 0.28, js::min(rect.x1 - w * 0.28, *col_x + off)) / 8) * 8;
      std::shared_ptr<Road> top = collector(*col_x, rect.y0, *col_x, *col_y);
      std::shared_ptr<Road> bot = collector(xb, *col_y, xb, rect.y1);
      col_x_road = top;
      row(rect.y0, *col_y, col_x, top, true);
      row(*col_y, rect.y1, xb, bot, false);
    } else if (layout_pick < 0.88) {
      // a T: the north-south collector stops at the other one (north or south half)
      const bool north = rng.chance(0.5);
      std::shared_ptr<Road> stub = north ? collector(*col_x, rect.y0, *col_x, *col_y) : collector(*col_x, *col_y, *col_x, rect.y1);
      col_x_road = stub;
      row(rect.y0, *col_y, north ? col_x : std::nullopt, stub, true);
      row(*col_y, rect.y1, north ? std::nullopt : col_x, stub, false);
    } else {
      // a single east-west collector
      row(rect.y0, *col_y, std::nullopt, nullptr, true);
      row(*col_y, rect.y1, std::nullopt, nullptr, false);
    }
  } else {
    col_x_road = col_x ? collector(*col_x, rect.y0, *col_x, rect.y1) : nullptr;
    col_y_road = col_y ? collector(rect.x0, *col_y, rect.x1, *col_y) : nullptr;
    for (size_t b = 0; b + 1 < ys.size(); ++b)
      for (size_t a = 0; a + 1 < xs.size(); ++a) {
        BlockSides s;
        s.W = a == 0 ? edge_side(edges.W) : road_side(*col_x_road);
        s.E = a == xs.size() - 2 ? edge_side(edges.E) : road_side(*col_x_road);
        s.N = b == 0 ? edge_side(edges.N) : road_side(*col_y_road);
        s.S = b == ys.size() - 2 ? edge_side(edges.S) : road_side(*col_y_road);
        subs.push_back({{xs[a], ys[b], xs[a + 1], ys[b + 1]}, s});
      }
  }

  // district of a point from its urban sample (macro fields + the settlement's flavor)
  auto district_at = [&](double x, double y, const Urban& ur, double key) -> std::string {
    const double dn = F.district_noise(x, y);
    const double ind = F.industry_noise(x, y);
    DistrictFields df;
    df.u = ur.u;
    df.core = ur.core;
    df.village = ur.settlement && ur.settlement->village;
    df.port = ur.u > 0.12 && port_near(world, x, y);
    df.dn = dn;
    df.ind = ind;
    df.seed = seed;
    df.key = key;
    const std::string base = classify_district(df);
    if (!ur.settlement) return flavored_district_id(base, nullptr, nullptr);
    const FlavorSettlement fs = flavor_settlement(*ur.settlement);
    FlavorPlace ctx;
    ctx.d = F.settlement_distance(*ur.settlement, x, y);
    ctx.dn = dn;
    ctx.ind = ind;
    ctx.u = ur.u;
    ctx.core = ur.core;
    return flavored_district_id(base, &fs, &ctx);
  };
  const bool infinite = config["world"]["mode"].to_string() == "infiniteCity";

  double sub_index = 0;
  for (const Sub& sub_rec : subs) {
    const Rect& sub_rect = sub_rec.rect;
    const BlockSides& sides = sub_rec.sides;
    const double sx = (sub_rect.x0 + sub_rect.x1) / 2;
    const double sy = (sub_rect.y0 + sub_rect.y1) / 2;
    const Urban ur = F.urban(sx, sy);
    const std::string sub_id = js::cat(id, "/s", sub_index);
    // island: a sub-cell that is mostly sea stays sea
    if (F.island && world.sea_share(sub_rect) > 0.5) {
      SubCell sc;
      sc.id = sub_id;
      sc.rect = sub_rect;
      sc.sides = sides;
      sc.district = "sea";
      sc.u = ur.u;
      sc.core = ur.core;
      sc.settlement = ur.settlement ? ur.settlement->id : std::string();
      subcells.push_back(std::move(sc));
      sub_index += 1;
      continue;
    }
    // a sub-cell of open country on the edge of a town (its centre rural, the town reaching into
    // a corner of it) is laid out like the town's outskirts there; its blocks are kept only where
    // the town reaches
    Urban pu = ur;
    double px = sx;
    double py = sy;
    bool fringe = false;
    if (ur.u < 0.14 && !(ur.settlement && ur.settlement->village) && !infinite) {
      const double qs[8][2] = {
          {sub_rect.x0 + 24, sub_rect.y0 + 24}, {sub_rect.x1 - 24, sub_rect.y0 + 24}, {sub_rect.x0 + 24, sub_rect.y1 - 24}, {sub_rect.x1 - 24, sub_rect.y1 - 24},
          {sx, sub_rect.y0 + 24},               {sx, sub_rect.y1 - 24},               {sub_rect.x0 + 24, sy},               {sub_rect.x1 - 24, sy},
      };
      for (const auto& q : qs) {
        Urban qu = F.urban(q[0], q[1]);
        if (qu.settlement && !qu.settlement->village && qu.u > 0.17 && qu.u > pu.u) {
          pu = std::move(qu);
          px = q[0];
          py = q[1];
          fringe = true;
        }
      }
    }
    const std::string district_id = district_at(px, py, pu, hash_string(sub_id));
    const District& district0 = DISTRICTS.get(district_id);
    // a flavor may lay a district out in another street pattern (organic old northern towns;
    // `adaptive`: the whole town grows organically)
    const Flavor* fl = pu.settlement && !pu.settlement->village ? &flavor_of_settlement(pu.settlement) : nullptr;
    // (a sub-cell the historic core reaches into grows organically too, block by block)
    bool near_core = false;
    if (fl && truthy_opt(fl->old_core) && pu.settlement) {
      const Settlement& S = *pu.settlement;
      const double qx = js::max(sub_rect.x0, js::min(sub_rect.x1, S.x));
      const double qy = js::max(sub_rect.y0, js::min(sub_rect.y1, S.y));
      near_core = F.settlement_distance(S, qx, qy) < *fl->old_core;
    }
    const bool adaptive = fl && (fl->adaptive || near_core);
    std::string pat;  // ("": null)
    if (fl) {
      if (const std::string* p = find_key(fl->patterns, district_id))
        pat = *p;
      else if (adaptive && pattern_family(district0.streets.pattern) != "none")
        pat = "organic";
    }
    District altered;
    const District* district = &district0;
    if (!pat.empty() && pat != district0.streets.pattern) {
      altered = district0;
      altered.streets.pattern = pat;
      district = &altered;
    }
    {
      SubCell sc;
      sc.id = sub_id;
      sc.rect = sub_rect;
      sc.sides = sides;
      sc.district = district_id;
      sc.u = pu.u;
      sc.core = pu.core;
      sc.settlement = pu.settlement ? pu.settlement->id : std::string();
      sc.fringe = fringe;
      subcells.push_back(std::move(sc));
    }
    Rng sub_rng = Rng::from(seed, sub_id, "streets");
    const std::string strip = strip_by_district(district_id);
    const std::string paving = district->streets.paving;  // (?? null)
    // the pattern's streets and blocks wait until every block has been looked at: a street is
    // built only where it serves a kept block
    struct PendingRoad {
      std::shared_ptr<Road> road;
      bool sea;
    };
    struct PendingBlock {
      Rect r;
      BlockSides s;
      std::optional<Poly> poly;
    };
    std::vector<PendingRoad> pending_roads;
    std::vector<PendingBlock> pending_blocks;
    StreetEmit emit;
    emit.road = [&](const std::string& cls, double ax, double ay, double bx, double by, const std::string* pav) -> RoadSide {
      const std::string& p = pav ? *pav : paving;
      std::shared_ptr<Road> r = make_road(cls, {{ax, ay}, {bx, by}}, false);
      r->sub = sub_id;
      r->strip = strip;
      if (!p.empty() && cls != "collector") r->paving = p;
      pending_roads.push_back({r, world.sea_hits_seg(ax, ay, bx, by, 3)});
      return road_side(*r);
    };
    // (a polygon block of the angled world: its bounding rect and sides, and the polygon, blockPoly)
    emit.block = [&](const Rect& r, const BlockSides& bs, const Poly* poly) {
      pending_blocks.push_back({r, bs, poly ? std::optional<Poly>(*poly) : std::nullopt});
    };
    // (adaptive patterns: the district of a part of the sub-cell, laid out organically)
    if (adaptive)
      emit.local = [&](const Rect& r) -> const District* {
        // (the most urban of the centre and the corners: a part the town only reaches into is
        // still split, its blocks kept where built)
        double lx = (r.x0 + r.x1) / 2;
        double ly = (r.y0 + r.y1) / 2;
        Urban lu = F.urban(lx, ly);
        if (lu.u < 0.25) {
          const double qs[4][2] = {{r.x0 + 16, r.y0 + 16}, {r.x1 - 16, r.y0 + 16}, {r.x0 + 16, r.y1 - 16}, {r.x1 - 16, r.y1 - 16}};
          for (const auto& q : qs) {
            Urban qu = F.urban(q[0], q[1]);
            if (qu.u > lu.u + 0.05 && qu.settlement && !qu.settlement->village) {
              lx = q[0];
              ly = q[1];
              lu = std::move(qu);
            }
          }
        }
        if (lu.u < 0.1) return &DISTRICTS.get("rural");
        const District* d = DISTRICTS.maybe(district_at(lx, ly, lu, hash_string(js::cat(sub_id, "/", r.x0 - lap_x, ",", r.y0 - lap_y))));
        // (parks come from the block programs and landmarks: never a whole cell of lawn)
        if (!d || d->id == "park") return &DISTRICTS.get("residential");
        return d->lots.mode == "micro" && js::min(r.x1 - r.x0, r.y1 - r.y0) < vx(120) ? &DISTRICTS.get("residential") : d;
      };
    // (the angled world: old-town cuts tilt now and then, except where a diagonal boulevard runs
    // through the sub-cell: no shallow crossings)
    bool crossed = false;
    for (const Diagonal& d : diagonals)
      if (line_crosses(rect_poly(sub_rect, sides), d.d.a, d.dx, d.dy)) {
        crossed = true;
        break;
      }
    const StreetPattern pattern = street_pattern(district->streets.pattern);
    if (!pattern) SVX_FAIL("cellNetwork: a district's street pattern is not one of STREET_PATTERNS");
    StreetOpts opts;
    opts.angled = true;
    opts.tilt = crossed ? 0 : 0.35;
    pattern(StreetSub{sub_rect, sides}, *district, sub_rng, emit, angled ? &opts : nullptr);
    // a diagonal boulevard splits the blocks it runs through along its centre line
    for (const Diagonal& d : diagonals) {
      std::vector<PendingBlock> next;
      for (PendingBlock& pb : pending_blocks) {
        const Poly poly = pb.poly ? *pb.poly : rect_poly(pb.r, pb.s);
        if (!line_crosses(poly, d.d.a, d.dx, d.dy)) {
          next.push_back(std::move(pb));
          continue;
        }
        std::pair<std::optional<Poly>, std::optional<Poly>> lr = split_poly(poly, d.d.a, d.dx, d.dy, road_side(*d.road));
        for (std::optional<Poly>* child : {&lr.first, &lr.second}) {
          if (!*child) continue;
          const PolyBlock b = poly_block(**child);
          if (!b.cuts.empty())
            next.push_back({b.r, b.s, std::move(**child)});
          else
            next.push_back({b.r, b.s, std::nullopt});
        }
      }
      pending_blocks = std::move(next);
    }
    const std::string_view family = pattern_family(district->streets.pattern);
    struct Kept {
      Rect r;
      BlockSides s;
      std::optional<Poly> poly;
      std::string district;
    };
    std::vector<Kept> kept;
    for (PendingBlock& pb : pending_blocks) {
      const Rect r = pb.r;
      // (island: blocks that are mostly sea are left to the sea)
      if (F.island && world.sea_share(r) > 0.5) continue;
      std::string d_id = district_id;
      if (family != "none" && (!district->port || adaptive)) {
        // (a polygon block: its own middle, not its bounding rect's)
        const std::optional<Point2> mid = pb.poly ? std::optional<Point2>(poly_centroid(*pb.poly)) : std::nullopt;
        const double bx = mid ? mid->x : (r.x0 + r.x1) / 2;
        const double by = mid ? mid->y : (r.y0 + r.y1) / 2;
        const Urban bu = F.urban(bx, by);
        const double key = hash_string(js::cat(sub_id, "/", r.x0 - lap_x, ",", r.y0 - lap_y));
        // the town's edge frays: out on the rim a block is built only where the town reaches
        // (fields, meadows and woods take the rest)
        if (!infinite && bu.u < 0.6) {
          const double reach = bu.u + 0.2 * F.fringe_noise(bx, by) + 0.12 * (std::fmod(key, 1000) / 1000 - 0.5);
          if (reach < 0.2 || (bu.settlement && bu.settlement->village)) continue;
        }
        // each block takes the district of its own place in the town (not a park or open
        // country, and superblocks only on big blocks)
        const std::string own = district_at(bx, by, bu, key);
        const District* od = DISTRICTS.maybe(own);
        const bool small = js::min(r.x1 - r.x0, r.y1 - r.y0) < vx(70);
        if (od && own != "rural" && pattern_family(od->streets.pattern) != "none" && !(od->lots.mode == "micro" && small)) d_id = own;
      }
      kept.push_back({r, pb.s, std::move(pb.poly), d_id});
    }
    // build the streets that front kept blocks, trimmed to the stretch they serve
    std::unordered_map<std::string, std::array<double, 2>> extent;  // (looked up only)
    auto grow = [&](const std::string& rid, double lo, double hi) {
      auto it = extent.find(rid);
      if (it != extent.end()) {
        it->second[0] = js::min(it->second[0], lo);
        it->second[1] = js::max(it->second[1], hi);
      } else {
        extent.emplace(rid, std::array<double, 2>{lo, hi});
      }
    };
    // (a slanted street's stretch is measured along it, from its first point)
    std::unordered_map<std::string, const Road*> by_id;
    for (const PendingRoad& pr : pending_roads) by_id.emplace(pr.road->id, pr.road.get());
    auto along = [](const Road& r, double x, double y) {
      const RoadPt& p = r.pts[0];
      const RoadPt& q = r.pts[1];
      return ((x - p.x) * (q.x - p.x) + (y - p.y) * (q.y - p.y)) / js::hypot(q.x - p.x, q.y - p.y);
    };
    for (const Kept& k : kept) {
      if (k.poly) {
        // a polygon block: the stretch of each edge's street it really fronts (measured the way
        // that street is trimmed: along x, along y, or along itself)
        const Poly& poly = *k.poly;
        for (size_t e = 0; e < poly.size(); ++e) {
          const PolyPt& p = poly[e];
          const PolyPt& q = poly[(e + 1) % poly.size()];
          if (!p.side.id) continue;
          auto it = by_id.find(*p.side.id);
          if (it == by_id.end()) continue;
          const Road& r = *it->second;
          const RoadPt& ra = r.pts[0];
          const RoadPt& rb = r.pts[1];
          if (ra.y == rb.y) {
            grow(r.id, js::min(p.x, q.x), js::max(p.x, q.x));
          } else if (ra.x == rb.x) {
            grow(r.id, js::min(p.y, q.y), js::max(p.y, q.y));
          } else {
            const double a = along(r, p.x, p.y);
            const double b = along(r, q.x, q.y);
            grow(r.id, js::min(a, b), js::max(a, b));
          }
        }
        continue;
      }
      for (const char side : {'N', 'S', 'W', 'E'}) {
        const RoadSide& s = side == 'N' ? k.s.N : side == 'S' ? k.s.S : side == 'W' ? k.s.W : k.s.E;
        if (!s.id || s.id->empty()) continue;
        const bool horizontal = side == 'N' || side == 'S';
        grow(*s.id, horizontal ? k.r.x0 : k.r.y0, horizontal ? k.r.x1 : k.r.y1);
      }
    }
    for (const PendingRoad& pr : pending_roads) {
      Road& r = *pr.road;
      auto it = extent.find(r.id);
      if (it == extent.end() || pr.sea) continue;
      const std::array<double, 2>& e = it->second;
      const RoadPt p = r.pts[0];
      const RoadPt q = r.pts[1];
      if (p.x != q.x && p.y != q.y) {
        // a slanted street (the angled world): trimmed along itself
        const double L = js::hypot(q.x - p.x, q.y - p.y);
        const double t0 = js::max(0.0, e[0]);
        const double t1 = js::min(L, e[1]);
        if (t1 - t0 < 1) continue;
        const double ux = (q.x - p.x) / L;
        const double uy = (q.y - p.y) / L;
        if (t0 > 0 || t1 < L) r.pts = {{p.x + ux * t0, p.y + uy * t0}, {p.x + ux * t1, p.y + uy * t1}};
      } else if (p.y == q.y) {
        const double x0 = js::max(js::min(p.x, q.x), e[0]);
        const double x1 = js::min(js::max(p.x, q.x), e[1]);
        r.pts = {{x0, p.y}, {x1, p.y}};
      } else {
        const double y0 = js::max(js::min(p.y, q.y), e[0]);
        const double y1 = js::min(js::max(p.y, q.y), e[1]);
        r.pts = {{p.x, y0}, {p.x, y1}};
      }
      roads.push_back(pr.road);
    }
    for (const Kept& k : kept) {
      const Rect& r = k.r;
      const BlockSides& s0 = k.s;
      const District& bd = k.district == district_id ? *district : DISTRICTS.get(k.district);
      auto push_block = [&](const Rect& rr, const BlockSides& ss, const Poly* poly) {
        Block b;
        b.id = js::cat(id, "/b", static_cast<double>(blocks.size()));
        b.cell = id;
        b.sub = sub_id;
        b.district = k.district;
        b.rect = rr;
        b.sides = ss;
        b.prop = Rect{rr.x0 + ss.W.hr, rr.y0 + ss.N.hr, rr.x1 - ss.E.hr - 1, rr.y1 - ss.S.hr - 1};
        b.u = pu.u;
        b.core = pu.core;
        // (a polygon block: its centre-line polygon and the property half-planes of its slanted edges)
        if (poly) {
          b.poly = *poly;
          b.cuts = poly_block(*poly).cuts;
        }
        blocks.push_back(std::move(b));
      };
      if (k.poly) {
        push_block(r, s0, &*k.poly);
        continue;
      }
      // service alleys split long urban blocks along their long axis
      const double alley_chance = bd.lots.alley_chance;  // (?? 0)
      const double bw = r.x1 - r.x0;
      const double bh = r.y1 - r.y0;
      const bool long_x = bw >= bh;
      const double long_len = long_x ? bw : bh;
      const double short_len = long_x ? bh : bw;
      Rng b_rng = Rng::from(seed, sub_id, "alley", r.x0 - lap_x, r.y0 - lap_y);
      if (alley_chance > 0 && long_len > vx(80) && short_len > vx(52) && b_rng.chance(alley_chance)) {
        const std::string alley_cls = bd.streets.lane_class.empty() ? std::string("alley") : bd.streets.lane_class;
        auto alley_road = [&](std::vector<RoadPt> pts) {
          std::shared_ptr<Road> a = make_road(alley_cls, std::move(pts), true);
          a->sub = sub_id;
          a->strip = "none";
          if (!bd.streets.paving.empty()) a->paving = bd.streets.paving;
          return RoadSide{alley_cls, a->hr, a->id};
        };
        if (long_x) {
          const double ay = js::round((r.y0 + r.y1) / 2 / 8) * 8;
          const RoadSide as = alley_road({{r.x0, ay}, {r.x1, ay}});
          Rect ra = r;
          ra.y1 = ay;
          BlockSides sa = s0;
          sa.S = as;
          push_block(ra, sa, nullptr);
          Rect rb = r;
          rb.y0 = ay;
          BlockSides sb = s0;
          sb.N = as;
          push_block(rb, sb, nullptr);
        } else {
          const double ax = js::round((r.x0 + r.x1) / 2 / 8) * 8;
          const RoadSide as = alley_road({{ax, r.y0}, {ax, r.y1}});
          Rect ra = r;
          ra.x1 = ax;
          BlockSides sa = s0;
          sa.E = as;
          push_block(ra, sa, nullptr);
          Rect rb = r;
          rb.x0 = ax;
          BlockSides sb = s0;
          sb.W = as;
          push_block(rb, sb, nullptr);
        }
        continue;
      }
      push_block(r, s0, nullptr);
    }
    sub_index += 1;
  }
  // an old town's collectors are cobbled where it lies on both sides of them
  for (int t = 0; t < 2; ++t) {
    const bool is_x = t == 0;
    const std::shared_ptr<Road>& road = is_x ? col_x_road : col_y_road;
    if (!road) continue;
    std::vector<const SubCell*> both;
    for (const SubCell& sc : subcells)
      if (is_x ? sc.rect.x0 == *col_x || sc.rect.x1 == *col_x : sc.rect.y0 == *col_y || sc.rect.y1 == *col_y) both.push_back(&sc);
    bool all = !both.empty();
    for (const SubCell* sc : both) {
      const District* d = DISTRICTS.maybe(sc->district);
      if (!d || d->streets.paving.empty()) {
        all = false;
        break;
      }
    }
    if (all) {
      const std::string& pav = DISTRICTS.get(both[0]->district).streets.paving;
      if (!pav.empty()) road->paving = pav;
    }
  }

  net->id = id;
  net->i = i;
  net->j = j;
  net->rect = rect;
  net->roads.assign(roads.begin(), roads.end());
  return net;
}

std::shared_ptr<const CellNet> World::cell_net(double i, double j) const {
  return caches().cell_nets.get(cell_key(i, j), [&] { return plan_cell_network(*this, i, j); });
}

}  // namespace svx::city
