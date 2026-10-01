// svx_city tests — the ground surfaces of open spaces and lots (voxel_city city/landscape.js, with
// city/parks.js and city/industry.js) against the reference (stage "landscape"), and their purity:
// spaces shared by four threads give what fresh ones give alone (their park layouts, frames and
// industry layouts made on first use by whichever thread asks).
#include <doctest.h>

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "city/landscape.hpp"
#include "city/parks.hpp"
#include "city/space.hpp"
#include "city_records.hpp"
#include "records.hpp"

using namespace svx::city;
using rec::Line;

namespace {

// stages/landscape.mjs KINDS, DISTRICTS, FRONTS, ARCHS, CIVICS, SEEDS
const char* const kKinds[16] = {"plaza",   "quay",    "parking",   "sports",   "courtyard",     "square",    "garden", "cemetery",
                                "allotments", "garages", "wasteland", "tankFarm", "containerYard", "riverside", "park",   "meadow"};
const char* const kDistricts[4] = {"residential", "microdistrict", "oldtown", "harbour"};
const char kFronts[5] = {'N', 'E', 'S', 'W', 0};
const char kWSides[4] = {'N', 'E', 'S', 'W'};
const char* const kArchs[17] = {"house",     "rowhouse", "walkup",  "midrise", "panelSlab", "panelTower", "office",    "tower", "warehouse",
                                "barn",      "factory",  "garage",  "school",  "townhouse", "wharfhouse", "cabin",     "church"};
const char* const kCivics[18] = {"supermarket", "departmentStore", "petrolStation", "marketHall", "concertHall", "houseOfCulture", "musicClub", "cinema", "hospital",
                                 "polyclinic",  "policeStation",   "fireStation",   "museum",     "artGallery",  "library",        "townHall",  "hotel",  "bathhouse"};
const double kSeeds[5] = {1337, 7, -12345, 99, 2024};

using XYs = std::vector<std::array<double, 2>>;

// a canonical texture position's lap offset, drawn from r (stages/landscape.mjs lap)
double lap(rec::Samples& r) { return r() < 0.3 ? std::floor((r() - 0.5) * 8) * 25600 : 0; }

std::string sample_rec(double x, double y, double lx, double ly, const SpaceSample& s) {
  return js::cat(x, ",", y, ",", lx, ",", ly, ":", s.mat, "/", s.dz, "/", s.water ? "1" : "0");
}

void chunked(rec::Out& out, const char* tag, const std::vector<std::string>& recs, size_t n) {
  for (size_t q = 0; q < recs.size(); q += n) {
    std::string s;
    for (size_t m = q; m < std::min(q + n, recs.size()); ++m) s += (m > q ? " " : "") + recs[m];
    out << (Line() << tag << s);
  }
}

// The k-th open space of the stage, drawn from r, and its columns.
std::unique_ptr<OpenSpace> make_space(int k, rec::Samples& r, XYs& pts) {
  auto space = std::make_unique<OpenSpace>();
  const double i = std::floor((r() - 0.5) * 100);
  const double j = std::floor((r() - 0.5) * 100);
  const double x0 = std::floor((r() - 0.5) * 600000);
  const double y0 = std::floor((r() - 0.5) * 600000);
  const double w = 8 + std::floor(r() * 1200);
  const double h = 8 + std::floor(r() * 1200);
  std::string district = "projects";
  if (!(r() < 0.4)) district = kDistricts[static_cast<size_t>(std::floor(r() * 4))];
  const char front = kFronts[static_cast<size_t>(std::floor(r() * 5))];
  space->id = js::cat("C", i, "_", j, "/b", k, "/o");
  space->kind = kKinds[k % 16];
  space->district = district;
  space->rect = {x0, y0, x0 + w, y0 + h};
  space->front = front;
  for (int q = 0; q < 100; ++q) {
    const double x = x0 - 8 + std::floor(r() * (w + 17));
    const double y = y0 - 8 + std::floor(r() * (h + 17));
    pts.push_back({x, y});
  }
  for (int q = 0; q < 24; ++q) {
    const double x = x0 + r() * w;
    const double y = y0 + r() * h;
    pts.push_back({x, y});
  }
  for (int q = 0; q < 24; ++q) {
    const double x = js::round(x0 + w / 2 + (r() - 0.5) * 80);
    const double y = js::round(y0 + h / 2 + (r() - 0.5) * 80);
    pts.push_back({x, y});
  }
  for (int q = 0; q < 24; ++q) {
    const double t = std::floor(r() * 4);
    const double a = std::floor(r() * 24);
    const double along = r();
    const double x = t == 0 ? x0 + a : t == 1 ? x0 + w - a : x0 + std::floor(along * w);
    const double y = t == 2 ? y0 + a : t == 3 ? y0 + h - a : y0 + std::floor(along * h);
    pts.push_back({x, y});
  }
  return space;
}

}  // namespace

TEST_CASE("city landscape: ground surfaces conform to the reference (stage landscape)") {
  rec::Samples r(41);
  rec::Out out;
  out << (Line() << "allot" << kAllot.w << kAllot.d << kAllot.path);
  CHECK(kAllot.w == vx(8));
  CHECK(kAllot.d == vx(11));
  CHECK(kAllot.path == vx(1.5));
  // open spaces
  for (int k = 0; k < 352; ++k) {
    XYs pts;
    const std::unique_ptr<OpenSpace> space = make_space(k, r, pts);
    Line head;
    head << "space" << k << space->id << space->kind << space->district;
    if (space->front)
      head << space->front;
    else
      head << rec::kUndef;
    out << (head << test::frect(space->rect));
    std::vector<std::string> recs;
    for (const auto& p : pts) {
      const double lx = lap(r);
      const double ly = lap(r);
      SpaceSample s;
      space_surface(*space, p[0], p[1], s, p[0] + lx, p[1] + ly);
      recs.push_back(sample_rec(p[0], p[1], lx, ly, s));
    }
    chunked(out, "ss", recs, 16);
  }
  // lots: synthetic envelopes
  for (int k = 0; k < 360; ++k) {
    std::string civic;
    if (k % 3 == 2) civic = kCivics[static_cast<size_t>(std::floor(r() * 18))];
    const std::string arch = !civic.empty() ? civic : std::string(kArchs[static_cast<size_t>(std::floor(r() * 17))]);
    const char front = kWSides[static_cast<size_t>(std::floor(r() * 4))];
    const double x0 = std::floor((r() - 0.5) * 600000);
    const double y0 = std::floor((r() - 0.5) * 600000);
    const bool turned = r() < 0.25;
    Frame frame;
    std::optional<Turn> turn;
    if (turned) {
      Turn t;
      t.yaw = static_cast<int>(std::floor(r() * 132));
      t.origin = {x0, y0};
      t.ou = std::floor((r() - 0.5) * 40);
      t.ov = std::floor((r() - 0.5) * 40);
      const double U = 40 + std::floor(r() * 400);
      const double V = 40 + std::floor(r() * 300);
      turn = t;
      frame = turned_frame(t, U, V, front);
    } else {
      const double x1 = x0 + 39 + std::floor(r() * 400);
      const double y1 = y0 + 39 + std::floor(r() * 300);
      frame = Frame(Rect{x0, y0, x1, y1}, front);
    }
    const double U = frame.U;
    const double V = frame.V;
    LotEnv env;
    {
      const double a = std::floor(r() * U * 0.3);
      const double b = std::floor(r() * V * 0.3);
      env.ground.push_back({0, 0, U - 1 - a, V - 1 - b});
    }
    if (r() < 0.4) {
      const double a = std::floor(r() * U * 0.5);
      const double b = std::floor(r() * V * 0.5);
      env.ground.push_back({a, b, U - 1, V - 1});
    }
    const int na = static_cast<int>(std::floor(r() * 3));
    for (int q = 0; q < na; ++q) {
      LotEnvAnnex a;
      a.kind = r() < 0.6 ? "garage" : "canopy";
      const double ax = std::floor((r() - 0.3) * U);
      const double ay = std::floor((r() - 0.2) * V);
      const double ax1 = ax + 20 + std::floor(r() * 40);
      const double ay1 = ay + 30 + std::floor(r() * 40);
      const Rect rect{ax, ay, ax1, ay1};
      if (turned) a.canon = rect;
      a.world = frame.rect_to_world(rect);
      env.annexes.push_back(a);
    }
    if (r() < 0.8) env.entrance_u = std::floor(r() * U);
    const double seed = kSeeds[k % 5];
    env.frame = frame;
    env.U = U;
    env.V = V;
    env.archetype = arch;
    env.R = frame.R;
    env.civic = civic;
    std::string ground;
    for (size_t q = 0; q < env.ground.size(); ++q) ground += (q ? ";" : "") + test::frect(env.ground[q]);
    std::string annexes;
    for (size_t q = 0; q < env.annexes.size(); ++q) {
      const LotEnvAnnex& a = env.annexes[q];
      annexes += js::cat(q ? ";" : "", a.kind, "/", a.canon ? test::frect(*a.canon) : std::string("-"), "/", test::frect(a.world));
    }
    Line head;
    head << "env" << k << arch << test::fo(civic) << front << test::frect(env.R) << U << V
         << (turn ? js::cat(turn->yaw, ",", turn->origin.x, ",", turn->origin.y, ",", turn->ou, ",", turn->ov) : std::string("-"));
    if (env.entrance_u)
      head << *env.entrance_u;
    else
      head << rec::kUndef;
    out << (head << seed << ground << annexes);
    // columns round the building, then cells of its own frame
    const Rect& R = env.R;
    XYs cols;
    for (int q = 0; q < 120; ++q) {
      const double x = R.x0 - 80 + std::floor(r() * (R.x1 - R.x0 + 160));
      const double y = R.y0 - 80 + std::floor(r() * (R.y1 - R.y0 + 160));
      cols.push_back({x, y});
    }
    for (int q = 0; q < 60; ++q) {
      const double u = std::floor(-60 + r() * (U + 120));
      const double v = q < 48 ? std::floor(-100 + r() * (V + 300)) : q < 54 ? -96 : V + 92;
      cols.push_back(frame.to_world(u, v));
    }
    std::vector<std::string> recs;
    for (const auto& c : cols) {
      const double lx = lap(r);
      const double ly = lap(r);
      recs.push_back(js::cat(c[0], ",", c[1], ",", lx, ",", ly, ":", lot_surface(&env, c[0], c[1], seed, c[0] + lx, c[1] + ly)));
    }
    chunked(out, "ls", recs, 20);
  }
  // a lot without a building
  for (int k = 0; k < 40; ++k) {
    const double seed = kSeeds[k % 5];
    std::string recs;
    for (int q = 0; q < 40; ++q) {
      const double x = std::floor((r() - 0.5) * 600000);
      const double y = std::floor((r() - 0.5) * 600000);
      const double lx = lap(r);
      recs += js::cat(q ? " " : "", x, ",", y, ",", lx, ":", lot_surface(nullptr, x, y, seed, x + lx, y - lx));
    }
    out << (Line() << "bare" << seed << recs);
  }
  // cobbles
  std::vector<std::string> recs;
  for (int q = 0; q < 400; ++q) {
    const double hx = (r() - 0.5) * 600000;
    const double hy = (r() - 0.5) * 600000;
    const double x = q % 2 ? hx : std::floor(hx);
    const double y = q % 2 ? hy : std::floor(hy);
    recs.push_back(js::cat(x, ",", y, ":", cobble(x, y)));
  }
  chunked(out, "cobble", recs, 40);
  CHECK(rec::record("landscape", out.text()) == rec::recorded_digest("landscape"));
}

TEST_CASE("city landscape: open spaces are the same from several threads at once") {
  // the stage's spaces, sampled alone (each made fresh), then shared by four threads, each in an
  // order of its own (the park layouts, space frames and industry layouts made by whichever asks)
  rec::Samples r(41);
  std::vector<std::unique_ptr<OpenSpace>> spaces;
  std::vector<XYs> pts;
  for (int k = 0; k < 96; ++k) {
    XYs p;
    spaces.push_back(make_space(k, r, p));
    pts.push_back(std::move(p));
  }
  auto text = [&](const OpenSpace& s, const XYs& ps) {
    std::string t;
    for (const auto& p : ps) {
      SpaceSample o;
      space_surface(s, p[0], p[1], o, p[0], p[1]);
      t += sample_rec(p[0], p[1], 0, 0, o) + " ";
    }
    return t;
  };
  std::vector<std::string> want;
  for (size_t k = 0; k < spaces.size(); ++k) {
    const OpenSpace fresh = *spaces[k];  // (a copy's lazy fields are its own, made again)
    want.push_back(text(fresh, pts[k]));
  }
  std::atomic<size_t> bad{0};
  std::vector<std::thread> threads;
  for (size_t t = 0; t < 4; ++t)
    threads.emplace_back([&, t] {
      const size_t n = spaces.size();
      for (size_t q = 0; q < n; ++q) {
        const size_t k = (q * 7 + t * 13 + (t & 1 ? n - 1 - q : 0)) % n;
        if (text(*spaces[k], pts[k]) != want[k]) bad.fetch_add(1);
      }
    });
  for (std::thread& th : threads) th.join();
  CHECK_MESSAGE(bad.load() == 0, "spaces differing on 4 threads: ", bad.load());
  // every space made each of its lazy fields once
  for (const auto& s : spaces) {
    if (s->kind == "park" || s->kind == "meadow") CHECK(s->park.ready());
    if (s->kind == "cemetery" || s->kind == "allotments") CHECK(s->frame.ready());
  }
}
