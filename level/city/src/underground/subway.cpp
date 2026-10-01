// svx_city — underground/subway.hpp (voxel_city underground/subway.js).
#include "underground/subway.hpp"

#include <cmath>
#include <utility>

#include "core/js.hpp"
#include "core/math.hpp"
#include "nature/rivers.hpp"
#include "network/arterials.hpp"
#include "svx/base/types.hpp"
#include "voxel/chunk.hpp"
#include "voxel/materials.hpp"
#include "world/World.hpp"
#include "world/caches.hpp"
#include "world/fields.hpp"

namespace svx::city {

namespace {

constexpr double HALF_LEN = 384;  // 48 m
constexpr double HALL_W = 64;     // half inner width (8 m)
constexpr double TUN_W = 36;      // tunnel half inner width
constexpr double HEAD = 20;       // stair headroom
constexpr double TRACK_TUN = 18;  // track centre offset in the running tunnel
constexpr double TRACK_ST = 46;   // track centre offset along the island platform
constexpr double SPREAD = 320;    // the tracks spread over the last 40 m before a station

// overlap(a, b): do two rects (their x, y) overlap?
bool overlap(const Box3& a, const Rect& b) { return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1; }

// buildStation's W: a box in the station's line-local frame (c across the line, l along it) as a
// world box; the shells (tile) only replace solid ground, so they never refill carved space.
struct StationWriter {
  int axis;
  Point2 n;
  std::vector<UndergroundBox>* boxes;
  void operator()(double c0, double c1, double l0, double l1, double z0, double z1, uint16_t m) const {
    const int mode = m == MAT::TUNNEL_TILE ? 2 : 0;
    UndergroundBox q;
    if (axis == 0) {
      q.x0 = n.x + js::min(c0, c1);
      q.x1 = n.x + js::max(c0, c1);
      q.y0 = n.y + js::min(l0, l1);
      q.y1 = n.y + js::max(l0, l1);
    } else {
      q.x0 = n.x + js::min(l0, l1);
      q.x1 = n.x + js::max(l0, l1);
      q.y0 = n.y + js::min(c0, c1);
      q.y1 = n.y + js::max(c0, c1);
    }
    q.z0 = z0;
    q.z1 = z1;
    q.m = m;
    q.mode = mode;
    boxes->push_back(q);
  }
};

// stairStraight: a straight stair (local frame) from level z_low to z_high (walking surface
// voxels), starting at l = l0 and rising towards `dir`. Enclosed in a tile shell; `surface` adds
// railings where it breaks the ground.
void stair_straight(const StationWriter& W, double c0, double c1, double z_low, double z_high, double l0, double dir, bool surface = false) {
  const double R = z_high - z_low;
  for (double k = 1; k <= R; k += 1) {
    const double la = l0 + dir * 2 * (k - 1);
    const double lb = la + dir;
    const double l_lo = js::min(la, lb);
    const double l_hi = js::max(la, lb);
    W(c0 - 2, c1 + 2, l_lo, l_hi, z_low + k - 3, surface ? js::min(z_low + k + HEAD + 2, z_high - 1) : z_low + k + HEAD + 2, MAT::TUNNEL_TILE);
    W(c0, c1, l_lo, l_hi, z_low + k - 2, z_low + k, MAT::STAIR_CONCRETE);
    W(c0, c1, l_lo, l_hi, z_low + k + 1, z_low + k + HEAD, 0);
    W(c0, c0, l_lo, l_hi, z_low + k + 7, z_low + k + 7, MAT::RAILING);
    if (surface && z_low + k + HEAD > z_high - 1) {
      // where the stair breaks the sidewalk: green posts + top rail
      W(c0 - 2, c1 + 2, l_lo, l_hi, z_high + 1, z_high + HEAD + 4, 0);
      for (const double cc : {c0 - 1, c1 + 1}) {
        W(cc, cc, l_lo, l_hi, z_high, z_high, MAT::GRANITE);
        W(cc, cc, l_lo, l_hi, z_high + 8, z_high + 8, MAT::POLE_GREEN);
        if ((js::to_int32(l_lo) & 3) == 0) W(cc, cc, l_lo, l_lo, z_high + 1, z_high + 7, MAT::POLE_GREEN);
      }
    }
  }
  if (surface) {
    const double l_end = l0 + dir * 2 * R;
    const double le = js::min(l_end, l_end + dir * 3);
    const double lf = js::max(l_end, l_end + dir * 3);
    W(c0, c1, le, lf, z_high - 1, z_high, MAT::SIDEWALK);
    // entrance sign
    W(c1 + 3, c1 + 3, l_end, l_end, z_high + 1, z_high + 22, MAT::POLE_METAL);
    W(c1 + 1, c1 + 5, l_end, l_end, z_high + 18, z_high + 22, MAT::SIGNAGE_BLUE);
    // back railing at the low end of the opening
    const double lb = l0 + dir * 2 * js::max(0.0, R - HEAD - 1);
    W(c0 - 1, c1 + 1, lb, lb, z_high + 8, z_high + 8, MAT::POLE_GREEN);
    for (double c = c0; c <= c1; c += 4) W(c, c, lb, lb, z_high + 1, z_high + 7, MAT::POLE_GREEN);
    // globe lamps on the entrance posts
    W(c0 - 1, c0 - 1, l_end, l_end, z_high + 9, z_high + 16, MAT::POLE_GREEN);
    W(c0 - 2, c0, l_end - 1, l_end + 1, z_high + 17, z_high + 19, MAT::LAMP_LIGHT);
  }
}

// Running tunnel between two stations (or through a node without one): two tracks at +-TRACK_TUN
// that spread to the platform tracks (+-TRACK_ST) over the last SPREAD voxels before a station,
// the tunnel widening with them, so rails, sleepers and walls meet the station hall exactly.
void rasterize_tunnel(ChunkBuffer& chunk, const Tunnel& t) {
  uint16_t* d = chunk.data.data();
  const double len = t.l1 - t.l0;
  auto ease = [](double u) { return u <= 0 ? 0.0 : u >= 1 ? 1.0 : u * u * (3 - 2 * u); };
  for (int j = 0; j < kP; ++j) {
    const double y = chunk.wy(j);
    for (int i = 0; i < kP; ++i) {
      const double x = chunk.wx(i);
      const double c = (t.axis == 0 ? x : y) - t.fixed;
      const double l = t.axis == 0 ? y : x;
      if (std::fabs(c) > HALL_W + 4 || l < t.l0 || l > t.l1) continue;
      double spread = 0;
      if (t.s0) spread = js::max(spread, ease(1 - (l - t.l0) / SPREAD));
      if (t.s1) spread = js::max(spread, ease(1 - (t.l1 - l) / SPREAD));
      const double off = js::round(TRACK_TUN + (TRACK_ST - TRACK_TUN) * spread);
      const double tw = js::max(TUN_W, off + 18);
      const double ac = std::fabs(c);
      if (ac > tw + 4) continue;
      const double zb = js::round(t.z0 + ((t.z1 - t.z0) * (l - t.l0)) / js::or_(len, 1));
      const IdxRange kr = chunk.range_z(zb - 4, zb + 48);
      for (int k = kr.lo; k <= kr.hi; ++k) {
        const double z = chunk.wz(k);
        uint16_t m;
        if (ac > tw || z < zb || z > zb + 44)
          m = MAT::TUNNEL_WALL;
        else if (z == zb) {
          const double tc = std::fabs(ac - off);
          m = tc <= 9 && mod(l, 5) < 2 ? MAT::RAIL_TIE : MAT::BALLAST;
        } else if (z == zb + 1 && (ac == off - 6 || ac == off + 6))
          m = MAT::RAIL_STEEL;
        else if (z == zb + 44 - 1 && ac < 2 && mod(l, 80) < 8)
          m = MAT::LIGHT_STRIP;
        else if (ac == tw && z == zb + 12)
          m = MAT::PIPE;
        else
          m = 0;
        d[i + j * kP + k * kP2] = m;
      }
    }
  }
}

}  // namespace

Subway::Subway(const World& w, StreetLevel sl, size_t cache_capacity)
    : world(&w),
      street_level(std::move(sl)),
      line_every(w.config["subway"]["lineEvery"].to_number()),
      min_urbanization(w.config["subway"]["minUrbanization"].to_number()),
      stations_v_(cache_capacity),
      stations_h_(cache_capacity) {
  const RoadSpecs specs = road_specs(w.config);
  const RoadSpec* a = specs.get("arterial");
  if (!a) SVX_FAIL("subway: config.roads has no arterial class");
  art = *a;
}

bool Subway::line_exists(int axis, double i) const { return std::fmod(std::fabs(i + axis * 7), line_every) == 0; }

bool Subway::span_active(int axis, double i, double j) const {
  if (!line_exists(axis, i)) return false;
  const ArterialGrid& A = *world->arterials;
  const double fixed = A.line(axis, i);
  const double a = A.line(1 - axis, j);
  const double b = A.line(1 - axis, j + 1);
  const double m = (a + b) / 2;
  const double x = axis == 0 ? fixed : m;
  const double y = axis == 0 ? m : fixed;
  return world->fields->urban(x, y).u >= min_urbanization;
}

bool Subway::has_station(int axis, double i, double j) const {
  if (!(span_active(axis, i, j - 1) || span_active(axis, i, j))) return false;
  return !river_blocked(axis, i, j);
}

bool Subway::river_blocked(int axis, double i, double j) const {
  const Rivers* rivers = world->rivers.get();
  if (!rivers) return false;
  const Point2 n = node_xy(axis, i, j);
  const double e = HALF_LEN - 160;
  return rivers->hits_rect({n.x - e, n.y - e, n.x + e, n.y + e}, 4);
}

Point2 Subway::node_xy(int axis, double i, double j) const {
  const ArterialGrid& A = *world->arterials;
  if (axis == 0) {
    const double x = A.line(0, i);
    const double y = A.line(1, j);
    return {x, y};
  }
  const double x = A.line(0, j);
  const double y = A.line(1, i);
  return {x, y};
}

double Subway::street_z(double x, double y) const {
  if (!street_level) SVX_FAIL("subway: no street level (World::street_level, network/roadLevel)");
  return js::round(street_level(x, y));
}

double Subway::platform_z(int axis, double i, double j) const {
  const Point2 n = node_xy(axis, i, j);
  const double zs = street_z(n.x, n.y);
  return zs - (axis == 0 ? 112 : 176);
}

std::shared_ptr<const Station> Subway::station(int axis, double i, double j) const {
  MemoCache<uint64_t, Station>& cache = axis == 0 ? stations_v_ : stations_h_;
  return cache.get(cell_key(i, j), [&]() -> std::shared_ptr<const Station> {
    if (!has_station(axis, i, j)) return nullptr;
    return build_station(axis, i, j);
  });
}

std::shared_ptr<const Station> Subway::build_station(int axis, double i, double j) const {
  const Point2 n = node_xy(axis, i, j);
  const double zs = street_z(n.x, n.y);
  const double zp = platform_z(axis, i, j);
  const double zm = zs - 56;
  auto st = std::make_shared<Station>();
  const StationWriter W{axis, n, &st->boxes};
  const bool ends_lo = span_active(axis, i, j - 1);
  const bool ends_hi = span_active(axis, i, j);
  // hall shell + carve
  W(-HALL_W - 4, HALL_W + 4, -HALF_LEN - 4, HALF_LEN + 4, zp - 12, zp + 48, MAT::TUNNEL_TILE);
  W(-HALL_W, HALL_W, -HALF_LEN, HALF_LEN, zp - 7, zp + 43, 0);
  // tunnel mouths at the hall ends (the tunnel arrives at full platform width)
  if (ends_lo) W(-HALL_W, HALL_W, -HALF_LEN - 4, -HALF_LEN - 1, zp - 7, zp + 36, 0);
  if (ends_hi) W(-HALL_W, HALL_W, HALF_LEN + 1, HALF_LEN + 4, zp - 7, zp + 36, 0);
  // accent band + ceiling
  W(-HALL_W, -HALL_W, -HALF_LEN, HALF_LEN, zp + 10, zp + 12, MAT::TUNNEL_TILE_ACCENT);
  W(HALL_W, HALL_W, -HALF_LEN, HALF_LEN, zp + 10, zp + 12, MAT::TUNNEL_TILE_ACCENT);
  // track beds
  for (const double sgn : {-1.0, 1.0}) {
    const double c0 = sgn < 0 ? -HALL_W : 28;
    const double c1 = sgn < 0 ? -28 : HALL_W;
    W(c0, c1, -HALF_LEN, HALF_LEN, zp - 8, zp - 8, MAT::BALLAST);
    const double tc = sgn * TRACK_ST;
    // sleepers in the same world phase as the tunnels' (every 5 voxels)
    const double nl = axis == 0 ? n.y : n.x;
    const double l0 = -HALF_LEN + mod(HALF_LEN - nl, 5);
    for (double l = l0; l <= HALF_LEN; l += 5) W(tc - 9, tc + 9, l, l + 1, zp - 8, zp - 8, MAT::RAIL_TIE);
    W(tc - 6, tc - 6, -HALF_LEN, HALF_LEN, zp - 7, zp - 7, MAT::RAIL_STEEL);
    W(tc + 6, tc + 6, -HALF_LEN, HALF_LEN, zp - 7, zp - 7, MAT::RAIL_STEEL);
    // lights over the tracks
    for (double l = -HALF_LEN + 8; l < HALF_LEN; l += 24) W(tc - 1, tc + 1, l, l + 6, zp + 43, zp + 43, MAT::LIGHT_STRIP);
  }
  // island platform
  W(-27, 27, -HALF_LEN, HALF_LEN, zp - 7, zp, MAT::FLOOR_TERRAZZO);
  W(-27, -26, -HALF_LEN, HALF_LEN, zp, zp, MAT::PLATFORM_EDGE);
  W(26, 27, -HALF_LEN, HALF_LEN, zp, zp, MAT::PLATFORM_EDGE);
  for (double l = -HALF_LEN + 32; l < HALF_LEN - 16; l += 64) {
    for (const double c : {-15.0, 14.0}) W(c, c + 1, l, l + 1, zp + 1, zp + 43, MAT::CONCRETE_LIGHT);
    W(-4, 3, l + 20, l + 22, zp + 4, zp + 4, MAT::WOOD_MED);
    W(-4, -4, l + 20, l + 20, zp + 1, zp + 3, MAT::METAL_BLACK);
    W(3, 3, l + 20, l + 20, zp + 1, zp + 3, MAT::METAL_BLACK);
    W(-2, 1, l + 40, l + 40, zp + 26, zp + 30, MAT::SIGN_BLUE);
  }
  for (double l = -HALF_LEN + 8; l < HALF_LEN; l += 24) W(-2, 2, l, l + 6, zp + 43, zp + 43, MAT::LIGHT_STRIP);

  const bool crossing = axis == 1 && has_station(0, j, i);
  if (!crossing) {
    // mezzanine
    W(-44, 44, -84, 84, zm - 2, zm + 42, MAT::TUNNEL_TILE);
    W(-40, 40, -80, 80, zm + 1, zm + 38, 0);
    W(-40, 40, -80, 80, zm, zm, MAT::FLOOR_TERRAZZO);
    for (double l = -72; l <= 72; l += 16) W(-2, 2, l, l + 6, zm + 38, zm + 38, MAT::LIGHT_STRIP);
    // ticket gates line
    for (double c = -30; c <= 30; c += 8) W(c, c + 1, -24, -21, zm + 1, zm + 7, MAT::METAL_CHROME);
    W(24, 36, 30, 34, zm + 1, zm + 12, MAT::METAL_PANEL);
    W(24, 36, 30, 30, zm + 6, zm + 10, MAT::SCREEN);
    // platform -> mezzanine stair (in the platform centre, rising along +l)
    stair_straight(W, -10, 10, zp, zm, -(zm - zp), +1);
    // two street entrances on opposite sidewalks
    const double hc = art.hc;
    for (const double sgn : {-1.0, 1.0}) {
      const double ca = sgn > 0 ? hc + 10 : -hc - 30;
      const double cb = sgn > 0 ? hc + 30 : -hc - 10;
      const double lp0 = sgn > 0 ? 60 : -80;
      const double lp1 = sgn > 0 ? 80 : -60;
      // passage from the mezzanine to under the sidewalk
      const double pc0 = sgn > 0 ? 40 : cb;
      const double pc1 = sgn > 0 ? cb : -40;
      W(pc0 - 2, pc1 + 2, lp0 - 2, lp1 + 2, zm - 2, zm + 26, MAT::TUNNEL_TILE);
      W(pc0, pc1, lp0, lp1, zm + 1, zm + 24, 0);
      W(pc0, pc1, lp0, lp1, zm, zm, MAT::FLOOR_TERRAZZO);
      // stair up to the sidewalk, away from the intersection; it tops out on the sidewalk's own
      // level (the street may climb along the block)
      const double l_start = sgn > 0 ? lp1 : lp0;
      double z_top = zs + 1;
      for (int it = 0; it < 3; ++it) {
        const double l_end = l_start + sgn * 2 * (z_top - zm);
        const double cm = (ca + cb) / 2;
        const double px = axis == 0 ? n.x + cm : n.x + l_end;
        const double py = axis == 0 ? n.y + l_end : n.y + cm;
        z_top = street_z(px, py) + 1;
      }
      stair_straight(W, ca, cb, zm, z_top, l_start, sgn, true);
    }
  } else {
    // transfer: deep platform -> shallow station mezzanine
    const double zs0 = zs;
    const double zm_t = zs0 - 56;
    const double R = zm_t - zp;
    stair_straight(W, -10, 10, zp, zm_t, 80, +1);
    const double l_top = 80 + 2 * R;
    W(-12, 12, 38, l_top + 2, zm_t - 2, zm_t + 26, MAT::TUNNEL_TILE);
    W(-10, 10, 40, l_top, zm_t + 1, zm_t + 24, 0);
    W(-10, 10, 40, l_top, zm_t, zm_t, MAT::FLOOR_TERRAZZO);
  }
  bool first = true;
  Box3& bb = st->bb;
  for (const UndergroundBox& b : st->boxes) {
    if (first) {
      bb = {b.x0, b.y0, b.z0, b.x1, b.y1, b.z1};
      first = false;
    } else {
      bb = {js::min(bb.x0, b.x0), js::min(bb.y0, b.y0), js::min(bb.z0, b.z0), js::max(bb.x1, b.x1), js::max(bb.y1, b.y1), js::max(bb.z1, b.z1)};
    }
  }
  st->axis = axis;
  st->i = i;
  st->j = j;
  st->x = n.x;
  st->y = n.y;
  st->zp = zp;
  st->zs = zs;
  return st;
}

std::vector<std::shared_ptr<const Station>> Subway::stations_near(const Rect& rect) const {
  const ArterialGrid& A = *world->arterials;
  std::vector<std::shared_ptr<const Station>> out;
  const double pad = 420;
  const double ix0 = A.index_at(0, rect.x0 - pad);
  const double ix1 = A.index_at(0, rect.x1 + pad) + 1;
  const double iy0 = A.index_at(1, rect.y0 - pad);
  const double iy1 = A.index_at(1, rect.y1 + pad) + 1;
  for (double i = ix0; i <= ix1; i += 1) {
    if (!line_exists(0, i)) continue;
    for (double j = iy0; j <= iy1; j += 1) {
      std::shared_ptr<const Station> s = station(0, i, j);
      if (s && overlap(s->bb, rect)) out.push_back(std::move(s));
    }
  }
  for (double i = iy0; i <= iy1; i += 1) {
    if (!line_exists(1, i)) continue;
    for (double j = ix0; j <= ix1; j += 1) {
      std::shared_ptr<const Station> s = station(1, i, j);
      if (s && overlap(s->bb, rect)) out.push_back(std::move(s));
    }
  }
  return out;
}

std::vector<Tunnel> Subway::tunnels_near(const Rect& rect) const {
  const ArterialGrid& A = *world->arterials;
  std::vector<Tunnel> out;
  for (const int axis : {0, 1}) {
    const double lo = axis == 0 ? rect.x0 : rect.y0;
    const double hi = axis == 0 ? rect.x1 : rect.y1;
    const double i0 = A.index_at(axis, lo - 100);
    const double i1 = A.index_at(axis, hi + 100) + 1;
    const double alo = axis == 0 ? rect.y0 : rect.x0;
    const double ahi = axis == 0 ? rect.y1 : rect.x1;
    const double j0 = A.index_at(1 - axis, alo) - 1;
    const double j1 = A.index_at(1 - axis, ahi) + 1;
    for (double i = i0; i <= i1; i += 1) {
      if (!line_exists(axis, i)) continue;
      const double fixed = A.line(axis, i);
      if (fixed + HALL_W + 4 < lo || fixed - HALL_W - 4 > hi) continue;
      for (double j = j0; j <= j1; j += 1) {
        if (!span_active(axis, i, j)) continue;
        // tunnels start at the hall ends, or run on through a node without a station
        const bool s0 = !river_blocked(axis, i, j);
        const bool s1 = !river_blocked(axis, i, j + 1);
        const double a = A.line(1 - axis, j) + (s0 ? HALF_LEN + 4 : 0);
        const double b = A.line(1 - axis, j + 1) - (s1 ? HALF_LEN + 4 : 0);
        if (b < alo || a > ahi) continue;
        Tunnel t;
        t.axis = axis;
        t.fixed = fixed;
        t.l0 = a;
        t.l1 = b;
        t.s0 = s0;
        t.s1 = s1;
        t.z0 = platform_z(axis, i, j) - 8;
        t.z1 = platform_z(axis, i, j + 1) - 8;
        out.push_back(t);
      }
    }
  }
  return out;
}

bool Subway::blocks_surface(double x, double y) const {
  for (const std::shared_ptr<const Station>& s : stations_near({x - 4, y - 4, x + 4, y + 4})) {
    const double dx = x - s->x;
    const double dy = y - s->y;
    const double c = s->axis == 0 ? dx : dy;
    const double l = s->axis == 0 ? dy : dx;
    if (std::fabs(c) > art.hc + 2 && std::fabs(c) < art.hc + 36 && std::fabs(l) > 50 && std::fabs(l) < 220) return true;
  }
  return false;
}

SubwayMap Subway::map_data(const Rect& rect) const {
  SubwayMap out;
  for (const Tunnel& t : tunnels_near(rect)) {
    SubwayMap::Line l;
    if (t.axis == 0)
      l.pts = {{{t.fixed, t.l0 - HALF_LEN}, {t.fixed, t.l1 + HALF_LEN}}};
    else
      l.pts = {{{t.l0 - HALF_LEN, t.fixed}, {t.l1 + HALF_LEN, t.fixed}}};
    out.lines.push_back(l);
  }
  for (const std::shared_ptr<const Station>& s : stations_near(rect)) out.stations.push_back({s->x, s->y, s->axis});
  return out;
}

bool subway_z_range(const World& world, const Rect& rect, int lod, double* z0, double* z1) {
  (void)lod;
  const Subway* sw = world.subway.get();
  if (!sw) return false;
  double lo = js::kInf;
  double hi = -js::kInf;
  for (const std::shared_ptr<const Station>& s : sw->stations_near(rect)) {
    lo = js::min(lo, s->bb.z0);
    hi = js::max(hi, s->bb.z1);
  }
  for (const Tunnel& t : sw->tunnels_near(rect)) {
    lo = js::min(lo, t.z0 - 6, t.z1 - 6);
    hi = js::max(hi, t.z0 + 50, t.z1 + 50);
  }
  if (lo == js::kInf) return false;
  *z0 = lo;
  *z1 = hi;
  return true;
}

void subway_rasterize(const World& world, ChunkBuffer& chunk) {
  const Subway* sw = world.subway.get();
  if (!sw) return;
  const Box3 box = chunk.world_box();
  const Rect rect{box.x0, box.y0, box.x1, box.y1};
  for (const std::shared_ptr<const Station>& s : sw->stations_near(rect)) {
    if (s->bb.z1 < box.z0 || s->bb.z0 > box.z1) continue;
    for (const UndergroundBox& q : s->boxes) {
      if (q.x1 < box.x0 || q.x0 > box.x1 || q.y1 < box.y0 || q.y0 > box.y1 || q.z1 < box.z0 || q.z0 > box.z1) continue;
      chunk.fill_box(q.x0, q.y0, q.z0, q.x1, q.y1, q.z1, q.m, q.mode);
    }
  }
  for (const Tunnel& t : sw->tunnels_near(rect)) rasterize_tunnel(chunk, t);
}

}  // namespace svx::city
