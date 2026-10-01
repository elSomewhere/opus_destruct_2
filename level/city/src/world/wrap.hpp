// svx_city — periodic lattices of a wrapping world (voxel_city world/wrap.js).
//
// A wrapping world (config.world.chart = 'torus') repeats every `size` metres, so a lattice with n
// cells round the world hashes the canonical index mod(i, n) and places cell i at its canonical
// position plus lap(i) * size. On unbounded charts every helper is the identity, so planar worlds
// are unchanged. Positions stay unwrapped everywhere (the plane is self-consistent and has no
// seam); only the seeds are canonical, so every lap holds the same towns, streets and buildings.
#pragma once

#include "core/js.hpp"
#include "core/math.hpp"
#include "core/value.hpp"

namespace svx::city {

struct Wrap {
  Wrap() = default;
  explicit Wrap(const Value& config) {
    on = config["world"]["chart"].is_string() && config["world"]["chart"].str() == "torus";
    size = on ? config["world"]["size"].to_number() : 0;
    size_v = size * kVoxelsPerMeter;
  }

  bool on = false;
  // the period in metres and in voxels (0 when the world does not wrap)
  double size = 0, size_v = 0;

  // count(spacingM): cells of a lattice with that spacing (m) round the world; 0 = unbounded.
  double count(double spacing_m) const { return on ? js::max(1.0, js::round(size / spacing_m)) : 0; }
  // canon(i, n): the canonical index of cell i on a lattice of n cells round the world.
  static double canon(double i, double n) { return js::truthy(n) ? std::fmod(std::fmod(i, n) + n, n) : i; }
  // lap(i, n): how many times round the world cell i is (0 on unbounded charts).
  static double lap(double i, double n) { return js::truthy(n) ? std::floor(i / n) : 0; }
  // v(x): the canonical coordinate (voxels) for position hashes.
  double v(double x) const { return on ? std::fmod(std::fmod(x, size_v) + size_v, size_v) : x; }
  // vi(x): the canonical integer coordinate (voxels) for position hashes (Math.round elsewhere).
  double vi(double x) const { return on ? std::fmod(js::round(v(x)), size_v) : js::round(x); }
};

// wrapOf(config): the Wrap of a config (the reference caches it per config object; it is a few
// numbers, made from the config when asked: hoist it out of loops).
inline Wrap wrap_of(const Value& config) { return Wrap(config); }

}  // namespace svx::city
