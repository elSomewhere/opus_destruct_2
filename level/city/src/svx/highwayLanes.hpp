// svx_city — the highways (network/highways.hpp) as lanes of structvox's road network (voxel_city
// svx/highwayLanes.js), beside the streets of svx/roads.cpp and in its conventions (metres, z the
// surface, right-hand traffic in structvox's frame, lane k counting from the median outwards):
//
//   - deck lanes along each carriageway, in pieces straight within LATERAL_SLACK of the curve and
//     PROFILE_SLACK of the deck's level; each carriageway cut into links where a ramp leaves or
//     joins it;
//   - a ramp is one lane, one way: an off-ramp from its deck end down to its landing on the
//     arterial, an on-ramp from its landing up; an off-ramp leaves from the kerb lane (a right
//     turn), an on-ramp joins the kerb lane;
//   - at a lattice node where two highways meet the lanes run on; at a junction of three or four,
//     lanes stop short of its plateau and turn across it (right from the kerb lane, left from the
//     inner lane), signalled one phase per heading as the streets' junctions are; at a terminus (a
//     route's last node) they turn back.
//
// `out` links the ramps to the streets (svx/roads.cpp): the lanes leaving an off-ramp's landing on
// its arterial. Every record is a pure function of the world. JS keeps every edge's lanes it made
// and answers for those alone (byEdge, index); here an edge's lanes are a cache (made again alike
// when dropped) and the edges whose lanes were made are remembered (svx/roads.hpp Remembered).
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/cache.hpp"
#include "core/rect.hpp"
#include "network/highways.hpp"
#include "svx/roads.hpp"

namespace svx::city {

class World;

class HighwayLanes {
 public:
  // city.out(edge id, ramp index): the lanes leaving an off-ramp's landing on its arterial.
  using Out = std::function<std::vector<RoadLane>(const std::string& edge_id, double ramp)>;
  HighwayLanes(const World& world, Out out, size_t edges, size_t remembered);
  HighwayLanes(const HighwayLanes&) = delete;
  HighwayLanes& operator=(const HighwayLanes&) = delete;

  // Highway and ramp lanes through a box (city voxels) that `in_box` keeps, unsorted.
  void lanes_in(const Rect& box, const std::function<bool(const RoadLane&)>& in_box, std::vector<RoadLane>& out) const;
  bool has(uint64_t id) const;
  std::optional<RoadLane> lane(uint64_t id) const;
  // The lanes of a ramp by (edge id, ramp index): its first and last pieces, and whether it is an
  // off-ramp (none where the ramp has no lane).
  struct RampLanes {
    std::optional<RoadLane> first, last;
    std::optional<bool> off;
  };
  RampLanes ramp_lanes(const std::string& edge_id, double ri) const;
  std::vector<RoadTurn> next(uint64_t id) const;
  // The signal a deck lane meets at a junction of three or four highways, or none.
  std::optional<RoadSignal> signal(uint64_t id) const;

  // An edge's lanes (edgeLanes): per direction (0 along the deck's arc, traffic on its right, side
  // -1; 1 against it, side +1) its links between the cuts, in its traffic's order; its lanes (the
  // deck's, then its ramps'); its ramps (the edge's: held with it).
  struct Link {
    double li = 0, from = 0, to = 0;
  };
  struct Phases {
    std::vector<std::string> edges;
    std::vector<double> phase;
    double phases = 0, offset = 0;
  };
  struct Rec {
    HighwayEdgePtr e;
    std::array<std::vector<Link>, 2> links;
    std::vector<RoadLane> lanes;
    const std::vector<HighwayRamp>* ramps = nullptr;
    // (the junction phases at the edge's either end node: a pure function of the node, kept)
    Lazy<Phases> phases[2];
  };
  std::shared_ptr<const Rec> edge_lanes(const HighwayEdgePtr& e) const;

 private:
  std::shared_ptr<const Rec> build(const HighwayEdgePtr& e) const;
  HighwayEdgePtr edge_by_id(const std::string& id) const;
  // The lane handed out with this id and its edge's lanes.
  struct Hit {
    std::shared_ptr<const Rec> rec;
    const RoadLane* lane = nullptr;
  };
  std::optional<Hit> hit(uint64_t id) const;
  // The lanes leaving lattice node (a, b) on its edges (first pieces), with their edge's lanes.
  std::vector<std::pair<const RoadLane*, std::shared_ptr<const Rec>>> leaving_node(double a, double b) const;
  const Phases& junction_phases(const Rec& rec, int end) const;

  const World& world_;
  const HighwayNetwork& hw_;
  Out out_;
  double n_ = 0;       // lanes per side
  double lane_w_ = 0;  // voxels
  mutable MemoCache<std::string, Rec> by_edge_;
  mutable Remembered<bool> index_;
};

// The numbers of an edge's id "H<axis>_<a>_<b>" (JS /^H(\d)_(-?\d+)_(-?\d+)$/), false where it is
// none.
bool parse_edge_id(const std::string& id, double* axis, double* a, double* b);

}  // namespace svx::city
