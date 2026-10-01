// svx_city — the elevated highway network (voxel_city network/highways.js).
//
// Nodes sit on a jittered lattice (~3.4 km); each lattice edge exists along intercity routes, or
// by chance between two urban nodes (never as a spur), and is drawn as a Catmull-Rom spline
// through lateral bends, so highways snake across the city. The deck profile follows the terrain
// plus clearance, lifted where it must clear something (urban land, crossing roads, rivers),
// within a 5% grade (cuttings and tunnels through hills, viaducts over valleys only where lifted),
// and meets the node heights; junctions of other than two edges are level plateaus. Diamond
// interchanges where a highway crosses an urban arterial: ramps beside the deck, landing on the
// arterial at its level. Piers stand off the streets.
//
// Everything is a pure function of the lattice, so any region can be generated independently. The
// network's products (edges, the edge-existence memo) are kept in caches (core/cache.hpp); an
// edge's ramps and piers, and a ramp's piers, are Lazy fields made on first use. Cells query
// corridors_near to keep lots under the deck free of buildings.
//
// The highways' feature source (highwaySource) is rasterize_highways / highway_z_range: what the
// reference's source does, the ground tile's level given as its z array (compose.js's tile.z, a
// later stage of the port; null: the terrain's, as the reference without a tile).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/cache.hpp"
#include "core/geom2d.hpp"
#include "core/rect.hpp"
#include "world/wrap.hpp"

namespace svx::city {

class World;
class ChunkBuffer;
struct Settlement;

// A lattice node: its lattice indices, position (voxels) and deck level.
struct HighwayNode {
  double a = 0, b = 0, x = 0, y = 0, z = 0;
};

// A straight piece of an edge's deck (z: the deck level at its ends).
struct HighwaySeg {
  double ax = 0, ay = 0, az = 0, bx = 0, by = 0, bz = 0, len = 0, dx = 0, dy = 0, s0 = 0;
  Rect bb;  // (deck and ramps)
};

// A junction plateau at an edge's end (where other than two edges meet: a crossing of highways, a
// terminus): level at the node's height out to r.
struct HighwayJunction {
  double x = 0, y = 0, z = 0, r = 0, degree = 0;
  std::array<double, 2> node{};
};

// A ramp beside the deck (side +1 left of the deck's direction, -1 right), from its deck end
// (arc s_deck, level z_deck) to its landing (arc s_ground, level z_ground) on an arterial at the
// crossing at arc `cross`; (x, y) the landing; drop the deck's height over the landing.
struct HighwayRamp {
  double side = 0, s_deck = 0, s_ground = 0, drop = 0, z_deck = 0, z_ground = 0, cross = 0, x = 0, y = 0;
  std::string arterial;  // the arterial's road id
  Lazy<std::vector<double>> piers;  // (rampPiers: its pier stations, made once)
};

// A pier: at arc s, (x, y) on the deck's centre line, the deck's direction (tx, ty) and level z;
// its columns' lateral offsets (voxels, left of the deck's direction) and the cap's half length.
struct HighwayPier {
  double s = 0, x = 0, y = 0, tx = 0, ty = 0, z = 0;
  std::vector<double> cols;
  double cap = 0;
};

// An edge of the lattice: (a, b) -> (a + 1, b) (axis 0) or (a, b + 1) (axis 1).
struct HighwayEdge {
  std::string id;  // H{axis}_{a}_{b}
  double axis = 0, a = 0, b = 0;
  std::array<std::array<double, 3>, 2> nodes{};  // [a, b, degree] at either end
  std::vector<PPoint> pts;                       // the deck's centre line, 8 m apart (z: the deck level, unrounded)
  std::vector<double> lengths;                   // arc at each point
  std::vector<HighwaySeg> segs;
  SpatialGrid<const HighwaySeg*> grid{512};
  Rect bb;
  double total = 0;
  std::vector<HighwayJunction> junctions;
  Lazy<std::vector<HighwayRamp>> ramps;  // (made once: HighwayNetwork::ramps)
  Lazy<std::vector<HighwayPier>> piers;  // (made once: HighwayNetwork::piers)

  HighwayEdge() = default;
  HighwayEdge(const HighwayEdge&) = delete;  // (the grid points into segs)
  HighwayEdge& operator=(const HighwayEdge&) = delete;
};
using HighwayEdgePtr = std::shared_ptr<const HighwayEdge>;

// pointAt: the deck's centre at arc s (extrapolated past the ends) and its unit tangent.
struct HighwayPoint {
  double x = 0, y = 0, z = 0, tx = 0, ty = 0;
};
HighwayPoint point_at(const HighwayEdge& e, double s);
// offsetAt: the point at arc s offset `off` voxels to the left of the deck's direction (z the deck's).
HighwayPoint offset_at(const HighwayEdge& e, double s, double off);
// rampZ: a ramp's level at arc s - the deck's less a drop that closes from its landing to its deck
// end, eased at both ends.
double ramp_z(const HighwayEdge& e, const HighwayRamp& r, double s);

// The nearest deck point: d lateral (signed: + left), s its arc, z the deck's top there.
struct HighwayNearest {
  double d = 0, s = 0, z = 0;
  const HighwayEdge* edge = nullptr;
  const HighwaySeg* seg = nullptr;
};

// A ramp's surface at a deck arc and lateral offset: its level (rounded), the ramp, t (0 at its
// landing, 1 at its deck end; past them outside 0..1), and whether the column is its outer or
// inner edge.
struct HighwayRampAt {
  double z = 0;
  const HighwayRamp* ramp = nullptr;
  double t = 0;
  bool outer = false, inner = false;
};

class HighwayNetwork;

// A corridor (corridorsNear): does a rect come too close to its deck or ramps?
struct HighwayCorridor {
  HighwayEdgePtr edge;
  const HighwayNetwork* net = nullptr;
  double margin = 0;
  bool hits_rect(const Rect& r) const;
};

class HighwayNetwork {
 public:
  explicit HighwayNetwork(const World& world);
  HighwayNetwork(const HighwayNetwork&) = delete;
  HighwayNetwork& operator=(const HighwayNetwork&) = delete;

  const World& world;
  // config.highways
  struct Cfg {
    double node_spacing = 0, jitter = 0, edge_chance = 0, min_urbanization = 0, deck_height = 0, lanes_per_side = 0, lane_width = 0, shoulder = 0,
           pier_spacing = 0, corridor_margin = 0;
  } cfg;
  double spacing = 0;  // voxels
  double hw = 0;       // the deck's half width (voxels)
  double clear = 0;    // the deck's height in towns (voxels)
  // a wrapping world has n lattice nodes round it (canonical seeds, positions by lap)
  Wrap wrap;
  double n = 0;
  double plateau = 0;   // half length of a junction plateau (voxels)
  double ramp_in = 0;   // a ramp's band beside the deck: from the deck's edge, 7 m wide
  double ramp_mid = 0;  // ... and its centre line

  double canon(double a) const { return Wrap::canon(a, n); }
  HighwayNode node(double a, double b) const;
  // Deck height above the terrain: elevated in towns, on a low embankment in open country.
  double clearance_at(double u) const { return u > 0.3 ? clear : 4; }
  // The deck profile for points 8 m apart between node heights z_start and z_end, level over
  // plateaus of flat_start / flat_end voxels at the ends.
  std::vector<double> profile(const std::vector<PPoint>& pts, double z_start, double z_end, double flat_start = 0, double flat_end = 0) const;
  // Does lattice edge (a, b) -> (a + 1, b) [axis 0] or (a, b + 1) [axis 1] exist?
  bool edge_exists(int axis, double a, double b) const { return base_edge(axis, a, b, 2); }
  // Edge existence after k pruning passes: routes, and chance edges with company at both ends.
  bool base_edge(int axis, double a, double b, int k) const;
  // A chance edge: drawn by the hash, between two urban nodes.
  bool chance_edge(int axis, double a, double b) const;
  // The existing edges at node (a, b): [axis, a, b] of the four lattice edges there.
  std::vector<std::array<double, 3>> edges_at(double a, double b) const;
  // A node's deck height as its edges meet it (at a terminus the street level: it ends at grade).
  double node_z(double a, double b) const;
  // Can the two junction heights be joined within the grade limit?
  bool grade_ok(int axis, double a, double b) const;
  // The unjittered lattice position of node (a, b).
  Point2 lattice_xy(double a, double b) const { return {a * spacing, b * spacing}; }
  // Intercity routes: every settlement links to its east and south neighbours by an L-shaped
  // path along the lattice (only settlements within two macro cells are examined).
  bool on_intercity_route(int axis, double a, double b) const;
  bool route_has(const Settlement& s, const Settlement& t, int axis, double a, double b) const;
  // The edge (null where none exists), cached.
  HighwayEdgePtr edge(int axis, double a, double b) const;
  // Highway edges whose bounds overlap rect.
  std::vector<HighwayEdgePtr> edges_near(const Rect& rect) const;
  // Corridors for cell planning.
  std::vector<HighwayCorridor> corridors_near(const Rect& rect) const;
  // The nearest deck point among edges within max_d.
  std::optional<HighwayNearest> nearest(double x, double y, const std::vector<HighwayEdgePtr>& edges, double max_d) const;
  // The edge's ramps (diamond interchanges at urban arterials; made once).
  const std::vector<HighwayRamp>& ramps(const HighwayEdge& e) const;
  // Would a ramp pass over another street too low?
  bool ramp_blocked(const HighwayEdge& e, const HighwayRamp& r) const;
  // A ramp's surface at (arc, lateral d), or none.
  std::optional<HighwayRampAt> ramp_at(const HighwayEdge& e, double s_pos, double d) const;
  // Is (x, y) under a deck or a ramp, or within `margin` voxels of one?
  bool covers(double x, double y, double margin = 0) const;
  // The lowest level (voxels) of a deck or ramp over column (x, y) (-Infinity: a ramp near the
  // ground fills to it), or none.
  std::optional<double> underside(double x, double y) const;
  // Is (x, y) on a street's carriageway or its kerb (where no pier may stand)?
  bool on_road(double x, double y) const;
  // The edge's piers (made once).
  const std::vector<HighwayPier>& piers(const HighwayEdge& e) const;
  // A ramp's pier stations (arcs; made once).
  const std::vector<double>& ramp_piers(const HighwayEdge& e, const HighwayRamp& r) const;
  // mapData: the edges near a rect as polylines, and the deck's width.
  struct MapEdge {
    std::string id;
    std::vector<std::array<double, 2>> pts;
    double width = 0;
  };
  std::vector<MapEdge> map_data(const Rect& rect) const;

 private:
  std::shared_ptr<const HighwayEdge> build_edge(int axis, double a, double b) const;
  double settlement_cell_ = 0;  // config.world.settlementCell (m)
  mutable MemoCache<uint64_t, HighwayEdge> edges_{256};
  mutable MemoCache<uint64_t, bool> memo_{1 << 14};
};

// ---- highwaySource (order 5, LOD <= 7)

constexpr double kHighwaySourceOrder = 5;
constexpr int kHighwaySourceMaxLod = 7;
// zRange: the LOD 0 levels the highways' content may take in a rect (false: none).
bool highway_z_range(const World& world, const Rect& rect, double* z0, double* z1);
// rasterize: the decks, ramps, earthworks, tunnels and piers of the world's highways into a chunk;
// tile_z the ground tile's level per padded column (kP * kP, compose.js tile.z), or null.
void rasterize_highways(const World& world, ChunkBuffer& chunk, const int32_t* tile_z);

}  // namespace svx::city
