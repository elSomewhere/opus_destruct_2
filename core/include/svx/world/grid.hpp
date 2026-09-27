// structvox — world voxel store (plan §B8): the resident representation of a whole level.
//
// One byte per voxel in 32^3 chunks (uniform chunks store a single value): 0 = air, low 7
// bits = 1 + MaterialId, bit 7 = anchored (rock / bedrock: Dirichlet, never simulated).
// A bond exists between every pair of face-adjacent solid voxels unless both are anchored
// or it is marked broken (bit per voxel and +axis, allocated lazily per chunk). Sparse
// overlays hold committed bond damage, per-brick (8^3) static baselines and persistent render
// offsets. Physics works on Lattice windows extracted from here (world/region.hpp), so the
// resident cost is ~1 B per voxel plus overlays for touched regions.
#pragma once

#include <array>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "svx/base/types.hpp"
#include "svx/mech/material.hpp"

namespace svx {

using Vox = u8;
constexpr Vox kAir = 0;
constexpr Vox kAnchorBit = 0x80;
inline Vox make_vox(MaterialId m, bool anchored) {
  return static_cast<Vox>((1 + static_cast<u8>(m)) | (anchored ? kAnchorBit : 0));
}
inline bool vox_solid(Vox v) { return v != kAir; }
inline bool vox_anchored(Vox v) { return (v & kAnchorBit) != 0; }
inline bool vox_free(Vox v) { return v != kAir && (v & kAnchorBit) == 0; }  // structure (not rock)
inline MaterialId vox_mat(Vox v) { return static_cast<MaterialId>((v & 0x7F) - 1); }

constexpr int kChunkBits = 5;
constexpr int kChunk = 1 << kChunkBits;  // 32
constexpr int kChunkVox = kChunk * kChunk * kChunk;
constexpr int kBrickBits = 3;
constexpr int kBrick = 1 << kBrickBits;  // 8

using IVec3 = std::array<i32, 3>;

inline u64 key3(i32 x, i32 y, i32 z) {
  constexpr i64 off = i64(1) << 20;
  return (u64(i64(x) + off) << 42) | (u64(i64(y) + off) << 21) | u64(i64(z) + off);
}
inline IVec3 unkey3(u64 k) {
  constexpr i64 off = i64(1) << 20;
  return {static_cast<i32>(i64((k >> 42) & 0x1FFFFF) - off), static_cast<i32>(i64((k >> 21) & 0x1FFFFF) - off),
          static_cast<i32>(i64(k & 0x1FFFFF) - off)};
}

struct Chunk {
  bool uniform = true;
  Vox value = kAir;             // uniform chunks
  std::vector<Vox> v;           // kChunkVox when mixed (index: (x * 32 + y) * 32 + z)
  std::vector<u8> broken;       // lazily allocated: bit a = bond to the +a neighbour broken
  std::vector<u8> strength;     // lazily allocated: design strength class per voxel
  u32 version = 0;              // bumped on every change (mesh invalidation)
  i32 solid = 0;                // solid voxel count (mixed chunks)
  i32 free = 0;                 // ... of them not anchored: structure, not rock (mixed chunks)
  i32 free_count() const { return uniform ? (vox_free(value) ? kChunkVox : 0) : free; }
};

struct BaselineBrick {
  std::vector<f32> u;           // 6 per voxel (8^3 * 6), metres / rad
  std::vector<u8> valid;        // per voxel
};

class VoxelGrid {
 public:
  f64 h = 0.125;
  IVec3 lo{0, 0, 0}, hi{0, 0, 0};  // loaded voxel bounds [lo, hi)

  Vox get(i32 x, i32 y, i32 z) const;
  Vox get(const IVec3& p) const { return get(p[0], p[1], p[2]); }
  void set(i32 x, i32 y, i32 z, Vox v);
  void set(const IVec3& p, Vox v) { set(p[0], p[1], p[2], v); }

  // bond between p and p + e_axis
  bool bond(const IVec3& p, int axis) const;
  bool broken(const IVec3& p, int axis) const;
  void break_bond(const IVec3& p, int axis);
  // Cracked bonds (unilateral contacts): ruptured, but the faces still touch; connected for
  // detachment, contact-only for mechanics (Lattice::crack_bond). Stored in bits 3..5 of the
  // broken byte; break_bond clears it.
  bool cracked(const IVec3& p, int axis) const;
  void crack_bond(const IVec3& p, int axis);

  u8 strength(const IVec3& p) const;
  void set_strength(const IVec3& p, u8 cls);

  f32 damage(const IVec3& p, int axis) const;
  void set_damage(const IVec3& p, int axis, f32 d);

  // baseline overlay (per 8^3 brick)
  bool baseline(const IVec3& p, f32 out[6]) const;
  void set_baseline(const IVec3& p, const f32 u[6]);
  void clear_baseline(const IVec3& p);
  // bumps the chunk's version (a change of its baselines: background jobs reading them are stale)
  void touch(const IVec3& chunk_coord);
  // persistent render offset (event-induced displacement after the bubble slept), metres
  bool offset(const IVec3& p, f32 out[3]) const;
  void set_offset(const IVec3& p, const f32 d[3]);

  // bulk construction: fill [z0, z1) of column (x, y) (no dirty marking), then compact()
  void fill_column(i32 x, i32 y, i32 z0, i32 z1, Vox v);
  void compact();
  void mark_all_dirty();

  // deterministic digest of the damage and render-offset overlays
  u64 overlay_hash() const;

  // A self-contained copy of the chunks `keys` (and their damage and baseline overlays;
  // baseline bricks are shared copy-on-write): the world a background job reads while this
  // grid keeps changing. Chunks outside `keys` are absent from the copy (partial()).
  VoxelGrid snapshot(std::span<const u64> keys) const;
  bool partial() const { return partial_; }
  // whether the chunk's content is known: always on a full grid; on a snapshot, the chunks it
  // was taken of (present or absent = air)
  bool known(const IVec3& cc) const { return !partial_ || known_.count(key3(cc[0], cc[1], cc[2])) > 0; }
  void mark_known(const IVec3& cc) {  // (a snapshot's chunk added after it was taken, e.g. hydrated)
    if (partial_) known_.insert(key3(cc[0], cc[1], cc[2]));
  }

  // chunk access
  const Chunk* chunk(const IVec3& c) const;
  const std::unordered_map<u64, Chunk>& chunks() const { return chunks_; }
  std::vector<u64> take_dirty();  // chunk keys changed since the last call (sorted)
  void mark_dirty(const IVec3& chunk_coord);

  i64 solid_count() const;
  i64 memory_bytes() const;

  // Persistence (plan §B8): chunks changed by gameplay since track_changes(true) was called,
  // serialized as a binary coordinate-keyed delta against the regenerable base world.
  void track_changes(bool on) { track_ = on; }
  bool tracking() const { return track_; }
  const std::vector<u64>& modified_chunks() const { return modified_list_; }
  bool is_modified(u64 key) const { return modified_.count(key) > 0; }
  std::vector<u8> save_delta() const;
  // Applies a delta (returns false on a malformed buffer; the grid is then unchanged). The
  // affected chunks' baselines are dropped (they are recomputed lazily).
  bool load_delta(const std::vector<u8>& bytes, std::vector<u64>* touched = nullptr);
  // Record-level access (streaming: archive evicted modified chunks, restore on reload).
  std::vector<u8> chunk_record(u64 key) const;
  bool apply_record(const std::vector<u8>& rec, u64* key_out = nullptr);
  static std::vector<u8> pack_delta(const std::vector<std::vector<u8>>& records);
  static bool unpack_delta(const std::vector<u8>& bytes, std::vector<std::pair<u64, std::vector<u8>>>* records);
  // Streaming: install a generated chunk / drop a chunk with all its overlays.
  void insert_chunk(const IVec3& cc, std::vector<Vox>&& voxels);
  void remove_chunk(const IVec3& cc);

 private:
  Chunk& chunk_mut(const IVec3& c);
  std::unordered_map<u64, Chunk> chunks_;
  std::array<std::unordered_map<u64, f32>, 3> damage_;  // per axis: voxel key -> d
  // Bricks are shared with snapshots and copied before a write while shared. Only the owning
  // (simulation) thread creates and drops snapshots, so the use counts it reads are exact.
  std::unordered_map<u64, std::shared_ptr<BaselineBrick>> baseline_;
  bool partial_ = false;
  std::unordered_set<u64> known_;  // snapshots: the chunk keys taken
  std::unordered_map<u64, std::array<f32, 3>> offset_;
  std::vector<u64> dirty_;
  bool track_ = false;
  std::unordered_map<u64, u8> modified_;
  std::vector<u64> modified_list_;
  void note_modified(const IVec3& chunk_coord) {
    if (!track_) return;
    const u64 k = key3(chunk_coord[0], chunk_coord[1], chunk_coord[2]);
    if (modified_.emplace(k, 1).second) modified_list_.push_back(k);
  }
};

inline IVec3 chunk_of(const IVec3& p) { return {p[0] >> kChunkBits, p[1] >> kChunkBits, p[2] >> kChunkBits}; }
inline int chunk_index(const IVec3& p) {
  return ((p[0] & (kChunk - 1)) * kChunk + (p[1] & (kChunk - 1))) * kChunk + (p[2] & (kChunk - 1));
}

}  // namespace svx
