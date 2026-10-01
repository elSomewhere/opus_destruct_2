// svx_city — voxel_city sites/militaryBase.js.
#include "sites/militaryBase.hpp"

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

constexpr double kFirstDepth = 112;  // vx(14): the first level's depth

const MilitaryBasePlan& plan_of(const Site& site) { return static_cast<const MilitaryBasePlan&>(*site.plan); }

std::shared_ptr<const SitePlan> plan_base(const World& world, const Site& site) {
  Rng rng(site.seed);
  const Rect& r = site.rect;
  const SiteGate g = plan_gate(world, site);
  auto p = std::make_shared<MilitaryBasePlan>();
  // internal ring road
  const double ring_in = vx(12);
  const double ring_w = vx(7);
  const Rect ring{r.x0 + ring_in, r.y0 + ring_in, r.x1 - ring_in, r.y1 - ring_in};
  // building pads inside the ring (canonical: front faces the ring road)
  const Rect inner{ring.x0 + ring_w + vx(4), ring.y0 + ring_w + vx(4), ring.x1 - ring_w - vx(4), ring.y1 - ring_w - vx(4)};
  const double iw = inner.x1 - inner.x0;
  const double ih = inner.y1 - inner.y0;
  auto lot_rect = [&](double fx0, double fy0, double fx1, double fy1) {
    return Rect{js::round(inner.x0 + iw * fx0), js::round(inner.y0 + ih * fy0), js::round(inner.x0 + iw * fx1), js::round(inner.y0 + ih * fy1)};
  };
  p->lots.push_back({"office", lot_rect(0.0, 0.0, 0.3, 0.3), 'N', ""});
  p->lots.push_back({"walkup", lot_rect(0.36, 0.0, 0.62, 0.22), 'N', ""});
  p->lots.push_back({"warehouse", lot_rect(0.0, 0.45, 0.42, 1.0), 'W', ""});
  p->lots.push_back({"warehouse", lot_rect(0.48, 0.55, 0.78, 1.0), 'S', ""});
  p->bunker = lot_rect(0.72, 0.05, 0.98, 0.32);
  p->helipad = {js::round(inner.x0 + iw * 0.62), js::round(inner.y0 + ih * 0.38), vx(10)};
  p->apron = lot_rect(0.44, 0.26, 0.98, 0.5);
  p->gate_side = g.gate_side;
  p->gate = g.gate;
  p->drive = g.drive;
  p->ring = ring;
  p->ring_w = ring_w;
  p->inner = inner;
  const Rect& b = site.blend;
  p->bounds = Rect{js::min(b.x0, g.drive.x0), js::min(b.y0, g.drive.y0), js::max(b.x1, g.drive.x1), js::max(b.y1, g.drive.y1)};
  p->levels = rng.int_(2, 3);
  return p;
}

// Surface envelopes for the cell plan (full interiors via archetypes).
std::vector<SiteSurface> surface(const World& world, const Site& site) {
  Rng rng(static_cast<double>(js::to_int32(site.seed) ^ 0x51ed));
  const District& district = district_registry().get("military");
  const MilitaryBasePlan& p = plan_of(site);
  std::vector<SiteSurface> envs;
  for (size_t k = 0; k < p.lots.size(); ++k) {
    const SiteLot& l = p.lots[k];
    Lot lot;
    lot.id = js::cat("C", site.cell.i, "_", site.cell.j, "/", site.type, site.a, "_", site.b, "/l", k);
    lot.rect = l.rect;
    lot.front = l.front;
    lot.frontages = {Frontage{l.front, "local"}};
    lot.corner = false;
    lot.district = "military";
    lot.ground_z = site.pad_z;
    lot.block = site.id;
    lot.cell = js::cat("C", site.cell.i, "_", site.cell.j);
    Rng fork = rng.fork(js::cat("b", k));
    EnvelopeExtra extra;
    extra.u = 0;
    extra.core = 0.3;
    extra.ground_z = site.pad_z;
    extra.config = &world.config;
    std::optional<Envelope> env = plan_building_envelope_as(lot, l.kind, "concrete", district, fork, extra);
    if (env) envs.push_back({std::move(lot), std::move(*env)});
  }
  return envs;
}

void ground(const Site& site, double x, double y, SiteGround& out) {
  const MilitaryBasePlan& p = plan_of(site);
  auto in_rect = [&](const Rect& q) { return x >= q.x0 && x <= q.x1 && y >= q.y0 && y <= q.y1; };
  const Rect& drive = *p.drive;
  if (in_rect(drive)) {
    const double c = p.gate_side == 'N' || p.gate_side == 'S' ? x - (drive.x0 + drive.x1) / 2 : y - (drive.y0 + drive.y1) / 2;
    out.mat = std::fabs(c) < 0.6 ? MAT::LINE_YELLOW : MAT::ASPHALT_WORN;
    out.sub = MAT::GRAVEL;
    return;
  }
  if (!out.inside) return;
  const Rect& r = p.ring;
  const bool on_ring = x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1 && (x < r.x0 + p.ring_w || x > r.x1 - p.ring_w || y < r.y0 + p.ring_w || y > r.y1 - p.ring_w);
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
  if (in_rect(p.apron) || in_rect(p.bunker)) {
    out.mat = (js::to_int32(static_cast<double>(js::sar(x, 5)) + js::sar(y, 5)) & 1) ? MAT::CONCRETE : MAT::CONCRETE_LIGHT;
    out.sub = MAT::CONCRETE;
    return;
  }
  for (const SiteLot& l : p.lots) {
    const Rect& q = l.rect;
    if (x >= q.x0 - vx(6) && x <= q.x1 + vx(6) && y >= q.y0 - vx(6) && y <= q.y1 + vx(6)) {
      out.mat = MAT::CONCRETE;
      out.sub = MAT::CONCRETE;
      return;
    }
  }
  out.mat = MAT::GRASS_DRY;
  out.sub = MAT::DIRT;
}

// ------------------------------------------------------------ underground

// The base's complex: one military sector under the compound, entered from the bunker.
Complex plan_underground(const Site& site, const MilitaryBasePlan& p, Rng& rng) {
  const Rect& r = site.rect;
  const Rect bounds{r.x0 + vx(8), r.y0 + vx(8), r.x1 - vx(8), r.y1 - vx(8)};
  const StairDims sd = stair_dims(30);
  const Rect& bk = p.bunker;
  // entrance stair centred in the bunker, near end facing the blast door (south)
  const double ex0 = js::round((bk.x0 + bk.x1 - sd.W) / 2);
  const Rect e_rect{ex0, bk.y0 + 12, ex0 + sd.W - 1, bk.y0 + 12 + sd.L - 1};
  ComplexSpec spec;
  ComplexSectorSpec s;
  s.bounds = bounds;
  s.z0 = site.pad_z + 1 - kFirstDepth;
  s.levels = p.levels;
  s.theme = "military";
  s.rooms = std::array<double, 2>{9, 14};
  spec.sectors.push_back(s);
  ComplexEntrySpec e;
  e.rect = e_rect;
  e.z_top = site.pad_z + 1;
  e.sector = 0;
  e.dir = -1;
  e.open_top = true;
  spec.entries.push_back(e);
  return plan_complex(rng, spec);
}

// ------------------------------------------------------------ structures

// Surface props, the bunker and the underground complex as world boxes (made once per site).
SiteStructure structure(const World& world, const Site& site) {
  Rng rng(static_cast<double>(js::to_int32(site.seed) ^ 0x77));
  const MilitaryBasePlan& p = plan_of(site);
  BoxLists lists;
  std::vector<SiteBox>& details = lists.details;
  const Rect& r = site.rect;
  const double z = site.pad_z;
  fence(details, r, z, p.gate_side, p.gate);
  gate_booth(details, p.gate, p.gate_side, z);
  watchtowers(details, r, z);
  radar_mast(details, p.apron.x1 - vx(4), p.apron.y0 + vx(4), z);
  fuel_tanks(details, p.apron.x0 + vx(8), p.apron.y1 - vx(6), z);
  if (world.config["vehicles"]["parked"].truthy())
    for (double k = 0; k < 4; k += 1) truck(details, p.apron.x0 + vx(6) + k * vx(5), p.apron.y0 + vx(4), z + 1, rng);
  // bunker over the entrance shaft, then the complex below
  auto under = std::make_shared<const Complex>(plan_underground(site, p, rng));
  portal_block(lists, p.bunker, z);
  emit_complex(lists, *under, rng);
  SiteStructure st = finish_structure(std::move(lists));
  st.under = std::move(under);
  return st;
}

// Link-tunnel port: the anteroom of the deepest level.
std::optional<SitePort> port(const World& world, const Site& site, const Point2*) {
  const Complex& u = *site.structure(world).under;
  const ComplexLevel& lv = u.levels.back();
  const ComplexRoom& q = lv.rooms[0];
  return SitePort{js::round((q.x0 + q.x1) / 2), js::round((q.y0 + q.y1) / 2), lv.zf, js::kNaN};
}

}  // namespace

void register_military_base() {
  district_registry_mut().add({.id = "military",
                               .label = "Military",
                               .color = "#6f7d5a",
                               .streets = {.pattern = "none", .block = {{{9999, 9999}, {9999, 9999}}}, .pedestrian_chance = 0, .merge_chance = 0, .local_class = "local"},
                               .block_use = {{"rural", 1}},
                               .lots = {.mode = "none", .width = {0, 0}, .alley_chance = 0},
                               .archetypes = {},
                               .floors = {2, 3},
                               .styles = {{"concrete", 1}}});
  SiteDef d;
  d.id = "militaryBase";
  d.label = "Military research base";
  d.frequency = 0.45;
  d.min_u = 0;
  d.max_u = 0.04;
  d.size = {230, 180};
  d.margin = 45;
  d.max_relief = 30;
  d.plan = plan_base;
  d.surface = surface;
  d.ground = ground;
  d.structure = structure;
  d.port = port;
  site_registry_mut().add(std::move(d));
}

}  // namespace svx::city
