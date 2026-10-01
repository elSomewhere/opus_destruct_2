// svx_city — the ground surfaces of lots and open spaces (voxel_city city/landscape.js): yards,
// driveways, forecourts, churchyards, kitchen gardens, plazas, squares, gardens, parks, car parks,
// sports courts, courtyards, cemeteries, allotments, garage yards, wasteland, quays and industry
// yards. Pure functions of the plan records, so they can be sampled per column at any LOD.
//
// (hx, hy) is the position texture hashes read: canonical in a wrapping world (compose passes the
// column's wrapped position; JS's default is (x, y)).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "buildings/frame.hpp"
#include "city/space.hpp"
#include "core/rect.hpp"

namespace svx::city {

// What lotSurface reads of the building on a lot (buildings/archetypes.js's envelope record), made
// once per envelope by its port:
//   frame       frameOf(env) (turned when the envelope is)
//   ground      env.tiers[0].rects (canonical)
//   annexes     env.annexes: kind, canon (a turned envelope's canonical rect; JS: undefined
//               otherwise), world (the world rect)
//   entrance_u  env.entranceU (nullopt: undefined - floor(U / 2) is read then)
//   U, V, R     the building frame's size and world rect
//   archetype   env.archetype
//   civic       env.civic: a civic building's id ("": undefined)
struct LotEnvAnnex {
  std::string kind;
  std::optional<Rect> canon = std::nullopt;
  Rect world{};
};
struct LotEnv {
  Frame frame;
  std::vector<Rect> ground;
  std::vector<LotEnvAnnex> annexes;
  std::optional<double> entrance_u = std::nullopt;
  double U = 0, V = 0;
  std::string archetype;
  Rect R{};
  std::string civic;
};

// lotSurface(lot, env, x, y, seed, hx, hy): the material of a lot's ground at world column (x, y)
// - concrete under the building and its annexes, then by archetype: a cabin's clearing, a
// churchyard, a house's driveway, path and flower beds, row houses' front paths, a schoolyard with
// its court, a works' car park, an old-town plot's flagstones and back court or kitchen garden, a
// civic building's forecourt (civic_yard), an office plaza, an apartment block's paved front and
// green court. `env` null: a lot without a building (gravel, bare earth, dry grass). (The
// reference passes the lot and reads nothing of it.)
uint16_t lot_surface(const LotEnv* env, double x, double y, double seed, double hx, double hy);
inline uint16_t lot_surface(const LotEnv* env, double x, double y, double seed) { return lot_surface(env, x, y, seed, x, y); }

// spaceSurface(space, x, y, out, hx, hy): an open space's ground at world column (x, y): out.mat,
// out.dz (a pond or a basin below the ground, a basin's rim above it) and out.water.
void space_surface(const OpenSpace& space, double x, double y, SpaceSample& out, double hx, double hy);
inline void space_surface(const OpenSpace& space, double x, double y, SpaceSample& out) { space_surface(space, x, y, out, x, y); }

// Cobbles (setts of about 25 cm) in three shades, from a canonical position hash.
uint16_t cobble(double hx, double hy);

// The frame of a space (cemeteries and allotments: u along its main street, v inwards): its rect
// facing `front ?? "S"`, made once per space.
const Frame& space_frame(const OpenSpace& space);

// Allotment plots of about 8 x 11 m in rows off gravel paths (the plot grid in the block's gate
// frame; JS ALLOT): w vx(8), d vx(11), path vx(1.5).
struct AllotSize {
  double w = 0, d = 0, path = 0;
};
constexpr AllotSize kAllot{64, 88, 12};

}  // namespace svx::city
