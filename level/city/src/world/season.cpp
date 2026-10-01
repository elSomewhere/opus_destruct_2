// svx_city — world/season.hpp (voxel_city world/season.js).
#include "world/season.hpp"

#include <cmath>
#include <utility>

#include "core/hash.hpp"
#include "core/js.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

namespace {

constexpr double kWarm = 0.64;

double smooth(double a, double b, double x) {
  const double t = js::max(0.0, js::min(1.0, (x - a) / (b - a)));
  return t * t * (3 - 2 * t);
}

double h01(double seed, double salt) { return static_cast<double>(hash32(seed, salt, 0x5ea) >> 8) / 16777216; }

// Kinds that keep their needles or leaves through the year (the season only lays snow on them).
bool evergreen(std::string_view kind) {
  static constexpr std::string_view kKinds[] = {"pine", "spruce", "dwarfpine", "juniper", "jungle", "palm",
                                                "cactus", "acacia", "shrubDry", "log", "stump", "snag"};
  for (std::string_view k : kKinds)
    if (k == kind) return true;
  return false;
}

// Autumn palettes [shadow, body, sunlit] per kind: birches and poplars go gold, oaks brown and
// orange, maples fiery, rowans red with berries.
struct AutumnEntry {
  std::string_view kind;
  LeafPalette p;
};
constexpr AutumnEntry kAutumn[] = {
    {"birch", {MAT::LEAVES_AUTUMN, MAT::LEAVES_YELLOW, MAT::LEAVES_YELLOW}},
    {"oak", {MAT::LEAVES_BROWN, MAT::LEAVES_AUTUMN, MAT::LEAVES_YELLOW}},
    {"maple", {MAT::LEAVES_RED, MAT::LEAVES_AUTUMN, MAT::LEAVES_YELLOW}},
    {"autumn", {MAT::LEAVES_RED, MAT::LEAVES_AUTUMN, MAT::LEAVES_YELLOW}},
    {"street", {MAT::LEAVES_BROWN, MAT::LEAVES_YELLOW, MAT::LEAVES_YELLOW}},
    {"blossom", {MAT::LEAVES_RED, MAT::LEAVES_RED, MAT::LEAVES_AUTUMN}},
    {"rowan", {MAT::LEAVES_RED, MAT::LEAVES_RED, MAT::LEAVES_AUTUMN}},
    {"willow", {MAT::LEAVES_BIRCH, MAT::LEAVES_YELLOW, MAT::LEAVES_YELLOW}},
    {"poplar", {MAT::LEAVES_AUTUMN, MAT::LEAVES_YELLOW, MAT::LEAVES_YELLOW}},
    {"shrub", {MAT::LEAVES_BROWN, MAT::LEAVES_RED, MAT::LEAVES_AUTUMN}},
    {"fern", {MAT::LEAVES_BROWN, MAT::LEAVES_BROWN, MAT::GRASS_STRAW}},
    {"berry", {MAT::TUNDRA_AUTUMN, MAT::LEAVES_RED, MAT::LEAVES_RED}},
    {"hazel", {MAT::LEAVES_BROWN, MAT::LEAVES_YELLOW, MAT::LEAVES_YELLOW}},
    {"aspen", {MAT::LEAVES_RED, MAT::LEAVES_AUTUMN, MAT::LEAVES_YELLOW}},
    {"alder", {MAT::LEAVES_DARK, MAT::LEAVES_BROWN, MAT::LEAVES_BROWN}},
    {"larch", {MAT::LEAVES_AUTUMN, MAT::LEAVES_YELLOW, MAT::LEAVES_YELLOW}},
};
constexpr LeafPalette kSpring = {MAT::LEAVES, MAT::LEAVES_SPRING, MAT::LEAVES_SPRING};
constexpr LeafPalette kBloom = {MAT::LEAVES_BLOSSOM, MAT::LEAVES_BLOSSOM, MAT::FLOWER_WHITE};

const LeafPalette* autumn_palette(std::string_view kind) {
  for (const AutumnEntry& e : kAutumn)
    if (e.kind == kind) return &e.p;
  return &kAutumn[1].p;  // AUTUMN.oak
}

// Seasonal ground: spring / autumn / winter replacement of a summer ground material.
using GroundPair = std::pair<int, int>;
constexpr GroundPair kSpringGround[] = {
    {MAT::GRASS, MAT::GRASS_SPRING}, {MAT::GRASS_DARK, MAT::GRASS_SPRING}, {MAT::GRASS_LAWN, MAT::GRASS_SPRING},
    {MAT::GRASS_DRY, MAT::GRASS_STRAW}, {MAT::MARSH_GRASS, MAT::MARSH_AUTUMN},
};
constexpr GroundPair kAutumnGround[] = {
    {MAT::GRASS, MAT::GRASS_AUTUMN},       {MAT::GRASS_DARK, MAT::LAWN_AUTUMN},     {MAT::GRASS_LAWN, MAT::LAWN_AUTUMN},
    {MAT::GRASS_DRY, MAT::GRASS_STRAW},    {MAT::FOREST_FLOOR, MAT::LEAF_LITTER},  {MAT::TUNDRA, MAT::TUNDRA_AUTUMN},
    {MAT::MOSS, MAT::HEATHER},             {MAT::MARSH_GRASS, MAT::MARSH_AUTUMN},  {MAT::SAVANNA_GRASS, MAT::GRASS_STRAW},
};
constexpr GroundPair kWinterGround[] = {
    {MAT::GRASS, MAT::GRASS_DEAD},         {MAT::GRASS_DARK, MAT::LAWN_WINTER},    {MAT::GRASS_LAWN, MAT::LAWN_WINTER},
    {MAT::GRASS_DRY, MAT::GRASS_STRAW},    {MAT::FOREST_FLOOR, MAT::LEAF_LITTER},  {MAT::TUNDRA, MAT::TUNDRA_AUTUMN},
    {MAT::MARSH_GRASS, MAT::MARSH_AUTUMN},
};

template <size_t N>
int lookup(const GroundPair (&t)[N], int m) {
  for (const GroundPair& p : t)
    if (p.first == m) return p.second;
  return -1;
}

}  // namespace

Season::Season(const Value& config) {
  const Value& w = config["world"];
  const Value& s = w["season"];
  id = "summer";
  if (s.is_string() && (s.str() == "spring" || s.str() == "summer" || s.str() == "autumn" || s.str() == "winter")) id = s.str();
  const Value& cl = w["climate"];  // (w.climate ?? null)
  if (!cl.is_nullish() && !cl["snowCover"].is_undefined()) fixed_snow = cl["snowCover"].to_number();
  cold = cl.truthy() ? cl["temperature"].num(0.5) + cl["temperatureVar"].num(0.05) < 0.45 : false;
  const Value& fr = cl.is_nullish() ? Value::undefined() : cl["freeze"];
  if (!fr.is_nullish())
    freeze_t = fr.to_number();
  else
    freeze_t = id == "spring" ? 0.3 : id == "summer" ? 0.18 : id == "autumn" ? 0.27 : 0.47;
  table_ = id == "spring" ? 0 : id == "autumn" ? 1 : id == "winter" ? 2 : -1;
  any = id != "summer" || (fixed_snow && *fixed_snow > 0);
}

double Season::strength(double t) const { return smooth(kWarm, kWarm - 0.1, t); }

double Season::snow(double t) const {
  if (fixed_snow) {
    const double f = *fixed_snow;
    return f > 0 && (cold || t < 0.5 + 0.1 * f) ? f : 0;
  }
  if (id == "winter") return 0.92 * smooth(0.6, 0.42, t);
  if (id == "spring") return 0.8 * smooth(0.33, 0.25, t);
  if (id == "autumn") return 0.7 * smooth(0.3, 0.22, t);
  return 0;
}

double Season::roof_snow(double t) const {
  if (fixed_snow) return *fixed_snow > 0 ? js::min(1.0, *fixed_snow) : 0;
  return snow(t);
}

std::optional<TreeLook> Season::tree_look(std::string_view kind, double seed, double t) const {
  const double sn = snow(t);
  if (evergreen(kind)) {
    if (!(sn > 0)) return std::nullopt;
    TreeLook l;
    l.snow = sn;
    return l;
  }
  const double k = strength(t);
  // (warm lands: the season does not show)
  if (k <= 0 && sn <= 0 && !fixed_snow) return std::nullopt;
  const double r = h01(seed, 1);
  TreeLook l;
  l.snow = sn;
  if (id == "winter") {
    l.bare = r < k;
    return l;
  }
  if (id == "autumn") {
    // colder = later in the autumn: more trees already bare
    l.bare = r < 0.5 * smooth(0.42, 0.28, t) * k;
    // (each tree turns at its own pace: some still green, most part-turned, some all gold)
    l.mix = k * js::min(1.0, h01(seed, 2) * 1.6);
    l.leaves = autumn_palette(kind);
    l.holes = 0.12 + 0.1 * l.mix;
    l.has_accent = true;
    l.accent = kind == "rowan" ? MAT::BERRY_RED : 0;
    return l;
  }
  if (id == "spring") {
    // late spring in the cold north: many trees still bare or just budding
    l.bare = r < 0.7 * smooth(0.37, 0.28, t) * k;
    const bool bloom = kind == "blossom" || (kind == "street" && h01(seed, 3) < 0.2) || (kind == "rowan" && h01(seed, 3) < 0.5);
    l.leaves = bloom ? &kBloom : &kSpring;
    l.mix = k * (bloom ? 1 : 0.9);
    l.holes = 0.2;
    return l;
  }
  // (an explicit climate snow cover strips summer broadleaves too)
  if (sn > 0) {
    l.bare = sn > 0.4;
    return l;
  }
  return std::nullopt;
}

int Season::ground(int m, double t, double n) const {
  if (table_ < 0) return m;
  const int to = table_ == 0 ? lookup(kSpringGround, m) : table_ == 1 ? lookup(kAutumnGround, m) : lookup(kWinterGround, m);
  if (to < 0) return m;
  const double k = strength(t);
  if (n > k) return m;
  // autumn heather only in patches of the moss
  if (to == MAT::HEATHER && n > k * 0.3) return m;
  // spring: some old straw left in the fresh grass, fresh shoots in the dry
  if (id == "spring" && m == MAT::GRASS && n > k * 0.82) return MAT::GRASS_STRAW;
  if (id == "spring" && m == MAT::GRASS_DRY && n < k * 0.5) return MAT::GRASS_SPRING;
  return to;
}

int Season::flower(int m, double t, double cl, double h, double sp) const {
  const double k = strength(t) + (t > kWarm ? 0.4 : 0);
  if (id == "summer") {
    if (m == MAT::MOSS || m == MAT::TUNDRA) return cl > 0.62 && h < 0.35 ? MAT::HEATHER_BLOOM : 0;
    if (m != MAT::GRASS && m != MAT::GRASS_DRY && m != MAT::MARSH_GRASS) return 0;
    if (cl < 0.55 || h > 0.1 + 0.3 * (cl - 0.55)) return 0;
    const double s = sp * 5;
    return s < 1.4 ? MAT::FLOWER_LUPIN : s < 2.5 ? MAT::FLOWER_YELLOW : s < 3.6 ? MAT::FLOWER_WHITE : s < 4.4 ? MAT::FLOWER_PINK : MAT::FLOWER_PURPLE;
  }
  if (id == "spring") {
    if (k <= 0) return 0;
    if (m == MAT::FOREST_FLOOR || m == MAT::LEAF_LITTER) return cl > 0.5 && h < 0.18 ? MAT::FLOWER_WHITE : 0;
    if (m == MAT::GRASS_SPRING || m == MAT::GRASS) return cl > 0.62 && h < 0.08 ? (sp < 0.6 ? MAT::FLOWER_YELLOW : MAT::FLOWER_WHITE) : 0;
  }
  return 0;
}

int Season::canopy(bool conifer, double tl, double k) const {
  if (conifer) return k < 0.55 ? MAT::LEAVES_SPRUCE : MAT::LEAVES_PINE;
  const double s = strength(tl);
  const double q = std::fmod(k * 7.31, 1);
  if (id == "autumn") {
    if (q < s) return q < s * 0.4 ? MAT::LEAVES_YELLOW : q < s * 0.65 ? MAT::LEAVES_AUTUMN : q < s * 0.82 ? MAT::LEAVES_RED : MAT::LEAVES_BROWN;
  } else if (id == "winter") {
    if (q < s) return MAT::TWIGS;
  } else if (id == "spring") {
    if (q < s) return q < s * 0.6 ? MAT::LEAVES_SPRING : MAT::LEAVES_LIGHT;
  }
  return k < 0.3 ? MAT::LEAVES_LIGHT : MAT::LEAVES_DARK;
}

double patch_noise(double seed, double x, double y, double cell, double salt) {
  const double gx = std::floor(x / cell);
  const double gy = std::floor(y / cell);
  double fx = x / cell - gx;
  double fy = y / cell - gy;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  const double a = hash32(seed, gx, gy, salt) / 4294967296.0;
  const double b = hash32(seed, gx + 1, gy, salt) / 4294967296.0;
  const double c = hash32(seed, gx, gy + 1, salt) / 4294967296.0;
  const double d = hash32(seed, gx + 1, gy + 1, salt) / 4294967296.0;
  return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

}  // namespace svx::city
