// svx_city — voxel_city buildings/interior/garage.js.
#include "buildings/interior/garage.hpp"

#include <vector>

#include "buildings/interior/common.hpp"
#include "buildings/interior/stairs.hpp"
#include "core/placement.hpp"
#include "svx/base/types.hpp"

namespace svx::city {

namespace {

constexpr double EXT_T = FloorGrid::EXT_T;
constexpr double RW = 30;       // ramp width (3.75 m)
constexpr double STALL_W = 20;  // 2.5 m
constexpr double STALL_D = 40;  // 5 m
constexpr double AISLE = 48;    // 6 m
constexpr double CROSS = 56;    // cross aisles at the front and back (7 m)
// RAMP_PITCHES: table pitches a pitched ramp may take, gentlest first: 12.5%, 14.4%, 16.8% (1:8 to 1:6)
constexpr int kRampPitches[] = {3, 4, 5};

struct StallLayout {
  std::vector<GarageStall> stalls;
  std::vector<Rect> pillars;
  Rect ramp_rect;
};

// Stall rows as bands across u, from the wall opposite the ramp: pairs of back-to-back rows share
// an aisle (row | aisle | row), whatever is left next to the ramp becomes the main aisle. Rows run
// along v between the cross aisles; pillars stand on the line between back-to-back rows.
StallLayout stall_layout(const Rect& inner, const Rect& ramp, const Rect& sr, bool ramp_left, Rng& rng) {
  StallLayout out;
  std::vector<double> pillars;
  const double v0 = inner.y0 + CROSS;
  const double v1 = inner.y1 - CROSS;
  // bands in "distance from the far wall" coordinates
  const double avail = inner.x1 - inner.x0 + 1 - RW - AISLE;
  struct Band {
    double d0, d1, nose;
  };
  std::vector<Band> bands;
  double d = 0;
  double facing = 1;  // +1: nose towards increasing d
  while (d + STALL_D <= avail) {
    bands.push_back({d, d + STALL_D - 1, facing});
    d += STALL_D;
    if (facing == 1) {
      if (d + AISLE + STALL_D > avail) break;
      d += AISLE;
      facing = -1;
    } else {
      pillars.push_back(d);
      facing = 1;
    }
  }
  auto to_u = [&](double dd) { return ramp_left ? inner.x1 - dd : inner.x0 + dd; };
  const Rect stair_box{sr.x0 - 2, sr.y0 - 14, sr.x1 + 2, sr.y1};
  for (const Band& b : bands) {
    const double ua = to_u(b.d0);
    const double ub = to_u(b.d1);
    const double u0 = js::min(ua, ub);
    const double u1 = js::max(ua, ub);
    // nose direction in canonical u
    const double nose_u = ramp_left ? -b.nose : b.nose;
    for (double v = v0; v + STALL_W - 1 <= v1; v += STALL_W) {
      GarageStall s;
      s.x0 = u0;
      s.x1 = u1;
      s.y0 = v;
      s.y1 = v + STALL_W - 1;
      s.nose_u = nose_u;
      s.car = rng.chance(0.65);
      if (s.x1 >= stair_box.x0 && s.x0 <= stair_box.x1 && s.y1 >= stair_box.y0 && s.y0 <= stair_box.y1) continue;
      out.stalls.push_back(s);
    }
  }
  for (double pd : pillars) {
    const double u = to_u(pd);
    for (double v = v0 + STALL_W * 2; v < v1 - 8; v += STALL_W * 3) out.pillars.push_back({u - 2, v - 2, u + 1, v + 1});
  }
  out.ramp_rect = ramp;
  return out;
}

}  // namespace

void plan_garage(const Envelope& env, Rng& rng, PlanBuilder& pb) {
  const double nF = env.floors;
  if (env.story_h.empty()) SVX_FAIL("garage: an envelope without stories");
  const double H = env.story_h[0];
  const std::vector<Rect>& fps = tier_rects(env, 0);
  if (fps.empty()) SVX_FAIL("garage: an envelope without its ground floor");
  const Rect fp = fps[0];
  const Rect inner{fp.x0 + EXT_T, fp.y0 + EXT_T, fp.x1 - EXT_T, fp.y1 - EXT_T};
  const double Vi = inner.y1 - inner.y0 + 1;
  const bool ramp_left = env.mirror;
  // ramp: slope 1:8 when there is room, never steeper than 1:6; in the angled world
  // (env.pitched_ramps) the gentlest table grade that fits, its run H c / s, so a pitched slab
  // part rises exactly a storey (cellPlan)
  double L = js::max(6 * H, js::min(8 * H, Vi - 2 * CROSS));
  double pitch = 0;
  if (env.pitched_ramps) {
    for (int p : kRampPitches) {
      const Yaw& P = pitches()[static_cast<size_t>(p)];
      const double run = js::round((H * P.c) / P.s);
      if (run <= Vi - 2 * CROSS) {
        L = run;
        pitch = p;
        break;
      }
    }
  }
  const double ra = inner.y0 + std::floor((Vi - L) / 2);
  const Rect ramp = ramp_left ? Rect{inner.x0, ra, inner.x0 + RW - 1, ra + L - 1} : Rect{inner.x1 - RW + 1, ra, inner.x1, ra + L - 1};
  // stair core in the back corner on the other side, landing towards the deck
  const StairDims sd = stair_dims(H);
  const Rect sr = ramp_left ? Rect{inner.x1 - sd.W + 1, inner.y1 - sd.L + 1, inner.x1, inner.y1} : Rect{inner.x0, inner.y1 - sd.L + 1, inner.x0 + sd.W - 1, inner.y1};
  MakeStairOpts so;
  so.rect = sr;
  so.axis = 'v';
  so.dir = 1;
  so.lane_low = rng.chance(0.5);
  so.f0 = 0;
  so.f1 = nF;
  const std::shared_ptr<Stair> stair = pb.add_stair(make_stair(so));
  std::vector<Rect> deck_rects;
  for (const Rect& r : r_subtract_all({inner}, {{sr.x0 - 1, sr.y0 - 1, sr.x1 + 1, sr.y1 + 1}}))
    if (r.x1 >= r.x0 && r.y1 >= r.y0) deck_rects.push_back(r);
  const StallLayout layout = stall_layout(inner, ramp, sr, ramp_left, rng);

  std::shared_ptr<FloorGrid> typical;
  std::vector<std::shared_ptr<FloorGrid>> decks;  // (the decks' grids, by floor)
  for (double f = 0; f < nF; f += 1) {
    std::shared_ptr<FloorGrid> grid = f > 0 ? typical : nullptr;
    if (!grid) {
      grid = pb.new_grid(f);
      const std::shared_ptr<Room> stair_room = add_stair_room(*grid, *stair);
      RoomProps p;
      p.paint = "CONCRETE";
      p.floor_mat = "FLOOR_CONCRETE";
      p.stalls = layout.stalls;
      p.pillars = layout.pillars;
      p.ramp_rect = layout.ramp_rect;
      p.deck_h = H;
      const std::shared_ptr<Room> deck = grid->add_room("deck", deck_rects, p);
      DoorOpts sdo;
      sdo.width = 8;
      stair_door(*grid, *stair_room, *stair, deck.get(), sdo);
      if (f == 0) {
        // vehicle entrance in line with the ramp aisle, pedestrian door beside it
        const double cu = ramp_left ? ramp.x1 + 1 + AISLE / 2 : ramp.x0 - 1 - AISLE / 2;
        DoorOpts car;
        car.kind = "entrance";
        car.width = 40;
        car.place = "near";
        car.near = DoorNear{cu, 0};
        car.leaf = "none";
        car.height = 20;
        grid->add_door(*deck, nullptr, car);
        DoorOpts walk;
        walk.kind = "entrance";
        walk.width = 8;
        walk.place = "near";
        walk.near = DoorNear{ramp_left ? inner.x1 - 24 : inner.x0 + 24, 0};
        walk.leaf = "metal";
        grid->add_door(*deck, nullptr, walk);
      } else {
        typical = grid;
      }
    }
    pb.add_floor(f, grid, "garage");
    decks.push_back(grid);
  }
  for (double f = 0; f + 1 < nF; f += 1) {
    Ramp r;
    r.rect = ramp;
    r.f = f;
    r.H = pb.h(f);
    r.pitch = pitch;
    r = pb.add_ramp(r);
    const Room* a = decks[static_cast<size_t>(f)]->room_at(ramp.x0, ramp.y0);
    const Room* b = decks[static_cast<size_t>(f + 1)]->room_at(ramp.x0, ramp.y1);
    if (!a || !b) SVX_FAIL("garage: a ramp's end off its deck");
    pb.links.push_back({f, a->id, f + 1, b->id, r.id});
  }
}

}  // namespace svx::city
