#include "svx/anim/characters/props.hpp"

#include <string>

#include "svx/anim/content.hpp"
#include "svx/anim/content_data.hpp"

namespace svx::anim {
bool Prop::has(std::string_view tag) const { return std::find(tags.begin(), tags.end(), tag) != tags.end(); }
bool Prop::satisfies(const std::vector<std::string>& req) const {
  for (const auto& tag : req)
    if (!has(tag)) return false;
  return true;
}
const PropSocket* Prop::socket(std::string_view id) const {
  for (const auto& s : sockets)
    if (s.id == id) return &s;
  return nullptr;
}
const ContactFeature* Prop::feature(std::string_view id) const {
  for (const auto& f : features)
    if (f.id == id) return &f;
  return nullptr;
}

// The library's catalogue: content data (data/content/props.json, embedded; authored by
// tools/prop_catalog), read once.
const std::vector<PropPtr>& prop_catalog() {
  static const std::vector<PropPtr> all = [] {
    const char* text = content_data("props.json");
    return text ? read_props(text) : std::vector<PropPtr>{};
  }();
  return all;
}
PropPtr prop_archetype(std::string_view id) {
  for (const auto& p : prop_catalog())
    if (p->id == id) return p;
  return {};
}
PropPtr legacy_prop(i32 index) {
  const auto& all = prop_catalog();
  return index > 0 && index <= 5 ? all[size_t(index - 1)] : PropPtr{};
}
}  // namespace svx::anim
