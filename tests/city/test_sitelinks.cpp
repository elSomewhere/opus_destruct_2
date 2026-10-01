// svx_city tests — site links (voxel_city sites/links.js) against the reference (stage
// "sitelinks" of tools/procgen_ref), between the sites of the stand-in kinds of site_kinds.hpp:
// the links round the origin of every world, zAt, near, mapData, and the link source's z ranges
// and chunks.
#include <doctest.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "complexes.hpp"
#include "records.hpp"
#include "site_kinds.hpp"
#include "sites/links.hpp"
#include "world/sites.hpp"
#include "worlds.hpp"

using namespace svx::city;
using rec::Line;

namespace {

void rect_fields(Line& l, const Rect& q) { l << q.x0 << q.y0 << q.x1 << q.y1; }
std::vector<double> rect_array(const Rect& q) { return {q.x0, q.y0, q.x1, q.y1}; }

std::string ids(const std::vector<std::shared_ptr<const SiteLink>>& list) {
  std::string s;
  for (size_t i = 0; i < list.size(); ++i) s += js::cat(i ? "," : "", list[i]->id);
  return s.empty() ? "none" : s;
}

void port_fields(Line& l, const SitePort& p) {
  l << p.x << p.y << p.z;
  if (p.d == p.d)
    l << p.d;
  else
    l << rec::kUndef;
}

void link_lines(rec::Out& out, const SiteLink& L) {
  Line l;
  l << "link" << L.id << L.a->id << L.b->id;
  port_fields(l, L.A);
  port_fields(l, L.B);
  l << L.len << L.zlo << L.zhi << rect_array(L.bb) << L.prof.size();
  out << l;
  for (const LinkSeg& g : L.segs) {
    Line s;
    s << "seg" << g.p.x << g.p.y << g.q.x << g.q.y << g.along_x << g.s0 << g.len << g.dir_sign << rect_array(g.rect);
    out << s;
  }
  std::vector<double> prof(L.prof.begin(), L.prof.end());
  out << (Line() << "prof" << prof);
}

struct OnLink {
  double x, y, s;
};
// A point of a link drawn from r (stages/sitelinks.mjs pointOn).
OnLink point_on(rec::Samples& r, const SiteLink& L) {
  const LinkSeg& g = L.segs[static_cast<size_t>(std::floor(r() * static_cast<double>(L.segs.size())))];
  const double t = std::floor(r() * g.len);
  return {g.along_x ? g.p.x + g.dir_sign * t : g.p.x, g.along_x ? g.p.y : g.p.y + g.dir_sign * t, g.s0 + t};
}

}  // namespace

TEST_CASE("city sitelinks: site links conform to the reference (stage sitelinks)") {
  const std::vector<const SiteDef*> defs = test::test_site_defs();
  rec::Samples r(47);
  rec::Out out;
  const std::shared_ptr<const FeatureSource> src = site_link_source();
  out << (Line() << "source" << src->id << src->order << src->max_lod);
  for (const test::WorldCase& c : test::all_worlds()) {
    World w(c.overrides);
    w.sites = std::make_shared<SiteLayer>(w, defs);
    w.site_links = std::make_shared<SiteLinks>(w);
    const SiteLinks& SL = *w.site_links;
    const double cell = w.sites->cell;
    out << (Line() << "world" << c.key);
    std::vector<std::shared_ptr<const SiteLink>> links;
    for (double b = -3; b <= 2; b += 1)
      for (double a = -3; a <= 2; a += 1)
        for (double dir = 0; dir < 2; dir += 1) {
          std::shared_ptr<const SiteLink> L = SL.link(a, b, dir);
          if (!L) {
            out << (Line() << "none" << a << b << dir);
            continue;
          }
          link_lines(out, *L);
          links.push_back(std::move(L));
        }
    for (const std::shared_ptr<const SiteLink>& L : links) {
      std::string z;
      for (int k = 0; k < 12; ++k) {
        const double s = js::round(-300 + r() * (L->len + 600)) + (k % 3 == 0 ? 0.5 : 0);
        z += js::cat(k ? " " : "", s, ":", SiteLinks::z_at(*L, s));
      }
      out << (Line() << "z" << L->id << z);
      for (const int lod : {0, 1, 2}) {
        const OnLink p = point_on(r, *L);
        const double s0 = static_cast<double>(1 << lod);
        const double cx = std::floor((p.x + std::floor((r() - 0.5) * 64 * s0)) / (32 * s0));
        const double cy = std::floor((p.y + std::floor((r() - 0.5) * 64 * s0)) / (32 * s0));
        const double bx = (cx * 32 - 1) * s0;
        const double by = (cy * 32 - 1) * s0;
        const Rect rect{bx, by, bx + 34 * s0 - 1, by + 34 * s0 - 1};
        double z0 = 0, z1 = 0;
        Line l;
        l << "zr" << L->id << lod;
        rect_fields(l, rect);
        if (site_links_z_range(w, rect, &z0, &z1))
          l << std::vector<double>{z0, z1};
        else
          l << rec::kUndef;
        out << l;
      }
      for (const int lod : {0, 0, 0, 1, 2}) {
        const OnLink p = point_on(r, *L);
        const double zl = SiteLinks::z_at(*L, p.s);
        const double e = static_cast<double>(32 << lod);
        const double cx = std::floor((p.x + std::floor((r() - 0.5) * e)) / e);
        const double cy = std::floor((p.y + std::floor((r() - 0.5) * e)) / e);
        const double cz = std::floor((zl + 20 + std::floor((r() - 0.5) * e)) / e);
        ChunkBuffer ch = test::ground_chunk(lod, cx, cy, cz, zl + 400);
        site_links_rasterize(w, ch);
        const int count = ch.count_non_air();
        out << (Line() << "ch" << L->id << lod << cx << cy << cz << count << test::chunk_digest(ch.data));
      }
    }
    for (int k = 0; k < 12; ++k) {
      const double x0 = js::round((r() - 0.5) * 5 * cell);
      const double y0 = js::round((r() - 0.5) * 5 * cell);
      const double x1 = x0 + js::round(r() * cell * 0.5);
      const double y1 = y0 + js::round(r() * cell * 0.5);
      const Rect rect{x0, y0, x1, y1};
      Line l;
      l << "near";
      rect_fields(l, rect);
      l << ids(SL.near(rect));
      out << l;
      if (k % 3 == 0) {
        std::string ms;
        for (const SiteLinks::MapItem& m : SL.map_data(rect)) {
          std::string pts;
          for (size_t i = 0; i < m.pts.size(); ++i) pts += js::cat(i ? ";" : "", m.pts[i].x, ",", m.pts[i].y);
          ms += js::cat(ms.empty() ? "" : " ", m.id, ":", pts);
        }
        out << (Line() << "map" << (ms.empty() ? std::string("none") : ms));
      }
    }
  }
  CHECK(rec::record("sitelinks", out.text()) == rec::recorded_digest("sitelinks"));
}

TEST_CASE("city sitelinks: links and their tunnels are the same on any thread, in any order") {
  const std::vector<const SiteDef*> defs = test::test_site_defs();
  World w(Value::object({{"seed", 8}, {"terrain", Value::object({{"spawnMountains", Value::array({1, 2})}})}}));
  w.sites = std::make_shared<SiteLayer>(w, defs);
  w.site_links = std::make_shared<SiteLinks>(w);
  // the reference: one thread, row by row
  std::vector<std::string> want;
  std::vector<uint32_t> want_chunks;
  std::vector<std::array<double, 3>> chunks;
  for (double b = -2; b <= 1; b += 1)
    for (double a = -2; a <= 1; a += 1)
      for (double dir = 0; dir < 2; dir += 1) {
        const auto L = w.site_links->link(a, b, dir);
        want.push_back(L ? js::cat(L->id, "/", L->len, "/", L->zlo, "/", L->zhi) : std::string("-"));
        if (L) {
          const LinkSeg& g = L->segs[0];
          const double zl = SiteLinks::z_at(*L, g.s0);
          chunks.push_back({std::floor(g.p.x / 32), std::floor(g.p.y / 32), std::floor(zl / 32)});
        }
      }
  REQUIRE(!chunks.empty());
  for (const auto& c : chunks) {
    ChunkBuffer ch = test::ground_chunk(0, c[0], c[1], c[2], c[2] * 32 + 400);
    site_links_rasterize(w, ch);
    want_chunks.push_back(test::chunk_digest(ch.data));
  }
  // a second world: four threads, each in its own order, then the same queries
  World w2(Value::object({{"seed", 8}, {"terrain", Value::object({{"spawnMountains", Value::array({1, 2})}})}}));
  w2.sites = std::make_shared<SiteLayer>(w2, defs);
  w2.site_links = std::make_shared<SiteLinks>(w2);
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([&, t] {
      for (int k = 0; k < 32; ++k) {
        const int q = (k * 7 + t * 5) % 32;
        const double dir = q % 2, a = (q / 2) % 4 - 2, b = q / 8 - 2;
        w2.site_links->link(a, b, dir);
      }
      for (size_t k = 0; k < chunks.size(); ++k) {
        const auto& c = chunks[(k + static_cast<size_t>(t)) % chunks.size()];
        ChunkBuffer ch = test::ground_chunk(0, c[0], c[1], c[2], c[2] * 32 + 400);
        site_links_rasterize(w2, ch);
      }
    });
  for (std::thread& th : threads) th.join();
  std::vector<std::string> got;
  for (double b = -2; b <= 1; b += 1)
    for (double a = -2; a <= 1; a += 1)
      for (double dir = 0; dir < 2; dir += 1) {
        const auto L = w2.site_links->link(a, b, dir);
        got.push_back(L ? js::cat(L->id, "/", L->len, "/", L->zlo, "/", L->zhi) : std::string("-"));
      }
  CHECK(got == want);
  for (size_t k = 0; k < chunks.size(); ++k) {
    const auto& c = chunks[k];
    ChunkBuffer ch = test::ground_chunk(0, c[0], c[1], c[2], c[2] * 32 + 400);
    site_links_rasterize(w2, ch);
    CHECK(test::chunk_digest(ch.data) == want_chunks[k]);
  }
}
