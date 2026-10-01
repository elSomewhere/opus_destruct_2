// svx_city — voxel_city core/noise.js, operation for operation.
#include "core/noise.hpp"

#include "core/hash.hpp"

namespace svx::city {

namespace {

const double F2 = 0.5 * (std::sqrt(3.0) - 1);
const double G2 = (3 - std::sqrt(3.0)) / 6;
constexpr double F3 = 1.0 / 3;
constexpr double G3 = 1.0 / 6;
constexpr double R3 = 0.5;
constexpr double K3 = 82;
const double F4 = (std::sqrt(5.0) - 1) / 4;
const double G4 = (5 - std::sqrt(5.0)) / 20;
constexpr double R4 = 0.5;
constexpr double K4 = 104;

constexpr float GRAD4[128] = {
    0, 1, 1, 1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, -1, -1, 0, -1, 1, 1, 0, -1, 1, -1, 0, -1, -1, 1, 0, -1, -1, -1,
    1, 0, 1, 1, 1, 0, 1, -1, 1, 0, -1, 1, 1, 0, -1, -1, -1, 0, 1, 1, -1, 0, 1, -1, -1, 0, -1, 1, -1, 0, -1, -1,
    1, 1, 0, 1, 1, 1, 0, -1, 1, -1, 0, 1, 1, -1, 0, -1, -1, 1, 0, 1, -1, 1, 0, -1, -1, -1, 0, 1, -1, -1, 0, -1,
    1, 1, 1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, -1, -1, 0, -1, 1, 1, 0, -1, 1, -1, 0, -1, -1, 1, 0, -1, -1, -1, 0,
};
constexpr float GRAD3[36] = {
    1, 1, 0, -1, 1, 0, 1, -1, 0, -1, -1, 0, 1, 0, 1, -1, 0, 1, 1, 0, -1, -1, 0, -1, 0, 1, 1, 0, -1, 1, 0, 1, -1, 0, -1, -1,
};

inline int and255(double v) { return js::to_int32(v) & 255; }

}  // namespace

SimplexNoise::SimplexNoise(double seed) {
  Rng rng(seed);
  std::array<uint8_t, 256> p{};
  for (int i = 0; i < 256; ++i) p[size_t(i)] = static_cast<uint8_t>(i);
  for (int i = 255; i > 0; --i) {
    const int j = static_cast<int>(std::floor(rng.next() * (i + 1)));
    std::swap(p[size_t(i)], p[size_t(j)]);
  }
  for (int i = 0; i < 512; ++i) {
    perm_[size_t(i)] = p[size_t(i & 255)];
    perm_mod12_[size_t(i)] = static_cast<uint8_t>(perm_[size_t(i)] % 12);
  }
}

double SimplexNoise::n2(double xin, double yin) const {
  const auto& perm = perm_;
  const auto& pm = perm_mod12_;
  double n0 = 0, n1 = 0, n2v = 0;
  const double s = (xin + yin) * F2;
  const double i = std::floor(xin + s);
  const double j = std::floor(yin + s);
  const double t = (i + j) * G2;
  const double x0 = xin - (i - t);
  const double y0 = yin - (j - t);
  int i1, j1;
  if (x0 > y0) {
    i1 = 1;
    j1 = 0;
  } else {
    i1 = 0;
    j1 = 1;
  }
  const double x1 = x0 - i1 + G2;
  const double y1 = y0 - j1 + G2;
  const double x2 = x0 - 1 + 2 * G2;
  const double y2 = y0 - 1 + 2 * G2;
  const int ii = and255(i), jj = and255(j);
  double t0 = 0.5 - x0 * x0 - y0 * y0;
  if (t0 >= 0) {
    const int gi = pm[size_t(ii + perm[size_t(jj)])] * 3;
    t0 *= t0;
    n0 = t0 * t0 * (GRAD3[gi] * x0 + GRAD3[gi + 1] * y0);
  }
  double t1 = 0.5 - x1 * x1 - y1 * y1;
  if (t1 >= 0) {
    const int gi = pm[size_t(ii + i1 + perm[size_t(jj + j1)])] * 3;
    t1 *= t1;
    n1 = t1 * t1 * (GRAD3[gi] * x1 + GRAD3[gi + 1] * y1);
  }
  double t2 = 0.5 - x2 * x2 - y2 * y2;
  if (t2 >= 0) {
    const int gi = pm[size_t(ii + 1 + perm[size_t(jj + 1)])] * 3;
    t2 *= t2;
    n2v = t2 * t2 * (GRAD3[gi] * x2 + GRAD3[gi + 1] * y2);
  }
  return 70 * (n0 + n1 + n2v);
}

double SimplexNoise::n3(double xin, double yin, double zin) const {
  const auto& perm = perm_;
  const auto& pm = perm_mod12_;
  double n0 = 0, n1 = 0, n2v = 0, n3v = 0;
  const double s = (xin + yin + zin) * F3;
  const double i = std::floor(xin + s);
  const double j = std::floor(yin + s);
  const double k = std::floor(zin + s);
  const double t = (i + j + k) * G3;
  const double x0 = xin - (i - t);
  const double y0 = yin - (j - t);
  const double z0 = zin - (k - t);
  int i1, j1, k1, i2, j2, k2;
  if (x0 >= y0) {
    if (y0 >= z0) {
      i1 = 1, j1 = 0, k1 = 0, i2 = 1, j2 = 1, k2 = 0;
    } else if (x0 >= z0) {
      i1 = 1, j1 = 0, k1 = 0, i2 = 1, j2 = 0, k2 = 1;
    } else {
      i1 = 0, j1 = 0, k1 = 1, i2 = 1, j2 = 0, k2 = 1;
    }
  } else if (y0 < z0) {
    i1 = 0, j1 = 0, k1 = 1, i2 = 0, j2 = 1, k2 = 1;
  } else if (x0 < z0) {
    i1 = 0, j1 = 1, k1 = 0, i2 = 0, j2 = 1, k2 = 1;
  } else {
    i1 = 0, j1 = 1, k1 = 0, i2 = 1, j2 = 1, k2 = 0;
  }
  const double x1 = x0 - i1 + G3, y1 = y0 - j1 + G3, z1 = z0 - k1 + G3;
  const double x2 = x0 - i2 + 2 * G3, y2 = y0 - j2 + 2 * G3, z2 = z0 - k2 + 2 * G3;
  const double x3 = x0 - 1 + 3 * G3, y3 = y0 - 1 + 3 * G3, z3 = z0 - 1 + 3 * G3;
  const int ii = and255(i), jj = and255(j), kk = and255(k);
  double t0 = R3 - x0 * x0 - y0 * y0 - z0 * z0;
  if (t0 > 0) {
    const int gi = pm[size_t(ii + perm[size_t(jj + perm[size_t(kk)])])] * 3;
    t0 *= t0;
    n0 = t0 * t0 * (GRAD3[gi] * x0 + GRAD3[gi + 1] * y0 + GRAD3[gi + 2] * z0);
  }
  double t1 = R3 - x1 * x1 - y1 * y1 - z1 * z1;
  if (t1 > 0) {
    const int gi = pm[size_t(ii + i1 + perm[size_t(jj + j1 + perm[size_t(kk + k1)])])] * 3;
    t1 *= t1;
    n1 = t1 * t1 * (GRAD3[gi] * x1 + GRAD3[gi + 1] * y1 + GRAD3[gi + 2] * z1);
  }
  double t2 = R3 - x2 * x2 - y2 * y2 - z2 * z2;
  if (t2 > 0) {
    const int gi = pm[size_t(ii + i2 + perm[size_t(jj + j2 + perm[size_t(kk + k2)])])] * 3;
    t2 *= t2;
    n2v = t2 * t2 * (GRAD3[gi] * x2 + GRAD3[gi + 1] * y2 + GRAD3[gi + 2] * z2);
  }
  double t3 = R3 - x3 * x3 - y3 * y3 - z3 * z3;
  if (t3 > 0) {
    const int gi = pm[size_t(ii + 1 + perm[size_t(jj + 1 + perm[size_t(kk + 1)])])] * 3;
    t3 *= t3;
    n3v = t3 * t3 * (GRAD3[gi] * x3 + GRAD3[gi + 1] * y3 + GRAD3[gi + 2] * z3);
  }
  return K3 * (n0 + n1 + n2v + n3v);
}

double SimplexNoise::n4(double x, double y, double z, double w) const {
  const auto& perm = perm_;
  const double s = (x + y + z + w) * F4;
  const double i = std::floor(x + s);
  const double j = std::floor(y + s);
  const double k = std::floor(z + s);
  const double l = std::floor(w + s);
  const double t = (i + j + k + l) * G4;
  const double x0 = x - (i - t), y0 = y - (j - t), z0 = z - (k - t), w0 = w - (l - t);
  int rx = 0, ry = 0, rz = 0, rw = 0;
  if (x0 > y0) rx += 1; else ry += 1;
  if (x0 > z0) rx += 1; else rz += 1;
  if (x0 > w0) rx += 1; else rw += 1;
  if (y0 > z0) ry += 1; else rz += 1;
  if (y0 > w0) ry += 1; else rw += 1;
  if (z0 > w0) rz += 1; else rw += 1;
  const int ii = and255(i), jj = and255(j), kk = and255(k), ll = and255(l);
  double sum = 0;
  for (int c = 0; c < 5; ++c) {
    const int oi = c == 0 ? 0 : c == 4 ? 1 : rx >= 4 - c ? 1 : 0;
    const int oj = c == 0 ? 0 : c == 4 ? 1 : ry >= 4 - c ? 1 : 0;
    const int ok = c == 0 ? 0 : c == 4 ? 1 : rz >= 4 - c ? 1 : 0;
    const int ol = c == 0 ? 0 : c == 4 ? 1 : rw >= 4 - c ? 1 : 0;
    const double dx = x0 - oi + c * G4;
    const double dy = y0 - oj + c * G4;
    const double dz = z0 - ok + c * G4;
    const double dw = w0 - ol + c * G4;
    double tt = R4 - dx * dx - dy * dy - dz * dz - dw * dw;
    if (tt <= 0) continue;
    const int gi = (perm[size_t(ii + oi + perm[size_t(jj + oj + perm[size_t(kk + ok + perm[size_t(ll + ol)])])])] & 31) * 4;
    tt *= tt;
    sum += tt * tt * (GRAD4[gi] * dx + GRAD4[gi + 1] * dy + GRAD4[gi + 2] * dz + GRAD4[gi + 3] * dw);
  }
  return K4 * sum;
}

double SimplexNoise::fbm2(double x, double y, int octaves, double lacunarity, double gain) const {
  double amp = 1, freq = 1, sum = 0, norm = 0;
  for (int o = 0; o < octaves; ++o) {
    sum += amp * n2(x * freq + o * 17.31, y * freq - o * 9.73);
    norm += amp;
    amp *= gain;
    freq *= lacunarity;
  }
  return sum / norm;
}

double SimplexNoise::fbm3(double x, double y, double z, int octaves, double lacunarity, double gain) const {
  double amp = 1, freq = 1, sum = 0, norm = 0;
  for (int o = 0; o < octaves; ++o) {
    sum += amp * n3(x * freq + o * 17.31, y * freq - o * 9.73, z * freq + o * 3.17);
    norm += amp;
    amp *= gain;
    freq *= lacunarity;
  }
  return sum / norm;
}

double SimplexNoise::fbm4(double x, double y, double z, double w, int octaves, double lacunarity, double gain) const {
  double amp = 1, freq = 1, sum = 0, norm = 0;
  for (int o = 0; o < octaves; ++o) {
    sum += amp * n4(x * freq + o * 17.31, y * freq - o * 9.73, z * freq + o * 3.17, w * freq - o * 5.41);
    norm += amp;
    amp *= gain;
    freq *= lacunarity;
  }
  return sum / norm;
}

double SimplexNoise::ridged2(double x, double y, int octaves) const {
  double amp = 0.5, freq = 1, sum = 0, norm = 0, weight = 1;
  for (int o = 0; o < octaves; ++o) {
    double n = 1 - std::fabs(n2(x * freq + o * 31.7, y * freq + o * 11.3));
    n *= n;
    n *= weight;
    weight = js::min(1.0, n * 2);
    sum += n * amp;
    norm += amp;
    amp *= 0.5;
    freq *= 2.1;
  }
  return sum / norm;
}

double SimplexNoise::ridged3(double x, double y, double z, int octaves, double lacunarity, double gain) const {
  double amp = 0.5, freq = 1, sum = 0, norm = 0, weight = 1;
  for (int o = 0; o < octaves; ++o) {
    double n = 1 - std::fabs(n3(x * freq + o * 31.7, y * freq + o * 11.3, z * freq - o * 7.9));
    n *= n;
    n *= weight;
    weight = js::min(1.0, n * 1.8);
    sum += n * amp;
    norm += amp;
    amp *= gain;
    freq *= lacunarity;
  }
  return sum / norm;
}

double SimplexNoise::ridged4(double x, double y, double z, double w, int octaves, double lacunarity, double gain) const {
  double amp = 0.5, freq = 1, sum = 0, norm = 0, weight = 1;
  for (int o = 0; o < octaves; ++o) {
    double n = 1 - std::fabs(n4(x * freq + o * 31.7, y * freq + o * 11.3, z * freq - o * 7.9, w * freq + o * 4.3));
    n *= n;
    n *= weight;
    weight = js::min(1.0, n * 1.8);
    sum += n * amp;
    norm += amp;
    amp *= gain;
    freq *= lacunarity;
  }
  return sum / norm;
}

}  // namespace svx::city
