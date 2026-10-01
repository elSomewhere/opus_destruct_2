// svx_city — voxel_city svx/materials.js.
#include "svx/materials.hpp"

#include <cstdio>
#include <string>

#include "core/js.hpp"
#include "svx/base/types.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

namespace {

// ---- the rules' regular expressions: literals, groups of alternatives, ^ and $ (all the rules
// use), matched as RegExp.prototype.test matches them (a match starting anywhere)

struct ReNode;
using ReSeq = std::vector<ReNode>;
using ReAlt = std::vector<ReSeq>;
struct ReNode {
  enum Kind : uint8_t { Lit, Begin, End, Group };
  Kind kind = Lit;
  char c = 0;
  ReAlt alts;
};

ReAlt parse_alt(std::string_view re, size_t& at);

ReSeq parse_seq(std::string_view re, size_t& at) {
  ReSeq seq;
  while (at < re.size() && re[at] != '|' && re[at] != ')') {
    const char ch = re[at++];
    ReNode n;
    if (ch == '^') {
      n.kind = ReNode::Begin;
    } else if (ch == '$') {
      n.kind = ReNode::End;
    } else if (ch == '(') {
      n.kind = ReNode::Group;
      n.alts = parse_alt(re, at);
      if (at >= re.size() || re[at] != ')') SVX_FAIL("svx materials: unbalanced rule pattern");
      ++at;
    } else {
      const bool lit = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
      if (!lit) SVX_FAIL("svx materials: unsupported rule pattern");
      n.c = ch;
    }
    seq.push_back(std::move(n));
  }
  return seq;
}

ReAlt parse_alt(std::string_view re, size_t& at) {
  ReAlt alt;
  alt.push_back(parse_seq(re, at));
  while (at < re.size() && re[at] == '|') {
    ++at;
    alt.push_back(parse_seq(re, at));
  }
  return alt;
}

ReAlt parse_re(std::string_view re) {
  size_t at = 0;
  ReAlt alt = parse_alt(re, at);
  if (at != re.size()) SVX_FAIL("svx materials: unbalanced rule pattern");
  return alt;
}

void ends_alt(const ReAlt& a, std::string_view s, size_t pos, std::vector<size_t>& out);

// Every position where sequence q (from item k) can end, starting at pos.
void ends_seq(const ReSeq& q, size_t k, std::string_view s, size_t pos, std::vector<size_t>& out) {
  for (; k < q.size(); ++k) {
    const ReNode& n = q[k];
    if (n.kind == ReNode::Lit) {
      if (pos < s.size() && s[pos] == n.c) {
        ++pos;
        continue;
      }
      return;
    }
    if (n.kind == ReNode::Begin) {
      if (pos == 0) continue;
      return;
    }
    if (n.kind == ReNode::End) {
      if (pos == s.size()) continue;
      return;
    }
    std::vector<size_t> mids;
    ends_alt(n.alts, s, pos, mids);
    for (const size_t p : mids) ends_seq(q, k + 1, s, p, out);
    return;
  }
  out.push_back(pos);
}

void ends_alt(const ReAlt& a, std::string_view s, size_t pos, std::vector<size_t>& out) {
  for (const ReSeq& q : a) ends_seq(q, 0, s, pos, out);
}

bool re_test(const ReAlt& re, std::string_view s) {
  std::vector<size_t> out;
  for (size_t start = 0; start <= s.size(); ++start) {
    ends_alt(re, s, start, out);
    if (!out.empty()) return true;
  }
  return false;
}

// ---- the classes: [name, structvox id, the city's own properties]

std::vector<SvxClass> make_classes() {
  auto own = [](double E, double G, double rho, double ft, double fb, double fc, double cohesion, double friction, double Gf,
                std::array<double, 3> frag, double frag_noise) {
    SvxOwnMaterial o;
    o.E = E, o.G = G, o.rho = rho, o.ft = ft, o.fb = fb, o.fc = fc, o.cohesion = cohesion, o.friction = friction, o.Gf = Gf;
    o.frag = frag;
    o.frag_noise = frag_noise;
    return o;
  };
  std::vector<SvxClass> c = {
      {"rc", 0, std::nullopt},       {"concrete", 1, std::nullopt},       {"steel", 2, std::nullopt},  {"masonry", 3, std::nullopt},
      {"soil", 4, std::nullopt},     {"rock", 5, std::nullopt},           {"bedrock", 6, std::nullopt}, {"wood", 7, std::nullopt},
      {"stone", 8, std::nullopt},    {"glass", 9, std::nullopt},          {"steel_section", 11, std::nullopt},
      {"sheet", 12, std::nullopt},   {"window", 15, std::nullopt},        {"tyre", 16, std::nullopt},  {"plastic", 17, std::nullopt},
      {"asphalt", 18, std::nullopt}, {"paint", 19, std::nullopt},         {"lamp", 20, std::nullopt},
  };
  // roof coverings (tiles, shingles, slate, sheet, membrane, sod) on their battens, smeared over a voxel
  c.push_back({"roofing", kCityBase + 0, own(5e9, 2e9, 700, 0.3e6, 0.6e6, 5e6, 0.3e6, 0.6, 60, {3, 3, 2}, 0.3)});
  // interior partitions and ceilings: plasterboard on studs, smeared
  c.push_back({"partition", kCityBase + 1, own(3e9, 1.2e9, 600, 0.2e6, 0.4e6, 3e6, 0.2e6, 0.6, 80, {3, 3, 3}, 0.35)});
  // upholstery, bedding, fabric, goods, paper: light, soft, tough to tear
  SvxOwnMaterial soft = own(0.02e9, 0.008e9, 200, 0.05e6, 0.05e6, 0.2e6, 0.05e6, 0.8, 500, {4, 4, 4}, 0.3);
  soft.crush = 2e4;
  c.push_back({"soft", kCityBase + 2, soft});
  // lake and sea ice, brittle, slippery
  SvxOwnMaterial ice = own(9e9, 3.5e9, 917, 1e6, 1.5e6, 5e6, 1e6, 0.1, 5, {3, 3, 3}, 0.5);
  ice.grip = 0.1;
  c.push_back({"ice", kCityBase + 3, ice});
  // packed snow on the ground and on roofs
  SvxOwnMaterial snow = own(0.05e9, 0.02e9, 350, 0.02e6, 0.02e6, 0.2e6, 0.02e6, 0.3, 5, {2, 2, 2}, 0.5);
  snow.grip = 0.3;
  c.push_back({"snow", kCityBase + 4, snow});
  // decorative plants as solid voxels (the "solid" flora policy only)
  c.push_back({"foliage", kCityBase + 5, own(0.01e9, 0.004e9, 60, 0.02e6, 0.02e6, 0.05e6, 0.02e6, 0.6, 50, {3, 3, 3}, 0.5)});
  return c;
}

// ---- the rules, first match wins: s its class as structure (off the ground), g its class as
// ground (anchored; default s), f decorative (flora), w liquid (the water layer). (null: JS's
// undefined)
struct Rule {
  const char* re;
  const char* s;
  const char* g;
  bool f;
  bool w;
};
const Rule kRules[] = {
    {"^AIR$", nullptr, nullptr, false, false},
    {"^(WATER|SEWER_WATER|SLUDGE|NUKAGE)$", nullptr, nullptr, false, true},
    // --- decorative plants: flora off the ground (on the ground as its cover: soil)
    {"^(LEAVES|LEAF_|HEDGE|BUSH|FLOWER|REED|CACTUS|PALM_FROND|SHRUB|TWIGS|BERRY|FERN|BLUEBERRY|LINGON|GRASS_TALL|MOSS_BRIGHT|MUSHROOM|CROP_|PLANT$|HEATHER|LICHEN)",
     nullptr, "soil", true, false},
    {"^NEEDLE_LITTER$", nullptr, "soil", true, false},
    // --- ground: soil, rock, bedrock (off the ground: a stone is stone, soil soil)
    {"^BEDROCK$", "rock", "bedrock", false, false},
    {"^(STONE|ROCK|ROCK_DARK|ROCK_LIGHT|SANDSTONE|GRANITE_PINK|CAVE_FLOOR|CRYSTAL_)", "stone", "rock", false, false},
    {"^(DIRT|GRASS|FOREST_FLOOR|SAND|GRAVEL|CLAY|MUD|RED_EARTH|SAVANNA_GRASS|MOSS|TUNDRA|MARSH|JUNGLE_FLOOR|CAVE_MOSS|LAWN_|SOIL_BED|BALLAST|PARK_PATH|TREE_PIT|SAND_BOX|SAND_DUNE)",
     "soil", nullptr, false, false},
    {"^(SNOW|SNOW_WIND)$", "snow", nullptr, false, false},
    {"^ICE$", "ice", nullptr, false, false},
    // --- roads and paving
    {"^(ASPHALT|SPORT_COURT|COURT_ORANGE)", "asphalt", nullptr, false, false},
    {"^(LINE_|TACTILE|HAZARD_)", "paint", nullptr, false, false},
    {"^(CURB|SIDEWALK|PAVER_|PLATFORM_EDGE|SEWER_CURB|SEWER_FLOOR)", "concrete", nullptr, false, false},
    {"^(PLAZA_STONE|COBBLE|FLAGSTONE)", "stone", nullptr, false, false},
    {"^(MANHOLE|DRAIN_GRATE|GRATE_STEEL|MANHOLE_RIM)", "steel", nullptr, false, false},
    {"^RUBBER_MAT$", "tyre", nullptr, false, false},
    // --- structure and facades
    {"^(HW_CONCRETE|HW_BARRIER|TUNNEL_WALL|CONCRETE|CONCRETE_DARK|CONCRETE_LIGHT)$", "rc", nullptr, false, false},
    {"^(CINDERBLOCK|BRICK_|SEWER_BRICK|PLASTER_|RENDER_|CERAMIC|BONE)", "masonry", nullptr, false, false},
    {"^(LIMESTONE|GRANITE|GRANITE_LIGHT|TRIM_STONE|CORNICE|COUNTERTOP|PLANT_POT)", "stone", nullptr, false, false},
    {"^(PANEL_WHITE|PANEL_GRAPHITE|PANEL_BEIGE|PANEL_GRAYBLUE|PANEL_JOINT|PARAPET_CAP|TUNNEL_TILE|STAIR_CONCRETE|RAIL_TIE|FLOOR_(TILE|CARPET|CONCRETE|EPOXY|MARBLE|LINOLEUM|TERRAZZO))",
     "concrete", nullptr, false, false},
    {"^(METAL_PANEL|CORRUGATED|CONTAINER_|SIGN_)", "sheet", nullptr, false, false},
    {"^(ROLLUP_DOOR|ELEVATOR_DOOR|TANK_WHITE|SILO_STEEL|AC_UNIT|VENT|SIGNAL_BOX|CAR_(RED|BLUE|WHITE|BLACK|SILVER|YELLOW|GREEN)|FIRE_RED|POLICE_BLUE|GOLD|DOME_GREEN|DOME_BLUE)$",
     "sheet", nullptr, false, false},
    {"^(WOOD_|SIDING_|CLAD_|LOG_TARRED|BARN_RED|PINE_PANEL|FLOOR_(OAK|WALNUT|PARQUET)|DOOR_(WOOD|WHITE|RED|GREEN|FRAME)|STAIR_WOOD|HANDRAIL_WOOD|FRAME_WOOD|BASEBOARD|LAMINATE_|PALLET|SHELF_ORANGE|CHALKBOARD|FENCE_WOOD|FENCE_WHITE|WATER_TANK_WOOD|STAGE_BLACK|BARK|DEADWOOD)",
     "wood", nullptr, false, false},
    {"^(GLASS|DOOR_GLASS|NEON_|LIGHT_STRIP|SCREEN|MIRROR|LAMP_SHADE|LAMP_LIGHT|SOLAR_PANEL|STAGE_LIGHT|PROJECTION|CEILING_LIGHT|BOTTLES)", "glass", nullptr,
     false, false},
    {"^(STEEL_BEAM|STEEL_RUST|RAIL_STEEL|HYDRANT|BOLLARD|VALVE_RED|APPLIANCE_STEEL)$", "steel", nullptr, false, false},
    {"^(MULLION|FRAME_WHITE|FRAME_DARK|DOOR_METAL|STAIR_NOSING|RAILING|POLE_|CHAINLINK|PIPE|LADDER|LAMP_CAGE|METAL_CHROME|METAL_BLACK|SHELF_METAL)",
     "steel_section", nullptr, false, false},
    // --- roofs, interiors, furnishings
    {"^(ROOF_|DOME_SHINGLE)", "roofing", nullptr, false, false},
    {"^(PAINT_|WALL_TILE_|CEILING|CEILING_TILE)", "partition", nullptr, false, false},
    {"^(FABRIC_|LEATHER|MATTRESS|BEDSHEET_|PILLOW|RUG_|BOOKS|CARDBOARD|GOODS|BREAD|PRODUCE_|CLOTHES|MEAT|FISH|CURTAIN_|VELVET_RED|MEDICAL_GREEN|ART_|AWNING_|HAY)",
     "soft", nullptr, false, false},
    {"^(PLASTIC_|BIN_GREEN|PLAY_|CONE_|SIGNAGE|EMERGENCY_RED)", "plastic", nullptr, false, false},
    {"^(TIRE)$", "tyre", nullptr, false, false},
    {"^(CAR_GLASS)$", "window", nullptr, false, false},
    {"^(HEADLIGHT|TAILLIGHT|SIGNAL_RED|SIGNAL_GREEN|SIGNAL_AMBER)$", "lamp", nullptr, false, false},
};
constexpr size_t kRuleCount = sizeof(kRules) / sizeof(kRules[0]);

struct Tables {
  std::vector<SvxClass> classes;
  std::vector<SvxClassify> classify;      // per city material
  std::vector<std::vector<int>> looks;    // per class id
  std::vector<int> look_classes;          // LOOKS's keys in insertion order
  std::vector<int> flora;
};

Tables make_tables() {
  Tables t;
  t.classes = make_classes();
  int max_id = 0;
  for (const SvxClass& c : t.classes) max_id = c.id > max_id ? c.id : max_id;
  t.looks.resize(static_cast<size_t>(max_id) + 1);
  std::vector<ReAlt> res;
  res.reserve(kRuleCount);
  for (const Rule& r : kRules) res.push_back(parse_re(r.re));
  auto rule_for = [&](std::string_view name) -> const Rule* {
    for (size_t k = 0; k < kRuleCount; ++k)
      if (re_test(res[k], name)) return &kRules[k];
    return nullptr;
  };
  auto class_of = [&](const char* name) -> const SvxClass* {
    if (!name) return nullptr;
    for (const SvxClass& c : t.classes)
      if (c.name == name) return &c;
    return nullptr;
  };
  // (lookOf: a material's look index in its class, the class's list made on first use)
  auto look_of = [&](int class_id, int mat) {
    std::vector<int>& list = t.looks[static_cast<size_t>(class_id)];
    if (list.empty()) t.look_classes.push_back(class_id);
    int k = -1;
    for (size_t q = 0; q < list.size(); ++q)
      if (list[q] == mat) k = static_cast<int>(q);
    if (k < 0) {
      k = static_cast<int>(list.size());
      list.push_back(mat);
    }
    if (k > 255) SVX_FAIL("svx materials: a class has more than 256 looks");
    return k;
  };
  std::string unclassified;
  for (int id = 0; id < kMaterialCount; ++id)
    if (!rule_for(material_info(id).name)) unclassified += std::string(" ") + material_info(id).name;
  if (!unclassified.empty()) {
    std::fprintf(stderr, "svx_city: svx: no physics class for%s\n", unclassified.c_str());
    SVX_FAIL("svx materials: a material no rule covers");
  }
  for (int id = 0; id < kMaterialCount; ++id) {
    const MaterialInfo& m = material_info(id);
    const Rule& spec = *rule_for(m.name);
    SvxClassify e;
    if (m.id == 0) {
      e.air = true;
      t.classify.push_back(e);
      continue;
    }
    if (spec.w) {
      e.liquid = true;
      t.classify.push_back(e);
      continue;
    }
    const SvxClass* s = class_of(spec.s ? spec.s : spec.f ? "foliage" : nullptr);
    const SvxClass* g = class_of(spec.g ? spec.g : spec.s ? spec.s : "soil");
    if (!s || !g) SVX_FAIL("svx materials: a rule names no class");
    e.s = s->id;
    e.g = g->id;
    e.s_look = look_of(s->id, m.id);
    e.g_look = look_of(g->id, m.id);
    if (spec.f) {
      e.flora = true;
      e.flora_idx = static_cast<int>(t.flora.size());
      t.flora.push_back(m.id);
    }
    t.classify.push_back(e);
  }
  return t;
}

const Tables& tables() {
  static const Tables t = make_tables();
  return t;
}

}  // namespace

const std::vector<SvxClass>& svx_classes() { return tables().classes; }

const SvxClass* svx_class(std::string_view name) {
  for (const SvxClass& c : tables().classes)
    if (c.name == name) return &c;
  return nullptr;
}

const SvxClassify& svx_classify(int material) { return tables().classify[static_cast<size_t>(material)]; }

const std::vector<int>& svx_looks(int class_id) {
  static const std::vector<int> none;
  const Tables& t = tables();
  if (class_id < 0 || static_cast<size_t>(class_id) >= t.looks.size()) return none;
  return t.looks[static_cast<size_t>(class_id)];
}

const std::vector<int>& svx_look_classes() { return tables().look_classes; }

const std::vector<int>& svx_flora() { return tables().flora; }

Value svx_materials() {
  const Tables& t = tables();
  using M = Value::Member;
  Value reg = Value::array();
  Value classes = Value::array();
  for (const SvxClass& c : t.classes) {
    classes.push(Value::object({M{"id", c.id}, M{"name", c.name}, M{"own", c.own.has_value()}}));
    if (!c.own) continue;
    const SvxOwnMaterial& o = *c.own;
    Value e = Value::object({M{"id", c.id}, M{"name", "city_" + c.name}, M{"E", o.E}, M{"G", o.G}, M{"rho", o.rho}, M{"ft", o.ft}, M{"fb", o.fb},
                             M{"fc", o.fc}, M{"cohesion", o.cohesion}, M{"friction", o.friction}, M{"Gf", o.Gf},
                             M{"frag", Value::array({o.frag[0], o.frag[1], o.frag[2]})}, M{"frag_noise", o.frag_noise}});
    if (o.crush) e.set("crush", *o.crush);
    if (o.grip) e.set("grip", *o.grip);
    reg.push(std::move(e));
  }
  Value looks = Value::object();
  for (const int id : t.look_classes) {
    Value names = Value::array();
    for (const int m : t.looks[static_cast<size_t>(id)]) names.push(material_info(m).name);
    looks.set(js::num(id), std::move(names));
  }
  Value flora = Value::array();
  for (const int m : t.flora) flora.push(material_info(m).name);
  Value palette = Value::object();
  for (int id = 0; id < kMaterialCount; ++id) {
    const MaterialInfo& m = material_info(id);
    palette.set(m.name, Value::object({M{"rgb", Value::array({m.rgb[0], m.rgb[1], m.rgb[2]})}, M{"transparent", m.transparent}, M{"opacity", m.opacity},
                                       M{"emissive", m.emissive}, M{"glow", m.glow}, M{"noise", m.noise}}));
  }
  return Value::object({M{"cityBase", kCityBase}, M{"register", std::move(reg)}, M{"classes", std::move(classes)}, M{"looks", std::move(looks)},
                        M{"flora", std::move(flora)}, M{"palette", std::move(palette)}});
}

}  // namespace svx::city
