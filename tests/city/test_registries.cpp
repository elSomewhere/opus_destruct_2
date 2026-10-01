// svx_city tests — the data registries (voxel_city city/districts.js, city/flavors.js,
// buildings/styles.js, buildings/civic.js, the archetypes of buildings/archetypes.js) and what
// reads them, against the reference (stage "registries" of tools/procgen_ref).
#include <doctest.h>

#include "buildings/archetype_registry.hpp"
#include "buildings/civic.hpp"
#include "buildings/frame.hpp"
#include "buildings/styles.hpp"
#include "city/districts.hpp"
#include "city/flavors.hpp"
#include "records.hpp"
#include "sites/complex.hpp"
#include "world/register_all.hpp"
#include "world/sites.hpp"

using namespace svx::city;
using rec::Line;

namespace {

std::string num(double v) { return js::num(v); }

// [id:weight,...]
std::string W(const WeightedIds& list) {
  std::string s = "[";
  for (size_t i = 0; i < list.size(); ++i) {
    if (i) s += ',';
    s += list[i].first + ":" + num(list[i].second);
  }
  return s + "]";
}
std::string A2(const std::array<double, 2>& a) { return "[" + num(a[0]) + "," + num(a[1]) + "]"; }
std::string str_or_undef(const std::string& s) { return s.empty() ? std::string("-") : s; }
template <class T>
std::string opt(const std::optional<T>& v) {
  return v ? rec::f(*v) : std::string("-");
}
std::string optnum(double v) { return v == v ? num(v) : std::string("-"); }
// {key:value,...}
template <class T, class Fmt>
std::string M(const ByDistrict<T>& m, Fmt fmt) {
  std::string s = "{";
  for (size_t i = 0; i < m.size(); ++i) {
    if (i) s += ',';
    s += m[i].first + ":" + fmt(m[i].second);
  }
  return s + "}";
}

Line flavor_line(const Flavor& fl) {
  Line l;
  l << "flavor" << fl.id << fl.weight << fl.floor_scale << static_cast<bool>(fl.when) << opt(fl.old_core) << fl.adaptive << fl.collectors << str_or_undef(fl.main_road)
    << opt(fl.main_road_wobble) << opt(fl.cobble_within) << str_or_undef(fl.church_style);
  if (fl.church_dome.empty())
    l << rec::kUndef;
  else
    l << fl.church_dome;
  l << M(fl.patterns, [](const std::string& v) { return v; }) << M(fl.pitched, [](double v) { return num(v); })
    << M(fl.styles, [](const std::optional<WeightedIds>& v) { return v ? W(*v) : std::string("-"); }) << M(fl.archetypes, W) << M(fl.floors, A2)
    << M(fl.block_use, W);
  if (fl.district_fn)
    l << "fn";
  else
    l << M(fl.districts, [](const std::string& v) { return v; });
  return l;
}

std::string rect_str(const Rect& q) { return num(q.x0) + "," + num(q.y0) + "," + num(q.x1) + "," + num(q.y1); }

Line env_line(const std::optional<EnvSpec>& env) {
  Line l;
  l << "env";
  if (!env) {
    l << rec::kUndef;
    return l;
  }
  std::string tiers;
  for (size_t i = 0; i < env->tiers.size(); ++i) {
    const EnvTier& t = env->tiers[i];
    if (i) tiers += '|';
    tiers += num(t.f0) + "/" + num(t.f1) + "/";
    for (size_t k = 0; k < t.rects.size(); ++k) tiers += (k ? ";" : "") + rect_str(t.rects[k]);
  }
  std::string annexes;
  for (size_t i = 0; i < env->annexes.size(); ++i) {
    const EnvAnnex& a = env->annexes[i];
    if (i) annexes += '|';
    annexes += a.kind + "/" + rect_str(a.rect) + "/" + num(a.height);
  }
  if (annexes.empty()) annexes = "none";
  l << tiers << annexes << env->floors << env->story_h << env->basements << env->roof.type << str_or_undef(env->roof.ridge) << optnum(env->roof.slope);
  if (env->roof.overhang_kind == EnvRoof::Overhang::Number)
    l << env->roof.overhang;
  else
    l << rec::kUndef;
  l << optnum(env->roof.pitch) << env->program.ground << str_or_undef(env->program.upper) << env->entrance_side;
  const CivicExtra& x = *env->extra;
  l << x.civic << x.storefront << str_or_undef(x.sign) << x.sign_color << x.portico << x.parking << x.set_f;
  return l;
}

}  // namespace

TEST_CASE("city registries: districts, flavors, styles and the civic table are the reference's (stage registries)") {
  register_all();
  rec::Samples r(17);
  rec::Out out;
  // ---- districts
  for (const District& d : district_registry().all()) {
    const DistrictStreets& s = d.streets;
    out << (Line() << "district" << d.id << d.label << d.color << d.port << s.pattern << ("[" + A2(s.block[0]) + "," + A2(s.block[1]) + "]") << s.pedestrian_chance
                   << s.merge_chance << s.local_class << str_or_undef(s.lane_class) << opt(s.lane_chance) << str_or_undef(s.paving) << W(d.block_use) << d.lots.mode
                   << A2(d.lots.width) << d.lots.alley_chance << W(d.archetypes) << A2(d.floors) << W(d.styles) << opt(d.pitched));
  }
  for (int i = 0; i < 800; ++i) {
    Line l;
    l << "cd";
    for (int k = 0; k < 25; ++k) {
      DistrictFields f;
      f.u = r();
      const double c1 = r();
      const double c2 = r();
      f.core = c1 * c2 * 1.2;
      f.dn = r() - 0.5;
      f.ind = r();
      f.seed = std::floor(r() * 1e6);
      f.key = std::floor(r() * 4294967296.0);
      f.village = r() < 0.1;
      f.port = r() < 0.2;
      l << classify_district(f);
    }
    out << l;
  }
  // ---- styles
  for (const Style& s : style_registry().all()) {
    const StyleWindow& w = s.window;
    Line l;
    l << "style" << s.id << s.walls << s.base << s.trim << s.glass << s.frame << w.type << w.width << w.sill << w.head << w.bay << w.lintel << opt(w.shutters) << w.casing
      << w.transom << s.cornice << s.roof << s.pitched << s.fire_escape << s.balcony << s.water_tank << s.awning << s.storefront;
    if (s.joint.empty())
      l << rec::kUndef;
    else
      l << s.joint;
    l << s.accent_strips << s.boards << s.corners << opt(s.shop_base) << opt(s.base_h);
    out << l;
  }
  for (int m = 0; m < 400; ++m)
    if (const Seam* s = seam_of(static_cast<uint16_t>(m))) out << (Line() << "seam" << m << s->m << s->dir);
  for (const Style& s : style_registry().all())
    for (int k = 0; k < 8; ++k) {
      Rng g(1000 + k * 7919);
      const ResolvedStyle rs = resolve_style(s.id, g);
      Line l;
      l << "rs" << rs.id << rs.wall << rs.base << rs.trim << rs.glass << rs.frame << rs.window.type << rs.window.bay << rs.cornice << rs.roof << rs.pitched << rs.storefront
        << rs.fire_escape << rs.balcony << rs.water_tank << rs.awning << rs.shutters << rs.accent_strips;
      if (rs.joint < 0)
        l << rec::kUndef;
      else
        l << rs.joint;
      if (rs.seam < 0)
        l << rec::kUndef << rec::kUndef;
      else
        l << rs.seam << rs.seam_dir;
      l << rs.corners << rs.shop_base << rs.base_h << g.next();
      out << l;
    }
  // ---- flavors
  std::vector<std::string> ids;
  for (const Flavor& fl : flavor_registry().all()) ids.push_back(fl.id);
  std::vector<const Flavor*> villages;
  for (const std::string& id : ids) {
    FlavorSettlement s;
    s.village = true;
    s.flavor = id;
    villages.push_back(&flavor_of(&s));
  }
  for (const Flavor& fl : flavor_registry().all()) out << flavor_line(fl);
  for (const Flavor* fl : villages) out << flavor_line(*fl);
  auto settlement = [&] {
    FlavorSettlement s;
    double i = 1, j = 2;
    if (r() < 0.15) s.flavor = r() < 0.8 ? ids[static_cast<size_t>(std::floor(r() * static_cast<double>(ids.size())))] : std::string("nope");
    if (r() < 0.1) {
      i = 0;
      j = 0;
    }
    s.origin_cell = i == 0 && j == 0;
    s.island = r() < 0.2;
    s.village = r() < 0.25;
    if (r() < 0.9) {
      s.t = r();
      s.m = r();
    }
    s.style = std::floor(r() * 4294967296.0);
    return s;
  };
  out << (Line() << "fo" << flavor_of(nullptr).id);
  for (int i = 0; i < 3000; ++i) {
    const FlavorSettlement s = settlement();
    out << (Line() << "fo" << flavor_of(&s).id << flavor_registry().get("desert").when(s) << flavor_registry().get("nordic").when(s));
  }
  for (const District& d : district_registry().all()) {
    std::vector<const Flavor*> fls;
    for (const Flavor& fl : flavor_registry().all()) fls.push_back(&fl);
    fls.insert(fls.end(), villages.begin(), villages.end());
    for (const Flavor* fl : fls) {
      const District fd = flavored_district(d, *fl);
      out << (Line() << "fd" << d.id << fl->id << fd.id << W(fd.styles) << W(fd.archetypes) << A2(fd.floors) << W(fd.block_use) << opt(fd.pitched));
    }
  }
  std::vector<std::string> all_ids;
  for (const District& d : district_registry().all()) all_ids.push_back(d.id);
  all_ids.push_back("nope");
  for (int i = 0; i < 2000; ++i) {
    std::optional<FlavorSettlement> s;
    if (!(r() < 0.05)) s = settlement();
    const double c = r();
    std::optional<FlavorPlace> ctx;
    if (c >= 0.3) {
      FlavorPlace p;
      p.d = r() * 1.2;
      p.dn = r() - 0.5;
      p.ind = r() < 0.1 ? js::kNaN : r();
      p.u = r();
      p.core = r();
      ctx = p;
    } else if (c >= 0.2) {
      ctx = FlavorPlace{};
    }
    Line l;
    l << "fdi";
    for (const std::string& id : all_ids) l << flavored_district_id(id, s ? &*s : nullptr, ctx ? &*ctx : nullptr);
    out << l;
  }
  // ---- civic
  for (const CivicSpec& s : civic_table())
    out << (Line() << "civic" << s.id << A2(s.w) << A2(s.d) << A2(s.floors) << s.story << A2(s.set_f) << A2(s.set_s) << A2(s.lot) << str_or_undef(s.front)
                   << str_or_undef(s.sign) << str_or_undef(s.roof) << s.parking << s.canopy << s.portico);
  {
    std::vector<std::pair<std::string, std::string>> cg;  // (printed, passed)
    for (const std::string& id : ids) cg.emplace_back(id, id);
    for (const std::string& id : ids) cg.emplace_back(id + "Village", id + "Village");
    cg.emplace_back("-", "");  // null
    cg.emplace_back("-", "");  // undefined
    cg.emplace_back("\"\"", "");
    cg.emplace_back("nope", "nope");
    cg.emplace_back("Village", "Village");
    cg.emplace_back("nordicVillageVillage", "nordicVillageVillage");
    for (const auto& [printed, passed] : cg) out << (Line() << "cg" << printed << civic_group(passed));
  }
  double seed = 7;
  std::vector<std::string> cids;
  for (const CivicSpec& s : civic_table()) cids.push_back(s.id);
  cids.push_back("school");
  for (const std::string& cid : cids) {
    std::vector<std::pair<std::string, std::string>> fids;
    for (const std::string& id : ids) fids.emplace_back(id, id);
    for (const std::string& id : ids) fids.emplace_back(id + "Village", id + "Village");
    fids.emplace_back("-", "");
    for (const auto& [printed, passed] : fids) {
      seed += 31;
      Rng g(seed);
      const std::string a = civic_style(cid, passed, g);
      const std::string b = civic_style(cid, passed, g);
      out << (Line() << "cs" << cid << printed << a << b << g.next());
    }
  }
  for (const Archetype& a : archetype_registry().all()) {
    std::string fits;
    for (double U = 40; U <= 640; U += 40)
      for (double V = 40; V <= 640; V += 60) fits += a.fits(U, V) ? '1' : '0';
    out << (Line() << "arch" << a.id << str_or_undef(a.label) << a.civic << fits << static_cast<bool>(a.entrance_u));
    if (!a.civic) continue;  // (the others' envelopes need a lot and a district: stage "archetypes")
    for (int k = 0; k < 40; ++k) {
      const double U = 60 + std::floor(r() * 700);
      const double V = 60 + std::floor(r() * 700);
      const double pcs[6] = {0, 0, 0.08, 0.35, 0.65, 1};  // (undefined: 0)
      const double pc = pcs[static_cast<int>(std::floor(r() * 6))];
      Rng rng(std::floor(r() * 1e9));
      const Frame frame(Rect{0, 0, U - 1, V - 1}, 'N');
      ArchetypeCtx ctx;
      ctx.rng = &rng;
      ctx.frame = &frame;
      ctx.pitched_civic = pc;
      const std::optional<EnvSpec> env = a.envelope(ctx);
      out << (env_line(env) << rng.next());
    }
  }
  CHECK(rec::record("registries", out.text()) == rec::recorded_digest("registries"));
}

TEST_CASE("city registries: registration order is the reference's") {
  register_all();
  register_all();  // (once)
  // city/districts.js's 17, then the site kinds' (sites/militaryBase.js, researchComplex.js,
  // mountainBase.js, after the complex themes), with their SITES in that order
  CHECK(district_registry().size() == 20);
  CHECK(district_registry().all()[16].id == "rural");
  CHECK(district_registry().all()[17].id == "military");
  CHECK(district_registry().all()[18].id == "research");
  CHECK(district_registry().all()[19].id == "stronghold");
  REQUIRE(site_registry().size() == 3);
  CHECK(site_registry().all()[0].id == "militaryBase");
  CHECK(site_registry().all()[1].id == "researchComplex");
  CHECK(site_registry().all()[2].id == "mountainBase");
  CHECK(complex_themes().size() == 6);
  CHECK(flavor_registry().size() == 9);
  CHECK(style_registry().size() == 20);
  CHECK(style_registry().all().back().id == "classical");
  CHECK(archetype_registry().size() == 34);
  CHECK(archetype_registry().all().front().id == "supermarket");
  CHECK(archetype_registry().all()[17].id == "house");
  CHECK(archetype_registry().all().back().id == "church");
  CHECK(district_registry().get("oldcore").pitched == 0.85);
  FlavorSettlement v;
  v.village = true;
  v.flavor = "nordic";
  CHECK(flavor_of(&v).id == "nordicVillage");
  CHECK(&flavor_of(&v) == &flavor_of(&v));
}
