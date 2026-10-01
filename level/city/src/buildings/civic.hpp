// svx_city — civic buildings, venues and big shops (voxel_city buildings/civic.js): archetypes
// whose envelopes come from one table (CIVIC). Each entry gives the footprint (m, along the street
// and deep), floors and story heights, setbacks (a forecourt, a car park in front), and what the
// front shows (a shop window, a sign, a portico, a petrol canopy); `lot` is the lot a town carves
// for it (m). civic_style picks the facade style by town flavor (classical stone museums and town
// halls, wooden northern ones, Stalinist houses of culture ...).
#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "core/hash.hpp"

namespace svx::city {

struct CivicSpec {
  std::string id{};
  std::array<double, 2> w{}, d{};  // footprint ranges (m)
  std::array<double, 2> floors{};
  std::vector<double> story{};             // story heights (m), the last for every floor above
  std::array<double, 2> set_f{}, set_s{};  // front and side setbacks (m)
  std::array<double, 2> lot{};             // the lot a town carves (m)
  std::string front{};  // "shop" or "" (undefined)
  std::string sign{};   // "fascia", "plate", "marquee", "neon", "cross", "police", "fire", "banners" ("": undefined)
  std::string roof{};   // "gable" or "" (undefined)
  bool parking = false, canopy = false, portico = false;
};

// CIVIC, in its key order, and CIVIC[id] (null: none).
const std::vector<CivicSpec>& civic_table();
const CivicSpec* civic_spec(std::string_view id);

// The flavor group of a town flavor id ("" for null): "default", "nordic" or "soviet".
std::string civic_group(std::string_view flavor_id);
// A facade style for civic building `id` in a town of flavor `flavor_id` (draws from rng only when
// a registered style is listed).
std::string civic_style(std::string_view id, std::string_view flavor_id, Rng& rng);

// buildings/civic.js's registrations: an archetype per CIVIC entry (ARCHETYPES) and the
// "classical" style (STYLES). register_all calls it after register_styles.
void register_civic();

}  // namespace svx::city
