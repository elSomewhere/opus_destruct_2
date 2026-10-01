// svx_city — how the city's props and furniture attach in a structvox world (docs/
// PROCGEN_MERGE_PLAN.md §10.2), what people can do with them (§10.8) and, for an entity, the
// game's kind of it: data/city/props.json. The export draws a prop by its class: fixed ones as
// structure bonded to what holds them, loose ones with seams on every outer face (a resting
// object), entities not at all (a spawn record), plants as decorative voxels.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace svx::city {

enum class Attachment : uint8_t { Fixed, Loose, Entity, Decorative };

// What a person can do with it (a bit each).
enum Use : uint8_t { kUseSit = 1, kUseSleep = 2, kUseWork = 4, kUseEat = 8, kUseOpen = 16, kUseClimb = 32 };

struct PropClass {
  Attachment attach = Attachment::Fixed;
  uint8_t uses = 0;
  std::string entity;  // (an entity's kind for the game: "car", "fire_truck", ...)
};

// The class of a prop of a group - "props" (city/propPrefabs PROPS), "furniture" (interior
// PREFABS), "civic" (CIVIC_PREFABS) - by its kind; nullptr: not in the table.
const PropClass* prop_class(std::string_view group, std::string_view kind);

}  // namespace svx::city
