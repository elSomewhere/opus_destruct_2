// svx_city — exact placements of local voxel lattices in the world (voxel_city
// core/placement.js, ANGLED_WORLD_PLAN.md §4.1-4.2).
//
// Every rotation is quantized to a Pythagorean triple p^2 + q^2 = r^2, so its cosine and sine are
// the rationals q / r and p / r: no sin, cos or atan2, and both mapping directions stay in integer
// arithmetic (in doubles, exact below 2^53), bit for bit JavaScript's.
//
//   yaw    about the world's up axis, an index into yaws() (132 exact yaws), optionally followed by
//          a second one, yaw2 (their exact product)
//   pitch  about the local v axis, a signed index into pitches() (+: climbing along +u)
//   roll   about the local u axis, an index into yaws()
//
// The rotation local -> world is M / D = Rz(yaw) Ry(pitch) Rx(roll), an integer matrix over a
// common denominator. A world voxel (x, y, z) belongs to the local cell holding its centre:
// (u, v, w) = floor(M^T (2 (p - origin) + 1) / 2D); a local cell's centre lies at
// origin + M (2 (u, v, w) + 1) / 2D.
#pragma once

#include <array>
#include <optional>
#include <vector>

#include "core/js.hpp"
#include "core/rect.hpp"

namespace svx::city {

struct Yaw {
  double c = 1, s = 0, r = 1;  // c^2 + s^2 = r^2: cos = c / r, sin = s / r
  double n = 0;                // (pitches: the family's n)
};

// The 16 primitive triples with r <= 100 by angle, 8.80 to 43.60 degrees.
extern const std::array<std::array<int, 3>, 16> kYawTriples;
extern const std::array<int, 6> kPitchN;
// Every exact yaw (132), by angle from 0 (east) to 360 degrees.
const std::vector<Yaw>& yaws();
// Pitches: index 0 level, 1..6 the grades of kPitchN; negative indices descend along +u.
const std::vector<Yaw>& pitches();
constexpr int kYawQuarter = 33;  // yaws per quarter turn: the proper rotations are 33 k
inline int front_yaw(char front) { return front == 'N' ? 0 : front == 'E' ? kYawQuarter : front == 'S' ? 2 * kYawQuarter : 3 * kYawQuarter; }

inline double floor_div_exact(double a, double b) { return std::floor(a / b); }
inline double ceil_div_exact(double a, double b) { return std::ceil(a / b); }

// The exact yaw of table yaw a followed by table yaw b (reduced to a primitive triple).
Yaw yaw_product(int a, int b);
// The table index of an exact rotation (reduced), or -1.
int yaw_index(const Yaw& y);
Yaw yaw_vector(int yaw, int yaw2 = 0);

struct RotMatrix {
  std::array<double, 9> m{};  // row-major, local -> world
  double d = 1;
};
RotMatrix rotation_matrix(int yaw = 0, int pitch = 0, int roll = 0, int yaw2 = 0);

struct QuatXYZW {
  double x = 0, y = 0, z = 0, w = 1;
};
// The unit quaternion (w >= 0) of M / D: every component the square root of an exact rational.
QuatXYZW matrix_quat(const std::array<double, 9>& m, double d);

// The table yaw nearest direction (dx, dy) among `allowed` (default all) by the largest cosine;
// ties to the lower index.
int nearest_yaw(double dx, double dy, const std::vector<int>* allowed = nullptr);
extern const std::vector<int> kCardinalYaws;
const std::vector<int>& all_yaws();

// A local box of cells (inclusive): u0..u1, v0..v1, w0..w1.
struct LocalBox {
  double u0 = 0, v0 = 0, w0 = 0, u1 = 0, v1 = 0, w1 = 0;
};
struct Box3 {
  double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
};
struct XYZ {
  double x = 0, y = 0, z = 0;
};

struct PlacementOpts {
  XYZ origin;
  int yaw = 0, yaw2 = 0, pitch = 0, roll = 0;
  double h = 0.125;
  double priority = 0;
  bool anchored = false;
  std::optional<LocalBox> extent;
};

class Placement {
 public:
  Placement() : Placement(PlacementOpts{}) {}
  explicit Placement(const PlacementOpts& o);
  // An axis-aligned placement from its pivot (the world cell of local cell (0, 0)) and its
  // quarter turns q (0 N, 1 E, 2 S, 3 W); the pivot is kept as given.
  static Placement cardinal(double px, double py, int q, PlacementOpts opts = {});

  bool axis_aligned() const { return q >= 0; }
  std::array<double, 2> to_local_xy(double x, double y) const;
  std::array<double, 2> to_world_xy(double u, double v) const;
  std::array<double, 2> dir_to_world_xy(double du, double dv) const;
  std::array<double, 3> to_local(double x, double y, double z) const;
  std::array<double, 3> to_world(double u, double v, double w) const;
  std::array<double, 3> to_world_scaled(double u, double v, double w) const;
  Box3 local_bounds_to_world_aabb(const LocalBox& e) const;
  std::optional<Box3> world_aabb() const;
  QuatXYZW to_quat() const { return matrix_quat(m, d); }

  XYZ origin;
  int yaw = 0, yaw2 = 0, pitch = 0, roll = 0;
  double h = 0.125;
  double priority = 0;
  bool anchored = false;
  std::optional<LocalBox> extent;
  std::array<double, 9> m{};
  double d = 1;
  bool flat = true;  // only a yaw: x, y map on their own, z = w + origin.z
  int q = 0;         // quarter turns of an axis-aligned placement (-1: not one)
  double px = 0, py = 0;  // (q >= 0) the world cell of local cell (0, 0)
};

}  // namespace svx::city
