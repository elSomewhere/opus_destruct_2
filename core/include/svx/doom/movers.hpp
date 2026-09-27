// structvox — Doom's moving sector planes as engine movers (plan Phase 7): doors, lifts, floors,
// ceilings, platforms, crushers, stairs and the donut.
#pragma once

#include "svx/doom/world.hpp"
#include "svx/engine/engine.hpp"

namespace svx::doom {

// Adds the world's movers to the engine with vanilla speeds and waits (doom/specials) and sets
// the engine's resolvers: switch lines (S1 / SR) on use, walk-over lines (W1 / WR) on viewer
// moves, gun lines (G1 / GR) on hitscan carves. Doors opened by a manual door line and lifts
// are usable directly. `w` must outlive the engine's use of the resolvers. Returns the number
// of movers.
int attach_doom_movers(Engine& eng, const DoomWorld& w);

}  // namespace svx::doom
