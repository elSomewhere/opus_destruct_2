// svx_city — voxel_city sites/researchComplex.js.
#include "sites/researchComplex.hpp"

#include <memory>
#include <optional>
#include <string>

#include "buildings/archetypes.hpp"
#include "buildings/interior/stairs.hpp"
#include "city/districts.hpp"
#include "core/hash.hpp"
#include "core/math.hpp"
#include "sites/complex.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

namespace {

const ResearchComplexPlan& plan_of(const Site& site) { return static_cast<const ResearchComplexPlan&>(*site.plan); }

std::shared_ptr<const SitePlan> plan(const World& world, const Site& site) {
  Rng rng(site.seed);
  const Rect& r = site.rect;
  const SiteGate g = plan_gate(world, site, vx(4.5));
  auto p = std::make_shared<ResearchComplexPlan>();
  const double ring_in = vx(12);
  const double ring_w = vx(8);
  const Rect ring{r.x0 + ring_in, r.y0 + ring_in, r.x1 - ring_in, r.y1 - ring_in};
  const Rect inner{ring.x0 + ring_w + vx(4), ring.y0 + ring_w + vx(4), ring.x1 - ring_w - vx(4), ring.y1 - ring_w - vx(4)};
  const double iw = inner.x1 - inner.x0;
  const double ih = inner.y1 - inner.y0;
  auto at = [&](double fx0, double fy0, double fx1, double fy1) {
    return Rect{js::round(inner.x0 + iw * fx0), js::round(inner.y0 + ih * fy0), js::round(inner.x0 + iw * fx1), js::round(inner.y0 + ih * fy1)};
  };
  p->lots.push_back({"office", at(0.0, 0.0, 0.3, 0.3), 'N', "glass"});
  {
    const Rect q = at(0.36, 0.0, 0.62, 0.27);
    static const std::vector<std::string> kStyles = {"futurist", "glass"};
    p->lots.push_back({"office", q, 'N', rng.pick(kStyles)});
  }
  p->lots.push_back({"office", at(0.7, 0.0, 1.0, 0.3), 'N', "concrete"});
  p->lots.push_back({"warehouse", at(0.0, 0.6, 0.38, 1.0), 'W', "industrial"});
  p->lots.push_back({"garage", at(0.44, 0.66, 0.7, 1.0), 'S', "parkingDeck"});
  // portal blocks over the stairs down to sectors 0 and 1
  const double pw = vx(12);
  const double ph = vx(15);
  for (const Rect& q : {at(0.18, 0.38, 0.18, 0.38), at(0.66, 0.4, 0.66, 0.4)}) p->portals.push_back({q.x0, q.y0, q.x0 + pw, q.y0 + ph});
  const Rect helipad = at(0.45, 0.46, 0.45, 0.46);
  p->helipad = {helipad.x0, helipad.y0, vx(10)};
  for (const Rect& q : {at(0.86, 0.5, 0.86, 0.5), at(0.94, 0.7, 0.94, 0.7)}) p->radomes.push_back({q.x0, q.y0});
  const Rect dishes = at(0.74, 0.88, 0.74, 0.88);
  p->dishes = {dishes.x0, dishes.y0};
  const Rect tanks = at(0.78, 0.34, 0.78, 0.34);
  p->tanks = {tanks.x0, tanks.y0};
  p->gate_side = g.gate_side;
  p->gate = g.gate;
  p->drive = g.drive;
  p->ring = ring;
  p->ring_w = ring_w;
  p->inner = inner;
  const Rect& b = site.blend;
  p->bounds = Rect{js::min(b.x0, g.drive.x0), js::min(b.y0, g.drive.y0), js::max(b.x1, g.drive.x1), js::max(b.y1, g.drive.y1)};
  p->themes = rng.shuffle(std::vector<std::string>{"lab", "containment", "power", "barracks"});
  for (double& l : p->levels) l = rng.int_(2, 3);
  return p;
}

std::vector<SiteSurface> surface(const World& world, const Site& site) {
  Rng rng(static_cast<double>(js::to_int32(site.seed) ^ 0x51ed));
  const District& district = district_registry().get("research");
  const ResearchComplexPlan& p = plan_of(site);
  std::vector<SiteSurface> envs;
  for (size_t k = 0; k < p.lots.size(); ++k) {
    const SiteLot& l = p.lots[k];
    Lot lot;
    lot.id = js::cat("C", site.cell.i, "_", site.cell.j, "/", site.type, site.a, "_", site.b, "/l", k);
    lot.rect = l.rect;
    lot.front = l.front;
    lot.frontages = {Frontage{l.front, "local"}};
    lot.corner = false;
    lot.district = "research";
    lot.ground_z = site.pad_z;
    lot.block = site.id;
    lot.cell = js::cat("C", site.cell.i, "_", site.cell.j);
    Rng fork = rng.fork(js::cat("b", k));
    EnvelopeExtra extra;
    extra.u = 0.2;
    extra.core = 0.45;
    extra.ground_z = site.pad_z;
    extra.config = &world.config;
    std::optional<Envelope> env = plan_building_envelope_as(lot, l.kind, l.style, district, fork, extra);
    if (env) envs.push_back({std::move(lot), std::move(*env)});
  }
  return envs;
}

void ground(const Site& site, double x, double y, SiteGround& out) {
  const ResearchComplexPlan& p = plan_of(site);
  auto in_rect = [&](const Rect& q, double m) { return x >= q.x0 - m && x <= q.x1 + m && y >= q.y0 - m && y <= q.y1 + m; };
  const Rect& drive = *p.drive;
  if (in_rect(drive, 0)) {
    const double c = p.gate_side == 'N' || p.gate_side == 'S' ? x - (drive.x0 + drive.x1) / 2 : y - (drive.y0 + drive.y1) / 2;
    out.mat = std::fabs(c) < 0.6 ? MAT::LINE_YELLOW : MAT::ASPHALT;
    out.sub = MAT::GRAVEL;
    return;
  }
  if (!out.inside) return;
  const Rect& r = p.ring;
  const bool on_ring = in_rect(r, 0) && (x < r.x0 + p.ring_w || x > r.x1 - p.ring_w || y < r.y0 + p.ring_w || y > r.y1 - p.ring_w);
  if (on_ring) {
    out.mat = MAT::ASPHALT;
    out.sub = MAT::GRAVEL;
    return;
  }
  const SiteHelipad& h = p.helipad;
  const double dh = js::hypot(x - h.x, y - h.y);
  if (dh < h.r) {
    out.mat = helipad_mat(h, x, y, dh);
    out.sub = MAT::CONCRETE;
    return;
  }
  for (const Rect& q : p.portals) {
    if (in_rect(q, vx(4))) {
      out.mat = (js::to_int32(static_cast<double>(js::sar(x, 4)) + js::sar(y, 4)) & 1) ? MAT::CONCRETE : MAT::CONCRETE_LIGHT;
      out.sub = MAT::CONCRETE;
      return;
    }
  }
  for (const SiteLot& l : p.lots) {
    if (in_rect(l.rect, vx(5))) {
      out.mat = MAT::PLAZA_STONE;
      out.sub = MAT::CONCRETE;
      return;
    }
  }
  // lawns crossed by paths between the buildings
  const double px = std::fmod(std::fmod(x, 160) + 160, 160);
  const double py = std::fmod(std::fmod(y, 160) + 160, 160);
  out.mat = px < 12 || py < 12 ? MAT::PAVER_GRAY : MAT::GRASS_LAWN;
  out.sub = MAT::DIRT;
}

// The facility: 2 x 2 sectors under the campus, a tram loop, portals to sectors 0 and 1.
Complex plan_underground(const Site& site, const ResearchComplexPlan& p, Rng& rng) {
  const Rect& r = site.rect;
  const double pad = vx(10);
  const double mx = js::round((r.x0 + r.x1) / 2);
  const double my = js::round((r.y0 + r.y1) / 2);
  const std::vector<Rect> quads = {
      {r.x0 + pad, r.y0 + pad, mx - pad / 2, my - pad / 2},
      {mx + pad / 2, r.y0 + pad, r.x1 - pad, my - pad / 2},
      {r.x0 + pad, my + pad / 2, mx - pad / 2, r.y1 - pad},
      {mx + pad / 2, my + pad / 2, r.x1 - pad, r.y1 - pad},
  };
  const double top = site.pad_z + 1;
  ComplexSpec spec;
  for (size_t k = 0; k < quads.size(); ++k) {
    ComplexSectorSpec s;
    s.bounds = quads[k];
    s.z0 = top - vx(16) - static_cast<double>(k) * vx(9);
    s.levels = p.levels[k];
    s.theme = p.themes[k];
    s.rooms = std::array<double, 2>{8, 12};
    spec.sectors.push_back(s);
  }
  double deepest = js::kInf;
  for (const ComplexSectorSpec& s : spec.sectors) deepest = js::min(deepest, s.z0 - (s.levels - 1) * kLevelGap);
  const StairDims sd = stair_dims(30);
  // portal shafts: portal k sits over the sector whose bounds contain it (at most one a sector)
  std::vector<double> seen;
  for (const Rect& q : p.portals) {
    const double cx = js::round((q.x0 + q.x1) / 2);
    ComplexEntrySpec e;
    e.rect = {cx - std::floor(sd.W / 2), q.y0 + 12, cx - std::floor(sd.W / 2) + sd.W - 1, q.y0 + 12 + sd.L - 1};
    double sector = -1;
    for (size_t k = 0; k < quads.size(); ++k)
      if (cx >= quads[k].x0 && cx <= quads[k].x1 && q.y0 >= quads[k].y0 && q.y0 <= quads[k].y1) {
        sector = static_cast<double>(k);
        break;
      }
    if (sector < 0) sector = 0;
    e.z_top = top;
    e.sector = sector;
    e.dir = -1;
    e.open_top = true;
    bool dup = false;
    for (const double s : seen)
      if (s == sector) dup = true;
    if (dup) continue;
    seen.push_back(sector);
    spec.entries.push_back(e);
  }
  spec.tram_z = deepest - kLevelGap;
  return plan_complex(rng, spec);
}

SiteStructure structure(const World& world, const Site& site) {
  Rng rng(static_cast<double>(js::to_int32(site.seed) ^ 0x77));
  const ResearchComplexPlan& p = plan_of(site);
  BoxLists lists;
  std::vector<SiteBox>& details = lists.details;
  const double z = site.pad_z;
  fence(details, site.rect, z, p.gate_side, p.gate, 26);
  gate_booth(details, p.gate, p.gate_side, z);
  watchtowers(details, site.rect, z);
  for (const Point2& q : p.radomes) radome(details, q.x, q.y, z, 44);
  dish_array(details, p.dishes.x, p.dishes.y, z, 3, 72);
  fuel_tanks(details, p.tanks.x, p.tanks.y, z, 2);
  if (world.config["vehicles"]["parked"].truthy())
    for (double k = 0; k < 3; k += 1) truck(details, p.helipad.x + vx(14) + k * vx(5), p.helipad.y - vx(6), z + 1, rng);
  auto under = std::make_shared<const Complex>(plan_underground(site, p, rng));
  for (const Rect& q : p.portals) portal_block(lists, q, z, MAT::SIGN_BLUE);
  emit_complex(lists, *under, rng);
  SiteStructure st = finish_structure(std::move(lists));
  st.under = std::move(under);
  return st;
}

// Link-tunnel port: the tram station nearest the linked site.
std::optional<SitePort> port(const World& world, const Site& site, const Point2* toward) {
  const Complex& u = *site.structure(world).under;
  if (!u.tram || u.tram->stations.empty()) return std::nullopt;
  std::optional<SitePort> best;
  for (const TramStation& st : u.tram->stations) {
    const double x = js::round((st.hall.x0 + st.hall.x1) / 2);
    const double y = js::round((st.hall.y0 + st.hall.y1) / 2);
    const double d = toward ? js::hypot(toward->x - x, toward->y - y) : 0;
    if (!best || d < best->d) best = SitePort{x, y, u.tram->z, d};
  }
  return best;
}

}  // namespace

void register_research_complex() {
  district_registry_mut().add({.id = "research",
                               .label = "Research campus",
                               .color = "#6a8aa8",
                               .streets = {.pattern = "none", .block = {{{9999, 9999}, {9999, 9999}}}, .pedestrian_chance = 0, .merge_chance = 0, .local_class = "local"},
                               .block_use = {{"rural", 1}},
                               .lots = {.mode = "none", .width = {0, 0}, .alley_chance = 0},
                               .archetypes = {},
                               .floors = {3, 6},
                               .styles = {{"glass", 2}, {"futurist", 1}, {"concrete", 1}}});
  SiteDef d;
  d.id = "researchComplex";
  d.label = "Research complex";
  d.frequency = 0.25;
  d.min_u = 0;
  d.max_u = 0.05;
  d.size = {340, 280};
  d.margin = 50;
  d.max_relief = 40;
  d.plan = plan;
  d.surface = surface;
  d.ground = ground;
  d.structure = structure;
  d.port = port;
  site_registry_mut().add(std::move(d));
}

}  // namespace svx::city
