// svx_city — canonical building frames (voxel_city buildings/frame.js).
//
// Every building and interior algorithm works in a rotated frame where u runs along the street
// frontage and v from the FRONT (v = 0) to the back; sides F (front, v = 0), B (back), L (u = 0),
// R (u = U - 1). Frames are proper rotations of the world grid (no mirroring), so stairs, hinges
// and furniture keep their handedness. Canonical rects use the world's Rect with x = u, y = v.
//
// A frame is a thin wrapper over an axis-aligned Placement (core/placement.hpp): the front picks
// the quarter turn (N 0, E 90, S 180, W 270 degrees) and the pivot is the world cell of
// canonical cell (0, 0).
//
// A turned frame (TurnedFrame, the angled world's turned buildings) is the same over a placement
// with any yaw of the table: canonical cell (u, v) is its local cell (u + ou, v + ov), so a lot
// and the building on it share one placement and differ by an integer offset. Its rect_to_world
// is the world bounds of the voxels a canonical rect holds (for culling and indexing: what draws
// it tests each voxel with from_world).
//
// In C++ both are one value type: a TurnedFrame is a Frame with `turned` set (JS's subclass
// overrides become branches on it), so frames copy freely. point_to_world, point_from_world,
// distance and shifted are a turned frame's only (a plain frame has none in JS: it fails here).
#pragma once

#include <array>
#include <optional>

#include "core/js.hpp"
#include "core/obb.hpp"
#include "core/placement.hpp"
#include "core/rect.hpp"

namespace svx::city {

// A turn recorded on a lot or an envelope: { yaw, yaw2?, origin, ou, ov } (a lot's also carries
// its U x V).
struct Turn {
  int yaw = 0;
  int yaw2 = 0;  // (0: none - JS leaves it out)
  Point2 origin;
  double ou = 0, ov = 0;
  double U = js::kNaN, V = js::kNaN;  // (a lot's turn only)
};

class Frame {
 public:
  // (an empty frame: the plain frame of the one-cell rect at the origin, front N)
  Frame() : Frame(Rect{0, 0, 0, 0}, 'N') {}
  // R the world rect (inclusive), front the world side ('N', 'E', 'S', 'W') of the street facade.
  Frame(const Rect& R, char front);

  Rect R;
  char front = 'N';
  double U = 1, V = 1;
  Placement placement;
  // (a TurnedFrame)
  bool turned = false;
  double ou = 0, ov = 0;

  // canonical cell -> world cell, world cell -> canonical cell
  XY to_world(double u, double v) const;
  XY from_world(double x, double y) const;
  Rect rect_to_world(const Rect& r) const;
  Rect rect_from_world(const Rect& r) const;
  // canonical side ('F', 'B', 'L', 'R') -> world side ('N', 'S', 'E', 'W'), and back ('\0': JS's
  // undefined)
  char world_side(char cs) const;
  char canon_side(char ws) const;
  // canonical direction (du, dv) -> world (dx, dy)
  XY dir_to_world(double du, double dv) const { return placement.dir_to_world_xy(du, dv); }

  // ---- turned frames
  // The same placement with the canonical origin at this frame's (du, dv), U x V cells.
  Frame shifted(double du, double dv, double U, double V) const;
  // The turn to record on a lot or an envelope (none: a plain frame).
  std::optional<Turn> turn() const;
  // Canonical continuous point -> world continuous point (exact rationals), and back.
  XY point_to_world(double u, double v) const;
  XY point_from_world(double x, double y) const;
  // Distance (voxels) from world voxel (x, y) to a canonical rect, between cell centres (0
  // inside), in the frame's own axes.
  double distance(const Rect& r, double x, double y) const;
};

// A turned frame: canonical (u, v) = local cell (u + ou, v + ov) of a placement with a yaw (no
// pitch or roll), U x V cells; `front` the nominal world side the facade faces (the nearest of
// N, E, S, W), for coarse decisions only.
class TurnedFrame : public Frame {
 public:
  TurnedFrame(const Placement& placement, double ou, double ov, double U, double V, char front);
};

// Canonical side vectors (outward normal) and opposite sides.
inline std::array<double, 2> cside_dir(char cs) {
  return cs == 'F' ? std::array<double, 2>{0, -1} : cs == 'B' ? std::array<double, 2>{0, 1} : cs == 'L' ? std::array<double, 2>{-1, 0} : std::array<double, 2>{1, 0};
}
inline char copp(char cs) { return cs == 'F' ? 'B' : cs == 'B' ? 'F' : cs == 'L' ? 'R' : 'L'; }

// The nearest proper front (N, E, S, W) to a turned front of yaw index `yaw` (followed by `yaw2`).
char nominal_front(int yaw, int yaw2 = 0);
// A turned frame from a recorded turn, U x V cells (front: the turn's nominal front).
Frame turned_frame(const Turn& turn, double U, double V);
Frame turned_frame(const Turn& turn, double U, double V, char front);
// A lot's frame (lotFrameOf): turned when the lot is (its turn's U x V), else the plain frame of
// its rect.
Frame lot_frame_of(const std::optional<Turn>& turn, const Rect& rect, char front);
// A building's frame (frameOf): turned when the envelope is, else the plain frame of env.R. (JS
// memoizes it per envelope: the envelope's port keeps it in a Lazy<Frame>.)
Frame frame_of(const std::optional<Turn>& turn, double U, double V, char front, const Rect& R);

}  // namespace svx::city
