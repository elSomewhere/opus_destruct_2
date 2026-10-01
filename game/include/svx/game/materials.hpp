// structvox game — the game's materials: what its vehicles and roads are made of, on top of the
// core's standard presets (svx/material/material.hpp). The core knows materials only by their
// properties (strengths, crumpling, penetration, grip); these are content, registered in the
// process's table at fixed ids right after the core's presets, so saved sessions and replays
// keep their meaning:
//
//   sheet      a car's body: pressed sheet steel and its stiffeners, smeared (crumples)
//   car_frame  a car's frame rails, floor pan and pillars (crumples, stiffer)
//   engine     an engine block and gearbox (solid, heavy: it barely crumples)
//   window     a car's glazing: 5 mm glass smeared over a voxel (light, shatters)
//   tyre       rubber on a rim (a wheel that came off)
//   plastic    bumpers, trim, lamp housings (crumples, light)
//   asphalt    road surface (anchored ground: tyres grip it)
//   paint      road markings (anchored ground)
//   lamp       head and tail lamps' glass (shatters)
#pragma once

#include "svx/material/material.hpp"

namespace svx {

class FireSystem;

namespace mat {
constexpr MaterialId Sheet = static_cast<MaterialId>(kStandardMaterials + 0);
constexpr MaterialId CarFrame = static_cast<MaterialId>(kStandardMaterials + 1);
constexpr MaterialId Engine = static_cast<MaterialId>(kStandardMaterials + 2);
constexpr MaterialId Window = static_cast<MaterialId>(kStandardMaterials + 3);
constexpr MaterialId Tyre = static_cast<MaterialId>(kStandardMaterials + 4);
constexpr MaterialId Plastic = static_cast<MaterialId>(kStandardMaterials + 5);
constexpr MaterialId Asphalt = static_cast<MaterialId>(kStandardMaterials + 6);
constexpr MaterialId Paint = static_cast<MaterialId>(kStandardMaterials + 7);
constexpr MaterialId Lamp = static_cast<MaterialId>(kStandardMaterials + 8);
constexpr int kGameMaterials = 9;
}  // namespace mat

// Puts the game's materials into the process's table (default_materials) at their ids, once:
// again only if something reset or overrode them. Worlds made after it have them; Game calls it
// before it makes its world, a host that makes worlds of its own with the game's content calls it
// first.
void register_game_materials();

// How the game's materials burn (the environment's fire: a wreck on fire - its plastic and tyres
// burn, its sheet steel conducts and cools as steel).
void set_game_fire_materials(FireSystem& fire);

}  // namespace svx
