// The city generator's road network as the game's RoadNetwork (svx/procgen/city_roads.hpp,
// docs/VEHICLES.md): the contract the traffic and the pedestrians rely on, on the infinite city as
// the export makes it (svx_city make_world) - lanes in a box are those running through it, in a
// stable order; every lane near the spawn leads on; walks join at their corners; crossings close
// and open with the signals, holding the traffic they cross; parking places at lanes' kerbsides;
// the same answers from several threads, in any order.
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "svx/city/world.hpp"
#include "svx/procgen/city_roads.hpp"

using namespace svx;

namespace {

std::shared_ptr<const city::World> city_world(const char* preset = "infiniteCity") {
  static std::map<std::string, std::shared_ptr<const city::World>> worlds;
  auto& w = worlds[preset];
  if (!w) {
    city::WorldSpec spec;
    spec.preset = preset;
    w = city::make_world(spec);
  }
  return w;
}

f64 flat_len(const V3& v) { return std::hypot(v.x, v.y); }

// A lane's (a walk's) bounds meet a box (x, y)?
bool meets(const V3& a, const V3& b, const V3& lo, const V3& hi) {
  return std::max(a.x, b.x) >= lo.x && std::min(a.x, b.x) <= hi.x && std::max(a.y, b.y) >= lo.y && std::min(a.y, b.y) <= hi.y;
}

// The distance (x, y) from p to segment a-b.
f64 seg_dist(const V3& p, const V3& a, const V3& b) {
  const f64 dx = b.x - a.x, dy = b.y - a.y;
  const f64 l2 = dx * dx + dy * dy;
  const f64 t = l2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0, 1.0) : 0.0;
  return std::hypot(p.x - (a.x + dx * t), p.y - (a.y + dy * t));
}

// Everything a box's traffic and people read, as text: its lanes and their ways on, its walks and
// theirs, its parking places.
std::string box_answers(const RoadNetwork& r, const V3& lo, const V3& hi) {
  std::string s;
  std::vector<Lane> lanes;
  r.lanes_in(lo, hi, lanes);
  for (const Lane& l : lanes) {
    s += std::to_string(l.id) + " " + std::to_string(l.a.x) + " " + std::to_string(l.b.y) + " " + std::to_string(l.a.z) + ":";
    std::vector<std::pair<u64, int>> next;
    r.next(l.id, next);
    for (const auto& [id, turn] : next) {
      Lane n;
      s += " " + std::to_string(id) + "/" + std::to_string(turn) + (r.lane(id, &n) ? "+" : "-");
    }
    s += r.green(l.id, 3.5) ? " g\n" : " r\n";
  }
  std::vector<Walk> walks;
  r.walks_in(lo, hi, walks);
  for (const Walk& w : walks) {
    s += std::to_string(w.id) + " " + std::to_string(w.a.y) + " " + std::to_string(w.inset.x) + (w.crossing ? " x" : " -") + ":";
    for (int end = 0; end < 2; ++end) {
      std::vector<std::pair<u64, int>> next;
      r.walk_next(w.id, end, next);
      for (const auto& [id, e] : next) s += " " + std::to_string(id) + "/" + std::to_string(e);
      s += " |";
    }
    s += r.walk_open(w.id, 20.25) ? " o\n" : " c\n";
  }
  std::vector<ParkingSpot> spots;
  r.parking_in(lo, hi, spots);
  for (const ParkingSpot& p : spots) s += std::to_string(p.id) + " " + std::to_string(p.pos.x) + " " + std::to_string(p.yaw) + "\n";
  return s;
}

}  // namespace

TEST_CASE("city roads: the lanes in a box are those running through it, in a stable order") {
  const CityRoadNetwork roads(city_world());
  const V3 lo{-150, -150, 0}, hi{150, 150, 0};
  std::vector<Lane> lanes;
  roads.lanes_in(lo, hi, lanes);
  MESSAGE(lanes.size() << " lanes within 150 m of the spawn");
  REQUIRE(lanes.size() > 150);
  for (size_t i = 0; i < lanes.size(); ++i) {
    CHECK(meets(lanes[i].a, lanes[i].b, lo, hi));
    if (i) CHECK(lanes[i - 1].id < lanes[i].id);  // (in id order)
    CHECK(lanes[i].width > 2.0);
    CHECK(lanes[i].speed > 5.0);
    CHECK(flat_len(lanes[i].b - lanes[i].a) > 0.5);
  }
  // (asked again, or by another network over the world: the same)
  std::vector<Lane> again;
  roads.lanes_in(lo, hi, again);
  const CityRoadNetwork other(city_world());
  std::vector<Lane> theirs;
  other.lanes_in(lo, hi, theirs);
  REQUIRE(again.size() == lanes.size());
  REQUIRE(theirs.size() == lanes.size());
  for (size_t i = 0; i < lanes.size(); ++i) {
    CHECK(again[i].id == lanes[i].id);
    CHECK((theirs[i].id == lanes[i].id && theirs[i].a.x == lanes[i].a.x && theirs[i].b.y == lanes[i].b.y && theirs[i].b.z == lanes[i].b.z));
  }
  // a smaller box: exactly the lanes of the big one that meet it
  const V3 slo{-40, -70, 0}, shi{60, 10, 0};
  std::vector<Lane> small;
  roads.lanes_in(slo, shi, small);
  std::vector<u64> want;
  for (const Lane& l : lanes)
    if (meets(l.a, l.b, slo, shi)) want.push_back(l.id);
  std::vector<u64> got;
  for (const Lane& l : small) got.push_back(l.id);
  CHECK(got == want);
  // (a lane by its id: the record lanes_in gave)
  for (const Lane& l : small) {
    Lane back;
    REQUIRE(roads.lane(l.id, &back));
    CHECK((back.a.x == l.a.x && back.a.y == l.a.y && back.b.z == l.b.z && back.width == l.width && back.speed == l.speed));
  }
}

TEST_CASE("city roads: every lane within 250 m of the spawn leads on, to a lane starting at its junction") {
  for (const char* preset : {"infiniteCity", "angledOldHarbourTown"}) {
    const CityRoadNetwork roads(city_world(preset));
    std::vector<Lane> lanes;
    roads.lanes_in(V3{-250, -250, 0}, V3{250, 250, 0}, lanes);
    REQUIRE(lanes.size() > 300);
    i32 nowhere = 0, unknown = 0, far = 0, turns = 0;
    for (const Lane& l : lanes) {
      std::vector<std::pair<u64, int>> next;
      roads.next(l.id, next);
      if (next.empty()) ++nowhere;
      for (const auto& [nid, turn] : next) {
        Lane n;
        if (!roads.lane(nid, &n)) {
          ++unknown;
          continue;
        }
        // (across its junction at most: the next lane starts where it ends)
        if (flat_len(n.a - l.b) > 60.0) ++far;
        turns += turn != 0;
      }
    }
    MESSAGE(std::string(preset) << ": " << lanes.size() << " lanes, " << turns << " turns");
    CHECK(nowhere == 0);
    CHECK(unknown == 0);
    CHECK(far == 0);
    CHECK(turns > 100);
  }
}

TEST_CASE("city roads: walks join at their corners, both ways") {
  const CityRoadNetwork roads(city_world());
  std::vector<Walk> walks;
  roads.walks_in(V3{-200, -200, 0}, V3{200, 200, 0}, walks);
  REQUIRE(walks.size() > 300);
  i32 joined = 0, one_way = 0, apart = 0, crossings = 0;
  for (const Walk& w : walks) {
    crossings += w.crossing;
    i32 ends = 0;
    for (int end = 0; end < 2; ++end) {
      std::vector<std::pair<u64, int>> next;
      roads.walk_next(w.id, end, next);
      ends += !next.empty();
      for (const auto& [oid, oend] : next) {
        std::vector<std::pair<u64, int>> back;
        roads.walk_next(oid, oend, back);
        if (std::find(back.begin(), back.end(), std::make_pair(w.id, end)) == back.end()) ++one_way;
        Walk o;
        REQUIRE(roads.walk(oid, &o));
        if (flat_len((end == 0 ? w.a : w.b) - (oend == 0 ? o.a : o.b)) > 0.5) ++apart;
      }
    }
    joined += ends == 2;
    // (people keep to the buildings' side of a sidewalk, a little: the inset)
    if (!w.crossing) CHECK(flat_len(w.inset) < 0.5 * w.width + 1e-9);
  }
  MESSAGE(walks.size() << " walks, " << crossings << " crossings, " << joined << " joined at both ends");
  CHECK(one_way == 0);
  CHECK(apart == 0);
  CHECK(joined >= static_cast<i32>(walks.size() * 0.95));
  CHECK(crossings > 30);
}

TEST_CASE("city roads: crossings close and open with the signals, holding the traffic they cross") {
  const CityRoadNetwork roads(city_world());
  const V3 lo{-500, -500, 0}, hi{500, 500, 0};
  std::vector<Walk> walks;
  roads.walks_in(lo, hi, walks);
  std::vector<Lane> lanes;
  roads.lanes_in(lo - V3{30, 30, 0}, hi + V3{30, 30, 0}, lanes);
  i32 signalled = 0, held = 0, crossed_on_green = 0, always_open = 0, holding = 0;
  for (const Walk& w : walks) {
    if (!w.crossing) continue;
    // (over a whole number of cycles of two, three and four phases: 204 s)
    i32 open = 0;
    for (f64 t = 0.25; t < 204.0; t += 0.5) open += roads.walk_open(w.id, t);
    if (open == 408) {
      ++always_open;
      continue;
    }
    CHECK(open > 0);  // (it opens, and closes)
    ++signalled;
    // the lanes of the road it crosses entering its junction: across it, ending by it
    const V3 d = w.b - w.a;
    const f64 dl = flat_len(d);
    const i32 held0 = held;
    for (const Lane& l : lanes) {
      const V3 ld = l.b - l.a;
      const f64 ll = flat_len(ld);
      if (ll < 1e-6 || std::abs((d.x * ld.x + d.y * ld.y) / (dl * ll)) > 0.3) continue;
      if (seg_dist(l.b, w.a, w.b) > 6.0) continue;
      ++held;
      for (f64 t = 0.25; t < 204.0; t += 0.5)
        if (roads.walk_open(w.id, t) && roads.green(l.id, t)) ++crossed_on_green;
    }
    holding += held > held0;
  }
  MESSAGE(signalled << " signalled crossings (" << always_open << " always open), " << holding << " holding " << held << " lanes");
  CHECK(signalled >= 20);
  CHECK(holding >= signalled * 0.9);
  CHECK(crossed_on_green == 0);
}

TEST_CASE("city roads: parking places stand at lanes' kerbsides, heading with the traffic") {
  const CityRoadNetwork roads(city_world());
  const V3 lo{-250, -250, 0}, hi{250, 250, 0};
  std::vector<ParkingSpot> spots;
  roads.parking_in(lo, hi, spots);
  REQUIRE(spots.size() > 50);
  std::vector<Lane> lanes;
  roads.lanes_in(lo - V3{60, 60, 0}, hi + V3{60, 60, 0}, lanes);
  i32 kerbside = 0;
  for (size_t i = 0; i < spots.size(); ++i) {
    const ParkingSpot& s = spots[i];
    CHECK((s.pos.x >= lo.x && s.pos.x <= hi.x && s.pos.y >= lo.y && s.pos.y <= hi.y));
    if (i) CHECK(spots[i - 1].id < s.id);
    const V3 h{std::cos(s.yaw), std::sin(s.yaw), 0.0};
    // (a lane heading its way, the place to its right - structvox's right of (dx, dy) is (dy, -dx) -
    // a few metres off its centre line, beside it)
    for (const Lane& l : lanes) {
      const V3 ld = l.b - l.a;
      const f64 len = flat_len(ld);
      if (len < 1e-6) continue;
      const V3 u{ld.x / len, ld.y / len, 0.0};
      if (u.x * h.x + u.y * h.y < 0.99) continue;
      const V3 p = s.pos - l.a;
      const f64 along = p.x * u.x + p.y * u.y;
      const f64 right = p.x * u.y - p.y * u.x;
      if (along < -6.0 || along > len + 6.0 || right < 1.0 || right > 4.5 || std::abs(s.pos.z - (l.a.z + l.b.z) / 2) > 1.0) continue;
      ++kerbside;
      break;
    }
  }
  MESSAGE(kerbside << " of " << spots.size() << " parking places beside a lane");
  CHECK(kerbside == static_cast<i32>(spots.size()));
}

TEST_CASE("city roads: the same answers from several threads, in any order") {
  const std::vector<std::pair<V3, V3>> boxes = {
      {V3{-120, -120, 0}, V3{0, 0, 0}}, {V3{0, -120, 0}, V3{120, 0, 0}}, {V3{-120, 0, 0}, V3{0, 120, 0}}, {V3{0, 0, 0}, V3{120, 120, 0}},
      {V3{-60, -60, 0}, V3{60, 60, 0}}, {V3{600, -900, 0}, V3{760, -740, 0}},  // (a highway's ramps)
  };
  std::vector<std::string> expect;
  {
    const CityRoadNetwork lone(city_world());
    for (const auto& [lo, hi] : boxes) expect.push_back(box_answers(lone, lo, hi));
  }
  for (const std::string& e : expect) CHECK(std::count(e.begin(), e.end(), '\n') > 20);
  // (one network, keeping few roads made: asked by four threads, each in its own order)
  const CityRoadNetwork shared(city_world(), city::RoadNetworkOptions{96, 2, 1 << 16, 1 << 12});
  std::vector<std::vector<std::string>> got(4, std::vector<std::string>(boxes.size()));
  std::vector<std::thread> threads;
  for (size_t t = 0; t < 4; ++t)
    threads.emplace_back([&, t] {
      for (size_t k = 0; k < boxes.size(); ++k) {
        const size_t b = t % 2 ? (k * 5 + t) % boxes.size() : (boxes.size() - 1 - k + t) % boxes.size();
        got[t][b] = box_answers(shared, boxes[b].first, boxes[b].second);
      }
    });
  for (std::thread& th : threads) th.join();
  for (size_t t = 0; t < got.size(); ++t)
    for (size_t b = 0; b < boxes.size(); ++b) CHECK_MESSAGE(got[t][b] == expect[b], "thread ", t, ", box ", b);
}
