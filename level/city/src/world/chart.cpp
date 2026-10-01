// svx_city — world/chart.hpp (voxel_city world/chart.js).
#include "world/chart.hpp"

#include <cstdio>

#include "svx/base/types.hpp"

namespace svx::city {

namespace {
constexpr double kPi = 3.141592653589793;  // Math.PI
}

Chart Chart::cube(double radius, double face) {
  Chart c;
  c.kind = Kind::Cube;
  c.id = js::cat("cube:", face);
  c.radius = radius;
  c.face = face;
  c.half = (kPi / 4) * radius;
  return c;
}

Chart Chart::torus(double size, bool latitude) {
  Chart c;
  c.kind = Kind::Torus;
  c.id = js::cat("torus:", size);
  c.size = size;
  c.R = size / (2 * kPi);
  c.has_latitude = latitude;
  return c;
}

FieldPoint Chart::cube_field(double x, double y) const {
  const double a = x / half;
  const double b = y / half;
  double px, py, pz;
  if (face == 0) {
    px = 1, py = a, pz = b;
  } else if (face == 1) {
    px = -1, py = -a, pz = b;
  } else if (face == 2) {
    px = -a, py = 1, pz = b;
  } else if (face == 3) {
    px = a, py = -1, pz = b;
  } else if (face == 4) {
    px = a, py = b, pz = 1;
  } else {
    px = a, py = -b, pz = -1;
  }
  const double len = js::hypot(px, py, pz);
  return {(px / len) * radius, (py / len) * radius, (pz / len) * radius, js::kNaN};
}

FieldPoint Chart::torus_field(double x, double y) const {
  const double S = size;
  // canonical position first: x and x + size give bit-identical fields
  const double a = ((std::fmod(std::fmod(x, S) + S, S) / S) * 2) * kPi;
  const double b = ((std::fmod(std::fmod(y, S) + S, S) / S) * 2) * kPi;
  return {R * js::cos(a), R * js::sin(a), R * js::cos(b), R * js::sin(b)};
}

std::optional<double> Chart::latitude(double fx, double fy, double fz, double fw) const {
  switch (kind) {
    case Kind::Flat: return std::nullopt;
    case Kind::Cube: return fz / radius;
    default: {
      // once round the world in y the climate runs from an equator to a pole and back; the origin
      // sits at mid latitudes
      if (!has_latitude) return std::nullopt;
      const double b = js::atan2(fw, fz);
      return js::sin(b / 2 + kPi / 6);
    }
  }
}

Chart make_chart(const Value& world_cfg) {
  const Value& kind = world_cfg.is_string() ? world_cfg : world_cfg["chart"];
  if (!kind.truthy() || (kind.is_string() && kind.str() == "flat")) return Chart();
  if (kind.is_string() && kind.str() == "torus") {
    const Value& lat = world_cfg["latitude"];
    return Chart::torus(world_cfg["size"].num(96000), lat.is_nullish() ? true : lat.truthy());
  }
  if (kind.is_string() && kind.str() == "cube") return Chart::cube(world_cfg["planet"]["radius"].num(240000), world_cfg["planet"]["face"].num(0));
  std::fprintf(stderr, "svx_city: chart \"%s\" is not implemented\n", kind.to_string().c_str());
  SVX_FAIL("city: unknown chart");
}

}  // namespace svx::city
