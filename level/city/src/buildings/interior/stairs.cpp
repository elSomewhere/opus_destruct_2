// svx_city — voxel_city buildings/interior/stairs.js.
#include "buildings/interior/stairs.hpp"

#include "svx/base/types.hpp"

namespace svx::city {

StairDims stair_dims(double H, double lane, double landing) {
  const double n1 = std::ceil(H / 2);
  const double W = 2 * lane + 1;
  const double L = landing + 2 * (n1 - 1) + js::max(landing, lane + 1);
  return {W, L, lane, landing};
}

Stair make_stair(const MakeStairOpts& o) {
  const Rect& rect = o.rect;
  const double along = o.axis == 'v' ? rect.y1 - rect.y0 + 1 : rect.x1 - rect.x0 + 1;
  const double across = o.axis == 'v' ? rect.x1 - rect.x0 + 1 : rect.y1 - rect.y0 + 1;
  Stair st;
  st.rect = rect;
  st.axis = o.axis;
  st.dir = o.dir;
  st.lane_low = o.lane_low;
  st.lane = o.lane;
  st.landing = o.landing;
  st.f0 = o.f0;
  st.f1 = o.f1;
  st.L = along;
  st.W = across;
  st.open = o.open;
  return st;
}

std::optional<StairST> stair_local(const Stair& st, double u, double v) {
  const Rect& r = st.rect;
  if (u < r.x0 || u > r.x1 || v < r.y0 || v > r.y1) return std::nullopt;
  double s, t;
  if (st.axis == 'v') {
    s = st.dir > 0 ? v - r.y0 : r.y1 - v;
    t = st.lane_low ? u - r.x0 : r.x1 - u;
  } else {
    s = st.dir > 0 ? u - r.x0 : r.x1 - u;
    t = st.lane_low ? v - r.y0 : r.y1 - v;
  }
  return StairST{s, t};
}

namespace {

// Local (s0..s1, t0..t1) -> canonical rect.
Rect local_rect(const Stair& st, double s0, double s1, double t0, double t1) {
  const Rect& r = st.rect;
  double u0, u1, v0, v1;
  if (st.axis == 'v') {
    if (st.dir > 0) {
      v0 = r.y0 + s0;
      v1 = r.y0 + s1;
    } else {
      v0 = r.y1 - s1;
      v1 = r.y1 - s0;
    }
    if (st.lane_low) {
      u0 = r.x0 + t0;
      u1 = r.x0 + t1;
    } else {
      u0 = r.x1 - t1;
      u1 = r.x1 - t0;
    }
  } else {
    if (st.dir > 0) {
      u0 = r.x0 + s0;
      u1 = r.x0 + s1;
    } else {
      u0 = r.x1 - s1;
      u1 = r.x1 - s0;
    }
    if (st.lane_low) {
      v0 = r.y0 + t0;
      v1 = r.y0 + t1;
    } else {
      v0 = r.y1 - t1;
      v1 = r.y1 - t0;
    }
  }
  return {u0, v0, u1, v1};
}

}  // namespace

Rect near_landing(const Stair& st) { return local_rect(st, 0, st.landing - 1, 0, st.W - 1); }

bool stair_slab_open(const Stair& st, double f, double u, double v) {
  if (f <= st.f0 || f > st.f1) return false;
  const auto p = stair_local(st, u, v);
  return p && p->s >= st.landing;
}

std::vector<CanonBox> stair_boxes(const Stair& st, const StairMats& mats) {
  if (!st.flights) SVX_FAIL("stairs: a stair without flights (PlanBuilder.addStair makes them)");
  std::vector<CanonBox> out;
  const double lane = st.lane;
  const double W = st.W;
  auto add = [&](double s0, double s1, double t0, double t1, double z0, double z1, uint16_t m) {
    if (s1 < s0 || t1 < t0 || z1 < z0) return;
    const Rect r = local_rect(st, s0, s1, t0, t1);
    out.push_back({r.x0, r.y0, z0, r.x1, r.y1, z1, m});
  };
  const double tA0 = 0;
  const double tA1 = lane - 1;
  const double tB0 = lane + 1;
  const double tB1 = W - 1;
  for (const StairFlight& fl : *st.flights) {
    const double z0 = fl.z0;
    const double H = fl.H;
    const double base = z0 + 1;  // level-0 walking surface voxel
    const double n1 = std::ceil(H / 2);
    const double n2 = H - n1;
    const bool lowest = fl.f == st.f0;
    // flight 1, lane A
    for (double k = 1; k < n1; k += 1) {
      const double s = st.landing + 2 * (k - 1);
      const double top = base + k;
      add(s, s + 1, tA0, tA1, lowest ? z0 : js::max(z0, top - 3), top, mats.tread);
    }
    // mid landing
    const double sA = st.landing + 2 * (n1 - 1);
    const double sB = st.landing + 2 * (n2 - 1);
    const double lTop = base + n1;
    add(sA, st.L - 1, tA0, tA1, lTop - 2, lTop, mats.landing);
    add(js::min(sA, sB), st.L - 1, lane, lane, lTop - 2, lTop, mats.landing);
    add(sB, st.L - 1, tB0, tB1, lTop - 2, lTop, mats.landing);
    // flight 2, lane B (descending towards the near end)
    for (double j = 1; j < n2; j += 1) {
      const double s = st.landing + 2 * (n2 - 1 - j);
      const double top = base + n1 + j;
      add(s, s + 1, tB0, tB1, js::max(z0, top - 3), top, mats.tread);
    }
    // divider between the flights
    const double dEnd = js::min(sA, sB) - 1;
    // (a full-height divider keeps both flights safe to walk; open stairs use a lighter material)
    if (dEnd >= st.landing) add(st.landing, dEnd, lane, lane, z0, z0 + H - 1, st.open ? mats.rail : mats.divider);
  }
  return out;
}

}  // namespace svx::city
