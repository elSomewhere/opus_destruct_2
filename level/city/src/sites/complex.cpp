// svx_city — voxel_city sites/complex.js.
#include "sites/complex.hpp"

#include <unordered_map>

#include "core/math.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

const Registry<ComplexTheme>& complex_themes() { return complex_themes_mut(); }

Registry<ComplexTheme>& complex_themes_mut() {
  static Registry<ComplexTheme> r("complexTheme");
  return r;
}

void register_complex_themes() {
  Registry<ComplexTheme>& R = complex_themes_mut();
  R.add({.id = "military",
         .weights = {{"lab", 4}, {"storage", 3}, {"server", 2}, {"barracks", 2}, {"armory", 1}, {"control", 2}, {"generator", 1}, {"medical", 1}},
         .big = [](double, bool last) -> std::string { return last ? "reactor" : "hangarHall"; },
         .frame = MAT::HAZARD_YELLOW,
         .accent = MAT::EMERGENCY_RED,
         .shapes = 0});
  R.add({.id = "lab",
         .weights = {{"lab", 5}, {"cleanroom", 2}, {"server", 2}, {"office", 2}, {"containment", 2}, {"storage", 1}, {"control", 1}},
         .big = [](double k, bool) -> std::string { return k == 0 ? "atrium" : "testChamber"; },
         .frame = MAT::SIGN_BLUE,
         .accent = MAT::NEON_CYAN,
         .shapes = 0.45});
  R.add({.id = "power",
         .weights = {{"generator", 3}, {"control", 2}, {"storage", 2}, {"server", 1}, {"office", 1}},
         .big = [](double, bool last) -> std::string { return last ? "reactor" : "hangarHall"; },
         .frame = MAT::HAZARD_YELLOW,
         .accent = MAT::EMERGENCY_RED,
         .shapes = 0.25});
  R.add({.id = "barracks",
         .weights = {{"barracks", 4}, {"mess", 2}, {"armory", 2}, {"medical", 1}, {"office", 1}, {"storage", 1}},
         .big = [](double k, bool) -> std::string { return k == 0 ? "atrium" : "messHall"; },
         .frame = MAT::SIGN_GREEN,
         .accent = MAT::LIGHT_STRIP,
         .shapes = 0.2});
  R.add({.id = "hangar",
         .weights = {{"storage", 3}, {"armory", 2}, {"control", 1}, {"generator", 1}, {"office", 1}},
         .big = [](double, bool) -> std::string { return "hangarHall"; },
         .frame = MAT::HAZARD_YELLOW,
         .accent = MAT::EMERGENCY_RED,
         .shapes = 0.1});
  R.add({.id = "containment",
         .weights = {{"containment", 4}, {"lab", 2}, {"security", 2}, {"control", 1}, {"medical", 1}},
         .big = [](double k, bool) -> std::string { return k == 0 ? "testChamber" : "atrium"; },
         .frame = MAT::SIGN_RED,
         .accent = MAT::CRYSTAL_VIOLET,
         .shapes = 0.5});
}

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kCW = 24;  // CW = vx(3): corridor width

struct RoomStyle {
  uint16_t wall, floor;
  double h;
};

// ROOM_STYLE (?? ROOM_STYLE.hall)
const RoomStyle& room_style(const std::string& type) {
  static const std::unordered_map<std::string, RoomStyle> styles = {
      {"lab", {MAT::PANEL_WHITE, MAT::FLOOR_EPOXY, 32}},
      {"cleanroom", {MAT::PANEL_WHITE, MAT::FLOOR_TILE_WHITE, 30}},
      {"storage", {MAT::CONCRETE, MAT::FLOOR_CONCRETE, 32}},
      {"server", {MAT::METAL_PANEL_DARK, MAT::FLOOR_TILE_DARK, 30}},
      {"barracks", {MAT::PAINT_SAGE, MAT::FLOOR_LINOLEUM, 28}},
      {"mess", {MAT::PAINT_CREAM, MAT::FLOOR_TILE_GRAY, 30}},
      {"messHall", {MAT::PAINT_CREAM, MAT::FLOOR_TILE_GRAY, 40}},
      {"armory", {MAT::METAL_PANEL, MAT::FLOOR_CONCRETE, 28}},
      {"security", {MAT::METAL_PANEL, MAT::FLOOR_TILE_DARK, 28}},
      {"control", {MAT::PANEL_GRAPHITE, MAT::FLOOR_TILE_DARK, 30}},
      {"office", {MAT::PAINT_WHITE, MAT::FLOOR_CARPET_GRAY, 28}},
      {"generator", {MAT::CORRUGATED, MAT::FLOOR_EPOXY, 40}},
      {"medical", {MAT::WALL_TILE_WHITE, MAT::FLOOR_TILE_WHITE, 28}},
      {"containment", {MAT::METAL_PANEL_DARK, MAT::FLOOR_EPOXY, 32}},
      {"reactor", {MAT::METAL_PANEL_DARK, MAT::FLOOR_EPOXY, 64}},
      {"hangarHall", {MAT::CONCRETE_DARK, MAT::FLOOR_CONCRETE, 56}},
      {"testChamber", {MAT::PANEL_GRAPHITE, MAT::FLOOR_EPOXY, 96}},
      {"atrium", {MAT::PANEL_WHITE, MAT::FLOOR_TERRAZZO, kLevelGap + 40}},
      {"atriumTop", {MAT::PANEL_WHITE, MAT::GRATE_STEEL, 40}},
      {"hall", {MAT::METAL_PANEL, MAT::FLOOR_EPOXY, 28}},
      {"station", {MAT::TUNNEL_TILE, MAT::FLOOR_TERRAZZO, 44}},
  };
  auto it = styles.find(type);
  return it != styles.end() ? it->second : styles.at("hall");
}

// overlaps(a, b, pad): the rects (inclusive) come within pad of each other.
template <class A, class B>
bool overlaps(const A& a, const B& b, double pad = 0) {
  return a.x0 - pad <= b.x1 && b.x0 <= a.x1 + pad && a.y0 - pad <= b.y1 && b.y0 <= a.y1 + pad;
}

ComplexRoom shrink(const ComplexRoom& q, double d) {
  ComplexRoom s = q;
  s.x0 = q.x0 + d;
  s.y0 = q.y0 + d;
  s.x1 = q.x1 - d;
  s.y1 = q.y1 - d;
  return s;
}

// Anteroom in front of a shaft's near end.
ComplexRoom anteroom(const Rect& sr, double dir) {
  ComplexRoom q;
  if (dir > 0) {
    q.x0 = sr.x0 - 16, q.y0 = sr.y0 - vx(7), q.x1 = sr.x1 + 16, q.y1 = sr.y0 - 2;
  } else {
    q.x0 = sr.x0 - 16, q.y0 = sr.y1 + 2, q.x1 = sr.x1 + 16, q.y1 = sr.y1 + vx(7);
  }
  q.type = "hall";
  q.fixed = true;
  return q;
}

bool is_one_of(const std::string& s, std::initializer_list<const char*> list) {
  for (const char* x : list)
    if (s == x) return true;
  return false;
}

// ------------------------------------------------------------ planning

// Corridors: the nearest pairs of rooms first (Kruskal with union-find), each joined by an L or
// Z route that avoids shafts and open atrium floors, until every room is connected; then a few
// loops. Rooms no route can reach are dropped, so a level never holds a sealed room.
void route_corridors(Rng& rng, ComplexLevel& level, const std::vector<Rect>& keep) {
  const std::vector<ComplexRoom>& rooms = level.rooms;
  const size_t n = rooms.size();
  std::vector<Point2> cen;
  cen.reserve(n);
  for (const ComplexRoom& q : rooms) cen.push_back({js::round((q.x0 + q.x1) / 2), js::round((q.y0 + q.y1) / 2)});
  // corridors meet an atrium's upper level at its catwalk ring (on the side facing the other
  // room), never crossing the open centre
  auto port = [&](size_t i, size_t j) -> Point2 {
    const ComplexRoom& q = rooms[i];
    if (q.type != "atriumTop") return cen[i];
    const double dx = cen[j].x - cen[i].x;
    const double dy = cen[j].y - cen[i].y;
    if (std::fabs(dx) > std::fabs(dy)) return {dx > 0 ? q.x1 - 6 : q.x0 + 6, cen[i].y};
    return {cen[i].x, dy > 0 ? q.y1 - 6 : q.y0 + 6};
  };
  std::vector<Rect> blockers;
  for (const Rect& q : keep) blockers.push_back({q.x0 - 6, q.y0 - 6, q.x1 + 6, q.y1 + 6});
  for (const ComplexRoom& r : rooms)
    if (r.type == "atriumTop") blockers.push_back({r.x0 + 20, r.y0 + 20, r.x1 - 20, r.y1 - 20});
  auto clear = [&](const std::vector<Rect>& segs) {
    for (const Rect& g : segs)
      for (const Rect& b : blockers)
        if (overlaps(g, b)) return false;
    return true;
  };
  // (JS lists every route, then finds the first clear one: the same as trying them in order)
  auto route = [&](size_t a, size_t b) -> std::optional<std::vector<Rect>> {
    const Point2 A = port(a, b);
    const Point2 B = port(b, a);
    std::vector<Rect> r1{seg_rect(A.x, A.y, B.x, A.y, kCW), seg_rect(B.x, A.y, B.x, B.y, kCW)};
    if (clear(r1)) return r1;
    std::vector<Rect> r2{seg_rect(A.x, A.y, A.x, B.y, kCW), seg_rect(A.x, B.y, B.x, B.y, kCW)};
    if (clear(r2)) return r2;
    for (const double off : {-1.0, 1.0, -2.0, 2.0, -3.0, 3.0, -4.0, 4.0, -6.0, 6.0}) {
      const double my = js::round((A.y + B.y) / 2) + off * vx(12);
      std::vector<Rect> ry{seg_rect(A.x, A.y, A.x, my, kCW), seg_rect(A.x, my, B.x, my, kCW), seg_rect(B.x, my, B.x, B.y, kCW)};
      if (clear(ry)) return ry;
      const double mx = js::round((A.x + B.x) / 2) + off * vx(12);
      std::vector<Rect> rx{seg_rect(A.x, A.y, mx, A.y, kCW), seg_rect(mx, A.y, mx, B.y, kCW), seg_rect(mx, B.y, B.x, B.y, kCW)};
      if (clear(rx)) return rx;
    }
    return std::nullopt;
  };
  std::vector<size_t> parent(n);
  for (size_t i = 0; i < n; ++i) parent[i] = i;
  std::function<size_t(size_t)> find = [&](size_t i) -> size_t {
    if (parent[i] == i) return i;
    parent[i] = find(parent[i]);
    return parent[i];
  };
  struct Pair {
    size_t a, b;
    double d;
  };
  std::vector<Pair> pairs;
  for (size_t a = 0; a < n; ++a)
    for (size_t b = a + 1; b < n; ++b) pairs.push_back({a, b, std::fabs(cen[a].x - cen[b].x) + std::fabs(cen[a].y - cen[b].y)});
  js::sort(pairs, [](const Pair& p, const Pair& q) { return p.d - q.d; });
  for (const Pair& p : pairs) {
    const size_t fa = find(p.a);
    const size_t fb = find(p.b);
    if (fa == fb) continue;
    auto r = route(p.a, p.b);
    if (!r) {
      level.blocked.push_back({static_cast<double>(p.a), static_cast<double>(p.b)});
      continue;
    }
    level.corridors.insert(level.corridors.end(), r->begin(), r->end());
    const size_t ra = find(p.a);
    const size_t rb = find(p.b);
    parent[ra] = rb;
  }
  // a few loops for Doom-like circulation
  const double nn = static_cast<double>(n);
  for (double e = 0; e < js::round(nn * 0.3); e += 1) {
    const double a = rng.int_(0, nn - 1);
    const double b = rng.int_(0, nn - 1);
    if (a == b) continue;
    auto r = route(static_cast<size_t>(a), static_cast<size_t>(b));
    if (r) level.corridors.insert(level.corridors.end(), r->begin(), r->end());
  }
  // keep only rooms connected to the shaft's anteroom (room 0)
  const size_t root = find(0);
  std::vector<bool> kept(n, false);
  size_t n_kept = 0;
  for (size_t i = 0; i < n; ++i)
    if (find(i) == root || rooms[i].fixed) kept[i] = true, ++n_kept;
  if (n_kept != n) {
    std::vector<ComplexRoom> keep_rooms, dropped;
    for (size_t i = 0; i < n; ++i) (kept[i] ? keep_rooms : dropped).push_back(rooms[i]);
    std::vector<Rect> corridors;
    for (const Rect& c : level.corridors) {
      bool hits_dropped = false;
      for (const ComplexRoom& q : dropped)
        if (overlaps(c, q)) hits_dropped = true;
      bool hits_kept = false;
      for (const ComplexRoom& q : keep_rooms)
        if (overlaps(c, q)) hits_kept = true;
      if (!hits_dropped || hits_kept) corridors.push_back(c);
    }
    level.corridors = std::move(corridors);
    level.rooms = std::move(keep_rooms);
  }
}

// Ladder shafts between consecutive levels of a sector, through the floor of a room.
void plan_ladders(Complex& out, const ComplexSector& sector, const std::vector<Rect>& shaft_rects) {
  std::vector<Rect> avoid;
  for (const Rect& q : shaft_rects) avoid.push_back({q.x0 - 24, q.y0 - 24, q.x1 + 24, q.y1 + 24});
  auto free = [](const ComplexRoom& r) { return !r.fixed && !is_one_of(r.type, {"reactor", "testChamber", "atrium", "atriumTop"}); };
  for (size_t k = 0; k + 1 < sector.levels.size(); ++k) {
    ComplexLevel& upper = out.levels[sector.levels[k]];
    ComplexLevel& lower = out.levels[sector.levels[k + 1]];
    bool found = false;
    double best_area = 0;
    Rect best{};
    for (const ComplexRoom& a : upper.rooms) {
      if (!free(a)) continue;
      const std::vector<Rect> sa = shape_rects(a);
      for (const ComplexRoom& b : lower.rooms) {
        if (b.fixed || b.type == "atriumTop") continue;
        const double m = b.type == "reactor" || b.type == "testChamber" ? 12 : 9;
        const Rect o{js::max(a.x0, b.x0) + m, js::max(a.y0, b.y0) + m, js::min(a.x1, b.x1) - m, js::min(a.y1, b.y1) - m};
        if (o.x1 - o.x0 < 12 || o.y1 - o.y0 < 12) continue;
        const std::vector<Rect> sb = shape_rects(b);
        const double corners[4][2] = {{o.x0, o.y0}, {o.x1 - 7, o.y0}, {o.x0, o.y1 - 7}, {o.x1 - 7, o.y1 - 7}};
        for (const auto& cxy : corners) {
          const Rect q{cxy[0], cxy[1], cxy[0] + 7, cxy[1] + 7};
          // inside both room shapes (octagons, circles)
          auto some = [](const std::vector<Rect>& rs, auto pred) {
            for (const Rect& r : rs)
              if (pred(r)) return true;
            return false;
          };
          const bool in_a = some(sa, [&](const Rect& r) { return r.x0 <= q.x0 - 2 && r.x1 >= q.x1 + 2 && r.y0 <= q.y0 - 2 && r.y1 >= q.y0 - 2; }) &&
                            some(sa, [&](const Rect& r) { return r.x0 <= q.x0 - 2 && r.x1 >= q.x1 + 2 && r.y0 <= q.y1 + 2 && r.y1 >= q.y1 + 2; });
          const bool in_b = some(sb, [&](const Rect& r) { return r.x0 <= q.x0 && r.x1 >= q.x1 && r.y0 <= q.y0 && r.y1 >= q.y0; }) &&
                            some(sb, [&](const Rect& r) { return r.x0 <= q.x0 && r.x1 >= q.x1 && r.y0 <= q.y1 && r.y1 >= q.y1; });
          if (!in_a || !in_b) continue;
          if (some(avoid, [&](const Rect& v) { return overlaps(q, v); })) continue;
          const double area = (o.x1 - o.x0) * (o.y1 - o.y0);
          if (!found || area > best_area) {
            found = true;
            best_area = area;
            best = q;
          }
          break;
        }
      }
    }
    if (!found) continue;
    out.ladders.push_back({best, upper.zf, lower.zf, static_cast<double>(k), static_cast<double>(k + 1), sector.id});
    const Rect clear_zone{best.x0 - 14, best.y0 - 14, best.x1 + 14, best.y1 + 14};
    upper.keep_clear.push_back(clear_zone);
    lower.keep_clear.push_back(clear_zone);
  }
}

// Tram loop: a station under every sector's shaft (its anteroom widened into a platform hall)
// and an L-shaped tunnel from each station to the next, closing the loop.
ComplexTram plan_tram(double z, const std::vector<ComplexSector>& sectors) {
  ComplexTram tram;
  tram.z = z;
  for (const ComplexSector& s : sectors) {
    const Rect& r = s.shaft_rect;
    // platform hall in front of the shaft's near end (north side), track along its far side
    TramStation st;
    st.sector = s.id;
    st.hall.x0 = r.x0 - vx(14), st.hall.x1 = r.x1 + vx(14), st.hall.y0 = r.y0 - vx(12), st.hall.y1 = r.y0 - 2;
    st.hall.type = "station";
    st.hall.fixed = true;
    st.track = {js::round((st.hall.x0 + st.hall.x1) / 2), st.hall.y0 + vx(2.5)};
    tram.stations.push_back(std::move(st));
  }
  std::vector<TramStation>& stations = tram.stations;
  const double n = static_cast<double>(stations.size());
  // loop order: by angle around the centroid
  double sx = 0, sy = 0;
  for (const TramStation& s : stations) sx = sx + s.track.x;
  for (const TramStation& s : stations) sy = sy + s.track.y;
  const double cx = sx / n;
  const double cy = sy / n;
  js::sort(stations, [&](const TramStation& p, const TramStation& q) {
    return js::atan2(p.track.y - cy, p.track.x - cx) - js::atan2(q.track.y - cy, q.track.x - cx);
  });
  const double TW = vx(6);
  if (stations.size() > 1) {
    for (size_t k = 0; k < stations.size(); ++k) {
      if (stations.size() == 2 && k == 1) break;
      const TramStation& A = stations[k];
      const TramStation& B = stations[(k + 1) % stations.size()];
      // trains run along the platforms: leave and enter each station along x; between rows the
      // line swings round outside the station halls
      std::vector<Point2> pts;
      if (std::fabs(A.track.y - B.track.y) < 4) {
        pts = {A.track, {B.track.x, A.track.y}};
      } else {
        const double side = (A.track.x + B.track.x) / 2 >= cx ? 1 : -1;
        const double xs = side > 0 ? js::max(A.hall.x1, B.hall.x1) + vx(12) : js::min(A.hall.x0, B.hall.x0) - vx(12);
        pts = {A.track, {xs, A.track.y}, {xs, B.track.y}, B.track};
      }
      for (size_t q = 0; q + 1 < pts.size(); ++q) tram.segs.push_back(seg_rect(pts[q].x, pts[q].y, pts[q + 1].x, pts[q + 1].y, TW));
      tram.routes.push_back(std::move(pts));
    }
  }
  return tram;
}

// ------------------------------------------------------------ emission

using Boxes = std::vector<SiteBox>;

void push_rect(Boxes& list, const Rect& q, double z0, double z1, uint16_t m, int mode = 0) {
  list.push_back({q.x0, q.y0, z0, q.x1, q.y1, z1, m, mode});
}
// (the emitters' push(x0, y0, z0, x1, y1, z1, m): mode 0)
void put(Boxes& list, double x0, double y0, double z0, double x1, double y1, double z1, uint16_t m, int mode = 0) {
  list.push_back({x0, y0, z0, x1, y1, z1, m, mode});
}

// Coloured door frame across the corridor end that touches room q.
void door_frame(Boxes& details, const Rect& c, const ComplexRoom& q, double zf, uint16_t m) {
  const bool vertical = c.y1 - c.y0 > c.x1 - c.x0;
  // only a piece spanning the full corridor width gets a frame (subtracting round rooms leaves
  // thin slivers whose posts would wall the corridor off)
  if ((vertical ? c.x1 - c.x0 : c.y1 - c.y0) < kCW - 4) return;
  if (vertical) {
    double y;
    if (c.y1 + 1 >= q.y0 - 2 && c.y1 < q.y0)
      y = c.y1;
    else if (c.y0 - 1 <= q.y1 + 2 && c.y0 > q.y1)
      y = c.y0;
    else
      return;
    put(details, c.x0, y, zf + 1, c.x0, y, zf + 22, m);
    put(details, c.x1, y, zf + 1, c.x1, y, zf + 22, m);
    put(details, c.x0, y, zf + 22, c.x1, y, zf + 24, m);
    put(details, c.x0 + 1, y, zf + 25, c.x1 - 1, y, zf + 26, MAT::CONCRETE_DARK);
  } else {
    double x;
    if (c.x1 + 1 >= q.x0 - 2 && c.x1 < q.x0)
      x = c.x1;
    else if (c.x0 - 1 <= q.x1 + 2 && c.x0 > q.x1)
      x = c.x0;
    else
      return;
    put(details, x, c.y0, zf + 1, x, c.y0, zf + 22, m);
    put(details, x, c.y1, zf + 1, x, c.y1, zf + 22, m);
    put(details, x, c.y0, zf + 22, x, c.y1, zf + 24, m);
    put(details, x, c.y0 + 1, zf + 25, x, c.y1 - 1, zf + 26, MAT::CONCRETE_DARK);
  }
}

// Sunken channel of glowing waste across a hall with two railed bridges (2 voxels deep).
void nukage_channel(Boxes& details, const ComplexRoom& q, double zf) {
  const bool along_x = q.x1 - q.x0 >= q.y1 - q.y0;
  const double len = along_x ? q.x1 - q.x0 : q.y1 - q.y0;
  const double c0 = (along_x ? q.y0 : q.x0) + 24;
  const double c1 = c0 + 15;
  auto R = [&](double a0, double a1, double b0, double b1) -> std::array<double, 4> {
    return along_x ? std::array<double, 4>{a0, b0, a1, b1} : std::array<double, 4>{b0, a0, b1, a1};
  };
  const auto w = R(along_x ? q.x0 : q.y0, along_x ? q.x1 : q.y1, c0, c1);
  put(details, w[0], w[1], zf - 3, w[2], w[3], zf - 2, MAT::METAL_BLACK);
  put(details, w[0], w[1], zf - 1, w[2], w[3], zf - 1, MAT::NUKAGE);
  put(details, w[0], w[1], zf, w[2], w[3], zf, 0);
  for (const double e : {c0 - 1, c1 + 1}) {
    const auto s = R(along_x ? q.x0 : q.y0, along_x ? q.x1 : q.y1, e, e);
    put(details, s[0], s[1], zf, s[2], s[3], zf, MAT::HAZARD_YELLOW);
  }
  const double base = along_x ? q.x0 : q.y0;
  for (const double t : {js::round(len / 3), js::round((2 * len) / 3)}) {
    const auto bb = R(base + t - 6, base + t + 5, c0, c1);
    put(details, bb[0], bb[1], zf, bb[2], bb[3], zf, MAT::GRATE_STEEL);
    put(details, bb[0], bb[1], zf - 2, bb[2], bb[3], zf - 1, MAT::STEEL_BEAM);
    for (const double side : {t - 6, t + 5}) {
      const auto rr = R(base + side, base + side, c0, c1);
      put(details, rr[0], rr[1], zf + 7, rr[2], rr[3], zf + 7, MAT::RAILING);
      const auto pp = R(base + side, base + side, c0, c0);
      put(details, pp[0], pp[1], zf + 1, pp[2], pp[3], zf + 6, MAT::RAILING);
      const auto qq = R(base + side, base + side, c1, c1);
      put(details, qq[0], qq[1], zf + 1, qq[2], qq[3], zf + 6, MAT::RAILING);
    }
  }
}

// Catwalk ring along the walls of a tall hall (any shape), reached by a straight stair along the
// flat south (or north) wall where no corridor enters; railings on the inner edge, open where
// the stair lands.
void catwalk(Boxes& details, const ComplexRoom& q, double zf, const std::vector<Rect>& corridors, double H) {
  const double W = 8;
  const double zc = zf + H;
  const double w = q.x1 - q.x0 + 1;
  const double h = q.y1 - q.y0 + 1;
  const double c = q.shape == "octagon" ? js::max(4, js::round(js::min(w, h) * 0.27)) : 0;
  const double sx = q.x0 + c + W + 4;
  const double top_x = sx + 2 * H;
  if (top_x + 12 > q.x1 - c - W) return;
  auto hits = [&](double y0, double y1) {
    for (const Rect& k : corridors)
      if (k.x0 <= top_x + 12 && sx - 4 <= k.x1 && k.y0 <= y1 + 4 && y0 - 4 <= k.y1) return true;
    return false;
  };
  bool south = true;
  if (hits(q.y1 - W - 10, q.y1 + 3)) {
    if (hits(q.y0 - 3, q.y0 + W + 10)) return;
    south = false;
  }
  const std::vector<Rect> outer = shape_rects(q);
  const std::vector<Rect> inner = shape_rects(shrink(q, W));
  for (const Rect& r : r_subtract_all(outer, inner)) {
    if (r.x1 < r.x0 || r.y1 < r.y0) continue;
    put(details, r.x0, r.y0, zc - 1, r.x1, r.y1, zc - 1, MAT::STEEL_BEAM);
    put(details, r.x0, r.y0, zc, r.x1, r.y1, zc, MAT::GRATE_STEEL);
  }
  const double sy0 = south ? q.y1 - W - 10 : q.y0 + W;
  const double sy1 = south ? q.y1 - W : q.y0 + W + 10;
  for (double k = 1; k <= H; k += 1) {
    const double x = sx + 2 * (k - 1);
    put(details, x, sy0, zf + 1, x + 1, sy1, zf + k, MAT::STAIR_CONCRETE);
  }
  const Rect landing{top_x, sy0, top_x + 9, sy1};
  put(details, landing.x0, landing.y0, zc - 1, landing.x1, landing.y1, zc, MAT::GRATE_STEEL);
  const double edge_y = south ? sy0 : sy1;
  for (double k = 1; k <= H; k += 4) put(details, sx + 2 * (k - 1), edge_y, zf + k + 1, sx + 2 * (k - 1), edge_y, zf + k + 7, MAT::RAILING);
  // inner railing: the 1-voxel ring just inside the walkway, a gap at the landing
  const std::vector<Rect> ring = r_subtract_all(inner, shape_rects(shrink(q, W + 1)));
  const Rect gap{landing.x0 - 1, landing.y0 - 3, landing.x1 + 1, landing.y1 + 3};
  for (const Rect& r : r_subtract_all(ring, {gap})) {
    if (r.x1 < r.x0 || r.y1 < r.y0) continue;
    put(details, r.x0, r.y0, zc + 7, r.x1, r.y1, zc + 7, MAT::RAILING);
    put(details, r.x0, r.y0, zc + 3, r.x1, r.y1, zc + 3, MAT::RAILING);
  }
  for (double x = q.x0 + c + 16; x < q.x1 - c - 8; x += 32) put(details, x, q.y0 + 2, zc - 2, x + 1, q.y0 + 2, zc - 2, MAT::LAMP_CAGE);
}

// Test chamber (Black Mesa style): an octagonal hall 12 m tall around a pit with a glowing
// anomaly on emitter pylons, a gallery ring halfway up with a stair, observation slits and
// warning lights.
void test_chamber(Boxes& details, const ComplexRoom& q, double zf, const std::vector<Rect>& corridors) {
  const double cx = js::round((q.x0 + q.x1) / 2);
  const double cy = js::round((q.y0 + q.y1) / 2);
  const double R = js::round(js::min(q.x1 - q.x0, q.y1 - q.y0) * 0.16);
  // pit: carved below the floor, lined, with a hazard rim
  for (double dy = -R; dy <= R; dy += 1) {
    const double half = js::round(std::sqrt(js::max(0, R * R - dy * dy)));
    put(details, cx - half, cy + dy, zf - 60, cx + half, cy + dy, zf, 0);
    put(details, cx - half - 2, cy + dy, zf, cx - half - 1, cy + dy, zf, MAT::HAZARD_YELLOW);
    put(details, cx + half + 1, cy + dy, zf, cx + half + 2, cy + dy, zf, MAT::HAZARD_YELLOW);
  }
  put(details, cx - R - 3, cy - R - 3, zf - 64, cx + R + 3, cy + R + 3, zf - 61, MAT::METAL_BLACK);
  // the anomaly: a glowing crystal column floating over the pit, held by emitter arms
  for (double z = zf + 10; z <= zf + 34; z += 1) {
    const double r = js::max(1, js::round(5 - std::fabs(z - zf - 22) / 3));
    put(details, cx - r, cy - r, z, cx + r, cy + r, z, (js::to_int32(z) & 3) == 0 ? MAT::CRYSTAL_VIOLET : MAT::CRYSTAL_CYAN);
  }
  const double dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  for (const auto& d : dirs) {
    const double dx = d[0], dy = d[1];
    const double L = R + 18;
    for (double t = 8; t <= L; t += 1) put(details, cx + dx * t, cy + dy * t, zf + 22, cx + dx * t, cy + dy * t, zf + 23, MAT::METAL_CHROME);
    put(details, cx + dx * L - 2, cy + dy * L - 2, zf + 1, cx + dx * L + 2, cy + dy * L + 2, zf + 24, MAT::METAL_PANEL_DARK);
    put(details, cx + dx * 8, cy + dy * 8, zf + 22, cx + dx * 8, cy + dy * 8, zf + 23, MAT::NEON_CYAN);
  }
  catwalk(details, q, zf, corridors, 40);
  // warning beacons
  const double beacons[4][2] = {{q.x0 + 20, q.y0 + 20}, {q.x1 - 20, q.y0 + 20}, {q.x0 + 20, q.y1 - 20}, {q.x1 - 20, q.y1 - 20}};
  for (const auto& p : beacons) put(details, p[0], p[1], zf + 60, p[0] + 1, p[1] + 1, zf + 61, MAT::EMERGENCY_RED);
}

// Upper floor of an atrium: a catwalk ring along the walls over the open hall below.
void atrium_ring(Boxes& details, const ComplexRoom& q, double zf, uint16_t accent) {
  const double W = 12;
  const std::vector<Rect> outer = shape_rects(q);
  const std::vector<Rect> inner = shape_rects(shrink(q, W));
  for (const Rect& r : r_subtract_all(outer, inner)) {
    if (!(r.x1 >= r.x0 && r.y1 >= r.y0)) continue;
    put(details, r.x0, r.y0, zf - 1, r.x1, r.y1, zf - 1, MAT::STEEL_BEAM);
    put(details, r.x0, r.y0, zf, r.x1, r.y1, zf, MAT::GRATE_STEEL);
  }
  for (const Rect& r : r_subtract_all(inner, shape_rects(shrink(q, W + 1)))) {
    if (!(r.x1 >= r.x0 && r.y1 >= r.y0)) continue;
    put(details, r.x0, r.y0, zf + 7, r.x1, r.y1, zf + 7, MAT::RAILING);
    put(details, r.x0, r.y0, zf + 3, r.x1, r.y1, zf + 3, MAT::RAILING);
  }
  // hanging accent lights over the void
  const double cx = js::round((q.x0 + q.x1) / 2);
  const double cy = js::round((q.y0 + q.y1) / 2);
  put(details, cx - 1, cy - 1, zf + 12, cx + 1, cy + 1, zf + 30, MAT::STEEL_BEAM);
  put(details, cx - 4, cy - 4, zf + 8, cx + 4, cy + 4, zf + 11, accent);
}

// Containment cells: glass-fronted cells along one wall, some with a glowing specimen.
void containment_cells(Boxes& details, const ComplexRoom& q, double zf, const std::vector<Rect>& entries) {
  if (!q.shape.empty() && q.shape != "rect") return;
  for (double x = q.x0 + 2; x + 18 < q.x1 - 2; x += 20) {
    const Rect cell{x, q.y0, x + 18, q.y0 + 16};
    bool blocked = false;
    for (const Rect& e : entries)
      if (overlaps(cell, e)) blocked = true;
    if (blocked) continue;
    put(details, x, q.y0, zf + 1, x, q.y0 + 16, zf + 26, MAT::METAL_PANEL_DARK);
    put(details, x + 18, q.y0, zf + 1, x + 18, q.y0 + 16, zf + 26, MAT::METAL_PANEL_DARK);
    put(details, x, q.y0 + 16, zf + 1, x + 18, q.y0 + 16, zf + 26, MAT::GLASS_TINT);
    if ((js::sar(x, 3) & 3) == 1) put(details, x + 7, q.y0 + 6, zf + 1, x + 11, q.y0 + 10, zf + 8, MAT::CRYSTAL_VIOLET);
  }
}

void room_props(Boxes& out, const ComplexRoom& q, double zf, Rng& rng) {
  auto b = [&](double x0, double y0, double z0, double x1, double y1, double z1, uint16_t m) { out.push_back({x0, y0, zf + z0, x1, y1, zf + z1, m, 0}); };
  const double w = q.x1 - q.x0;
  const double h = q.y1 - q.y0;
  const double cx = js::round((q.x0 + q.x1) / 2);
  const double cy = js::round((q.y0 + q.y1) / 2);
  const std::string& t = q.type;
  if (t == "server") {
    for (double y = q.y0 + 8; y + 5 < q.y1 - 6; y += 14) {
      b(q.x0 + 6, y, 1, q.x1 - 6, y + 4, 16, MAT::METAL_PANEL_DARK);
      for (double x = q.x0 + 7; x < q.x1 - 6; x += 3) b(x, y, 3 + std::fmod(x, 4), x, y, 3 + std::fmod(x, 4), MAT::SIGNAL_GREEN);
    }
  } else if (t == "lab" || t == "cleanroom") {
    for (double x = q.x0 + 6; x + 10 < q.x1 - 4; x += 18) {
      b(x, q.y0 + 2, 1, x + 10, q.y0 + 6, 6, MAT::LAMINATE_WHITE);
      b(x + 3, q.y0 + 2, 7, x + 6, q.y0 + 2, 10, MAT::SCREEN);
    }
    for (double x = q.x0 + 10; x + 8 < q.x1 - 8; x += 20) {
      b(x, cy - 4, 1, x + 8, cy + 4, 1, MAT::METAL_BLACK);
      b(x + 1, cy - 3, 2, x + 7, cy + 3, 18, t == "cleanroom" ? MAT::GLASS : MAT::GLASS_GREEN);
      b(x, cy - 4, 19, x + 8, cy + 4, 20, MAT::METAL_BLACK);
    }
  } else if (t == "storage" || t == "armory") {
    for (double k = 0; k < js::max(3, js::round((w * h) / 900)); k += 1) {
      const double x = rng.int_(q.x0 + 2, q.x1 - 10);
      const double y = rng.int_(q.y0 + 2, q.y1 - 10);
      const double z1 = rng.int_(4, 10);
      b(x, y, 1, x + 7, y + 7, z1, t == "armory" ? MAT::CONTAINER_GREEN : MAT::CARDBOARD);
    }
  } else if (t == "barracks") {
    for (double x = q.x0 + 2; x + 8 < q.x1; x += 12) {
      b(x, q.y0 + 1, 1, x + 7, q.y0 + 16, 3, MAT::MATTRESS);
      b(x, q.y0 + 1, 10, x + 7, q.y0 + 16, 12, MAT::MATTRESS);
      b(x, q.y0 + 1, 1, x, q.y0 + 1, 14, MAT::STEEL_BEAM);
      b(x + 7, q.y0 + 16, 1, x + 7, q.y0 + 16, 14, MAT::STEEL_BEAM);
    }
  } else if (t == "mess" || t == "messHall") {
    for (double y = q.y0 + 10; y + 6 < q.y1 - 6; y += 16)
      for (double x = q.x0 + 8; x + 20 < q.x1 - 6; x += 28) {
        b(x, y, 5, x + 20, y + 4, 5, MAT::LAMINATE_GRAY);
        b(x + 2, y - 3, 3, x + 18, y - 2, 3, MAT::METAL_BLACK);
        b(x + 2, y + 6, 3, x + 18, y + 7, 3, MAT::METAL_BLACK);
      }
    b(q.x0 + 4, q.y1 - 6, 1, q.x1 - 4, q.y1 - 2, 7, MAT::APPLIANCE_STEEL);
  } else if (t == "security") {
    b(cx - 10, cy - 3, 1, cx + 10, cy + 3, 7, MAT::PANEL_GRAPHITE);
    b(cx - 8, cy - 3, 8, cx + 8, cy - 3, 12, MAT::SCREEN);
  } else if (t == "office") {
    for (double x = q.x0 + 4; x + 10 < q.x1 - 4; x += 14) {
      b(x, cy - 3, 5, x + 10, cy + 2, 5, MAT::WOOD_MED);
      b(x + 3, cy - 3, 6, x + 6, cy - 3, 9, MAT::SCREEN);
    }
  } else if (t == "control") {
    for (double x = q.x0 + 4; x + 8 < q.x1 - 4; x += 10) {
      b(x, q.y0 + 4, 1, x + 8, q.y0 + 8, 6, MAT::PANEL_GRAPHITE);
      b(x + 1, q.y0 + 4, 7, x + 7, q.y0 + 4, 11, MAT::SCREEN);
    }
    b(q.x0 + 4, q.y1 - 1, 8, q.x1 - 4, q.y1 - 1, 20, MAT::SCREEN);
  } else if (t == "generator" || t == "hangarHall") {
    for (double k = 0; k < 3; k += 1) {
      const double x = q.x0 + 8 + k * js::round((w - 20) / 3);
      const double y = cy - 8;
      b(x, y, 1, x + 14, y + 16, 14, MAT::HAZARD_YELLOW);
      b(x + 2, y + 2, 15, x + 12, y + 14, 18, MAT::PIPE);
      b(x + 6, y + 6, 19, x + 8, y + 8, 40, MAT::PIPE);
    }
  } else if (t == "reactor") {
    for (double dz = 1; dz < 60; dz += 1) {
      const double rr = dz < 8 ? 26 : 18;
      b(cx - rr, cy - rr * 0.4, dz, cx + rr, cy + rr * 0.4, dz, MAT::CONCRETE_DARK);
      b(cx - rr * 0.4, cy - rr, dz, cx + rr * 0.4, cy + rr, dz, MAT::CONCRETE_DARK);
      b(cx - rr * 0.75, cy - rr * 0.75, dz, cx + rr * 0.75, cy + rr * 0.75, dz, MAT::CONCRETE_DARK);
    }
    for (const double dz : {14.0, 30.0, 46.0}) b(cx - 19, cy - 19, dz, cx + 19, cy + 19, dz, MAT::NEON_CYAN);
    for (double k = 0; k < 6; k += 1) b(q.x0 + 12, q.y0 + 12 + k * 8, 20 + k * 3, q.x1 - 12, q.y0 + 13 + k * 8, 21 + k * 3, MAT::PIPE);
  } else if (t == "medical") {
    for (double x = q.x0 + 3; x + 8 < q.x1; x += 14) b(x, q.y0 + 2, 1, x + 7, q.y0 + 16, 4, MAT::BEDSHEET_WHITE);
  } else if (t == "atrium") {
    // planters and benches around a central sculpture
    b(cx - 6, cy - 6, 1, cx + 6, cy + 6, 3, MAT::PLANT_POT);
    b(cx - 4, cy - 4, 4, cx + 4, cy + 4, 10, MAT::PLANT);
    const double benches[4][2] = {{-24, 0}, {24, 0}, {0, -24}, {0, 24}};
    for (const auto& d : benches) b(cx + d[0] - 6, cy + d[1] - 1, 1, cx + d[0] + 6, cy + d[1] + 1, 3, MAT::WOOD_MED);
  }
}

void emit_level(BoxLists& out, const ComplexLevel& lv, Rng& rng, const ComplexTheme* theme) {
  Boxes& shells = out.shells;
  Boxes& carves = out.carves;
  Boxes& details = out.details;
  const double zf = lv.zf;
  const uint16_t frame_mat = theme ? theme->frame : static_cast<uint16_t>(MAT::HAZARD_YELLOW);
  const uint16_t accent = theme ? theme->accent : static_cast<uint16_t>(MAT::EMERGENCY_RED);
  std::vector<Rect> cut;
  for (const ComplexRoom& q : lv.rooms) {
    const std::vector<Rect> rs = shape_rects(q);
    cut.insert(cut.end(), rs.begin(), rs.end());
  }
  // corridors run centre to centre; only the pieces between rooms are built
  std::vector<Rect> pieces;
  for (const Rect& c : r_subtract_all(lv.corridors, cut))
    if (c.x1 >= c.x0 && c.y1 >= c.y0) pieces.push_back(c);
  for (const ComplexRoom& q : lv.rooms) {
    const RoomStyle& st = room_style(q.type);
    const std::vector<Rect> rects = shape_rects(q);
    const double z_base = q.type == "atriumTop" ? q.lower : zf;
    for (const Rect& r : rects) {
      push_rect(shells, {r.x0 - 2, r.y0 - 2, r.x1 + 2, r.y1 + 2}, z_base - 3, zf + st.h + 2, st.wall, 2);
      push_rect(carves, r, z_base + 1, zf + st.h, 0);
      if (q.type != "atriumTop") push_rect(details, r, zf, zf, st.floor);
      // wall band and ceiling lights
      push_rect(details, {r.x0 - 1, r.y0 - 1, r.x1 + 1, r.y1 + 1}, zf + 1, zf + 1, q.type == "testChamber" ? MAT::HAZARD_BLACK : MAT::HAZARD_YELLOW, 2);
      if (r.y1 - r.y0 > 4)
        for (double y = r.y0 + 12; y < r.y1 - 4; y += 28)
          for (double x = r.x0 + 6; x < r.x1 - 6; x += 20) put(details, x, y, zf + st.h, x + 6, y + 1, zf + st.h, MAT::LIGHT_STRIP);
    }
    // props, kept clear of corridor mouths and ladders
    std::vector<Rect> entries;
    for (const Rect& c : pieces) {
      if (!overlaps(c, q, 3)) continue;
      entries.push_back({js::max(c.x0, q.x0 - 3) - 14, js::max(c.y0, q.y0 - 3) - 14, js::min(c.x1, q.x1 + 3) + 14, js::min(c.y1, q.y1 + 3) + 14});
    }
    for (const Rect& k : lv.keep_clear) entries.push_back(k);
    Boxes props;
    room_props(props, q, zf, rng);
    auto inside = [&](const SiteBox& b) {
      for (const Rect& r : rects)
        if (b.x0 >= r.x0 && b.x1 <= r.x1 && b.y0 >= r.y0 && b.y1 <= r.y1) return true;
      return false;
    };
    for (const SiteBox& b : props) {
      bool blocked = false;
      for (const Rect& e : entries)
        if (overlaps(b, e)) {
          blocked = true;
          break;
        }
      if (blocked) continue;
      if (!q.shape.empty() && q.shape != "rect" && !inside(b)) continue;
      details.push_back(b);
    }
    if (q.type == "hangarHall") nukage_channel(details, q, zf);
    if (q.type == "reactor") catwalk(details, q, zf, pieces, 28);
    if (q.type == "testChamber") test_chamber(details, q, zf, pieces);
    if (q.type == "atriumTop") atrium_ring(details, q, zf, accent);
    if (q.type == "containment") containment_cells(details, q, zf, entries);
  }
  for (const Rect& c : pieces) {
    push_rect(shells, {c.x0 - 2, c.y0 - 2, c.x1 + 2, c.y1 + 2}, zf - 3, zf + 28 + 2, MAT::TUNNEL_WALL, 2);
    push_rect(carves, c, zf + 1, zf + 26, 0);
    push_rect(details, c, zf, zf, MAT::FLOOR_EPOXY);
    const bool lng = c.x1 - c.x0 > c.y1 - c.y0;
    const double mid = lng ? js::round((c.y0 + c.y1) / 2) : js::round((c.x0 + c.x1) / 2);
    if (lng)
      for (double x = c.x0 + 8; x < c.x1; x += 32) put(details, x, mid, zf + 26, x + 4, mid, zf + 26, MAT::LIGHT_STRIP);
    else
      for (double y = c.y0 + 8; y < c.y1; y += 32) put(details, mid, y, zf + 26, mid, y + 4, zf + 26, MAT::LIGHT_STRIP);
    // emergency / accent lamps on alternating walls
    if (lng)
      for (double x = c.x0 + 24; x < c.x1 - 8; x += 64) {
        const double y = (js::sar(x, 6) & 1) ? c.y0 : c.y1;
        put(details, x, y, zf + 19, x + 1, y, zf + 20, accent);
      }
    else
      for (double y = c.y0 + 24; y < c.y1 - 8; y += 64) {
        const double x = (js::sar(y, 6) & 1) ? c.x0 : c.x1;
        put(details, x, y, zf + 19, x, y + 1, zf + 20, accent);
      }
    // keycard door frames where the corridor meets a room
    for (const ComplexRoom& q : lv.rooms) {
      if (q.fixed) continue;
      if (!overlaps(c, q, 2)) continue;
      door_frame(details, c, q, zf, frame_mat);
    }
  }
}

// Rails (gauge 12 voxels) and sleepers along an axis-aligned polyline, the rails curving round
// each corner on a quarter circle (radius R), so the track runs on from station to station.
void emit_track(Boxes& details, const std::vector<Point2>& pts, double z) {
  const double R = 24;
  auto push = [&](double x0, double y0, double x1, double y1, double zz, uint16_t m) {
    details.push_back({js::min(x0, x1), js::min(y0, y1), zz, js::max(x0, x1), js::max(y0, y1), zz, m, 0});
  };
  auto dir_of = [](const Point2& a, const Point2& b) { return std::array<double, 2>{js::sign(b.x - a.x), js::sign(b.y - a.y)}; };
  for (size_t k = 0; k + 1 < pts.size(); ++k) {
    const Point2& a = pts[k];
    const Point2& b = pts[k + 1];
    const auto d = dir_of(a, b);
    const double dx = d[0], dy = d[1];
    // straight part between the curves
    const double sa = k > 0 ? R : 0;
    const double sb = k + 2 < pts.size() ? R : 0;
    const double ax = a.x + dx * sa;
    const double ay = a.y + dy * sa;
    const double bx = b.x - dx * sb;
    const double by = b.y - dy * sb;
    if ((bx - ax) * dx + (by - ay) * dy < 0) continue;
    for (const double o : {-6.0, 6.0}) push(ax - dy * o, ay + dx * o, bx - dy * o, by + dx * o, z + 1, MAT::RAIL_STEEL);
    const double len = std::fabs(bx - ax) + std::fabs(by - ay);
    for (double t = 0; t <= len; t += 1) {
      const double px = ax + dx * t;
      const double py = ay + dy * t;
      if (std::fmod(std::fmod(js::truthy(dx) ? px : py, 5) + 5, 5) != 0) continue;
      push(px - dy * 9, py + dx * 9, px + dy * 9 + dx, py - dx * 9 + dy, z, MAT::RAIL_TIE);
    }
    for (double t = 8; t < len; t += 48) push(ax + dx * t - dy, ay + dy * t + dx, ax + dx * (t + 4) + dy, ay + dy * (t + 4) - dx, z + 40, MAT::LIGHT_STRIP);
  }
  // quarter-circle curves at the corners
  for (size_t k = 1; k + 1 < pts.size(); ++k) {
    const auto d1 = dir_of(pts[k - 1], pts[k]);
    const auto d2 = dir_of(pts[k], pts[k + 1]);
    const double d1x = d1[0], d1y = d1[1], d2x = d2[0], d2y = d2[1];
    const Point2& c = pts[k];
    const double ox = c.x - d1x * R + d2x * R;
    const double oy = c.y - d1y * R + d2y * R;
    const double a0 = js::atan2(c.y - d1y * R - oy, c.x - d1x * R - ox);
    const double a1 = js::atan2(c.y + d2y * R - oy, c.x + d2x * R - ox);
    double da = a1 - a0;
    if (da > kPi) da -= 2 * kPi;
    if (da < -kPi) da += 2 * kPi;
    for (const double r : {R - 6, R + 6}) {
      const double n = std::ceil(std::fabs(da) * r * 1.5);
      for (double q = 0; q <= n; q += 1) {
        const double ang = a0 + (da * q) / n;
        const double x = js::round(ox + js::cos(ang) * r);
        const double y = js::round(oy + js::sin(ang) * r);
        push(x, y, x, y, z + 1, MAT::RAIL_STEEL);
      }
    }
    for (double q = 0; q <= 5; q += 1) {
      const double ang = a0 + (da * q) / 5;
      const double x0 = js::round(ox + js::cos(ang) * (R - 9));
      const double y0 = js::round(oy + js::sin(ang) * (R - 9));
      const double x1 = js::round(ox + js::cos(ang) * (R + 9));
      const double y1 = js::round(oy + js::sin(ang) * (R + 9));
      const double steps = js::max(std::fabs(x1 - x0), std::fabs(y1 - y0));
      for (double t = 0; t <= steps; t += 1) {
        const double x = js::round(x0 + ((x1 - x0) * t) / steps);
        const double y = js::round(y0 + ((y1 - y0) * t) / steps);
        push(x, y, x, y, z, MAT::RAIL_TIE);
      }
    }
  }
}

// Tram tunnel loop and station halls (the stations as JS's Map by sector keeps them).
void emit_tram(BoxLists& out, const ComplexTram& tram, const std::vector<const TramStation*>& stations) {
  Boxes& shells = out.shells;
  Boxes& carves = out.carves;
  Boxes& details = out.details;
  const double z = tram.z;
  for (const TramStation* s : stations) {
    const ComplexRoom& q = s->hall;
    push_rect(shells, {q.x0 - 2, q.y0 - 2, q.x1 + 2, q.y1 + 2}, z - 3, z + 46, MAT::TUNNEL_TILE, 2);
    push_rect(carves, q.rect(), z + 1, z + 44, 0);
    push_rect(details, q.rect(), z, z, MAT::FLOOR_TERRAZZO);
    // platform edge along the track side (a hand's width from the car) and a bench row
    push_rect(details, {q.x0, s->track.y + 12, q.x1, s->track.y + 12}, z, z, MAT::PLATFORM_EDGE);
    for (double x = q.x0 + 12; x + 10 < q.x1 - 8; x += 28) put(details, x, q.y1 - 4, z + 1, x + 10, q.y1 - 3, z + 3, MAT::WOOD_MED);
    for (double x = q.x0 + 8; x < q.x1; x += 24) put(details, x, q.y0 + 20, z + 44, x + 6, q.y0 + 21, z + 44, MAT::LIGHT_STRIP);
    put(details, q.x0 + 30, q.y1 - 1, z + 16, q.x0 + 44, q.y1 - 1, z + 22, MAT::SIGNAGE_BLUE);
  }
  for (const Rect& c : tram.segs) {
    push_rect(shells, {c.x0 - 2, c.y0 - 2, c.x1 + 2, c.y1 + 2}, z - 4, z + 42, MAT::TUNNEL_WALL, 2);
    push_rect(carves, c, z + 1, z + 40, 0);
    push_rect(details, c, z, z, MAT::BALLAST);
  }
  for (const std::vector<Point2>& pts : tram.routes) emit_track(details, pts, z);
  // a tram car waiting at the first station
  if (!stations.empty() && !tram.segs.empty()) {
    const Point2& t = stations[0]->track;
    put(details, t.x - 60, t.y - 10, z + 3, t.x + 60, t.y + 10, z + 24, MAT::PANEL_WHITE);
    put(details, t.x - 58, t.y - 10, z + 14, t.x + 58, t.y - 10, z + 20, MAT::GLASS_TINT);
    put(details, t.x - 58, t.y + 10, z + 14, t.x + 58, t.y + 10, z + 20, MAT::GLASS_TINT);
    put(details, t.x - 60, t.y - 10, z + 8, t.x + 60, t.y - 10, z + 9, MAT::SIGN_BLUE);
    put(details, t.x - 60, t.y + 10, z + 8, t.x + 60, t.y + 10, z + 9, MAT::SIGN_BLUE);
  }
}

// Ladder shaft through rock and the upper floor, rungs on the west wall, a railing round the hole.
void emit_ladder(Boxes& details, const ComplexLadder& L) {
  const Rect& r = L.rect;
  put(details, r.x0 - 2, r.y0 - 2, L.z_bot + 28, r.x1 + 2, r.y1 + 2, L.z_top - 1, MAT::CONCRETE, 2);
  put(details, r.x0, r.y0, L.z_bot + 1, r.x1, r.y1, L.z_top, 0);
  for (double z = L.z_bot + 1; z <= L.z_top; z += 1) {
    put(details, r.x0, r.y0 + 1, z, r.x0, r.y0 + 1, z, MAT::LADDER);
    put(details, r.x0, r.y1 - 1, z, r.x0, r.y1 - 1, z, MAT::LADDER);
    if (std::fmod(z - L.z_bot, 2) == 0) put(details, r.x0, r.y0 + 2, z, r.x0, r.y1 - 2, z, MAT::LADDER);
  }
  const double zt = L.z_top + 1;
  put(details, r.x1 + 1, r.y0 - 1, zt + 6, r.x1 + 1, r.y1 + 1, zt + 6, MAT::RAILING);
  put(details, r.x0, r.y0 - 1, zt + 6, r.x1 + 1, r.y0 - 1, zt + 6, MAT::RAILING);
  put(details, r.x0, r.y1 + 1, zt + 6, r.x1 + 1, r.y1 + 1, zt + 6, MAT::RAILING);
  const double posts[5][2] = {{r.x1 + 1, r.y0 - 1}, {r.x1 + 1, r.y1 + 1}, {r.x0, r.y0 - 1}, {r.x0, r.y1 + 1}, {r.x1 + 1, r.y0 + 3}};
  for (const auto& p : posts) put(details, p[0], p[1], zt, p[0], p[1], zt + 5, MAT::RAILING);
  put(details, r.x0 - 1, r.y0 - 1, L.z_top, r.x0 - 1, r.y1 + 1, L.z_top, MAT::HAZARD_YELLOW);
  put(details, r.x1, r.y1, L.z_bot + 24, r.x1, r.y1, L.z_bot + 24, MAT::LAMP_CAGE);
}

}  // namespace

Rect seg_rect(double x0, double y0, double x1, double y1, double w) {
  const double h = std::floor(w / 2);
  return {js::min(x0, x1) - h, js::min(y0, y1) - h, js::max(x0, x1) + h, js::max(y0, y1) + h};
}

std::vector<Rect> shape_rects(const ComplexRoom& q) {
  const double w = q.x1 - q.x0 + 1;
  const double h = q.y1 - q.y0 + 1;
  std::vector<Rect> out;
  if (q.shape == "octagon") {
    const double c = js::max(4, js::round(js::min(w, h) * 0.27));
    out.push_back({q.x0, q.y0 + c, q.x1, q.y1 - c});
    for (double i = 0; i < c; i += 1) {
      out.push_back({q.x0 + c - i, q.y0 + i, q.x1 - c + i, q.y0 + i});
      out.push_back({q.x0 + c - i, q.y1 - i, q.x1 - c + i, q.y1 - i});
    }
  } else if (q.shape == "round") {
    const double cx = (q.x0 + q.x1) / 2;
    const double cy = (q.y0 + q.y1) / 2;
    const double rx = w / 2;
    const double ry = h / 2;
    for (double y = q.y0; y <= q.y1; y += 1) {
      const double t = (y + 0.5 - cy) / ry;
      const double half = rx * std::sqrt(js::max(0, 1 - t * t));
      if (half < 1) continue;
      out.push_back({js::round(cx - half), y, js::round(cx + half) - 1, y});
    }
  } else if (q.shape == "cross") {
    const double bw = js::round(w * 0.2);
    const double bh = js::round(h * 0.2);
    out.push_back({q.x0, q.y0 + bh, q.x1, q.y1 - bh});
    out.push_back({q.x0 + bw, q.y0, q.x1 - bw, q.y0 + bh - 1});
    out.push_back({q.x0 + bw, q.y1 - bh + 1, q.x1 - bw, q.y1});
  } else {
    out.push_back({q.x0, q.y0, q.x1, q.y1});
  }
  return out;
}

ComplexShaft mk_shaft(const Rect& rect, const std::vector<double>& levels_z, double dir, bool open_top) {
  std::vector<double> zs = levels_z;
  js::sort(zs, [](double a, double b) { return a - b; });
  std::vector<StairFlight> flights;
  double f = 0;
  for (size_t g = 0; g + 1 < zs.size(); ++g) {
    const double rise = zs[g + 1] - zs[g];
    const double n = js::max(1, js::round(rise / 30));
    double z = zs[g];
    for (double k = 0; k < n; k += 1) {
      const double h = k == n - 1 ? zs[g + 1] - z : js::round(rise / n);
      flights.push_back({f, z - 1, h});
      f += 1;
      z += h;
    }
  }
  MakeStairOpts o;
  o.rect = rect;
  o.axis = 'v';
  o.dir = dir;
  o.lane_low = true;
  o.f0 = 0;
  o.f1 = f;
  ComplexShaft sh;
  sh.rect = rect;
  sh.st = make_stair(o);
  sh.st.flights = std::move(flights);
  sh.dir = dir;
  sh.open_top = open_top;
  sh.z_low = zs.empty() ? js::kNaN : zs.front();
  sh.z_high = zs.empty() ? js::kNaN : zs.back();
  sh.levels = std::move(zs);
  return sh;
}

Complex plan_complex(Rng& rng, const ComplexSpec& spec) {
  const StairDims sd = stair_dims(30);
  Complex out;
  std::vector<Rect> entry_rects;
  for (const ComplexEntrySpec& e : spec.entries) entry_rects.push_back(e.rect);
  static const std::vector<std::string> kBigTypes = {"hangarHall", "generator", "storage"};
  static const std::vector<std::pair<std::string, double>> kShapes = {{"octagon", 3}, {"round", 1.5}, {"cross", 1}};
  for (size_t si = 0; si < spec.sectors.size(); ++si) {
    const ComplexSectorSpec& S = spec.sectors[si];
    const ComplexTheme& theme = complex_themes().get(S.theme.empty() ? std::string("military") : S.theme);
    const Rect& b = S.bounds;
    const double cx = js::round((b.x0 + b.x1) / 2);
    const double cy = js::round((b.y0 + b.y1) / 2);
    const Rect c_rect{cx - 8, cy - js::round(sd.L / 2), cx - 8 + sd.W - 1, cy - js::round(sd.L / 2) + sd.L - 1};
    ComplexSector sector;
    sector.id = static_cast<double>(si);
    sector.theme = theme.id;
    sector.bounds = b;
    sector.center = {cx, cy};
    sector.shaft_rect = c_rect;
    sector.theme_def = &theme;
    std::vector<const ComplexEntrySpec*> entries;
    for (const ComplexEntrySpec& e : spec.entries)
      if (e.sector == static_cast<double>(si)) entries.push_back(&e);
    auto lv_z = [&](double k) { return S.z0 - k * kLevelGap; };
    struct Atrium {
      double k;
      ComplexRoom rect;
    };
    std::optional<Atrium> atrium;
    for (double k = 0; k < S.levels; k += 1) {
      const double zf = lv_z(k);
      std::vector<ComplexRoom> rooms{anteroom(c_rect, 1)};
      if (k == 0)
        for (const ComplexEntrySpec* e : entries) rooms.push_back(anteroom(e->rect, e->dir));
      std::vector<Rect> keep_rects{c_rect};
      if (k == 0)
        for (const ComplexEntrySpec* e : entries) keep_rects.push_back(e->rect);
      std::vector<Rect> keep;
      for (const Rect& q : keep_rects) keep.push_back({q.x0 - 8, q.y0 - 8, q.x1 + 8, q.y1 + 8});
      // an atrium opens on level k as a catwalk ring over its hall on level k + 1
      if (atrium && k == atrium->k + 1) {
        ComplexRoom hall;
        hall.x0 = atrium->rect.x0, hall.y0 = atrium->rect.y0, hall.x1 = atrium->rect.x1, hall.y1 = atrium->rect.y1;
        hall.type = "atrium";
        hall.shape = atrium->rect.shape;
        hall.fixed = true;
        rooms.push_back(std::move(hall));
      }
      const double r_min = S.rooms ? (*S.rooms)[0] : 9;
      const double r_max = S.rooms ? (*S.rooms)[1] : 14;
      const double target = rng.int_(r_min, r_max);
      const std::string big_type = theme.big(k, k == S.levels - 1);
      bool any_atrium = false;
      for (const ComplexRoom& r : rooms)
        if (r.type == "atrium") any_atrium = true;
      bool big_placed = any_atrium && big_type == "atrium";
      for (int t = 0; t < 500 && static_cast<double>(rooms.size()) < target + 2; ++t) {
        const bool big = !big_placed || (rooms.size() < 3 && rng.chance(0.4));
        std::string type;
        if (!big_placed)
          type = big_type;
        else if (big)
          type = rng.pick(kBigTypes);
        else
          type = rng.weighted(theme.weights);
        if (type == "atrium" && (k + 1 >= S.levels || atrium)) {
          big_placed = true;
          continue;
        }
        const bool sq = type == "testChamber" || type == "reactor";
        const double w = sq ? vx(rng.float_(26, 32)) : big ? vx(rng.float_(20, 30)) : vx(rng.float_(8, 16));
        const double h = sq ? w : big ? vx(rng.float_(16, 26)) : vx(rng.float_(7, 14));
        const double x0 = js::round(rng.float_(b.x0, b.x1 - w) / 8) * 8;
        const double y0 = js::round(rng.float_(b.y0, b.y1 - h) / 8) * 8;
        ComplexRoom q;
        q.x0 = x0, q.y0 = y0, q.x1 = x0 + w - 1, q.y1 = y0 + h - 1;
        const double pad = vx(5);
        bool hit = false;
        for (const ComplexRoom& o : rooms)
          if (overlaps(q, o, pad)) hit = true;
        for (const Rect& o : keep)
          if (overlaps(q, o, pad)) hit = true;
        if (hit) continue;
        // on levels below an atrium keep its footprint free
        if (atrium && k > atrium->k + 1 && overlaps(q, atrium->rect, pad)) continue;
        q.type = type == "atrium" ? "atriumTop" : type;
        if (type == "atrium") q.lower = lv_z(k + 1);
        if (type == "testChamber")
          q.shape = "octagon";
        else if (type == "atrium")
          q.shape = rng.chance(0.5) ? "octagon" : "rect";
        else if (type == "reactor" || type == "hangarHall" || type == "messHall")
          q.shape = "rect";
        else if (rng.chance(theme.shapes) && js::min(w, h) >= vx(9))
          q.shape = rng.weighted(kShapes);
        else
          q.shape = "rect";
        if (!big_placed) {
          big_placed = true;
          if (type == "atrium") atrium = Atrium{k, q};
        }
        rooms.push_back(std::move(q));
      }
      ComplexLevel level;
      level.sector = static_cast<double>(si);
      level.k = k;
      level.zf = zf;
      level.rooms = std::move(rooms);
      level.keep = keep_rects;
      level.theme = theme.id;
      route_corridors(rng, level, keep);
      sector.levels.push_back(out.levels.size());
      out.levels.push_back(std::move(level));
    }
    // the sector shaft links its levels (and the tram station below)
    std::vector<double> shaft_levels;
    for (const size_t l : sector.levels) shaft_levels.push_back(out.levels[l].zf);
    if (spec.tram_z) shaft_levels.push_back(*spec.tram_z);
    if (shaft_levels.size() > 1) out.shafts.push_back(mk_shaft(c_rect, shaft_levels));
    for (const ComplexEntrySpec* e : entries)
      out.shafts.insert(out.shafts.begin(), mk_shaft(e->rect, {lv_z(0), e->z_top}, std::isnan(e->dir) ? 1 : e->dir, e->open_top.value_or(true)));
    std::vector<Rect> shaft_rects{c_rect};
    shaft_rects.insert(shaft_rects.end(), entry_rects.begin(), entry_rects.end());
    plan_ladders(out, sector, shaft_rects);
    out.sectors.push_back(std::move(sector));
  }
  if (spec.tram_z) out.tram = plan_tram(*spec.tram_z, out.sectors);
  for (const ComplexSector& s : out.sectors) {
    if (out.bounds) {
      const Rect& bb = *out.bounds;
      out.bounds = Rect{js::min(bb.x0, s.bounds.x0), js::min(bb.y0, s.bounds.y0), js::max(bb.x1, s.bounds.x1), js::max(bb.y1, s.bounds.y1)};
    } else {
      out.bounds = s.bounds;
    }
  }
  return out;
}

void emit_complex(BoxLists& out, const Complex& cx, Rng& rng) {
  // (JS: new Map(stations.map((s) => [s.sector, s])): one entry per sector, in the stations' order)
  std::vector<const TramStation*> stations;
  if (cx.tram) {
    for (const TramStation& s : cx.tram->stations) {
      bool replaced = false;
      for (const TramStation*& t : stations)
        if (t->sector == s.sector) {
          t = &s;
          replaced = true;
        }
      if (!replaced) stations.push_back(&s);
    }
  }
  for (const ComplexLevel& lv : cx.levels) {
    const size_t si = static_cast<size_t>(lv.sector);
    emit_level(out, lv, rng, si < cx.sectors.size() ? cx.sectors[si].theme_def : nullptr);
  }
  if (cx.tram) emit_tram(out, *cx.tram, stations);
  for (const ComplexLadder& L : cx.ladders) emit_ladder(out.details, L);
  for (const ComplexShaft& sh : cx.shafts) emit_shaft(out.details, sh);
}

void emit_shaft(std::vector<SiteBox>& details, const ComplexShaft& sh) {
  const Rect& r = sh.rect;
  const double ring_top = sh.open_top ? sh.z_high - 1 : sh.z_high + 30;
  put(details, r.x0 - 2, r.y0 - 2, sh.z_low - 3, r.x1 + 2, r.y1 + 2, ring_top, MAT::CONCRETE);
  put(details, r.x0, r.y0, sh.z_low, r.x1, r.y1, sh.z_high + 26, 0);
  for (const double zl : sh.levels) {
    if (sh.open_top && zl == sh.z_high) continue;
    if (sh.dir > 0)
      put(details, r.x0 + 3, r.y0 - 3, zl + 1, r.x1 - 3, r.y0 - 1, zl + 18, 0);
    else
      put(details, r.x0 + 3, r.y1 + 1, zl + 1, r.x1 - 3, r.y1 + 3, zl + 18, 0);
  }
  if (sh.open_top) {
    const double z = sh.z_high + 1;
    const double far_y = sh.dir > 0 ? r.y1 + 1 : r.y0 - 1;
    put(details, r.x0 - 1, js::min(r.y0, r.y1) - 1, z, r.x0 - 1, r.y1 + 1, z + 7, MAT::RAILING);
    put(details, r.x1 + 1, r.y0 - 1, z, r.x1 + 1, r.y1 + 1, z + 7, MAT::RAILING);
    put(details, r.x0 - 1, far_y, z, r.x1 + 1, far_y, z + 7, MAT::RAILING);
  }
  const StairMats mats{MAT::STAIR_CONCRETE, MAT::STAIR_CONCRETE, MAT::CONCRETE_LIGHT, MAT::RAILING};
  for (const CanonBox& b : stair_boxes(sh.st, mats)) put(details, b.x0, b.y0, b.z0, b.x1, b.y1, b.z1, b.m);
  const Rect land = near_landing(sh.st);
  for (const StairFlight& fl : *sh.st.flights) put(details, land.x0, land.y0, fl.z0 - 1, land.x1, land.y1, fl.z0 + 1, MAT::STAIR_CONCRETE);
  put(details, land.x0, land.y0, sh.z_high - 2, land.x1, land.y1, sh.z_high, MAT::STAIR_CONCRETE);
  put(details, r.x0, r.y0, sh.z_low - 2, r.x1, r.y1, sh.z_low, MAT::FLOOR_CONCRETE);
  for (double z = sh.z_low + 20; z < sh.z_high; z += 30) put(details, r.x0 + 6, r.y1, z, r.x1 - 6, r.y1, z, MAT::LIGHT_STRIP);
}

}  // namespace svx::city
