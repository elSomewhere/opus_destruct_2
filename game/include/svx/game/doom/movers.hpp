// structvox game — Doom's moving sector planes as game movers: doors, lifts, floors, ceilings,
// platforms, crushers, stairs and the donut.
#pragma once

#include "svx/game/doom/world.hpp"
#include "svx/game/game.hpp"

namespace svx::doom {

// Adds the map's movers to the game with vanilla speeds and waits (doom/specials) and sets the
// game's resolvers: switch lines (S1 / SR) on use, walk-over lines (W1 / WR) on viewer moves,
// gun lines (G1 / GR) on hitscan carves. Doors opened by a manual door line and lifts are
// usable directly. `w` must outlive the game's use of the resolvers. Returns the number of
// movers.
int attach_doom_movers(Game& game, const DoomWorld& w);

}  // namespace svx::doom
