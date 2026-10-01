// svx_city — world/island.hpp (voxel_city world/island.js).
#include "world/island.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "network/arterials.hpp"
#include "terrain/terrain.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"

namespace svx::city {

namespace {

constexpr double kPi = 3.141592653589793;  // Math.PI

double island_seed(const Value& config) { return config["seed"].to_number(); }
SimplexNoise island_noise(const Value& config, const char* name) {
  return SimplexNoise(derive_seed(island_seed(config), std::string("island.") + name));
}

}  // namespace

size_t TrunkEdges::KeyHash::operator()(const Key& k) const {
  uint64_t a, l, s;
  std::memcpy(&a, &k.a, 8);
  std::memcpy(&l, &k.l, 8);
  std::memcpy(&s, &k.s, 8);
  uint64_t h = a * 0x9e3779b97f4a7c15ull;
  h ^= l + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  h ^= s + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return static_cast<size_t>(h);
}

void TrunkEdges::add(double axis, double line, double span) {
  // (JS's key is the string `${axis}:${line}:${span}`: -0 and 0 print alike)
  const Key k{axis + 0.0, line + 0.0, span + 0.0};
  if (set_.insert(k).second) order_.push_back({k.a, k.l, k.s});
}

IslandPlan::IslandPlan(const Value& config)
    : n_warp_u_(island_noise(config, "warpU")),
      n_warp_v_(island_noise(config, "warpV")),
      n_bays_(island_noise(config, "bays")),
      n_points_(island_noise(config, "points")),
      n_high_(island_noise(config, "highland")),
      n_fjord_(island_noise(config, "fjord")),
      n_fjord_warp_(island_noise(config, "fjordWarp")),
      n_cliff_(island_noise(config, "cliff")),
      n_skerry_(island_noise(config, "skerry")) {
  cfg = config["world"]["island"];
  seed = island_seed(config);
  R = cfg["radius"].to_number();
  roughness = cfg["roughness"].num(0.5);
  highlands = cfg["highlands"].num(0.45);
  fjords = cfg["fjords"].num(0);
  cliffs = cfg["cliffs"].num(0.35);
  skerries = cfg["skerries"].num(0);
  shelf = cfg["shelf"].num(2200);
  town_rise = cfg["townRise"].num(0);
  population = cfg["population"].num(20000);
  flavor = cfg["flavor"].is_nullish() ? std::string() : cfg["flavor"].to_string();
  theta = hash_float(seed, 901) * kPi;
  const double e = js::max(1.0, cfg["elongation"].num(1.4));
  a = R * std::sqrt(e);
  b = R / std::sqrt(e);
  cos_ = js::cos(theta);
  sin_ = js::sin(theta);
  // the highlands lie towards one side of the island
  const double hd = hash_float(seed, 902) * kPi * 2;
  hdx = js::cos(hd);
  hdy = js::sin(hd);
  // (each count gives two rays: a small island has a handful of fjords, a big one a dozen)
  fjord_count = js::max(2.0, js::round((2.5 + 2.5 * hash_float(seed, 903)) * js::min(1.5, std::sqrt(R / 8000))));
  fjord_phase = hash_float(seed, 904) * kPi * 2;
  ox = 0;
  oy = 0;
  high_threshold = calibrate_highlands();
  site_settlements();
}

double IslandPlan::shape(double lx, double ly) const {
  const double rough = roughness;
  const double s = R * 0.9;
  // big headlands and peninsulas: a domain warp of the ellipse
  const double wu = R * 0.3 * rough * n_warp_u_.fbm2(lx / s + 3.1, ly / s, 3);
  const double wv = R * 0.3 * rough * n_warp_v_.fbm2(lx / s, ly / s - 5.3, 3);
  const double u = lx * cos_ + ly * sin_ + wu;
  const double v = -lx * sin_ + ly * cos_ + wv;
  const double rho = js::hypot(u / a, v / b);
  double d = (1 - rho) * R;
  // far offshore the detail does not matter
  if (d < -R * 0.6) return d;
  // bays and points at a few hundred metres to a few kilometres
  d += R * 0.13 * rough * n_bays_.fbm2(lx / (R * 0.32), ly / (R * 0.32), 3);
  d += 170 * rough * n_points_.fbm2(lx / 650, ly / 650, 3);
  return d;
}

double IslandPlan::high_raw(double lx, double ly) const {
  const double proj = (lx * hdx + ly * hdy) / R;
  return 0.5 + 0.42 * proj + 0.38 * n_high_.fbm2(lx / (R * 0.55), ly / (R * 0.55), 3);
}

double IslandPlan::calibrate_highlands() const {
  const double share = clamp01(highlands);
  std::vector<double> vals;
  const double r = R * 1.4;
  for (int j = -20; j <= 20; ++j)
    for (int i = -20; i <= 20; ++i) {
      const double lx = (i / 20.0) * r;
      const double ly = (j / 20.0) * r;
      if (shape(lx, ly) > 0) vals.push_back(high_raw(lx, ly));
    }
  if (vals.empty() || share <= 0) return js::kInf;
  js::sort(vals, [](double p, double q) { return p - q; });
  const double n = static_cast<double>(vals.size());
  return vals[static_cast<size_t>(js::min(n - 1, std::floor((1 - share) * n)))];
}

double IslandPlan::highland_local(double lx, double ly) const {
  if (high_threshold == js::kInf) return 0;
  return smoothstep(high_threshold - 0.04, high_threshold + 0.2, high_raw(lx, ly));
}

double IslandPlan::fjord_cut(double lx, double ly, double d) const {
  const double F = fjords;
  if (F <= 0 || d < -200) return d;
  const double size = js::min(1.6, R / 8000);
  const double reach = (900 + 3800 * F) * size;
  if (d > reach) return d;
  const double hi = highland_local(lx, ly);
  if (hi < 0.2) return d;
  auto fn = [&](double px, double py) {
    const double w = 1.3 * n_fjord_warp_.fbm2(px / 2200, py / 2200, 3);
    return js::sin(fjord_count * js::atan2(py, px) + fjord_phase + w);
  };
  const double v = fn(lx, ly);
  if (std::fabs(v) > 0.7) return d;
  const double e = 20;
  const double gx = (fn(lx + e, ly) - v) / e;
  const double gy = (fn(lx, ly + e) - v) / e;
  const double dist = std::fabs(v) / js::max(js::hypot(gx, gy), 1.0 / 2500);
  // the fjord narrows from its mouth to its head; faded in over the highland edge
  const double t = clamp01(d / reach);
  const double half = (70 + 200 * F) * std::sqrt(size) * js::pow(1 - t, 0.6) * smoothstep(0.2, 0.55, hi) * (1 - smoothstep(0.5, 0.7, std::fabs(v)));
  if (half <= 0) return d;
  return smin(d, dist - half, 70);
}

double IslandPlan::cliff(double x, double y) const {
  const double lx = x + ox;
  const double ly = y + oy;
  // about `cliffs` of the shore is rocky (the noise's quantile), the rest low
  const double share = cliffs;
  const double n = n_cliff_.fbm2(lx / 1400, ly / 1400, 3) * 0.5 + 0.5;
  const double t = 0.5 + (0.5 - share) * 0.55;
  const double k = smoothstep(t - 0.07, t + 0.07, n);
  return js::max(k, smoothstep(0.3, 0.65, highland_local(lx, ly)));
}

double IslandPlan::skerry(double x, double y, double c) const {
  const double S = skerries;
  if (S <= 0 || c > -30 || c < -1600) return 0;
  const double lx = x + ox;
  const double ly = y + oy;
  const double zone = smoothstep(-1600, -700, c) * (1 - smoothstep(-90, -30, c));
  const double n = n_skerry_.fbm2(lx / 170, ly / 170, 3) + 0.5 * n_skerry_.n2(lx / 45 + 7.7, ly / 45);
  const double thr = 0.78 - 0.32 * S;
  return n > thr ? (n - thr) * 34 * zone : 0;
}

void IslandPlan::site_settlements() {
  const double pop = population;
  // a compact northern town: ~4000 people per km² inside its radius
  // (a small island's town of a few thousand is a few hundred metres across)
  const double town_r = js::max(R < 4000 ? 380.0 : 700.0, 1300 * std::sqrt(pop / 20000));
  // smaller places keep proportionally closer on a small island
  const double gap_k = js::min(1.0, R / 4500);
  const double step = js::max(150.0, R / 60);
  struct Cand {
    double x, y, c, hi, k;
  };
  std::vector<Cand> cands;
  for (double ly = -R * 1.7; ly <= R * 1.7; ly += step)
    for (double lx = -R * 1.7; lx <= R * 1.7; lx += step) {
      const double jx = lx + (hash_float(seed, js::round(lx), js::round(ly), 911) - 0.5) * step * 0.6;
      const double jy = ly + (hash_float(seed, js::round(lx), js::round(ly), 912) - 0.5) * step * 0.6;
      const double c = fjord_cut(jx, jy, shape(jx, jy));
      if (c < 120) continue;
      const double hi = highland_local(jx, jy);
      const double k = hash_float(seed, js::round(jx), js::round(jy), 913);
      cands.push_back({jx, jy, c, hi, k});
    }
  auto land_share = [&](double x, double y, double r) {
    double land = 0;
    double n = 0;
    for (int a = 0; a < 16; ++a)
      for (const double f : {0.35, 0.7, 1.0}) {
        const double t = (a / 16.0) * kPi * 2;
        const double px = x + js::cos(t) * r * f;
        const double py = y + js::sin(t) * r * f;
        n += 1;
        if (fjord_cut(px, py, shape(px, py)) > 0) land += 1;
      }
    return land / n;
  };
  auto high_around = [&](double x, double y, double r) {
    double m = highland_local(x, y);
    for (int a = 0; a < 12; ++a) {
      const double t = (a / 12.0) * kPi * 2;
      m = js::max(m, highland_local(x + js::cos(t) * r, y + js::sin(t) * r));
    }
    return m;
  };
  // main town: on the coast (its centre 300-900 m inland), mostly land around it, low ground
  const Cand* best = nullptr;
  double best_score = 0;
  for (const Cand& p : cands) {
    if (p.c > town_r * 0.75 + 300 || p.c < js::min(250.0, town_r * 0.5)) continue;
    const double hi_a = high_around(p.x, p.y, town_r * 1.6);
    if (hi_a > 0.35) continue;
    const double land = land_share(p.x, p.y, town_r * 1.1);
    if (land < 0.55) continue;
    // a sheltered bay: some sea close by, but most of the ring is land
    const double score = land * 1.2 - hi_a - std::fabs(p.c - town_r * 0.45) / town_r * 0.4 + p.k * 0.15;
    if (!best || score > best_score) best = &p, best_score = score;
  }
  if (!best) {
    // fallback: the lowest-highland land point nearest the shore
    for (const Cand& p : cands) {
      const double score = -p.hi - p.c / R;
      if (!best || score > best_score) best = &p, best_score = score;
    }
  }
  const double town_x = best ? best->x : 0, town_y = best ? best->y : 0;
  ox = town_x;
  oy = town_y;
  sites.push_back({"town", town_x, town_y, town_r, 1});
  // smaller places: small towns and villages spread over the lowlands, hamlets anywhere below the
  // fells; alternate shore and forest sites
  struct Want {
    const char* kind;
    double r0, r1, gap;
  };
  std::vector<Want> want;
  const double towns = cfg["towns"].num(0), villages = cfg["villages"].num(0), hamlets = cfg["hamlets"].num(0);
  for (double k = 0; k < towns; k += 1) want.push_back({"smallTown", 520, 760, 3600 * gap_k});
  for (double k = 0; k < villages; k += 1) want.push_back(gap_k < 1 ? Want{"village", 220, 340, 2600 * gap_k} : Want{"village", 280, 520, 2600 * gap_k});
  for (double k = 0; k < hamlets; k += 1) want.push_back(gap_k < 1 ? Want{"hamlet", 120, 200, 1700 * gap_k} : Want{"hamlet", 150, 260, 1700 * gap_k});
  for (size_t n = 0; n < want.size(); ++n) {
    const Want& w = want[n];
    const bool shore = n % 2 == 0;
    const bool hamlet = std::strcmp(w.kind, "hamlet") == 0;
    const Cand* pick = nullptr;
    double pick_score = 0, pick_rr = 0;
    for (const Cand& p : cands) {
      if (p.hi > (hamlet ? 0.45 : 0.25)) continue;
      const double rr = lerp(w.r0, w.r1, p.k);
      if (p.c < rr * 0.5 + 120) continue;
      double near = js::kInf;
      for (const IslandSite& s : sites) near = js::min(near, js::hypot(p.x - s.lx, p.y - s.ly) - s.radius);
      if (near < w.gap) continue;
      const double coastal = shore ? -std::fabs(p.c - rr * 0.6 - 200) / 800 : -std::fabs(p.c - R * 0.35) / R;
      const double score = coastal - p.hi * 0.8 - js::max(0.0, 1 - near / (w.gap * 2)) * 0.3 + p.k * 0.25;
      if (!pick || score > pick_score) pick = &p, pick_score = score, pick_rr = rr;
    }
    if (pick) sites.push_back({w.kind, pick->x, pick->y, pick_rr, std::strcmp(w.kind, "smallTown") == 0 ? 0.35 : 0.1});
  }
}

const IslandSettlements& IslandPlan::settlements(const MacroFields& fields) const {
  return settlements_.get([&] {
    IslandSettlements out;
    std::vector<std::shared_ptr<Settlement>> towns, villages;
    for (size_t k = 0; k < sites.size(); ++k) {
      const IslandSite& s = sites[k];
      const double n = static_cast<double>(k);
      const double x = (s.lx - ox) / kVoxelSize;
      const double y = (s.ly - oy) / kVoxelSize;
      const double style = hash32(seed, n, 71);
      auto rec = std::make_shared<Settlement>();
      rec->x = x;
      rec->y = y;
      rec->cx = x;
      rec->cy = y;
      rec->radius = s.radius / kVoxelSize;
      rec->style = style;
      rec->flavor = flavor;
      rec->island = true;
      if (s.kind == "town" || s.kind == "smallTown") {
        rec->id = n == 0 ? std::string("S0_0") : js::cat("S", n, "_1000");
        rec->i = n;
        rec->j = n == 0 ? 0 : 1000;
        rec->importance = s.importance;
        towns.push_back(rec);
      } else {
        const bool hamlet = s.kind == "hamlet";
        rec->id = js::cat("V", n, "_1000");
        rec->i = n;
        rec->j = 1000;
        rec->village = true;
        rec->hamlet = hamlet;
        rec->peak = hamlet ? 0.3 : 0.42 + 0.12 * hash_float(seed, n, 72);
        rec->importance = 0.1;
        villages.push_back(rec);
      }
    }
    for (auto* list : {&towns, &villages})
      for (const auto& s : *list) {
        s->t = fields.temperature(s->x, s->y);
        s->m = fields.moisture(s->x, s->y);
      }
    out.towns.assign(towns.begin(), towns.end());
    out.villages.assign(villages.begin(), villages.end());
    return out;
  });
}

const std::optional<Harbour>& IslandPlan::harbour() const {
  return harbour_.get([&] {
    std::optional<Harbour> best;
    for (int a = 0; a < 72; ++a) {
      const double t = (a / 72.0) * kPi * 2;
      const double dx = js::cos(t);
      const double dy = js::sin(t);
      for (double r = 40; r < 4000 && (!best || r < best->r); r += 20) {
        if (coast(dx * r, dy * r) >= 0) continue;
        best = Harbour{r, dx * r, dy * r, dx, dy};
        break;
      }
    }
    return best;
  });
}

const TrunkEdges& IslandPlan::trunk_edges(const World& world) const {
  return trunk_.get([&] {
    TrunkEdges edges;
    const IslandSettlements& st = settlements(*world.fields);
    std::vector<const Settlement*> places;
    for (const auto& s : st.towns) places.push_back(s.get());
    for (const auto& s : st.villages) places.push_back(s.get());
    if (places.size() < 2) return edges;
    const ArterialGrid& A = *world.arterials;
    struct Node {
      double i, j, x, y, c, h;
    };
    std::vector<Node> nodes;
    struct PairHash {
      size_t operator()(const std::pair<double, double>& p) const {
        uint64_t a, b;
        const double x = p.first + 0.0, y = p.second + 0.0;
        std::memcpy(&a, &x, 8);
        std::memcpy(&b, &y, 8);
        return static_cast<size_t>(a * 0x9e3779b97f4a7c15ull ^ (b + 0x632be59bd9b4e019ull + (a << 6) + (a >> 2)));
      }
    };
    std::unordered_map<std::pair<double, double>, int, PairHash> node_cache;
    auto node = [&](double i, double j) -> int {
      const std::pair<double, double> k{i + 0.0, j + 0.0};
      auto it = node_cache.find(k);
      if (it != node_cache.end()) return it->second;
      const double x = A.line(0, i);
      const double y = A.line(1, j);
      const double c = coast(x / 8, y / 8);
      const double h = c > 0 ? world.terrain->sample(x, y).h / 8 : 0;
      nodes.push_back({i, j, x, y, c, h});
      const int n = static_cast<int>(nodes.size()) - 1;
      node_cache.emplace(k, n);
      return n;
    };
    auto edge_cost = [&](const Node& a, const Node& b) {
      if (a.c < 25 || b.c < 25) return js::kInf;
      // the midpoint and quarter points must be on land too
      for (const double t : {0.25, 0.5, 0.75})
        if (coast((a.x + (b.x - a.x) * t) / 8, (a.y + (b.y - a.y) * t) / 8) < 25) return js::kInf;
      const double len = js::hypot(b.x - a.x, b.y - a.y) / 8;
      const double grade = std::fabs(b.h - a.h) / len;
      if (grade > 0.16) return js::kInf;
      return len * (1 + 40 * grade * grade + 2.5 * highland(a.x / 8, a.y / 8));
    };
    auto route = [&](const Settlement& s, const Settlement& t) {
      const CellIJ ca = A.cell_at(s.x, s.y);
      const CellIJ cb = A.cell_at(t.x, t.y);
      const int start = node(ca.i, ca.j);
      const int goal = node(cb.i, cb.j);
      std::unordered_map<int, double> g;
      g[start] = 0;
      std::unordered_map<int, int> prev;
      struct Open {
        int n;
        double f;
      };
      std::vector<Open> open{{start, 0}};
      auto h_est = [&](int n) { return js::hypot(nodes[size_t(n)].x - nodes[size_t(goal)].x, nodes[size_t(n)].y - nodes[size_t(goal)].y) / 8; };
      int iter = 0;
      while (!open.empty() && iter < 6000) {
        iter += 1;
        size_t bi = 0;
        for (size_t k = 1; k < open.size(); ++k)
          if (open[k].f < open[bi].f) bi = k;
        const int n = open[bi].n;
        open.erase(open.begin() + static_cast<std::ptrdiff_t>(bi));
        if (n == goal) break;
        const double gn = g[n];
        static constexpr int kDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : kDirs) {
          const int m = node(nodes[size_t(n)].i + d[0], nodes[size_t(n)].j + d[1]);
          const double cost = edge_cost(nodes[size_t(n)], nodes[size_t(m)]);
          if (cost == js::kInf) continue;
          const double gm = gn + cost;
          auto gi = g.find(m);
          if (gm < (gi == g.end() ? js::kInf : gi->second)) {
            g[m] = gm;
            prev[m] = n;
            open.push_back({m, gm + h_est(m)});
          }
        }
      }
      if (!prev.count(goal)) return;
      for (int n = goal; n != start;) {
        const int p = prev[n];
        const Node& P = nodes[size_t(p)];
        const Node& N = nodes[size_t(n)];
        if (P.i == N.i)
          edges.add(0, N.i, js::min(P.j, N.j));
        else
          edges.add(1, N.j, js::min(P.i, N.i));
        n = p;
      }
    };
    // a tree over the places: each joins the nearest one already linked
    std::vector<const Settlement*> linked{places[0]};
    std::vector<const Settlement*> rest(places.begin() + 1, places.end());
    while (!rest.empty()) {
      const Settlement* bp = nullptr;
      const Settlement* bq = nullptr;
      double bd = 0;
      for (const Settlement* p : rest)
        for (const Settlement* q : linked) {
          const double d = js::hypot(p->x - q->x, p->y - q->y);
          if (!bp || d < bd) bp = p, bq = q, bd = d;
        }
      route(*bq, *bp);
      linked.push_back(bp);
      rest.erase(std::find(rest.begin(), rest.end(), bp));
    }
    return edges;
  });
}

Rect IslandPlan::bounds() const {
  const double r = R * 2.1 + 2000;
  return {-ox - r, -oy - r, -ox + r, -oy + r};
}

}  // namespace svx::city
