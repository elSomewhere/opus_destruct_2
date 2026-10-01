// svx_city — voxel_city city/flavors.js.
#include "city/flavors.hpp"

#include <cmath>

#include "buildings/archetype_registry.hpp"
#include "buildings/styles.hpp"

namespace svx::city {

const Registry<Flavor>& flavor_registry() { return flavor_registry_mut(); }

Registry<Flavor>& flavor_registry_mut() {
  static Registry<Flavor> r("flavor");
  return r;
}

namespace {

using W = WeightedIds;

template <class T>
const T* find_key(const ByDistrict<T>& m, const std::string& key) {
  for (const auto& kv : m)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

// (obj[key] = v: an existing key keeps its place)
template <class T>
void set_key(ByDistrict<T>& m, const std::string& key, T v) {
  for (auto& kv : m)
    if (kv.first == key) {
      kv.second = std::move(v);
      return;
    }
  m.emplace_back(key, std::move(v));
}

// f.ind ?? 1
double ind_or_1(const FlavorPlace& f) { return js::is_undefined(f.ind) ? 1 : f.ind; }

}  // namespace

void register_flavors() {
  Registry<Flavor>& R = flavor_registry_mut();
  // Modern city: a gridded downtown of towers round a small historic core (irregular cobbled
  // blocks), mixed quarters laid out organically, grids that jog here and there.
  R.add({.id = "modern",
         .weight = 5,
         .floor_scale = 1,
         .old_core = 0.1,
         .patterns = {{"mixed", "organic"}},
         .pitched = {{"mixed", 0.2}, {"residential", 0.35}},
         .styles = {}});
  // Historic city: a big old core, the whole town grown organically, pitched roofs.
  R.add({.id = "historic",
         .weight = 2,
         .floor_scale = 0.75,
         .old_core = 0.22,
         .adaptive = true,
         .pitched = {{"mixed", 0.6}, {"midtown", 0.4}, {"residential", 0.65}},
         .styles = {{"downtown", W{{"deco", 4}, {"brick", 3}, {"concrete", 1}}},
                    {"midtown", W{{"brick", 4}, {"deco", 2}, {"plaster", 2}}},
                    {"mixed", W{{"brick", 5}, {"plaster", 3}}},
                    {"residential", W{{"brick", 4}, {"plaster", 4}}}}});
  R.add({.id = "futuristic",
         .weight = 1,
         .floor_scale = 1.35,
         .styles = {{"downtown", W{{"futurist", 5}, {"glass", 3}}},
                    {"midtown", W{{"futurist", 4}, {"glass", 3}, {"concrete", 1}}},
                    {"mixed", W{{"futurist", 3}, {"concrete", 2}, {"plaster", 1}}},
                    {"residential", W{{"futurist", 2}, {"plaster", 2}, {"concrete", 1}}},
                    {"suburban", W{{"futurist", 2}, {"siding", 1}}},
                    {"industrial", W{{"futurist", 1}, {"industrial", 2}}}}});
  // Soviet town: a Stalinist centre on wide avenues, everything else microdistricts of panel slabs
  // and towers; no skyscrapers. (Orthodox churches: onion domes - gilded, green, blue)
  R.add({.id = "soviet",
         .weight = 2,
         .floor_scale = 0.6,
         .church_dome = {"GOLD", "DOME_GREEN", "DOME_BLUE", "GOLD"},
         .styles = {{"downtown", W{{"stalinist", 5}, {"panel", 2}, {"concrete", 1}}},
                    {"midtown", W{{"stalinist", 3}, {"panel", 2}}},
                    {"industrial", W{{"industrial", 3}, {"concrete", 1}}}},
         .districts = {{"midtown", "microdistrict"}, {"mixed", "microdistrict"}, {"residential", "microdistrict"}, {"suburban", "microdistrict"}}});
  // (a medina: the old core grows organically)
  R.add({.id = "desert",
         .weight = 0,
         .floor_scale = 0.8,
         .when = [](const FlavorSettlement& s) { return s.t > 0.66 && s.m < 0.36; },
         .old_core = 0.18,
         .styles = {{"oldcore", W{{"adobe", 5}, {"plaster", 2}}},
                    {"downtown", W{{"adobe", 3}, {"concrete", 2}, {"glass", 1}}},
                    {"midtown", W{{"adobe", 4}, {"plaster", 2}, {"concrete", 1}}},
                    {"mixed", W{{"adobe", 5}, {"plaster", 2}}},
                    {"residential", W{{"adobe", 5}, {"plaster", 2}}},
                    {"suburban", W{{"adobe", 3}, {"plaster", 2}}}}});
  R.add({.id = "nordic",
         .weight = 0,
         .floor_scale = 0.85,
         .when = [](const FlavorSettlement& s) { return s.t < 0.3; },
         .old_core = 0.14,
         .church_style = "nordicChurch",
         .pitched = {{"mixed", 0.5}, {"residential", 0.8}},
         .styles = {{"oldcore", W{{"nordicPlaster", 4}, {"nordicWood", 3}, {"brick", 2}}},
                    {"downtown", W{{"brick", 3}, {"glass", 2}, {"concrete", 2}}},
                    {"midtown", W{{"brick", 4}, {"plaster", 2}}},
                    {"mixed", W{{"brick", 3}, {"siding", 2}, {"plaster", 2}}},
                    {"residential", W{{"siding", 4}, {"brick", 2}}},
                    {"suburban", W{{"siding", 5}, {"suburbanBrick", 1}}}}});
  // Harbour town of a temperate island: a low old centre of rendered and brick houses, the usual
  // residential rings, a small harbour.
  R.add({.id = "harbourTown",
         .weight = 0,
         .floor_scale = 1,
         .old_core = 0.3,
         .adaptive = true,
         .pitched = {{"downtown", 0.4}, {"midtown", 0.5}, {"mixed", 0.6}, {"residential", 0.8}},
         .styles = {{"downtown", W{{"plaster", 4}, {"brick", 3}, {"nordicPlaster", 2}}},
                    {"midtown", W{{"plaster", 4}, {"brick", 3}}},
                    {"mixed", W{{"plaster", 4}, {"brick", 3}, {"siding", 1}}},
                    {"residential", W{{"siding", 3}, {"plaster", 2}, {"brick", 2}}}},
         .archetypes = {{"downtown", W{{"midrise", 3}, {"walkup", 4}, {"office", 1}}}, {"midtown", W{{"walkup", 5}, {"midrise", 2}, {"rowhouse", 1}}}},
         .floors = {{"downtown", {3, 6}}, {"midtown", {3, 5}}, {"mixed", {2, 4}}, {"residential", {2, 3}}},
         .districts = {{"port", "harbour"}}});
  // Bleak northern harbour town (Norway, Karelia, the White Sea coast): an old centre of wooden and
  // rendered town houses behind the harbour, a ring of post-war Stalinist and brick blocks, then
  // wide streets through panel-block microdistricts in some quarters and wooden houses in others,
  // an industrial quarter with its housing projects, a small harbour. Its districts are a
  // function of the district picked from the macro fields and the place.
  R.add({.id = "nordicBleak",
         .weight = 0,
         .floor_scale = 1,
         .adaptive = true,
         .collectors = false,
         .main_road_wobble = 0.12,
         // cobbled lanes in the wooden old centre, broad Soviet streets through the estates
         .cobble_within = 0.3,
         .church_style = "nordicChurch",
         // Karelian wooden churches: onion domes of aspen shingle (now and then a green one)
         .church_dome = {"DOME_SHINGLE", "DOME_SHINGLE", "DOME_GREEN"},
         .pitched = {{"oldtown", 1}, {"mixed", 0.35}, {"residential", 0.9}},
         .styles = {{"mixed", W{{"stalinist", 3}, {"nordicPlaster", 3}, {"brick", 1}, {"panel", 1}}},
                    {"residential", W{{"nordicWood", 6}, {"siding", 1}}},
                    {"suburban", W{{"nordicWood", 6}, {"siding", 1}}},
                    {"industrial", W{{"industrial", 4}, {"concrete", 2}}},
                    {"heavyIndustry", W{{"industrial", 4}, {"concrete", 3}}},
                    {"projects", W{{"panel", 3}, {"projects", 2}}},
                    {"village", W{{"nordicWood", 6}, {"siding", 1}}}},
         .archetypes = {{"mixed", W{{"walkup", 5}, {"midrise", 2}, {"townhouse", 1}}},
                        {"residential", W{{"house", 6}, {"townhouse", 1}, {"rowhouse", 1}}},
                        {"suburban", W{{"house", 8}}}},
         .floors = {{"mixed", {3, 5}}, {"residential", {1, 2}}, {"microdistrict", {5, 9}}, {"projects", {5, 12}}},
         .block_use = {{"microdistrict", W{{"micro", 0.8}, {"school", 0.07}, {"garages", 0.07}, {"park", 0.03}, {"wasteland", 0.03}}},
                       {"projects", W{{"micro", 0.75}, {"garages", 0.12}, {"wasteland", 0.08}, {"parking", 0.05}}},
                       {"industrial", W{{"lots", 0.86}, {"wasteland", 0.08}, {"garages", 0.06}}},
                       {"mixed", W{{"lots", 0.9}, {"square", 0.03}, {"park", 0.04}, {"school", 0.03}}}},
         .district_fn = [](const std::string& id, const FlavorPlace& f) -> std::string {
           if (id == "port") return "harbour";
           // the works (a sawmill, a fish plant) with bleak blocks of flats round them
           if (id == "industrial" && ind_or_1(f) < 0.42) return "projects";
           if (id == "industrial" || id == "heavyIndustry" || id == "projects" || id == "park" || id == "rural") return id;
           if (f.d < 0.36) return "oldtown";
           if (id == "downtown" || id == "midtown") return "mixed";
           if (f.d < 0.62) return id == "mixed" || f.dn > 0.12 ? "mixed" : f.dn < -0.05 ? "microdistrict" : "residential";
           return f.dn < -0.12 ? "microdistrict" : id == "mixed" ? "residential" : id;
         }});
  // Norwegian harbour town (Bergen, Alesund, Stavanger, the Lofoten towns): a big old town of
  // wooden houses on cobbled streets and lanes round the harbour, a ring of rendered stone merchant
  // houses and blocks from the turn of the century, wooden villas beyond; a small works with a few
  // bleak blocks of flats; a harbour of quays, warehouses and fish sheds. (narrow main streets
  // bending through the town - cobbled in the old town -, no collector grid, irregular blocks well
  // beyond the old centre)
  R.add({.id = "nordicHarbour",
         .weight = 0,
         .floor_scale = 1,
         .adaptive = true,
         .collectors = false,
         .main_road = "village",
         .main_road_wobble = 0.22,
         .cobble_within = 0.42,
         .church_style = "nordicChurch",
         .pitched = {{"oldtown", 1}, {"mixed", 0.85}, {"residential", 0.9}, {"harbour", 0.4}},
         .styles = {{"oldtown", W{{"nordicWood", 7}, {"nordicPlaster", 2}}},
                    {"mixed", W{{"nordicPlaster", 4}, {"nordicWood", 3}, {"brick", 1}, {"plaster", 1}}},
                    {"residential", W{{"nordicWood", 8}, {"siding", 1}}},
                    {"suburban", W{{"nordicWood", 8}}},
                    {"projects", W{{"projects", 2}, {"panel", 1}, {"concrete", 1}}},
                    {"industrial", W{{"industrial", 3}, {"nordicWood", 1}}},
                    {"harbour", W{{"nordicWood", 3}, {"industrial", 2}}},
                    {"village", W{{"nordicWood", 6}, {"siding", 1}}}},
         .archetypes = {{"mixed", W{{"townhouse", 4}, {"walkup", 3}, {"midrise", 0.4}}},
                        {"residential", W{{"house", 7}, {"townhouse", 2}, {"rowhouse", 0.5}}},
                        {"suburban", W{{"house", 8}}}},
         .floors = {{"mixed", {2, 4}}, {"residential", {1, 2}}, {"projects", {4, 8}}},
         .block_use = {{"harbour", W{{"lots", 0.62}, {"quay", 0.22}, {"parking", 0.1}, {"containerYard", 0.06}}},
                       {"mixed", W{{"lots", 0.9}, {"square", 0.03}, {"park", 0.04}, {"garden", 0.03}}},
                       {"projects", W{{"micro", 0.8}, {"garages", 0.1}, {"wasteland", 0.05}, {"sports", 0.05}}}},
         .district_fn = [](const std::string& id, const FlavorPlace& f) -> std::string {
           if (id == "port") return "harbour";
           if (id == "heavyIndustry") return "industrial";
           // a small works: only the heart of the industrial quarter; round it a few bleak blocks
           // of flats, then ordinary housing
           if (id == "industrial" && ind_or_1(f) < 0.45) return f.d > 0.55 ? "projects" : "mixed";
           if (id == "projects" && ind_or_1(f) < 0.28) return f.d < 0.66 ? "mixed" : "residential";
           if (id == "industrial" || id == "projects" || id == "park" || id == "rural") return id;
           if (id == "microdistrict") return "residential";
           if (f.d < 0.46) return "oldtown";
           if (f.d < 0.66) return f.dn > -0.08 || id == "downtown" || id == "midtown" ? "mixed" : "oldtown";
           return id == "downtown" || id == "midtown" || (id == "mixed" && f.dn > 0.15) ? "mixed" : "residential";
         }});
}

namespace {

// Villages keep their region's look (desert adobe, nordic siding ...) at village height.
Flavor make_village(const Flavor& f) {
  Flavor v = f;
  v.id = f.id + "Village";
  v.floor_scale = js::min(f.floor_scale, 0.45);
  // { mixed: [...], ...f.styles, downtown: undefined }
  ByDistrict<std::optional<WeightedIds>> styles;
  styles.emplace_back("mixed", W{{"plaster", 3}, {"brick", 3}, {"siding", 1}});
  for (const auto& kv : f.styles) set_key(styles, kv.first, kv.second);
  set_key(styles, std::string("downtown"), std::optional<WeightedIds>());
  v.styles = std::move(styles);
  return v;
}

const Flavor& base_flavor(const FlavorSettlement& s) {
  const Registry<Flavor>& F = flavor_registry();
  // a flavor set by the world (an island's town) wins
  if (!s.flavor.empty() && F.has(s.flavor)) return F.get(s.flavor);
  if (s.origin_cell && !s.island) return F.get("modern");
  for (const Flavor& f : F.all())
    if (f.when && !js::is_undefined(s.t) && f.when(s)) return f;
  std::vector<const Flavor*> all;
  for (const Flavor& f : F.all())
    if (f.weight > 0) all.push_back(&f);
  double total = 0;
  for (const Flavor* f : all) total = total + f->weight;
  double r = std::fmod(static_cast<double>(js::to_uint32(s.style)), 10000) / 10000 * total;
  for (const Flavor* f : all) {
    if (r < f->weight) return *f;
    r -= f->weight;
  }
  return *all[0];
}

// Districts an old core never replaces.
bool keep_in_core(const std::string& id) {
  return id == "port" || id == "park" || id == "rural" || id == "industrial" || id == "heavyIndustry" || id == "oldtown";
}

}  // namespace

const Flavor& village_flavor(const Flavor& f) {
  return f.village.get([&] { return make_village(f); });
}

const Flavor& flavor_of(const FlavorSettlement* settlement) {
  if (!settlement) return flavor_registry().get("modern");
  if (settlement->village) return village_flavor(base_flavor(*settlement));
  return base_flavor(*settlement);
}

District flavored_district(const District& district, const Flavor& flavor) {
  const std::optional<WeightedIds>* st = find_key(flavor.styles, district.id);
  const WeightedIds* styles = st && *st ? &**st : nullptr;
  const WeightedIds* archetypes = find_key(flavor.archetypes, district.id);
  const std::array<double, 2>* floors = find_key(flavor.floors, district.id);
  const WeightedIds* block_use = find_key(flavor.block_use, district.id);
  const double* pitched = find_key(flavor.pitched, district.id);
  const bool has_pitched = pitched && js::truthy(*pitched);
  auto complete = [](const WeightedIds& list, auto& reg) {
    for (const auto& e : list)
      if (!reg.has(e.first)) return false;
    return true;
  };
  if (!styles && !archetypes && !floors && !block_use && !has_pitched && flavor.floor_scale == 1 && complete(district.styles, style_registry()) &&
      complete(district.archetypes, archetype_registry()))
    return district;
  const std::array<double, 2>& f = floors ? *floors : district.floors;
  // the flavor's list (its registered entries), else the district's own
  auto pick = [](const WeightedIds* own, const WeightedIds& base, auto& reg) {
    WeightedIds ok;
    if (own)
      for (const auto& e : *own)
        if (reg.has(e.first)) ok.push_back(e);
    if (!ok.empty()) return ok;
    for (const auto& e : base)
      if (reg.has(e.first)) ok.push_back(e);
    return ok;
  };
  District out = district;
  out.styles = pick(styles, district.styles, style_registry());
  out.archetypes = pick(archetypes, district.archetypes, archetype_registry());
  out.floors = {f[0], js::max(f[0], js::round(f[1] * flavor.floor_scale))};
  if (block_use) out.block_use = *block_use;
  if (has_pitched) out.pitched = *pitched;
  return out;
}

std::string flavored_district_id(const std::string& district_id, const FlavorSettlement* settlement, const FlavorPlace* ctx) {
  if (!settlement) return district_id;
  const Flavor& flavor = flavor_of(settlement);
  if (settlement->village) return flavor.district_fn && district_id == "port" ? flavor.district_fn(district_id, ctx ? *ctx : FlavorPlace{}) : district_id;
  // the historic core round the centre (old_core: its radius as a share of the town's)
  if (flavor.old_core && js::truthy(*flavor.old_core) && ctx && ctx->d < *flavor.old_core && !keep_in_core(district_id) && district_registry().has("oldcore"))
    return "oldcore";
  if (flavor.district_fn) {
    FlavorPlace def;
    def.d = 1;
    def.dn = 0;
    return flavor.district_fn(district_id, ctx ? *ctx : def);
  }
  const std::string* mapped = find_key(flavor.districts, district_id);
  return mapped ? *mapped : district_id;
}

}  // namespace svx::city
