// structvox — memory accounting helpers (approximate: container capacities and node overheads).
#pragma once

#include <vector>

#include "svx/base/types.hpp"

namespace svx {

// What a measure counts. Held: what a container holds (its capacity, its hash buckets): the
// process's memory, for reports. Used: what it uses (its elements, and no pointer-sized
// bookkeeping) - the same on every platform, compiler and standard library, so decisions taken by
// it (the memory budgets) are the same everywhere (docs/CORE.md: determinism).
enum class Bytes : u8 { Held, Used };

// A record's own bytes: what it takes here (Held), or a fixed size (Used: about what it takes on a
// 64-bit platform, the same on every platform).
template <class T>
inline i64 record_bytes(Bytes kind, i64 used) {
  return kind == Bytes::Held ? static_cast<i64>(sizeof(T)) : used;
}

template <class T>
inline i64 vec_bytes(const std::vector<T>& v, Bytes kind = Bytes::Held) {
  return static_cast<i64>((kind == Bytes::Held ? v.capacity() : v.size()) * sizeof(T));
}

// An unordered map / set: its nodes (value and two pointers) and its bucket array (Held); its
// values alone (Used).
template <class M>
inline i64 hash_bytes(const M& m, Bytes kind = Bytes::Held) {
  if (kind == Bytes::Used) return static_cast<i64>(m.size() * sizeof(typename M::value_type));
  return static_cast<i64>(m.size() * (sizeof(typename M::value_type) + 2 * sizeof(void*)) + m.bucket_count() * sizeof(void*));
}

}  // namespace svx
