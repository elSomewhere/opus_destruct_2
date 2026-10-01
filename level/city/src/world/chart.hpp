// svx_city — charts (voxel_city world/chart.js): 2D world-plane coordinates (metres) to the
// domain the macro fields sample their noise in.
//
// All local generation (roads, buildings, interiors, voxels) happens in a flat chart frame with z
// up. The chart only decides where the fields sample:
//
//   flat    (x, y) -> (x, y, 0): the 3D noise on a plane (no 4th coordinate)
//   cube    one face of a cube-sphere planet: the face point projected on a sphere of `radius`
//           metres, so climate, urbanization, biomes and terrain line up across faces
//   torus   a world wrapping every `size` metres in x and y: the point on a Clifford torus in 4D,
//           [R cos a, R sin a, R cos b, R sin b], R = size / 2 pi (isometric, so noise keeps its
//           scale; every field built on it is periodic)
//
// A chart is immutable: any thread may use one (the reference's torus memo of the last point is
// a cache only, left out).
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "core/js.hpp"
#include "core/value.hpp"

namespace svx::city {

// A field-sampling point: x, y, z, and w (NaN: a 3D chart; JS's toField returns 3 coordinates and
// the 4th reads undefined, which noise.js's nP / fbmP / ridgedP take as "3D").
struct FieldPoint {
  double x = 0, y = 0, z = 0, w = js::kNaN;
};

class Chart {
 public:
  enum class Kind { Flat, Cube, Torus };

  // FlatChart
  Chart() = default;
  // CubeSphereChart({ radius, face })
  static Chart cube(double radius, double face);
  // TorusChart({ size, latitude }): latitude's JS truthiness
  static Chart torus(double size, bool latitude = true);

  Kind kind = Kind::Flat;
  std::string id = "flat";  // "flat", "cube:{face}", "torus:{size}"
  // cube
  double radius = 0, face = 0, half = 0;
  // torus
  double size = 0, R = 0;
  bool has_latitude = true;

  // toField(x, y): 2D chart coordinates (m) to field coordinates (m).
  FieldPoint to_field(double x, double y) const {
    switch (kind) {
      case Kind::Flat: return {x, y, 0, js::kNaN};
      case Kind::Cube: return cube_field(x, y);
      default: return torus_field(x, y);
    }
  }
  // contains(x, y): charts are unbounded unless they say otherwise.
  bool contains(double x, double y) const {
    return kind != Kind::Cube || (std::fabs(x) <= half && std::fabs(y) <= half);
  }
  // edgeDistance(x, y): distance (m) to the chart's edge (negative outside); Infinity unbounded.
  double edge_distance(double x, double y) const {
    return kind == Kind::Cube ? half - js::max(std::fabs(x), std::fabs(y)) : js::kInf;
  }
  // Whether the chart has a latitude (JS: `chart.latitude` is defined: cube and torus charts).
  bool has_latitude_fn() const { return kind != Kind::Flat; }
  // latitude(fx, fy, fz, fw): the sine of the latitude (0 equator, +-1 poles) of a field point;
  // nothing for JS's null (a torus without latitude; a flat chart has no latitude at all).
  std::optional<double> latitude(double fx, double fy, double fz, double fw) const;

 private:
  FieldPoint cube_field(double x, double y) const;
  FieldPoint torus_field(double x, double y) const;
};

// makeChart(worldCfg): 'flat' (or none), 'torus' (size, latitude), 'cube' (planet.radius,
// planet.face). An unknown chart is a programming error (JS throws).
Chart make_chart(const Value& world_cfg);

}  // namespace svx::city
