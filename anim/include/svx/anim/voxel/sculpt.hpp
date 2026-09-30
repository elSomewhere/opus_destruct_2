// svx_anim — building voxel models from signed-distance shapes. A Sculptor holds a dense lattice
// over the model's box; shapes are added (owned by a bone, in a palette slot), painted over, or
// carved away, in order, at voxel centres. `finish` splits the lattice into one part per bone,
// turns the inside of organic shapes into flesh (and bone around the limb axes, for gore), and
// copies the cells of joint balls into every bone that shares the joint, so limbs rotating about
// a joint overlap instead of opening gaps.
//
// Distances are taken as the TypeScript original takes them, to the last bit (its Math.hypot
// included), so a look sculpted here is the original's, voxel for voxel.
#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "svx/anim/voxel/model.hpp"

namespace svx::anim {

using Sdf = std::function<f64(f64, f64, f64)>;

// A signed-distance shape (negative inside) with a conservative bounding box.
struct Shape {
  Sdf sdf;
  V3 min, max;
};

// ---- primitives

Shape sphere(const V3& c, f64 r);
// An ellipsoid with semi-axes r (approximate distance, exact sign).
Shape ellipsoid(const V3& c, const V3& r);
// A capsule from a (radius ra) to b (radius rb): a cone with round ends.
Shape capsule(const V3& a, const V3& b, f64 ra, f64 rb);
inline Shape capsule(const V3& a, const V3& b, f64 r) { return capsule(a, b, r, r); }
// A box with half extents h about c, edges rounded by `round` (inside the extents), optionally
// turned by q about c.
Shape box(const V3& c, const V3& h, f64 round = 0.0);
Shape box(const V3& c, const V3& h, f64 round, const Quat& q);
// A flat-capped cylinder of radius r from a to b.
Shape cylinder(const V3& a, const V3& b, f64 r);

// ---- combinators

// The points of s on the side of the plane (through p, normal n) that n points to.
Shape clip(const Shape& s, const V3& p, const V3& n);
Shape intersect(const Shape& a, const Shape& b);
Shape subtract(const Shape& a, const Shape& b);
// Smooth union (polynomial smooth minimum over `k` metres): organic blends of shapes.
Shape smooth_union(std::vector<Shape> shapes, f64 k);
// A hollow shell of thickness t on the inside of s.
Shape shell(const Shape& s, f64 t);
// s grown by d (negative shrinks).
Shape inflate(const Shape& s, f64 d);
// The mirror image across x = 0.
Shape mirror_x(const Shape& s);

// ---- sculptor

struct AddOptions {
  bool organic = true;  // the inside turns to flesh (and bone) in finish
  f64 shade = 1.0;      // brightness multiplier of the voxels
  f64 jitter = 0.035;   // per-voxel brightness jitter amplitude
};

struct PaintOptions {
  std::vector<u8> only{};    // only recolour cells in these slots (empty: any)
  std::vector<i32> bones{};  // only recolour cells owned by these bones (empty: any)
  f64 shade = 1.0;
  f64 jitter = 0.035;
};

struct FinishOptions {
  i32 flesh_depth = 2;      // organic cells at least this many cells deep (6-connected) become flesh
  f64 bone_radius = 0.018;  // flesh cells this close (m) to their bone's axis become bone
  std::string name = "model";
};

// A procedural recolouring: the voxel centre (m), its slot and its lattice coordinates -> a new
// slot, or -1 to keep it.
using PaintFn = std::function<i32(f64 x, f64 y, f64 z, u8 slot, i32 i, i32 j, i32 k)>;

class Sculptor {
 public:
  // A lattice of pitch s over the box (min, max) (m); `seed` jitters the shades.
  Sculptor(SkeletonPtr sk, f64 s, const V3& min, const V3& max, i32 seed = 1);
  SkeletonPtr skeleton;
  f64 s = 0.0;                     // voxel pitch (m)
  std::array<i32, 3> lo{0, 0, 0};  // lattice coordinates of cell (0, 0, 0)
  std::array<i32, 3> dims{0, 0, 0};
  std::vector<u8> cells;  // slot + 1 per cell (0: empty); x fastest, then y, then z
  std::vector<u8> bone;   // the bone owning each cell
  std::vector<u8> shade;
  std::vector<u8> organic;

  // Fills the shape's voxels, owned by `bone`, in `slot` (over whatever was there).
  Sculptor& add(const Shape& shape, i32 bone, u8 slot, const AddOptions& opts = {});
  // Recolours the existing voxels inside the shape.
  Sculptor& paint(const Shape& shape, u8 slot, const PaintOptions& opts = {});
  // Procedural recolouring of every voxel (PaintFn).
  Sculptor& paint_fn(const PaintFn& fn, f64 shade = 1.0, f64 jitter = 0.035);
  // Removes the voxels inside the shape.
  Sculptor& carve(const Shape& shape);
  // A joint ball: at finish, the solid voxels inside it are copied into the parts of all the given
  // bones (so the limbs meeting there overlap when they rotate about it).
  Sculptor& joint(const V3& c, f64 r, std::vector<i32> bones);
  ModelPtr finish(const FinishOptions& opts = {});

 private:
  struct Joint {
    V3 c;
    f64 r = 0.0;
    std::vector<i32> bones;
  };
  std::vector<Joint> joints_;
  i32 seed_ = 1;
  // fn(idx, i, j, k) for the cells (lattice i, j, k) whose centres are inside the shape, z-major;
  // `solid_only` skips empty cells before taking the distance (the same cells for an fn that
  // ignores empty ones: distances have no side effects).
  template <class Fn>
  void each(const Shape& shape, bool solid_only, Fn&& fn) const;
  u8 shade_of(i32 i, i32 j, i32 k, f64 shade, f64 jitter) const;
};

}  // namespace svx::anim
