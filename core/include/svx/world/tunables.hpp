// structvox core — the world's tunables by name: every field of WorldConfig and WorldParams,
// for hosts, settings UIs, scripting and command logs (docs/CORE.md §3).
//
// An index is stable within a build; an id (a 32-bit hash of the name) across builds - a command
// log records a tunable by its id, so a log replays on a build that has more tunables, or has
// them in another order. Setup tunables are meant for before load() (fragmentation, the tick
// length): changed later, they apply from then on. Values are clamped where a field needs it
// (counts, flags).
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
u32 tunable_id(i32 index);              // (FNV-1a of its name; 0: out of range)
i32 tunable_by_id(u32 id);              // -1: unknown
bool set_tunable(World& w, i32 index, f64 value);  // false: unknown, or not finite
f64 get_tunable(const World& w, i32 index);        // NaN: unknown
// Every NaN knob takes its default (World::configure applies it: a comparison with NaN is
// always false, so a NaN would quietly disable what the knob decides).
void default_nan_tunables(WorldConfig& c);
inline bool set_tunable(World& w, const char* name, f64 value) { return set_tunable(w, tunable_index(name), value); }
inline f64 get_tunable(const World& w, const char* name) { return get_tunable(w, tunable_index(name)); }

}  // namespace svx
