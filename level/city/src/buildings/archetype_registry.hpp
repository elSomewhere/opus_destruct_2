// svx_city — the building archetypes' registry (voxel_city world/registry.js ARCHETYPES) and
// what an archetype is (voxel_city buildings/archetypes.js: "massing rules per building type").
//
// `fits(U, V)` says whether a lot frame of U x V cells can hold it; `envelope(ctx)` runs during
// cell planning (cheap: footprint tiers, floor counts, heights, program) and returns the
// building's raw envelope in LOT-canonical coordinates (u along the frontage, v from the street),
// or none (the lot stays open); finalizeEnvelope (buildings/archetypes) then normalizes it.
// `entrance_u(env, U, V, mirror)` (optional) tells the lot dressing where the front door is
// before the interior is planned.
//
// Filled by buildings/civic.cpp (the 17 civic buildings: register_civic) and then by
// buildings/archetypes.cpp (the rest), in that order (docs/CITY.md §2.2). This header holds the
// types both need; the archetypes' port adds its functions (planBuildingEnvelope, finalize ...)
// in buildings/archetypes.hpp and may add fields here.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "buildings/frame.hpp"
#include "city/districts.hpp"
#include "core/hash.hpp"
#include "core/rect.hpp"
#include "core/value.hpp"
#include "world/registry.hpp"

namespace svx::city {

struct Lot;  // (city/lots.hpp: the lot record)

// A tier of floors f0..f1 sharing footprint rects (canonical).
struct EnvTier {
  double f0 = 0, f1 = 0;
  std::vector<Rect> rects;
};

// An annex: a garage, a petrol canopy, a pylon ... (rect canonical, height in voxels).
struct EnvAnnex {
  std::string kind;
  Rect rect;
  double height = 0;
};

// A roof: type "flat", "gable", "hip", "sawtooth"; the optional keys as JS leaves them (NaN,
// "" or none: undefined).
struct EnvRoof {
  std::string type = "flat";
  double pitch = js::kNaN;  // (house, barn: 1)
  std::string ridge;        // "u" (ridge along the street) | "v" (gable to the street)
  double slope = js::kNaN;  // rise per voxel (default 0.7)
  // overhang: undefined, a number of voxels, or per side {F, B, L, R} (NaN: a side left out)
  enum class Overhang : uint8_t { None, Number, Sides };
  Overhang overhang_kind = Overhang::None;
  double overhang = 0;
  double overhang_f = js::kNaN, overhang_b = js::kNaN, overhang_l = js::kNaN, overhang_r = js::kNaN;
  std::optional<bool> crown, chimney;
};

// What the floors hold: the ground floor's program, the podium's, the upper floors' ("": null or
// undefined).
struct EnvProgram {
  std::string ground, podium, upper;
};

// A church tower: ground-tier rect `rect` rising `shaft` voxels above the eaves, then a spire; an
// onion dome (material) and a tented spire on Orthodox churches.
struct EnvSteeple {
  double rect = 0, shaft = 0, spire = 0;
  std::optional<uint16_t> dome;
  std::optional<bool> tent;
};
struct EnvDome {
  double rect = 0, fv = 0, r = 0, drum = 0, h = 0;
  uint16_t m = 0;
};

// The civic buildings' extra fields (buildings/civic.js), spread onto their envelope.
struct CivicExtra {
  std::string civic;
  bool storefront = false;
  std::string sign;  // ("": null)
  uint16_t sign_color = 0;
  bool portico = false, parking = false;
  double set_f = 0;  // the building's distance from the lot front (voxels)
};

// An archetype's raw envelope (what envelope(ctx) returns).
struct EnvSpec {
  std::vector<EnvTier> tiers;
  std::vector<EnvAnnex> annexes;
  double floors = 0;
  std::vector<double> story_h;
  double basements = 0;
  EnvRoof roof;
  EnvProgram program;
  std::optional<double> podium_floors;
  std::string entrance_side = "F";
  bool stoop = false, yard = false, plinth = false, porch = false;
  std::optional<EnvSteeple> steeple;
  std::vector<EnvDome> domes;  // (empty: none)
  std::string force_style;     // ("": none)
  std::optional<CivicExtra> extra;
};

// The context of envelope(ctx): { lot, frame (the lot's frame), district (flavored), rng, u, core,
// groundZ, config } and what a caller adds (chapel, dome: a flavor's church dome materials,
// pitchedCivic: the chance of a pitched civic roof, 0 none).
struct ArchetypeCtx {
  const Lot* lot = nullptr;
  const Frame* frame = nullptr;
  const District* district = nullptr;
  Rng* rng = nullptr;
  double u = 0, core = 0, ground_z = 0;
  const Value* config = nullptr;
  bool chapel = false;
  std::vector<std::string> dome;
  double pitched_civic = 0;
};

struct Archetype {
  std::string id;
  std::string label;  // ("": undefined)
  bool civic = false;
  std::function<bool(double U, double V)> fits;
  std::function<std::optional<EnvSpec>(const ArchetypeCtx& ctx)> envelope;
  std::function<double(const EnvSpec& env, double U, double V, bool mirror)> entrance_u;  // (empty: none)
};

// ARCHETYPES: read-only once register_all() has run; _mut for registration only.
const Registry<Archetype>& archetype_registry();
Registry<Archetype>& archetype_registry_mut();

}  // namespace svx::city
