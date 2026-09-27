// structvox — detached-island detection after ruptures, fractures or carves.
//
// Lockstep multi-source search (plan §B7). Every seed (a surviving cell next to a change:
// the ends of a broken bond, the neighbours of a removed cell) starts a search over intact
// bonds; the searches advance one cell each per round and merge when they touch. A search
// stops when it reaches a support (a live cell with a Dirichlet DOF or a support spring); a
// search that exhausts without one is a detached island.
// Invariant shortcut: before the change every seed was in a supported component, so while no
// search has reached a support and a single search is left alive, that search holds the
// support(s) and stops at once (valid only if no support was removed by the change). A cut
// that separates nothing therefore costs O(size of the cut) and one that cuts off a piece
// O(piece), independent of the structure size — unless some search reached a support first,
// in which case the others run to the nearest support (Phase 3 hierarchical labels bound
// that case). Counters make the work measurable (plan Phase 1 gate).
#pragma once

#include <span>
#include <vector>

#include "svx/solve/lattice.hpp"

namespace svx {

struct ConnStats {
  i64 visited = 0;      // cells expanded by the searches
  i64 searches = 0;     // searches started
  i64 shortcut = 0;     // searches ended by the class invariant
};

struct CutSet {
  std::vector<i32> seeds;
  bool supports_removed = false;  // a removed cell was a support: no invariant shortcut
};

// Detached islands caused by the change (each listed once, cells sorted ascending; islands
// ordered by their smallest cell).
std::vector<std::vector<i32>> detached_islands(const Lattice& L, const CutSet& cut, ConnStats* stats = nullptr);

// Conservative variant (no shortcut): every search runs to a support or exhaustion.
std::vector<std::vector<i32>> detached_islands(const Lattice& L, std::span<const i32> seeds, ConnStats* stats = nullptr);

// Pieces held only through cracked bonds (unilateral contacts): the components the searches
// from `seeds` reach over uncracked bonds without meeting a support (conservative, no shortcut).
// Such a piece still counts as connected (it stands on its cracks, detached_islands); a caller
// releases one that moves off them - a structure tipping about the crushed side of its hinge.
std::vector<std::vector<i32>> contact_held_pieces(const Lattice& L, std::span<const i32> seeds, ConnStats* stats = nullptr);

// Cut set for removing `cells` (call BEFORE removing them): the surviving neighbours.
CutSet removal_cut(const Lattice& L, std::span<const i32> cells);

// All free-cell components with no support (whole-lattice scan; for setup / tests).
std::vector<std::vector<i32>> unsupported_components(const Lattice& L);

}  // namespace svx
