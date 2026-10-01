// svx_city — city/diagonals.hpp (voxel_city city/diagonals.js).
#include "city/diagonals.hpp"

#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "core/placement.hpp"

namespace svx::city {

namespace {

constexpr double kJitter = 0.25;

DiagonalFamily family(double f, int yaw) {
  const Yaw& y = yaws()[static_cast<size_t>(yaw)];
  DiagonalFamily d;
  d.f = f;
  d.yaw = yaw;
  d.c = y.c;
  d.s = y.s;
  d.r = y.r;
  return d;
}

}  // namespace

const std::array<DiagonalFamily, 2>& diagonal_families() {
  static const std::array<DiagonalFamily, 2> fams = {family(0, nearest_yaw(4, 3)), family(1, nearest_yaw(4, -3))};
  return fams;
}

double diagonal_offset(double seed, const DiagonalFamily& fam, double k, double spacing) {
  const double j = (hash_float(seed, k, fam.f, 0xd1a6) - 0.5) * 2 * kJitter * spacing;
  return js::round(fam.r * (k * spacing + j));
}

bool diagonals_on(const Value& config) {
  const Value& a = config["world"]["angles"];
  if (!a["enabled"].truthy()) return false;
  const Value& roads = a["features"]["roads"];
  if (roads.is_bool() && !roads.truthy()) return false;
  const Value& chart = config["world"]["chart"];
  return chart.is_nullish() ? true : chart.is_string() && chart.str() == "flat";
}

std::vector<DiagonalPiece> diagonal_pieces(const Value& config, const Rect& rect) {
  const double spacing = vx(config["world"]["angles"]["diagonalSpacing"].num(2000));
  const double seed = config["seed"].to_number();
  std::vector<DiagonalPiece> out;
  for (const DiagonalFamily& fam : diagonal_families()) {
    auto L = [&](double x, double y) { return -fam.s * x + fam.c * y; };
    const double v0 = L(rect.x0, rect.y0);
    const double v1 = L(rect.x1, rect.y0);
    const double v2 = L(rect.x0, rect.y1);
    const double v3 = L(rect.x1, rect.y1);
    const double lo = js::min(v0, v1, v2, v3);
    const double hi = js::max(v0, v1, v2, v3);
    const double k0 = std::floor(lo / fam.r / spacing - kJitter) - 1;
    const double k1 = std::ceil(hi / fam.r / spacing + kJitter) + 1;
    for (double k = k0; k <= k1; k += 1) {
      const double D = diagonal_offset(seed, fam, k, spacing);
      if (D <= lo || D >= hi) continue;
      // crossings with the cell's four edge lines (each edge line computed the same way by both
      // cells that share it)
      std::vector<Point2> pts;
      for (const double x : {rect.x0, rect.x1}) {
        const double y = (D + fam.s * x) / fam.c;
        if (y >= rect.y0 && y <= rect.y1) pts.push_back({x, y});
      }
      for (const double y : {rect.y0, rect.y1}) {
        const double x = (fam.c * y - D) / fam.s;
        if (x >= rect.x0 && x <= rect.x1) pts.push_back({x, y});
      }
      if (pts.size() < 2) continue;
      // the two ends along the family's direction
      js::sort(pts, [&](const Point2& p, const Point2& q) { return fam.c * p.x + fam.s * p.y - (fam.c * q.x + fam.s * q.y); });
      const Point2 a = pts.front();
      const Point2 b = pts.back();
      if (js::hypot(b.x - a.x, b.y - a.y) < 1) continue;
      out.push_back({&fam, k, D, a, b});
    }
  }
  return out;
}

}  // namespace svx::city
