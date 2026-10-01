// svx_city — voxel_city buildings/archetypes.js.
#include "buildings/archetypes.hpp"

#include <utility>

#include "buildings/interior/cabins.hpp"
#include "buildings/interior/houses.hpp"
#include "core/math.hpp"
#include "svx/base/types.hpp"
#include "voxel/materials.hpp"
#include "world/wrap.hpp"

namespace svx::city {

namespace {

// S: story heights (voxels)
struct StoryHeights {
  double house, res, office, retail, industrial;
};
const StoryHeights& S() {
  static const StoryHeights s{vx(2.75), vx(3.0), vx(3.75), vx(4.5), vx(8.0)};
  return s;
}

double floors_for(const ArchetypeCtx& ctx, double extra_scale = 1) {
  const double f_min = ctx.district->floors[0];
  const double f_max = ctx.district->floors[1];
  const double t = clamp(ctx.core * 1.1 + ctx.rng->float_(-0.2, 0.25), 0, 1);
  const double f = js::round(lerp(f_min, f_max, t * t) * extra_scale);
  return clamp(f, f_min, f_max);
}

// new Array(n).fill(h)
std::vector<double> fill(double n, double h) { return std::vector<double>(static_cast<size_t>(n), h); }

EnvTier tier(double f0, double f1, std::vector<Rect> rects) { return EnvTier{f0, f1, std::move(rects)}; }

// A roof's overhang per side {F, B, L, R} (NaN: a side left out).
void overhang_sides(EnvRoof& roof, double F, double B, double L, double R) {
  roof.overhang_kind = EnvRoof::Overhang::Sides;
  roof.overhang_f = F;
  roof.overhang_b = B;
  roof.overhang_l = L;
  roof.overhang_r = R;
}
void overhang_number(EnvRoof& roof, double o) {
  roof.overhang_kind = EnvRoof::Overhang::Number;
  roof.overhang = o;
}

EnvProgram program(const char* ground, const char* upper, const char* podium = "") {
  EnvProgram p;
  p.ground = ground;
  p.upper = upper;
  p.podium = podium;
  return p;
}

// What a town's warehouse holds: pallet racks, or a distribution centre (conveyors, parcel cages),
// self-storage units, a cold store, a timber merchant's hall (drawn from its own stream: the
// envelope stays the same). Warehouses on military and research sites stay plain.
std::string warehouse_kind(const ArchetypeCtx& ctx) {
  static const char* const kTownDistricts[] = {"industrial", "heavyIndustry", "harbour", "port", "mixed", "suburban", "residential", "projects"};
  bool town = false;
  if (ctx.district)
    for (const char* d : kTownDistricts)
      if (ctx.district->id == d) town = true;
  if (!town) return "warehouse";
  static const std::vector<std::pair<std::string, double>> kKinds = {{"warehouse", 5}, {"distribution", 2}, {"selfStorage", 1.5}, {"coldStore", 1}, {"timberYard", 1}};
  Rng kind = ctx.rng->fork("kind");
  return kind.weighted(kKinds);
}

// ---- the archetypes' envelopes

std::optional<EnvSpec> house_envelope(const ArchetypeCtx& ctx) {
  Rng& rng = *ctx.rng;
  const double U = ctx.frame->U;
  const double V = ctx.frame->V;
  const double set_f = vx(rng.float_(4.5, 7));
  const double set_s = vx(rng.float_(1.5, 2.5));
  const double width = js::min(U - 2 * set_s, vx(rng.float_(8.5, 12)));
  const double depth = js::min(V - set_f - vx(6), vx(rng.float_(10.75, 12.5)));
  if (width < vx(8.25) || depth < vx(10.5)) return std::nullopt;
  const bool garage = U - width - 2 * set_s >= vx(4) && rng.chance(0.55);
  const bool left_side = rng.chance(0.5);
  const double u0 = garage ? (left_side ? U - set_s - width : set_s) : js::round((U - width) / 2);
  const Rect main{u0, set_f, u0 + width - 1, set_f + depth - 1};
  const double floors = rng.chance(0.25) ? 1 : 2;
  EnvSpec env;
  env.tiers.push_back(tier(0, floors - 1, {main}));
  if (garage) {
    const double gw = vx(3.75);
    const double gu0 = left_side ? main.x0 - gw : main.x1 + 1;
    env.annexes.push_back({"garage", Rect{gu0, set_f + vx(0.5), gu0 + gw - 1, set_f + vx(0.5) + vx(6.5) - 1}, vx(2.75)});
  }
  env.floors = floors;
  env.story_h = fill(floors, S().house);
  env.basements = 0;
  env.roof.type = rng.chance(0.65) ? "gable" : "hip";
  env.roof.pitch = 1;
  env.program = program("house", "house");
  env.entrance_side = "F";
  return env;
}

std::optional<EnvSpec> rowhouse_envelope(const ArchetypeCtx& ctx) {
  Rng& rng = *ctx.rng;
  const double U = ctx.frame->U;
  const double V = ctx.frame->V;
  static const double kSetbacks[] = {0, 1.5, 2.5};
  const double set_f = ctx.lot->corner ? 0 : vx(rng.pick(kSetbacks));
  const double depth = js::min(V - set_f - vx(4), vx(rng.float_(10.5, 14)));
  if (depth < vx(10)) return std::nullopt;
  const double floors = clamp(floors_for(ctx), 2, 4);
  const Rect main{0, set_f, U - 1, set_f + depth - 1};
  EnvSpec env;
  env.tiers.push_back(tier(0, floors - 1, {main}));
  env.floors = floors;
  env.story_h = fill(floors, S().res);
  env.basements = rng.chance(0.4) ? 1 : 0;
  env.roof.type = rng.chance(0.25) ? "gable" : "flat";
  env.program = program("house", "house");
  env.entrance_side = "F";
  env.stoop = set_f > 0;
  return env;
}

std::optional<EnvSpec> walkup_envelope(const ArchetypeCtx& ctx) {
  Rng& rng = *ctx.rng;
  const District& district = *ctx.district;
  const double U = ctx.frame->U;
  const double V = ctx.frame->V;
  const double depth = js::min(V - vx(3), vx(rng.float_(11, 16)));
  if (depth < vx(9.5)) return std::nullopt;
  const double floors = clamp(floors_for(ctx), 3, 6);
  const bool retail = (district.id == "mixed" || district.id == "midtown") && rng.chance(0.7);
  const Rect main{0, 0, U - 1, depth - 1};
  std::vector<double> story_h = fill(floors, S().res);
  if (retail) story_h[0] = vx(4.0);
  EnvSpec env;
  env.tiers.push_back(tier(0, floors - 1, {main}));
  env.floors = floors;
  env.story_h = std::move(story_h);
  env.basements = rng.chance(0.5) ? 1 : 0;
  env.roof.type = "flat";
  env.program = program(retail ? "retail" : "apartments", "apartments");
  env.entrance_side = "F";
  return env;
}

std::optional<EnvSpec> midrise_envelope(const ArchetypeCtx& ctx) {
  Rng& rng = *ctx.rng;
  const District& district = *ctx.district;
  const double U = ctx.frame->U;
  const double V = ctx.frame->V;
  const double depth = js::min(V - vx(2), vx(rng.float_(15.5, 20)));
  if (depth < vx(14.5)) return std::nullopt;
  const double floors = clamp(floors_for(ctx, 1.1), 5, 16);
  const bool retail = district.id != "residential" && rng.chance(0.75);
  const Rect main{0, 0, U - 1, depth - 1};
  std::vector<double> story_h = fill(floors, S().res);
  story_h[0] = retail ? S().retail : vx(3.5);
  EnvSpec env;
  env.tiers.push_back(tier(0, floors - 1, {main}));
  env.floors = floors;
  env.story_h = std::move(story_h);
  env.basements = 1;
  env.roof.type = "flat";
  env.program = program(retail ? "retail" : "lobbyApartments", "apartments");
  env.entrance_side = "F";
  return env;
}

// Panel slab (Soviet khrushchyovka / brezhnevka, social housing slab): a long, shallow
// freestanding block with one stair section per entrance, 5, 9 or 12-16 floors of flats with low
// ceilings. The lot is the footprint.
std::optional<EnvSpec> panel_slab_envelope(const ArchetypeCtx& ctx) {
  Rng& rng = *ctx.rng;
  const Frame& frame = *ctx.frame;
  const District& district = *ctx.district;
  const double depth = js::min(frame.V - vx(1), vx(rng.float_(12, 13.5)));
  const double f_min = district.floors[0];
  const double f_max = district.floors[1];
  std::vector<double> options;
  for (double f : {5.0, 9.0, 9.0, 12.0, 14.0, 16.0})
    if (f >= f_min && f <= f_max) options.push_back(f);
  const double floors = !options.empty() ? rng.pick(options) : clamp(f_min, 2, 16);
  const double y0 = js::round((frame.V - depth) / 2);
  EnvSpec env;
  env.tiers.push_back(tier(0, floors - 1, {Rect{0, y0, frame.U - 1, y0 + depth - 1}}));
  env.floors = floors;
  env.story_h = fill(floors, vx(2.875));
  env.basements = 1;
  env.roof.type = "flat";
  env.program = program("lobbyApartments", "apartments");
  env.entrance_side = "F";
  return env;
}

// Point tower of the same system: a square block of flats round one core.
std::optional<EnvSpec> panel_tower_envelope(const ArchetypeCtx& ctx) {
  Rng& rng = *ctx.rng;
  const Frame& frame = *ctx.frame;
  const District& district = *ctx.district;
  const double side = js::min(frame.U, frame.V, vx(rng.float_(21, 25)));
  const double f_min = district.floors[0];
  const double f_max = district.floors[1];
  const double floors = clamp(rng.int_(js::max(f_min, 12.0), js::max(f_min, f_max + 6)), 9, 24);
  const double x0 = js::round((frame.U - side) / 2);
  const double y0 = js::round((frame.V - side) / 2);
  EnvSpec env;
  env.tiers.push_back(tier(0, floors - 1, {Rect{x0, y0, x0 + side - 1, y0 + side - 1}}));
  env.floors = floors;
  env.story_h = fill(floors, vx(2.875));
  env.basements = 1;
  env.roof.type = "flat";
  env.program = program("lobbyApartments", "apartments");
  env.entrance_side = "F";
  return env;
}

std::optional<EnvSpec> office_envelope(const ArchetypeCtx& ctx) {
  Rng& rng = *ctx.rng;
  const double U = ctx.frame->U;
  const double V = ctx.frame->V;
  const double depth = js::min(V - vx(1), vx(rng.float_(20, 30)));
  const double floors = clamp(floors_for(ctx, 0.8), 4, 18);
  const Rect main{0, 0, U - 1, depth - 1};
  std::vector<double> story_h = fill(floors, S().office);
  story_h[0] = S().retail;
  EnvSpec env;
  env.tiers.push_back(tier(0, floors - 1, {main}));
  env.floors = floors;
  env.story_h = std::move(story_h);
  env.basements = rng.chance(0.6) ? 2 : 1;
  env.roof.type = "flat";
  env.program = program("officeLobby", "office");
  env.entrance_side = "F";
  return env;
}

std::optional<EnvSpec> tower_envelope(const ArchetypeCtx& ctx) {
  Rng& rng = *ctx.rng;
  const double U = ctx.frame->U;
  const double V = ctx.frame->V;
  const double podium_floors = rng.int_(2, 4);
  const double floors = clamp(floors_for(ctx, 1), 12, 46);
  const bool residential = rng.chance(0.3);
  const double inset = vx(rng.float_(3, 6));
  const double tw = js::min(U - 2 * inset, vx(rng.float_(24, 38)));
  const double td = js::min(V - 2 * inset, vx(rng.float_(24, 34)));
  if (tw < vx(20) || td < vx(20)) return std::nullopt;
  const double tu0 = js::round((U - tw) / 2);
  const double half = (V - td) / 2;
  const double tv0 = js::round(half + rng.float_(-0.15, 0.1) * (V - td));
  const Rect tower{tu0, tv0, tu0 + tw - 1, tv0 + td - 1};
  const Rect podium{0, 0, U - 1, js::min(V - 1, tower.y1 + vx(rng.float_(2, 8)))};
  EnvSpec env;
  env.tiers.push_back(tier(0, podium_floors - 1, {podium}));
  env.tiers.push_back(tier(podium_floors, floors - 1, {tower}));
  // upper setback for tall towers
  if (floors > 28 && rng.chance(0.6)) {
    const double cut = js::round(floors * rng.float_(0.65, 0.8));
    const double s = vx(rng.float_(2.5, 4.5));
    const Rect upper{tower.x0 + s, tower.y0 + s, tower.x1 - s, tower.y1 - s};
    env.tiers[1].f1 = cut - 1;
    env.tiers.push_back(tier(cut, floors - 1, {upper}));
  }
  std::vector<double> story_h = fill(floors, residential ? S().res : S().office);
  for (double f = 0; f < podium_floors; f += 1) story_h[static_cast<size_t>(f)] = f == 0 ? S().retail : S().office;
  env.floors = floors;
  env.story_h = std::move(story_h);
  env.basements = 2;
  env.roof.type = "flat";
  env.roof.crown = rng.chance(0.5);
  env.program = program("officeLobby", residential ? "apartments" : "office", "office");
  env.podium_floors = podium_floors;
  env.entrance_side = "F";
  return env;
}

std::optional<EnvSpec> warehouse_envelope(const ArchetypeCtx& ctx) {
  Rng& rng = *ctx.rng;
  const double U = ctx.frame->U;
  const double V = ctx.frame->V;
  const double set_f = vx(rng.float_(10, 18));
  const double set_s = vx(rng.float_(3, 6));
  const double w = U - 2 * set_s;
  const double d = js::min(V - set_f - vx(5), vx(rng.float_(24, 70)));
  if (w < vx(18) || d < vx(16)) return std::nullopt;
  const Rect hall{set_s, set_f, set_s + w - 1, set_f + d - 1};
  EnvSpec env;
  env.tiers.push_back(tier(0, 0, {hall}));
  env.floors = 1;
  env.story_h = {vx(rng.float_(7.5, 10))};
  env.basements = 0;
  env.roof.type = rng.chance(0.3) ? "sawtooth" : "flat";
  env.program.ground = warehouse_kind(ctx);  // (upper: null)
  env.entrance_side = "F";
  env.yard = true;
  return env;
}

// Farm barn: one tall hall under a gable roof (hay, stalls), a small office and loft stair in a
// corner.
std::optional<EnvSpec> barn_envelope(const ArchetypeCtx& ctx) {
  Rng& rng = *ctx.rng;
  const Frame& frame = *ctx.frame;
  const double set_f = vx(rng.float_(3, 6));
  const double set_s = vx(rng.float_(1.5, 3));
  const double w = js::min(frame.U - 2 * set_s, vx(rng.float_(16, 24)));
  const double d = js::min(frame.V - set_f - vx(3), vx(rng.float_(20, 32)));
  if (w < vx(14) || d < vx(16)) return std::nullopt;
  const double u0 = js::round((frame.U - w) / 2);
  EnvSpec env;
  env.tiers.push_back(tier(0, 0, {Rect{u0, set_f, u0 + w - 1, set_f + d - 1}}));
  env.floors = 1;
  env.story_h = {vx(rng.float_(6.5, 8))};
  env.basements = 0;
  env.roof.type = "gable";
  env.roof.pitch = 1;
  env.program.ground = "barn";
  env.entrance_side = "F";
  env.yard = true;
  return env;
}

std::optional<EnvSpec> factory_envelope(const ArchetypeCtx& ctx) {
  std::optional<EnvSpec> env = archetype_registry().get("warehouse").envelope(ctx);
  if (!env) return std::nullopt;
  EnvRoof roof;
  roof.type = "sawtooth";
  roof.chimney = ctx.rng->chance(0.7);
  env->roof = roof;
  env->program = program("factory", "");
  return env;
}

std::optional<EnvSpec> garage_envelope(const ArchetypeCtx& ctx) {
  Rng& rng = *ctx.rng;
  const double U = ctx.frame->U;
  const double V = ctx.frame->V;
  const double depth = js::min(V - vx(1), vx(rng.float_(40, 52)));
  const double floors = rng.int_(3, 6);
  const Rect main{0, 0, U - 1, depth - 1};
  EnvSpec env;
  env.tiers.push_back(tier(0, floors - 1, {main}));
  env.floors = floors;
  env.story_h = fill(floors, vx(3.0));
  env.basements = 0;
  env.roof.type = "flat";
  env.program = program("garage", "garage");
  env.entrance_side = "F";
  env.force_style = "parkingDeck";
  return env;
}

std::optional<EnvSpec> school_envelope(const ArchetypeCtx& ctx) {
  Rng& rng = *ctx.rng;
  const double U = ctx.frame->U;
  const double V = ctx.frame->V;
  const double set_f = vx(rng.float_(5, 8));
  const double width = js::min(U - vx(10), vx(rng.float_(46, 72)));
  const double depth = js::min(V - set_f - vx(14), vx(rng.float_(19, 22)));
  if (depth < vx(18)) return std::nullopt;
  const double u0 = js::round((U - width) / 2);
  const double floors = rng.int_(2, 3);
  EnvSpec env;
  env.tiers.push_back(tier(0, floors - 1, {Rect{u0, set_f, u0 + width - 1, set_f + depth - 1}}));
  env.floors = floors;
  env.story_h = fill(floors, vx(3.75));
  env.basements = 0;
  env.roof.type = "flat";
  env.program = program("school", "school");
  env.entrance_side = "F";
  return env;
}

// --- nordic
//
// Optional envelope keys used here (absent on every other archetype):
//   roof.slope     rise per voxel of a pitched roof (default 0.7, ~35 degrees)
//   roof.ridge     "u" (default: ridge along the street) | "v" (gable to the street)
//   roof.overhang  eaves overhang in voxels, a number or {F, B, L, R} (default 3)
//   plinth         stone plinth from the ground up to a raised ground floor
//   porch          wooden porch + steps at the front door instead of a canopy
//   steeple        {rect, shaft, spire}: ground-tier rect `rect` rises as a church tower `shaft`
//                  voxels above the eaves, then a spire
// and an archetype may define entrance_u(env, U, V, mirror) so the lot dressing knows where its
// front door is before the interior is planned.

// Nordic wooden town house (old town centres of Norwegian, Swedish and Karelian towns) on a
// perimeter-block lot: 2-3 low floors built to the street line under a steep gable roof, the ridge
// along the street or, on narrow lots, the gable to the street; gable ends and eaves stay flush
// with the party walls. In an "oldtown" district the ground floor is often a shop with flats
// above; otherwise one family house (house planner) or small flats (apartment planner).
std::optional<EnvSpec> townhouse_envelope(const ArchetypeCtx& ctx) {
  Rng& rng = *ctx.rng;
  const District& district = *ctx.district;
  const double U = ctx.frame->U;
  const double V = ctx.frame->V;
  const double depth = js::min(V - vx(2), vx(rng.float_(9, 12.5)));
  if (depth < vx(8.5)) return std::nullopt;
  const double floors = clamp(floors_for(ctx), 2, 3);
  const bool shop = district.id == "oldtown" && rng.chance(0.55);
  // an old street line is never quite straight: some houses stand a little back, and a passage
  // (a fire lane, a gate to the back yard) opens beside some of them
  const bool old = district.id == "oldtown" || district.id == "mixed";
  static const double kSetbacks[] = {0, 0, 0, 0, 1, 2, 3, 4};
  const double set_f = old && !ctx.lot->corner ? rng.pick(kSetbacks) : 0;
  const double gap = old && U >= vx(10) && rng.chance(0.3) ? vx(rng.float_(1.5, 2.5)) : 0;
  const bool gap_left = rng.chance(0.5);
  const double W = U - gap;
  // (a broad plot holds flats: the house planner is for narrow houses)
  const bool house = !shop && W <= vx(17) && (W < vx(10) || rng.chance(0.45));
  std::vector<double> story_h = fill(floors, S().res);
  if (shop) story_h[0] = vx(3.5);
  const bool gable_front = W <= vx(12) && rng.chance(0.5);
  EnvRoof roof;
  roof.type = "gable";
  if (gable_front) {
    roof.ridge = "v";
    roof.slope = rng.float_(1.0, 1.2);
    overhang_sides(roof, 2, 2, js::truthy(gap) ? 2 : 0, js::truthy(gap) ? 2 : 0);
  } else {
    roof.ridge = "u";
    roof.slope = rng.float_(0.9, 1.1);
    overhang_sides(roof, 3, 3, 0, 0);
  }
  const double x0 = js::truthy(gap) && gap_left ? gap : 0;
  EnvSpec env;
  env.tiers.push_back(tier(0, floors - 1, {Rect{x0, set_f, x0 + W - 1, set_f + depth - 1}}));
  env.floors = floors;
  env.story_h = std::move(story_h);
  env.basements = rng.chance(0.3) ? 1 : 0;
  env.roof = roof;
  env.program = shop ? program("retail", "apartments") : house ? program("house", "house") : program("apartments", "apartments");
  env.entrance_side = "F";
  return env;
}

// Wharf warehouse (Bryggen in Bergen): a narrow, deep wooden house of three or four storeys with
// its steep gable to the street, in falu red, ochre or white boards, a shop or workshop below and
// rooms above; a row of them lines the harbour front.
std::optional<EnvSpec> wharfhouse_envelope(const ArchetypeCtx& ctx) {
  Rng& rng = *ctx.rng;
  const double U = ctx.frame->U;
  const double V = ctx.frame->V;
  const double depth = js::min(V - vx(1), vx(rng.float_(13, 22)));
  if (depth < vx(12)) return std::nullopt;
  const double floors = rng.chance(0.35) ? 4 : 3;
  std::vector<double> story_h = fill(floors, vx(2.875));
  story_h[0] = vx(3.25);
  EnvSpec env;
  env.tiers.push_back(tier(0, floors - 1, {Rect{0, 0, U - 1, depth - 1}}));
  env.floors = floors;
  env.story_h = std::move(story_h);
  env.basements = 0;
  env.roof.type = "gable";
  env.roof.ridge = "v";
  env.roof.slope = rng.float_(1.2, 1.45);
  overhang_sides(env.roof, 3, 2, 0, 0);
  env.program = program("retail", "apartments");
  env.entrance_side = "F";
  return env;
}

// Forest cabin (hytte): one low floor of tarred logs or falu-red boards on a stone plinth under a
// steep gable roof with deep eaves, a wooden porch at the front door; living room, kitchen, one or
// two bedrooms and a bathroom (interior/cabins.hpp). Freestanding and set back on a ~14-22 m lot.
std::optional<EnvSpec> cabin_envelope(const ArchetypeCtx& ctx) {
  Rng& rng = *ctx.rng;
  const double U = ctx.frame->U;
  const double V = ctx.frame->V;
  const double w = js::min(U - vx(3), vx(rng.float_(6.25, 8.25)));
  const double d = js::min(V - vx(4), vx(rng.float_(7, 9.75)));
  if (w < vx(6) || d < vx(6.5)) return std::nullopt;
  // set back far enough for the porch and its steps
  const double set_f = js::min(V - d - vx(1.5), vx(rng.float_(2.75, 5)));
  const double half = (U - w) / 2;
  const double u0 = js::round(half + rng.float_(-0.2, 0.2) * (U - w));
  EnvSpec env;
  env.tiers.push_back(tier(0, 0, {Rect{u0, set_f, u0 + w - 1, set_f + d - 1}}));
  env.floors = 1;
  env.story_h = {vx(2.625)};
  env.basements = 0;
  env.roof.type = "gable";
  env.roof.ridge = rng.chance(0.6) ? "u" : "v";
  env.roof.slope = rng.float_(0.95, 1.25);
  overhang_number(env.roof, 4);
  env.program.ground = "cabin";  // (upper: null)
  env.entrance_side = "F";
  env.stoop = true;
  env.plinth = true;
  env.porch = true;
  return env;
}

// Wooden church (Norwegian long church) for an old-town square: one tall nave under a steep roof
// with its gable to the street, a square west tower in front (the entrance vestibule) rising above
// the ridge as a boarded belfry with a pyramid spire and a finial cross (massing.js steeple).
// Interior: vestibule + one open nave with pews and an altar.
std::optional<EnvSpec> church_envelope(const ArchetypeCtx& ctx) {
  Rng& rng = *ctx.rng;
  const double U = ctx.frame->U;
  const double V = ctx.frame->V;
  // (a cemetery chapel is smaller than a town's church)
  const bool chapel = ctx.chapel;
  const double nave_w = js::min(U - vx(3), chapel ? vx(rng.float_(9.5, 10.5)) : vx(rng.float_(9.5, 12.5)));
  const double tower_w = clamp(js::round(nave_w * 0.5), vx(4.75), vx(6));
  const double set_f = js::min(vx(rng.float_(3, 6)), V - tower_w - vx(15));
  const double nave_l = js::min(V - set_f - tower_w - vx(2), chapel ? vx(rng.float_(12, 15)) : vx(rng.float_(16, 22)));
  if (nave_w < vx(9.5) || nave_l < vx(12) || set_f < vx(1.5)) return std::nullopt;
  const double u0 = js::round((U - nave_w) / 2);
  const double tu0 = js::round((U - tower_w) / 2);
  // the tower overlaps the nave's front wall: vestibule and nave share a partition
  const double ny0 = set_f + tower_w - 2;
  const Rect nave{u0, ny0, u0 + nave_w - 1, ny0 + nave_l - 1};
  const Rect tower{tu0, set_f, tu0 + tower_w - 1, ny0 + 1};
  const double slope = rng.float_(1.3, 1.6);
  const double ridge = std::floor((nave_w / 2 + 3) * slope) + 1;
  const std::vector<double> story_h = {vx(rng.float_(6, 7))};
  EnvSteeple steeple;
  steeple.rect = 1;
  steeple.shaft = ridge + vx(chapel ? rng.float_(1, 2) : rng.float_(2.5, 4.5));
  steeple.spire = vx(chapel ? rng.float_(4, 6) : rng.float_(8, 12));
  // an Orthodox church (Russian and Karelian towns: ctx.dome, the flavor's dome materials): an
  // onion dome on the bell tower (or a tented spire with a small onion on top), one to three onions
  // on drums along the nave ridge
  std::vector<EnvDome> domes;
  if (!ctx.dome.empty()) {
    const std::string& name = rng.pick(ctx.dome);
    const int id = material_id(name);
    const uint16_t m = id >= 0 ? static_cast<uint16_t>(id) : static_cast<uint16_t>(MAT::DOME_GREEN);
    steeple.dome = m;
    steeple.tent = !chapel && rng.chance(0.35);
    if (!chapel) {
      const double r = clamp(js::round(nave_w * 0.24), 12, vx(2.25));
      const double drum = ridge + vx(rng.float_(1.25, 2.25));
      domes.push_back({0, 0.5, r, drum, js::round(r * 2.4), m});
      if (nave_l >= vx(17) && rng.chance(0.6)) {
        const double r2 = js::round(r * 0.62);
        for (double fv : {0.2, 0.8}) domes.push_back({0, fv, r2, ridge + vx(1), js::round(r2 * 2.4), m});
      }
    }
  }
  EnvSpec env;
  env.tiers.push_back(tier(0, 0, {nave, tower}));
  env.floors = 1;
  env.story_h = story_h;
  env.basements = 0;
  env.roof.type = "gable";
  env.roof.ridge = "v";
  env.roof.slope = slope;
  overhang_sides(env.roof, 2, 3, 3, 3);
  env.program.ground = "church";  // (upper: null)
  env.entrance_side = "F";
  env.steeple = steeple;
  env.domes = std::move(domes);
  return env;
}

// ---- finalizing

// Archetypes whose flat roof a flavor may turn into a pitched one (one rect, apartment or house
// plans).
bool pitchable(const std::string& id) { return id == "walkup" || id == "midrise" || id == "rowhouse"; }

// Pitched roofs by flavor (district.pitched, the chance per district, set by city/flavors.js):
// rendered blocks of old northern and European towns carry gabled roofs along the street,
// freestanding and corner ones hips.
void pitch_roof(EnvSpec& env, const std::string& archetype, const Lot& lot, const District& district, Rng& rng) {
  const double pc = district.pitched.value_or(0);
  if (pc <= 0 || env.roof.type != "flat" || !pitchable(archetype) || env.floors > 7) return;
  if (env.tiers.size() != 1 || env.tiers[0].rects.size() != 1 || !rng.chance(pc)) return;
  EnvRoof roof;
  if (lot.corner || lot.micro) {
    roof.type = "hip";
    roof.slope = rng.float_(0.6, 0.85);
    overhang_number(roof, 2);
  } else {
    roof.type = "gable";
    roof.ridge = "u";
    roof.slope = rng.float_(0.65, 0.95);
    overhang_sides(roof, 3, 3, 0, 0);
  }
  env.roof = roof;
}

// The rise (voxels) of a pitched roof above the top floor: exactly the highest point massing's
// pitchedRoof draws (slope times the distance from the eaves, over the building's span plus the
// overhangs), plus a little margin. The building's rect bounds its top tier, so this is an upper
// bound for every roof type (gable either way, hip, sawtooth).
double roof_rise(const EnvRoof& roof, const Frame& frame) {
  const double slope = js::is_undefined(roof.slope) ? 0.7 : roof.slope;
  double oF = 3, oB = 3, oL = 3, oR = 3;
  if (roof.overhang_kind == EnvRoof::Overhang::Number) {
    oF = oB = oL = oR = roof.overhang;
  } else if (roof.overhang_kind == EnvRoof::Overhang::Sides) {
    oF = js::is_undefined(roof.overhang_f) ? 3 : roof.overhang_f;
    oB = js::is_undefined(roof.overhang_b) ? 3 : roof.overhang_b;
    oL = js::is_undefined(roof.overhang_l) ? 3 : roof.overhang_l;
    oR = js::is_undefined(roof.overhang_r) ? 3 : roof.overhang_r;
  }
  const double du = std::floor((frame.U - 1 + oL + oR) / 2);
  const double dv = std::floor((frame.V - 1 + oF + oB) / 2);
  const double d = roof.type == "hip" ? js::min(du, dv) : roof.type == "sawtooth" ? js::min(47.0, frame.V - 1 + oF + oB) / 3 : roof.ridge == "v" ? du : dv;
  return std::floor(d * slope) + 1 + 4;
}

ArchetypeCtx make_ctx(const Lot& lot, const Frame& lot_frame, const District& district, Rng& rng, const EnvelopeExtra& extra) {
  ArchetypeCtx ctx;
  ctx.lot = &lot;
  ctx.frame = &lot_frame;
  ctx.district = &district;
  ctx.rng = &rng;
  ctx.u = extra.u;
  ctx.core = extra.core;
  ctx.ground_z = extra.ground_z;
  ctx.config = extra.config;
  ctx.chapel = extra.chapel;
  ctx.dome = extra.dome;
  ctx.pitched_civic = extra.pitched_civic;
  return ctx;
}

// The envelope record from its building frame, world box, canonical tiers and annexes.
Envelope finalize_with(const Lot& lot, const std::string& archetype, const std::string& style_id, const EnvSpec& env, const EnvelopeExtra& extra, const Frame& frame,
                       const Rect& R, std::vector<EnvTier> tiers, std::vector<EnvelopeAnnex> annexes) {
  double height_above = 0;
  for (double h : env.story_h) height_above = height_above + h;
  const double base_z = extra.ground_z + (env.stoop ? 4 : 1);
  double roof_extra = env.roof.type == "flat" ? vx(4.5) : roof_rise(env.roof, frame);
  if (env.steeple) {
    const bool dome = env.steeple->dome && *env.steeple->dome != 0;
    roof_extra = js::max(roof_extra, env.steeple->shaft + env.steeple->spire + (dome ? 34 : 24));
  }
  const double margin = vx(2);
  Rect bounds{R.x0 - margin, R.y0 - margin, R.x1 + margin, R.y1 + margin};
  for (const EnvelopeAnnex& a : annexes) {
    bounds.x0 = js::min(bounds.x0, a.world.x0 - margin);
    bounds.y0 = js::min(bounds.y0, a.world.y0 - margin);
    bounds.x1 = js::max(bounds.x1, a.world.x1 + margin);
    bounds.y1 = js::max(bounds.y1, a.world.y1 + margin);
  }
  const double basement_h = vx(3.25);
  // (canonical lot position: a wrapping world mirrors the same buildings every lap)
  if (!extra.config) SVX_FAIL("archetypes: an envelope finalized without the world's config");
  const Wrap W = wrap_of(*extra.config);
  const bool mirror = (hash32((*extra.config)["seed"].to_number(), W.vi(lot.rect.x0), W.vi(lot.rect.y0), 3) & 1) == 1;
  const Archetype& arch = archetype_registry().get(archetype);
  double entrance_u;
  if (arch.entrance_u)
    entrance_u = arch.entrance_u(env, frame.U, frame.V, mirror);
  else if (archetype == "house" || archetype == "rowhouse")
    entrance_u = house_columns(frame.U, mirror).entrance_u;
  else
    entrance_u = std::floor(frame.U / 2);
  Envelope out;
  out.mirror = mirror;
  out.entrance_u = entrance_u;
  out.id = js::cat(lot.id, "/B");
  out.lot = lot.id;
  out.archetype = archetype;
  out.style = style_id;
  out.district = lot.district;
  out.front = lot.front;
  out.R = R;
  out.U = frame.U;
  out.V = frame.V;
  out.tiers = std::move(tiers);
  out.annexes = std::move(annexes);
  out.floors = env.floors;
  out.story_h = env.story_h;
  out.basements = env.basements;
  out.basement_h = basement_h;
  out.base_z = base_z;
  out.ground_z = extra.ground_z;
  out.roof = env.roof;
  out.program = env.program;
  out.podium_floors = env.podium_floors.value_or(0);
  out.stoop = env.stoop;
  out.yard = env.yard;
  out.plinth = env.plinth;
  out.porch = env.porch;
  out.steeple = env.steeple;
  out.domes = env.domes;
  out.extra = env.extra;
  out.top_z = base_z + height_above + roof_extra;
  out.bottom_z = base_z - env.basements * basement_h - 2;
  out.bounds = bounds;
  return out;
}

// A turned envelope (the angled world): the lot and the building share the lot's placement, so the
// footprints stay canonical, shifted to the building's own corner; env.turn records the building
// frame (frame_of), env.R is the world box of the turned footprint and annexes keep a canonical
// rect (canon) beside their world box.
Envelope finalize_turned(const Lot& lot, const Frame& lot_frame, const std::string& archetype, const std::string& style_id, const EnvSpec& env,
                         const EnvelopeExtra& extra) {
  double bu0 = js::kInf, bv0 = js::kInf, bu1 = -js::kInf, bv1 = -js::kInf;
  for (const EnvTier& t : env.tiers)
    for (const Rect& r : t.rects) {
      bu0 = js::min(bu0, r.x0);
      bv0 = js::min(bv0, r.y0);
      bu1 = js::max(bu1, r.x1);
      bv1 = js::max(bv1, r.y1);
    }
  auto shift = [&](const Rect& r) { return Rect{r.x0 - bu0, r.y0 - bv0, r.x1 - bu0, r.y1 - bv0}; };
  const Frame frame = lot_frame.shifted(bu0, bv0, bu1 - bu0 + 1, bv1 - bv0 + 1);
  const Rect R = frame.rect_to_world(Rect{0, 0, frame.U - 1, frame.V - 1});
  std::vector<EnvTier> tiers;
  for (const EnvTier& t : env.tiers) {
    EnvTier s{t.f0, t.f1, {}};
    for (const Rect& r : t.rects) s.rects.push_back(shift(r));
    tiers.push_back(std::move(s));
  }
  std::vector<EnvelopeAnnex> annexes;
  for (const EnvAnnex& a : env.annexes) {
    EnvelopeAnnex x;
    static_cast<EnvAnnex&>(x) = a;
    x.canon = shift(a.rect);
    x.world = frame.rect_to_world(*x.canon);
    annexes.push_back(std::move(x));
  }
  Envelope out = finalize_with(lot, archetype, style_id, env, extra, frame, R, std::move(tiers), std::move(annexes));
  out.turn = frame.turn();
  return out;
}

}  // namespace

Envelope finalize_envelope(const Lot& lot, const Frame& lot_frame, const std::string& archetype, const std::string& style_id, const EnvSpec& env,
                           const EnvelopeExtra& extra) {
  if (lot_frame.turned) return finalize_turned(lot, lot_frame, archetype, style_id, env, extra);
  std::vector<Rect> world_rects;
  for (const EnvTier& t : env.tiers)
    for (const Rect& r : t.rects) world_rects.push_back(lot_frame.rect_to_world(r));
  double bx0 = js::kInf, by0 = js::kInf, bx1 = -js::kInf, by1 = -js::kInf;
  for (const Rect& r : world_rects) {
    bx0 = js::min(bx0, r.x0);
    by0 = js::min(by0, r.y0);
    bx1 = js::max(bx1, r.x1);
    by1 = js::max(by1, r.y1);
  }
  const Rect R{bx0, by0, bx1, by1};
  const Frame frame(R, lot.front);
  std::vector<EnvTier> tiers;
  for (const EnvTier& t : env.tiers) {
    EnvTier s{t.f0, t.f1, {}};
    for (const Rect& r : t.rects) s.rects.push_back(frame.rect_from_world(lot_frame.rect_to_world(r)));
    tiers.push_back(std::move(s));
  }
  std::vector<EnvelopeAnnex> annexes;
  for (const EnvAnnex& a : env.annexes) {
    EnvelopeAnnex x;
    static_cast<EnvAnnex&>(x) = a;
    x.world = lot_frame.rect_to_world(a.rect);
    annexes.push_back(std::move(x));
  }
  return finalize_with(lot, archetype, style_id, env, extra, frame, R, std::move(tiers), std::move(annexes));
}

std::optional<Envelope> plan_building_envelope(const Lot& lot, const District& district, Rng& rng, const EnvelopeExtra& extra) {
  const Frame lot_frame = lot_frame_of(lot.turn, lot.rect, lot.front);
  const double U = lot_frame.U;
  const double V = lot_frame.V;
  WeightedIds options;
  for (const auto& p : district.archetypes)
    if (archetype_registry().get(p.first).fits(U, V)) options.push_back(p);
  std::string choice;
  if (!options.empty()) choice = rng.weighted(options);
  if (choice.empty()) {
    std::vector<std::string> fallbacks;
    for (const char* id : {"walkup", "rowhouse", "house", "warehouse"})
      if (archetype_registry().get(id).fits(U, V)) fallbacks.emplace_back(id);
    if (fallbacks.empty() || district.archetypes.empty()) return std::nullopt;
    choice = fallbacks[0];
  }
  const Archetype& arch = archetype_registry().get(choice);
  std::optional<EnvSpec> env = arch.envelope(make_ctx(lot, lot_frame, district, rng, extra));
  if (!env) return std::nullopt;
  pitch_roof(*env, choice, lot, district, rng);
  std::string style_id;
  if (!env->force_style.empty())
    style_id = env->force_style;
  else
    style_id = !district.styles.empty() ? rng.weighted(district.styles) : std::string("concrete");
  return finalize_envelope(lot, lot_frame, choice, style_id, *env, extra);
}

std::optional<Envelope> plan_building_envelope_as(const Lot& lot, const std::string& archetype_id, const std::string& style_id, const District& district, Rng& rng,
                                                  const EnvelopeExtra& extra) {
  const Frame lot_frame = lot_frame_of(lot.turn, lot.rect, lot.front);
  const Archetype& arch = archetype_registry().get(archetype_id);
  if (!arch.fits(lot_frame.U, lot_frame.V)) return std::nullopt;
  std::optional<EnvSpec> env = arch.envelope(make_ctx(lot, lot_frame, district, rng, extra));
  if (!env) return std::nullopt;
  return finalize_envelope(lot, lot_frame, archetype_id, style_id, *env, extra);
}

const Frame& envelope_frame(const Envelope& env) {
  return env.frame_cache.get([&] { return frame_of(env.turn, env.U, env.V, env.front, env.R); });
}

const std::vector<Rect>& tier_rects(const Envelope& env, double f) {
  static const std::vector<Rect> kNone;
  const double ff = js::max(0.0, f);
  for (const EnvTier& t : env.tiers)
    if (ff >= t.f0 && ff <= t.f1) return t.rects;
  return kNone;
}

double floor_z(const Envelope& env, double f) {
  if (f < 0) return env.base_z + f * env.basement_h;
  double z = env.base_z;
  for (double k = 0; k < f; k += 1) z += k < static_cast<double>(env.story_h.size()) ? env.story_h[static_cast<size_t>(k)] : js::kNaN;
  return z;
}

double floor_height(const Envelope& env, double f) {
  if (f < 0) return env.basement_h;
  if (f != std::floor(f) || f >= static_cast<double>(env.story_h.size())) return js::kNaN;  // (undefined)
  return env.story_h[static_cast<size_t>(f)];
}

void register_archetypes() {
  Registry<Archetype>& R = archetype_registry_mut();
  auto add = [&](const char* id, const char* label, std::function<bool(double, double)> fits, std::function<std::optional<EnvSpec>(const ArchetypeCtx&)> envelope,
                 std::function<double(const EnvSpec&, double, double, bool)> entrance_u = nullptr) {
    Archetype a;
    a.id = id;
    a.label = label;
    a.fits = std::move(fits);
    a.envelope = std::move(envelope);
    a.entrance_u = std::move(entrance_u);
    R.add(std::move(a));
  };
  add("house", "", [](double U, double V) { return U >= vx(12) && V >= vx(21); }, house_envelope);
  add("rowhouse", "", [](double U, double V) { return U >= vx(7) && U <= vx(11) && V >= vx(14); }, rowhouse_envelope);
  add("walkup", "", [](double U, double V) { return U >= vx(9) && V >= vx(13); }, walkup_envelope);
  add("midrise", "", [](double U, double V) { return U >= vx(16) && V >= vx(17); }, midrise_envelope);
  add("panelSlab", "", [](double U, double V) { return U >= vx(24) && V >= vx(12); }, panel_slab_envelope);
  add("panelTower", "", [](double U, double V) { return U >= vx(20) && V >= vx(20); }, panel_tower_envelope);
  add("office", "", [](double U, double V) { return U >= vx(20) && V >= vx(20); }, office_envelope);
  add("tower", "", [](double U, double V) { return U >= vx(30) && V >= vx(30); }, tower_envelope);
  add("warehouse", "", [](double U, double V) { return U >= vx(25) && V >= vx(30); }, warehouse_envelope);
  add("barn", "", [](double U, double V) { return U >= vx(16) && V >= vx(20); }, barn_envelope);
  add("factory", "", [](double U, double V) { return U >= vx(30) && V >= vx(35); }, factory_envelope);
  add("garage", "Parking garage", [](double U, double V) { return U >= vx(32) && U <= vx(70) && V >= vx(40); }, garage_envelope);
  // (civic: placed on whole blocks by the cell plan, never picked by weight)
  add("school", "School", [](double U, double V) { return U >= vx(44) && V >= vx(36); }, school_envelope);
  // (old-town block ends and corner plots run to ~26 m: a broad merchant's house)
  add("townhouse", "Town house", [](double U, double V) { return U >= vx(7.5) && U <= vx(26) && V >= vx(11); }, townhouse_envelope,
      [](const EnvSpec& env, double U, double V, bool mirror) { return env.program.upper == "house" ? house_columns(U, mirror).entrance_u : std::floor(U / 2); });
  add("wharfhouse", "Wharf warehouse", [](double U, double V) { return U >= vx(6.5) && U <= vx(13) && V >= vx(14); }, wharfhouse_envelope);
  add("cabin", "Cabin", [](double U, double V) { return U >= vx(10) && V >= vx(11); }, cabin_envelope,
      [](const EnvSpec& env, double U, double V, bool mirror) { return cabin_layout(U, V, mirror).entrance_u; });
  add("church", "Church", [](double U, double V) { return U >= vx(13) && V >= vx(24); }, church_envelope);
}

}  // namespace svx::city
