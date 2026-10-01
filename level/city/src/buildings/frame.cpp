// svx_city — voxel_city buildings/frame.js.
#include "buildings/frame.hpp"

#include <cmath>

#include "svx/base/types.hpp"

namespace svx::city {

namespace {

// SIDE_MAP[front]: the world sides of F, B, L, R (null: a front that is none of N, E, S, W).
const char* side_row(char front) {
  return front == 'N' ? "NSWE" : front == 'S' ? "SNEW" : front == 'W' ? "WESN" : front == 'E' ? "EWNS" : nullptr;
}

}  // namespace

Frame::Frame(const Rect& R_, char front_) : R(R_), front(front_) {
  const double w = R_.x1 - R_.x0 + 1;
  const double h = R_.y1 - R_.y0 + 1;
  if (front_ == 'N' || front_ == 'S') {
    U = w;
    V = h;
  } else {
    U = h;
    V = w;
  }
  // (QUARTER[front] ?? 1: anything else turns like E)
  const int q = front_ == 'N' ? 0 : front_ == 'E' ? 1 : front_ == 'S' ? 2 : front_ == 'W' ? 3 : 1;
  PlacementOpts o;
  o.extent = LocalBox{0, 0, 0, U - 1, V - 1, 0};
  placement = Placement::cardinal((q == 0 || q == 3) ? R_.x0 : R_.x1, (q == 0 || q == 1) ? R_.y0 : R_.y1, q, o);
}

TurnedFrame::TurnedFrame(const Placement& p, double ou_, double ov_, double U_, double V_, char front_)
    : Frame(obb_bounds(p, Rect{ou_, ov_, ou_ + U_ - 1, ov_ + V_ - 1}), front_) {
  U = U_;
  V = V_;
  placement = p;
  ou = ou_;
  ov = ov_;
  turned = true;
}

XY Frame::to_world(double u, double v) const {
  if (turned) return placement.to_world_xy(u + ou, v + ov);
  return placement.to_world_xy(u, v);
}

XY Frame::from_world(double x, double y) const {
  const XY uv = placement.to_local_xy(x, y);
  if (turned) return {uv[0] - ou, uv[1] - ov};
  return uv;
}

Rect Frame::rect_to_world(const Rect& r) const {
  if (turned)
    return obb_bounds(placement, Rect{std::ceil(r.x0) + ou, std::ceil(r.y0) + ov, std::floor(r.x1) + ou, std::floor(r.y1) + ov});
  const XY a = to_world(r.x0, r.y0);
  const XY b = to_world(r.x1, r.y1);
  return {js::min(a[0], b[0]), js::min(a[1], b[1]), js::max(a[0], b[0]), js::max(a[1], b[1])};
}

Rect Frame::rect_from_world(const Rect& r) const {
  if (turned) {
    // (conservative: the canonical bounds of its corners' cells)
    double x0 = js::kInf, y0 = js::kInf, x1 = -js::kInf, y1 = -js::kInf;
    const double pts[4][2] = {{r.x0, r.y0}, {r.x1, r.y0}, {r.x0, r.y1}, {r.x1, r.y1}};
    for (const auto& p : pts) {
      const XY uv = from_world(p[0], p[1]);
      x0 = js::min(x0, uv[0]);
      y0 = js::min(y0, uv[1]);
      x1 = js::max(x1, uv[0]);
      y1 = js::max(y1, uv[1]);
    }
    return {x0, y0, x1, y1};
  }
  const XY a = from_world(r.x0, r.y0);
  const XY b = from_world(r.x1, r.y1);
  return {js::min(a[0], b[0]), js::min(a[1], b[1]), js::max(a[0], b[0]), js::max(a[1], b[1])};
}

char Frame::world_side(char cs) const {
  const char* row = side_row(front);
  if (!row) return 0;
  const int k = cs == 'F' ? 0 : cs == 'B' ? 1 : cs == 'L' ? 2 : cs == 'R' ? 3 : -1;
  return k < 0 ? 0 : row[k];
}

char Frame::canon_side(char ws) const {
  const char* row = side_row(front);
  if (!row) return 0;
  for (int k = 0; k < 4; ++k)
    if (row[k] == ws) return "FBLR"[k];
  return 0;
}

Frame Frame::shifted(double du, double dv, double U_, double V_) const {
  if (!turned) SVX_FAIL("frame: shifted() of a frame that is not turned");
  return TurnedFrame(placement, ou + du, ov + dv, U_, V_, front);
}

std::optional<Turn> Frame::turn() const {
  if (!turned) return std::nullopt;
  Turn t;
  t.yaw = placement.yaw;
  t.yaw2 = placement.yaw2;
  t.origin = {placement.origin.x, placement.origin.y};
  t.ou = ou;
  t.ov = ov;
  return t;
}

XY Frame::point_to_world(double u, double v) const {
  if (!turned) SVX_FAIL("frame: point_to_world() of a frame that is not turned");
  const Placement& p = placement;
  const double lu = u + ou;
  const double lv = v + ov;
  return {p.origin.x + (p.m[0] * lu + p.m[1] * lv) / p.d, p.origin.y + (p.m[3] * lu + p.m[4] * lv) / p.d};
}

XY Frame::point_from_world(double x, double y) const {
  if (!turned) SVX_FAIL("frame: point_from_world() of a frame that is not turned");
  const XY uv = world_point_to_local(placement, x, y);
  return {uv[0] - ou, uv[1] - ov};
}

double Frame::distance(const Rect& r, double x, double y) const {
  const XY uv = point_from_world(x + 0.5, y + 0.5);
  const double du = js::max(r.x0 + 0.5 - uv[0], 0.0, uv[0] - (r.x1 + 0.5));
  const double dv = js::max(r.y0 + 0.5 - uv[1], 0.0, uv[1] - (r.y1 + 0.5));
  return std::sqrt(du * du + dv * dv);
}

char nominal_front(int yaw, int yaw2) {
  if (yaw2) {
    // (the nearest axis to the product's direction: never a tie, no yaw is 45 degrees)
    const Yaw y = yaw_vector(yaw, yaw2);
    return std::fabs(y.c) > std::fabs(y.s) ? (y.c > 0 ? 'N' : 'S') : y.s > 0 ? 'E' : 'W';
  }
  const double q = std::fmod(js::round(yaw / (static_cast<double>(yaws().size()) / 4)), 4);
  // (["N", "E", "S", "W"][q]: undefined off the table)
  return (q >= 0 && q < 4) ? "NESW"[static_cast<int>(q)] : 0;
}

Frame turned_frame(const Turn& turn, double U, double V, char front) {
  PlacementOpts o;
  o.origin = {turn.origin.x, turn.origin.y, 0};
  o.yaw = turn.yaw;
  o.yaw2 = turn.yaw2;
  return TurnedFrame(Placement(o), turn.ou, turn.ov, U, V, front);
}

Frame turned_frame(const Turn& turn, double U, double V) { return turned_frame(turn, U, V, nominal_front(turn.yaw, turn.yaw2)); }

Frame lot_frame_of(const std::optional<Turn>& turn, const Rect& rect, char front) {
  return turn ? turned_frame(*turn, turn->U, turn->V, front) : Frame(rect, front);
}

Frame frame_of(const std::optional<Turn>& turn, double U, double V, char front, const Rect& R) {
  return turn ? turned_frame(*turn, U, V, front) : Frame(R, front);
}

}  // namespace svx::city
