// svx_city — seeded simplex noise in 2D, 3D and 4D and fractal sums (voxel_city core/noise.js).
// Output about [-1, 1] (ridged: [0, 1]). The 3D and 4D kernels have a radius^2 of 0.5, so the
// fields are continuous at cell boundaries.
#pragma once

#include <array>
#include <cstdint>

#include "core/js.hpp"

namespace svx::city {

class SimplexNoise {
 public:
  explicit SimplexNoise(double seed);
  double n2(double x, double y) const;
  double n3(double x, double y, double z) const;
  double n4(double x, double y, double z, double w) const;
  double fbm2(double x, double y, int octaves = 4, double lacunarity = 2, double gain = 0.5) const;
  double fbm3(double x, double y, double z, int octaves = 4, double lacunarity = 2, double gain = 0.5) const;
  double fbm4(double x, double y, double z, double w, int octaves = 4, double lacunarity = 2, double gain = 0.5) const;
  double ridged2(double x, double y, int octaves = 5) const;
  double ridged3(double x, double y, double z, int octaves = 6, double lacunarity = 2.05, double gain = 0.5) const;
  double ridged4(double x, double y, double z, double w, int octaves = 6, double lacunarity = 2.05, double gain = 0.5) const;
  // Chart-agnostic sampling: w NaN (JS: undefined) is the 3D noise, else the 4D one.
  double nP(double x, double y, double z, double w) const { return w == w ? n4(x, y, z, w) : n3(x, y, z); }
  double fbmP(double x, double y, double z, double w, int octaves = 4, double lacunarity = 2, double gain = 0.5) const {
    return w == w ? fbm4(x, y, z, w, octaves, lacunarity, gain) : fbm3(x, y, z, octaves, lacunarity, gain);
  }
  double ridgedP(double x, double y, double z, double w, int octaves = 6, double lacunarity = 2.05, double gain = 0.5) const {
    return w == w ? ridged4(x, y, z, w, octaves, lacunarity, gain) : ridged3(x, y, z, octaves, lacunarity, gain);
  }

 private:
  std::array<uint8_t, 512> perm_{};
  std::array<uint8_t, 512> perm_mod12_{};
};

}  // namespace svx::city
