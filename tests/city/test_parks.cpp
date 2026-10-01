// svx_city tests — park layouts (voxel_city city/parks.js) against the reference (stage "parks"):
// the layouts of synthetic park spaces, the distances and the grove noise, and the park ground at
// points over and round them.
#include <doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "city/parks.hpp"
#include "city/space.hpp"
#include "records.hpp"

using namespace svx::city;
using rec::Line;

namespace {

constexpr double kPi = 3.141592653589793;

// stages/parks.mjs SIZES: [w0, w1, h0, h1] by k % 6; DEGENERATE: [w, h] of the parks after them
const double kSizes[6][4] = {{0, 100, 0, 100}, {100, 300, 100, 300}, {280, 720, 280, 720}, {480, 1200, 480, 1200}, {40, 300, 600, 1500}, {700, 1600, 700, 1600}};
const double kDegenerate[6][2] = {{0, 0}, {50, 0}, {0, 50}, {0, 800}, {1, 1}, {900, 0}};

std::string fseg(const ParkSeg& s) { return js::cat(s[0], ",", s[1], ",", s[2], ",", s[3]); }

}  // namespace

TEST_CASE("city parks: park layouts conform to the reference (stage parks)") {
  rec::Samples r(29);
  rec::Out out;
  for (int k = 0; k < 246; ++k) {
    const double* sz = kSizes[k % 6];
    const double i = std::floor((r() - 0.5) * 100);
    const double j = std::floor((r() - 0.5) * 100);
    const double x0 = std::floor((r() - 0.5) * 600000);
    const double y0 = std::floor((r() - 0.5) * 600000);
    const double w = k < 240 ? sz[0] + std::floor(r() * (sz[1] - sz[0])) : kDegenerate[k - 240][0];
    const double h = k < 240 ? sz[2] + std::floor(r() * (sz[3] - sz[2])) : kDegenerate[k - 240][1];
    OpenSpace space;
    space.id = js::cat("C", i, "_", j, "/b", k, "/o");
    space.kind = "park";
    space.rect = {x0, y0, x0 + w, y0 + h};
    const ParkLayout& L = park_layout(space);
    CHECK(&park_layout(space) == &L);
    out << (Line() << "park" << k << space.id << x0 << y0 << w << h << L.W << L.H << L.seed << L.hub.u << L.hub.v << L.hub.r << L.path_w << L.G << L.ents.size()
                   << L.segs.size());
    std::string ents;
    for (size_t q = 0; q < L.ents.size(); ++q) ents += js::cat(q ? ";" : "", L.ents[q].u, ",", L.ents[q].v);
    out << (Line() << "ents" << ents);
    const std::optional<ParkPond>& pd = L.pond;
    if (pd)
      out << (Line() << "pond" << pd->u << pd->v << pd->r << pd->a[0] << pd->a[1] << pd->a[2] << pd->p[0] << pd->p[1] << pd->p[2] << pd->stretch << pd->rot);
    else
      out << (Line() << "pond" << rec::kUndef);
    for (size_t q = 0; q < L.segs.size(); q += 20) {
      std::string s;
      for (size_t n = q; n < std::min(q + 20, L.segs.size()); ++n) s += (n > q ? ";" : "") + fseg(L.segs[n]);
      out << (Line() << "segs" << s);
    }
    // the lookup grid, by key
    std::vector<double> keys;
    for (const auto& kv : L.grid) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    for (size_t q = 0; q < keys.size(); q += 40) {
      std::string s;
      for (size_t n = q; n < std::min(q + 40, keys.size()); ++n) {
        std::string list;
        const std::vector<uint32_t>& segs = L.grid.at(keys[n]);
        for (size_t m = 0; m < segs.size(); ++m) list += js::cat(m ? "." : "", segs[m]);
        s += js::cat(n > q ? " " : "", keys[n], ":", list);
      }
      out << (Line() << "grid" << s);
    }
    // points: over and round the park, by the hub, the pond's shore and the paths
    std::vector<std::array<double, 2>> pts;
    for (int q = 0; q < 60; ++q) {
      const double u = -0.15 * L.W - 20 + r() * (1.3 * L.W + 40);
      const double v = -0.15 * L.H - 20 + r() * (1.3 * L.H + 40);
      pts.push_back(q % 2 ? std::array<double, 2>{u, v} : std::array<double, 2>{js::round(u), js::round(v)});
    }
    for (int q = 0; q < 20; ++q) {
      const double u = L.hub.u + (r() - 0.5) * 2 * (L.hub.r + 20);
      const double v = L.hub.v + (r() - 0.5) * 2 * (L.hub.r + 20);
      pts.push_back(q % 2 ? std::array<double, 2>{u, v} : std::array<double, 2>{js::round(u), js::round(v)});
    }
    if (pd)
      for (int q = 0; q < 24; ++q) {
        const double a = r() * 2 * kPi;
        const double d = pd->r * (0.5 + r()) * pd->stretch;
        pts.push_back({js::round(pd->u + js::cos(a) * d), js::round(pd->v + js::sin(a) * d)});
      }
    for (int q = 0; q < 20 && !L.segs.empty(); ++q) {
      const ParkSeg& s = L.segs[static_cast<size_t>(std::floor(r() * static_cast<double>(L.segs.size())))];
      const double t = r();
      const double u = s[0] + (s[2] - s[0]) * t + (r() - 0.5) * 30;
      const double v = s[1] + (s[3] - s[1]) * t + (r() - 0.5) * 30;
      pts.push_back(q % 2 ? std::array<double, 2>{u, v} : std::array<double, 2>{js::round(u), js::round(v)});
    }
    std::vector<std::string> recs;
    for (const auto& p : pts) {
      const double u = p[0];
      const double v = p[1];
      const double lap = r() < 0.3 ? std::floor((r() - 0.5) * 8) * 25600 : 0;
      SpaceSample s;
      park_surface(space, u, v, s, u + lap, v - lap);
      recs.push_back(js::cat(u, ",", v, ",", lap, ":", path_dist(L, u, v), ",", grove_at(L, u, v), ",", pd ? js::num(pond_dist(*pd, u, v)) : std::string("-"), ":", s.mat, "/",
                             s.dz, "/", s.water ? "1" : "0"));
    }
    for (size_t q = 0; q < recs.size(); q += 12) {
      std::string s;
      for (size_t n = q; n < std::min(q + 12, recs.size()); ++n) s += (n > q ? " " : "") + recs[n];
      out << (Line() << "pts" << s);
    }
  }
  CHECK(rec::record("parks", out.text()) == rec::recorded_digest("parks"));
}
