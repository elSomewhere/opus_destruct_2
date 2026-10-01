// svx_city — voxel_city city/industry.js.
#include "city/industry.hpp"

#include <initializer_list>

#include "voxel/materials.hpp"

namespace svx::city {

namespace {

constexpr double CL = 98;  // container length (12.2 m)
constexpr double CW = 20;  // width (2.44 m)
constexpr double CH = 21;  // height (2.6 m)

IndustryLayout tank_farm(const IndustrySpace& space) {
  Rng rng = Rng::from(0x7a4f, space.id);
  const Rect& r = space.rect;
  const double m = vx(6);
  const double R = js::round(vx(rng.float_(8, 13)));
  const double cell = 2 * R + vx(10);
  const double nx = js::max(1, std::floor((r.x1 - r.x0 - 2 * m) / cell));
  const double ny = js::max(1, std::floor((r.y1 - r.y0 - 2 * m) / cell));
  const double x0 = js::round((r.x0 + r.x1 - nx * cell) / 2);
  const double y0 = js::round((r.y0 + r.y1 - ny * cell) / 2);
  IndustryLayout L;
  L.kind = "tankFarm";
  L.cell = cell;
  L.x0 = x0;
  L.y0 = y0;
  L.nx = nx;
  L.ny = ny;
  // the last column (if there are several) is the process unit
  if (nx >= 3) L.process = Rect{x0 + (nx - 1) * cell, y0, x0 + nx * cell - 1, y0 + ny * cell - 1};
  for (double j = 0; j < ny; j += 1)
    for (double i = 0; i < (L.process ? nx - 1 : nx); i += 1) {
      if (rng.chance(0.08)) continue;
      const double cx = x0 + i * cell + js::round(cell / 2);
      const double cy = y0 + j * cell + js::round(cell / 2);
      const double tr = R - (rng.chance(0.3) ? vx(2) : 0);
      const double th = js::round(vx(rng.float_(9, 15)));
      L.tanks.push_back({cx, cy, tr, th, js::round(cell / 2) - vx(3)});
    }
  // pipe racks along the lanes between tank rows
  for (double j = 1; j < ny; j += 1) L.racks.push_back({x0 + vx(2), x0 + nx * cell - vx(2), y0 + j * cell - vx(2)});
  if (L.process) {
    const Rect& process = *L.process;
    const double px = js::round((process.x0 + process.x1) / 2);
    // distillation columns spread along the unit, in pairs across a central rack
    const double len = process.y1 - process.y0;
    const double n = js::max(2, js::min(8, std::floor(len / vx(16))));
    const double gap = (len - vx(20)) / js::max(1, n - 1);
    for (double k = 0; k < n; k += 1) {
      const double cr = js::round(vx(rng.float_(1.6, 2.8)));
      const double ch = js::round(vx(rng.float_(22, 44)));
      L.columns.push_back({px + ((js::to_int32(k) & 1) ? vx(6) : -vx(6)), js::round(process.y0 + vx(8) + k * gap), cr, ch});
    }
    L.racks_v = IndustryRackV{px, process.y0 + vx(4), process.y1 - vx(14)};
    L.flare = IndustryFlare{process.x1 - vx(4), process.y1 - vx(6), js::round(vx(rng.float_(40, 55)))};
  }
  return L;
}

IndustryLayout container_yard(const IndustrySpace& space) {
  Rng rng = Rng::from(0x5c0f, space.id);
  const Rect& r = space.rect;
  const double m = vx(5);
  const double lane = vx(10);
  const double rows = 6;
  const double block_w = rows * (CW + 2);
  IndustryLayout L;
  L.kind = "containerYard";
  L.lane = lane;
  for (double y = r.y0 + m; y + block_w <= r.y1 - m; y += block_w + lane) {
    ContainerBlock blk;
    for (double x = r.x0 + m; x + CL <= r.x1 - m; x += CL + 6)
      for (double k = 0; k < rows; k += 1) {
        const double n = rng.chance(0.12) ? 0 : rng.int_(1, 4);
        if (js::truthy(n)) {
          const double seed = rng.int_(0, 1 << 30);
          blk.stacks.push_back({x, y + k * (CW + 2), n, seed});
        }
      }
    const double span = block_w + vx(4);
    const double len = r.x1 - m - (r.x0 + m);
    if (len > vx(40)) blk.cranes.push_back({r.x0 + m + js::round(rng.float_(0.2, 0.8) * (len - vx(8))), y - vx(2), span});
    blk.y0 = y;
    blk.y1 = y + block_w - 1;
    L.blocks.push_back(std::move(blk));
  }
  return L;
}

}  // namespace

const IndustryLayout* industry_layout(const IndustrySpace& space) {
  const std::optional<IndustryLayout>& L = space.layout.get([&]() -> std::optional<IndustryLayout> {
    if (space.kind == "tankFarm") return tank_farm(space);
    if (space.kind == "containerYard") return container_yard(space);
    return std::nullopt;
  });
  return L ? &*L : nullptr;
}

bool industry_surface(const IndustrySpace& space, double x, double y, uint16_t& mat) {
  const IndustryLayout* L = industry_layout(space);
  if (!L) return false;
  const Rect& r = space.rect;
  const double edge = js::min(x - r.x0, r.x1 - x, y - r.y0, r.y1 - y);
  if (edge < vx(2)) {
    mat = MAT::GRAVEL;
    return true;
  }
  if (L->kind == "tankFarm") {
    for (const IndustryTank& t : L->tanks) {
      const double d = js::hypot(x - t.x, y - t.y);
      if (d < t.r + vx(1)) {
        mat = MAT::CONCRETE;
        return true;
      }
      if (std::fabs(x - t.x) < t.pad && std::fabs(y - t.y) < t.pad) {
        mat = MAT::GRAVEL;
        return true;
      }
    }
    if (L->process && x >= L->process->x0 && x <= L->process->x1 && y >= L->process->y0 && y <= L->process->y1) {
      mat = (js::to_int32(static_cast<double>(js::sar(x, 4)) + js::sar(y, 4)) & 1) ? MAT::CONCRETE : MAT::CONCRETE_DARK;
      return true;
    }
    mat = MAT::ASPHALT_WORN;
    return true;
  }
  // container yard: concrete stacking blocks, asphalt lanes with markings
  for (const ContainerBlock& b : L->blocks) {
    if (y >= b.y0 - 2 && y <= b.y1 + 2) {
      mat = std::fmod(y - b.y0, CW + 2) >= CW ? MAT::LINE_YELLOW : MAT::CONCRETE;
      return true;
    }
  }
  mat = (js::sar(x, 3) & 7) == 0 && std::fabs(std::fmod(y - r.y0, L->lane + 6 * (CW + 2)) - 3 * (CW + 2)) > 0 ? MAT::ASPHALT : MAT::ASPHALT_WORN;
  return true;
}

// ------------------------------------------------------------ prefabs

namespace {

using Boxes = std::vector<PrefabBox>;

inline PrefabBox B(double a0, double a1, double b0, double b1, double z0, double z1, uint16_t m) { return {a0, a1, b0, b1, z0, z1, m}; }
inline double num(const std::optional<double>& v) { return v ? *v : js::kNaN; }
inline uint16_t pick(Rng& rng, std::initializer_list<uint16_t> items) {
  return items.begin()[static_cast<size_t>(std::floor(rng.next() * static_cast<double>(items.size())))];
}

// A cylinder of radius r (voxels) as full-height slabs, one per row.
void cylinder(Boxes& out, double r, double z0, double z1, uint16_t m, double ca = 0, double cb = 0) {
  for (double b = -r; b <= r; b += 1) {
    const double w = std::floor(std::sqrt(js::max(0, r * r - b * b)));
    out.push_back(B(ca - w, ca + w, cb + b, cb + b, z0, z1, m));
  }
}

Boxes silo(Rng& rng, const PropOpts& o) {
  Boxes out;
  const double r = num(o.r);
  const double h = num(o.h);
  cylinder(out, r, 0, h, rng.chance(0.3) ? MAT::CONCRETE_LIGHT : MAT::SILO_STEEL);
  for (double k = 1; k <= std::ceil(r * 0.6); k += 1) cylinder(out, js::round(std::sqrt(js::max(0, r * r - js::pow(k * 1.6, 2)))), h + k, h + k, MAT::SILO_STEEL);
  out.push_back(B(-1, 1, -r - 1, -r - 1, 0, h, MAT::LADDER));
  for (const double z : {js::round(h * 0.3), js::round(h * 0.7)}) cylinder(out, r + 1, z, z, MAT::METAL_PANEL_DARK);
  return out;
}

Boxes sts_crane(Rng&, const PropOpts&) {
  // ship-to-shore gantry: legs on the quay (b <= 0), boom far out over the water (+b)
  Boxes out;
  const double H = vx(32);
  const double hw = vx(7);
  for (const double a : {-hw, hw}) {
    out.push_back(B(a - 2, a + 2, -vx(16), -vx(16) + 3, 0, H, MAT::SIGNAL_RED));
    out.push_back(B(a - 2, a + 2, -3, 0, 0, H, MAT::SIGNAL_RED));
    out.push_back(B(a - 2, a + 2, -vx(16), 0, H - 3, H, MAT::SIGNAL_RED));
    out.push_back(B(a - 2, a + 2, -vx(16), 0, vx(10), vx(10) + 2, MAT::SIGNAL_RED));
  }
  out.push_back(B(-hw, hw, -vx(16), -vx(16) + 3, H - 3, H, MAT::SIGNAL_RED));
  out.push_back(B(-hw, hw, -3, 0, H - 3, H, MAT::SIGNAL_RED));
  // boom and back-reach, machinery house, trolley and spreader
  out.push_back(B(-vx(2), vx(2), -vx(30), vx(38), H + 1, H + 5, MAT::PANEL_WHITE));
  out.push_back(B(-vx(3), vx(3), -vx(24), -vx(14), H + 6, H + vx(3.5), MAT::PANEL_WHITE));
  out.push_back(B(-vx(1), vx(1), -vx(6), -vx(4), H + 6, H + vx(9), MAT::SIGNAL_RED));
  out.push_back(B(-vx(2), vx(2), vx(14), vx(17), H - 4, H, MAT::METAL_PANEL_DARK));
  out.push_back(B(-1, 0, vx(15), vx(15) + 1, vx(14), H - 5, MAT::METAL_BLACK));
  out.push_back(B(-vx(3), vx(3), vx(14), vx(17), vx(13), vx(14) - 1, MAT::HAZARD_YELLOW));
  out.push_back(B(-1, 1, -1, 1, H + vx(9), H + vx(9) + 2, MAT::SIGNAL_AMBER));
  return out;
}

Boxes cargo_ship(Rng&, const PropOpts& o) {
  // a container feeder alongside the quay: hull, stacked containers, bridge aft
  Rng r(o.seed.value_or(1));
  const double len = vx(r.float_(70, 110));
  const double beam = vx(r.float_(13, 17));
  const double hl = len / 2;
  const double hb = beam / 2;
  const uint16_t hull = pick(r, {MAT::CONTAINER_BLUE, MAT::PAINT_DARK, MAT::CONTAINER_RED, MAT::CONTAINER_GREEN});
  Boxes out;
  // bow tapers over the front 12 m
  for (double a = -hl; a <= hl; a += 4) {
    const double t = js::max(0, (a - (hl - vx(12))) / vx(12));
    const double w = js::round(hb * (1 - 0.8 * t * t));
    out.push_back(B(a, js::min(hl, a + 3), -w, w, 0, vx(3.5), hull));
    out.push_back(B(a, js::min(hl, a + 3), -w, w, vx(3.5) + 1, vx(3.5) + 2, MAT::PAINT_GRAY));
  }
  const uint16_t colors[] = {MAT::CONTAINER_RED, MAT::CONTAINER_BLUE, MAT::CONTAINER_GREEN, MAT::CONTAINER_ORANGE, MAT::CORRUGATED_RUST};
  for (double a = -hl + vx(18); a + 98 < hl - vx(14); a += 102)
    for (double b = -hb + 4; b + 20 < hb - 3; b += 22) {
      const double n = r.int_(1, 4);
      for (double k = 0; k < n; k += 1) out.push_back(B(a, a + 97, b, b + 19, vx(3.5) + 3 + k * 21, vx(3.5) + 22 + k * 21, r.pick(colors)));
    }
  // bridge house at the stern
  out.push_back(B(-hl + vx(3), -hl + vx(12), -hb + 4, hb - 4, vx(3.5) + 3, vx(14), MAT::PANEL_WHITE));
  out.push_back(B(-hl + vx(3), -hl + vx(12), -hb + 4, hb - 4, vx(12), vx(13), MAT::GLASS_TINT));
  out.push_back(B(-hl + vx(6), -hl + vx(8), -2, 2, vx(14) + 1, vx(18), MAT::PAINT_DARK));
  return out;
}

Boxes garage_row(Rng& rng, const PropOpts& o) {
  // lock-up garages: corrugated boxes side by side, doors facing +b
  Boxes out;
  const double n = num(o.n);
  for (double k = 0; k < n; k += 1) {
    const double a = k * 24;
    const uint16_t door = pick(rng, {MAT::DOOR_METAL, MAT::CORRUGATED_BLUE, MAT::CORRUGATED_RUST, MAT::DOOR_GREEN});
    out.push_back(B(a + 3, a + 20, 47, 47, 1, 16, door));
    out.push_back(B(a, a + 23, 0, 47, 20, 20, MAT::ROOF_MEMBRANE));
    out.push_back(B(a, a + 23, 0, 47, 0, 19, pick(rng, {MAT::CORRUGATED, MAT::CORRUGATED_RUST, MAT::CINDERBLOCK, MAT::CORRUGATED})));
  }
  return out;
}

Boxes round_bale(Rng&, const PropOpts&) {
  // a round hay bale lying on its side (a cylinder along a)
  Boxes out;
  const double R = 5;
  for (double z = 0; z <= 2 * R; z += 1) {
    const double w = js::round(std::sqrt(js::max(0, R * R - js::pow(z - R, 2))));
    out.push_back(B(-5, 5, -w, w, z, z, MAT::HAY));
  }
  return out;
}

Boxes storage_tank(Rng& rng, const PropOpts& o) {
  Boxes out;
  const double r = num(o.r);
  const double h = num(o.h);
  cylinder(out, r, 0, h, MAT::TANK_WHITE);
  // shallow cone roof, a band and a caged ladder with a top railing
  for (double k = 1; k <= 4; k += 1) cylinder(out, js::round(r * (1 - k * 0.2)), h + k, h + k, MAT::METAL_PANEL);
  cylinder(out, r + 1, js::round(h * 0.55), js::round(h * 0.55) + 2, rng.chance(0.5) ? MAT::HAZARD_YELLOW : MAT::SIGN_BLUE);
  out.push_back(B(-2, 2, -r - 2, -r - 1, 0, h, MAT::LADDER));
  out.push_back(B(-3, 3, -r - 3, -r - 3, 6, h + 6, MAT::RAILING));
  return out;
}

Boxes bund(Rng&, const PropOpts& o) {
  const double w = num(o.hw);
  return {B(-w, w, -w, -w + 1, 0, 7, MAT::CONCRETE), B(-w, w, w - 1, w, 0, 7, MAT::CONCRETE), B(-w, -w + 1, -w, w, 0, 7, MAT::CONCRETE),
          B(w - 1, w, -w, w, 0, 7, MAT::CONCRETE)};
}

Boxes pipe_rack(Rng&, const PropOpts& o) {
  Boxes out;
  const double len = num(o.len);
  for (double a = 0; a <= len; a += vx(6)) {
    out.push_back(B(a, a + 1, -8, -7, 0, 52, MAT::STEEL_BEAM));
    out.push_back(B(a, a + 1, 7, 8, 0, 52, MAT::STEEL_BEAM));
    out.push_back(B(a, a + 1, -8, 8, 52, 53, MAT::STEEL_BEAM));
    out.push_back(B(a, a + 1, -8, 8, 36, 37, MAT::STEEL_BEAM));
  }
  for (const double b : {-6.0, -2.0, 2.0, 6.0}) out.push_back(B(0, len, b - 1, b, 54, 55, b > 0 ? MAT::PIPE : MAT::STEEL_RUST));
  for (const double b : {-5.0, 0.0, 5.0}) out.push_back(B(0, len, b - 1, b, 38, 39, MAT::PIPE));
  return out;
}

Boxes process_column(Rng&, const PropOpts& o) {
  Boxes out;
  const double r = num(o.r);
  const double h = num(o.h);
  cylinder(out, r, 0, h, MAT::METAL_CHROME);
  for (double z = vx(6); z < h - 4; z += vx(6)) cylinder(out, r + 5, z, z, MAT::GRATE_STEEL);
  out.push_back(B(-1, 1, -r - 3, -r - 2, 0, h, MAT::LADDER));
  out.push_back(B(-1, 1, -1, 1, h + 1, h + 6, MAT::PIPE));
  out.push_back(B(-1, 1, -1, 1, h + 7, h + 7, MAT::SIGNAL_RED));
  return out;
}

Boxes flare_stack(Rng&, const PropOpts& o) {
  const double h = num(o.h);
  Boxes out = {B(-3, 3, -3, 3, 0, 4, MAT::CONCRETE), B(-1, 1, -1, 1, 5, h, MAT::STEEL_RUST)};
  const double legs[4][2] = {{-10, -10}, {10, -10}, {-10, 10}, {10, 10}};
  for (const auto& l : legs) {
    const double da = l[0], db = l[1];
    for (double k = 0; k < 10; k += 1)
      out.push_back(B(js::round(da * (1 - k / 10)), js::round(da * (1 - k / 10)), js::round(db * (1 - k / 10)), js::round(db * (1 - k / 10)),
                      js::round((k * h) / 14), js::round(((k + 1) * h) / 14), MAT::STEEL_BEAM));
  }
  out.push_back(B(-2, 2, -2, 2, h + 1, h + 2, MAT::METAL_BLACK));
  out.push_back(B(-1, 1, -1, 1, h + 3, h + 7, MAT::LAMP_LIGHT));
  out.push_back(B(0, 0, 0, 0, h + 8, h + 10, MAT::SIGNAL_AMBER));
  return out;
}

Boxes container_stack(Rng&, const PropOpts& o) {
  // (an undefined seed is ToUint32 0, as NaN is here)
  Rng r(num(o.seed));
  const uint16_t colors[] = {MAT::CONTAINER_RED, MAT::CONTAINER_BLUE, MAT::CONTAINER_GREEN, MAT::CONTAINER_ORANGE, MAT::CORRUGATED_RUST, MAT::CORRUGATED_BLUE};
  Boxes out;
  const double n = num(o.n);
  for (double k = 0; k < n; k += 1) {
    const double z = k * CH;
    const uint16_t c = r.pick(colors);
    // props fill only air: details first, then the body
    out.push_back(B(0, 0, 0, CW - 1, z, z + CH - 1, MAT::METAL_BLACK));
    out.push_back(B(CL - 1, CL - 1, 0, CW - 1, z, z + CH - 1, MAT::METAL_BLACK));
    out.push_back(B(1, CL - 2, 0, CW - 1, z + CH - 1, z + CH - 1, MAT::METAL_PANEL));
    out.push_back(B(0, CL - 1, 0, CW - 1, z, z + CH - 1, c));
  }
  return out;
}

Boxes gantry_crane(Rng&, const PropOpts& o) {
  const double s = num(o.span);
  const double h = vx(18);
  Boxes out;
  for (const double b : {0.0, s}) {
    out.push_back(B(0, 3, b, b + 3, 0, h, MAT::HAZARD_YELLOW));
    out.push_back(B(vx(8), vx(8) + 3, b, b + 3, 0, h, MAT::HAZARD_YELLOW));
    out.push_back(B(0, vx(8) + 3, b, b + 3, 0, 3, MAT::HAZARD_YELLOW));
    out.push_back(B(-2, 5, b - 1, b + 4, 0, 1, MAT::TIRE));
    out.push_back(B(vx(8) - 2, vx(8) + 5, b - 1, b + 4, 0, 1, MAT::TIRE));
  }
  out.push_back(B(0, 3, 0, s + 3, h, h + 5, MAT::HAZARD_YELLOW));
  out.push_back(B(vx(8), vx(8) + 3, 0, s + 3, h, h + 5, MAT::HAZARD_YELLOW));
  const double tb = js::round(s * 0.4);
  out.push_back(B(0, vx(8) + 3, tb, tb + 16, h - 6, h - 1, MAT::METAL_PANEL_DARK));
  out.push_back(B(2, 8, tb + 2, tb + 10, h - 12, h - 7, MAT::CAR_GLASS));
  out.push_back(B(vx(4), vx(4) + 1, tb + 6, tb + 7, h - 40, h - 7, MAT::METAL_BLACK));
  out.push_back(B(vx(4) - 8, vx(4) + 9, tb, tb + 16, h - 42, h - 41, MAT::HAZARD_BLACK));
  return out;
}

}  // namespace

const std::vector<PropDef>& industry_props() {
  static const std::vector<PropDef> table = {
      {"silo", silo},
      {"stsCrane", sts_crane},
      {"cargoShip", cargo_ship},
      {"garageRow", garage_row},
      {"roundBale", round_bale},
      {"storageTank", storage_tank},
      {"bund", bund},
      {"pipeRack", pipe_rack},
      {"processColumn", process_column},
      {"flareStack", flare_stack},
      {"containerStack", container_stack},
      {"gantryCrane", gantry_crane},
  };
  return table;
}

}  // namespace svx::city
