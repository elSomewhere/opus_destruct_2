// svx_city — voxel_city buildings/massing.js.
#include "buildings/massing.hpp"

#include <cmath>
#include <vector>

#include "buildings/chamfer.hpp"
#include "buildings/facade.hpp"
#include "buildings/frame.hpp"
#include "core/hash.hpp"
#include "core/js.hpp"
#include "nature/landcover.hpp"
#include "voxel/materials.hpp"
#include "world/fields.hpp"
#include "world/season.hpp"

namespace svx::city {

namespace {

// Default eaves overhang (voxels) of pitched roofs.
constexpr double kOverhang = 3;
constexpr double kHalfPi = 3.141592653589793 / 2;

// A material as a Uint16Array stores it (JS's null: 0).
inline uint16_t mat16(int m) { return m < 0 ? 0 : static_cast<uint16_t>(m); }
inline void put(ChunkBuffer& c, int i, int j, int k, uint16_t m) { c.data[static_cast<size_t>(ChunkBuffer::index(i, j, k))] = m; }

// floorZ(env, f) for f = 0 .. n: the same sums, in the same order, as floor_z.
std::vector<double> floor_zs(const Envelope& env, double n) {
  std::vector<double> z;
  z.reserve(static_cast<size_t>(js::max(0.0, n)) + 1);
  double acc = env.base_z;
  z.push_back(acc);
  for (double k = 0; k < n; k += 1) {
    acc += k < static_cast<double>(env.story_h.size()) ? env.story_h[static_cast<size_t>(k)] : js::kNaN;
    z.push_back(acc);
  }
  return z;
}

bool inside(const std::vector<Rect>& rects, double u, double v) {
  for (const Rect& r : rects)
    if (u >= r.x0 && u <= r.x1 && v >= r.y0 && v <= r.y1) return true;
  return false;
}

// Facade side of a wall cell ('F', 'B', 'L', 'R'), or 0 if interior.
char wall_side(const std::vector<Rect>& rects, double u, double v, double t) {
  if (!inside(rects, u, v - t)) return 'F';
  if (!inside(rects, u, v + t)) return 'B';
  if (!inside(rects, u - t, v)) return 'L';
  if (!inside(rects, u + t, v)) return 'R';
  return 0;
}

const Rect& containing_rect(const std::vector<Rect>& rects, double u, double v) {
  for (const Rect& r : rects)
    if (u >= r.x0 && u <= r.x1 && v >= r.y0 && v <= r.y1) return r;
  return rects[0];
}

// Fills padded column (i, j) between world heights a and b with one material.
void col(ChunkBuffer& chunk, int i, int j, double a, double b, uint16_t mat) {
  const IdxRange rk = chunk.range_z(a, b);
  for (int k = rk.lo; k <= rk.hi; ++k) put(chunk, i, j, k, mat);
}

// Profile of an onion dome: radius share at height share t (a bulge a third of the way up, a
// drawn-out point).
double onion_profile(double t) {
  if (t < 0.36) return 0.68 + 0.32 * js::sin((t / 0.36) * kHalfPi);
  return js::pow(js::cos(((t - 0.36) / 0.64) * kHalfPi), 1.4);
}

// One column of an onion dome centred (du, dv) away: a drum (radius rd, dh voxels tall from z0, a
// cornice on top), the bulb (radius R, H tall) above it and a gilded three-bar cross on the tip.
void onion(ChunkBuffer& chunk, int i, int j, double du, double dv, double z0, double rd, double dh, double R, double H, uint16_t m, const ResolvedStyle& st) {
  const double d = js::hypot(du, dv);
  if (dh > 0 && d <= rd + 0.5) col(chunk, i, j, z0, z0 + dh - 2, st.wall);
  if (dh > 0 && d <= rd + 1.5) col(chunk, i, j, z0 + dh - 1, z0 + dh - 1, st.trim);
  const double zb = z0 + dh;
  double lo = -1;
  double hi = -1;
  for (double h = 0; h < H; h += 1) {
    if (R * onion_profile((h + 0.5) / H) + 0.35 >= d) {
      if (lo < 0) lo = h;
      hi = h;
    }
  }
  if (lo >= 0) col(chunk, i, j, zb + lo, zb + hi, m);
  // the cross: a rod, a short top bar, the main bar and a slanted foot bar (one column wide at
  // every LOD: columns are 1 << lod voxels apart)
  const double step = static_cast<double>(1 << chunk.lod);
  if (dv < 0 || dv >= step) return;
  const double top = zb + H;
  const double a = std::fabs(du);
  if (du >= 0 && du < step) {
    col(chunk, i, j, top - 1, top + 15, MAT::GOLD);
  } else if (a <= 4) {
    col(chunk, i, j, top + 10, top + 10, MAT::GOLD);
    if (a <= 2) col(chunk, i, j, top + 13, top + 13, MAT::GOLD);
    if (a <= 3) col(chunk, i, j, top + 5 + js::sign(du), top + 5 + js::sign(du), MAT::GOLD);
  }
}

// Onion domes on drums along a church's nave ridge (env.domes; the drums rise from the eaves
// through the roof).
void nave_domes(ChunkBuffer& chunk, int i, int j, const Envelope& env, double z_top, double u, double v, const ResolvedStyle& st) {
  for (const EnvDome& dm : env.domes) {
    const Rect& r = env.tiers[0].rects[static_cast<size_t>(dm.rect)];
    const double cu = std::floor((r.x0 + r.x1) / 2);
    const double cv = js::round(r.y0 + (r.y1 - r.y0) * dm.fv);
    const double du = u - cu;
    const double dv = v - cv;
    if (std::fabs(du) > dm.r + 5 || std::fabs(dv) > dm.r + 5) continue;
    onion(chunk, i, j, du, dv, z_top, js::round(dm.r * 0.62), dm.drum, dm.r, dm.h, dm.m, st);
  }
}

// Open annexes of civic lots: a petrol canopy (a flat roof with a coloured fascia on steel columns
// at its corners and along its sides) and a sign pylon (a post with a lit panel). Drawn at every
// LOD.
void open_annex(ChunkBuffer& chunk, int i, int j, const EnvelopeAnnex& a, const Envelope& env, double x, double y, bool snowy) {
  const Rect& r = a.world;
  const double z0 = env.ground_z + 1;
  const uint16_t fascia = env.extra ? env.extra->sign_color : static_cast<uint16_t>(MAT::SIGNAGE_RED);
  if (a.kind == "pylon") {
    const bool post = x - r.x0 < 2 && y - r.y0 < 2;
    const IdxRange rk = chunk.range_z(z0, z0 + a.height);
    for (int k = rk.lo; k <= rk.hi; ++k) {
      const double zr = chunk.wz(k) - z0;
      if (zr >= a.height - 20)
        put(chunk, i, j, k, zr >= a.height - 2 ? static_cast<uint16_t>(MAT::METAL_PANEL_DARK) : fascia);
      else if (post)
        put(chunk, i, j, k, MAT::METAL_PANEL_DARK);
    }
    return;
  }
  const double H = a.height;
  const bool edge = x - r.x0 < 2 || r.x1 - x < 2 || y - r.y0 < 2 || r.y1 - y < 2;
  const bool column = (std::fabs(x - r.x0 - 12) < 2 || std::fabs(r.x1 - x - 12) < 2 || std::fabs(x - (r.x0 + r.x1) / 2) < 2) && std::fabs(y - (r.y0 + r.y1) / 2) < 2;
  const IdxRange rk = chunk.range_z(z0, z0 + H + 3);
  for (int k = rk.lo; k <= rk.hi; ++k) {
    const double zr = chunk.wz(k) - z0;
    uint16_t m = 0;
    if (zr >= H)
      m = zr == H + 3 ? (snowy && !edge ? static_cast<uint16_t>(MAT::SNOW) : edge ? fascia : static_cast<uint16_t>(MAT::PLASTIC_WHITE))
                      : edge ? fascia : static_cast<uint16_t>(MAT::PLASTIC_WHITE);
    else if (column)
      m = MAT::METAL_PANEL;
    if (zr == H && !edge && std::fmod(static_cast<double>(js::sar(x, 4)) + static_cast<double>(js::sar(y, 4)), 3) == 0) m = MAT::LIGHT_STRIP;
    if (m) put(chunk, i, j, k, m);
  }
}

bool is_garage_door(const Frame& frame, const EnvelopeAnnex& a, double u, double v) {
  const Rect g = a.canon ? *a.canon : frame.rect_from_world(a.world);
  return v <= g.y0 + 1 && u >= g.x0 + 3 && u <= g.x1 - 3;
}

void flat_roof(ChunkBuffer& chunk, int i, int j, double zr0, const std::vector<Rect>& rects, double u, double v, double thick, const ResolvedStyle& st, bool snowy) {
  const bool edge = !inside(rects, u - thick, v) || !inside(rects, u + thick, v) || !inside(rects, u, v - thick) || !inside(rects, u, v + thick);
  const double parapet = edge ? 5 : 0;
  const IdxRange rk = chunk.range_z(zr0, zr0 + 1 + parapet);
  for (int k = rk.lo; k <= rk.hi; ++k) {
    const double zz = chunk.wz(k) - zr0;
    uint16_t m;
    if (zz < 2)
      m = edge ? st.wall : snowy && zz > 1 - chunk.s ? static_cast<uint16_t>(MAT::SNOW) : st.roof;
    else
      m = zz == 1 + parapet ? static_cast<uint16_t>(MAT::PARAPET_CAP) : st.wall;
    put(chunk, i, j, k, m);
  }
}

// Eaves overhang of a pitched roof per canonical side (roof.overhang: voxels or {F, B, L, R}).
struct Overhang {
  double F, B, L, R;
};
Overhang overhang_of(const EnvRoof& roof) {
  if (roof.overhang_kind == EnvRoof::Overhang::Sides) {
    auto o = [](double v) { return v == v ? v : kOverhang; };
    return {o(roof.overhang_f), o(roof.overhang_b), o(roof.overhang_l), o(roof.overhang_r)};
  }
  const double o = roof.overhang_kind == EnvRoof::Overhang::Number ? roof.overhang : kOverhang;
  return {o, o, o, o};
}

// Gable triangle wall: the facade wall, with the nordic board seams / corner boards continued.
uint16_t gable_material(const ResolvedStyle& st, const Rect& main, double u, double v, bool along_v) {
  const bool seam = st.seam != -1 && st.seam != 0;
  if (!seam && !st.corners) return st.wall;
  // position along the gable's facade side, measured like the facade below
  const double len = along_v ? main.x1 - main.x0 + 1 : main.y1 - main.y0 + 1;
  const double t = along_v ? (v - main.y0 < 2 ? u - main.x0 : main.x1 - u) : u - main.x0 < 2 ? main.y1 - v : v - main.y0;
  if (st.corners && (t < 2 || t >= len - 2)) return st.trim;
  if (seam && st.seam_dir == 'v' && std::fmod(t, 3) == 0) return static_cast<uint16_t>(st.seam);
  return st.wall;
}

// Pitched roof over the top tier's main rect. roof.slope is the rise per voxel (default 0.7,
// ~35 degrees), roof.ridge the ridge direction: "u" (default, parallel to the street, eaves at the
// front and back) or "v" (gable to the street). The top `skin` voxels are roofing, the attic below
// is solid.
void pitched_roof(ChunkBuffer& chunk, int i, int j, const Envelope& env, double z_top, double u, double v, const ResolvedStyle& st, bool snowy) {
  const EnvRoof& roof = env.roof;
  const std::vector<Rect>& rects = env.tiers[env.tiers.size() - 1].rects;
  const Rect& main = rects[0];
  const Overhang oh = overhang_of(roof);
  const Rect r{main.x0 - oh.L, main.y0 - oh.F, main.x1 + oh.R, main.y1 + oh.B};
  if (u < r.x0 || u > r.x1 || v < r.y0 || v > r.y1) return;
  const double dv = js::min(v - r.y0, r.y1 - v);
  const double du = js::min(u - r.x0, r.x1 - u);
  const bool along_v = roof.ridge == "v";
  double h;
  if (roof.type == "hip")
    h = js::min(du, dv);
  else if (roof.type == "sawtooth")
    h = std::fmod(v - r.y0, 48) / 3;
  else
    h = along_v ? du : dv;
  const double slope = roof.slope == roof.slope ? roof.slope : 0.7;
  h = std::floor(h * slope) + 1;
  // roofing deep enough to cover the steps between sampled columns at coarse LODs (2 voxels up
  // close), so no attic shows through
  const double skin = std::ceil(slope * chunk.s) + chunk.s;
  const bool in_main = u >= main.x0 && u <= main.x1 && v >= main.y0 && v <= main.y1;
  const double base = in_main ? z_top : z_top - 2;
  const IdxRange rk = chunk.range_z(base, z_top + h);
  const bool gable_wall = roof.type == "gable" && in_main && (along_v ? v - main.y0 < 2 || main.y1 - v < 2 : u - main.x0 < 2 || main.x1 - u < 2);
  // snow on the top voxel(s) (as deep as the steps of a steep roof), the eaves and verge rows stay
  // bare
  const double snow_top = snowy && u > r.x0 && u < r.x1 && v > r.y0 && v < r.y1 ? h - chunk.s * js::max(1.0, std::ceil(slope)) : js::kInf;
  const uint16_t gable = gable_wall ? gable_material(st, main, u, v, along_v) : 0;
  for (int k = rk.lo; k <= rk.hi; ++k) {
    const double zz = chunk.wz(k) - z_top;
    const uint16_t m = zz >= h - (skin - 1) ? (zz > snow_top ? static_cast<uint16_t>(MAT::SNOW) : st.pitched)
                       : gable_wall      ? (st.seam_dir == 'h' && std::fmod(zz, 3) == 0 ? mat16(st.seam) : gable)
                                         : static_cast<uint16_t>(MAT::WOOD_MED);
    if (!in_main && zz < h - skin) continue;
    put(chunk, i, j, k, m);
  }
}

// Church steeple over ground-tier rect env.steeple.rect: the tower shaft rises `shaft` voxels
// above the eaves, boarded like the facade with corner boards and louvred belfry openings near
// the top; a pyramid spire of `spire` voxels flares out over it and carries a finial cross. Draws
// the column and returns true inside the tower rect (no roof there); outside it only adds the
// spire's flared eaves, high above the nave roof.
bool steeple(ChunkBuffer& chunk, int i, int j, const Envelope& env, double z_top, double u, double v, const ResolvedStyle& st) {
  const EnvSteeple& sp = *env.steeple;
  const Rect& t = env.tiers[0].rects[static_cast<size_t>(sp.rect)];
  const double oh = 2;
  if (u < t.x0 - oh || u > t.x1 + oh || v < t.y0 - oh || v > t.y1 + oh) return false;
  const bool in_t = u >= t.x0 && u <= t.x1 && v >= t.y0 && v <= t.y1;
  const double zS = z_top + sp.shaft;
  const bool dome = sp.dome && *sp.dome != 0;
  const bool tent = sp.tent && *sp.tent;
  const bool seam = st.seam != -1 && st.seam != 0;
  if (in_t) {
    // shaft: boarded shell, solid inside
    const double W = t.x1 - t.x0 + 1;
    const double D = t.y1 - t.y0 + 1;
    double len = 0;
    double tt = -1;
    if (v - t.y0 < 2) {
      len = W;
      tt = u - t.x0;
    } else if (t.y1 - v < 2) {
      len = W;
      tt = t.x1 - u;
    } else if (u - t.x0 < 2) {
      len = D;
      tt = t.y1 - v;
    } else if (t.x1 - u < 2) {
      len = D;
      tt = v - t.y0;
    }
    const IdxRange rk = chunk.range_z(z_top, zS - 1);
    for (int k = rk.lo; k <= rk.hi; ++k) {
      const double zr = chunk.wz(k) - z_top;
      uint16_t m = st.wall;
      if (tt >= 0) {
        const bool belfry = zr >= sp.shaft - 22 && zr < sp.shaft - 5 && std::fabs(tt - (len - 1) / 2) <= len * 0.22;
        if (tt < 2 || tt >= len - 2)
          m = st.trim;
        else if (belfry)
          m = std::fmod(zr, 2) == 0 ? static_cast<uint16_t>(MAT::WOOD_DARK) : static_cast<uint16_t>(MAT::FRAME_DARK);
        else if (zr == sp.shaft - 23 || zr == sp.shaft - 4)
          m = st.trim;
        else
          m = seam && st.seam_dir == 'v' && std::fmod(tt, 3) == 0 ? static_cast<uint16_t>(st.seam) : st.wall;
      }
      put(chunk, i, j, k, m);
    }
  }
  const double cu0 = std::floor((t.x0 + t.x1) / 2);
  const double cv0 = std::floor((t.y0 + t.y1) / 2);
  if (dome && !tent) {
    // an onion dome on a drum over a cornice round the tower top
    if (!in_t) {
      col(chunk, i, j, zS - 1, zS - 1, st.trim);
      return false;
    }
    col(chunk, i, j, zS, zS, st.trim);
    const double R = std::floor((js::min(t.x1 - t.x0, t.y1 - t.y0) + 1) * 0.46);
    onion(chunk, i, j, u - cu0, v - cv0, zS + 1, js::round(R * 0.6), 8, R, js::min(sp.spire, js::round(R * 2.5)), *sp.dome, st);
    return true;
  }
  // spire: a pyramid over the tower + overhang, flared eaves at its foot
  const double half = (js::min(t.x1 - t.x0, t.y1 - t.y0) + 1) / 2 + oh;
  const double dist = js::min(u - t.x0 + oh, t.x1 + oh - u, v - t.y0 + oh, t.y1 + oh - v);
  const double slope = sp.spire / half;
  const double hs = std::floor(dist * slope);
  const uint16_t spire_m = sp.dome ? *sp.dome : st.pitched;
  const IdxRange rk = chunk.range_z(in_t ? zS : zS - 1, zS + hs);
  for (int k = rk.lo; k <= rk.hi; ++k) put(chunk, i, j, k, chunk.wz(k) < zS ? st.trim : spire_m);
  if (dome) {
    // a tented spire (shatyor) with a small onion on its apex
    onion(chunk, i, j, u - cu0, v - cv0, zS + std::floor(half * slope) - 3, 0, 0, 5, 14, *sp.dome, st);
    return in_t;
  }
  // finial: a rod with a cross arm over the apex
  const double cu = (t.x0 + t.x1) / 2;
  const double cv = (t.y0 + t.y1) / 2;
  const double top = zS + std::floor(half * slope);
  if (std::fabs(u - cu) <= 0.5 && std::fabs(v - cv) <= 0.5)
    col(chunk, i, j, zS + hs + 1, top + 16, MAT::METAL_BLACK);
  else if (std::fabs(u - cu) <= 3.5 && std::fabs(v - cv) <= 0.5)
    col(chunk, i, j, top + 11, top + 11, MAT::METAL_BLACK);
  return in_t;
}

}  // namespace

double roof_snow_cover(const World& world, const Envelope& env) {
  if (env.snow) return *env.snow;
  return env.snow_cache.get([&] {
    const Season season = season_of(world.config);
    double c = 0;
    if (season.any) {
      const double t = world.fields->temperature((env.R.x0 + env.R.x1) / 2, (env.R.y0 + env.R.y1) / 2) - js::max(0.0, env.base_z / 8) / kLapse;
      c = js::min(1.0, season.roof_snow(t));
    }
    return c;
  });
}

bool snow_at(double seed, double cover, double x, double y) {
  if (cover >= 1) return true;
  // smooth 2.5 m value noise with a little 0.5 m grain: bare patches, not speckle
  const double c = 20;
  const double gx = std::floor(x / c);
  const double gy = std::floor(y / c);
  double fx = x / c - gx;
  double fy = y / c - gy;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  const double a = hash_float(seed, gx, gy, 0x5e0);
  const double b = hash_float(seed, gx + 1, gy, 0x5e0);
  const double d = hash_float(seed, gx, gy + 1, 0x5e0);
  const double e = hash_float(seed, gx + 1, gy + 1, 0x5e0);
  const double n = (a + (b - a) * fx) * (1 - fy) + (d + (e - d) * fx) * fy;
  return n * 0.8 + hash_float(seed, js::sar(x, 2), js::sar(y, 2), 0x5e1) * 0.2 < cover;
}

void voxelize_massing(const World& world, const Envelope& env, ChunkBuffer& chunk) {
  const double s = chunk.s;
  const Box3 box = chunk.world_box();
  const Rect& b = env.bounds;
  if (b.x1 < box.x0 || b.x0 > box.x1 || b.y1 < box.y0 || b.y0 > box.y1) return;
  if (env.top_z < box.z0 || env.bottom_z > box.z1) return;
  const Frame& frame = envelope_frame(env);
  const BuildingLook& look = building_look(env, world.seed);
  const ResolvedStyle& st = look.style;
  const IdxRange ri = chunk.range_x(js::max(b.x0, box.x0), js::min(b.x1, box.x1));
  const IdxRange rj = chunk.range_y(js::max(b.y0, box.y0), js::min(b.y1, box.y1));
  const double thick = js::max(2.0, s);
  const double nF = env.floors;
  const std::vector<double> fz = floor_zs(env, nF);
  const double z_top = fz[fz.size() - 1];
  const double snow = roof_snow_cover(world, env);
  const bool snowing = js::truthy(snow);
  const std::vector<Rect>& ground = env.tiers[0].rects;

  for (int j = rj.lo; j <= rj.hi; ++j) {
    const double y = chunk.wy(j);
    for (int i = ri.lo; i <= ri.hi; ++i) {
      const double x = chunk.wx(i);
      const XY uv = frame.from_world(x, y);
      const double u = uv[0], v = uv[1];

      // annexes (garages): simple single-story boxes
      for (const EnvelopeAnnex& a : env.annexes) {
        const Rect& r = a.world;
        if (x < r.x0 || x > r.x1 || y < r.y0 || y > r.y1) continue;
        // (a turned building's annex is its canonical rect; the world box only culls)
        const std::optional<Rect>& c = a.canon;
        if (c && (u < c->x0 || u > c->x1 || v < c->y0 || v > c->y1)) continue;
        if (a.kind == "canopy" || a.kind == "pylon") {
          open_annex(chunk, i, j, a, env, x, y, snowing && snow_at(world.seed, snow, x, y));
          continue;
        }
        const double z0 = env.base_z;
        const bool edge = c ? u - c->x0 < thick || c->x1 - u < thick || v - c->y0 < thick || c->y1 - v < thick
                            : x - r.x0 < thick || r.x1 - x < thick || y - r.y0 < thick || r.y1 - y < thick;
        const IdxRange rk = chunk.range_z(z0, z0 + a.height + 1);
        for (int k = rk.lo; k <= rk.hi; ++k) {
          const double zr = chunk.wz(k) - z0;
          uint16_t m = edge ? st.wall : static_cast<uint16_t>(MAT::PAINT_DARK);
          if (zr >= a.height)
            m = snowing && zr > a.height + 1 - s && snow_at(world.seed, snow, x, y) ? MAT::SNOW : MAT::ROOF_MEMBRANE;
          else if (zr < 2)
            m = MAT::CONCRETE;
          put(chunk, i, j, k, m);
        }
      }

      // basements: solid foundation block under the ground tier
      const bool in_ground = inside(ground, u, v);
      if (in_ground && env.basements > 0) {
        const IdxRange rk = chunk.range_z(env.bottom_z, env.base_z - 1);
        for (int k = rk.lo; k <= rk.hi; ++k) put(chunk, i, j, k, MAT::CONCRETE_DARK);
      }
      // raised ground floors on a plinth (cabins): stone up to the floor slab
      if (in_ground && env.plinth) {
        const IdxRange rk = chunk.range_z(env.ground_z + 1, env.base_z - 1);
        for (int k = rk.lo; k <= rk.hi; ++k) put(chunk, i, j, k, st.base);
      }

      // (a chamfered corner: nothing from the ground floor up, its slab part holds the facade)
      if (env.chamfer && chamfer_cut(*env.chamfer, env.U, u, v)) continue;
      for (double f = 0; f < nF; f += 1) {
        const size_t fi = static_cast<size_t>(f);
        const double z0 = fz[fi];
        const double H = fi < env.story_h.size() ? env.story_h[fi] : js::kNaN;
        if (z0 > box.z1) break;
        const std::vector<Rect>& rects = tier_rects(env, f);
        const bool in_f = inside(rects, u, v);
        // roof over this tier where the next tier doesn't continue
        if (in_f && (f == nF - 1 || !inside(tier_rects(env, f + 1), u, v))) {
          if (!(f == nF - 1 && env.roof.type != "flat")) {
            flat_roof(chunk, i, j, z0 + H, rects, u, v, thick, st, snowing && snow_at(world.seed, snow, x, y));
          }
        }
        if (z0 + H - 1 < box.z0) continue;
        if (!in_f) continue;
        const char side = wall_side(rects, u, v, thick);
        const IdxRange rk = chunk.range_z(z0, z0 + H - 1);
        double len = 0;
        double t = 0;
        if (side) {
          const Rect& r = containing_rect(rects, u, v);
          if (side == 'F' || side == 'B') {
            len = r.x1 - r.x0 + 1;
            t = side == 'F' ? u - r.x0 : r.x1 - u;
          } else {
            len = r.y1 - r.y0 + 1;
            t = side == 'L' ? r.y1 - v : v - r.y0;
          }
        }
        for (int k = rk.lo; k <= rk.hi; ++k) {
          const double zr = chunk.wz(k) - z0;
          uint16_t m;
          if (side) {
            const int cls = facade_cell(look, len, t, zr, H, f);
            m = mat16(facade_material(look, cls, f, zr));
          } else {
            m = zr < 2 ? MAT::CONCRETE : MAT::PAINT_DARK;
          }
          put(chunk, i, j, k, m);
        }
      }

      if (env.roof.type != "flat" && !(env.steeple && steeple(chunk, i, j, env, z_top, u, v, st))) {
        pitched_roof(chunk, i, j, env, z_top, u, v, st, snowing && snow_at(world.seed, snow, x, y));
      }
      if (!env.domes.empty()) nave_domes(chunk, i, j, env, z_top, u, v, st);
    }
  }
}

void pitched_roof_only(const World& world, const Envelope& env, ChunkBuffer& chunk) {
  const Box3 box = chunk.world_box();
  const Rect& b = env.bounds;
  if (b.x1 < box.x0 || b.x0 > box.x1 || b.y1 < box.y0 || b.y0 > box.y1) return;
  const Frame& frame = envelope_frame(env);
  const ResolvedStyle& st = building_look(env, world.seed).style;
  const IdxRange ri = chunk.range_x(js::max(b.x0, box.x0), js::min(b.x1, box.x1));
  const IdxRange rj = chunk.range_y(js::max(b.y0, box.y0), js::min(b.y1, box.y1));
  const double z_top = floor_z(env, env.floors);
  const double thick = 2;
  const double snow = roof_snow_cover(world, env);
  const bool snowing = js::truthy(snow);
  for (int j = rj.lo; j <= rj.hi; ++j) {
    const double y = chunk.wy(j);
    for (int i = ri.lo; i <= ri.hi; ++i) {
      const double x = chunk.wx(i);
      const XY uv = frame.from_world(x, y);
      const double u = uv[0], v = uv[1];
      for (const EnvelopeAnnex& a : env.annexes) {
        const Rect& r = a.world;
        if (x < r.x0 || x > r.x1 || y < r.y0 || y > r.y1) continue;
        // (a turned building's annex is its canonical rect; the world box only culls)
        const std::optional<Rect>& c = a.canon;
        if (c && (u < c->x0 || u > c->x1 || v < c->y0 || v > c->y1)) continue;
        if (a.kind == "canopy" || a.kind == "pylon") {
          open_annex(chunk, i, j, a, env, x, y, snowing && snow_at(world.seed, snow, x, y));
          continue;
        }
        const double z0 = env.base_z;
        const bool edge = c ? u - c->x0 < thick || c->x1 - u < thick || v - c->y0 < thick || c->y1 - v < thick
                            : x - r.x0 < thick || r.x1 - x < thick || y - r.y0 < thick || r.y1 - y < thick;
        const IdxRange rk = chunk.range_z(z0, z0 + a.height + 1);
        for (int k = rk.lo; k <= rk.hi; ++k) {
          const double zr = chunk.wz(k) - z0;
          uint16_t m = edge ? (zr > 1 && zr < 18 && a.kind == "garage" && is_garage_door(frame, a, u, v) ? static_cast<uint16_t>(MAT::ROLLUP_DOOR) : st.wall)
                       : zr < 2 ? static_cast<uint16_t>(MAT::FLOOR_CONCRETE)
                                : static_cast<uint16_t>(0);
          if (zr >= a.height) m = snowing && zr > a.height && snow_at(world.seed, snow, x, y) ? MAT::SNOW : MAT::ROOF_MEMBRANE;
          put(chunk, i, j, k, m);
        }
      }
      if (env.roof.type != "flat" && !(env.steeple && steeple(chunk, i, j, env, z_top, u, v, st))) {
        pitched_roof(chunk, i, j, env, z_top, u, v, st, snowing && snow_at(world.seed, snow, x, y));
      }
      if (!env.domes.empty()) nave_domes(chunk, i, j, env, z_top, u, v, st);
    }
  }
}

}  // namespace svx::city
