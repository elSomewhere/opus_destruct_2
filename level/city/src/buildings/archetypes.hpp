// svx_city — building archetypes: massing ("envelope") rules per building type (voxel_city
// buildings/archetypes.js).
//
// An archetype's envelope(ctx) runs during cell planning (cheap: footprint tiers, floor counts,
// heights, program) and is all that coarse LODs and maps need; the full interior is planned later,
// lazily (buildings/interior). register_archetypes() registers the 17 archetypes of archetypes.js
// (house, rowhouse, walkup, midrise, panelSlab, panelTower, office, tower, warehouse, barn,
// factory, garage, school, townhouse, wharfhouse, cabin, church) after civic.js's
// (archetype_registry.hpp). Footprints come in LOT-canonical coordinates (u along the frontage, v
// from the street); finalize_envelope turns them into the building's envelope: a tight building
// frame (turned on a turned lot), world boxes, heights.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "buildings/archetype_registry.hpp"
#include "buildings/chamfer.hpp"
#include "buildings/facade.hpp"
#include "buildings/frame.hpp"
#include "buildings/wing.hpp"
#include "city/districts.hpp"
#include "city/lots.hpp"
#include "core/cache.hpp"
#include "core/hash.hpp"
#include "core/rect.hpp"
#include "core/value.hpp"

namespace svx::city {

// planBuildingEnvelope's `extra`: what the cell plan adds to an archetype's context (JS spreads it
// into ctx): the lot's urbanization and downtown-ness, its ground level (voxels) and the world's
// config (its seed and wrap: the mirror hash); a cemetery chapel, a flavor's church dome
// materials, the chance of a pitched civic roof.
struct EnvelopeExtra {
  double u = 0, core = 0, ground_z = 0;
  const Value* config = nullptr;
  bool chapel = false;
  std::vector<std::string> dome{};  // (empty: undefined)
  double pitched_civic = 0;         // (0: undefined)
};

// An annex of a building ({...annex, world, canon?}): its world box, and on a turned building its
// canonical rect (shifted to the building's corner) beside it.
struct EnvelopeAnnex : EnvAnnex {
  Rect world{};
  std::optional<Rect> canon{};
};

// A door where a skybridge meets a building (city/skybridges.js: env.skyDoors[k] = { floor, span,
// bridge }): its floor, the clear width on the facade (world coordinates: x0..x1 when the two
// buildings face each other across y - JS alongX, span {x0, x1} - else y0..y1) and the bridge.
struct SkyDoor {
  double floor = 0;
  bool span_x = true;  // the span is {x0, x1} (else {y0, y1})
  double s0 = 0, s1 = 0;
  std::string bridge{};
};

// A building's envelope (finalizeEnvelope's record): what the cell plan keeps per building and
// every later stage (massing, interiors, parts) reads. Canonical coordinates are the building
// frame's (envelope_frame): u along the front, v from it.
struct Envelope {
  bool mirror = false;    // the floor plans mirrored left to right (a hash of the canonical lot position)
  double entrance_u = 0;  // where the front door is along the front (canonical u)
  std::string id{};       // "<lot id>/B"
  std::string lot{};      // the lot's id
  std::string archetype{}, style{};
  std::string district{};  // the lot's district id
  char front = 'N';        // the world side of the street facade
  Rect R{};                // the footprint's world box (a turned building: its turned footprint's)
  double U = 0, V = 0;     // the building frame's size
  std::vector<EnvTier> tiers{};  // canonical footprints by floor range
  std::vector<EnvelopeAnnex> annexes{};
  double floors = 0;
  std::vector<double> story_h{};
  double basements = 0, basement_h = 0;
  double base_z = 0;    // the ground floor slab's bottom (voxels)
  double ground_z = 0;  // the lot's ground level
  EnvRoof roof{};
  EnvProgram program{};
  double podium_floors = 0;
  bool stoop = false, yard = false;
  bool plinth = false, porch = false;  // (JS sets these keys only when true)
  std::optional<EnvSteeple> steeple{};
  std::vector<EnvDome> domes{};          // (empty: none)
  std::optional<CivicExtra> extra{};     // a civic building's fields (JS spreads env.extra onto the envelope)
  double top_z = 0, bottom_z = 0;        // the vertical extent of everything it draws
  Rect bounds{};                         // the world box of everything it draws (annexes, eaves)
  std::optional<Turn> turn{};            // a turned building's frame (the angled world)
  // ---- set by the cell plan once the envelope is made (city/cellPlan.js)
  std::string flavor{};          // the town flavor's id ("": undefined)
  bool pitched_ramps = false;    // (a garage on a slope)
  std::string part{};            // the part it is cast into ("": none)
  std::optional<Chamfer> chamfer{};  // a chamfered front corner (wings.js)
  std::vector<Wing> wings{};         // its wings (wings.js planWings; empty: none - JS undefined)
  std::vector<SkyDoor> sky_doors{};  // where skybridges meet it (city/skybridges.js; empty: none)

  // frameOf(env) and buildingLook(env, seed), made on first use (envelope_frame, building_look).
  Lazy<Frame> frame_cache{};
  Lazy<BuildingLook> look_cache{};
};

// buildings/archetypes.js's registrations (register_all calls it after register_civic).
void register_archetypes();

// planBuildingEnvelope: chooses an archetype for a lot (weighted among the district's that fit,
// else the first fallback that does) and builds its envelope; none: the lot stays open (a yard,
// parking). Draws from rng: the archetype, its envelope, a flavor's pitched roof, the style.
std::optional<Envelope> plan_building_envelope(const Lot& lot, const District& district, Rng& rng, const EnvelopeExtra& extra);
// planBuildingEnvelopeAs: the envelope of a forced archetype and style (sites, special plans);
// none when the archetype does not fit the lot or gives no envelope.
std::optional<Envelope> plan_building_envelope_as(const Lot& lot, const std::string& archetype_id, const std::string& style_id, const District& district, Rng& rng,
                                                  const EnvelopeExtra& extra);
// finalizeEnvelope: an archetype's raw envelope on a lot (its frame lot_frame) as the building's
// envelope: lot-canonical footprints to world boxes and a tight building frame (a turned lot: the
// lot's turned frame shifted to the building's corner).
Envelope finalize_envelope(const Lot& lot, const Frame& lot_frame, const std::string& archetype, const std::string& style_id, const EnvSpec& env,
                           const EnvelopeExtra& extra);

// envelopeFrame(env): the building's frame (frameOf: turned when the envelope is), made once.
const Frame& envelope_frame(const Envelope& env);
// tierRects(env, f): the footprint rects (canonical) of floor f; basements use the ground tier.
const std::vector<Rect>& tier_rects(const Envelope& env, double f);
// floorZ(env, f): the z of the bottom of floor f's slab (negative f: basements); NaN beyond the
// story heights.
double floor_z(const Envelope& env, double f);
// floorHeight(env, f): the story height of floor f (NaN: undefined, beyond the stories).
double floor_height(const Envelope& env, double f);

}  // namespace svx::city
