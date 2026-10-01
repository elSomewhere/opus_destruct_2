// svx_city tests — the configuration and the presets (voxel_city config/*.js) against the
// reference (stage "config" of tools/procgen_ref).
#include <doctest.h>

#include "config/defaults.hpp"
#include "config/presets.hpp"
#include "records.hpp"

using namespace svx::city;

TEST_CASE("city config: presets resolve as the reference's (stage config)") {
  const std::vector<std::string> seasons = {"", "spring", "summer", "autumn", "winter"};
  std::string text;
  auto put = [&](const std::string& s) {
    text += s;
    text += '\n';
  };
  put(default_config().json());
  put(make_config(Value::object()).json());
  for (const Preset& p : presets()) {
    std::vector<std::string> sizes = {""};
    for (const Value& s : p.sizes.items()) sizes.push_back(s["id"].str());
    for (const std::string& size : sizes)
      for (const std::string& season : seasons) {
        const Value o = preset_config(p.id, size, season == "autumn" ? 42 : js::kNaN, season);
        const Value c = make_config(o);
        put(p.id + " " + size + " " + season + " " + o.json());
        put(c.json());
        put(make_config(c).json());
        put(preset_viewer(p.id, season).json());
      }
  }
  CHECK(rec::record("config", text) == rec::recorded_digest("config"));
}

TEST_CASE("city config: values read as JS reads them") {
  const Value c = make_config(preset_config("nordicTown", "skerry"));
  CHECK(c["world"]["mode"].str() == "island");
  CHECK(c["world"]["island"]["radius"].to_number() == 1900);
  CHECK(c["world"]["nothing"]["deeper"].is_undefined());
  CHECK(c["world"]["nothing"].num(7) == 7);
  CHECK(c["terrain"]["rugged"].to_number() == 0.15);
  CHECK(c["world"]["climate"]["moisture"].to_number() == 0.66);
}
