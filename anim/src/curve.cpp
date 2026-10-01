#include "svx/anim/curve.hpp"

#include <algorithm>

namespace svx::anim {

namespace {

f64 ease(Ease e, f64 u) {
  switch (e) {
    case Ease::In:
      return u * u * u;
    case Ease::Out:
      return 1.0 - (1.0 - u) * (1.0 - u) * (1.0 - u);
    case Ease::InOut:
      return u * u * (3.0 - 2.0 * u);
    case Ease::Snap: {
      const f64 a = 1.0 - u;
      return 1.0 - a * a * a * a;
    }
    case Ease::Hold:
      return u >= 1.0 ? 1.0 : 0.0;
    default:
      return u;
  }
}

}  // namespace

Track::Track(std::vector<Key> keys) {
  if (keys.empty()) return;
  std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
  dim = static_cast<i32>(keys.front().v.size());
  for (const Key& k : keys) {
    t_.push_back(k.t);
    e_.push_back(k.e);
    for (i32 d = 0; d < dim; ++d) v_.push_back(d < static_cast<i32>(k.v.size()) ? k.v[size_t(d)] : 0.0);
  }
}

void Track::sample(f64 time, f64* out) const {
  const size_t n = t_.size();
  const size_t D = static_cast<size_t>(dim);
  if (n == 0) return;
  if (n == 1 || time <= t_[0]) {
    for (size_t d = 0; d < D; ++d) out[d] = v_[d];
    return;
  }
  if (time >= t_[n - 1]) {
    for (size_t d = 0; d < D; ++d) out[d] = v_[(n - 1) * D + d];
    return;
  }
  size_t i = 0;
  while (i < n - 2 && time >= t_[i + 1]) ++i;
  const f64 t0 = t_[i], t1 = t_[i + 1];
  const f64 h = t1 - t0;
  const f64 u = h > 0.0 ? (time - t0) / h : 1.0;
  const Ease e = e_[i + 1];
  if (e != Ease::Smooth) {
    const f64 w = ease(e, u);
    for (size_t d = 0; d < D; ++d) out[d] = v_[i * D + d] + (v_[(i + 1) * D + d] - v_[i * D + d]) * w;
    return;
  }
  // cubic Hermite, Catmull-Rom tangents for uneven spacing (zero at the ends)
  const f64 u2 = u * u, u3 = u2 * u;
  const f64 h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
  for (size_t d = 0; d < D; ++d) {
    const f64 p0 = v_[i * D + d], p1 = v_[(i + 1) * D + d];
    f64 m0 = 0.0, m1 = 0.0;
    if (i > 0) m0 = ((p1 - v_[(i - 1) * D + d]) / (t1 - t_[i - 1])) * h;
    if (i + 2 < n) m1 = ((v_[(i + 2) * D + d] - p0) / (t_[i + 2] - t0)) * h;
    out[d] = h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
  }
}

}  // namespace svx::anim
