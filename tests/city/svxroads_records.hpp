// svx_city tests — the records of the road network's stage "svxroads" (tools/procgen_ref/stages/
// svxroads.mjs is the Node twin): the worlds as the export makes them, the boxes round the spawn, a
// town, a village, highway ramps and a highway junction, and the lines of lanes, walks and parking
// places.
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/js.hpp"
#include "core/math.hpp"
#include "network/highways.hpp"
#include "network/roadView.hpp"
#include "records.hpp"
#include "svx/city/roads.hpp"
#include "svx/city/world.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"

namespace svx::city::test {

struct RoadWorld {
  const char* key;
  const char* preset;
  const char* size;  // "" its default
};
// The worlds (svxroads.mjs ROAD_WORLDS).
inline const std::vector<RoadWorld>& road_worlds() {
  static const std::vector<RoadWorld> w = {
      {"infiniteCity", "infiniteCity", ""},     {"angledInfiniteCity", "angledInfiniteCity", ""},     {"cities", "cities", ""},
      {"oldHarbourTown", "oldHarbourTown", ""}, {"angledOldHarbourTown", "angledOldHarbourTown", ""}, {"island", "island", ""},
  };
  return w;
}

// A world as the export makes it (svx/city/world.hpp make_world: makeConfig, the parts apart).
inline std::shared_ptr<World> export_world(const std::string& preset, const std::string& size = "") {
  WorldSpec s;
  s.preset = preset;
  s.size = size;
  std::string err;
  std::shared_ptr<World> w = make_world(s, &err);
  if (!w) SVX_FAIL("svxroads: no such world");
  return w;
}

inline double to_m(double q) { return kVoxelSize * (q - 0.5); }

struct RoadBox {
  std::string tag;
  RoadNetwork::Vec2 lo, hi;
};
// The boxes of a world (svxroads.mjs boxesOf): round the spawn (500 m), round the first town and
// the first village whose centre lies over 400 m from the spawn (240 m), round the landings of the
// first two ramps within 1.5 km of the spawn (160 m), round the highway junction or terminus
// nearest the spawn (200 m).
inline std::vector<RoadBox> road_boxes(const World& w) {
  std::vector<RoadBox> out{{"spawn", {-250, -250}, {250, 250}}};
  const MacroFields& F = *w.fields;
  auto far = [](const Settlement* s) { return js::hypot(to_m(s->x), to_m(s->y)) > 400; };
  for (const Settlement* s : F.settlements_in({-160000, -160000, 160000, 160000}))
    if (far(s)) {
      out.push_back({"town", {to_m(s->x) - 120, to_m(s->y) - 120}, {to_m(s->x) + 120, to_m(s->y) + 120}});
      break;
    }
  for (const Settlement* s : F.villages_in({-80000, -80000, 80000, 80000}))
    if (far(s)) {
      out.push_back({"village", {to_m(s->x) - 120, to_m(s->y) - 120}, {to_m(s->x) + 120, to_m(s->y) + 120}});
      break;
    }
  if (w.highways) {
    int n = 0;
    for (const HighwayEdgePtr& e : w.highways->edges_near({-12000, -12000, 12000, 12000}))
      for (const HighwayRamp& r : w.highways->ramps(*e)) {
        if (n >= 2 || js::abs(r.x) >= 12000 || js::abs(r.y) >= 12000) continue;
        out.push_back({"ramp", {to_m(r.x) - 80, to_m(r.y) - 80}, {to_m(r.x) + 80, to_m(r.y) + 80}});
        n += 1;
      }
    // (the lattice node nearest the spawn where other than two highways meet: a junction, a terminus)
    bool found = false;
    double best_d = 0, best_x = 0, best_y = 0;
    for (double b = -3; b <= 3; b += 1)
      for (double a = -3; a <= 3; a += 1) {
        const size_t deg = w.highways->edges_at(a, b).size();
        if (deg == 0 || deg == 2) continue;
        const HighwayNode p = w.highways->node(a, b);
        const double d = js::hypot(p.x, p.y);
        if (!found || d < best_d) {
          found = true;
          best_d = d;
          best_x = p.x;
          best_y = p.y;
        }
      }
    if (found) out.push_back({"node", {to_m(best_x) - 100, to_m(best_y) - 100}, {to_m(best_x) + 100, to_m(best_y) + 100}});
  }
  return out;
}

// ---- fields as svxroads.mjs prints them (rec.mjs f)

template <size_t N>
std::string farr(const std::array<double, N>& a) {
  std::string s = "[";
  for (size_t i = 0; i < N; ++i) s += (i ? "," : "") + js::num(a[i]);
  return s + "]";
}
inline std::string fturns(const std::vector<RoadTurn>& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); ++i) s += js::cat(i ? "," : "", "[", static_cast<double>(v[i].first), ",", v[i].second, "]");
  return s + "]";
}
inline std::string fsig(const std::optional<RoadSignal>& s) {
  return s ? js::cat("[", s->cycle, ",", s->offset, ",", s->from, ",", s->to, "]") : std::string("-");
}
// Whether open(t) over a signal's cycle (every 0.5 s, from 0.25 s), then at -7.3 s and 123456.7 s.
template <class F>
std::string fbits(const F& open, double cycle) {
  std::string s;
  for (double k = 0; k < 2 * cycle; k += 1) s += open(k * 0.5 + 0.25) ? '1' : '0';
  s += '/';
  s += open(-7.3) ? '1' : '0';
  s += open(123456.7) ? '1' : '0';
  return s;
}

// A lane: its id, ends, width, speed and key (svxroads.mjs laneFields).
inline void lane_fields(rec::Line& l, const RoadLane& lane) {
  l << lane.id << farr(lane.a) << farr(lane.b) << lane.width << lane.speed;
  const RoadLaneKey& k = lane.key;
  switch (k.kind) {
    case RoadLaneKey::Kind::Street:
      l << "s" << k.road << k.link << k.piece << k.dir << k.k << k.last;
      break;
    case RoadLaneKey::Kind::Ramp:
      l << "r" << k.hw << k.ramp << k.off << k.piece << k.last << k.s_deck << k.side << k.arterial;
      break;
    case RoadLaneKey::Kind::Deck:
      l << "h" << k.hw << k.dir << k.link << k.piece << k.k << k.last << k.sa << k.sb;
      break;
  }
}

// The answers for an id (svxroads.mjs probe), asked in its order.
inline rec::Line road_probe(const RoadNetwork& net, const char* tag, uint64_t id) {
  const bool lane = net.lane(id).has_value();
  const std::vector<RoadTurn> next = net.next(id);
  const std::optional<RoadSignal> sig = net.signal(id);
  const bool green = net.green(id, 3);
  const bool walk = net.walk(id).has_value();
  const std::vector<RoadTurn> n0 = net.walk_next(id, 0);
  const std::vector<RoadTurn> n1 = net.walk_next(id, 1);
  const bool open = net.walk_open(id, 3);
  rec::Line l;
  l << tag << id << (lane ? 1 : 0) << fturns(next) << fsig(sig) << green << (walk ? 1 : 0) << fturns(n0) << fturns(n1) << open;
  return l;
}

// A box's records (svxroads.mjs boxRecords), asked in its order.
inline void road_box_records(rec::Out& out, const RoadNetwork& net, const RoadBox& b) {
  out << (rec::Line() << "box" << b.tag << farr(b.lo) << farr(b.hi));
  const std::vector<RoadLane> lanes = net.lanes_in(b.lo, b.hi);
  out << (rec::Line() << "lanes" << lanes.size());
  for (const RoadLane& l : lanes) {
    rec::Line line;
    line << "lane";
    lane_fields(line, l);
    out << line;
  }
  for (const RoadLane& l : lanes) {
    const std::vector<RoadTurn> next = net.next(l.id);
    const std::optional<RoadSignal> sig = net.signal(l.id);
    const std::string bits = sig ? fbits([&](double t) { return net.green(l.id, t); }, sig->cycle) : std::string("-");
    out << (rec::Line() << "next" << l.id << fturns(next) << fsig(sig) << bits);
    for (const RoadTurn& t : next) {
      const std::optional<RoadLane> n = net.lane(t.first);
      out << (rec::Line() << "to" << t.first << (n ? farr(n->a) : std::string("-")) << (n ? farr(n->b) : std::string("-")));
    }
  }
  const std::vector<RoadWalk> walks = net.walks_in(b.lo, b.hi);
  out << (rec::Line() << "walks" << walks.size());
  for (const RoadWalk& q : walks)
    out << (rec::Line() << "walk" << q.id << farr(q.a) << farr(q.b) << farr(q.inset) << q.width << q.crossing << walk_kind_name(q.kind) << fsig(q.signal));
  for (const RoadWalk& q : walks) {
    const std::vector<RoadTurn> n0 = net.walk_next(q.id, 0);
    const std::vector<RoadTurn> n1 = net.walk_next(q.id, 1);
    const std::string bits = q.signal ? fbits([&](double t) { return net.walk_open(q.id, t); }, q.signal->cycle) : std::string("-");
    out << (rec::Line() << "wnext" << q.id << fturns(n0) << fturns(n1) << bits);
    std::vector<RoadTurn> both = n0;
    both.insert(both.end(), n1.begin(), n1.end());
    for (const RoadTurn& t : both) {
      const std::optional<RoadWalk> o = net.walk(t.first);
      out << (rec::Line() << "wto" << t.first << (o ? farr(o->a) : std::string("-")) << (o ? farr(o->b) : std::string("-"))
                          << (o ? std::string(walk_kind_name(o->kind)) : std::string("-")));
    }
  }
  const std::vector<RoadParking> park = net.parking_in(b.lo, b.hi);
  out << (rec::Line() << "parks" << park.size());
  for (const RoadParking& p : park) out << (rec::Line() << "park" << p.id << farr(p.pos) << farr(p.heading));
}

}  // namespace svx::city::test
