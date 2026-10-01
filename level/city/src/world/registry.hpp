// svx_city — named registries (voxel_city world/registry.js): the generator's extension points
// (districts, archetypes, styles, prefabs, biomes, landforms, flavors, sites ...). Entries keep
// their registration order (JS's Map: registering an id again replaces its entry in place), which
// weighted picks iterate; so registries are filled by explicit functions in the reference's module
// evaluation order (city::register_all), never by static initialisers. Filled once, before any
// generation; read-only (and so safe from any thread) after.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <cstdio>

#include "svx/base/types.hpp"

namespace svx::city {

template <class T>
class Registry {
 public:
  explicit Registry(const char* kind) : kind_(kind) {}
  // register(def): T has a std::string `id`.
  void add(T def) {
    if (def.id.empty()) SVX_FAIL("registry: an entry needs an id");
    auto it = index_.find(def.id);
    if (it != index_.end()) {
      entries_[it->second] = std::move(def);
      return;
    }
    index_.emplace(def.id, entries_.size());
    entries_.push_back(std::move(def));
  }
  // get(id): the entry (an unknown id is a programming error).
  const T& get(const std::string& id) const {
    auto it = index_.find(id);
    if (it == index_.end()) {
      std::fprintf(stderr, "svx_city: %s: unknown \"%s\"\n", kind_, id.c_str());
      SVX_FAIL("registry: unknown id");
    }
    return entries_[it->second];
  }
  // maybe(id): the entry or null.
  const T* maybe(const std::string& id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &entries_[it->second];
  }
  bool has(const std::string& id) const { return index_.count(id) > 0; }
  // all(): in registration order.
  const std::vector<T>& all() const { return entries_; }
  size_t size() const { return entries_.size(); }

 private:
  const char* kind_;
  std::vector<T> entries_;
  std::unordered_map<std::string, size_t> index_;
};

}  // namespace svx::city
