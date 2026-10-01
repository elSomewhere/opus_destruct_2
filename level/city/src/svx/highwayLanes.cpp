// svx_city — svx/highwayLanes.hpp (voxel_city svx/highwayLanes.js).
#include "svx/highwayLanes.hpp"

#include <algorithm>
#include <cctype>

#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "svx/ids.hpp"
#include "world/World.hpp"

namespace svx::city {

namespace {

constexpr double H = kVoxelSize;
inline double m(double q) { return H * (q - 0.5); }
// Speed limits (m/s): the deck, a ramp.
constexpr double kSpeed = 27.8;
constexpr double kRampSpeed = 13.9;
// Most a lane piece's chord strays from the deck's curve (voxels) before it is cut in two, and from
// its level.
constexpr double kLateralSlack = 2;
constexpr double kProfileSlack = 1;
// The median's half-width and gap (voxels): lane 0's inner edge.
constexpr double kInner = 3;
// Signal phases (as the streets'): 17 s each, 14 green.
constexpr double kPhase = 17;
constexpr double kGreen = 14;
constexpr double kOnePhase = 0.924;
// Straight on: within this much of the heading (cos 30°: Math.cos(Math.PI / 6)).
double straight() {
  static const double s = js::cos(3.141592653589793 / 6);
  return s;
}

uint64_t to_id(double id) { return static_cast<uint64_t>(id); }

using Pieces = std::vector<std::array<double, 2>>;

// The pieces [s, s'] of arc interval [s0, s1] along a curve (offset `off`, level z(s)): straight
// within the slack.
template <class Z>
Pieces pieces(const HighwayEdge& e, double s0, double s1, double off, const Z& z) {
  Pieces out;
  auto refine = [&](auto&& self, double a, double b, int depth) -> void {
    const HighwayPoint pa = offset_at(e, a, off);
    const HighwayPoint pb = offset_at(e, b, off);
    const double za = z(a);
    const double zb = z(b);
    double worst = 0;
    double worst_z = 0;
    for (int k = 1; k < 8; ++k) {
      const double t = k / 8.0;
      const HighwayPoint p = offset_at(e, a + (b - a) * t, off);
      const double cx = pa.x + (pb.x - pa.x) * t;
      const double cy = pa.y + (pb.y - pa.y) * t;
      worst = js::max(worst, js::hypot(p.x - cx, p.y - cy));
      worst_z = js::max(worst_z, js::abs(z(a + (b - a) * t) - (za + (zb - za) * t)));
    }
    if ((worst > kLateralSlack || worst_z > kProfileSlack) && depth < 8 && js::abs(b - a) > 32) {
      self(self, a, (a + b) / 2, depth + 1);
      self(self, (a + b) / 2, b, depth + 1);
    } else {
      out.push_back({a, b});
    }
  };
  if (js::abs(s1 - s0) >= 8) refine(refine, s0, s1, 0);
  return out;
}

// (in the other direction: the pieces in reverse, each reversed)
Pieces reversed(const Pieces& ps) {
  Pieces out;
  for (size_t i = ps.size(); i-- > 0;) out.push_back({ps[i][1], ps[i][0]});
  return out;
}

// Lane end (metres) at arc s, offset off, level z.
std::array<double, 3> end_at(const HighwayEdge& e, double s, double off, double z) {
  const HighwayPoint p = offset_at(e, s, off);
  return {m(p.x), m(p.y), H * (js::round(z) + 0.5)};
}

std::array<double, 2> heading(const RoadLane& l) {
  const double dx = l.b[0] - l.a[0];
  const double dy = l.b[1] - l.a[1];
  const double d = js::or_(js::hypot(dx, dy), 1);
  return {dx / d, dy / d};
}

int turn_of(const RoadLane& from, const RoadLane& to) {
  const std::array<double, 2> h = heading(from);
  const std::array<double, 2> o = heading(to);
  const double dot = h[0] * o[0] + h[1] * o[1];
  return dot > straight() ? 0 : h[0] * o[1] - h[1] * o[0] < 0 ? 1 : -1;
}

void by_first(std::vector<RoadTurn>& v) {
  js::sort(v, [](const RoadTurn& p, const RoadTurn& q) { return static_cast<double>(p.first) - static_cast<double>(q.first); });
}

bool is_ramp(const RoadLane& l) { return l.key.kind == RoadLaneKey::Kind::Ramp; }

// -?\d+ at s[p...]: its number (Number of the digits), p past it.
bool parse_int(const std::string& s, size_t& p, double* out) {
  size_t q = p;
  if (q < s.size() && s[q] == '-') ++q;
  const size_t d = q;
  while (q < s.size() && std::isdigit(static_cast<unsigned char>(s[q]))) ++q;
  if (q == d) return false;
  *out = js::parse_number(std::string_view(s).substr(p, q - p));
  p = q;
  return true;
}

}  // namespace

bool parse_edge_id(const std::string& id, double* axis, double* a, double* b) {
  if (id.size() < 2 || id[0] != 'H' || !std::isdigit(static_cast<unsigned char>(id[1]))) return false;
  *axis = id[1] - '0';
  size_t p = 2;
  if (p >= id.size() || id[p] != '_') return false;
  ++p;
  if (!parse_int(id, p, a)) return false;
  if (p >= id.size() || id[p] != '_') return false;
  ++p;
  if (!parse_int(id, p, b)) return false;
  return p == id.size();
}

HighwayLanes::HighwayLanes(const World& world, Out out, size_t edges, size_t remembered)
    : world_(world), hw_(*world.highways), out_(std::move(out)), by_edge_(edges), index_(remembered) {
  const Value& cfg = world.config["highways"];
  n_ = cfg["lanesPerSide"].to_number();
  lane_w_ = vx(cfg["laneWidth"].to_number());
}

HighwayEdgePtr HighwayLanes::edge_by_id(const std::string& id) const {
  double axis = 0, a = 0, b = 0;
  if (!parse_edge_id(id, &axis, &a, &b)) return nullptr;
  return hw_.edge(static_cast<int>(axis), a, b);
}

std::shared_ptr<const HighwayLanes::Rec> HighwayLanes::build(const HighwayEdgePtr& ep) const {
  auto rec = std::make_shared<Rec>();
  const HighwayEdge& e = *ep;
  rec->e = ep;
  const double deg[2] = {e.nodes[0][2], e.nodes[1][2]};
  // (a junction of three or four: the lanes stop short of its plateau; elsewhere they run to the node)
  auto trim = [&](double d) { return d >= 3 ? js::round(hw_.hw * 1.4) : 0.0; };
  const double s0 = trim(deg[0]);
  const double s1 = e.total - trim(deg[1]);
  auto deck_z = [&](double s) { return point_at(e, js::max(0.0, js::min(e.total, s))).z; };
  const std::vector<HighwayRamp>& ramps = hw_.ramps(e);
  rec->ramps = &ramps;
  for (int dir = 0; dir < 2; ++dir) {
    const double side = dir == 0 ? -1 : 1;
    // (the deck ends of the ramps on that side, once each, in order)
    std::vector<double> cuts;
    for (const HighwayRamp& r : ramps)
      if (r.side == side && r.s_deck > s0 + 8 && r.s_deck < s1 - 8 && std::find(cuts.begin(), cuts.end(), r.s_deck) == cuts.end())
        cuts.push_back(r.s_deck);
    js::sort(cuts, [](double p, double q) { return p - q; });
    std::vector<double> bounds{s0};
    bounds.insert(bounds.end(), cuts.begin(), cuts.end());
    bounds.push_back(s1);
    std::vector<std::array<double, 2>> order;
    for (size_t k = 0; k + 1 < bounds.size(); ++k) order.push_back({bounds[k], bounds[k + 1]});
    // (in its traffic's order)
    if (dir == 1) std::reverse(order.begin(), order.end());
    for (size_t li = 0; li < order.size(); ++li) {
      const double a = order[li][0];
      const double b = order[li][1];
      rec->links[static_cast<size_t>(dir)].push_back({static_cast<double>(li), dir == 0 ? a : b, dir == 0 ? b : a});
      for (int k = 0; k < n_; ++k) {
        const double off = side * (kInner + (k + 0.5) * lane_w_);
        const Pieces ps = pieces(e, a, b, off, deck_z);
        const Pieces seq = dir == 0 ? ps : reversed(ps);
        for (size_t pi = 0; pi < seq.size(); ++pi) {
          const double sa = seq[pi][0];
          const double sb = seq[pi][1];
          RoadLane l;
          l.id = to_id(id_of("hw", e.id, dir, li, pi, k));
          l.a = end_at(e, sa, off, deck_z(sa));
          l.b = end_at(e, sb, off, deck_z(sb));
          l.width = lane_w_ * H;
          l.speed = kSpeed;
          l.key.kind = RoadLaneKey::Kind::Deck;
          l.key.hw = e.id;
          l.key.dir = dir;
          l.key.link = static_cast<int>(li);
          l.key.piece = static_cast<int>(pi);
          l.key.k = k;
          l.key.last = pi + 1 == seq.size();
          l.key.sa = sa;
          l.key.sb = sb;
          rec->lanes.push_back(std::move(l));
        }
      }
    }
  }
  // ramps: one lane each, the way its traffic goes
  for (size_t ri = 0; ri < ramps.size(); ++ri) {
    const HighwayRamp& r = ramps[ri];
    const double off = r.side * hw_.ramp_mid;
    const bool off_ramp = r.side < 0 ? r.s_deck < r.s_ground : r.s_deck > r.s_ground;
    const double from = off_ramp ? r.s_deck : r.s_ground;
    const double to = off_ramp ? r.s_ground : r.s_deck;
    auto z = [&](double s) { return ramp_z(e, r, s); };
    const Pieces ps = pieces(e, js::min(from, to), js::max(from, to), off, z);
    const Pieces seq = from < to ? ps : reversed(ps);
    for (size_t pi = 0; pi < seq.size(); ++pi) {
      const double sa = seq[pi][0];
      const double sb = seq[pi][1];
      RoadLane l;
      l.id = to_id(id_of("hwramp", e.id, ri, pi));
      l.a = end_at(e, sa, off, z(sa));
      l.b = end_at(e, sb, off, z(sb));
      l.width = vx(3.75) * H;
      l.speed = kRampSpeed;
      l.key.kind = RoadLaneKey::Kind::Ramp;
      l.key.hw = e.id;
      l.key.ramp = static_cast<int>(ri);
      l.key.off = off_ramp;
      l.key.piece = static_cast<int>(pi);
      l.key.last = pi + 1 == seq.size();
      l.key.s_deck = r.s_deck;
      l.key.side = r.side;
      l.key.arterial = r.arterial;
      rec->lanes.push_back(std::move(l));
    }
  }
  return rec;
}

std::shared_ptr<const HighwayLanes::Rec> HighwayLanes::edge_lanes(const HighwayEdgePtr& e) const {
  std::shared_ptr<const Rec> rec = by_edge_.get(e->id, [&] { return build(e); });
  index_.add(e->id, true, rec->lanes);
  return rec;
}

std::optional<HighwayLanes::Hit> HighwayLanes::hit(uint64_t id) const {
  const auto owner = index_.find(id);
  if (!owner) return std::nullopt;
  const HighwayEdgePtr e = edge_by_id(owner->first);
  if (!e) return std::nullopt;
  std::shared_ptr<const Rec> rec = edge_lanes(e);
  for (const RoadLane& l : rec->lanes)
    if (l.id == id) return Hit{rec, &l};
  return std::nullopt;
}

std::vector<std::pair<const RoadLane*, std::shared_ptr<const HighwayLanes::Rec>>> HighwayLanes::leaving_node(double a, double b) const {
  std::vector<std::pair<const RoadLane*, std::shared_ptr<const Rec>>> out;
  for (const std::array<double, 3>& ex : hw_.edges_at(a, b)) {
    const HighwayEdgePtr e = hw_.edge(static_cast<int>(ex[0]), ex[1], ex[2]);
    std::shared_ptr<const Rec> rec = edge_lanes(e);
    // (leaving the node: along the arc from its start, against it from its end)
    const bool at_start = ex[1] == a && ex[2] == b;
    const int dir = at_start ? 0 : 1;
    for (const RoadLane& l : rec->lanes)
      if (!is_ramp(l) && l.key.dir == dir && l.key.link == 0 && l.key.piece == 0) out.push_back({&l, rec});
  }
  return out;
}

const HighwayLanes::Phases& HighwayLanes::junction_phases(const Rec& rec, int end) const {
  return rec.phases[end].get([&] {
    const double a = rec.e->nodes[static_cast<size_t>(end)][0];
    const double b = rec.e->nodes[static_cast<size_t>(end)][1];
    const HighwayNode node = hw_.node(a, b);
    std::vector<std::array<double, 2>> heads;
    Phases ph;
    std::vector<HighwayEdgePtr> arms;
    for (const std::array<double, 3>& ex : hw_.edges_at(a, b)) arms.push_back(hw_.edge(static_cast<int>(ex[0]), ex[1], ex[2]));
    js::sort(arms, [](const HighwayEdgePtr& p, const HighwayEdgePtr& q) { return js::compare(p->id, q->id) < 0 ? -1 : 1; });
    for (const HighwayEdgePtr& e : arms) {
      const bool at_start = e->nodes[0][0] == a && e->nodes[0][1] == b;
      const HighwayPoint p = point_at(*e, at_start ? 0 : e->total);
      int i = -1;
      for (size_t k = 0; k < heads.size(); ++k)
        if (js::abs(heads[k][0] * p.tx + heads[k][1] * p.ty) >= kOnePhase) {
          i = static_cast<int>(k);
          break;
        }
      if (i < 0) {
        heads.push_back({p.tx, p.ty});
        i = static_cast<int>(heads.size()) - 1;
      }
      ph.edges.push_back(e->id);
      ph.phase.push_back(i);
    }
    ph.phases = static_cast<double>(heads.size());
    ph.offset = (hash32(world_.seed, js::round(node.x), js::round(node.y), 0x517) / 4294967296.0) * kPhase * ph.phases;
    return ph;
  });
}

void HighwayLanes::lanes_in(const Rect& box, const std::function<bool(const RoadLane&)>& in_box, std::vector<RoadLane>& out) const {
  for (const HighwayEdgePtr& e : hw_.edges_near(box)) {
    const std::shared_ptr<const Rec> rec = edge_lanes(e);
    for (const RoadLane& l : rec->lanes)
      if (in_box(l)) out.push_back(l);
  }
}

bool HighwayLanes::has(uint64_t id) const { return index_.has(id); }

std::optional<RoadLane> HighwayLanes::lane(uint64_t id) const {
  const std::optional<Hit> h = hit(id);
  if (!h) return std::nullopt;
  return *h->lane;
}

HighwayLanes::RampLanes HighwayLanes::ramp_lanes(const std::string& edge_id, double ri) const {
  RampLanes out;
  const HighwayEdgePtr e = edge_by_id(edge_id);
  if (!e) return out;
  const std::shared_ptr<const Rec> rec = edge_lanes(e);
  for (const RoadLane& l : rec->lanes) {
    if (!is_ramp(l) || l.key.ramp != ri) continue;
    if (!out.off) out.off = l.key.off;
    if (!out.first && l.key.piece == 0) out.first = l;
    if (!out.last && l.key.last) out.last = l;
  }
  return out;
}

std::vector<RoadTurn> HighwayLanes::next(uint64_t id) const {
  const std::optional<Hit> h = hit(id);
  if (!h) return {};
  const Rec& rec = *h->rec;
  const RoadLane& l = *h->lane;
  const RoadLaneKey& k = l.key;
  const double n = n_;
  if (is_ramp(l)) {
    if (!k.last) {
      for (const RoadLane& q : rec.lanes)
        if (is_ramp(q) && q.key.ramp == k.ramp && q.key.piece == k.piece + 1) return {{q.id, 0}};
      return {};
    }
    if (k.off) {
      // down on the arterial: its lanes leaving the landing, either way
      std::vector<RoadTurn> out;
      for (const RoadLane& q : out_(rec.e->id, k.ramp)) out.push_back({q.id, turn_of(l, q)});
      by_first(out);
      return out;
    }
    // up on the deck: the kerb lane of the link that starts where it joins
    const int dir = k.side < 0 ? 0 : 1;
    const Link* link = nullptr;
    for (const Link& q : rec.links[static_cast<size_t>(dir)])
      if (js::abs(q.from - k.s_deck) < 1) {
        link = &q;
        break;
      }
    if (link)
      for (const RoadLane& q : rec.lanes)
        if (!is_ramp(q) && q.key.dir == dir && q.key.link == link->li && q.key.piece == 0 && q.key.k == n - 1) return {{q.id, 0}};
    return {};
  }
  auto lane_of = [&](double li, auto&& pred) -> const RoadLane* {
    for (const RoadLane& q : rec.lanes)
      if (!is_ramp(q) && q.key.dir == k.dir && q.key.link == li && pred(q)) return &q;
    return nullptr;
  };
  if (!k.last) {
    const RoadLane* q = lane_of(k.link, [&](const RoadLane& r) { return r.key.piece == k.piece + 1 && r.key.k == k.k; });
    return q ? std::vector<RoadTurn>{{q->id, 0}} : std::vector<RoadTurn>{};
  }
  std::vector<RoadTurn> out;
  // (the node ahead: the end its last link runs into, else mid-edge)
  const bool at_node = k.link == static_cast<int>(rec.links[static_cast<size_t>(k.dir)].size()) - 1;
  if (!at_node) {
    // a cut: on along the next link, and (from the kerb lane) the off-ramp leaving here
    const RoadLane* nl = lane_of(k.link + 1, [&](const RoadLane& r) { return r.key.piece == 0 && r.key.k == k.k; });
    if (nl) out.push_back({nl->id, 0});
    const double side = k.dir == 0 ? -1 : 1;
    if (k.k == n - 1)
      for (size_t ri = 0; ri < rec.ramps->size(); ++ri) {
        const HighwayRamp& r = (*rec.ramps)[ri];
        const bool off_ramp = r.side < 0 ? r.s_deck < r.s_ground : r.s_deck > r.s_ground;
        if (r.side == side && off_ramp && js::abs(r.s_deck - k.sb) < 1)
          for (const RoadLane& q : rec.lanes)
            if (is_ramp(q) && q.key.ramp == static_cast<int>(ri) && q.key.piece == 0) {
              out.push_back({q.id, 1});
              break;
            }
      }
    by_first(out);
    return out;
  }
  const std::array<double, 3>& node = rec.e->nodes[k.dir == 0 ? 1 : 0];
  const double a = node[0];
  const double b = node[1];
  const double deg = node[2];
  if (deg <= 1) {
    // a terminus: back the way it came, from the inner lane
    for (const RoadLane& q : rec.lanes)
      if (!is_ramp(q) && q.key.dir != k.dir && q.key.link == 0 && q.key.piece == 0 && q.key.k == 0) return {{q.id, -1}};
    return {};
  }
  // (the ways on: each other edge's lanes leaving the node, by edge)
  std::vector<std::pair<std::string, std::vector<const RoadLane*>>> ways;
  const auto leaving = leaving_node(a, b);
  for (const auto& [q, qrec] : leaving) {
    if (qrec->e->id == rec.e->id) continue;
    auto it = std::find_if(ways.begin(), ways.end(), [&](const auto& w) { return w.first == qrec->e->id; });
    if (it == ways.end()) {
      ways.push_back({qrec->e->id, {}});
      it = ways.end() - 1;
    }
    it->second.push_back(q);
  }
  if (deg == 2)
    for (const auto& w : ways)
      for (const RoadLane* q : w.second)
        if (q->key.k == k.k) out.push_back({q->id, 0});
  if (deg >= 3) {
    bool straight_on = false;
    for (const auto& w : ways)
      if (turn_of(l, *w.second[0]) == 0) straight_on = true;
    for (const auto& w : ways) {
      const int turn = turn_of(l, *w.second[0]);
      auto by_k = [&](double kk) -> const RoadLane* {
        for (const RoadLane* q : w.second)
          if (q->key.k == kk) return q;
        return nullptr;
      };
      // (every lane of an edge's link has its first piece: JS reads them unguarded)
      const RoadLane* q = turn == 0                                         ? by_k(k.k)
                          : turn == 1 && (k.k == n - 1 || !straight_on)  ? by_k(n - 1)
                          : turn == -1 && (k.k == 0 || !straight_on)     ? by_k(0)
                                                                         : nullptr;
      if (q) out.push_back({q->id, turn});
    }
  }
  by_first(out);
  return out;
}

std::optional<RoadSignal> HighwayLanes::signal(uint64_t id) const {
  const std::optional<Hit> h = hit(id);
  if (!h || is_ramp(*h->lane) || !h->lane->key.last) return std::nullopt;
  const Rec& rec = *h->rec;
  const RoadLaneKey& k = h->lane->key;
  if (k.link != static_cast<int>(rec.links[static_cast<size_t>(k.dir)].size()) - 1) return std::nullopt;
  const int end = k.dir == 0 ? 1 : 0;
  if (rec.e->nodes[static_cast<size_t>(end)][2] < 3) return std::nullopt;
  const Phases& j = junction_phases(rec, end);
  if (j.phases < 2) return std::nullopt;
  double i = js::kNaN;
  for (size_t q = 0; q < j.edges.size(); ++q)
    if (j.edges[q] == rec.e->id) i = j.phase[q];
  return RoadSignal{kPhase * j.phases, j.offset, kPhase * i, kPhase * i + kGreen};
}

}  // namespace svx::city
