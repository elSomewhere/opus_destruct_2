// svx_city — voxel_city sites/mountainBase.js.
#include "sites/mountainBase.hpp"

#include <memory>
#include <optional>
#include <string>

#include "buildings/archetypes.hpp"
#include "buildings/interior/stairs.hpp"
#include "city/districts.hpp"
#include "core/hash.hpp"
#include "core/math.hpp"
#include "sites/complex.hpp"
#include "terrain/terrain.hpp"
#include "voxel/chunk.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

namespace {

constexpr double kTunnelW = 72;   // TUNNEL_W = vx(9)
constexpr double kTunnelH = 60;   // TUNNEL_H = vx(7.5)
constexpr double kApronD = 352;   // APRON_D = vx(44)
constexpr double kApronW = 448;   // APRON_W = vx(56)
constexpr double kCover = 320;    // COVER = vx(40)
constexpr double kPi = 3.141592653589793;  // Math.PI

// The service road (planRoad): a path pad of half width ROAD_HALF descending at ROAD_GRADE in
// steps of ROAD_STEP.
constexpr double kRoadHalf = 28;  // ROAD_HALF = vx(3.5)
constexpr double kRoadGrade = 0.09;
constexpr double kRoadStep = 64;  // ROAD_STEP = vx(8)

// DIRS: u, the unit vector from the cavern's centre toward the portal; v, the lateral one.
struct Dir {
  std::array<double, 2> u, v;
};
const Dir& dir_of(char side) {
  static const Dir kE{{1, 0}, {0, 1}};
  static const Dir kW{{-1, 0}, {0, 1}};
  static const Dir kS{{0, 1}, {1, 0}};
  static const Dir kN{{0, -1}, {1, 0}};
  return side == 'E' ? kE : side == 'W' ? kW : side == 'S' ? kS : kN;
}

char side_of(double dx, double dy) { return dx > 0 ? 'E' : dx < 0 ? 'W' : dy > 0 ? 'S' : 'N'; }

const MountainPlaced& placed_of(const Site& site) { return static_cast<const MountainPlaced&>(*site.placed); }
const MountainBasePlan& plan_of(const Site& site) { return static_cast<const MountainBasePlan&>(*site.plan); }

// Has the world createWorld's water test (JS: `world.isWet && ...`; a World of World.js has none)?
bool has_is_wet(const World& world) { return static_cast<bool>(world.wet_source) || (world.rivers && world.lakes); }

}  // namespace

// ------------------------------------------------------------ frame

MountainFrame::MountainFrame(double cx_, double cy_, char side_) : cx(cx_), cy(cy_), side(side_) {
  const Dir& d = dir_of(side_);
  u = d.u;
  v = d.v;
}

Point2 MountainFrame::to_world(double uu, double vv) const { return {js::round(cx + u[0] * uu + v[0] * vv), js::round(cy + u[1] * uu + v[1] * vv)}; }

Rect MountainFrame::rect(double u0, double v0, double u1, double v1) const {
  const Point2 a = to_world(u0, v0);
  const Point2 b = to_world(u1, v1);
  return {js::min(a.x, b.x), js::min(a.y, b.y), js::max(a.x, b.x), js::max(a.y, b.y)};
}

char MountainFrame::facing(double sign) const { return side_of(v[0] * sign, v[1] * sign); }
char MountainFrame::facing_u(double sign) const { return side_of(u[0] * sign, u[1] * sign); }

// ------------------------------------------------------------ cavern shape

double cavern_r(const Cavern& cav, double x, double y) {
  const double nx = (x - cav.cx) / cav.A;
  const double ny = (y - cav.cy) / cav.B;
  const double r = std::sqrt(std::sqrt(nx * nx * nx * nx + ny * ny * ny * ny));
  return r * (1 + 0.05 * cav.noise->n2(x / 90, y / 90) + 0.02 * cav.noise->n2(x / 23, y / 23));
}

double cavern_ceil(const Cavern& cav, double x, double y, double r) {
  const double t = js::max(0.0, (r - 0.3) / 0.7);
  const double vault = std::sqrt(js::max(0.0, 1 - t * t));
  const double rough = vx(1.5) * cav.noise->n2(x / 40 + 7.1, y / 40 - 3.3);
  return js::round(cav.zf + cav.Hw + (cav.H - cav.Hw) * vault + rough);
}

namespace {

std::shared_ptr<const Cavern> make_cavern(double seed, double cx, double cy, double A, double B, double H, double zf) {
  auto c = std::make_shared<Cavern>();
  c->cx = cx;
  c->cy = cy;
  c->A = A;
  c->B = B;
  c->H = H;
  c->Hw = vx(12);
  c->zf = zf;
  c->noise = std::make_shared<const SimplexNoise>(static_cast<double>(js::to_int32(seed) ^ 0x6a7));
  return c;
}

// Custom volume: carve the vault, lay the floor, line the walls, hang ribs and lights.
SiteVolume cavern_volume(std::shared_ptr<const Cavern> cav, char side) {
  const double pad = vx(4);
  SiteVolume vol;
  vol.bb = {cav->cx - cav->A * 1.1 - pad, cav->cy - cav->B * 1.1 - pad, cav->zf - 1, cav->cx + cav->A * 1.1 + pad, cav->cy + cav->B * 1.1 + pad, cav->zf + cav->H + vx(3)};
  const int along = side == 'E' || side == 'W' ? 0 : 1;
  vol.rasterize = [cav, along](ChunkBuffer& chunk) {
    if (chunk.lod > 2) return;
    const Cavern& c = *cav;
    uint16_t* data = chunk.data.data();
    for (int j = 0; j < kP; ++j)
      for (int i = 0; i < kP; ++i) {
        const double x = chunk.wx(i);
        const double y = chunk.wy(j);
        const double r = cavern_r(c, x, y);
        if (r > 1.04) continue;
        const int col = i + j * kP;
        if (r >= 1) {
          // shotcrete skirt on the lower walls
          for (int k = 0; k < kP; ++k) {
            const double z = chunk.wz(k);
            if (z < c.zf || z > c.zf + vx(5)) continue;
            if (is_solid(data[col + k * kP2])) data[col + k * kP2] = MAT::CONCRETE;
          }
          continue;
        }
        const double ceil = cavern_ceil(c, x, y, r);
        const double a = along == 0 ? x - c.cx : y - c.cy;
        const double b = along == 0 ? y - c.cy : x - c.cx;
        const bool rib = std::fmod(std::fmod(a, 96) + 96, 96) < 3;
        const double la = std::fmod(std::fmod(a + 48, 96) + 96, 96);
        const double lb = std::fmod(std::fmod(b, 72) + 72, 72);
        const bool light = r < 0.85 && la >= 44 && la < 52 && lb < 3;
        const bool hanger = light && la == 48 && lb == 1;
        for (int k = 0; k < kP; ++k) {
          const double z = chunk.wz(k);
          const int idx = col + k * kP2;
          if (z == c.zf || (z < c.zf && z > c.zf - chunk.s)) {
            data[idx] = MAT::FLOOR_CONCRETE;
            continue;
          }
          if (z < c.zf || z > ceil + chunk.s) continue;
          if (z > ceil) {
            // the rock right above the vault
            if (is_solid(data[idx])) data[idx] = rib ? MAT::STEEL_BEAM : MAT::ROCK;
            continue;
          }
          uint16_t m = 0;
          if (rib && (z >= ceil - 2 || r > 0.975))
            m = MAT::STEEL_BEAM;
          else if (light && z >= ceil - 8 && z <= ceil - 7)
            m = MAT::LIGHT_STRIP;
          else if (hanger && z > ceil - 7)
            m = MAT::STEEL_BEAM;
          data[idx] = m;
        }
      }
  };
  return vol;
}

// ------------------------------------------------------------ placement

// Service road down the flank (planRoad): a path pad (a ribbon cut and filled into the slope) that
// leaves the apron from the gate and descends at ROAD_GRADE, each 8 m step turning (up to 90
// degrees) toward the ground that matches its level, so it traverses the flank along the contours
// and folds into hairpins where it must, never closer than 16 m to itself. It ends in the valley,
// where the ground stops falling away, after at most ~5 km. None when it is shorter than 12 points.
std::vector<SitePad> plan_road(const World& world, const Rect& apron, double sx, double sy, const std::array<double, 2>& heading, double z0) {
  const Terrain& T = *world.terrain;
  const bool wet = has_is_wet(world);
  const Rect keep_out{apron.x0 - vx(4), apron.y0 - vx(4), apron.x1 + vx(4), apron.y1 + vx(4)};
  std::vector<SitePathPoint> pts = {{sx, sy, z0}};
  double hx = heading[0];
  double hy = heading[1];
  double x = sx;
  double y = sy;
  double z = z0;
  struct Step {
    double qx, qy, dx, dy, zt, err, cost;
  };
  for (int k = 0; k < 600; ++k) {
    std::optional<Step> best;
    for (double a = -90; a <= 90; a += 10) {
      const double r = (a * kPi) / 180;
      const double dx = hx * js::cos(r) - hy * js::sin(r);
      const double dy = hx * js::sin(r) + hy * js::cos(r);
      const double qx = x + dx * kRoadStep;
      const double qy = y + dy * kRoadStep;
      if (qx >= keep_out.x0 && qx <= keep_out.x1 && qy >= keep_out.y0 && qy <= keep_out.y1) continue;
      bool clash = false;
      for (size_t m = 0; m + 4 < pts.size() && !clash; ++m)
        if (js::hypot(pts[m].x - qx, pts[m].y - qy) < vx(16)) clash = true;
      if (clash) continue;
      const double zt = z - kRoadGrade * kRoadStep;
      const double err = std::fabs(T.sample(qx, qy).h - zt);
      const double cost = err + std::fabs(a) * 0.15;
      if (!best || cost < best->cost) best = Step{qx, qy, dx, dy, zt, err, cost};
    }
    // leaving the apron the road may ride a short fill; later it hugs the ground
    if (!best || best->err > (k < 5 ? vx(12) : vx(4))) break;
    if (wet && world.is_wet(best->qx, best->qy, 4)) break;
    x = best->qx;
    y = best->qy;
    z = best->zt;
    hx = best->dx;
    hy = best->dy;
    pts.push_back({js::round(x), js::round(y), js::round(z)});
  }
  if (pts.size() < 12) return {};
  double x0 = js::kInf, y0 = js::kInf, x1 = -js::kInf, y1 = -js::kInf;
  for (const SitePathPoint& q : pts) {
    x0 = js::min(x0, q.x);
    y0 = js::min(y0, q.y);
    x1 = js::max(x1, q.x);
    y1 = js::max(y1, q.y);
  }
  SitePad pad;
  pad.rect = {x0 - kRoadHalf, y0 - kRoadHalf, x1 + kRoadHalf, y1 + kRoadHalf};
  pad.z = z0;
  pad.path = std::move(pts);
  pad.half = kRoadHalf;
  pad.margin = vx(9);
  pad.round = true;
  pad.road = true;
  return {pad};
}

// Where the road leaves the apron (pickGate): the edge point (outer or lateral) closest to the
// apron's level.
MountainGate pick_gate(const World& world, const MountainFrame& f, double dist, double zf) {
  const Dir& D = dir_of(f.side);
  struct Opt {
    double u, v;
    char side;
    std::array<double, 2> out;
    bool outer;  // (out === DIRS[f.side].u)
  };
  const Opt opts[3] = {
      {dist + kApronD, 0, f.side, D.u, true},
      {dist + kApronD * 0.62, kApronW / 2, f.facing(1), D.v, false},
      {dist + kApronD * 0.62, -kApronW / 2, f.facing(-1), {-D.v[0], -D.v[1]}, false},
  };
  std::optional<MountainGate> best;
  for (const Opt& o : opts) {
    const Point2 p = f.to_world(o.u, o.v);
    const Point2 q = f.to_world(o.u + (o.outer ? vx(10) : 0), o.v + (o.outer ? 0 : js::sign(o.v) * vx(10)));
    const double err = std::fabs(world.terrain->sample(q.x, q.y).h - zf);
    if (!best || err < best->err) best = MountainGate{o.u, o.v, o.side, o.out, p.x, p.y, err};
  }
  return *best;
}

// Find a flank for the portal: walk out from the centre along the four axis directions until the
// ground is low enough that a cavern floored at that level keeps COVER of rock over its vault and
// the tunnel stays under rock; prefer the shortest tunnel whose service road makes it down.
std::shared_ptr<const SitePlaced> place(const World& world, const SiteCandidate& cand) {
  const Rect& rect = cand.rect;
  const double seed = cand.seed;
  const double cx = js::round((rect.x0 + rect.x1) / 2);
  const double cy = js::round((rect.y0 + rect.y1) / 2);
  const Terrain& T = *world.terrain;
  const TerrainSample tc = T.sample(cx, cy);
  if (tc.mountain < 0.35 || tc.u > 0.01) return nullptr;
  const double L = vx(70 + 20 * hash_float(seed, 1));
  const double Wd = vx(55 + 15 * hash_float(seed, 2));
  const double H = vx(48 + 20 * hash_float(seed, 3));
  const double sea = world.config["world"]["seaLevel"].to_number() * 8;
  static const double kVault[13][2] = {{0, 0}, {0.5, 0}, {-0.5, 0}, {0, 0.5}, {0, -0.5}, {0.8, 0.8}, {-0.8, 0.8}, {0.8, -0.8}, {-0.8, -0.8}, {0.95, 0}, {-0.95, 0}, {0, 0.95}, {0, -0.95}};
  struct Cand {
    char side;
    double dist, zf, A, B;
  };
  std::vector<Cand> cands;
  for (const char side : {'N', 'E', 'S', 'W'}) {
    const Dir& d = dir_of(side);
    const double A = d.u[0] != 0 ? L : Wd;
    const double B = d.u[0] != 0 ? Wd : L;
    for (double dist = L + vx(60); dist <= L + vx(620); dist += vx(20)) {
      const double px = cx + d.u[0] * dist;
      const double py = cy + d.u[1] * dist;
      // the apron is levelled at the ground height of its centre: cut into the slope behind (the
      // portal stands in the cut face), fill in front
      const double hp = T.sample(px + (d.u[0] * kApronD) / 2, py + (d.u[1] * kApronD) / 2).h;
      if (hp < sea + vx(10)) break;
      const double zf = js::round(hp);
      // rock over the vault
      bool ok = true;
      for (const auto& fq : kVault) {
        const double rr = js::max(std::fabs(fq[0]), std::fabs(fq[1]));
        const double need = rr < 0.3 ? H : vx(12) + (H - vx(12)) * std::sqrt(js::max(0.0, 1 - js::pow((rr - 0.3) / 0.7, 2)));
        if (T.sample(cx + fq[0] * A, cy + fq[1] * B).h < zf + need + kCover) {
          ok = false;
          break;
        }
      }
      if (!ok) continue;
      // rock over the tunnel (the last stretch before the portal is cut and cover)
      for (double t = L; t < dist - vx(45) && ok; t += vx(25))
        if (T.sample(cx + d.u[0] * t, cy + d.u[1] * t).h < zf + kTunnelH + vx(10)) ok = false;
      if (!ok) continue;
      // the apron: ground falls away (or stays level) beyond the portal
      const double out = T.sample(px + d.u[0] * kApronD, py + d.u[1] * kApronD).h;
      if (out > zf + vx(6)) continue;
      cands.push_back({side, dist, zf, A, B});
      break;
    }
  }
  // the shortest tunnel whose service road makes it down the flank (a stronghold nobody can
  // drive to is no stronghold)
  js::sort(cands, [](const Cand& p, const Cand& q) { return p.dist - q.dist; });
  const Cand* best = nullptr;
  MountainFrame f;
  Rect apron{};
  MountainGate gate{};
  std::vector<SitePad> road;
  for (const Cand& c : cands) {
    f = MountainFrame(cx, cy, c.side);
    apron = f.rect(c.dist, -kApronW / 2, c.dist + kApronD, kApronW / 2);
    gate = pick_gate(world, f, c.dist, c.zf);
    road = plan_road(world, apron, gate.x, gate.y, gate.out, c.zf);
    if (!road.empty()) {
      best = &c;
      break;
    }
  }
  if (!best) return nullptr;
  auto p = std::make_shared<MountainPlaced>();
  p->cav_rect = {cx - best->A, cy - best->B, cx + best->A, cy + best->B};
  p->tunnel = f.rect(L * 0.8, -kTunnelW / 2, best->dist, kTunnelW / 2);
  p->footprint = {js::min(p->cav_rect.x0, apron.x0, p->tunnel.x0), js::min(p->cav_rect.y0, apron.y0, p->tunnel.y0), js::max(p->cav_rect.x1, apron.x1, p->tunnel.x1),
                  js::max(p->cav_rect.y1, apron.y1, p->tunnel.y1)};
  p->pad_z = best->zf;
  SitePad apad;
  apad.rect = apron;
  apad.z = best->zf;
  apad.margin = vx(22);
  apad.round = true;
  p->pads.push_back(apad);
  for (const SitePad& q : road) p->pads.push_back(q);
  p->road = std::move(road);
  p->gate = gate;
  p->side = best->side;
  p->dist = best->dist;
  p->L = L;
  p->Wd = Wd;
  p->H = H;
  p->A = best->A;
  p->B = best->B;
  p->apron = apron;
  return p;
}

// ------------------------------------------------------------ plan

struct ThemeSet {
  std::vector<std::string> themes;
  uint16_t accent;
  std::vector<std::string> kinds;
};
const ThemeSet& theme_set(const std::string& flavor) {
  static const ThemeSet kStronghold{{"military", "power"}, MAT::HAZARD_YELLOW, {"office", "warehouse", "office"}};
  static const ThemeSet kResearch{{"lab", "containment"}, MAT::SIGN_BLUE, {"office", "warehouse", "office"}};
  return flavor == "stronghold" ? kStronghold : kResearch;
}

std::shared_ptr<const SitePlan> plan(const World&, const Site& site) {
  Rng rng(site.seed);
  const MountainPlaced& pl = placed_of(site);
  const double cx = site.center.x;
  const double cy = site.center.y;
  auto p = std::make_shared<MountainBasePlan>();
  p->frame = MountainFrame(cx, cy, pl.side);
  const MountainFrame& f = p->frame;
  const double L = pl.L;
  const double Wd = pl.Wd;
  p->flavor = rng.chance(0.5) ? "stronghold" : "research";
  const ThemeSet& set = theme_set(p->flavor);
  const double zf = site.pad_z;
  p->cavern = make_cavern(site.seed, cx, cy, pl.A, pl.B, pl.H, zf);
  const double inner = 0.74;
  p->road = f.rect(-inner * L, -vx(5), L * 1.02, vx(5));
  // lots on both sides of the road; the portal yard near the tunnel mouth
  p->lots.push_back({set.kinds[0], f.rect(-inner * L, vx(10), -vx(5), inner * Wd), f.facing(-1), "concrete"});
  // the hangar opens toward the tunnel mouth
  p->lots.push_back({set.kinds[1], f.rect(vx(6), vx(10), inner * L * 0.92, inner * Wd), f.facing_u(1), "industrial"});
  {
    const Rect q = f.rect(-inner * L, -inner * Wd, -vx(5), -vx(10));
    static const std::vector<std::string> kStyles = {"concrete", "futurist"};
    p->lots.push_back({set.kinds[2], q, f.facing(1), rng.pick(kStyles)});
  }
  p->yard = f.rect(vx(6), -inner * Wd, inner * L * 0.92, -vx(10));
  // portal block over the stairs down (door on its south side, into the yard)
  const double pw = vx(12);
  const double ph = vx(15);
  const double px0 = js::round((p->yard.x0 + p->yard.x1) / 2 - pw / 2);
  const double py0 = p->yard.y0 + vx(3);
  p->portal = {px0, py0, px0 + pw, py0 + ph};
  p->portals = {p->portal};
  {
    const Point2 h = f.to_world(pl.dist + kApronD * 0.6, kApronW * 0.22);
    p->helipad = {h.x, h.y, vx(9)};
  }
  // the gate gap in the apron fence where the road leaves
  const MountainGate& g = pl.gate;
  p->gate = g.side == 'N' || g.side == 'S' ? Rect{g.x - vx(4), g.y, g.x + vx(4), g.y} : Rect{g.x, g.y - vx(4), g.x, g.y + vx(4)};
  const Rect apron_m{pl.apron.x0 - vx(22), pl.apron.y0 - vx(22), pl.apron.x1 + vx(22), pl.apron.y1 + vx(22)};
  Rect fp = pl.footprint;
  for (const SitePad& q : pl.road) {
    const Rect rm{q.rect.x0 - q.margin, q.rect.y0 - q.margin, q.rect.x1 + q.margin, q.rect.y1 + q.margin};
    fp.x0 = js::min(fp.x0, rm.x0);
    fp.y0 = js::min(fp.y0, rm.y0);
    fp.x1 = js::max(fp.x1, rm.x1);
    fp.y1 = js::max(fp.y1, rm.y1);
  }
  p->side = pl.side;
  p->gate_side = pl.gate.side;
  p->apron = pl.apron;
  p->tunnel = pl.tunnel;
  p->dist = pl.dist;
  p->themes = set.themes;
  p->accent = set.accent;
  p->clear = std::vector<Rect>{apron_m};
  p->bounds = Rect{js::min(fp.x0, apron_m.x0) - vx(8), js::min(fp.y0, apron_m.y0) - vx(8), js::max(fp.x1, apron_m.x1) + vx(8), js::max(fp.y1, apron_m.y1) + vx(8)};
  const double l0 = rng.int_(2, 3);
  const double l1 = rng.int_(2, 3);
  p->levels = {l0, l1};
  return p;
}

// Buildings on the cavern floor (underground lots: they leave the surface above alone).
std::vector<SiteSurface> surface(const World& world, const Site& site) {
  Rng rng(static_cast<double>(js::to_int32(site.seed) ^ 0x51ed));
  const District& district = district_registry().get("stronghold");
  const MountainBasePlan& p = plan_of(site);
  const Cavern& cav = *p.cavern;
  std::vector<SiteSurface> envs;
  for (size_t k = 0; k < p.lots.size(); ++k) {
    const SiteLot& l = p.lots[k];
    Lot lot;
    lot.id = js::cat("C", site.cell.i, "_", site.cell.j, "/", site.type, site.a, "_", site.b, "/l", k);
    lot.rect = l.rect;
    lot.front = l.front;
    lot.frontages = {Frontage{l.front, "local"}};
    lot.corner = false;
    lot.district = "stronghold";
    lot.ground_z = cav.zf;
    lot.block = site.id;
    lot.cell = js::cat("C", site.cell.i, "_", site.cell.j);
    lot.underground = true;
    Rng fork = rng.fork(js::cat("b", k));
    EnvelopeExtra extra;
    extra.u = 0.1;
    extra.core = 0.3;
    extra.ground_z = cav.zf;
    extra.config = &world.config;
    std::optional<Envelope> env = plan_building_envelope_as(lot, l.kind, l.style, district, fork, extra);
    if (!env) continue;
    // must clear the vault everywhere over its footprint
    const Rect& b = env->bounds;
    double low = js::kInf;
    const double pts[5][2] = {{b.x0, b.y0}, {b.x1, b.y0}, {b.x0, b.y1}, {b.x1, b.y1}, {(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2}};
    for (const auto& q : pts) {
      const double r = cavern_r(cav, q[0], q[1]);
      low = js::min(low, r >= 1 ? -js::kInf : cavern_ceil(cav, q[0], q[1], r));
    }
    if (env->top_z + vx(2) < low) envs.push_back({std::move(lot), std::move(*env)});
  }
  return envs;
}

// The apron: concrete with a painted helipad and a lane from the gate to the portal; the road
// and the cut into the flank round it.
void ground(const Site& site, double x, double y, SiteGround& out) {
  const MountainBasePlan& p = plan_of(site);
  const Rect& a = p.apron;
  const SitePad* pad = out.pad;
  if (pad && pad->road) {
    if (out.inside) {
      out.mat = out.path_c < 0.7 && std::fmod(out.path_s, 48) < 24 ? MAT::LINE_YELLOW : out.path_c > kRoadHalf - 1.2 ? MAT::LINE_WHITE : MAT::ASPHALT_WORN;
      out.sub = MAT::GRAVEL;
    } else {
      out.mat = out.natural - out.z > vx(1) ? MAT::ROCK : MAT::GRAVEL;
      out.sub = out.mat;
    }
    return;
  }
  if (x < a.x0 || x > a.x1 || y < a.y0 || y > a.y1) {
    // the cut into the flank shows bare rock; fill and verges are gravel
    if (std::fabs(out.z - out.natural) > 2 || out.z < site.pad_z + vx(2)) {
      out.mat = out.natural - out.z > vx(1) ? MAT::ROCK : MAT::GRAVEL;
      out.sub = out.mat;
    }
    return;
  }
  const SiteHelipad& h = p.helipad;
  const double dh = js::hypot(x - h.x, y - h.y);
  if (dh < h.r) {
    out.mat = helipad_mat(h, x, y, dh);
    out.sub = MAT::CONCRETE;
    return;
  }
  const bool along_x = p.side == 'E' || p.side == 'W';
  const double c = along_x ? y - site.center.y : x - site.center.x;
  if (std::fabs(c) < kTunnelW / 2) {
    out.mat = std::fabs(c) < 0.6 ? MAT::LINE_YELLOW : MAT::ASPHALT;
    out.sub = MAT::GRAVEL;
    return;
  }
  out.mat = (js::to_int32(static_cast<double>(js::sar(x, 5)) + js::sar(y, 5)) & 1) ? MAT::CONCRETE : MAT::CONCRETE_LIGHT;
  out.sub = MAT::CONCRETE;
}

// ------------------------------------------------------------ structures

// Portal headwall with blast doors, the cut-and-cover section and the bored tunnel.
void emit_tunnel(BoxLists& lists, const Site& site) {
  std::vector<SiteBox>& shells = lists.shells;
  std::vector<SiteBox>& carves = lists.carves;
  std::vector<SiteBox>& details = lists.details;
  const MountainBasePlan& p = plan_of(site);
  const MountainFrame& f = p.frame;
  const double z = site.pad_z;
  const double L = placed_of(site).L;
  const double d = p.dist;
  const double hw = kTunnelW / 2;
  auto R = [&](double u0, double v0, double u1, double v1) { return f.rect(u0, v0, u1, v1); };
  auto put = [](std::vector<SiteBox>& list, const Rect& r, double z0, double z1, uint16_t m, int mode = 0) { box(list, r.x0, r.y0, z0, r.x1, r.y1, z1, m, mode); };
  // headwall: a massive concrete face in the cut slope
  put(shells, R(d - vx(4), -vx(15), d, vx(15)), z + 1, z + vx(17), MAT::CONCRETE_DARK);
  put(shells, R(d - vx(4), -vx(16), d + vx(1), vx(16)), z + vx(17) + 1, z + vx(18), MAT::CONCRETE);
  // cut and cover: a concrete box around the tunnel where the cut left air
  put(shells, R(d - vx(46), -hw - vx(1.5), d - vx(4), hw + vx(1.5)), z + 1, z + kTunnelH + vx(1.5), MAT::CONCRETE, 1);
  // bored tunnel with a concrete lining and a chamfered crown
  const double u0 = L * 0.8;
  const double uw = L * 1.08;  // beyond the (rough) cavern wall
  put(shells, R(uw, -hw - 2, d, hw + 2), z - 1, z + kTunnelH + 2, MAT::CONCRETE, 2);
  put(carves, R(u0, -hw, d + 1, hw), z + 1, z + kTunnelH - 4, 0);
  put(carves, R(u0, -hw + 4, d + 1, hw - 4), z + kTunnelH - 3, z + kTunnelH, 0);
  put(details, R(u0, -hw, d, hw), z, z, MAT::ASPHALT);
  put(details, R(u0, -1, d, 0), z, z, MAT::LINE_YELLOW);
  put(details, R(uw, -hw, d, -hw + 3), z + 1, z + 2, MAT::CURB);
  put(details, R(uw, hw - 3, d, hw), z + 1, z + 2, MAT::CURB);
  for (double u = uw + vx(3); u < d - vx(1); u += vx(8)) put(details, R(u, -1, u + vx(3), 0), z + kTunnelH, z + kTunnelH, MAT::LIGHT_STRIP);
  for (double u = uw + vx(6); u < d - vx(6); u += vx(24)) {
    put(details, R(u, -hw + 1, u + 3, -hw + 1), z + vx(2), z + vx(3.5), MAT::SIGNAL_GREEN);
    put(details, R(u, hw - 1, u + 3, hw - 1), z + vx(2), z + vx(3.5), MAT::EMERGENCY_RED);
  }
  // blast doors: two thick leaves parked half open in pockets of the headwall
  put(details, R(d - vx(3), -hw, d - vx(1.5), -hw + vx(2)), z + 1, z + kTunnelH - 4, MAT::METAL_PANEL_DARK);
  put(details, R(d - vx(3), hw - vx(2), d - vx(1.5), hw), z + 1, z + kTunnelH - 4, MAT::METAL_PANEL_DARK);
  for (double k = 0; k < kTunnelH - 6; k += 8) {
    put(details, R(d - vx(1.5), -hw + vx(2) - 2, d - vx(1.4), -hw + vx(2)), z + 1 + k, z + 4 + k, MAT::HAZARD_YELLOW);
    put(details, R(d - vx(1.5), hw - vx(2), d - vx(1.4), hw - vx(2) + 2), z + 1 + k, z + 4 + k, MAT::HAZARD_YELLOW);
  }
  // portal frame, sign and beacons
  put(details, R(d, -hw - 3, d + 1, -hw - 1), z + 1, z + kTunnelH + 2, MAT::HAZARD_YELLOW);
  put(details, R(d, hw + 1, d + 1, hw + 3), z + 1, z + kTunnelH + 2, MAT::HAZARD_YELLOW);
  put(details, R(d, -hw - 3, d + 1, hw + 3), z + kTunnelH + 1, z + kTunnelH + 3, MAT::HAZARD_BLACK);
  put(details, R(d, -vx(6), d + 1, vx(6)), z + kTunnelH + vx(1.5), z + kTunnelH + vx(3), p.accent == MAT::SIGN_BLUE ? MAT::SIGNAGE_BLUE : MAT::SIGNAGE_RED);
  for (const double v : {-vx(13), vx(13)}) put(details, R(d - vx(1), v - 1, d, v + 1), z + vx(18) + 1, z + vx(18) + 4, MAT::SIGNAL_RED);
  // retaining walls along the apron sides
  put(shells, R(d, -kApronW / 2 - vx(1), d + vx(14), -kApronW / 2), z + 1, z + vx(5), MAT::CONCRETE, 1);
  put(shells, R(d, kApronW / 2, d + vx(14), kApronW / 2 + vx(1)), z + 1, z + vx(5), MAT::CONCRETE, 1);
}

// The cavern floor: the main road, the yard with the portal block, tanks and trucks.
void emit_cavern_floor(const World& world, BoxLists& lists, const Site& site, Rng& rng) {
  std::vector<SiteBox>& details = lists.details;
  const MountainBasePlan& p = plan_of(site);
  const double z = p.cavern->zf;
  const Rect& r = p.road;
  const bool parked = world.config["vehicles"]["parked"].truthy();
  box(details, r.x0, r.y0, z, r.x1, r.y1, z, MAT::ASPHALT);
  const bool along_x = p.side == 'E' || p.side == 'W';
  if (along_x)
    box(details, r.x0, site.center.y - 1, z, r.x1, site.center.y, z, MAT::LINE_YELLOW);
  else
    box(details, site.center.x - 1, r.y0, z, site.center.x, r.y1, z, MAT::LINE_YELLOW);
  const Rect& y = p.yard;
  box(details, y.x0, y.y0, z, y.x1, y.y1, z, MAT::FLOOR_EPOXY);
  for (double x = y.x0; x <= y.x1; x += 4) box(details, x, y.y0, z, x + 1, y.y0, z, (js::sar(x, 2) & 1) ? MAT::HAZARD_BLACK : MAT::HAZARD_YELLOW);
  portal_block(lists, p.portal, z, p.accent);
  // fuel tanks and a truck park wherever the yard has room (the portal door stays clear)
  const Rect& q = p.portal;
  std::vector<Rect> taken = {{q.x0 - vx(2), q.y0 - vx(2), q.x1 + vx(2), q.y1 + vx(9)}};
  auto spot = [&](double w, double h) -> std::optional<Rect> {
    for (double yy = y.y0 + vx(2); yy + h <= y.y1 - vx(2); yy += 8)
      for (double xx = y.x0 + vx(2); xx + w <= y.x1 - vx(2); xx += 8) {
        const Rect s{xx, yy, xx + w, yy + h};
        bool hit = false;
        for (const Rect& t : taken)
          if (s.x0 <= t.x1 && t.x0 <= s.x1 && s.y0 <= t.y1 && t.y0 <= s.y1) {
            hit = true;
            break;
          }
        if (hit) continue;
        taken.push_back({s.x0 - vx(1.5), s.y0 - vx(1.5), s.x1 + vx(1.5), s.y1 + vx(1.5)});
        return s;
      }
    return std::nullopt;
  };
  const std::optional<Rect> tanks = spot(vx(9) + 44, 44);
  if (tanks) fuel_tanks(details, tanks->x0 + 22, tanks->y0 + 22, z, 2);
  for (int k = 0; k < 3; ++k) {
    const std::optional<Rect> t = spot(18, 54);
    if (t && parked) truck(details, t->x0, t->y0, z + 1, rng);
  }
  // floodlight masts at the road ends
  const double masts[2][2] = {{r.x0 + 2, r.y0 + 2}, {r.x1 - 2, r.y1 - 2}};
  for (const auto& m : masts) {
    const double x = m[0];
    const double yy = m[1];
    box(details, x - 1, yy - 1, z + 1, x + 1, yy + 1, z + vx(9), MAT::STEEL_BEAM);
    box(details, x - 4, yy - 4, z + vx(9) + 1, x + 4, yy + 4, z + vx(9) + 2, MAT::LAMP_LIGHT);
  }
}

// Apron dressing: a fence with a gate on the downhill side, a guard booth, a truck.
void emit_apron(const World& world, BoxLists& lists, const Site& site, Rng& rng) {
  std::vector<SiteBox>& details = lists.details;
  const MountainBasePlan& p = plan_of(site);
  const double z = site.pad_z;
  const Rect& a = p.apron;
  // (the side facing the mountain stays open: the portal stands in the cut face)
  FenceSkip skip;
  const char open = p.side == 'N' ? 'S' : p.side == 'S' ? 'N' : p.side == 'E' ? 'W' : 'E';
  skip.N = open == 'N';
  skip.S = open == 'S';
  skip.W = open == 'W';
  skip.E = open == 'E';
  fence(details, a, z, p.gate_side, p.gate, 22, skip);
  gate_booth(details, p.gate, p.gate_side, z);
  const Point2 t = p.frame.to_world(p.dist + vx(10), -kApronW * 0.3);
  if (world.config["vehicles"]["parked"].truthy()) truck(details, t.x, t.y, z + 1, rng);
}

// ------------------------------------------------------------ underground

// Two sectors under the cavern (split across the tunnel axis), a tram below, stairs from the yard.
Complex plan_underground(const Site& site, Rng& rng) {
  const MountainBasePlan& p = plan_of(site);
  const MountainPlaced& pl = placed_of(site);
  const MountainFrame& f = p.frame;
  const double half_l = pl.L * 1.05;
  const double half_w = pl.Wd * 1.05;
  const std::vector<Rect> quads = {f.rect(vx(6), -half_w, half_l, half_w), f.rect(-half_l, -half_w, -vx(6), half_w)};
  const double top = p.cavern->zf;
  ComplexSpec spec;
  for (size_t k = 0; k < quads.size(); ++k) {
    ComplexSectorSpec s;
    s.bounds = quads[k];
    s.z0 = top - vx(18) - static_cast<double>(k) * vx(8);
    s.levels = p.levels[k];
    s.theme = p.themes[k];
    s.rooms = std::array<double, 2>{8, 12};
    spec.sectors.push_back(s);
  }
  double deepest = js::kInf;
  for (const ComplexSectorSpec& s : spec.sectors) deepest = js::min(deepest, s.z0 - (s.levels - 1) * vx(15));
  const StairDims sd = stair_dims(30);
  const Rect& q = p.portal;
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
  e.z_top = top + 1;
  e.sector = sector;
  e.dir = -1;
  e.open_top = true;
  spec.entries.push_back(e);
  spec.tram_z = deepest - vx(15);
  return plan_complex(rng, spec);
}

SiteStructure structure(const World& world, const Site& site) {
  Rng rng(static_cast<double>(js::to_int32(site.seed) ^ 0x77));
  const MountainBasePlan& p = plan_of(site);
  BoxLists lists;
  lists.custom.push_back(cavern_volume(p.cavern, p.side));
  emit_tunnel(lists, site);
  emit_apron(world, lists, site, rng);
  auto under = std::make_shared<const Complex>(plan_underground(site, rng));
  emit_cavern_floor(world, lists, site, rng);
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

void register_mountain_base() {
  district_registry_mut().add({.id = "stronghold",
                               .label = "Mountain stronghold",
                               .color = "#7a6f64",
                               .streets = {.pattern = "none", .block = {{{9999, 9999}, {9999, 9999}}}, .pedestrian_chance = 0, .merge_chance = 0, .local_class = "local"},
                               .block_use = {{"rural", 1}},
                               .lots = {.mode = "none", .width = {0, 0}, .alley_chance = 0},
                               .archetypes = {},
                               .floors = {3, 5},
                               .styles = {{"concrete", 2}, {"industrial", 1}}});
  SiteDef d;
  d.id = "mountainBase";
  d.label = "Mountain stronghold";
  d.frequency = 0.3;
  d.min_u = 0;
  d.max_u = 0.02;
  d.size = {190, 190};
  d.margin = 30;
  d.place = place;
  d.plan = plan;
  d.surface = surface;
  d.ground = ground;
  d.structure = structure;
  d.port = port;
  site_registry_mut().add(std::move(d));
}

}  // namespace svx::city
