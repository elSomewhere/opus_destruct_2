// structvox procgen — presets (svx/procgen/presets.hpp, docs/PRESETS.md).
#include "svx/procgen/presets.hpp"

#include <algorithm>
#include <cmath>

#define JSON_NOEXCEPTION 1
#include "json.hpp"
#include "svx/game/level.hpp"
#include "svx/procgen/city.hpp"
#include "svx/procgen/drive_city.hpp"
#include "svx/procgen/levels.hpp"
#include "svx/world/tunables.hpp"

namespace svx {

namespace {

using Json = nlohmann::json;

// (typed reads that never throw: a field of the wrong kind is an error, a missing one its default)
struct Reader {
  std::string* error;
  std::string where;
  bool ok = true;
  void fail(const std::string& field, const char* want) {
    if (ok && error) *error = where + field + ": expected " + want;
    ok = false;
  }
  f64 num(const Json& j, const char* k, f64 def) {
    if (!j.is_object() || !j.contains(k)) return def;
    const Json& v = j[k];
    if (!v.is_number()) {
      fail(k, "a number");
      return def;
    }
    const f64 x = v.get<f64>();
    if (!std::isfinite(x)) fail(k, "a finite number");
    return x;
  }
  bool boolean(const Json& j, const char* k, bool def) {
    if (!j.is_object() || !j.contains(k)) return def;
    if (!j[k].is_boolean()) {
      fail(k, "true or false");
      return def;
    }
    return j[k].get<bool>();
  }
  std::string str(const Json& j, const char* k, const std::string& def) {
    if (!j.is_object() || !j.contains(k)) return def;
    if (!j[k].is_string()) {
      fail(k, "a string");
      return def;
    }
    return j[k].get<std::string>();
  }
  const Json* object(const Json& j, const char* k) {
    if (!j.is_object() || !j.contains(k)) return nullptr;
    if (!j[k].is_object()) {
      fail(k, "an object");
      return nullptr;
    }
    return &j[k];
  }
  std::vector<std::pair<std::string, f64>> numbers(const Json& j, const char* k) {
    std::vector<std::pair<std::string, f64>> out;
    const Json* o = object(j, k);
    if (!o) return out;
    for (auto it = o->begin(); it != o->end(); ++it) {
      if (!it.value().is_number()) {
        fail(std::string(k) + "." + it.key(), "a number");
        continue;
      }
      out.emplace_back(it.key(), it.value().get<f64>());
    }
    return out;
  }
};

}  // namespace

bool parse_preset(const std::string& json, Preset* out, std::string* error) {
  const Json j = Json::parse(json, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    if (error) *error = "not a JSON object";
    return false;
  }
  Reader r{error, ""};
  Preset p;
  p.id = r.str(j, "id", "");
  r.where = p.id + ": ";
  p.label = r.str(j, "label", p.id);
  p.group = r.str(j, "group", "");
  p.description = r.str(j, "description", "");
  p.generator = r.str(j, "generator", "");
  p.level = r.str(j, "level", "");
  p.experimental = r.boolean(j, "experimental", false);
  const f64 seed = r.num(j, "seed", 1.0);
  p.seed = seed >= 0.0 && seed < 9e15 ? static_cast<u64>(seed) : 1;
  if (j.contains("params")) {
    if (!j["params"].is_object()) r.fail("params", "an object");
    else p.params_json = j["params"].dump();
  }
  if (const Json* s = r.object(j, "streaming")) {
    p.has_stream = true;
    p.stream.load_radius = r.num(*s, "load_radius", p.stream.load_radius);
    p.stream.evict_radius = r.num(*s, "evict_radius", p.stream.evict_radius);
    p.stream.chunks_per_tick = static_cast<int>(r.num(*s, "chunks_per_tick", p.stream.chunks_per_tick));
    p.stream.max_resident_mb = r.num(*s, "max_resident_mb", p.stream.max_resident_mb);
    p.stream.archive_mb = r.num(*s, "archive_mb", p.stream.archive_mb);
    p.stream.forget_after_s = r.num(*s, "forget_after_s", p.stream.forget_after_s);
  }
  if (const Json* f = r.object(j, "far")) {
    p.has_far = true;
    p.far.radius = r.num(*f, "radius", p.far.radius);
    p.far.tile = static_cast<i32>(r.num(*f, "tile", p.far.tile));
    p.far.factor = static_cast<i32>(r.num(*f, "factor", p.far.factor));
    p.far.tiles_per_tick = static_cast<int>(r.num(*f, "tiles_per_tick", p.far.tiles_per_tick));
  }
  p.tunables = r.numbers(j, "tunables");
  p.env = r.numbers(j, "env");
  if (const Json* t = r.object(j, "traffic")) {
    p.has_traffic = true;
    p.traffic.enabled = r.boolean(*t, "enabled", p.traffic.enabled);
    p.traffic.cars = static_cast<i32>(r.num(*t, "cars", p.traffic.cars));
    p.traffic.parked = static_cast<i32>(r.num(*t, "parked", p.traffic.parked));
    p.traffic.near_radius = r.num(*t, "near_radius", p.traffic.near_radius);
    p.traffic.radius = r.num(*t, "radius", p.traffic.radius);
    p.traffic.speed_scale = r.num(*t, "speed_scale", p.traffic.speed_scale);
  }
  if (const Json* q = r.object(j, "pedestrians")) {
    p.has_pedestrians = true;
    p.pedestrians.enabled = r.boolean(*q, "enabled", p.pedestrians.enabled);
    p.pedestrians.count = static_cast<i32>(r.num(*q, "count", p.pedestrians.count));
    p.pedestrians.near_radius = r.num(*q, "near_radius", p.pedestrians.near_radius);
    p.pedestrians.radius = r.num(*q, "radius", p.pedestrians.radius);
    p.pedestrians.bodies = static_cast<i32>(r.num(*q, "bodies", p.pedestrians.bodies));
    p.pedestrians.max_deep = static_cast<i32>(r.num(*q, "max_deep", p.pedestrians.max_deep));
  }
  if (j.contains("atmosphere")) {
    if (!j["atmosphere"].is_object()) r.fail("atmosphere", "an object");
    else p.atmosphere_json = j["atmosphere"].dump();
  }
  if (const Json* s = r.object(j, "spawn")) {
    const Json* pos = s->contains("pos") ? &(*s)["pos"] : nullptr;
    const Json* dir = s->contains("dir") ? &(*s)["dir"] : nullptr;
    auto vec3 = [&](const Json* a, V3* v, const char* name) {
      if (!a) return;
      if (!a->is_array() || a->size() != 3 || !(*a)[0].is_number() || !(*a)[1].is_number() || !(*a)[2].is_number()) {
        r.fail(std::string("spawn.") + name, "[x, y, z]");
        return;
      }
      *v = V3{(*a)[0].get<f64>(), (*a)[1].get<f64>(), (*a)[2].get<f64>()};
    };
    p.has_spawn = pos != nullptr;
    vec3(pos, &p.spawn_pos, "pos");
    vec3(dir, &p.spawn_dir, "dir");
  }
  if (r.ok && p.id.empty()) r.fail("id", "a string");
  if (r.ok && p.generator != "drive" && p.generator != "city1km" && p.generator != "level" && p.generator != "city")
    r.fail("generator", "drive, city1km, level or city");
  if (r.ok && p.generator == "level" && p.level.empty()) r.fail("level", "a level's kind");
  if (!r.ok) return false;
  *out = std::move(p);
  return true;
}

const std::vector<Preset>& presets() {
  static const std::vector<Preset> all = [] {
    std::vector<Preset> v;
    for (const EmbeddedPreset& e : embedded_presets()) {
      Preset p;
      std::string err;
      if (parse_preset(e.json, &p, &err)) v.push_back(std::move(p));
    }
    std::sort(v.begin(), v.end(), [](const Preset& a, const Preset& b) { return a.id < b.id; });
    return v;
  }();
  return all;
}

const Preset* find_preset(const std::string& id) {
  for (const Preset& p : presets())
    if (p.id == id) return &p;
  return nullptr;
}

const char* default_preset_id() { return "legacy/drive"; }

bool load_preset(Game& game, const Preset& p, u64 seed, f64 h, std::string* error) {
  if (seed == 0) seed = p.seed;
  if (p.generator == "drive") {
    game.load_streaming(make_drive_city(seed, h), h, p.stream, p.far);
  } else if (p.generator == "city1km") {
    game.load_streaming(make_city_source(seed, 1000.0, h, true), h, p.stream, p.far);
  } else if (p.generator == "level") {
    load_level(game, make_procedural(p.level, seed, h));
  } else {
    if (error) *error = p.id + ": the city generator is not in this build yet";
    return false;
  }
  for (const auto& [name, v] : p.tunables)
    if (!game.set_tunable(name.c_str(), v)) {
      if (error) *error = p.id + ": unknown tunable " + name;
      return false;
    }
  for (const auto& [name, v] : p.env)
    if (!game.set_env(name.c_str(), v)) {
      if (error) *error = p.id + ": unknown environment parameter " + name;
      return false;
    }
  if (p.has_traffic) game.set_traffic(p.traffic);
  if (p.has_pedestrians) game.set_pedestrians(p.pedestrians);
  if (p.has_spawn) game.set_spawn(p.spawn_pos, p.spawn_dir);
  return true;
}

}  // namespace svx
