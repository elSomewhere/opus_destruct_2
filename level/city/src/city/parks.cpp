// svx_city — voxel_city city/parks.js.
#include "city/parks.hpp"

#include <cmath>

#include "city/space.hpp"
#include "core/hash.hpp"
#include "core/math.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

namespace {

constexpr double kPi = 3.141592653589793;  // Math.PI

double h01(double seed, double a, double b = 0) { return hash32(seed, a, b, 0x9a4c) / 4294967296.0; }

// Smooth value noise (0..1) on cells of `c` voxels, relative coordinates.
double vnoise(double seed, double u, double v, double c, double salt) {
  const double gx = std::floor(u / c);
  const double gy = std::floor(v / c);
  double fx = u / c - gx;
  double fy = v / c - gy;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  const double a = h01(seed, gx * 7 + salt, gy);
  const double b = h01(seed, (gx + 1) * 7 + salt, gy);
  const double d = h01(seed, gx * 7 + salt, gy + 1);
  const double e = h01(seed, (gx + 1) * 7 + salt, gy + 1);
  return (a + (b - a) * fx) * (1 - fy) + (d + (e - d) * fx) * fy;
}

ParkPoint rot(const ParkPond& pond, const ParkPoint& p) {
  const double c = js::cos(pond.rot);
  const double s = js::sin(pond.rot);
  const double du = p.u - pond.u;
  const double dv = p.v - pond.v;
  return {pond.u + du * c - dv * s, pond.v + du * s + dv * c};
}

double pond_radius(const ParkPond& pond, double t) {
  return pond.r * (1 + pond.a[0] * js::sin(t + pond.p[0]) + pond.a[1] * js::sin(2 * t + pond.p[1]) + pond.a[2] * js::sin(3 * t + pond.p[2]));
}

// A grid key as JS's Map holds it (an integer-valued number; a key no segment can have: none).
bool grid_key(double k, int64_t& key) {
  if (!(std::fabs(k) < 9007199254740992.0)) return false;
  key = static_cast<int64_t>(k);
  return true;
}

}  // namespace

ParkLayout plan_park_layout(const std::string& id, const Rect& r) {
  ParkLayout L;
  const double W = r.x1 - r.x0;
  const double H = r.y1 - r.y0;
  const double seed = hash_string(id);
  auto R = [&](double k) { return h01(seed, k); };
  const double m = vx(5);
  // the hub: off-centre, on a path crossing
  const ParkHub hub{W * (0.35 + 0.3 * R(1)), H * (0.35 + 0.3 * R(2)), js::min(vx(7), js::min(W, H) * 0.1)};
  // entrances on each side (a side with a street), 1-2 per side on bigger parks
  std::vector<ParkPoint> ents;
  const char sides[4] = {'N', 'S', 'W', 'E'};
  for (int k = 0; k < 4; ++k) {
    const char s = sides[k];
    const double len = s == 'N' || s == 'S' ? W : H;
    const double n = len > vx(90) ? 2 : len > vx(35) ? 1 : 0;
    for (double q = 0; q < n; q += 1) {
      if (R(10 + k * 3 + q) < 0.25) continue;
      const double t = (q + 0.25 + 0.5 * R(20 + k * 3 + q)) / n;
      const double a = m * 2 + (len - m * 4) * t;
      ents.push_back(s == 'N' ? ParkPoint{a, 0} : s == 'S' ? ParkPoint{a, H} : s == 'W' ? ParkPoint{0, a} : ParkPoint{W, a});
    }
  }
  if (ents.size() < 2) {
    ents.push_back({W / 2, 0});
    ents.push_back({W / 2, H});
  }
  // pond: in bigger parks, off-centre, away from the hub
  std::optional<ParkPond> pond;
  const bool big = js::min(W, H) > vx(60);
  if (big && R(3) < 0.75) {
    const double pr = js::min(W, H) * (0.13 + 0.08 * R(4));
    bool found = false;
    double bu = 0, bv = 0, bd = 0;
    for (int k = 0; k < 8; ++k) {
      const double pu = pr + vx(8) + (W - 2 * pr - vx(16)) * R(40 + k);
      const double pv = pr + vx(8) + (H - 2 * pr - vx(16)) * R(50 + k);
      const double d = js::hypot(pu - hub.u, pv - hub.v);
      if (d > pr + hub.r + vx(8) && (!found || d < bd)) {
        found = true;
        bu = pu;
        bv = pv;
        bd = d;
      }
    }
    if (found) {
      ParkPond p;
      p.u = bu;
      p.v = bv;
      p.r = pr;
      p.a = {0.12 + 0.1 * R(5), 0.08 * R(6), 0.05 * R(7)};
      p.p = {R(8) * 6.28, R(9) * 6.28, R(11) * 6.28};
      p.stretch = 1 + 0.5 * R(12);
      p.rot = R(13) * 3.14;
      pond = p;
    }
  }
  // paths: quadratic curves from each entrance to the hub (bowed sideways), plus a link or two
  std::vector<ParkSeg> segs;
  auto add_curve = [&](const ParkPoint& a, const ParkPoint& b, double bow) {
    const double mx = (a.u + b.u) / 2;
    const double my = (a.v + b.v) / 2;
    const double dx = b.u - a.u;
    const double dy = b.v - a.v;
    const double len = js::or_(js::hypot(dx, dy), 1);
    const ParkPoint c{mx - (dy / len) * bow * len, my + (dx / len) * bow * len};
    const double n = js::max(2, std::ceil(len / vx(2)));
    ParkPoint prev = a;
    for (double k = 1; k <= n; k += 1) {
      const double t = k / n;
      const ParkPoint p{(1 - t) * (1 - t) * a.u + 2 * (1 - t) * t * c.u + t * t * b.u, (1 - t) * (1 - t) * a.v + 2 * (1 - t) * t * c.v + t * t * b.v};
      segs.push_back({prev.u, prev.v, p.u, p.v});
      prev = p;
    }
  };
  for (size_t k = 0; k < ents.size(); ++k) add_curve(ents[k], ParkPoint{hub.u, hub.v}, (R(60 + static_cast<double>(k)) - 0.5) * 0.5);
  const int links = ents.size() > 3 ? 2 : 1;
  for (int k = 0; k < links; ++k) {
    const double n = static_cast<double>(ents.size());
    const size_t ia = static_cast<size_t>(std::floor(R(70 + k) * n));
    const size_t ib = static_cast<size_t>(std::floor(R(75 + k) * n));
    // (a !== b: two entrances are two objects, even at one place)
    if (ia != ib) add_curve(ents[ia], ents[ib], (R(80 + k) - 0.5) * 0.7);
  }
  // a walk round the pond (most of the way)
  if (pond) {
    const double n = 36;
    const double gap = std::floor(R(14) * n);
    bool has_prev = false;
    ParkPoint prev;
    for (double k = 0; k <= n; k += 1) {
      const double t = (k / n) * kPi * 2;
      const double rr = pond_radius(*pond, t) + vx(3.5);
      const ParkPoint p{pond->u + js::cos(t) * rr * pond->stretch, pond->v + js::sin(t) * rr};
      const ParkPoint q = rot(*pond, p);
      if (has_prev && std::fabs(k - gap) > 4) segs.push_back({prev.u, prev.v, q.u, q.v});
      prev = q;
      has_prev = true;
    }
  }
  // drop path pieces that would run through the pond
  if (pond) {
    for (const ParkSeg& s : segs)
      if (pond_dist(*pond, (s[0] + s[2]) / 2, (s[1] + s[3]) / 2) > vx(1.5)) L.segs.push_back(s);
  } else {
    L.segs = std::move(segs);
  }
  // lookup grid of segments (8 m buckets)
  const double G = vx(8);
  for (size_t idx = 0; idx < L.segs.size(); ++idx) {
    const ParkSeg& s = L.segs[idx];
    const double i0 = std::floor((js::min(s[0], s[2]) - vx(2)) / G);
    const double i1 = std::floor((js::max(s[0], s[2]) + vx(2)) / G);
    const double j0 = std::floor((js::min(s[1], s[3]) - vx(2)) / G);
    const double j1 = std::floor((js::max(s[1], s[3]) + vx(2)) / G);
    for (double j = j0; j <= j1; j += 1)
      for (double i = i0; i <= i1; i += 1) {
        int64_t key = 0;
        if (grid_key(i * 4096 + j, key)) L.grid[key].push_back(static_cast<uint32_t>(idx));
      }
  }
  L.W = W;
  L.H = H;
  L.seed = seed;
  L.hub = hub;
  L.ents = std::move(ents);
  L.pond = pond;
  L.G = G;
  L.path_w = vx(1.2) + (big ? vx(0.4) : 0);
  return L;
}

const ParkLayout& park_layout(const OpenSpace& space) {
  return space.park.get([&] { return plan_park_layout(space.id, space.rect); });
}

double pond_dist(const ParkPond& pond, double u, double v) {
  const double c = js::cos(-pond.rot);
  const double s = js::sin(-pond.rot);
  const double du = u - pond.u;
  const double dv = v - pond.v;
  const double x = (du * c - dv * s) / pond.stretch;
  const double y = du * s + dv * c;
  const double t = js::atan2(y, x);
  return js::hypot(x, y) - pond_radius(pond, t);
}

double path_dist(const ParkLayout& L, double u, double v) {
  double best = js::kInf;
  int64_t key = 0;
  if (!grid_key(std::floor(u / L.G) * 4096 + std::floor(v / L.G), key)) return best;
  const auto it = L.grid.find(key);
  if (it == L.grid.end()) return best;
  for (const uint32_t idx : it->second) {
    const ParkSeg& s = L.segs[idx];
    const double dx = s[2] - s[0];
    const double dy = s[3] - s[1];
    const double l2 = js::or_(dx * dx + dy * dy, 1);
    double t = ((u - s[0]) * dx + (v - s[1]) * dy) / l2;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    const double d = js::hypot(u - s[0] - dx * t, v - s[1] - dy * t);
    if (d < best) best = d;
  }
  return best;
}

double grove_at(const ParkLayout& L, double u, double v) { return 0.65 * vnoise(L.seed, u, v, vx(22), 1) + 0.35 * vnoise(L.seed, u, v, vx(9), 2); }

void park_surface(const ParkLayout& L, double u, double v, SpaceSample& out, double hx, double hy) {
  if (L.pond) {
    const double pd = pond_dist(*L.pond, u, v);
    if (pd < 0) {
      out.water = true;
      out.mat = MAT::WATER;
      out.dz = -js::min(8, std::floor(-pd / 5) + 2);
      return;
    }
    if (pd < vx(1.4)) {
      out.mat = pd < vx(0.6) && vnoise(L.seed, u, v, vx(4), 5) < 0.6 ? MAT::REED : (hash32(hx, hy, 0x57) & 3u) == 0 ? MAT::GRAVEL : MAT::MUD;
      return;
    }
  }
  const double hd = js::hypot(u - L.hub.u, v - L.hub.v);
  if (hd < L.hub.r) {
    out.mat = hd < L.hub.r - vx(0.8) ? ((js::to_int32(std::floor(hd / 6)) & 1) == 0 ? MAT::PLAZA_STONE : MAT::PLAZA_STONE_DARK) : MAT::CURB;
    return;
  }
  if (hd < L.hub.r + vx(1.6) && (js::to_int32(std::floor(js::atan2(v - L.hub.v, u - L.hub.u) * 5)) & 1) == 0) {
    out.mat = (hash32(js::sar(hx, 1), js::sar(hy, 1), 0x58) & 3u) == 0 ? MAT::FLOWER_YELLOW : MAT::FLOWER_RED;
    return;
  }
  const double pd = path_dist(L, u, v);
  if (pd < L.path_w) {
    out.mat = pd > L.path_w - 1 ? MAT::GRAVEL : MAT::PARK_PATH;
    return;
  }
  // unmown meadow in the quiet parts, mown lawn near the paths
  const double meadow = vnoise(L.seed, u, v, vx(28), 3);
  if (meadow > 0.62 && pd > vx(4)) {
    out.mat = (hash32(hx, hy, 0x59) & 7u) < 2 ? MAT::GRASS_DRY : MAT::GRASS;
    return;
  }
  out.mat = (hash32(js::sar(hx, 2), js::sar(hy, 2), 0x5a) & 15u) == 0 ? MAT::GRASS : MAT::GRASS_LAWN;
}

void park_surface(const OpenSpace& space, double u, double v, SpaceSample& out, double hx, double hy) { park_surface(park_layout(space), u, v, out, hx, hy); }

}  // namespace svx::city
