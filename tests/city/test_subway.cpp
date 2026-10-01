// svx_city tests — the subway (voxel_city underground/subway.js) and its feature source against
// the reference (stage "subway"): lines, nodes, stations, tunnels, the queries and the rasterizer
// over chunks filled with ground of the stage's own.
#include <doctest.h>

#include <algorithm>
#include <set>
#include <tuple>

#include "city_records.hpp"
#include "network/arterials.hpp"
#include "records.hpp"
#include "underground/subway.hpp"
#include "underground_records.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"

using namespace svx::city;
using rec::Line;

namespace {

// The worlds of lib/worlds.mjs cityWorlds the stage samples (stages/subway.mjs SUBWAY_KEYS).
const char* const kSubwayKeys[] = {"cities",     "infiniteCity",  "wrapWorld:small", "island:medium", "planetNorth",  "torusInfinite",
                                   "cube2",      "islandFlat",    "islandCrowded",   "allMountains",  "angledCities", "angledInfiniteCity"};

bool sampled(const std::string& key) {
  for (const char* k : kSubwayKeys)
    if (key == k) return true;
  return false;
}

struct Node {
  int axis;
  double i, j;
};

// stages/subway.mjs nodesOf: the nodes a world's records cover, drawn from r, each once.
std::vector<Node> nodes_of(const World& w, rec::Samples& r) {
  const ArterialGrid& A = *w.arterials;
  std::vector<Node> out;
  std::set<std::tuple<int, double, double>> seen;
  auto add = [&](int axis, double i, double j) {
    if (seen.insert({axis, i + 0.0, j + 0.0}).second) out.push_back({axis, i, j});
  };
  auto around = [&](double ci, double cj, double d) {
    for (int axis = 0; axis < 2; ++axis)
      for (double di = -d; di <= d; di += 1)
        for (double dj = -d; dj <= d; dj += 1) add(axis, (axis == 0 ? ci : cj) + di, (axis == 0 ? cj : ci) + dj);
  };
  const CellIJ c0 = A.cell_at(0, 0);
  around(c0.i, c0.j, 4);
  std::vector<const Settlement*> towns = w.fields->settlements_in({-120000, -120000, 120000, 120000});
  if (towns.size() > 4) towns.resize(4);
  for (const Settlement* s : towns) {
    const CellIJ c = A.cell_at(js::round(s->x), js::round(s->y));
    around(c.i, c.j, 2);
  }
  for (int k = 0; k < 6; ++k) {
    const double x = js::round((r() - 0.5) * 400000);
    const double y = js::round((r() - 0.5) * 400000);
    const CellIJ c = A.cell_at(x, y);
    around(c.i, c.j, 1);
  }
  if (js::truthy(A.n)) {
    const double laps[2][2] = {{1, 0}, {-1, 2}};
    for (const auto& l : laps) around(c0.i + l[0] * A.n, c0.j + l[1] * A.n, 1);
  }
  return out;
}

}  // namespace

TEST_CASE("city subway: the subway and its feature source conform to the reference (stage subway)") {
  rec::Samples r(97);
  rec::Out out;
  for (const test::WorldCase& ws : test::city_worlds()) {
    if (!sampled(ws.key)) continue;
    const std::unique_ptr<World> wu = test::underground_world(ws.overrides);
    const World& w = *wu;
    const Subway* sw = w.subway.get();
    Line head;
    head << "subway" << ws.key;
    if (sw)
      head << std::vector<double>{sw->line_every, sw->min_urbanization, sw->art.hc};
    else
      head << rec::kUndef;
    out << head;
    if (!sw) continue;
    const StreetLevel street_level = test::terrain_street_level(w);
    auto bits = [&](int axis) {
      std::string s;
      for (double i = -12; i <= 12; i += 1) s += sw->line_exists(axis, i) ? '1' : '0';
      return s;
    };
    out << (Line() << "lines" << bits(0) << bits(1));
    // the nodes and their stations
    const std::vector<Node> nodes = nodes_of(w, r);
    std::vector<std::shared_ptr<const Station>> stations;
    std::set<char> listed;
    for (const Node& nd : nodes) {
      const int axis = nd.axis;
      const double i = nd.i;
      const double j = nd.j;
      const Point2 n = sw->node_xy(axis, i, j);
      out << (Line() << "n" << axis << i << j << n.x << n.y << sw->line_exists(axis, i) << sw->span_active(axis, i, j - 1) << sw->span_active(axis, i, j)
                     << sw->river_blocked(axis, i, j) << sw->has_station(axis, i, j) << sw->platform_z(axis, i, j));
      std::shared_ptr<const Station> s = sw->station(axis, i, j);
      if (!s) continue;
      stations.push_back(s);
      // the first station of each kind in full: on a vertical line, a horizontal one, a transfer
      const char kind = axis == 0 ? 'v' : sw->has_station(0, j, i) ? 'x' : 'h';
      const bool full = listed.count(kind) == 0;
      listed.insert(kind);
      test::station_lines(out, *s, full);
    }
    // stations and tunnel spans near rects round stations (four in five) and nodes, and their z range
    const double sizes[5] = {0, 33, 270, 1500, 6000};
    for (int k = 0; k < 40; ++k) {
      Point2 n;
      if (k % 5 != 4 && !stations.empty()) {
        const Station& s = *stations[static_cast<size_t>(std::floor(r() * static_cast<double>(stations.size())))];
        n = {s.x, s.y};
      } else {
        const Node& nd = nodes[static_cast<size_t>(std::floor(r() * static_cast<double>(nodes.size())))];
        n = sw->node_xy(nd.axis, nd.i, nd.j);
      }
      const double size = sizes[k % 5];
      const double x0 = js::round(n.x + (r() - 0.5) * 1000 - size / 2);
      const double y0 = js::round(n.y + (r() - 0.5) * 1000 - size / 2);
      const double y1 = y0 + js::round(size * r());
      const Rect rect{x0, y0, x0 + size, y1};
      Line l;
      l << "near" << rect.x0 << rect.y0 << rect.x1 << rect.y1;
      for (const auto& s : sw->stations_near(rect)) l << js::cat(s->axis, "/", s->i, "/", s->j);
      out << l;
      for (const Tunnel& t : sw->tunnels_near(rect)) out << test::tunnel_line(t);
      double z0 = 0, z1 = 0;
      const bool zr = subway_z_range(w, rect, k % 3, &z0, &z1);
      out << (Line() << "zr" << test::fzr(zr, z0, z1));
    }
    // mapData
    for (int k = 0; k < 3; ++k) {
      const Node& nd = nodes[static_cast<size_t>(std::floor(r() * static_cast<double>(nodes.size())))];
      const Point2 n = sw->node_xy(nd.axis, nd.i, nd.j);
      const SubwayMap md = sw->map_data({n.x - 3000, n.y - 2000, n.x + 2000, n.y + 3000});
      out << (Line() << "map" << md.lines.size() << md.stations.size());
      for (const SubwayMap::Line& ml : md.lines)
        out << (Line() << "ml" << std::vector<double>{ml.pts[0][0], ml.pts[0][1], ml.pts[1][0], ml.pts[1][1]});
      for (const SubwayMap::Stop& s : md.stations) out << (Line() << "ms" << s.x << s.y << s.axis);
    }
    // blocksSurface round the entrances of the first stations (each bound of its test) ...
    const double hc = sw->art.hc;
    const double cs[8] = {0, hc + 1, hc + 2, hc + 3, hc + 20, hc + 35, hc + 36, hc + 37};
    const double ls[9] = {0, 49, 50, 51, 130, 219, 220, 221, 260};
    for (size_t q = 0; q < stations.size() && q < 12; ++q) {
      const Station& s = *stations[q];
      std::string b;
      for (const double sc : {-1.0, 1.0})
        for (const double c : cs)
          for (const double sl : {-1.0, 1.0})
            for (const double l : ls) {
              const double x = s.x + (s.axis == 0 ? sc * c : sl * l);
              const double y = s.y + (s.axis == 0 ? sl * l : sc * c);
              b += sw->blocks_surface(x, y) ? '1' : '0';
            }
      out << (Line() << "bs" << s.axis << s.i << s.j << b);
    }
    // ... and anywhere near the nodes
    std::string b;
    for (int k = 0; k < 200; ++k) {
      const Node& nd = nodes[static_cast<size_t>(std::floor(r() * static_cast<double>(nodes.size())))];
      const Point2 n = sw->node_xy(nd.axis, nd.i, nd.j);
      const double x = js::round(n.x + (r() - 0.5) * 600);
      const double y = js::round(n.y + (r() - 0.5) * 600);
      b += sw->blocks_surface(x, y) ? '1' : '0';
    }
    out << (Line() << "bsr" << b);
    // column tiles' z range and chunks round stations: the platform, the mezzanine, an entrance's
    // top, a hall's end
    auto raster = [&](ChunkBuffer& ch, const SewerColumns*) { subway_rasterize(w, ch); };
    for (size_t q = 0; q < stations.size() && q < 4; ++q) {
      const Station& s = *stations[q];
      auto along = [&](double c, double l) { return s.axis == 0 ? Point2{s.x + c, s.y + l} : Point2{s.x + l, s.y + c}; };
      for (const int lod : {0, 1, 2}) {
        const double span = 32 << lod;
        const Rect rect = test::tile_rect(lod, std::floor(s.x / span), std::floor(s.y / span));
        double z0 = 0, z1 = 0;
        const bool zr = subway_z_range(w, rect, lod, &z0, &z1);
        out << (Line() << "tz" << lod << test::fzr(zr, z0, z1));
        const Point2 e = along(hc + 20, 200);
        const Point2 h = along(-30, 384);
        out << test::chunk_line(w, street_level, lod, s.x + 5, s.y - 7, s.zp, raster);
        out << test::chunk_line(w, street_level, lod, s.x, s.y, s.zs - 56, raster);
        out << test::chunk_line(w, street_level, lod, e.x, e.y, s.zs, raster);
        out << test::chunk_line(w, street_level, lod, h.x, h.y, s.zp + 20, raster);
      }
    }
    // ... above and below the first (its boxes kept out by their bounds)
    if (!stations.empty()) {
      const Station& s = *stations[0];
      out << test::chunk_line(w, street_level, 0, s.x, s.y, s.zs + 200, raster);
      out << test::chunk_line(w, street_level, 0, s.x, s.y, s.zp - 300, raster);
    }
    // ... and along tunnels: the middle of a span, the tracks' spread before a station, its first voxel
    int tunnels = 0;
    for (size_t k = 0; k < stations.size() && tunnels < 4; ++k) {
      const Station& s = *stations[k];
      for (const Tunnel& t : sw->tunnels_near({s.x - 600, s.y - 600, s.x + 600, s.y + 600})) {
        if (tunnels >= 4) break;
        tunnels += 1;
        auto at = [&](double l) { return t.axis == 0 ? Point2{t.fixed + 9, l} : Point2{l, t.fixed - 11}; };
        const Point2 m = at(js::round((t.l0 + t.l1) / 2));
        const Point2 sp = at(t.l0 + 100);
        for (const int lod : {0, 1, 2}) {
          out << test::chunk_line(w, street_level, lod, m.x, m.y, js::round((t.z0 + t.z1) / 2) + 6, raster);
          out << test::chunk_line(w, street_level, lod, sp.x, sp.y, t.z0 + 6, raster);
        }
        const Point2 f = at(t.l0);
        out << test::chunk_line(w, street_level, 0, f.x, f.y, t.z0 + 6, raster);
      }
    }
    // ... through the first node a river keeps a station from (the tracks do not spread there)
    const Node* wet = nullptr;
    for (const Node& nd : nodes)
      if (sw->river_blocked(nd.axis, nd.i, nd.j) && sw->span_active(nd.axis, nd.i, nd.j)) {
        wet = &nd;
        break;
      }
    if (wet) {
      const Point2 n = sw->node_xy(wet->axis, wet->i, wet->j);
      for (const Tunnel& t : sw->tunnels_near({n.x - 50, n.y - 50, n.x + 50, n.y + 50})) {
        if (t.s0) continue;
        const double l = (t.axis == 0 ? n.y : n.x) + 30;
        const double x = t.axis == 0 ? t.fixed - 20 : l;
        const double y = t.axis == 0 ? l : t.fixed + 20;
        out << test::tunnel_line(t);
        for (const int lod : {0, 1}) out << test::chunk_line(w, street_level, lod, x, y, t.z0 + 6, raster);
      }
    }
    Line count;
    count << "count" << stations.size() << tunnels;
    if (wet)
      count << std::vector<double>{static_cast<double>(wet->axis), wet->i, wet->j};
    else
      count << rec::kUndef;
    out << count;
  }
  CHECK(rec::record("subway", out.text()) == rec::recorded_digest("subway"));
}
