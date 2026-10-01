// svx_city tests — the city's streets, highways, walkways and parking as structvox's road network
// (svx/city/roads.hpp; voxel_city svx/roads.js, svx/highwayLanes.js): against the reference (stage
// "svxroads", on worlds as the export makes them); the reference's own checks
// (test/svxRoads.test.js), the voxels they look at standing in for what the road levels give
// until compose is ported; the plan's acceptance (no lane without a way on within 250 m of the
// spawn); what lane() and walk() remember.
#include <doctest.h>

#include <cmath>
#include <map>
#include <set>

#include "config/presets.hpp"
#include "core/value.hpp"
#include "network/highways.hpp"
#include "network/roadLevel.hpp"
#include "network/roadView.hpp"
#include "records.hpp"
#include "svx/highwayLanes.hpp"
#include "svx/ids.hpp"
#include "svxroads_records.hpp"

using namespace svx::city;
using rec::Line;

TEST_CASE("city svx roads: lanes, ways on, signals, walks, corners, parking conform (stage svxroads)") {
  rec::Out out;
  for (const test::RoadWorld& rw : test::road_worlds()) {
    const std::shared_ptr<World> w = test::export_world(rw.preset, rw.size);
    const std::vector<test::RoadBox> boxes = test::road_boxes(*w);
    const RoadNetwork net(w, RoadNetworkOptions{1 << 16, 1 << 10, 1 << 20});
    // (ids of a road at the spawn, before anything is handed out and after)
    const CellIJ c = w->cell_at(0, 0);
    const std::string road = w->road_view(c.i, c.j)->segs[0].road->id;
    const uint64_t ids[4] = {1, static_cast<uint64_t>(id_of(road, "lane", 0, 0, 0, 0)), static_cast<uint64_t>(id_of(road, "walk", 0, 1)),
                             static_cast<uint64_t>(id_of(road, "lane", 0, 0, 1, 0))};
    out << (Line() << "world" << rw.key << boxes.size() << road);
    for (const uint64_t id : ids) out << test::road_probe(net, "before", id);
    for (const test::RoadBox& b : boxes) test::road_box_records(out, net, b);
    for (const uint64_t id : ids) out << test::road_probe(net, "after", id);
    // (a region round the spawn whole, as the worker gives a host)
    const RoadRegion reg = net.region({-60, -60}, {60, 60});
    out << (Line() << "region" << reg.lanes.size() << reg.walks.size() << reg.parking.size());
    for (const RoadRegion::LaneEntry& l : reg.lanes)
      out << (Line() << "rl" << l.lane.id << test::farr(l.lane.a) << test::farr(l.lane.b) << l.lane.width << l.lane.speed << test::fturns(l.next)
                     << test::fsig(l.signal));
    for (const RoadRegion::WalkEntry& q : reg.walks)
      out << (Line() << "rw" << q.walk.id << test::farr(q.walk.a) << test::farr(q.walk.b) << test::farr(q.walk.inset) << q.walk.width << q.walk.crossing
                     << walk_kind_name(q.walk.kind) << test::fsig(q.walk.signal) << test::fturns(q.next_a) << test::fturns(q.next_b));
    for (const RoadParking& p : reg.parking) out << (Line() << "rp" << p.id << test::farr(p.pos) << test::farr(p.heading));
  }
  CHECK(rec::record("svxroads", out.text()) == rec::recorded_digest("svxroads"));
}

namespace {

constexpr double H = 0.125;
const RoadNetwork::Vec2 kLo{-250, -250};
const RoadNetwork::Vec2 kHi{250, 250};

// A world of a preset as the reference's checks make it (createWorld(presetConfig(preset))).
std::shared_ptr<World> preset_world(const char* id) { return create_world(preset_config(id)); }

// (city voxels of a point in metres)
double vox(double m) { return m / H + 0.5; }

// The voxel under a lane's middle (svxRoads.test.js matAt) waits for compose: here the lane's
// middle lies on the carriageway of the road nearest it, within two voxels of its surface.
bool on_carriageway(const World& w, const RoadLane& l) {
  const double x = vox((l.a[0] + l.b[0]) / 2);
  const double y = vox((l.a[1] + l.b[1]) / 2);
  const double z = (l.a[2] + l.b[2]) / 2;
  const CellIJ c = w.cell_at(x, y);
  const std::shared_ptr<const RoadView> view = w.road_view(c.i, c.j);
  const std::optional<RoadLevel> r = road_level_at(w, *view, x, y, 8);
  return r && r->dist <= r->seg->hc + 1 && std::abs(z - H * (js::round(r->z) + 0.5)) <= 2 * H;
}

// ... a walk's middle within a road's right-of-way, within three voxels of its surface (a sidewalk
// a kerb above it).
bool on_ground(const World& w, const RoadWalk& q) {
  const double x = vox((q.a[0] + q.b[0]) / 2);
  const double y = vox((q.a[1] + q.b[1]) / 2);
  const double z = (q.a[2] + q.b[2]) / 2;
  const CellIJ c = w.cell_at(x, y);
  const std::shared_ptr<const RoadView> view = w.road_view(c.i, c.j);
  const std::optional<RoadLevel> r = road_level_at(w, *view, x, y, 8);
  return r && r->dist <= r->seg->hr + 1 && std::abs(z - H * (js::round(r->z) + 1)) <= 3 * H;
}

// ... a deck lane's middle at the deck's level there (its arcs' middle), within two voxels.
bool on_deck(const World& w, const RoadLane& l) {
  double axis = 0, a = 0, b = 0;
  if (!parse_edge_id(l.key.hw, &axis, &a, &b)) return false;
  const HighwayEdgePtr e = w.highways->edge(static_cast<int>(axis), a, b);
  if (!e) return false;
  const double s = (l.key.sa + l.key.sb) / 2;
  const double z = point_at(*e, std::max(0.0, std::min(e->total, s))).z;
  return std::abs((l.a[2] + l.b[2]) / 2 - H * (js::round(z) + 0.5)) <= 2 * H;
}

// Do segments p-q and r-s cross?
bool crosses(const std::array<double, 3>& p, const std::array<double, 3>& q, const std::array<double, 3>& r, const std::array<double, 3>& s) {
  auto o = [](const std::array<double, 3>& a, const std::array<double, 3>& b, const std::array<double, 3>& c) {
    const double v = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    return v > 0 ? 1 : v < 0 ? -1 : 0;
  };
  return o(p, q, r) * o(p, q, s) < 0 && o(r, s, p) * o(r, s, q) < 0;
}

double dist2(const std::array<double, 3>& p, const std::array<double, 3>& q) { return std::hypot(p[0] - q[0], p[1] - q[1]); }

// (a lane's road, as the reference's checks key it: a highway's lanes have none - "undefined")
std::string road_key(const RoadLane& l) { return l.key.kind == RoadLaneKey::Kind::Street ? l.key.road : std::string("undefined"); }

}  // namespace

TEST_CASE("city svx roads: lanes one way on the carriageway, right of their road in structvox's frame, every one with a way on (svxRoads.test.js)") {
  for (const char* preset : {"cities", "angledOldHarbourTown"}) {
    const std::shared_ptr<World> w = preset_world(preset);
    const RoadNetwork net(w);
    const std::vector<RoadLane> lanes = net.lanes_in(kLo, kHi);
    CHECK_MESSAGE(lanes.size() > 300, preset, ": ", lanes.size(), " lanes");
    int on_road = 0, checked = 0, onto = 0, nowhere = 0, unknown = 0, wrong_turn = 0, back_beside = 0;
    for (const RoadLane& l : lanes) {
      // (a way on: the next piece, a turn, or at a dead end back the way it came)
      const std::vector<RoadTurn> next = net.next(l.id);
      if (next.empty()) ++nowhere;
      const double dx = l.b[0] - l.a[0];
      const double dy = l.b[1] - l.a[1];
      const double len = std::hypot(dx, dy);
      for (const auto& [nid, turn] : next) {
        const std::optional<RoadLane> n = net.lane(nid);
        if (!n) {
          ++unknown;
          continue;
        }
        const double ex = n->b[0] - n->a[0];
        const double ey = n->b[1] - n->a[1];
        const double el = std::hypot(ex, ey);
        const double cross = (dx * ey - dy * ex) / (len * el);
        const double dot = (dx * ex + dy * ey) / (len * el);
        // right: clockwise (cross < 0), left: counter-clockwise, straight on: within 30°
        if (turn == 0 && !(dot > 0.85)) ++wrong_turn;
        if (turn != 0 && dot > -0.95 && (cross > 0 ? 1 : cross < 0 ? -1 : 0) != (turn == 1 ? -1 : 1)) ++wrong_turn;
        if (turn == 0 && road_key(*n) != road_key(l)) onto += 1;
      }
      // (back the way it came only at a dead end: no other road's lane leaves near its end)
      if (next.size() == 1) {
        const std::optional<RoadLane> b = net.lane(next[0].first);
        if (b && road_key(*b) == road_key(l) && b->key.kind != RoadLaneKey::Kind::Ramp && b->key.dir != l.key.dir)
          for (const RoadLane& o : lanes)
            if (road_key(o) != road_key(l) && !(dist2(o.a, l.b) > 5)) ++back_beside;
      }
      // on the carriageway (sampled)
      if (checked < 60 && len > 10) {
        checked += 1;
        if (on_carriageway(*w, l)) on_road += 1;
      }
    }
    CHECK_MESSAGE(nowhere == 0, preset, ": ", nowhere, " lanes lead nowhere");
    CHECK(unknown == 0);
    CHECK(wrong_turn == 0);
    CHECK_MESSAGE(back_beside == 0, preset, ": ", back_beside, " lanes turn back beside another road");
    CHECK_MESSAGE(on_road >= checked * 0.9, preset, ": ", on_road, " of ", checked, " lanes on the road");
    // (straight on over junctions onto another road: the arterials, cut at every cell's border, go on)
    CHECK_MESSAGE(onto > 20, preset, ": ", onto, " straight on onto another road");
    // two-way streets: the two directions side by side, each on its right (structvox: heading
    // (dx, dy), right is (dy, -dx)) - the inner lanes of one piece of a link, one each way
    std::map<std::string, std::vector<const RoadLane*>> pairs;
    std::vector<std::string> order;
    for (const RoadLane& l : lanes) {
      if (l.key.kind == RoadLaneKey::Kind::Ramp || l.key.k != 0) continue;
      const std::string k = js::cat(road_key(l), "/", l.key.link);
      if (!pairs.count(k)) order.push_back(k);
      pairs[k].push_back(&l);
    }
    int sides = 0, left = 0;
    for (const std::string& k : order) {
      const RoadLane* p = nullptr;
      const RoadLane* q = nullptr;
      for (const RoadLane* l : pairs[k]) {
        if (!p && l->key.dir == 0 && l->key.piece == 0) p = l;
        if (!q && l->key.dir == 1 && l->key.last) q = l;
      }
      if (!p || !q) continue;
      const double hx = p->b[0] - p->a[0];
      const double hy = p->b[1] - p->a[1];
      if (hx * (q->b[0] - q->a[0]) + hy * (q->b[1] - q->a[1]) >= 0) continue;
      // (p's right is towards its kerb: away from q, which drives the other way on the other side)
      const double tx = (q->a[0] + q->b[0]) / 2 - (p->a[0] + p->b[0]) / 2;
      const double ty = (q->a[1] + q->b[1]) / 2 - (p->a[1] + p->b[1]) / 2;
      if (!(tx * hy - ty * hx < 0)) ++left;
      sides += 1;
    }
    CHECK_MESSAGE(left == 0, preset, ": ", left, " pairs keep left");
    CHECK_MESSAGE(sides > 20, preset, ": ", sides, " two-way pairs");
  }
}

TEST_CASE("city svx roads: signals never let crossing traffic go at once, every road of a junction on its cycle (svxRoads.test.js)") {
  for (const char* preset : {"cities", "angledCities"}) {
    const std::shared_ptr<World> w = preset_world(preset);
    const RoadNetwork net(w);
    // the ways straight on through the junctions (a lane's end to the next lane's start), with their signals
    struct Move {
      uint64_t id;
      std::array<double, 3> p, q;
      std::optional<RoadSignal> sig;
    };
    std::vector<Move> moves;
    for (const RoadLane& l : net.lanes_in({-500, -500}, {500, 500})) {
      if (!l.key.last) continue;
      for (const auto& [n, turn] : net.next(l.id))
        if (turn == 0) moves.push_back({l.id, l.b, net.lane(n)->a, net.signal(l.id)});
    }
    int signalled = 0, crossing = 0, mixed = 0, cycles = 0, offsets = 0, together = 0;
    for (size_t i = 0; i < moves.size(); ++i)
      for (size_t k = i + 1; k < moves.size(); ++k) {
        const Move& a = moves[i];
        const Move& b = moves[k];
        if (dist2(a.p, b.p) > 60 || !crosses(a.p, a.q, b.p, b.q)) continue;
        crossing += 1;
        // (a signalled way crosses only signalled ones, of the same cycle, never green together)
        if (!a.sig != !b.sig) ++mixed;
        if (!a.sig || !b.sig) continue;
        signalled += 1;
        if (a.sig->cycle != b.sig->cycle) ++cycles;
        if (a.sig->offset != b.sig->offset) ++offsets;
        for (double t = 0; t < a.sig->cycle; t += 0.25)
          if (net.green(a.id, t) && net.green(b.id, t)) ++together;
      }
    CHECK_MESSAGE(mixed == 0, preset, ": a signalled way crosses an unsignalled one");
    CHECK(cycles == 0);
    CHECK_MESSAGE(offsets == 0, preset, ": one junction, two offsets");
    CHECK_MESSAGE(together == 0, preset, ": crossing ways green together");
    CHECK_MESSAGE((crossing > 100 && signalled > 20), preset, ": ", crossing, " crossing ways, ", signalled, " signalled");
  }
}

TEST_CASE("city svx roads: walkways meet at their corners, down the alleys too; parking in the strips (svxRoads.test.js)") {
  const std::shared_ptr<World> w = preset_world("cities");
  const RoadNetwork net(w);
  // walkways: sidewalks on the sidewalk, a crossing now and then, the walks at an end know each other
  const std::vector<RoadWalk> walks = net.walks_in(kLo, kHi);
  size_t crossings = 0;
  for (const RoadWalk& q : walks) crossings += q.crossing ? 1 : 0;
  CHECK_MESSAGE((walks.size() > 300 && crossings > 30), walks.size(), " walks, ", crossings, " crossings");
  int paved = 0, sampled = 0, joined = 0, one_way = 0, apart = 0;
  for (const RoadWalk& wk : walks) {
    for (int end = 0; end < 2; ++end)
      for (const auto& [oid, oend] : net.walk_next(wk.id, end)) {
        // (mutual: the other walk's end lists this one's)
        const std::vector<RoadTurn> back = net.walk_next(oid, oend);
        if (std::find(back.begin(), back.end(), RoadTurn{wk.id, end}) == back.end()) ++one_way;
        // (and the two ends are the same corner)
        const std::optional<RoadWalk> o = net.walk(oid);
        REQUIRE(o);
        if (!(dist2(end == 0 ? wk.a : wk.b, oend == 0 ? o->a : o->b) < 0.5)) ++apart;
      }
    if (!net.walk_next(wk.id, 0).empty() && !net.walk_next(wk.id, 1).empty()) joined += 1;
    if (!wk.crossing && sampled < 60) {
      sampled += 1;
      if (on_ground(*w, wk)) paved += 1;
    }
  }
  CHECK_MESSAGE(one_way == 0, "walk_next both ways");
  CHECK_MESSAGE(apart == 0, "at one corner");
  CHECK_MESSAGE(joined >= walks.size() * 0.95, joined, " of ", walks.size(), " walks joined at both ends");
  CHECK_MESSAGE(paved >= sampled * 0.9, paved, " of ", sampled, " sidewalks on the ground");
  // parking: in the strips, heading with the traffic on its side (a unit vector)
  const std::vector<RoadParking> park = net.parking_in(kLo, kHi);
  CHECK(park.size() > 50);
  for (const RoadParking& p : park) CHECK(std::abs(std::hypot(p.heading[0], p.heading[1]) - 1) < 1e-9);
  // an old town's alleys and lanes: a walk down the middle, joined to the streets' sidewalks at their mouths
  const RoadNetwork old(preset_world("angledOldHarbourTown"));
  const std::vector<RoadWalk> all = old.walks_in(kLo, kHi);
  size_t middle = 0, both = 0;
  bool meets_sidewalk = false;
  for (const RoadWalk& q : all) {
    if (!old.walk_next(q.id, 0).empty() && !old.walk_next(q.id, 1).empty()) ++both;
    if (q.kind != WalkKind::Middle) continue;
    ++middle;
    for (const auto& [o, end] : old.walk_next(q.id, 0))
      if (old.walk(o)->kind == WalkKind::Sidewalk) meets_sidewalk = true;
  }
  CHECK_MESSAGE(middle > 20, middle, " walks down alleys and lanes");
  CHECK_MESSAGE(both >= all.size() * 0.9, both, " of ", all.size(), " old town walks joined at both ends");
  CHECK_MESSAGE(meets_sidewalk, "an alley's walk meets a sidewalk");
}

TEST_CASE("city svx roads: stable ids and records, whatever the query and its order (svxRoads.test.js)") {
  const RoadNetwork a(preset_world("angledCities"));
  const RoadNetwork b(preset_world("angledCities"));
  const std::vector<RoadLane> la = a.lanes_in(kLo, kHi);
  // (another network over a fresh world, asked in pieces from the other corner first)
  b.lanes_in({0, 0}, kHi);
  b.walks_in({0, 0}, kHi);
  const std::vector<RoadLane> lb = b.lanes_in(kLo, kHi);
  REQUIRE(la.size() == lb.size());
  for (size_t i = 0; i < la.size(); ++i) {
    CHECK(la[i].id == lb[i].id);
    if (i < 50) {
      CHECK(la[i].a == lb[i].a);
      CHECK(la[i].b == lb[i].b);
      CHECK(a.next(la[i].id) == b.next(lb[i].id));
    }
  }
  const std::vector<RoadWalk> wa = a.walks_in(kLo, kHi);
  const std::vector<RoadWalk> wb = b.walks_in(kLo, kHi);
  REQUIRE(wa.size() == wb.size());
  for (size_t i = 0; i < wa.size(); ++i) CHECK(wa[i].id == wb[i].id);
  // (the walks at a walk's ends and the signals: the same answers, whatever was asked before)
  int differ = 0;
  for (const RoadWalk& q : wa)
    for (int end = 0; end < 2; ++end) differ += a.walk_next(q.id, end) != b.walk_next(q.id, end);
  for (const RoadLane& l : la) {
    const std::optional<RoadSignal> sa = a.signal(l.id);
    const std::optional<RoadSignal> sb = b.signal(l.id);
    differ += sa.has_value() != sb.has_value() || (sa && (sa->cycle != sb->cycle || sa->offset != sb->offset || sa->from != sb->from || sa->to != sb->to));
  }
  CHECK(differ == 0);
  const std::vector<RoadParking> pa = a.parking_in(kLo, kHi);
  const std::vector<RoadParking> pb = b.parking_in(kLo, kHi);
  REQUIRE(pa.size() == pb.size());
  for (size_t i = 0; i < pa.size(); ++i) CHECK((pa[i].id == pb[i].id && pa[i].pos == pb[i].pos && pa[i].heading == pb[i].heading));
  for (const RoadLane& l : la) CHECK((l.id > 0 && l.id < (uint64_t{1} << 53)));
  // (what the worker would give a host of a region: every lane with its ways on)
  for (const RoadLane& l : la) CHECK(!a.next(l.id).empty());
}

TEST_CASE("city svx roads: highways - lanes on the deck, ramps off it onto their arterial and up from the street, junctions taking turns (svxRoads.test.js)") {
  Value cfg;
  REQUIRE(Value::parse_json(R"({"seed": 1337, "world": {"mode": "infiniteCity"}})", &cfg));
  const std::shared_ptr<World> w = create_world(cfg);
  const RoadNetwork net(w);
  const std::vector<RoadLane> lanes = net.lanes_in({-2500, -2500}, {2500, 2500});
  std::vector<const RoadLane*> hwl, ramps;
  for (const RoadLane& l : lanes) {
    if (l.key.kind == RoadLaneKey::Kind::Street) continue;
    hwl.push_back(&l);
    if (l.key.kind == RoadLaneKey::Kind::Ramp) ramps.push_back(&l);
  }
  CHECK_MESSAGE((hwl.size() > 500 && ramps.size() > 20), hwl.size(), " highway lanes, ", ramps.size(), " on ramps");
  int on_deck_n = 0, sampled = 0, nowhere = 0, astray = 0;
  for (const RoadLane* l : hwl) {
    const std::vector<RoadTurn> next = net.next(l->id);
    if (next.empty()) ++nowhere;
    if (sampled < 120 && l->key.kind == RoadLaneKey::Kind::Deck) {
      sampled += 1;
      if (on_deck(*w, *l)) on_deck_n += 1;
    }
    // (an off-ramp ends on its arterial: every way on is a street's lane)
    if (l->key.kind == RoadLaneKey::Kind::Ramp && l->key.off && l->key.last)
      for (const auto& [id, turn] : next) astray += net.lane(id)->key.road != l->key.arterial;
  }
  CHECK_MESSAGE(nowhere == 0, nowhere, " highway lanes lead nowhere");
  CHECK(astray == 0);
  CHECK_MESSAGE(on_deck_n >= sampled * 0.95, on_deck_n, " of ", sampled, " deck lanes on the deck");
  // every on-ramp is reached from its arterial, every off-ramp from the deck's kerb lane
  std::set<std::string> reached;
  for (const RoadLane& l : lanes)
    for (const auto& [id, turn] : net.next(l.id)) {
      const std::optional<RoadLane> n = net.lane(id);
      if (n && n->key.kind == RoadLaneKey::Kind::Ramp && n->key.piece == 0)
        reached.insert(js::cat(n->key.hw, "/", n->key.ramp, "/", l.key.kind == RoadLaneKey::Kind::Street ? "street" : "deck"));
    }
  // (of the ramps whose ends lie well inside the box: what leads to them was asked for too)
  auto inside = [](const std::array<double, 3>& p) { return std::abs(p[0]) < 2300 && std::abs(p[1]) < 2300; };
  int whole = 0, unreachable = 0;
  for (const RoadLane* q : ramps) {
    if (q->key.piece != 0 || !inside(q->a)) continue;
    bool ends_inside = false;
    for (const RoadLane* z : ramps)
      if (z->key.hw == q->key.hw && z->key.ramp == q->key.ramp && z->key.last && inside(z->b)) ends_inside = true;
    if (!ends_inside) continue;
    ++whole;
    if (!reached.count(js::cat(q->key.hw, "/", q->key.ramp, "/", q->key.off ? "deck" : "street"))) ++unreachable;
  }
  CHECK_MESSAGE(whole > 5, whole, " ramps inside");
  CHECK_MESSAGE(unreachable == 0, unreachable, " ramps unreachable");
  // junctions of highways: straight ways across one never green together
  struct Move {
    uint64_t id;
    std::array<double, 3> p, q;
    RoadSignal sig;
  };
  std::vector<Move> moves;
  for (const RoadLane* l : hwl)
    if (l->key.last && l->key.kind == RoadLaneKey::Kind::Deck)
      for (const auto& [n, turn] : net.next(l->id)) {
        const std::optional<RoadSignal> sig = net.signal(l->id);
        if (turn == 0 && sig) moves.push_back({l->id, l->b, net.lane(n)->a, *sig});
      }
  int together = 0;
  for (size_t i = 0; i < moves.size(); ++i)
    for (size_t k = i + 1; k < moves.size(); ++k) {
      const Move& a = moves[i];
      const Move& b = moves[k];
      if (!crosses(a.p, a.q, b.p, b.q)) continue;
      for (double t = 0; t < a.sig.cycle; t += 0.25)
        if (net.green(a.id, t) && net.green(b.id, t)) ++together;
    }
  CHECK_MESSAGE(together == 0, "crossing highway traffic green together");
}

TEST_CASE("city svx roads: no lane without a way on within 250 m of the spawn, in every world of the stage (the plan's acceptance)") {
  for (const test::RoadWorld& rw : test::road_worlds()) {
    const std::shared_ptr<World> w = test::export_world(rw.preset, rw.size);
    const RoadNetwork net(w);  // (the default bounds)
    const std::vector<RoadLane> lanes = net.lanes_in(kLo, kHi);
    CHECK_MESSAGE(lanes.size() > 300, rw.key, ": ", lanes.size(), " lanes");
    int nowhere = 0, unknown = 0;
    for (const RoadLane& l : lanes) {
      const std::vector<RoadTurn> next = net.next(l.id);
      if (next.empty()) ++nowhere;
      for (const RoadTurn& t : next) unknown += !net.lane(t.first);
    }
    CHECK_MESSAGE(nowhere == 0, rw.key, ": ", nowhere, " lanes without a way on");
    CHECK_MESSAGE(unknown == 0, rw.key, ": ", unknown, " ways on to lanes lane() does not know");
  }
}

TEST_CASE("city svx roads: lane() and walk() know what was handed out, the most recent roads' when bounded") {
  const std::shared_ptr<World> w = test::export_world("infiniteCity");
  const RoadNetwork::Vec2 lo{-40, -40}, hi{40, 40};
  const RoadNetwork big(w);
  const std::vector<RoadLane> ref = big.lanes_in(lo, hi);
  const std::vector<RoadWalk> ref_walks = big.walks_in(lo, hi);
  REQUIRE(!ref.empty());
  REQUIRE(!ref_walks.empty());
  const RoadNetwork net(w, RoadNetworkOptions{64, 4, 8});
  // (nothing handed out: nothing known, no way on, no signal; always green, always open)
  int answered = 0;
  for (const RoadLane& l : ref) answered += net.lane(l.id).has_value() || !net.next(l.id).empty() || net.signal(l.id).has_value() || !net.green(l.id, 5);
  for (const RoadWalk& q : ref_walks) answered += net.walk(q.id).has_value() || !net.walk_next(q.id, 0).empty() || !net.walk_open(q.id, 5);
  CHECK(answered == 0);
  // handed out: the lanes of the roads handed out last (a box's roads go in id order) are known,
  // the records another network gives
  const std::vector<RoadLane> near = net.lanes_in(lo, hi);
  REQUIRE(near.size() == ref.size());
  std::string last_road;
  for (const RoadLane& l : near)
    if (l.key.kind == RoadLaneKey::Kind::Street && l.key.road > last_road) last_road = l.key.road;
  auto known_of = [&](const std::string& road) {
    int n = 0;
    for (const RoadLane& l : near)
      if (l.key.kind == RoadLaneKey::Kind::Street && (road.empty() || l.key.road == road)) {
        const std::optional<RoadLane> k = net.lane(l.id);
        n += k && k->a == l.a && k->b == l.b && k->id == l.id;
      }
    return n;
  };
  const int known = known_of(last_road);
  CHECK(known > 0);
  for (const RoadLane& l : near)
    if (l.key.road == last_road) {
      CHECK(net.next(l.id) == big.next(l.id));
      break;
    }
  // (far away, many roads: those forgotten)
  CHECK(net.lanes_in({1500, 1500}, {1800, 1800}).size() > 20);
  CHECK(known_of("") == 0);
  // handed out again: known again
  net.lanes_in(lo, hi);
  CHECK(known_of(last_road) == known);
  // walks alike
  net.walks_in(lo, hi);
  const RoadWalk* q = nullptr;
  for (const RoadWalk& r : ref_walks)
    if (net.walk(r.id) && !big.walk_next(r.id, 0).empty()) {
      q = &r;
      break;
    }
  REQUIRE(q);
  CHECK(net.walk_next(q->id, 0) == big.walk_next(q->id, 0));
  net.walks_in({1500, 1500}, {1800, 1800});
  CHECK(!net.walk(q->id));
  CHECK(net.walk_next(q->id, 0).empty());
}

