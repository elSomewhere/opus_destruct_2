// svx_anim — keyframe tracks: timed keys of numbers or vectors, sampled with smooth (cubic Hermite
// with Catmull-Rom tangents, zero at the ends: eases in and out), linear, eased or held segments.
// They drive the channels of actions (motion/actions.hpp): hand and foot targets, strike reach,
// trunk offsets, weights.
#pragma once

#include <initializer_list>
#include <vector>

#include "svx/anim/math.hpp"

namespace svx::anim {

// How the segment arriving at a key is interpolated.
enum class Ease : u8 {
  Smooth,  // C1 cubic through the keys (default)
  Linear,
  In,      // slow start, fast arrival (a wind-up into a strike)
  Out,     // fast start, slow arrival (recovery, settling)
  InOut,
  Snap,    // explosive: most of the motion in the first third (strikes, flinches)
  Hold,    // keeps the previous value until the key
};

struct Key {
  f64 t = 0.0;
  std::vector<f64> v;  // (1 value, or a vector)
  Ease e = Ease::Smooth;
  Key(f64 t_, f64 v_, Ease e_ = Ease::Smooth) : t(t_), v{v_}, e(e_) {}
  Key(f64 t_, std::initializer_list<f64> v_, Ease e_ = Ease::Smooth) : t(t_), v(v_), e(e_) {}
  Key(f64 t_, std::vector<f64> v_, Ease e_ = Ease::Smooth) : t(t_), v(std::move(v_)), e(e_) {}
};

class Track {
 public:
  Track() = default;
  explicit Track(std::vector<Key> keys);
  i32 dim = 0;
  f64 start() const { return t_.empty() ? 0.0 : t_.front(); }
  f64 end() const { return t_.empty() ? 0.0 : t_.back(); }
  bool empty() const { return t_.empty(); }
  // The value at `time` (clamped to the keys) into out[0..dim).
  void sample(f64 time, f64* out) const;

 private:
  std::vector<f64> t_, v_;
  std::vector<Ease> e_;
};

}  // namespace svx::anim
