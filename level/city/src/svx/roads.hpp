// svx_city — the city's streets as structvox's road network (voxel_city svx/roads.js): the
// internals of svx/city/roads.hpp's RoadNetwork (the public records and the class are there).
//
// A road's structure (roadInfo: its segments in its own cell's view, its nodes - the junctions
// along it, merged where roads meet at one point, its dead ends and the highways' ramp landings -
// and its links between them) is a pure function of the world and the road's id: the network keeps
// the structures it made in a cache (MemoCache: made again alike when dropped), and what JS caches
// on them (a link's pieces, a node's junction and corners, the road's lanes and walks) in Lazy
// fields. A junction refers to its roads by their Road and a node's index (never to another road's
// structure: a structure holds no other one alive), and JS's identities become ids: a road's
// structure is one per road id in JS (`g.info === info`), a meet one of its node's (an index).
//
// What JS remembers of the lanes and walks it handed out (laneIndex, laneInfo, walkIndex,
// walkEnds: lane(id), walk(id), next, signal and walkNext answer only for those) is a Remembered
// set here: the roads (edges) whose records were handed out, the most recent ones first, bounded.
#pragma once

#include <array>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/cache.hpp"
#include "core/rect.hpp"
#include "network/road.hpp"
#include "network/roadView.hpp"
#include "svx/city/roads.hpp"

namespace svx::city {

// The owners (roads, highway edges) whose records a network has handed out, with what it needs to
// find them again (payload P), and the ids of their records: the most recently added (or asked)
// `capacity` owners. Thread-safe.
template <class P>
class Remembered {
 public:
  explicit Remembered(size_t capacity) : capacity_(capacity < 1 ? 1 : capacity) {}
  Remembered(const Remembered&) = delete;
  Remembered& operator=(const Remembered&) = delete;

  // Owner `key` has handed out `records` (each with an `id`): remembered, the most recent.
  template <class Records>
  void add(const std::string& key, const P& payload, const Records& records) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = owners_.find(key);
    if (it != owners_.end()) {
      lru_.splice(lru_.begin(), lru_, it->second.pos);
      return;
    }
    lru_.push_front(key);
    auto ins = owners_.emplace(key, Entry{payload, {}, lru_.begin()}).first;
    const std::string* k = &ins->first;
    for (const auto& r : records) {
      const uint64_t id = id_of_record(r);
      ins->second.ids.push_back(id);
      by_id_[id] = k;
    }
    while (owners_.size() > capacity_) {
      auto old = owners_.find(lru_.back());
      for (uint64_t id : old->second.ids) {
        auto b = by_id_.find(id);
        if (b != by_id_.end() && b->second == &old->first) by_id_.erase(b);
      }
      owners_.erase(old);
      lru_.pop_back();
    }
  }
  // The owner (its key and payload) of a record handed out, or none; it is the most recent now.
  std::optional<std::pair<std::string, P>> find(uint64_t id) {
    std::lock_guard<std::mutex> lk(m_);
    auto b = by_id_.find(id);
    if (b == by_id_.end()) return std::nullopt;
    auto it = owners_.find(*b->second);
    lru_.splice(lru_.begin(), lru_, it->second.pos);
    return std::make_pair(it->first, it->second.payload);
  }
  bool has(uint64_t id) const {
    std::lock_guard<std::mutex> lk(m_);
    return by_id_.count(id) != 0;
  }
  size_t owners() const {
    std::lock_guard<std::mutex> lk(m_);
    return owners_.size();
  }

 private:
  template <class R>
  static uint64_t id_of_record(const R& r) {
    if constexpr (requires { r.walk.id; })
      return r.walk.id;
    else
      return r.id;
  }
  struct Entry {
    P payload;
    std::vector<uint64_t> ids;
    std::list<std::string>::iterator pos;
  };
  size_t capacity_;
  mutable std::mutex m_;
  std::unordered_map<std::string, Entry> owners_;  // (node-based: keys stay where they are)
  std::list<std::string> lru_;
  std::unordered_map<uint64_t, const std::string*> by_id_;
};

namespace roads {

// A road met at a node (roadInfo's meets, `q`): the other road, this road's segment the junction
// was found on (own) and the other road's (seg), both in this road's view; where it crosses (t, on
// seg), its half carriageway and right-of-way, whether it ends there, a signal, crosswalks.
struct Meet {
  RoadPtr road;
  const RoadSeg* own = nullptr;
  const RoadSeg* seg = nullptr;
  double t = 0, hc = 0, hr = 0;
  bool ends = false, signal = false, cw = false;
};

// A highway ramp landing at a node: its edge's id, its index on the edge, an off-ramp.
struct RampRef {
  std::string edge;
  double index = 0;
  bool off = false;
};

// A junction, as every road there works it out (junctionOf): its roads' ids (sorted as strings),
// each road's node there (the same order), their phases (one per heading), a signal, the offset.
struct Junction {
  struct Member {
    RoadPtr road;
    int node = 0;  // (an index into its structure's nodes)
  };
  std::vector<std::string> ids;
  std::vector<Member> members;
  std::vector<double> phase;
  double phases = 0;
  bool signal = false;
  double offset = 0;
  double phase_of(const std::string& id) const {
    for (size_t k = 0; k < ids.size(); ++k)
      if (ids[k] == id) return phase[k];
    return js::kNaN;
  }
};

// A corner on a road's walking line at a node (cornersAt): where it is (voxels), its arc offset
// from the node, the meet whose walking line crosses there (-1: none, the line at a dead end) and
// that road with its lane count.
struct Corner {
  double x = 0, y = 0, s = 0;
  int q = -1;
  RoadPtr other;
  double other_lanes = 0;
};

struct Node {
  double at = 0, trim = 0;
  std::vector<Meet> meets;
  std::vector<RampRef> ramps;
  Lazy<Junction> junction;
  Lazy<std::vector<Corner>> corners[3];  // (by side: -1, 0, 1)
};

struct Link {
  int k = 0;
  int from = 0, to = 0;  // (node indices)
  Lazy<std::vector<std::array<double, 2>>> pieces;
};

// A walk with what walkNext reads of it (walkEnds): its corners (voxels) and the roads of the
// junctions at its ends.
struct WalkRec {
  RoadWalk walk;
  Point2 pa, pb;
  std::vector<RoadPtr> roads;
};

// A road's structure (roadInfo).
struct Info {
  RoadPtr road;
  std::shared_ptr<const RoadView> view;
  std::vector<const RoadSeg*> segs;
  double L = 0;
  std::vector<Node> nodes;
  std::vector<Link> links;
  double lanes = 0;  // each way
  Lazy<std::vector<RoadLane>> lane_list;
  Lazy<std::vector<WalkRec>> walk_list;
};
using InfoPtr = std::shared_ptr<const Info>;

}  // namespace roads

// The numbers of a cell's id "C<i>_<j>..." (JS /^C(-?\d+)_(-?\d+)/), false where it is none.
bool parse_cell_id(const std::string& id, double* i, double* j);

}  // namespace svx::city
