// svx_city — nature/lakes.hpp (voxel_city nature/lakes.js).
#include "nature/lakes.hpp"

#include <cmath>
#include <cstring>

#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "terrain/terrain.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"

namespace svx::city {

namespace {

constexpr double kPi = 3.141592653589793;

}  // namespace

namespace {

// murmur3's 64-bit finalizer: every bit of the key reaches the low bits (the cache picks its shard
// by them; integer-valued doubles keep theirs zero)
uint64_t fmix64(uint64_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ull;
  h ^= h >> 33;
  return h;
}

}  // namespace

size_t Lakes::CellHash::operator()(const Cell& c) const {
  uint64_t a, b;
  std::memcpy(&a, &c.a, 8);
  std::memcpy(&b, &c.b, 8);
  return static_cast<size_t>(fmix64(a ^ fmix64(b)));
}

Lakes::Lakes(const World& w, size_t cache_capacity)
    : world(&w),
      enabled(w.config["lakes"]["enabled"].truthy()),
      chance(w.config["lakes"]["chance"].to_number()),
      big_chance(w.config["lakes"]["bigChance"].to_number()),
      scale(w.config["lakes"]["scale"].num(1)),
      town_proximity(w.config["lakes"]["townProximity"].num(0.2)),
      cell(w.config["lakes"]["cell"].num(3500) * 8),
      wrap(wrap_of(w.config)),
      n(wrap.count(w.config["lakes"]["cell"].num(3500))),
      shore(derive_seed(w.seed, "lake.shore")),
      cache_(cache_capacity) {}

std::shared_ptr<const Lake> Lakes::lake(double a, double b) const {
  return cache_.get(Cell{a + 0.0, b + 0.0}, [&] { return build(a, b); });
}

std::shared_ptr<const Lake> Lakes::build(double a, double b) const {
  const World& w = *world;
  const double seed = w.seed;
  // a wrapping world: the canonical lake, moved by whole laps
  const Wrap& W = wrap;
  const double ca = W.canon(a, n);
  const double cb = W.canon(b, n);
  if (ca != a || cb != b) {
    const std::shared_ptr<const Lake> L = lake(ca, cb);
    if (!L) return nullptr;
    const double dx = W.lap(a, n) * W.size_v;
    const double dy = W.lap(b, n) * W.size_v;
    auto copy = std::make_shared<Lake>(*L);
    copy->x = L->x + dx;
    copy->y = L->y + dy;
    copy->port = L->port;
    return copy;
  }
  const double CELL = cell;
  if (!enabled || hash_float(seed, a, b, 601) > chance) return nullptr;
  double x = js::round((a + 0.2 + 0.6 * hash_float(seed, a, b, 602)) * CELL);
  double y = js::round((b + 0.2 + 0.6 * hash_float(seed, a, b, 603)) * CELL);
  // raw terrain: harbour grading depends on the lakes, not the other way round
  TerrainSample ts = w.terrain->sample(x, y, nullptr, true);
  // now and then a big lake (kilometres across) in the lowlands
  const bool big = ts.mountain < 0.2 && hash_float(seed, a, b, 606) < big_chance;
  // (lakes.scale shrinks the ordinary lakes: tarns on a small island)
  const double r0 =
      (big ? 1400 + 2600 * hash_float(seed, a, b, 604) : (ts.mountain > 0.3 ? 90 + 260 * hash_float(seed, a, b, 604) : 150 + 650 * hash_float(seed, a, b, 604)) * scale) *
      8;
  // half the big lakes lie right against a town: its shore becomes a harbour front
  std::string port;
  if (big && hash_float(seed, a, b, 607) < 0.5) {
    const Settlement* best = nullptr;
    double best_d = 0;
    for (const Settlement* s : w.fields->nearest_settlements(x, y)) {
      const double d = js::hypot(s->x - x, s->y - y);
      if (!best || d < best_d) {
        best = s;
        best_d = d;
      }
    }
    if (best && best_d < r0 + best->radius + 12000 * 8) {
      const Settlement& s = *best;
      const double dx = (x - s.x) / js::or_(best_d, 1);
      const double dy = (y - s.y) / js::or_(best_d, 1);
      const double dist = s.radius * 0.92 + r0;
      x = js::round(s.x + dx * dist);
      y = js::round(s.y + dy * dist);
      ts = w.terrain->sample(x, y, nullptr, true);
      port = s.id;
    }
  }
  if (ts.u > 0.03 || (port.empty() && w.fields->settlement_proximity(x, y) > town_proximity)) return nullptr;
  // island: lakes lie well inside the shore
  if (w.fields->island && w.fields->coast_distance(x, y) < (r0 / 8) * 1.7 + scale * 120) return nullptr;
  const double mountain = ts.mountain;
  if (big && mountain > 0.2) return nullptr;
  // water level: below the lowest point of the rim
  double lo = ts.h;
  const int nr = big ? 40 : 16;
  for (int k = 0; k < nr; ++k) {
    const double t = (static_cast<double>(k) / nr) * kPi * 2;
    lo = js::min(lo, w.terrain->sample(x + js::cos(t) * r0 * 1.2, y + js::sin(t) * r0 * 1.2, nullptr, true).h);
  }
  const double level = std::floor(lo - (r0 < 800 ? 8 : 12));
  if (level < w.config["world"]["seaLevel"].to_number() * 8 + 8) return nullptr;
  const double depth =
      js::round((js::min(3.0, 1 + r0 / 400) + 12 * js::min(1.0, r0 / (700 * 8)) + (big ? 25 * js::min(1.0, r0 / (4000 * 8)) : 0)) * 8);
  // cx, cy: canonical centre (the shore noise follows it, so every lap has the same shore)
  auto L = std::make_shared<Lake>();
  L->id = js::cat("L", a, "_", b);
  L->x = x;
  L->y = y;
  L->cx = x;
  L->cy = y;
  L->r0 = r0;
  L->level = level;
  L->depth = depth;
  L->big = big;
  L->port = port;
  L->reach = r0 * 1.55;
  return L;
}

std::shared_ptr<const Lake> Lakes::port_lake_of(const Settlement& s) const {
  return s.port_lake.get([&]() -> std::shared_ptr<const Lake> {
    const double R = s.radius + 12000 * 8 + 4000 * 8;
    for (const std::shared_ptr<const Lake>& L : near({s.x - R, s.y - R, s.x + R, s.y + R}))
      if (L->port == s.id) return L;
    return nullptr;
  });
}

std::vector<std::shared_ptr<const Lake>> Lakes::near(const Rect& r) const {
  std::vector<std::shared_ptr<const Lake>> out;
  const double CELL = cell;
  for (double b = std::floor(r.y0 / CELL) - 2; b <= std::floor(r.y1 / CELL) + 2; b += 1)
    for (double a = std::floor(r.x0 / CELL) - 2; a <= std::floor(r.x1 / CELL) + 2; a += 1) {
      std::shared_ptr<const Lake> L = lake(a, b);
      if (!L) continue;
      if (L->x + L->reach < r.x0 || L->x - L->reach > r.x1 || L->y + L->reach < r.y0 || L->y - L->reach > r.y1) continue;
      out.push_back(std::move(L));
    }
  return out;
}

double Lakes::shore_k(const Lake& L, double x, double y, bool smooth) const {
  const double dx = x - L.x;
  const double dy = y - L.y;
  const double ang = js::atan2(dy, dx);
  const double nn = shore.fbm2(js::cos(ang) * 1.3 + L.cx * 1e-5, js::sin(ang) * 1.3 + L.cy * 1e-5, 3);
  // (shore wiggles in the lake's canonical frame)
  const double lx = x - L.x + L.cx;
  const double ly = y - L.y + L.cy;
  // small bays and points: at most ~40 m on any lake; big lakes also get bays that scale with
  // their size (a fixed-scale wiggle proportional to the radius would make kilometre-sized lake
  // shores jagged)
  const double n2 = smooth ? 0 : shore.n2(lx / 700, ly / 700);
  const double bays = L.r0 > 8000 ? 0.08 * L.r0 * shore.n2(lx / (L.r0 * 0.7) + 3.3, ly / (L.r0 * 0.7)) : 0;
  return js::hypot(dx, dy) / (L.r0 * (1 + 0.35 * nn) + js::min(0.12 * L.r0, 320.0) * n2 + bays);
}

std::optional<LakeInfo> Lakes::at(double x, double y) const {
  if (!enabled) return std::nullopt;
  const double a = std::floor(x / cell);
  const double b = std::floor(y / cell);
  for (double j = b - 2; j <= b + 2; j += 1)
    for (double i = a - 2; i <= a + 2; i += 1) {
      std::shared_ptr<const Lake> L = lake(i, j);
      if (!L || std::fabs(x - L->x) > L->reach || std::fabs(y - L->y) > L->reach) continue;
      const double k = shore_k(*L, x, y);
      if (k > 1.35) continue;
      LakeInfo o;
      o.k = k;
      o.level = L->level;
      o.bed = k < 1 ? js::round(L->level - L->depth * (1 - k * k) - 4) : L->level + js::round((k - 1) * 90);
      o.lake = std::move(L);
      return o;
    }
  return std::nullopt;
}

std::optional<LakeShore> Lakes::shore_near(double x, double y, double max_dist) const {
  if (!enabled) return std::nullopt;
  std::optional<LakeShore> best;
  for (const std::shared_ptr<const Lake>& L : near({x - max_dist, y - max_dist, x + max_dist, y + max_dist})) {
    if (!L->big) continue;
    const double k = shore_k(*L, x, y);
    const double dist = (k - 1) * L->r0;
    if (dist < -max_dist || dist > max_dist) continue;
    if (!best || std::fabs(dist) < std::fabs(best->dist)) {
      const double d = js::or_(js::hypot(x - L->x, y - L->y), 1);
      best = LakeShore{L, dist, (x - L->x) / d, (y - L->y) / d};
    }
  }
  return best;
}

double Lakes::ground_at(const LakeInfo& info, double h) const {
  if (info.k < 1) return js::min(h, info.bed);
  // shore: the land eases down to the water
  const double t = smoothstep(1, 1.35, info.k);
  return js::min(h, js::round(info.level + 2 + t * (h - info.level - 2)));
}

bool Lakes::hits_rect(const Rect& r, double margin_m) const {
  if (!enabled) return false;
  const double pad = margin_m * 8;
  for (const std::shared_ptr<const Lake>& L : near({r.x0 - pad, r.y0 - pad, r.x1 + pad, r.y1 + pad})) {
    const double cx = js::max(r.x0, js::min(r.x1, L->x));
    const double cy = js::max(r.y0, js::min(r.y1, L->y));
    if (js::hypot(cx - L->x, cy - L->y) < L->r0 * 1.5 + pad) {
      // sample the rect edges and interior for a real overlap
      const double step = 64;
      for (double y = r.y0; y <= r.y1; y += step)
        for (double x = r.x0; x <= r.x1; x += step)
          if (shore_k(*L, x, y) < 1.1 + pad / L->r0) return true;
    }
  }
  return false;
}

}  // namespace svx::city
