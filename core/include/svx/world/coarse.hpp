// structvox — coarse connectivity of chunks that are not resident (plan §B4 / §B8: the coarse
// hierarchy stays resident for evicted chunks, so load paths cross the streaming boundary).
//
// A streamed world holds fine voxels only around the viewer. A detachment search that walks a
// structure out of the resident chunks continues on summaries of the others: the connected
// components (intact or cracked bonds) of a chunk's non-anchored voxels, whether each touches
// an anchored voxel inside the chunk, and the component (or anchor) of every voxel on the
// chunk's six faces. Components are exact connectivity, so the search decides what a fine
// search of the whole world would, at 6 x 32^2 labels per chunk instead of 32^3 voxels.
// A summary is a pure function of the chunk's content (a streamed chunk: its source chunk
// plus its archived edits), computed when a search first needs it.
#pragma once

#include <array>
#include <vector>

#include "svx/world/grid.hpp"

namespace svx {

struct ChunkSummary {
  static constexpr u16 kAnchor = 0xFFFF;
  // per component c (face labels hold 1 + c)
  std::vector<u8> anchored;  // bonded to an anchored voxel inside the chunk
  std::vector<u32> voxels;   // voxel count
  // Face f = 2 * axis + side (side 0: local coordinate 0 along the axis, 1: kChunk - 1); one
  // label per face voxel at u * kChunk + v, u and v the local coordinates along the other two
  // axes in cyclic order (axis + 1, axis + 2): 0 = no connection across the face (air; on a
  // side-1 face also a broken bond to the neighbour, whose bit this chunk holds), kAnchor =
  // an anchored voxel, else 1 + component. An all-zero face is stored empty.
  std::array<std::vector<u16>, 6> face;
  u16 label(int f, int u, int v) const { return face[f].empty() ? 0 : face[f][size_t(u * kChunk + v)]; }
  i32 components() const { return static_cast<i32>(voxels.size()); }
  size_t bytes() const;
};

// The summary of chunk `cc` of `g` (only that chunk is read: a scratch grid holding just the
// chunk will do).
ChunkSummary summarize_chunk(const VoxelGrid& g, const IVec3& cc);

// What a connectivity search needs to know about the chunks a grid does not hold.
class CoarseWorld {
 public:
  virtual ~CoarseWorld() = default;
  // whether the grid holds the chunk's content (an absent resident chunk is air)
  virtual bool resident(const IVec3& cc) const = 0;
  // The summary of a non-resident chunk, or nullptr when it cannot be had: the search then
  // takes reaching the chunk as reaching a support (conservative: never a false detachment).
  // Summaries stay valid while the world does not change (a search holds several at once).
  virtual const ChunkSummary* summary(const IVec3& cc) = 0;
};

}  // namespace svx
