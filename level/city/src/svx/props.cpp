// svx_city — the props' classes (svx/props.hpp), from data/city/props.json.
#include "svx/props.hpp"

#include <map>
#include <utility>

#include "core/value.hpp"
#include "svx/base/types.hpp"
#include "svx/data.hpp"

namespace svx::city {

namespace {

using Table = std::map<std::pair<std::string, std::string>, PropClass, std::less<>>;

Table load() {
  Table t;
  Value doc;
  std::string err;
  const char* text = city_data("props.json");
  if (!text || !Value::parse_json(text, &doc, &err)) SVX_FAIL("data/city/props.json: missing or malformed");
  for (const char* group : {"props", "furniture", "civic"}) {
    const Value& g = doc[group];
    for (const auto& [kind, v] : g.members()) {
      PropClass c;
      const std::string& a = v["attach"].str();
      c.attach = a == "loose" ? Attachment::Loose : a == "entity" ? Attachment::Entity : a == "decorative" ? Attachment::Decorative : Attachment::Fixed;
      if (a != "fixed" && a != "loose" && a != "entity" && a != "decorative") SVX_FAIL("data/city/props.json: an unknown attachment");
      for (const Value& u : v["uses"].items()) {
        const std::string& n = u.str();
        c.uses |= n == "sit" ? kUseSit : n == "sleep" ? kUseSleep : n == "work" ? kUseWork : n == "eat" ? kUseEat : n == "open" ? kUseOpen : n == "climb" ? kUseClimb : 0;
      }
      c.entity = v["entity"].str();
      if (c.attach == Attachment::Entity && c.entity.empty()) SVX_FAIL("data/city/props.json: an entity without its kind");
      t.emplace(std::make_pair(std::string(group), kind), std::move(c));
    }
  }
  return t;
}

}  // namespace

const PropClass* prop_class(std::string_view group, std::string_view kind) {
  static const Table table = load();
  const auto it = table.find(std::make_pair(std::string(group), std::string(kind)));
  return it == table.end() ? nullptr : &it->second;
}

}  // namespace svx::city
