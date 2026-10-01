// svx_city — world/fields.hpp (voxel_city world/fields.js).
#include "world/fields.hpp"

#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "world/island.hpp"

namespace svx::city {

namespace {

constexpr double kPi = 3.141592653589793;  // Math.PI

// A settlement record of a wrapping world seen dx, dy voxels (whole laps) away from its canonical
// cell: the same record, moved; its lazily cached fields start over (Lazy copies empty).
std::shared_ptr<const Settlement> lap_copy(const Settlement& c, double dx, double dy) {
  auto copy = std::make_shared<Settlement>(c);
  copy->x = c.x + dx;
  copy->y = c.y + dy;
  return copy;
}

}  // namespace

std::string Settlement::i_js() const { return village ? js::cat("v", i) : js::num(i); }

// The configuration MacroFields reads (once).
struct MacroFields::Cfg {
  double seed = 0;
  bool island = false, infinite_city = false;
  double settlement_cell = 0, village_cell = 0, settlement_chance = 0, village_chance = 0;
  double city_r0 = 0, city_r1 = 0, spawn_city_radius = 0, village_r0 = 0, village_r1 = 0, face_margin = 0;
  double climate_scale = 0;
  bool has_temperature = false, has_moisture = false;
  double temperature = 0, temperature_var = 0, moisture = 0, moisture_var = 0;
  double belt_scale = 0, belt_warp = 0, belt0 = 0, belt1 = 0, massif_scale = 0, massif0 = 0, massif1 = 0;
  double spawn_near = 10, spawn_far = 35;
};

// The settlement and village caches (JS: one Map that never forgets): (i, j) -> record or null.
struct MacroFields::Cache {
  struct Key {
    double i, j;
    bool operator==(const Key& o) const { return i == o.i && j == o.j; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      uint64_t a, b;
      std::memcpy(&a, &k.i, 8);
      std::memcpy(&b, &k.j, 8);
      uint64_t h = a * 0x9e3779b97f4a7c15ull;
      h ^= b + 0x632be59bd9b4e019ull + (h << 6) + (h >> 2);
      return static_cast<size_t>(h ^ (h >> 29));
    }
  };
  struct Shard {
    mutable std::shared_mutex m;
    std::unordered_map<Key, std::shared_ptr<const Settlement>, KeyHash> map;
  };
  static constexpr size_t kShards = 16;
  Shard towns[kShards];
  Shard villages[kShards];

  // The record of (i, j), made by make() the first time (outside any lock; the first stored wins).
  template <class F>
  const Settlement* get(Shard* shards, double i, double j, F&& make) {
    const Key k{i + 0.0, j + 0.0};  // (JS's key `${i},${j}`: -0 prints as 0)
    Shard& sh = shards[KeyHash{}(k) % kShards];
    {
      std::shared_lock<std::shared_mutex> lk(sh.m);
      auto it = sh.map.find(k);
      if (it != sh.map.end()) return it->second.get();
    }
    std::shared_ptr<const Settlement> made = make();
    std::unique_lock<std::shared_mutex> lk(sh.m);
    auto it = sh.map.emplace(k, std::move(made)).first;
    return it->second.get();
  }
};

MacroFields::MacroFields(const Value& config, std::shared_ptr<const Chart> chart)
    : config_(config),
      chart_(std::move(chart)),
      cache_(std::make_unique<Cache>()),
      n_warp_(derive_seed(config["seed"].to_number(), "field.warp")),
      n_urban_(derive_seed(config["seed"].to_number(), "field.urban")),
      n_moist_(derive_seed(config["seed"].to_number(), "field.moisture")),
      n_temp_(derive_seed(config["seed"].to_number(), "field.temperature")),
      n_district_(derive_seed(config["seed"].to_number(), "field.district")),
      n_industry_(derive_seed(config["seed"].to_number(), "field.industry")),
      n_style_(derive_seed(config["seed"].to_number(), "field.style")),
      n_belt_(derive_seed(config["seed"].to_number(), "field.mountainBelt")),
      n_massif_(derive_seed(config["seed"].to_number(), "field.mountainMassif")),
      n_belt_warp_(derive_seed(config["seed"].to_number(), "field.mountainWarp")),
      n_lobe_(derive_seed(config["seed"].to_number(), "field.lobes")),
      n_fringe_(derive_seed(config["seed"].to_number(), "field.fringe")) {
  const Value& w = config["world"];
  const Value& t = config["terrain"];
  auto c = std::make_unique<Cfg>();
  c->seed = config["seed"].to_number();
  c->island = w["mode"].is_string() && w["mode"].str() == "island";
  c->infinite_city = w["mode"].is_string() && w["mode"].str() == "infiniteCity";
  c->settlement_cell = w["settlementCell"].to_number();
  c->village_cell = w["villageCell"].to_number();
  c->settlement_chance = w["settlementChance"].to_number();
  c->village_chance = w["villageChance"].to_number();
  c->city_r0 = w["cityRadius"][size_t(0)].to_number();
  c->city_r1 = w["cityRadius"][size_t(1)].to_number();
  c->spawn_city_radius = w["spawnCityRadius"].to_number();
  c->village_r0 = w["villageRadius"][size_t(0)].to_number();
  c->village_r1 = w["villageRadius"][size_t(1)].to_number();
  c->face_margin = w["faceMargin"].to_number();
  c->climate_scale = w["climateScale"].to_number();
  const Value& cl = w["climate"];
  if (!cl.is_nullish()) {
    c->has_temperature = !cl["temperature"].is_undefined();
    c->temperature = cl["temperature"].to_number();
    c->temperature_var = cl["temperatureVar"].num(0.05);
    c->has_moisture = !cl["moisture"].is_undefined();
    c->moisture = cl["moisture"].to_number();
    c->moisture_var = cl["moistureVar"].num(0.08);
  }
  c->belt_scale = t["mountainBeltScale"].to_number();
  c->belt_warp = t["mountainBeltWarp"].to_number();
  c->belt0 = t["mountainBelt"][size_t(0)].to_number();
  c->belt1 = t["mountainBelt"][size_t(1)].to_number();
  c->massif_scale = t["mountainMassifScale"].to_number();
  c->massif0 = t["mountainMassif"][size_t(0)].to_number();
  c->massif1 = t["mountainMassif"][size_t(1)].to_number();
  if (!t["spawnMountains"].is_nullish()) {
    c->spawn_near = t["spawnMountains"][size_t(0)].to_number();
    c->spawn_far = t["spawnMountains"][size_t(1)].to_number();
  }
  cfg_ = std::move(c);
  // a wrapping world: settlement and village lattices repeat round it
  wrap = Wrap(config);
  n_town = wrap.count(cfg_->settlement_cell);
  n_village = wrap.count(cfg_->village_cell);
  m_off = {0, 0};
  if (cfg_->island) island = std::make_unique<IslandPlan>(config);
  if (!island) m_off = pick_mountain_offset();
}

MacroFields::~MacroFields() = default;

const IslandSettlements& MacroFields::island_settlements() const { return island->settlements(*this); }

double MacroFields::coast_distance(double x, double y) const {
  return island ? island->coast(x * kVoxelSize, y * kVoxelSize) : js::kInf;
}

std::vector<const Settlement*> MacroFields::settlements_in(const Rect& r) const {
  std::vector<const Settlement*> out;
  if (island) {
    for (const auto& s : island_settlements().towns)
      if (s->x + s->radius * 2.6 >= r.x0 && s->x - s->radius * 2.6 <= r.x1 && s->y + s->radius * 2.6 >= r.y0 && s->y - s->radius * 2.6 <= r.y1)
        out.push_back(s.get());
    return out;
  }
  const double cell = cfg_->settlement_cell / kVoxelSize;
  for (double cj = std::floor(r.y0 / cell) - 1; cj <= std::ceil(r.y1 / cell) + 1; cj += 1)
    for (double ci = std::floor(r.x0 / cell) - 1; ci <= std::ceil(r.x1 / cell) + 1; ci += 1)
      if (const Settlement* s = settlement(ci, cj)) out.push_back(s);
  return out;
}

std::vector<const Settlement*> MacroFields::villages_in(const Rect& r) const {
  std::vector<const Settlement*> out;
  if (island) {
    for (const auto& v : island_settlements().villages)
      if (v->x + v->radius * 2.2 >= r.x0 && v->x - v->radius * 2.2 <= r.x1 && v->y + v->radius * 2.2 >= r.y0 && v->y - v->radius * 2.2 <= r.y1)
        out.push_back(v.get());
    return out;
  }
  const double cell = cfg_->village_cell / kVoxelSize;
  for (double cj = std::floor(r.y0 / cell) - 1; cj <= std::ceil(r.y1 / cell) + 1; cj += 1)
    for (double ci = std::floor(r.x0 / cell) - 1; ci <= std::ceil(r.x1 / cell) + 1; ci += 1)
      if (const Settlement* v = village(ci, cj)) out.push_back(v);
  return out;
}

std::array<double, 2> MacroFields::pick_mountain_offset() {
  const double near = cfg_->spawn_near, far = cfg_->spawn_far;
  const double bs = cfg_->belt_scale;
  const double seed = cfg_->seed;
  std::optional<std::array<double, 2>> fallback;
  for (int k = 0; k < 96; ++k) {
    m_off = {(hash_float(seed, k, 41) - 0.5) * 4 * bs, (hash_float(seed, k, 42) - 0.5) * 4 * bs};
    if (mountainness(0, 0) > 0.02) continue;
    // distance (km) to the nearest high ground along 12 rays
    double d = js::kInf;
    for (int a = 0; a < 12; ++a) {
      const double ang = (a / 12.0) * kPi * 2;
      for (double r = 4; r <= far; r += 2) {
        if (mountainness((js::cos(ang) * r * 1000) / kVoxelSize, (js::sin(ang) * r * 1000) / kVoxelSize) > 0.6) {
          d = js::min(d, r);
          break;
        }
      }
    }
    if (d >= near && d <= far) return m_off;
    if (!fallback) fallback = m_off;
  }
  return fallback ? *fallback : std::array<double, 2>{0, 0};
}

const Settlement* MacroFields::settlement(double i, double j) const {
  if (island) {
    for (const auto& t : island_settlements().towns)
      if (t->i == i && t->j == j) return t.get();
    return nullptr;
  }
  return cache_->get(cache_->towns, i, j, [&] { return make_settlement(i, j); });
}

std::shared_ptr<const Settlement> MacroFields::make_settlement(double i, double j) const {
  // a wrapping world: the canonical town, moved by whole laps
  const double ci = wrap.canon(i, n_town);
  const double cj = wrap.canon(j, n_town);
  if (ci != i || cj != j) {
    const Settlement* c = settlement(ci, cj);
    return c ? lap_copy(*c, wrap.lap(i, n_town) * wrap.size_v, wrap.lap(j, n_town) * wrap.size_v) : nullptr;
  }
  const Cfg& w = *cfg_;
  const double seed = w.seed;
  const double cell = w.settlement_cell;
  const bool spawn = i == 0 && j == 0;
  // on a planet face, settlements (and with them roads, highways and sites) keep a wilderness band
  // along the face edges, so neighbouring faces only have to agree on terrain, climate, biomes and
  // rivers
  const double edge = chart_->edge_distance(i * cell, j * cell);
  const double jx = spawn ? 0 : (hash_float(seed, i, j, 12) - 0.5) * 0.4 * cell;
  const double jy = spawn ? 0 : (hash_float(seed, i, j, 13) - 0.5) * 0.4 * cell;
  const double radius = spawn ? w.spawn_city_radius : lerp(w.city_r0, w.city_r1, hash_float(seed, i, j, 14));
  if (!spawn && (hash_float(seed, i, j, 11) > w.settlement_chance || edge < w.face_margin + w.city_r1 ||
                 mountains_around(i * cell + jx, j * cell + jy, radius) > 0.12))
    return nullptr;
  auto s = std::make_shared<Settlement>();
  s->id = js::cat("S", i, "_", j);
  s->i = i;
  s->j = j;
  s->x = (i * cell + jx) / kVoxelSize;
  s->y = (j * cell + jy) / kVoxelSize;
  s->radius = radius / kVoxelSize;
  s->importance = spawn ? 1 : 0.55 + 0.45 * hash_float(seed, i, j, 15);
  s->style = hash32(seed, i, j, 16);
  // canonical position (wrapping worlds hash it; equal to x, y elsewhere)
  s->cx = s->x;
  s->cy = s->y;
  // regional climate at the centre, for climate-bound city flavors
  s->t = temperature(s->x, s->y);
  s->m = moisture(s->x, s->y);
  return s;
}

double MacroFields::mountains_around(double mx, double my, double radius) const {
  double m = mountainness(mx / kVoxelSize, my / kVoxelSize);
  const double R = radius * 2.6 + 1000;
  for (int k = 0; k < 10 && m <= 0.12; ++k) {
    const double a = (k / 10.0) * kPi * 2;
    m = js::max(m, mountainness((mx + js::cos(a) * R) / kVoxelSize, (my + js::sin(a) * R) / kVoxelSize));
  }
  return m;
}

const Settlement* MacroFields::village(double i, double j) const {
  if (island) {
    for (const auto& v : island_settlements().villages)
      if (v->i == i && v->j == j) return v.get();
    return nullptr;
  }
  return cache_->get(cache_->villages, i, j, [&] { return make_village(i, j); });
}

std::shared_ptr<const Settlement> MacroFields::make_village(double i, double j) const {
  const double ci = wrap.canon(i, n_village);
  const double cj = wrap.canon(j, n_village);
  if (ci != i || cj != j) {
    const Settlement* c = village(ci, cj);
    return c ? lap_copy(*c, wrap.lap(i, n_village) * wrap.size_v, wrap.lap(j, n_village) * wrap.size_v) : nullptr;
  }
  const Cfg& w = *cfg_;
  const double seed = w.seed;
  const double cell = w.village_cell;
  if (!(hash_float(seed, i, j, 51) < w.village_chance)) return nullptr;
  const double mx = (i + 0.5 + (hash_float(seed, i, j, 52) - 0.5) * 0.7) * cell;
  const double my = (j + 0.5 + (hash_float(seed, i, j, 53) - 0.5) * 0.7) * cell;
  const double k = hash_float(seed, i, j, 54);
  const double radius = lerp(w.village_r0, w.village_r1, k * k);
  const double x = mx / kVoxelSize;
  const double y = my / kVoxelSize;
  bool ok = chart_->edge_distance(mx, my) > w.face_margin + radius && mountains_around(mx, my, radius) <= 0.15;
  // keep clear of the towns (and their suburbs)
  if (ok)
    for (const Settlement* s : nearest_settlements(x, y))
      if (js::hypot(x - s->x, y - s->y) < s->radius * 1.7 + (radius * 2) / kVoxelSize) {
        ok = false;
        break;
      }
  if (!ok) return nullptr;
  auto v = std::make_shared<Settlement>();
  v->id = js::cat("V", i, "_", j);
  v->i = i;
  v->j = j;
  v->village = true;
  v->hamlet = radius < 330;
  v->x = x;
  v->y = y;
  v->radius = radius / kVoxelSize;
  v->peak = 0.3 + 0.32 * k;
  v->importance = 0.1;
  v->style = hash32(seed, i, j, 55);
  v->cx = x;
  v->cy = y;
  v->t = temperature(x, y);
  v->m = moisture(x, y);
  return v;
}

std::vector<const Settlement*> MacroFields::nearest_villages(double x, double y) const {
  std::vector<const Settlement*> out;
  if (island) {
    for (const auto& v : island_settlements().villages) out.push_back(v.get());
    return out;
  }
  const double cell_v = cfg_->village_cell / kVoxelSize;
  const double ci = std::floor(x / cell_v);
  const double cj = std::floor(y / cell_v);
  for (int dj = -1; dj <= 1; ++dj)
    for (int di = -1; di <= 1; ++di)
      if (const Settlement* v = village(ci + di, cj + dj)) out.push_back(v);
  return out;
}

std::vector<const Settlement*> MacroFields::nearest_settlements(double x, double y) const {
  std::vector<const Settlement*> out;
  if (island) {
    for (const auto& s : island_settlements().towns) out.push_back(s.get());
    return out;
  }
  const double cell_v = cfg_->settlement_cell / kVoxelSize;
  const double ci = js::round(x / cell_v);
  const double cj = js::round(y / cell_v);
  for (int dj = -1; dj <= 1; ++dj)
    for (int di = -1; di <= 1; ++di)
      if (const Settlement* s = settlement(ci + di, cj + dj)) out.push_back(s);
  return out;
}

double MacroFields::settlement_warp(double x, double y) const {
  const FieldPoint f = chart_->to_field(x * kVoxelSize, y * kVoxelSize);
  return 1 + 0.28 * n_warp_.fbmP(f.x / 1800, f.y / 1800, f.z / 1800, f.w / 1800, 3);
}

double MacroFields::settlement_distance(const Settlement& s, double x, double y) const {
  return settlement_distance(s, x, y, settlement_warp(x, y));
}

double MacroFields::settlement_distance(const Settlement& s, double x, double y, double warp) const {
  const double d = js::hypot(x - s.x, y - s.y) / s.radius;
  // (the lobes fade out far from the place: no noise to evaluate out there)
  const double far = smoothstep(2.2, 3.2, d);
  if (far >= 1) return d * warp;
  const FieldPoint f = chart_->to_field(x * kVoxelSize, y * kVoxelSize);
  const double sc = js::max(220.0, s.radius * kVoxelSize * 0.6);
  const double o = std::fmod(s.style, 997) * 0.37;
  const double lobes = 1 + 0.3 * n_lobe_.fbmP(f.x / sc + o, f.y / sc - o, f.z / sc, f.w / sc, 2) * js::min(1.0, d * 2) * (1 - far);
  return d * warp * lobes;
}

double MacroFields::fringe_noise(double x, double y) const {
  const FieldPoint f = chart_->to_field(x * kVoxelSize, y * kVoxelSize);
  return n_fringe_.fbmP(f.x / 240, f.y / 240, f.z / 240, f.w / 240, 2);
}

Urban MacroFields::urban(double x, double y) const {
  const FieldPoint f = chart_->to_field(x * kVoxelSize, y * kVoxelSize);
  const double wobble = 0.12 * n_urban_.fbmP(f.x / 900, f.y / 900, f.z / 900, f.w / 900, 3);
  Urban out;
  const Settlement* best = nullptr;
  double u = 0, core = 0, prox = 0;
  const std::vector<const Settlement*> near = nearest_settlements(x, y);
  const double warp = !near.empty() ? settlement_warp(x, y) : 1;
  for (const Settlement* s : near) {
    const double d = settlement_distance(*s, x, y, warp);
    prox = js::max(prox, 1 - smoothstep(0.95, 2.6, d));
    // (a wide rim: the town thins out gradually into its outskirts)
    const double su = 1 - smoothstep(0.34, 1.06, d + wobble);
    const double sc = (1 - smoothstep(0.0, 0.34, d)) * s->importance;
    if (su > 0) out.parts.push_back({s, su});
    if (su > u) {
      u = su;
      best = s;
    }
    if (sc > core) core = sc;
  }
  // villages and hamlets: low urbanization peaks, no core
  if (!cfg_->infinite_city && u < 0.5) {
    for (const Settlement* v : nearest_villages(x, y)) {
      const double d = settlement_distance(*v, x, y, warp);
      if (d > 2.6) continue;
      prox = js::max(prox, 1 - smoothstep(0.95, 2.2, d));
      const double vu = v->peak * (1 - smoothstep(0.3, 1.02, d + wobble * 0.6));
      if (vu > 0) out.parts.push_back({v, vu});
      if (vu > u) {
        u = vu;
        best = v;
      }
    }
  }
  if (cfg_->infinite_city) u = js::max(u, 0.72 + 0.28 * n_urban_.fbmP(f.x / 2500, f.y / 2500, f.z / 2500, f.w / 2500, 2));
  out.u = clamp01(u);
  out.core = clamp01(core);
  out.settlement = best;
  out.prox = prox;
  return out;
}

double MacroFields::settlement_proximity(double x, double y) const {
  const std::vector<const Settlement*> near = nearest_settlements(x, y);
  const std::vector<const Settlement*> vil = cfg_->infinite_city ? std::vector<const Settlement*>{} : nearest_villages(x, y);
  if (near.empty() && vil.empty()) return 0;
  const double warp = settlement_warp(x, y);
  double p = 0;
  for (const Settlement* s : near) p = js::max(p, 1 - smoothstep(0.95, 2.6, settlement_distance(*s, x, y, warp)));
  for (const Settlement* v : vil) p = js::max(p, 1 - smoothstep(0.95, 2.2, settlement_distance(*v, x, y, warp)));
  return p;
}

double MacroFields::mountainness(double x, double y) const {
  if (island) {
    const double xm = x * kVoxelSize;
    const double ym = y * kVoxelSize;
    const double h = island->highland(xm, ym);
    return h > 0 ? h * smoothstep(-200, 0, island->coast(xm, ym)) : 0;
  }
  const FieldPoint p = chart_->to_field(x * kVoxelSize, y * kVoxelSize);
  const double fx = p.x + m_off[0];
  const double fy = p.y + m_off[1];
  const double fz = p.z, fw = p.w;
  const Cfg& t = *cfg_;
  const double bs = t.belt_scale;
  const double wx = t.belt_warp * bs * n_belt_warp_.fbmP(fx / (bs * 0.8), fy / (bs * 0.8), fz / (bs * 0.8), fw / (bs * 0.8), 2);
  const double belt = 1 - std::fabs(n_belt_.fbmP((fx + wx) / bs, (fy - wx) / bs, fz / bs, fw / bs, 2));
  const double m1 = smoothstep(t.belt0, t.belt1, belt);
  const double ms = t.massif_scale;
  const double m2 = 0.85 * smoothstep(t.massif0, t.massif1, n_massif_.fbmP(fx / ms, fy / ms, fz / ms, fw / ms, 3));
  return js::max(m1, m2);
}

double MacroFields::moisture(double x, double y) const {
  const FieldPoint f = chart_->to_field(x * kVoxelSize, y * kVoxelSize);
  const double c = cfg_->climate_scale;
  const double n = n_moist_.fbmP(f.x / (c * 0.55), f.y / (c * 0.55), f.z / (c * 0.55), f.w / (c * 0.55), 3);
  if (cfg_->has_moisture) return cfg_->moisture + cfg_->moisture_var * n;
  return 0.5 + 0.62 * n;
}

double MacroFields::temperature(double x, double y) const {
  const FieldPoint f = chart_->to_field(x * kVoxelSize, y * kVoxelSize);
  const double c = cfg_->climate_scale;
  if (cfg_->has_temperature) return cfg_->temperature + cfg_->temperature_var * n_temp_.fbmP(f.x / c, f.y / c, f.z / c, f.w / c, 3);
  double t = 0.5 + 0.6 * n_temp_.fbmP(f.x / c, f.y / c, f.z / c, f.w / c, 3);
  const std::optional<double> lat = chart_->latitude(f.x, f.y, f.z, f.w);
  if (lat) t = 0.25 * t + 0.95 - 0.9 * std::fabs(*lat);
  return t;
}

double MacroFields::district_noise(double x, double y) const {
  const FieldPoint f = chart_->to_field(x * kVoxelSize, y * kVoxelSize);
  return n_district_.fbmP(f.x / 1100, f.y / 1100, f.z / 1100, f.w / 1100, 2);
}

double MacroFields::industry_noise(double x, double y) const {
  const FieldPoint f = chart_->to_field(x * kVoxelSize, y * kVoxelSize);
  double q = 0;
  for (const Settlement* s : nearest_settlements(x, y)) {
    const double d = js::hypot(x - s->x, y - s->y) / s->radius;
    if (d < 0.3 || d > 1.2) continue;
    // (JS hashes the record's i: a town's is a number)
    const double th = hash_float(cfg_->seed, s->village ? js::kNaN : s->i, s->j, 17) * kPi * 2;
    const double c = js::cos(js::atan2(y - s->y, x - s->x) - th);
    const double ang = smoothstep(0.3, 0.65, c);
    const double rad = smoothstep(0.34, 0.5, d) * (1 - smoothstep(0.98, 1.15, d));
    q = js::max(q, ang * rad * (0.45 + 0.35 * smoothstep(0.55, 0.85, d)));
  }
  // (an island town keeps its industry to its quarter: a few plants, not a belt)
  return n_industry_.fbmP(f.x / 1600, f.y / 1600, f.z / 1600, f.w / 1600, 2) * (island ? 0.35 : 1) + q;
}

double MacroFields::style_noise(double x, double y) const {
  const FieldPoint f = chart_->to_field(x * kVoxelSize, y * kVoxelSize);
  return n_style_.fbmP(f.x / 900, f.y / 900, f.z / 900, f.w / 900, 2);
}

}  // namespace svx::city
