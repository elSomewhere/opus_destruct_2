// svx_city — voxel_city buildings/civic.js.
#include "buildings/civic.hpp"

#include <optional>

#include "buildings/archetype_registry.hpp"
#include "buildings/styles.hpp"
#include "core/math.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

namespace {

std::vector<CivicSpec> make_civic() {
  return {
      {.id = "supermarket", .w = {28, 56}, .d = {26, 42}, .floors = {1, 1}, .story = {5.5}, .set_f = {16, 28}, .set_s = {3, 6}, .lot = {52, 64},
       .front = "shop", .sign = "fascia", .parking = true},
      {.id = "departmentStore", .w = {30, 52}, .d = {26, 40}, .floors = {3, 5}, .story = {4.5, 4}, .set_f = {0, 1}, .set_s = {0, 0}, .lot = {44, 36},
       .front = "shop", .sign = "fascia"},
      {.id = "petrolStation", .w = {10, 15}, .d = {8, 11}, .floors = {1, 1}, .story = {3.5}, .set_f = {15, 19}, .set_s = {4, 8}, .lot = {34, 34},
       .front = "shop", .sign = "fascia", .canopy = true},
      {.id = "marketHall", .w = {26, 44}, .d = {22, 34}, .floors = {1, 1}, .story = {7}, .set_f = {2, 5}, .set_s = {1, 3}, .lot = {44, 38},
       .front = "shop", .sign = "plate", .roof = "gable"},
      {.id = "concertHall", .w = {30, 46}, .d = {34, 48}, .floors = {2, 2}, .story = {4.75, 4.25}, .set_f = {6, 9}, .set_s = {2, 4}, .lot = {50, 56},
       .sign = "marquee", .portico = true},
      {.id = "houseOfCulture", .w = {34, 52}, .d = {34, 46}, .floors = {2, 2}, .story = {4.75, 4.25}, .set_f = {7, 10}, .set_s = {3, 5}, .lot = {56, 56},
       .sign = "plate", .portico = true},
      {.id = "musicClub", .w = {15, 22}, .d = {20, 30}, .floors = {1, 1}, .story = {5}, .set_f = {0, 2}, .set_s = {0, 1}, .lot = {22, 32}, .sign = "neon"},
      {.id = "cinema", .w = {26, 40}, .d = {28, 40}, .floors = {2, 2}, .story = {4.25, 4}, .set_f = {2, 4}, .set_s = {1, 2}, .lot = {42, 44},
       .front = "shop", .sign = "marquee"},
      {.id = "hospital", .w = {44, 76}, .d = {22, 30}, .floors = {3, 6}, .story = {4, 3.75}, .set_f = {10, 16}, .set_s = {3, 6}, .lot = {80, 48},
       .sign = "cross"},
      {.id = "polyclinic", .w = {40, 64}, .d = {20, 26}, .floors = {3, 5}, .story = {3.75, 3.5}, .set_f = {8, 12}, .set_s = {3, 6}, .lot = {68, 40},
       .sign = "cross"},
      {.id = "policeStation", .w = {28, 40}, .d = {17, 22}, .floors = {2, 3}, .story = {4.25, 3.5}, .set_f = {4, 7}, .set_s = {1, 3}, .lot = {44, 30},
       .sign = "police"},
      {.id = "fireStation", .w = {30, 40}, .d = {17, 22}, .floors = {2, 2}, .story = {5, 3.25}, .set_f = {9, 12}, .set_s = {1, 3}, .lot = {44, 36},
       .sign = "fire"},
      {.id = "museum", .w = {38, 64}, .d = {24, 32}, .floors = {2, 3}, .story = {5, 5}, .set_f = {7, 10}, .set_s = {3, 6}, .lot = {66, 44},
       .sign = "banners", .portico = true},
      {.id = "artGallery", .w = {28, 50}, .d = {22, 30}, .floors = {2, 3}, .story = {5, 4.5}, .set_f = {3, 7}, .set_s = {2, 4}, .lot = {52, 38},
       .front = "shop", .sign = "banners"},
      {.id = "library", .w = {28, 46}, .d = {20, 26}, .floors = {2, 3}, .story = {4.25, 4}, .set_f = {3, 6}, .set_s = {2, 4}, .lot = {48, 34},
       .sign = "plate"},
      {.id = "townHall", .w = {30, 50}, .d = {18, 26}, .floors = {2, 4}, .story = {4.5, 4.25}, .set_f = {5, 8}, .set_s = {2, 4}, .lot = {52, 36},
       .sign = "plate", .portico = true},
      {.id = "hotel", .w = {24, 44}, .d = {16, 22}, .floors = {3, 7}, .story = {4.25, 3}, .set_f = {0, 3}, .set_s = {0, 2}, .lot = {44, 26},
       .sign = "plate"},
  };
}

// Facade styles of the civic buildings by flavor group (first match wins; unregistered styles are
// skipped): STYLE_TABLE[group][id].
struct StyleRow {
  const char* group;
  const char* id;
  std::vector<const char*> styles;
};
const std::vector<StyleRow>& style_table() {
  static const std::vector<StyleRow> t = {
      {"default", "supermarket", {"concrete", "industrial", "glass"}},
      {"default", "departmentStore", {"deco", "glass", "concrete"}},
      {"default", "petrolStation", {"concrete"}},
      {"default", "marketHall", {"brick", "classical"}},
      {"default", "concertHall", {"classical", "glass", "deco"}},
      {"default", "houseOfCulture", {"classical"}},
      {"default", "musicClub", {"brick", "concrete", "industrial"}},
      {"default", "cinema", {"deco", "concrete", "brick"}},
      {"default", "hospital", {"concrete", "plaster"}},
      {"default", "polyclinic", {"concrete"}},
      {"default", "policeStation", {"brick", "concrete"}},
      {"default", "fireStation", {"brick"}},
      {"default", "museum", {"classical", "deco"}},
      {"default", "artGallery", {"glass", "concrete"}},
      {"default", "library", {"brick", "classical", "glass"}},
      {"default", "townHall", {"classical", "brick"}},
      {"default", "hotel", {"plaster", "deco", "brick"}},
      {"nordic", "supermarket", {"concrete", "nordicWood"}},
      {"nordic", "departmentStore", {"nordicPlaster", "brick"}},
      {"nordic", "marketHall", {"nordicWood"}},
      {"nordic", "concertHall", {"nordicWood", "glass"}},
      {"nordic", "musicClub", {"nordicWood", "brick"}},
      {"nordic", "cinema", {"nordicPlaster"}},
      {"nordic", "hospital", {"concrete", "nordicPlaster"}},
      {"nordic", "policeStation", {"brick", "nordicPlaster"}},
      {"nordic", "fireStation", {"nordicWood", "brick"}},
      {"nordic", "museum", {"nordicPlaster", "classical"}},
      {"nordic", "artGallery", {"nordicWood", "glass"}},
      {"nordic", "library", {"nordicPlaster", "nordicWood"}},
      {"nordic", "townHall", {"nordicPlaster"}},
      {"nordic", "hotel", {"nordicWood", "nordicPlaster"}},
      {"soviet", "supermarket", {"concrete", "panel"}},
      {"soviet", "departmentStore", {"stalinist", "concrete"}},
      {"soviet", "houseOfCulture", {"stalinist"}},
      {"soviet", "concertHall", {"stalinist"}},
      {"soviet", "cinema", {"stalinist", "concrete"}},
      {"soviet", "polyclinic", {"panel", "concrete"}},
      {"soviet", "hospital", {"panel", "concrete"}},
      {"soviet", "policeStation", {"panel", "stalinist"}},
      {"soviet", "fireStation", {"brick", "stalinist"}},
      {"soviet", "museum", {"stalinist", "classical"}},
      {"soviet", "library", {"stalinist"}},
      {"soviet", "townHall", {"stalinist"}},
      {"soviet", "hotel", {"panel", "stalinist"}},
      {"soviet", "musicClub", {"concrete", "brick"}},
  };
  return t;
}
const std::vector<const char*>* style_list(std::string_view group, std::string_view id) {
  for (const StyleRow& r : style_table())
    if (group == r.group && id == r.id) return &r.styles;
  return nullptr;
}

std::optional<EnvSpec> civic_envelope(const ArchetypeCtx& ctx, const CivicSpec& spec) {
  Rng& rng = *ctx.rng;
  const double U = ctx.frame->U;
  const double V = ctx.frame->V;
  const double set_s = vx(rng.float_(spec.set_s[0], spec.set_s[1]));
  const double w = js::min(U - 2 * set_s, vx(rng.float_(spec.w[0], spec.w[1])));
  const double set_f = js::min(vx(rng.float_(spec.set_f[0], spec.set_f[1])), V - vx(spec.d[0]) - vx(1));
  const double d = js::min(V - set_f - vx(1), vx(rng.float_(spec.d[0], spec.d[1])));
  if (w < vx(spec.w[0]) - 8 || d < vx(spec.d[0]) - 8 || set_f < vx(spec.set_f[0]) - 8) return std::nullopt;
  const double u0 = js::round((U - w) / 2);
  const double floors = rng.int_(spec.floors[0], spec.floors[1]);
  std::vector<double> story_h;
  for (double f = 0; f < floors; f += 1) story_h.push_back(vx(spec.story[static_cast<size_t>(js::min(f, static_cast<double>(spec.story.size()) - 1))]));
  const Rect rect{u0, set_f, u0 + w - 1, set_f + d - 1};
  std::vector<EnvAnnex> annexes;
  if (spec.canopy) {
    // the petrol canopy over the pumps in front of the shop
    const double cw = js::min(U - vx(2), w + vx(rng.float_(6, 12)));
    const double cu0 = js::round((U - cw) / 2);
    annexes.push_back({"canopy", Rect{cu0, vx(2.5), cu0 + cw - 1, set_f - vx(2)}, vx(5)});
  }
  if (spec.parking || spec.canopy) {
    // a price / logo pylon at the lot's street corner
    const double pu = rng.chance(0.5) ? vx(1.5) : U - vx(2.5);
    annexes.push_back({"pylon", Rect{pu, vx(1), pu + 7, vx(1) + 3}, vx(spec.canopy ? 7 : 9)});
  }
  bool pitched = spec.roof == "gable";
  if (!pitched && js::truthy(ctx.pitched_civic)) pitched = rng.chance(ctx.pitched_civic) && floors <= 4 && !spec.parking && !spec.canopy;
  EnvRoof roof;
  if (pitched) {
    roof.type = spec.roof == "gable" ? "gable" : rng.chance(0.5) ? "hip" : "gable";
    roof.ridge = "u";
    roof.slope = spec.roof == "gable" ? 0.55 : rng.float_(0.55, 0.8);
    roof.overhang_kind = EnvRoof::Overhang::Number;
    roof.overhang = 3;
  } else {
    roof.type = "flat";
  }
  static const uint16_t kSignColors[] = {MAT::SIGNAGE_RED, MAT::SIGNAGE_BLUE, MAT::SIGNAGE, MAT::SIGN_GREEN, MAT::NEON_RED};
  const uint16_t sign_color = rng.pick(kSignColors);
  EnvSpec env;
  env.tiers.push_back(EnvTier{0, floors - 1, {rect}});
  env.annexes = std::move(annexes);
  env.floors = floors;
  env.story_h = std::move(story_h);
  env.basements = 0;
  env.roof = roof;
  env.program.ground = spec.id;  // (upper: null)
  env.entrance_side = "F";
  CivicExtra x;
  x.civic = spec.id;
  x.storefront = spec.front == "shop";
  x.sign = spec.sign;
  x.sign_color = sign_color;
  x.portico = spec.portico && set_f >= vx(4.5) && w >= vx(20);
  x.parking = spec.parking;
  // the building's distance from the lot front (voxels): room for a portico, pumps, a porch
  x.set_f = set_f;
  env.extra = x;
  return env;
}

}  // namespace

const std::vector<CivicSpec>& civic_table() {
  static const std::vector<CivicSpec> t = make_civic();
  return t;
}

const CivicSpec* civic_spec(std::string_view id) {
  for (const CivicSpec& s : civic_table())
    if (s.id == id) return &s;
  return nullptr;
}

std::string civic_group(std::string_view flavor_id) {
  // String(flavorId ?? "").replace(/Village$/, "")
  std::string_view f = flavor_id;
  constexpr std::string_view kVillage = "Village";
  if (f.size() >= kVillage.size() && f.substr(f.size() - kVillage.size()) == kVillage) f = f.substr(0, f.size() - kVillage.size());
  // FLAVOR_GROUP
  if (f == "nordic" || f == "nordicHarbour" || f == "harbourTown") return "nordic";
  if (f == "nordicBleak" || f == "soviet") return "soviet";
  return "default";
}

std::string civic_style(std::string_view id, std::string_view flavor_id, Rng& rng) {
  const std::string group = civic_group(flavor_id);
  const std::vector<const char*>* list = style_list(group, id);
  if (!list) list = style_list("default", id);
  std::vector<std::string> ok;
  if (list) {
    for (const char* s : *list)
      if (style_registry().has(s)) ok.push_back(s);
  } else if (style_registry().has("concrete")) {
    ok.push_back("concrete");
  }
  return ok.empty() ? std::string("concrete") : rng.pick(ok);
}

void register_civic() {
  for (const CivicSpec& spec : civic_table()) {
    Archetype a;
    a.id = spec.id;
    a.label = spec.id;
    a.civic = true;
    a.fits = [spec](double U, double V) { return U >= vx(spec.w[0] + 2 * spec.set_s[0]) && V >= vx(spec.d[0] + spec.set_f[0] + 1); };
    a.envelope = [spec](const ArchetypeCtx& ctx) { return civic_envelope(ctx, spec); };
    archetype_registry_mut().add(std::move(a));
  }
  // Classical stone front of museums, town halls and concert halls: limestone, tall windows, a cornice.
  using namespace MAT;
  style_registry_mut().add({.id = "classical",
                            .walls = {LIMESTONE, TRIM_STONE, PLASTER_CREAM, PLASTER_WHITE},
                            .base = {GRANITE, GRANITE_LIGHT},
                            .trim = {PLASTER_WHITE, TRIM_STONE, CORNICE},
                            .glass = {GLASS},
                            .frame = {FRAME_WHITE, FRAME_DARK},
                            .window = {.type = "punched", .width = 1.375, .sill = 1.0, .head = 3.125, .bay = 3.5, .lintel = true, .casing = true},
                            .cornice = true,
                            .roof = {ROOF_MEMBRANE},
                            .pitched = {ROOF_METAL_GREEN, ROOF_SLATE},
                            .fire_escape = 0,
                            .balcony = 0,
                            .water_tank = 0,
                            .awning = 0,
                            .storefront = {FRAME_DARK}});
}

}  // namespace svx::city
