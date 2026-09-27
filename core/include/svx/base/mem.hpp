// structvox — memory accounting helpers (approximate: container capacities and node overheads).
#pragma once

#include <vector>

#include "svx/base/types.hpp"

namespace svx {

template <class T>
inline i64 vec_bytes(const std::vector<T>& v) {
  return static_cast<i64>(v.capacity() * sizeof(T));
}

// An unordered map / set: its nodes (value and two pointers) and its bucket array.
template <class M>
inline i64 hash_bytes(const M& m) {
  return static_cast<i64>(m.size() * (sizeof(typename M::value_type) + 2 * sizeof(void*)) + m.bucket_count() * sizeof(void*));
}

}  // namespace svx
