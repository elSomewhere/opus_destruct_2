// svx_city — voxel_city core/placement.js.
#include "core/placement.hpp"

namespace svx::city {

const std::array<std::array<int, 3>, 16> kYawTriples = {{
    {13, 84, 85}, {11, 60, 61}, {9, 40, 41}, {16, 63, 65}, {7, 24, 25}, {12, 35, 37}, {5, 12, 13}, {36, 77, 85},
    {39, 80, 89}, {8, 15, 17}, {33, 56, 65}, {28, 45, 53}, {3, 4, 5}, {48, 55, 73}, {65, 72, 97}, {20, 21, 29},
}};
const std::array<int, 6> kPitchN = {25, 20, 16, 14, 12, 10};

namespace {

double gcd(double a, double b) {
  a = std::fabs(a);
  b = std::fabs(b);
  while (b != 0) {
    const double t = std::fmod(a, b);
    a = b;
    b = t;
  }
  return a;
}

std::vector<Yaw> make_yaws() {
  // a quadrant [0, 90): the identity, the 16 triples, then their mirrors (90 - theta), by angle
  std::vector<std::array<double, 3>> quad;
  quad.push_back({1, 0, 1});
  for (const auto& t : kYawTriples) quad.push_back({double(t[1]), double(t[0]), double(t[2])});
  for (int i = 15; i >= 0; --i) quad.push_back({double(kYawTriples[size_t(i)][0]), double(kYawTriples[size_t(i)][1]), double(kYawTriples[size_t(i)][2])});
  std::vector<Yaw> out;
  for (int k = 0; k < 4; ++k)
    for (const auto& q : quad) {
      double c = q[0], s = q[1];
      for (int t = 0; t < k; ++t) {
        const double nc = 0 - s, ns = c;  // (0 - s: a quarter turn of a zero sine is +0)
        c = nc;
        s = ns;
      }
      out.push_back({c, s, q[2], 0});
    }
  return out;
}

std::vector<Yaw> make_pitches() {
  std::vector<Yaw> out;
  out.push_back({1, 0, 1, 0});
  for (int n : kPitchN) {
    const double g = gcd(gcd(2.0 * n, double(n) * n - 1), double(n) * n + 1);
    out.push_back({(double(n) * n - 1) / g, (2.0 * n) / g, (double(n) * n + 1) / g, double(n)});
  }
  return out;
}

const Yaw& yaw_entry(int i) {
  const auto& y = yaws();
  if (i < 0 || i >= static_cast<int>(y.size())) SVX_FAIL("placement: no such yaw");
  return y[static_cast<size_t>(i)];
}

Yaw pitch_entry(int i) {
  const auto& p = pitches();
  const int a = i < 0 ? -i : i;
  if (a >= static_cast<int>(p.size())) SVX_FAIL("placement: no such pitch");
  Yaw e = p[static_cast<size_t>(a)];
  if (i < 0) e.s = -e.s;
  return e;
}

std::array<double, 9> mul3(const std::array<double, 9>& a, const std::array<double, 9>& b) {
  std::array<double, 9> o{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) o[size_t(i * 3 + j)] = a[size_t(i * 3)] * b[size_t(j)] + a[size_t(i * 3 + 1)] * b[size_t(3 + j)] + a[size_t(i * 3 + 2)] * b[size_t(6 + j)];
  return o;
}

}  // namespace

const std::vector<Yaw>& yaws() {
  static const std::vector<Yaw> y = make_yaws();
  return y;
}
const std::vector<Yaw>& pitches() {
  static const std::vector<Yaw> p = make_pitches();
  return p;
}

Yaw yaw_product(int a, int b) {
  const Yaw& A = yaw_entry(a);
  const Yaw& B = yaw_entry(b);
  const double c = A.c * B.c - A.s * B.s;
  const double s = A.c * B.s + A.s * B.c;
  const double r = A.r * B.r;
  const double g = gcd(gcd(c, s), r);
  return {c / g + 0, s / g + 0, r / g, 0};  // (+ 0: never -0)
}

int yaw_index(const Yaw& y) {
  const auto& all = yaws();
  for (size_t i = 0; i < all.size(); ++i)
    if (all[i].c == y.c && all[i].s == y.s && all[i].r == y.r) return static_cast<int>(i);
  return -1;
}

Yaw yaw_vector(int yaw, int yaw2) { return yaw2 ? yaw_product(yaw, yaw2) : yaw_entry(yaw); }

RotMatrix rotation_matrix(int yaw, int pitch, int roll, int yaw2) {
  const Yaw Y = yaw_vector(yaw, yaw2);
  const Yaw P = pitch_entry(pitch);
  const Yaw& R = yaw_entry(roll);
  const std::array<double, 9> rz = {Y.c, 0 - Y.s, 0, Y.s, Y.c, 0, 0, 0, Y.r};
  const std::array<double, 9> ry = {P.c, 0, 0 - P.s, 0, P.r, 0, P.s, 0, P.c};
  const std::array<double, 9> rx = {R.r, 0, 0, 0, R.c, 0 - R.s, 0, R.s, R.c};
  RotMatrix out;
  out.m = mul3(mul3(rz, ry), rx);
  for (double& v : out.m) v = v + 0;  // (a sum of zero products is +0)
  out.d = Y.r * P.r * R.r;
  double g = out.d;
  for (double v : out.m) g = gcd(g, v);
  if (g > 1) {
    for (double& v : out.m) v /= g;
    out.d /= g;
  }
  return out;
}

QuatXYZW matrix_quat(const std::array<double, 9>& m, double d) {
  const double m00 = m[0], m01 = m[1], m02 = m[2], m10 = m[3], m11 = m[4], m12 = m[5], m20 = m[6], m21 = m[7], m22 = m[8];
  const double d4 = 4 * d;
  const double W = d + m00 + m11 + m22;
  const double X = d + m00 - m11 - m22;
  const double Y = d - m00 + m11 - m22;
  const double Z = d - m00 - m11 + m22;
  const double w = std::sqrt(W / d4);
  double x = std::sqrt(X / d4), y = std::sqrt(Y / d4), z = std::sqrt(Z / d4);
  if (W > 0) {
    if (m21 - m12 < 0) x = -x;
    if (m02 - m20 < 0) y = -y;
    if (m10 - m01 < 0) z = -z;
  } else if (X >= Y && X >= Z) {
    if (m01 + m10 < 0) y = -y;
    if (m02 + m20 < 0) z = -z;
  } else if (Y >= Z) {
    if (m01 + m10 < 0) x = -x;
    if (m12 + m21 < 0) z = -z;
  } else {
    if (m02 + m20 < 0) x = -x;
    if (m12 + m21 < 0) y = -y;
  }
  return {x + 0, y + 0, z + 0, w};
}

int nearest_yaw(double dx, double dy, const std::vector<int>* allowed) {
  const auto& all = yaws();
  int best = 0;
  double best_cos = -js::kInf;
  const size_t n = allowed ? allowed->size() : all.size();
  for (size_t k = 0; k < n; ++k) {
    const int i = allowed ? (*allowed)[k] : static_cast<int>(k);
    const Yaw& y = all[static_cast<size_t>(i)];
    const double cos = (y.c * dx + y.s * dy) / y.r;
    if (cos > best_cos) {
      best_cos = cos;
      best = i;
    }
  }
  return best;
}

const std::vector<int> kCardinalYaws = {0, kYawQuarter, 2 * kYawQuarter, 3 * kYawQuarter};
const std::vector<int>& all_yaws() {
  static const std::vector<int> a = [] {
    std::vector<int> v;
    for (size_t i = 0; i < yaws().size(); ++i) v.push_back(static_cast<int>(i));
    return v;
  }();
  return a;
}

Placement::Placement(const PlacementOpts& o) {
  if (o.h != 0.125) SVX_FAIL("placement: only the world voxel size is supported");
  int y = o.yaw, y2 = o.yaw2;
  if (y2) {
    const int k = yaw_index(yaw_product(y, y2));
    if (k >= 0) {
      y = k;
      y2 = 0;
    }
  }
  origin = o.origin;
  yaw = y;
  yaw2 = y2;
  pitch = o.pitch;
  roll = o.roll;
  h = o.h;
  priority = o.priority;
  anchored = o.anchored;
  extent = o.extent;
  const RotMatrix r = rotation_matrix(yaw, pitch, roll, yaw2);
  m = r.m;
  d = r.d;
  flat = pitch == 0 && roll == 0;
  q = (flat && yaw2 == 0 && yaw % kYawQuarter == 0) ? yaw / kYawQuarter : -1;
  if (q >= 0) {
    px = origin.x - ((q == 1 || q == 2) ? 1 : 0);
    py = origin.y - ((q == 2 || q == 3) ? 1 : 0);
  }
}

Placement Placement::cardinal(double px, double py, int q, PlacementOpts opts) {
  opts.origin = {px + ((q == 1 || q == 2) ? 1 : 0), py + ((q == 2 || q == 3) ? 1 : 0), opts.origin.z};
  opts.yaw = q * kYawQuarter;
  Placement p(opts);
  p.px = px;
  p.py = py;
  return p;
}

std::array<double, 2> Placement::to_local_xy(double x, double y) const {
  switch (q) {
    case 0: return {x - px, y - py};
    case 1: return {y - py, px - x};
    case 2: return {px - x, py - y};
    case 3: return {py - y, x - px};
    default: {
      const double dx = 2 * (x - origin.x) + 1, dy = 2 * (y - origin.y) + 1, d2 = 2 * d;
      return {floor_div_exact(m[0] * dx + m[3] * dy, d2), floor_div_exact(m[1] * dx + m[4] * dy, d2)};
    }
  }
}

std::array<double, 2> Placement::to_world_xy(double u, double v) const {
  switch (q) {
    case 0: return {px + u, py + v};
    case 1: return {px - v, py + u};
    case 2: return {px - u, py - v};
    case 3: return {px + v, py - u};
    default: {
      const double du = 2 * u + 1, dv = 2 * v + 1, d2 = 2 * d;
      return {origin.x + floor_div_exact(m[0] * du + m[1] * dv, d2), origin.y + floor_div_exact(m[3] * du + m[4] * dv, d2)};
    }
  }
}

std::array<double, 2> Placement::dir_to_world_xy(double du, double dv) const {
  switch (q) {
    case 0: return {du, dv};
    case 1: return {-dv, du};
    case 2: return {-du, -dv};
    case 3: return {dv, -du};
    default: return {(m[0] * du + m[1] * dv) / d, (m[3] * du + m[4] * dv) / d};
  }
}

std::array<double, 3> Placement::to_local(double x, double y, double z) const {
  if (flat) {
    const auto uv = to_local_xy(x, y);
    return {uv[0], uv[1], z - origin.z};
  }
  const double dx = 2 * (x - origin.x) + 1, dy = 2 * (y - origin.y) + 1, dz = 2 * (z - origin.z) + 1, d2 = 2 * d;
  return {floor_div_exact(m[0] * dx + m[3] * dy + m[6] * dz, d2), floor_div_exact(m[1] * dx + m[4] * dy + m[7] * dz, d2),
          floor_div_exact(m[2] * dx + m[5] * dy + m[8] * dz, d2)};
}

std::array<double, 3> Placement::to_world(double u, double v, double w) const {
  if (flat) {
    const auto xy = to_world_xy(u, v);
    return {xy[0], xy[1], w + origin.z};
  }
  const auto S = to_world_scaled(u, v, w);
  const double d2 = 2 * d;
  return {origin.x + floor_div_exact(S[0], d2), origin.y + floor_div_exact(S[1], d2), origin.z + floor_div_exact(S[2], d2)};
}

std::array<double, 3> Placement::to_world_scaled(double u, double v, double w) const {
  const double du = 2 * u + 1, dv = 2 * v + 1, dw = 2 * w + 1;
  return {m[0] * du + m[1] * dv + m[2] * dw, m[3] * du + m[4] * dv + m[5] * dw, m[6] * du + m[7] * dv + m[8] * dw};
}

Box3 Placement::local_bounds_to_world_aabb(const LocalBox& e) const {
  const double w0 = e.w0, w1 = e.w1;
  if (q >= 0) {
    const auto a = to_world_xy(e.u0, e.v0);
    const auto b = to_world_xy(e.u1, e.v1);
    return {js::min(a[0], b[0]), js::min(a[1], b[1]), w0 + origin.z, js::max(a[0], b[0]), js::max(a[1], b[1]), w1 + origin.z};
  }
  double lo[3] = {js::kInf, js::kInf, js::kInf}, hi[3] = {-js::kInf, -js::kInf, -js::kInf};
  const double us[2] = {e.u0, e.u1 + 1}, vs[2] = {e.v0, e.v1 + 1}, ws[2] = {w0, w1 + 1};
  for (double u : us)
    for (double v : vs)
      for (double w : ws)
        for (int a = 0; a < 3; ++a) {
          const double n = m[size_t(a * 3)] * u + m[size_t(a * 3 + 1)] * v + m[size_t(a * 3 + 2)] * w;
          if (n < lo[a]) lo[a] = n;
          if (n > hi[a]) hi[a] = n;
        }
  auto cell = [&](double n, bool up) { return up ? ceil_div_exact(2 * n - d, 2 * d) : floor_div_exact(2 * n - d, 2 * d); };
  return {origin.x + cell(lo[0], true), origin.y + cell(lo[1], true), origin.z + cell(lo[2], true),
          origin.x + cell(hi[0], false), origin.y + cell(hi[1], false), origin.z + cell(hi[2], false)};
}

std::optional<Box3> Placement::world_aabb() const {
  if (!extent) return std::nullopt;
  return local_bounds_to_world_aabb(*extent);
}

}  // namespace svx::city
