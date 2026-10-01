// svx_city tests — the site layer (voxel_city world/sites.js) against the reference (stage
// "sites" of tools/procgen_ref), with the stand-in site kinds of site_kinds.hpp: placement over
// every world, pads and the ground override, the layer's queries, and the site source's z ranges
// and chunks.
#include <doctest.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "complexes.hpp"
#include "records.hpp"
#include "site_kinds.hpp"
#include "terrain/terrain.hpp"
#include "world/chart.hpp"
#include "world/sites.hpp"
#include "worlds.hpp"

using namespace svx::city;
using rec::Line;

namespace {

std::string ids(const std::vector<std::shared_ptr<const Site>>& list) {
  std::string s;
  for (size_t i = 0; i < list.size(); ++i) s += js::cat(i ? "," : "", list[i]->id);
  return s.empty() ? "none" : s;
}

void rect_fields(Line& l, const Rect& q) { l << q.x0 << q.y0 << q.x1 << q.y1; }
std::vector<double> rect_array(const Rect& q) { return {q.x0, q.y0, q.x1, q.y1}; }

void pad_line(rec::Out& out, const SitePad& p) {
  Line l;
  l << "pad";
  rect_fields(l, p.rect);
  l << p.z << p.margin;
  if (p.round)
    l << true;
  else
    l << rec::kUndef;
  if (p.ramp)
    l << js::cat("[", p.ramp->axis, ",", p.ramp->a0, ",", p.ramp->a1, ",", p.ramp->z0, ",", p.ramp->z1, "]");
  else
    l << rec::kUndef;
  if (!p.path.empty()) {
    std::string s = js::cat(p.path.size(), ":");
    for (size_t k = 0; k < p.path.size(); ++k) s += js::cat(k ? ";" : "", p.path[k].x, ",", p.path[k].y, ",", p.path[k].z);
    l << s << p.half;
  } else {
    l << rec::kUndef << rec::kUndef;
  }
  if (p.road)
    l << true;
  else
    l << rec::kUndef;
  out << l;
}

void site_lines(rec::Out& out, const Site& s) {
  Line l;
  l << "site" << s.id << s.type << s.a << s.b << s.cell.i << s.cell.j;
  rect_fields(l, s.cell_rect);
  rect_fields(l, s.rect);
  rect_fields(l, s.blend);
  l << s.margin << s.pad_z << s.seed << s.center.x << s.center.y;
  if (s.plan->bounds)
    l << rect_array(*s.plan->bounds);
  else
    l << rec::kUndef;
  l << rect_array(s.placed->footprint) << s.pads().size();
  out << l;
  for (const SitePad& p : s.pads()) pad_line(out, p);
}

}  // namespace

TEST_CASE("city sites: the site layer conforms to the reference (stage sites)") {
  const std::vector<const SiteDef*> defs = test::test_site_defs();
  rec::Samples r(43);
  rec::Out out;
  for (const test::WorldCase& c : test::all_worlds()) {
    World w(c.overrides);
    w.sites = std::make_shared<SiteLayer>(w, defs);
    const SiteLayer& L = *w.sites;
    const double cell = L.cell;
    std::vector<std::array<double, 2>> cells;
    for (double b = -3; b <= 3; b += 1)
      for (double a = -3; a <= 3; a += 1) cells.push_back({a, b});
    const bool cube = w.chart->kind == Chart::Kind::Cube;
    const double edge = cube ? std::floor((w.chart->half * 8) / cell) : 0;
    if (cube)
      for (double b = -1; b <= 1; b += 1)
        for (double a = edge - 2; a <= edge + 1; a += 1) cells.push_back({a, b});
    {
      Line l;
      l << "world" << c.key << cell << L.n;
      if (cube)
        l << edge;
      else
        l << rec::kUndef;
      out << l;
    }
    std::vector<std::shared_ptr<const Site>> found;
    for (const auto& ab : cells) {
      std::shared_ptr<const Site> s = L.site_at(ab[0], ab[1]);
      if (!s) {
        out << (Line() << "none" << ab[0] << ab[1]);
        continue;
      }
      site_lines(out, *s);
      found.push_back(std::move(s));
    }
    for (const std::shared_ptr<const Site>& s : found) {
      // padDistance and the ground at points round the site
      for (const SitePad& p : s->pads()) {
        const Rect& q = p.rect;
        std::string d;
        for (int k = 0; k < 4; ++k) {
          const double x = js::round(q.x0 - p.margin + r() * (q.x1 - q.x0 + 2 * p.margin));
          const double y = js::round(q.y0 - p.margin + r() * (q.y1 - q.y0 + 2 * p.margin));
          d += js::cat(k ? " " : "", x, ",", y, ":", pad_distance(p, x, y));
        }
        out << (Line() << "pd" << s->id << d);
      }
      const Rect& B = s->blend;
      const double m = s->margin;
      for (int k = 0; k < 40; ++k) {
        double x, y;
        if (k < 30) {
          x = js::round(B.x0 - m + r() * (B.x1 - B.x0 + 2 * m));
          y = js::round(B.y0 - m + r() * (B.y1 - B.y0 + 2 * m));
        } else {
          const SitePad& p = s->pads()[static_cast<size_t>(std::floor(r() * static_cast<double>(s->pads().size())))];
          if (!p.path.empty()) {
            const SitePathPoint& q = p.path[static_cast<size_t>(std::floor(r() * static_cast<double>(p.path.size())))];
            x = q.x + js::round((r() - 0.5) * 60);
            y = q.y + js::round((r() - 0.5) * 60);
          } else {
            x = js::round(p.rect.x0 + r() * (p.rect.x1 - p.rect.x0));
            y = js::round(p.rect.y0 + r() * (p.rect.y1 - p.rect.y0));
          }
        }
        const double nat = s->pad_z + js::round((r() - 0.5) * 160);
        const std::optional<SiteGround> g = L.ground(x, y, nat);
        if (!g) {
          out << (Line() << "g" << x << y << nat << rec::kUndef);
          continue;
        }
        Line l;
        l << "g" << x << y << nat << g->z << g->mat << g->sub << g->site->id << static_cast<double>(g->pad - g->site->pads().data()) << g->natural << g->inside;
        if (!g->pad->path.empty())
          l << std::vector<double>{g->path_z, g->path_s, g->path_c};
        else
          l << rec::kUndef;
        out << l;
      }
      out << (Line() << "cell" << s->id << ids(L.sites_in_cell(s->cell.i, s->cell.j)));
    }
    // sitesNear, nearest and mapData round the origin
    for (int k = 0; k < 20; ++k) {
      const double x0 = js::round((r() - 0.5) * 6 * cell);
      const double y0 = js::round((r() - 0.5) * 6 * cell);
      const double x1 = x0 + js::round(r() * cell);
      const double y1 = y0 + js::round(r() * cell);
      const Rect rect{x0, y0, x1, y1};
      {
        Line l;
        l << "near";
        rect_fields(l, rect);
        l << ids(L.sites_near(rect));
        out << l;
      }
      const double n = 1 + std::floor(r() * 6);
      const double radius = std::floor(r() * 3);
      std::string ns;
      for (const SiteLayer::Nearest& q : L.nearest(x0, y0, n, radius)) ns += js::cat(ns.empty() ? "" : ",", q.id, "/", q.type, "/", q.x, "/", q.y, "/", q.d, "/", q.z);
      out << (Line() << "nearest" << x0 << y0 << (ns.empty() ? std::string("none") : ns));
      std::string ms;
      for (const SiteLayer::MapItem& q : L.map_data(rect))
        ms += js::cat(ms.empty() ? "" : " ", q.id, "/", q.type, "/", q.rect.x0, ",", q.rect.y0, ",", q.rect.x1, ",", q.rect.y1, "/", q.center.x, ",", q.center.y);
      out << (Line() << "map" << (ms.empty() ? std::string("none") : ms));
    }
    {
      std::string ns;
      for (const SiteLayer::Nearest& q : L.nearest(0, 0)) ns += js::cat(ns.empty() ? "" : ",", q.id);
      out << (Line() << "nearest4" << (ns.empty() ? std::string("none") : ns));
    }
    // structures and siteSource round the sites of the cells next to the origin
    for (const std::shared_ptr<const Site>& s : found) {
      if (std::fabs(s->a) > 1 || std::fabs(s->b) > 1) continue;
      const SiteStructure& st = s->structure(w);
      uint32_t h = 2166136261u;
      for (const SiteBox& q : st.boxes) h = test::text_digest(test::box_line(q) + "\n", h);
      Line l;
      l << "st" << s->id << st.boxes.size() << h;
      if (st.bb)
        l << std::vector<double>{st.bb->x0, st.bb->y0, st.bb->z0, st.bb->x1, st.bb->y1, st.bb->z1};
      else
        l << rec::kUndef;
      l << st.custom.size();
      if (st.under) {
        const Complex& u = *st.under;
        l << js::cat(u.sectors.size(), "/", u.levels.size(), "/", u.shafts.size(), "/", u.ladders.size(), "/", u.tram ? u.tram->stations.size() : 0);
      } else {
        l << rec::kUndef;
      }
      out << l;
      // z ranges at the corners of the site's bounds (beyond its structure's, some)
      const Rect sb = s->plan->bounds ? *s->plan->bounds : s->blend;
      const double corners[4][2] = {{sb.x0, sb.y0}, {sb.x1, sb.y0}, {sb.x0, sb.y1}, {sb.x1, sb.y1}};
      for (const auto& q : corners) {
        const Rect rect{q[0] - 40, q[1] - 40, q[0] + 40, q[1] + 40};
        double z0 = 0, z1 = 0;
        Line zl;
        zl << "zc" << s->id;
        rect_fields(zl, rect);
        if (site_source_z_range(w, rect, &z0, &z1))
          zl << std::vector<double>{z0, z1};
        else
          zl << rec::kUndef;
        out << zl;
      }
      std::vector<std::array<double, 3>> pts;
      if (st.under) pts = test::complex_points(*st.under);
      pts.push_back({js::round(s->center.x), js::round(s->center.y), s->pad_z});
      for (const int lod : {0, 0, 0, 0, 1, 2, 2, 3, 4, 5}) {
        const auto& p = pts[static_cast<size_t>(std::floor(r() * static_cast<double>(pts.size())))];
        const double e = static_cast<double>(32 << lod);
        const double cx = std::floor((p[0] + std::floor((r() - 0.5) * e * 0.5)) / e);
        const double cy = std::floor((p[1] + std::floor((r() - 0.5) * e * 0.5)) / e);
        const double cz = std::floor((p[2] + std::floor((r() - 0.5) * e * 0.5)) / e);
        ChunkBuffer ch = test::ground_chunk(lod, cx, cy, cz, s->pad_z);
        const double s0 = static_cast<double>(1 << lod);
        const double bx = (cx * 32 - 1) * s0;
        const double by = (cy * 32 - 1) * s0;
        const Rect rect{bx, by, bx + 34 * s0 - 1, by + 34 * s0 - 1};
        double z0 = 0, z1 = 0;
        const bool has = site_source_z_range(w, rect, &z0, &z1);
        site_source_rasterize(w, ch, s->pad_z - 6);
        Line cl;
        cl << "ch" << s->id << lod << cx << cy << cz;
        if (has)
          cl << std::vector<double>{z0, z1};
        else
          cl << rec::kUndef;
        const int count = ch.count_non_air();
        cl << count << test::chunk_digest(ch.data);
        out << cl;
      }
    }
  }
  CHECK(rec::record("sites", out.text()) == rec::recorded_digest("sites"));
}

TEST_CASE("city sites: sites are the same whatever was asked before (cache order)") {
  const std::vector<const SiteDef*> defs = test::test_site_defs();
  World w(Value::object({{"seed", 1337}}));
  SiteLayer a(w, defs);
  SiteLayer b(w, defs);
  // a asks row by row, b in the reverse order and twice
  std::vector<std::shared_ptr<const Site>> sa, sb;
  for (double j = -2; j <= 2; j += 1)
    for (double i = -2; i <= 2; i += 1) sa.push_back(a.site_at(i, j));
  for (double j = 2; j >= -2; j -= 1)
    for (double i = 2; i >= -2; i -= 1) {
      b.site_at(i, j);
      sb.insert(sb.begin(), b.site_at(i, j));
    }
  REQUIRE(sa.size() == sb.size());
  for (size_t k = 0; k < sa.size(); ++k) {
    CHECK(static_cast<bool>(sa[k]) == static_cast<bool>(sb[k]));
    if (!sa[k] || !sb[k]) continue;
    CHECK(sa[k]->id == sb[k]->id);
    CHECK(sa[k]->rect == sb[k]->rect);
    CHECK(sa[k]->pad_z == sb[k]->pad_z);
  }
}

TEST_CASE("city sites: the layer, its ground and its source are the same on any thread") {
  const std::vector<const SiteDef*> defs = test::test_site_defs();
  auto make = [&] {
    auto w = std::make_unique<World>(Value::object({{"seed", 1337}}));
    w->sites = std::make_shared<SiteLayer>(*w, defs);
    return w;
  };
  auto ground_of = [](const World& w, double x, double y, double nat) {
    const std::optional<SiteGround> g = w.sites->ground(x, y, nat);
    if (!g) return std::string("-");
    return js::cat(g->z, "/", g->mat, "/", g->sub, "/", g->site->id, "/", static_cast<double>(g->pad - g->site->pads().data()), "/", g->inside);
  };
  auto chunk_of = [](const World& w, const std::array<double, 5>& c) {
    ChunkBuffer ch = test::ground_chunk(static_cast<int>(c[0]), c[1], c[2], c[3], c[4]);
    site_source_rasterize(w, ch, c[4] - 6);
    return test::chunk_digest(ch.data);
  };
  // one thread: the sites round the origin, points round them, chunks over them
  const auto w1 = make();
  std::vector<std::array<double, 3>> probes;
  std::vector<std::array<double, 5>> chunks;
  for (double j = -2; j <= 2; j += 1)
    for (double i = -2; i <= 2; i += 1) {
      const auto s = w1->sites->site_at(i, j);
      if (!s) continue;
      const Rect& b = s->blend;
      for (double k = 0; k < 12; k += 1) probes.push_back({b.x0 + std::fmod(k * 977, b.x1 - b.x0), b.y0 + std::fmod(k * 541, b.y1 - b.y0), s->pad_z + k - 6});
      for (const int lod : {0, 2}) {
        const double e = static_cast<double>(32 << lod);
        chunks.push_back({static_cast<double>(lod), std::floor(s->center.x / e), std::floor(s->center.y / e), std::floor((s->pad_z - 40) / e), s->pad_z});
      }
    }
  REQUIRE(!probes.empty());
  std::vector<std::string> want;
  for (const auto& p : probes) want.push_back(ground_of(*w1, p[0], p[1], p[2]));
  std::vector<uint32_t> want_chunks;
  for (const auto& c : chunks) want_chunks.push_back(chunk_of(*w1, c));
  // four threads on a fresh world, each in an order of its own
  const auto w2 = make();
  std::vector<std::vector<std::string>> got(4, std::vector<std::string>(probes.size()));
  std::vector<std::vector<uint32_t>> got_chunks(4, std::vector<uint32_t>(chunks.size()));
  std::vector<std::thread> threads;
  for (size_t t = 0; t < 4; ++t)
    threads.emplace_back([&, t] {
      for (size_t k = 0; k < chunks.size(); ++k) {
        const size_t q = (k * 3 + t * 7) % chunks.size();
        got_chunks[t][q] = chunk_of(*w2, chunks[q]);
      }
      for (size_t k = 0; k < probes.size(); ++k) {
        const size_t q = (k + t * probes.size() / 4) % probes.size();
        got[t][q] = ground_of(*w2, probes[q][0], probes[q][1], probes[q][2]);
      }
    });
  for (std::thread& th : threads) th.join();
  for (size_t t = 0; t < 4; ++t) {
    CHECK(got[t] == want);
    CHECK(got_chunks[t] == want_chunks);
  }
}
