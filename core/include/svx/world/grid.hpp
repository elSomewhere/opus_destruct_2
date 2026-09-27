// structvox — the voxel grid: the resident representation of a world.
//
// One byte per voxel in 32^3 chunks (uniform chunks store a single value): 0 = air, low 7
// bits = 1 + MaterialId, bit 7 = anchored (a support: bedrock, foundations, kinematic parts;
// never simulated). A bond exists between every pair of face-adjacent solid voxels unless both
// are anchored or it is marked broken (bit per voxel and +axis, allocated lazily per chunk).
// Design strength classes (the world's design pass) are a lazily allocated byte per voxel.
// Voxel p is the cube h (p - 1/2) .. h (p + 1/2) (its centre is h p, metres).
#pragma once

#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "svx/base/types.hpp"
#include "svx/material/material.hpp"

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
  u32 vox_version = 0;          // changes (unique value) when voxels change: fragment caches
  i32 solid = 0;                // solid voxel count (mixed chunks)
  i32 free = 0;                 // ... of them not anchored: structure, not rock (mixed chunks)
  i32 free_count() const { return uniform ? (vox_free(value) ? kChunkVox : 0) : free; }
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

  u8 strength(const IVec3& p) const;  // design strength class (0: as the material)
  void set_strength(const IVec3& p, u8 cls);

  // bulk construction: fill [z0, z1) of column (x, y) (no dirty marking), then compact()
  void fill_column(i32 x, i32 y, i32 z0, i32 z1, Vox v);
  void compact();
  void mark_all_dirty();

  // chunk access
  const Chunk* chunk(const IVec3& c) const;
  const std::unordered_map<u64, Chunk>& chunks() const { return chunks_; }
  // Keys of chunks whose voxels changed since the last call (sorted, each once). Bounded: if far
  // more piled up than there are chunks (nobody takes them), every chunk is reported instead.
  std::vector<u64> take_dirty();
  void mark_dirty(const IVec3& chunk_coord);
  // Chunks emptied since the last call become uniform air (their arrays are recycled).
  void compact_changed();
  // Recycled 32^3 arrays (a bounded pool: streaming and destruction do not churn the allocator).
  std::vector<u8> acquire_buffer(u8 fill);
  void release_buffer(std::vector<u8>&& b);

  i64 solid_count() const;
  i64 memory_bytes() const;                  // the chunks' voxel arrays
  i64 bookkeeping_bytes() const;             // change tracking
  i64 dirty_bytes() const;

  // Persistence: chunks changed since track_changes(true) was called, serialized as a binary
  // coordinate-keyed delta against the regenerable base world (voxels, broken bonds, strength
  // classes per chunk).
  void track_changes(bool on) { track_ = on; }
  bool tracking() const { return track_; }
  const std::vector<u64>& modified_chunks() const { return modified_list_; }
  bool is_modified(u64 key) const { return modified_.count(key) > 0; }
  std::vector<u8> save_delta() const;
  // Applies a delta (returns false on a malformed buffer; the grid is then unchanged).
  bool load_delta(const std::vector<u8>& bytes, std::vector<u64>* touched = nullptr);
  // Record-level access (streaming: archive evicted modified chunks, restore on reload).
  std::vector<u8> chunk_record(u64 key) const;
  bool apply_record(const std::vector<u8>& rec, u64* key_out = nullptr);
  static bool check_record(const std::vector<u8>& rec);  // (well-formed: apply_record will succeed)
  static std::vector<u8> pack_delta(const std::vector<std::vector<u8>>& records);
  static bool unpack_delta(const std::vector<u8>& bytes, std::vector<std::pair<u64, std::vector<u8>>>* records);
  // Streaming: install a generated chunk / drop a chunk with all its overlays.
  void insert_chunk(const IVec3& cc, std::vector<Vox>&& voxels);
  void remove_chunk(const IVec3& cc);

 private:
  Chunk& chunk_mut(const IVec3& c);
  std::unordered_map<u64, Chunk> chunks_;
  u32 vox_seq_ = 0;  // source of Chunk::vox_version (unique over the grid's life)
  std::unordered_set<u64> dirty_;
  u64 last_dirty_ = ~0ull;  // (the key added last: the common repeat is skipped cheaply)
  bool dirty_all_ = false;  // (overflow: every chunk is reported)
  void touch_dirty(u64 k);
  std::vector<u64> compact_;              // chunks that may have been emptied
  std::vector<std::vector<u8>> spare_;    // recycled kChunkVox arrays
  static constexpr size_t kMaxSpare = 256;
  bool track_ = false;
  std::unordered_map<u64, u32> modified_;  // key -> index in modified_list_
  std::vector<u64> modified_list_;
  void note_modified(const IVec3& chunk_coord) {
    if (!track_) return;
    const u64 k = key3(chunk_coord[0], chunk_coord[1], chunk_coord[2]);
    if (modified_.emplace(k, static_cast<u32>(modified_list_.size())).second) modified_list_.push_back(k);
  }
  void forget_modified(u64 k);
};

inline IVec3 chunk_of(const IVec3& p) { return {p[0] >> kChunkBits, p[1] >> kChunkBits, p[2] >> kChunkBits}; }
inline int chunk_index(const IVec3& p) {
  return ((p[0] & (kChunk - 1)) * kChunk + (p[1] & (kChunk - 1))) * kChunk + (p[2] & (kChunk - 1));
}

}  // namespace svx
