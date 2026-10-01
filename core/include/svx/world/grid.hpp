// structvox — the voxel grid: the resident representation of a world.
//
// One byte per voxel in 32^3 chunks (uniform chunks store a single value): 0 = air, low 7
// bits = 1 + MaterialId, bit 7 = anchored (a support: bedrock, foundations, kinematic parts;
// never simulated). A bond exists between every pair of face-adjacent solid voxels unless both
// are anchored or it is marked broken (bit per voxel and +axis, allocated lazily per chunk).
// Design strength classes (the world's design pass) are a lazily allocated byte per voxel.
// Voxel p is the cube h (p - 1/2) .. h (p + 1/2) (its centre is h p, metres, in the grid's own
// frame: a world holds several grids, each placed with a frame of its own, World::add_grid).
// Where a voxel's face meets another grid's voxels, the world bonds them through samples of the
// face (junctions); the samples that broke are kept per voxel (sparse, per chunk).
#pragma once

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "svx/base/mem.hpp"
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
// A value a voxel may hold: air, or a material (anchored or not); 0x80 ("anchored air") is not.
inline bool vox_valid(Vox v) { return v == kAir || (v & 0x7F) != 0; }

constexpr int kChunkBits = 5;
constexpr int kChunk = 1 << kChunkBits;  // 32
constexpr int kChunkVox = kChunk * kChunk * kChunk;
constexpr int kBrickBits = 3;
constexpr int kBrick = 1 << kBrickBits;  // 8

using IVec3 = std::array<i32, 3>;

// Voxel coordinates a world accepts: keys pack 21 bits per axis, less a margin for the reach of
// events around a voxel (edits, loads, layers and commands beyond are refused).
constexpr i32 kVoxelLimit = (1 << 20) - 4096;
inline bool in_voxel_range(const IVec3& p) {
  auto ok = [](i32 v) { return (v < 0 ? -static_cast<i64>(v) : static_cast<i64>(v)) < kVoxelLimit; };
  return ok(p[0]) && ok(p[1]) && ok(p[2]);
}

inline u64 key3(i32 x, i32 y, i32 z) {
  constexpr i64 off = i64(1) << 20;
  return (u64(i64(x) + off) << 42) | (u64(i64(y) + off) << 21) | u64(i64(z) + off);
}
inline IVec3 unkey3(u64 k) {
  constexpr i64 off = i64(1) << 20;
  return {static_cast<i32>(i64((k >> 42) & 0x1FFFFF) - off), static_cast<i32>(i64((k >> 21) & 0x1FFFFF) - off),
          static_cast<i32>(i64(k & 0x1FFFFF) - off)};
}

// Junction samples of a voxel's face (the bonds between grids, World): face = axis * 2 + (1 for
// the +axis face), sub = the sample (0 .. kJunctionSubs - 1), or kJunctionFace for every sample
// of the face.
constexpr int kJunctionSubs = 63;
constexpr int kJunctionFace = 63;
inline u32 junction_code(int local_index, int face, int sub) {
  return (static_cast<u32>(local_index) << 9) | (static_cast<u32>(face) << 6) | static_cast<u32>(sub);
}

// Per-voxel byte channels besides the voxel itself (the world's damage, a fire's heat, water,
// ...): registered by name; a chunk allocates a layer's array when it first holds a nonzero
// value there, and drops it when the layer is all zero again.
constexpr int kMaxLayers = 8;
// What a layer's value belongs to: a solid voxel (damage, heat, char: cleared when the voxel is
// removed or replaced), an air voxel (water, gas: cleared when a solid takes its place), or the
// place (kept whatever the voxel becomes).
enum class LayerBind : u8 { Place = 0, Solid = 1, Air = 2 };
struct LayerSpec {
  std::string name;
  // Persistent: part of the chunk's changes (deltas, the change archive: it comes back with the
  // chunk). Transient: dropped with the chunk (a fire's heat).
  bool persistent = true;
  LayerBind bind = LayerBind::Place;
};

// A chunk's broken faces are kept as a sorted list while there are few (a seam under a chair, a
// crack) and as an array of a byte per voxel beyond this many voxels with one.
constexpr size_t kBrokenListMax = 2048;

struct Chunk {
  bool uniform = true;
  Vox value = kAir;             // uniform chunks
  std::vector<Vox> v;           // kChunkVox when mixed (index: (x * 32 + y) * 32 + z)
  // Broken faces (bit a of a voxel: its bond to the +a neighbour is broken), either
  std::vector<u8> broken;       // a byte per voxel (kChunkVox), or
  std::vector<u32> broken_few;  // (voxel index << 3) | bits, sorted: while there are few (kBrokenListMax)
  std::vector<u8> strength;     // lazily allocated: design strength class per voxel
  u32 vox_version = 0;          // changes (unique value) when voxels change: fragment caches
  i32 solid = 0;                // solid voxel count (mixed chunks)
  i32 free = 0;                 // ... of them not anchored: structure, not rock (mixed chunks)
  std::array<std::vector<u8>, kMaxLayers> layer;  // kChunkVox each when the layer has values here
  std::array<u16, kMaxLayers> layer_count{};      // nonzero values per layer
  std::vector<u32> jbroken;     // broken junction samples of its voxels (junction_code), sorted
  i32 free_count() const { return uniform ? (vox_free(value) ? kChunkVox : 0) : free; }
  // The broken faces of voxel i (Chunk::v order).
  u8 broken_at(i32 i) const {
    if (!broken.empty()) return broken[size_t(i)];
    if (broken_few.empty()) return 0;
    size_t lo = 0, hi = broken_few.size();
    const u32 key = static_cast<u32>(i) << 3;
    while (lo < hi) {
      const size_t m = (lo + hi) / 2;
      if (broken_few[m] < key) lo = m + 1;
      else hi = m;
    }
    return lo < broken_few.size() && (broken_few[lo] >> 3) == static_cast<u32>(i) ? static_cast<u8>(broken_few[lo] & 7) : 0;
  }
  bool any_broken() const { return !broken.empty() || !broken_few.empty(); }
  // Its broken faces as a byte per voxel (zeros where none).
  void broken_dense(std::vector<u8>& out) const {
    if (!broken.empty()) {
      out = broken;
      return;
    }
    out.assign(kChunkVox, 0);
    for (u32 e : broken_few) out[e >> 3] = static_cast<u8>(e & 7);
  }
  // Its broken faces as a sorted list ((voxel index << 3) | bits), whichever way it keeps them.
  void broken_list(std::vector<u32>& out) const {
    if (broken.empty()) {
      out = broken_few;
      return;
    }
    out.clear();
    for (i32 i = 0; i < kChunkVox; ++i)
      if (broken[size_t(i)] & 7) out.push_back((static_cast<u32>(i) << 3) | (broken[size_t(i)] & 7u));
  }
  // Its bytes (what the grid's memory_bytes counts for it): its own record - of pointer-sized
  // containers: in what is used a fixed size, about its size on a 64-bit platform - and its arrays.
  i64 memory_bytes(Bytes kind = Bytes::Held) const {
    i64 b = record_bytes<Chunk>(kind, 328) + vec_bytes(v, kind) + vec_bytes(broken, kind) + vec_bytes(broken_few, kind) + vec_bytes(strength, kind) +
            vec_bytes(jbroken, kind);
    for (const auto& l : layer) b += vec_bytes(l, kind);
    return b;
  }
  bool has_layers() const {
    for (const auto& l : layer)
      if (!l.empty()) return true;
    return false;
  }
};

// A chunk's broken faces read in ascending voxel order (a fragment's voxels, a scan): its array,
// or a cursor through its sorted list - no search per voxel.
struct BrokenCursor {
  const u8* dense = nullptr;
  const u32* few = nullptr;
  size_t n = 0, e = 0;
  explicit BrokenCursor(const Chunk& c)
      : dense(c.broken.empty() ? nullptr : c.broken.data()), few(c.broken_few.data()), n(c.broken_few.size()) {}
  // (i: not below the last call's)
  u8 at(i32 i) {
    if (dense) return dense[size_t(i)];
    const u32 key = static_cast<u32>(i) << 3;
    while (e < n && few[e] < key) ++e;
    return e < n && (few[e] >> 3) == static_cast<u32>(i) ? static_cast<u8>(few[e] & 7) : 0;
  }
};

class VoxelGrid {
 public:
  f64 h = 0.125;  // voxel size (m; a world holds its own within 1 mm .. 100 m, World::load)
  IVec3 lo{0, 0, 0}, hi{0, 0, 0};  // loaded voxel bounds [lo, hi)

  Vox get(i32 x, i32 y, i32 z) const;
  Vox get(const IVec3& p) const { return get(p[0], p[1], p[2]); }
  void set(i32 x, i32 y, i32 z, Vox v);
  void set(const IVec3& p, Vox v) { set(p[0], p[1], p[2], v); }

  // bond between p and p + e_axis
  bool bond(const IVec3& p, int axis) const;
  bool broken(const IVec3& p, int axis) const;
  void break_bond(const IVec3& p, int axis);
  // A generated chunk's seams (ChunkSource::generate_seams: bit a of a voxel, its face to the +a
  // neighbour does not bond): the chunk's base state, not a change. (The chunk is there.)
  void install_seams(const IVec3& cc, const std::vector<u8>& bits);

  u8 strength(const IVec3& p) const;  // design strength class (0: as the material)
  void set_strength(const IVec3& p, u8 cls);

  // Junctions (bonds to other grids): whether sample `sub` of face `face` of voxel p no longer
  // bonds (it, or the whole face, broke), and breaking one (sub kJunctionFace: the whole face).
  bool junction_broken(const IVec3& p, int face, int sub) const;
  void break_junction(const IVec3& p, int face, int sub);
  void clear_junction_breaks();  // (every chunk's: a grid placed anew bonds afresh)

  // Layers: add_layer returns a layer's index (the existing one for a name already added).
  int add_layer(const LayerSpec& spec);
  void sanitize();  // invalid voxel values (vox_valid) become air
  int layer_index(const std::string& name) const;  // -1: none
  const std::vector<LayerSpec>& layers() const { return layers_; }
  // Takes these layers (by name: the values of layers it has move to their index in specs; its
  // layers not in specs are dropped).
  void adopt_layers(const std::vector<LayerSpec>& specs);
  u8 layer(int L, const IVec3& p) const;
  // Returns whether the value changed. (A chunk is made, as air, for a value in a chunk that is
  // not there.)
  bool set_layer(int L, const IVec3& p, u8 v);
  std::vector<u64> take_layer_dirty(int L);  // keys of chunks whose layer L changed since the last call (sorted)

  // bulk construction: fill [z0, z1) of column (x, y) (no dirty marking), then compact()
  void fill_column(i32 x, i32 y, i32 z0, i32 z1, Vox v);
  void compact();
  void mark_all_dirty();
  // Changes whenever any of its voxels do (a cache of what it holds is stale when it differs).
  u32 revision() const { return vox_seq_; }

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
  i64 memory_bytes(Bytes kind = Bytes::Held) const;  // the chunks' voxel arrays
  i64 bookkeeping_bytes() const;             // change tracking
  i64 dirty_bytes() const;

  // Persistence: chunks changed since track_changes(true) was called, serialized as a binary
  // coordinate-keyed delta against the regenerable base world (voxels, broken bonds, strength
  // classes per chunk).
  void track_changes(bool on) { track_ = on; }
  bool tracking() const { return track_; }
  const std::vector<u64>& modified_chunks() const { return modified_list_; }
  bool is_modified(u64 key) const { return modified_.count(key) > 0; }
  // Whether its voxels or bonds changed (not only its persistent layers): what a structure's
  // design (a first touch) must not undo.
  bool voxels_modified(u64 key) const { return voxel_modified_.count(key) > 0; }
  void note_voxels_modified(u64 key) {
    if (track_) voxel_modified_.insert(key);
  }
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
  std::vector<LayerSpec> layers_;
  std::array<std::unordered_set<u64>, kMaxLayers> layer_dirty_;
  std::vector<u64> compact_;              // chunks that may have been emptied
  std::vector<std::vector<u8>> spare_;    // recycled kChunkVox arrays
  static constexpr size_t kMaxSpare = 256;
  bool track_ = false;
  std::unordered_map<u64, u32> modified_;  // key -> index in modified_list_
  std::unordered_set<u64> voxel_modified_;  // (of those: voxels or bonds changed)
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
