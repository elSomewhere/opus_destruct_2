// structvox core — the world's tunables by name: every field of WorldConfig and WorldParams,
// for hosts, settings UIs, scripting and command logs (docs/CORE.md §3).
//
// An index is stable within a build (logs made by one build replay on it). Setup tunables are
// meant for before load() (fragmentation, the tick length): changed later, they apply from then
// on. Values are clamped where a field needs it (counts, flags).
#pragma once

#include "svx/world/world.hpp"

namespace svx {

struct TunableInfo {
  const char* name;
  bool setup;  // (a setup knob: meant for before load())
};

i32 tunable_count();
const TunableInfo* tunable(i32 index);  // nullptr: out of range
i32 tunable_index(const char* name);    // -1: unknown
bool set_tunable(World& w, i32 index, f64 value);  // false: unknown, or not finite
f64 get_tunable(const World& w, i32 index);        // NaN: unknown
inline bool set_tunable(World& w, const char* name, f64 value) { return set_tunable(w, tunable_index(name), value); }
inline f64 get_tunable(const World& w, const char* name) { return get_tunable(w, tunable_index(name)); }

}  // namespace svx
