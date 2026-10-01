// svx_city — a padded voxel chunk at some LOD and its writer in LOD 0 world voxels (voxel_city
// voxel/chunk.js).
//
// Generators never think about LOD: they write boxes / points / columns in LOD 0 world voxels and
// the writer point-samples them at the chunk's resolution (a coarse voxel takes the value of its
// representative fine voxel, the one at bx + i s + s / 2). That is what lets one generator feed
// every LOD.
//
// Layout: kP^3 (34^3) uint16 material ids, index i + j kP + k kP2, one voxel apron on every side
// so the mesher sees neighbours without touching other chunks. `data` is public: the rasterizers
// write it directly (JS: chunk.data[i + j * P + k * P2] = m), the padded index ranges they loop
// over come from range_x / range_y / range_z, and wx / wy / wz give a padded index's
// representative world coordinate.
//
// Every coordinate is a double (a JS number); the writes keep JS's semantics exactly, including
// what a Uint16Array does with a store at an index that is no integer (nothing): a non-integer
// coordinate at LOD 0, a NaN.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/math.hpp"
#include "core/placement.hpp"
#include "core/rect.hpp"

namespace svx::city {

// P, P2, P3 (core/units.js PCHUNK = CHUNK + 2)
constexpr int kP = kPChunk;
constexpr int kP2 = kP * kP;
constexpr int kP3 = kP * kP * kP;

// A padded index range [lo, hi] (inclusive). Empty when lo > hi: JS keeps the raw (unclamped,
// possibly NaN) bounds of an empty range, which nothing reads but that comparison and the loops
// that do not run; here they are lo in [0, kP], hi in [-1, kP - 1], NaN as empty.
struct IdxRange {
  int lo = 0, hi = -1;
  bool empty() const { return lo > hi; }
};

class ChunkBuffer {
 public:
  ChunkBuffer(int lod, double cx, double cy, double cz);

  int lod;
  double cx, cy, cz;
  double s;           // 1 << lod: LOD 0 voxels per voxel
  double half;        // s >> 1
  double bx, by, bz;  // LOD 0 coordinate of the start of padded voxel 0 on each axis
  std::vector<uint16_t> data;  // kP3 material ids
  int non_air = 0;             // (count_non_air's last count)
  // The structvox export (svx/source.js): `iso` records the material written while `isolating` is
  // on (props and furniture, which a physics host writes as isolated voxels after generation);
  // empty (JS: null) until track_isolated(). Off, it costs nothing.
  std::vector<uint16_t> iso;
  bool isolating = false;

  // Record isolated voxels from here on (the structvox export).
  ChunkBuffer& track_isolated() {
    iso.assign(kP3, 0);
    return *this;
  }
  // (JS: chunk.iso, null when not tracking)
  uint16_t* iso_data() { return iso.empty() ? nullptr : iso.data(); }
  const uint16_t* iso_data() const { return iso.empty() ? nullptr : iso.data(); }

  // (The export's addition, PROCGEN_MERGE_PLAN.md §7.3, §10.2: no JS twin.) Which object an
  // isolated voxel belongs to - a street prop, a piece of furniture, a civic fitting - so that the
  // export can give a loose one seams on every outer face, leave an entity out and bond a fixed one
  // (svx/props.hpp). `obj` holds 1 + the index in `objects` of the object being written while it
  // is isolating, 0 elsewhere; empty until track_objects(). An emitter brackets an object's writes
  // with begin_object / end_object (which turn `isolating` on and off as the reference does);
  // an object begun again under the same id (its boxes drawn one by one) keeps its index.
  struct ObjectRef {
    std::string group;  // "props", "furniture", "civic" (svx/props.hpp)
    std::string kind;   // its prefab's id
    std::string id;     // the instance (stable: the same object, the same id, in every chunk)
  };
  std::vector<uint32_t> obj;
  std::vector<ObjectRef> objects;
  uint32_t object = 0;  // (the one being written: 1 + its index; 0: none)
  ChunkBuffer& track_objects() {
    obj.assign(kP3, 0);
    return *this;
  }
  void begin_object(const char* group, const std::string& kind, const std::string& id) {
    isolating = true;
    if (obj.empty()) return;
    for (size_t k = 0; k < objects.size(); ++k)
      if (objects[k].id == id && objects[k].group == group) {
        object = static_cast<uint32_t>(k + 1);
        return;
      }
    objects.push_back(ObjectRef{group, kind, id});
    object = static_cast<uint32_t>(objects.size());
  }
  void end_object() {
    isolating = false;
    object = 0;
  }

  static int index(int i, int j, int k) { return i + j * kP + k * kP2; }

  // LOD 0 world bounds covered by the padded buffer (inclusive).
  Box3 world_box() const {
    const double e = kP * s - 1;
    return {bx, by, bz, bx + e, by + e, bz + e};
  }

  // Representative LOD 0 coordinate of padded index i on x / y / z.
  double wx(double i) const { return bx + i * s + half; }
  double wy(double j) const { return by + j * s + half; }
  double wz(double k) const { return bz + k * s + half; }

  // Padded index range [lo, hi] whose representatives fall in [a, b].
  IdxRange range_x(double a, double b) const { return idx_range(a, b, bx + half, s); }
  IdxRange range_y(double a, double b) const { return idx_range(a, b, by + half, s); }
  IdxRange range_z(double a, double b) const { return idx_range(a, b, bz + half, s); }

  // (i, j, k inside the padded buffer)
  uint16_t get(int i, int j, int k) const { return data[static_cast<size_t>(index(i, j, k))]; }

  // Material at a LOD 0 world coordinate (its voxel), 0 outside (JS: undefined for NaN).
  uint16_t sample_world(double x, double y, double z) const {
    const double i = std::floor((x - bx) / s);
    const double j = std::floor((y - by) / s);
    const double k = std::floor((z - bz) / s);
    if (!(i >= 0 && j >= 0 && k >= 0 && i < kP && j < kP && k < kP)) return 0;
    return data[static_cast<size_t>(i + j * kP + k * kP2)];
  }

  // The padded index a LOD 0 voxel lands on, or -1: it is no representative, lies outside, or
  // its index is no integer (a typed array ignores that store).
  int write_index(double x, double y, double z) const {
    double i = x - bx - half;
    double j = y - by - half;
    double k = z - bz - half;
    if (s > 1) {
      i /= s;
      j /= s;
      k /= s;
    }
    // (JS tests divisibility, then the bounds: either only returns, so the bounds come first
    // here, rejecting NaN and the infinities too, and the integer tests below convert safely)
    if (!(i >= 0 && j >= 0 && k >= 0 && i < kP && j < kP && k < kP)) return -1;
    if (s > 1) {
      // (i % s !== 0 for s a power of two: i / s is exact, an integer exactly when s divides i)
      const int ii = static_cast<int>(i), jj = static_cast<int>(j), kk = static_cast<int>(k);
      if (ii != i || jj != j || kk != k) return -1;
      return index(ii, jj, kk);
    }
    // (at LOD 0 an index that is no integer: the typed array ignores the store)
    const double idx = i + j * kP + k * kP2;
    const int n = static_cast<int>(idx);
    if (n != idx || n >= kP3) return -1;
    return n;
  }

  // Write a single LOD 0 voxel (only lands if it is a representative).
  void set(double x, double y, double z, uint16_t m) {
    const int idx = write_index(x, y, z);
    if (idx >= 0) data[static_cast<size_t>(idx)] = m;
  }

  // Write only into air.
  void set_if_air(double x, double y, double z, uint16_t m) {
    const int idx = write_index(x, y, z);
    if (idx >= 0 && data[static_cast<size_t>(idx)] == 0) data[static_cast<size_t>(idx)] = m;
  }

  // Fill an inclusive LOD 0 box. mode: 0 overwrite, 1 only air, 2 only solid (isolating, any
  // other mode writes nothing; not isolating, any mode but 0 and 1 is 2 - as JS's).
  void fill_box(double x0, double y0, double z0, double x1, double y1, double z1, uint16_t m, int mode = 0);

  // Does the padded buffer's world box intersect the given LOD 0 box?
  bool touches(double x0, double y0, double z0, double x1, double y1, double z1) const {
    const double e = kP * s - 1;
    return x1 >= bx && x0 <= bx + e && y1 >= by && y0 <= by + e && z1 >= bz && z0 <= bz + e;
  }
  bool touches_rect(const Rect& r, double z0, double z1) const { return touches(r.x0, r.y0, z0, r.x1, r.y1, z1); }

  // Non-air voxels of the padded buffer (kept in non_air).
  int count_non_air();

 private:
  // padded indices whose representative base + idx s lies in [a, b]
  static IdxRange idx_range(double a, double b, double base, double s) {
    double lo = std::ceil((a - base) / s);
    double hi = std::floor((b - base) / s);
    if (lo < 0) lo = 0;
    if (hi > kP - 1) hi = kP - 1;
    // (beyond JS: NaN as empty, the bounds of an empty range kept within int)
    if (!(lo <= kP)) lo = kP;
    if (!(hi >= -1)) hi = -1;
    return {static_cast<int>(lo), static_cast<int>(hi)};
  }
};

// World (LOD 0) voxel box of a chunk's core (unpadded) region.
inline Box3 chunk_core_box(int lod, double cx, double cy, double cz) {
  const double s = static_cast<double>(1 << lod);
  const double e = kChunk * s;
  return {cx * e, cy * e, cz * e, cx * e + e - 1, cy * e + e - 1, cz * e + e - 1};
}

}  // namespace svx::city
