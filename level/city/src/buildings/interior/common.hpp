// svx_city — helpers shared by the floor planners (voxel_city buildings/interior/common.js).
#pragma once

#include <array>
#include <memory>
#include <vector>

#include "buildings/interior/grid.hpp"
#include "buildings/interior/stairs.hpp"
#include "core/hash.hpp"
#include "core/rect.hpp"

namespace svx::city {

// Paints the stairwell room of `stair` on a floor grid ("stair", its stair's id, gray paint,
// concrete stair floor).
std::shared_ptr<Room> add_stair_room(FloorGrid& grid, const Stair& stair);

// A door from a circulation room (null: the outside) into a stairwell: it must land on the near
// landing; tries the landing's walls that touch `circ`, from opts.width [8] down to 6 cells. Reads
// opts.width, opts.kind ["stair"] and opts.leaf [stair.open ? "none" : "metal"]. Null when no wall
// borders the landing.
std::shared_ptr<Door> stair_door(FloorGrid& grid, const Room& stair_room, const Stair& stair, const Room* circ, const DoorOpts& opts = {});

// Splits a length a0..a1 into pieces of about `target` (at least min_w) separated by 1-cell
// walls: [a, b] inclusive, the last ending at a1. Draws from rng.
std::vector<std::array<double, 2>> split_length(double a0, double a1, double target, double min_w, Rng& rng, double jitter = 0.25);

// The sides of a rect that lie on the exterior wall of the grid, in the order found (N, S, W, E).
std::vector<char> facade_sides_of(const FloorGrid& grid, const Rect& r);

bool rects_overlap_any(const Rect& r, const std::vector<Rect>& list);

// Cuts an interval [a0, a1] by blocked intervals, keeping gaps of 1 wall cell.
std::vector<std::array<double, 2>> free_intervals(double a0, double a1, const std::vector<std::array<double, 2>>& blocked);

}  // namespace svx::city
