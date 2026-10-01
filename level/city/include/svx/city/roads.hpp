// svx_city — the city's streets, highways, walkways and kerbside parking as a host's road network
// (voxel_city svx/roads.js roadNetwork, with svx/highwayLanes.js): what a game's traffic drives and
// its people walk, as plain records over a World from make_world (svx/city/world.hpp). The adapter
// in svx_procgen wraps it as the game's RoadNetwork (svx/procgen/city_roads.hpp).
//
// Conventions (the export's, after structvox's drive city):
//   - metres in the export's frame: a city point q (voxels) is at h (q - 1/2), h = 0.125 m; z the
//     surface - a lane on its road's carriageway (a highway's deck, a ramp), a walk on its
//     sidewalk (a kerb higher), down an alley on its paving;
//   - traffic keeps to the right in structvox's frame (right-handed, z up: heading (dx, dy), right
//     is (dy, -dx)); a turn is right (1) when it turns clockwise seen from above, left (-1)
//     counter-clockwise, straight on (0) within 30 degrees;
//   - a lane is a straight run one way: a road is cut at its junctions into links, a link at its
//     bends into pieces, and a lane stops at the kerb line of the road it meets (the turn through
//     the junction is the host's, from one lane's b to the next one's a); lane k counts from the
//     centre line (or the median) outwards; streets with a lane each way or more carry traffic
//     (alleys, single-track lanes and pedestrian streets only people);
//   - signals: one phase per heading of a junction's roads, 17 s each, green the first 14 s; a
//     crossing over a road opens the first 4 s of the phase after the road's;
//   - walks: along each side of a link on the middle of its sidewalk from corner to corner, between
//     two roads' corners on one side of a junction, over a road at its crosswalks, down the middle
//     of alleys and single-track lanes; a walk's end meets the walks ending at its corner;
//   - parking: in a road's parking strips, a place every 6 m, heading with the traffic on its side;
//   - ids: stable 52-bit integers of structural keys (svx/ids.js), the same whatever the query, on
//     any thread; queries return records in id order.
//
// Every answer is a pure function of the world and the arguments, but for one contract the
// reference keeps: lane(id) knows only lanes handed out - those of the roads (highway edges) whose
// lanes a query has made (lanes_in, next) - and walk(id) only walks of the roads whose walks a query
// has made (walks_in, walk_next); next, signal and walk_next answer for those alone. The network
// remembers the records of the most recent `remembered` roads (`remembered_edges` highway edges:
// docs/CITY.md §6). Queries may come from several threads at once; memory is bounded
// (RoadNetworkOptions).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace svx::city {

class World;

// A signal (s): its traffic may go while (t + offset) mod cycle lies in [from, to).
struct RoadSignal {
  double cycle = 0, offset = 0, from = 0, to = 0;
};
// Is a signal open at time t (s)? (none: always)
bool signal_open(const std::optional<RoadSignal>& signal, double t);

// What a lane is: a street's, a highway deck's or a ramp's, and where it lies in its road (edge).
struct RoadLaneKey {
  enum class Kind : uint8_t { Street, Deck, Ramp };
  Kind kind = Kind::Street;
  std::string road;                          // (a street) its road's id ("C<i>_<j>/r<n>")
  std::string hw;                            // (a deck lane, a ramp) its highway edge's id ("H<axis>_<a>_<b>")
  int link = 0, piece = 0, dir = 0, k = 0;   // (a street, a deck) its link and piece in its direction's order,
                                             // its direction (0 along the road / the deck's arc), its lane
                                             // from the centre line (median) out; (a ramp) its piece
  bool last = false;                         // the last piece of its link (ramp)
  double sa = 0, sb = 0;                     // (a deck lane) its ends' arcs along the deck (voxels)
  int ramp = -1;                             // (a ramp) its index on its edge
  bool off = false;                          // (a ramp) an off-ramp (down from the deck)
  double s_deck = 0, side = 0;               // (a ramp) its deck end's arc; its side of the deck (+1 left)
  std::string arterial;                      // (a ramp) the arterial it lands on
};

// A lane: a straight run of road one way, from a to b (metres), its width and speed limit.
struct RoadLane {
  uint64_t id = 0;
  std::array<double, 3> a{}, b{};
  double width = 0;  // m
  double speed = 0;  // m/s
  RoadLaneKey key;
};

enum class WalkKind : uint8_t {
  Sidewalk,  // along a road's side, corner to corner
  Corner,    // between two roads' corners on one side of a junction
  Crossing,  // over a road
  Middle,    // down an alley or a single-track lane
};
// "sidewalk", "corner", "crossing", "middle"
const char* walk_kind_name(WalkKind kind);

// A walkway between two corners (metres); along a sidewalk people keep `inset` off the a-b line in
// its middle (towards the buildings).
struct RoadWalk {
  uint64_t id = 0;
  std::array<double, 3> a{}, b{}, inset{};
  double width = 0;  // m
  bool crossing = false;
  WalkKind kind = WalkKind::Sidewalk;
  std::optional<RoadSignal> signal;  // (a crossing at a signalled junction) when it is open
};

// A kerbside parking place: where a car stands (metres, on the carriageway) and its heading (a
// unit vector: with the traffic on its side).
struct RoadParking {
  uint64_t id = 0;
  std::array<double, 3> pos{};
  std::array<double, 2> heading{};
};

// A lane's (a walk's) way on: an id and the turn (-1 left, 0 straight on, 1 right); a walk's end
// (0 its a, 1 its b).
using RoadTurn = std::pair<uint64_t, int>;

// Everything of a box a host's own road network holds (a host that cannot call back: a browser's
// engine beside a worker): its lanes with their ways on and signals, its walks with the walks at
// their ends, its parking places (RoadNetwork::region).
struct RoadRegion {
  struct LaneEntry {
    RoadLane lane;
    std::vector<std::pair<uint64_t, int>> next;
    std::optional<RoadSignal> signal;
  };
  struct WalkEntry {
    RoadWalk walk;
    std::vector<std::pair<uint64_t, int>> next_a, next_b;
  };
  std::vector<LaneEntry> lanes;
  std::vector<WalkEntry> walks;
  std::vector<RoadParking> parking;
};

// Bounds of what the network keeps (least recently used dropped beyond them: a structure dropped
// is made again alike when asked; a road's records dropped from what lane() and walk() remember are
// answered again once a query hands them out again).
struct RoadNetworkOptions {
  size_t roads = 1024;            // roads' structures (nodes, junctions, lanes, walks) kept made
  size_t edges = 32;              // highway edges' lanes kept made
  size_t remembered = 4096;       // roads whose lanes and walks lane() and walk() answer
  size_t remembered_edges = 256;  // highway edges whose lanes lane() answers
};

class RoadNetwork {
 public:
  explicit RoadNetwork(std::shared_ptr<const World> world, const RoadNetworkOptions& options = {});
  ~RoadNetwork();
  RoadNetwork(const RoadNetwork&) = delete;
  RoadNetwork& operator=(const RoadNetwork&) = delete;

  using Vec2 = std::array<double, 2>;

  // The lanes running through a box (metres, x and y: a lane whose bounds meet it), in id order.
  std::vector<RoadLane> lanes_in(const Vec2& lo, const Vec2& hi) const;
  // One lane by id, as a query handed it out (lanes_in, next), or none.
  std::optional<RoadLane> lane(uint64_t id) const;
  // The lanes a car at the end of lane `id` may go on along, with the turn, in id order: the next
  // piece; through its junction straight on (keeping its lane), right from the kerb lane, left
  // from the inner lane (any lane where there is no straight on); onto a highway's on-ramp; at a
  // dead end back the way it came. None for an id not handed out.
  std::vector<RoadTurn> next(uint64_t id) const;
  // Lane `id`'s signal at its junction, or none (never held).
  std::optional<RoadSignal> signal(uint64_t id) const;
  // May lane `id`'s traffic enter its junction at time t (s)?
  bool green(uint64_t id, double t) const;
  // Kerbside parking places in a box (metres), in id order.
  std::vector<RoadParking> parking_in(const Vec2& lo, const Vec2& hi) const;
  // The walkways running through a box (metres), in id order.
  std::vector<RoadWalk> walks_in(const Vec2& lo, const Vec2& hi) const;
  // May people step onto walk `id` at time t (s)? (a sidewalk, an unknown id: always)
  bool walk_open(uint64_t id, double t) const;
  // One walk by id, as a query handed it out (walks_in, walk_next), or none.
  std::optional<RoadWalk> walk(uint64_t id) const;
  // The walks meeting walk `id`'s end (0 a, 1 b) at its corner, and at which of their ends, in id
  // order.
  std::vector<RoadTurn> walk_next(uint64_t id, int end) const;
  // Everything of a box (metres): lanes_in with next and signal of each, walks_in with walk_next
  // of each end, parking_in.
  RoadRegion region(const Vec2& lo, const Vec2& hi) const;

  const World& world() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace svx::city
