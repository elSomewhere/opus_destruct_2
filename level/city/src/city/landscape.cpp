// svx_city — voxel_city city/landscape.js.
#include "city/landscape.hpp"

#include <cmath>

#include "city/industry.hpp"
#include "city/parks.hpp"
#include "core/hash.hpp"
#include "core/math.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

namespace {

// (x % n + n) % n
inline double wrap_mod(double x, double n) { return std::fmod(std::fmod(x, n) + n, n); }

bool in_rects(const std::vector<Rect>& rects, double u, double v) {
  for (const Rect& r : rects)
    if (u >= r.x0 && u <= r.x1 && v >= r.y0 && v <= r.y1) return true;
  return false;
}

// Flower beds either side of a house's front path ('\0': none).
uint16_t flower_bed(double u, double v, const LotEnv& env) {
  if (v < -vx(1.0) && v > -vx(2.2) && (u < env.U * 0.35 || u > env.U * 0.65) && u > 4 && u < env.U - 4) {
    return (js::sar(u, 2) & 1) == 0 ? MAT::FLOWER_RED : MAT::FLOWER_YELLOW;
  }
  return 0;
}

// Forecourts and yards of civic buildings: a supermarket's car park, a petrol station's asphalt,
// an engine apron, a stone forecourt in front of museums and town halls, a drive-up in front of a
// hospital, lawn round the sides.
uint16_t civic_yard(const LotEnv& env, double u, double v, double hx, double hy, double seed) {
  const std::string& c = env.civic;
  if (v >= 0) {
    // sides and back: service yard behind shops and stations, lawn elsewhere
    if (c == "supermarket" || c == "petrolStation" || c == "fireStation" || c == "policeStation" || c == "marketHall") return v > env.V ? MAT::ASPHALT_WORN : MAT::PAVER_GRAY;
    return (hash32(std::floor(hx / 12), std::floor(hy / 12), seed, 31) & 7u) == 0 ? MAT::GRASS : MAT::GRASS_LAWN;
  }
  const double ent_u = env.entrance_u ? *env.entrance_u : std::floor(env.U / 2);
  if (c == "supermarket") {
    // rows of stalls with drive aisles, a walkway to the doors
    if (std::fabs(u - ent_u) < vx(1.5)) return MAT::PAVER_GRAY;
    const double row = std::floor(-v / vx(6));
    const double in_row = std::fmod(-v, vx(6));
    if (std::fmod(row, 2) == 1) return MAT::ASPHALT;
    if (in_row == 0) return MAT::LINE_WHITE;
    return wrap_mod(u, 20) == 0 ? MAT::LINE_WHITE : MAT::ASPHALT_WORN;
  }
  if (c == "petrolStation") return (hash32(std::floor(hx / 24), std::floor(hy / 24), seed, 32) & 7u) == 0 ? MAT::ASPHALT_PATCH : MAT::ASPHALT;
  if (c == "fireStation") return std::fabs(v + vx(0.5)) < 2 ? MAT::HAZARD_YELLOW : MAT::CONCRETE;
  if (c == "policeStation" || c == "hospital" || c == "polyclinic") return std::fabs(u - ent_u) < vx(3) ? MAT::PAVER_GRAY : v > -vx(4) ? MAT::PAVER_GRAY : MAT::ASPHALT;
  if (c == "museum" || c == "townHall" || c == "concertHall" || c == "houseOfCulture" || c == "library") {
    const double a = wrap_mod(hx, 16);
    const double b = wrap_mod(hy, 16);
    return a == 0 || b == 0 ? MAT::PLAZA_STONE_DARK : MAT::PLAZA_STONE;
  }
  return MAT::PAVER_GRAY;
}

// Cemetery ground in its gate frame (u along the street, v inwards): a gravel main path from the
// gate to the chapel, cross paths every ~16 m, a path along the inside of the wall; grass between
// the rows of graves.
void cemetery_surface(const OpenSpace& space, double x, double y, double hx, double hy, SpaceSample& out) {
  const Frame& f = space_frame(space);
  const XY uv = f.from_world(x, y);
  const double u = uv[0];
  const double v = uv[1];
  const double U = f.U;
  const double V = f.V;
  const double e = js::min(u, v, U - 1 - u, V - 1 - v);
  const bool main = std::fabs(u - (U - 1) / 2) < vx(1.4);
  const bool cross = v > vx(4) && std::fmod(v - vx(4), vx(16)) < vx(1.1);
  if (e < vx(2) && e >= vx(0.8))
    out.mat = MAT::GRAVEL;
  else if (main || (cross && e > vx(2)))
    out.mat = MAT::GRAVEL;
  else
    out.mat = (hash32(std::floor(hx / 10), std::floor(hy / 10), 951) & 7u) == 0 ? MAT::GRASS : MAT::GRASS_LAWN;
}

void allotment_surface(const OpenSpace& space, double x, double y, double hx, double hy, SpaceSample& out) {
  const Frame& f = space_frame(space);
  const XY uv = f.from_world(x, y);
  const double u = uv[0];
  const double v = uv[1];
  const double pu = std::fmod(u, kAllot.w + kAllot.path);
  const double pv = std::fmod(v, kAllot.d + kAllot.path);
  if (pu < kAllot.path || pv < kAllot.path) {
    out.mat = MAT::GRAVEL;
    return;
  }
  // each plot: beds in rows at the back, grass at the front (by the hut)
  const double cu = std::floor(u / (kAllot.w + kAllot.path));
  const double cv = std::floor(v / (kAllot.d + kAllot.path));
  const uint32_t kind = hash32(cu, cv, 961) & 3u;
  const double iv = pv - kAllot.path;
  if (kind != 3 && iv > kAllot.d * 0.45) {
    const double row = std::floor((pu - kAllot.path) / 4);
    out.mat = (js::to_int32(row) & 1) ? MAT::SOIL_BED : kind == 0 ? MAT::LEAVES_LIGHT : kind == 1 ? MAT::GRASS : MAT::SOIL_BED;
    return;
  }
  out.mat = (hash32(std::floor(hx / 8), std::floor(hy / 8), 962) & 7u) == 0 ? MAT::GRASS : MAT::GRASS_LAWN;
}

}  // namespace

uint16_t lot_surface(const LotEnv* env, double x, double y, double seed, double hx, double hy) {
  // (hx, hy: the position for texture hashes, canonical in a wrapping world)
  if (!env) {
    const uint32_t h = hash32(std::floor(hx / 16), std::floor(hy / 16), seed, 5);
    return (h & 7u) == 0 ? MAT::GRAVEL : (h & 7u) == 1 ? MAT::DIRT : MAT::GRASS_DRY;
  }
  const Frame& f = env->frame;
  const XY uv = f.from_world(x, y);
  const double u = uv[0];
  const double v = uv[1];
  if (in_rects(env->ground, u, v)) return MAT::CONCRETE;
  for (const LotEnvAnnex& a : env->annexes) {
    if (a.canon ? r_contains(*a.canon, u, v) : r_contains(a.world, x, y)) return MAT::CONCRETE;
  }
  const double ent_u = env->entrance_u ? *env->entrance_u : std::floor(env->U / 2);
  const std::string& arch = env->archetype;
  if (arch == "cabin") {
    // a clearing of forest floor and moss, a gravel path from the porch
    if (v < 0 && std::fabs(u - ent_u) <= vx(0.6)) return MAT::GRAVEL;
    return (hash32(std::floor(hx / 12), std::floor(hy / 12), seed, 6) & 3u) == 0 ? MAT::MOSS : MAT::FOREST_FLOOR;
  }
  if (arch == "church") {
    // churchyard: a gravel path to the tower door and round the church, grass between the graves
    if (v < 0 && std::fabs(u - ent_u) <= vx(1.25)) return MAT::GRAVEL;
    if (u > -vx(1.5) && u < env->U + vx(1.5) && v > -vx(1.5) && v < env->V + vx(1.5)) return MAT::GRAVEL;
    return MAT::GRASS_DRY;
  }
  if (arch == "house") {
    // driveway from the garage straight to the street
    for (const LotEnvAnnex& a : env->annexes) {
      if (a.kind != "garage") continue;
      const Rect g = a.canon ? *a.canon : f.rect_from_world(a.world);
      if (u >= g.x0 + 2 && u <= g.x1 - 2 && v < g.y0) return MAT::CONCRETE_LIGHT;
    }
    if (v < 0 && std::fabs(u - ent_u) <= vx(0.6)) return MAT::PAVER_GRAY;
    if (v > env->V && v < env->V + vx(3.5) && std::fabs(u - ent_u) < vx(2.5)) return MAT::PAVER_RED;
    const uint16_t bed = flower_bed(u, v, *env);
    return bed ? bed : static_cast<uint16_t>(MAT::GRASS_LAWN);
  }
  if (arch == "rowhouse") {
    if (v < 0) return std::fabs(u - ent_u) <= vx(0.75) ? MAT::PAVER_GRAY : MAT::GRASS_LAWN;
    return (hash32(std::floor(u / 12), std::floor(v / 12), seed) & 3u) == 0 ? MAT::PAVER_RED : MAT::GRASS_LAWN;
  }
  if (arch == "school") {
    // paved forecourt, schoolyard with a court behind the building, lawn at the sides
    if (v < 0) return std::fabs(u - ent_u) < vx(3) ? MAT::PAVER_GRAY : MAT::GRASS_LAWN;
    if (v > env->V + vx(2)) {
      const double yv = v - env->V - vx(2);
      const bool court = u > vx(4) && u < js::min(env->U - vx(4), vx(32)) && yv > vx(2) && yv < vx(17);
      if (court) {
        const double cu = u - vx(4);
        const double cv = yv - vx(2);
        const bool edge = cu < 1 || cv < 1 || cu > js::min(env->U - vx(8), vx(28)) - 1 || cv > vx(15) - 1 || std::fabs(cv - vx(7.5)) < 0.5;
        return edge ? MAT::LINE_WHITE : MAT::COURT_ORANGE;
      }
      return MAT::ASPHALT;
    }
    return MAT::GRASS_LAWN;
  }
  if (arch == "warehouse" || arch == "factory") {
    // front parking with stall lines
    if (v < 0) return wrap_mod(u, 20) == 0 ? MAT::LINE_WHITE : MAT::ASPHALT;
    return MAT::CONCRETE_DARK;
  }
  if (arch == "townhouse" || arch == "wharfhouse") {
    // old-town plots: flagstones in front, behind the house a cobbled or gravel court, or a little
    // kitchen garden with beds
    if (v < 0) return MAT::FLAGSTONE;
    const uint32_t kind = hash32(env->R.x0, env->R.y0, seed, 21) % 3u;
    if (kind == 0) return cobble(hx, hy);
    if (kind == 1) return (hash32(std::floor(hx / 10), std::floor(hy / 10), seed, 22) & 7u) == 0 ? MAT::GRASS : MAT::GRAVEL;
    const double back = v - env->V;
    if (back > vx(2) && (js::sar(u, 3) & 1) == 0 && std::fabs(u - env->U / 2) < env->U * 0.35) return MAT::SOIL_BED;
    return back < vx(1.5) ? MAT::GRAVEL : MAT::GRASS_LAWN;
  }
  if (!env->civic.empty()) return civic_yard(*env, u, v, hx, hy, seed);
  if (arch == "office" || arch == "tower") {
    const double a = wrap_mod(x, 16);
    const double b = wrap_mod(y, 16);
    return a == 0 || b == 0 ? MAT::PLAZA_STONE_DARK : MAT::PLAZA_STONE;
  }
  // apartments: paved front, green courtyard behind
  if (v < 0) return MAT::PAVER_GRAY;
  const double cu = wrap_mod(u, 48);
  if (cu < 8) return MAT::PAVER_GRAY;
  return MAT::GRASS_LAWN;
}

void space_surface(const OpenSpace& space, double x, double y, SpaceSample& out, double hx, double hy) {
  const Rect& r = space.rect;
  out.dz = 0;
  out.water = false;
  const double w = r.x1 - r.x0;
  const double h = r.y1 - r.y0;
  const double u = x - r.x0;
  const double v = y - r.y0;
  const std::string& kind = space.kind;
  if (kind == "plaza") {
    // a paved city plaza: large slabs in a running bond, a border band, a basin off-centre, rows
    // of tree beds along two sides
    const uint32_t k = hash32(r.x0, r.y0, 0x71a);
    const double fu = w * (0.35 + 0.3 * ((k & 255u) / 255.0));
    const double fv = h * (0.35 + 0.3 * (((k >> 8) & 255u) / 255.0));
    const double d = js::hypot(u - fu, v - fv);
    if (d < vx(4)) {
      out.mat = d < vx(3.4) ? MAT::WATER : MAT::GRANITE_LIGHT;
      out.water = d < vx(3.4);
      out.dz = d < vx(3.4) ? 0 : 2;
      return;
    }
    const double e = js::min(u, v, w - u, h - v);
    if (e < vx(1.2)) {
      out.mat = MAT::PLAZA_STONE_DARK;
      return;
    }
    // tree beds every 8 m, 3 m in from the long sides
    const bool lng = w >= h;
    const double along = lng ? u : v;
    const double across = lng ? js::min(v, h - v) : js::min(u, w - u);
    if (std::fabs(across - vx(4)) < vx(0.9) && wrap_mod(along, vx(8)) < vx(1.8)) {
      out.mat = MAT::TREE_PIT;
      return;
    }
    const double row = std::floor(v / 12);
    const double off = (js::to_int32(row) & 1) ? 12 : 0;
    const bool joint = std::fmod(v, 12) == 0 || std::fmod(u + off, 24) == 0;
    out.mat = joint ? MAT::SIDEWALK_JOINT : (hash32(std::floor((hx + off) / 24), row, 0x71b) & 7u) == 0 ? MAT::PLAZA_STONE_DARK : MAT::PLAZA_STONE;
    return;
  }
  if (kind == "quay") {
    // a paved harbour apron: concrete slabs with joints, worn tracks
    const double ju = std::fmod(u, vx(6));
    const double jv = std::fmod(v, vx(6));
    out.mat = ju == 0 || jv == 0 ? MAT::CONCRETE_DARK : (hash32(std::floor(hx / vx(6)), std::floor(hy / vx(6)), 913) & 15u) == 0 ? MAT::CONCRETE_LIGHT : MAT::CONCRETE;
    return;
  }
  if (kind == "parking") {
    if (u < vx(1.5) || v < vx(1.5) || w - u < vx(1.5) || h - v < vx(1.5)) {
      out.mat = MAT::GRASS_LAWN;
      return;
    }
    const double row = std::floor((v - vx(1.5)) / vx(12));
    const double in_row = std::fmod(v - vx(1.5), vx(12));
    if (in_row >= vx(5) && in_row < vx(7)) {
      out.mat = MAT::ASPHALT;
      return;
    }
    const double su = std::fmod(u - vx(1.5), 20);
    out.mat = su == 0 ? MAT::LINE_WHITE : js::truthy(std::fmod(row, 2)) ? MAT::ASPHALT : MAT::ASPHALT_WORN;
    return;
  }
  if (kind == "sports") {
    const double m = vx(3);
    if (u < m || v < m || w - u < m || h - v < m) {
      out.mat = MAT::GRASS_LAWN;
      return;
    }
    const double iu = u - m;
    const double iv = v - m;
    const double cw = w - 2 * m;
    const double ch = h - 2 * m;
    const bool line = iu < 2 || iv < 2 || cw - iu < 2 || ch - iv < 2 || std::fabs(iu - cw / 2) < 1;
    out.mat = line ? MAT::LINE_WHITE : MAT::SPORT_COURT_GREEN;
    return;
  }
  if (kind == "courtyard") {
    // parking along the block edge, then lawns crossed by footpaths; worn patches of bare earth
    // (more of them in the projects)
    const double e = js::min(u, v, w - u, h - v);
    if (e < vx(6)) {
      const double along = e == u || e == w - u ? v : u;
      out.mat = e < vx(0.5) ? MAT::CURB : e < vx(5.5) && wrap_mod(along, 20) == 0 ? MAT::LINE_WHITE : MAT::ASPHALT_WORN;
      return;
    }
    if (e < vx(7.5)) {
      out.mat = MAT::PAVER_GRAY;
      return;
    }
    const double pu = wrap_mod(u + 90, 360);
    const double pv = wrap_mod(v + 90, 360);
    if (pu < 18 || pv < 18) {
      out.mat = MAT::ASPHALT_WORN;
      return;
    }
    const uint32_t h1 = hash32(std::floor(hx / 24), std::floor(hy / 24), 911) & 15u;
    const bool bleak = space.district == "projects";
    out.mat = h1 < (bleak ? 4u : 2u) ? MAT::DIRT : bleak && h1 < 8 ? MAT::GRASS_DRY : MAT::GRASS_LAWN;
    return;
  }
  if (kind == "square") {
    // market square: cobbles, a flagstone border along the houses and a flagstone walk across it,
    // a granite plinth round the monument in the middle
    const double e = js::min(u, v, w - u, h - v);
    const double d = js::hypot(u - w / 2, v - h / 2);
    if (e < vx(1.5))
      out.mat = MAT::FLAGSTONE;
    else if (d < vx(2.5))
      out.mat = MAT::GRANITE_LIGHT;
    else if (std::fabs(w >= h ? v - h / 2 : u - w / 2) < vx(1.2))
      out.mat = MAT::FLAGSTONE;
    else
      out.mat = cobble(hx, hy);
    return;
  }
  if (kind == "garden") {
    // a small town garden: lawn, a gravel cross and a round bed in the middle, beds along the edge
    const double e = js::min(u, v, w - u, h - v);
    const double d = js::hypot(u - w / 2, v - h / 2);
    if (e < vx(1.2))
      out.mat = MAT::HEDGE;
    else if (e < vx(2.4))
      out.mat = (js::to_int32(std::floor(hx / 6) + std::floor(hy / 6)) & 1) ? MAT::FLOWER_RED : MAT::SOIL_BED;
    else if (d < vx(2.5))
      out.mat = d < vx(1.6) ? MAT::FLOWER_YELLOW : MAT::SOIL_BED;
    else if (std::fabs(u - w / 2) < vx(0.8) || std::fabs(v - h / 2) < vx(0.8) || std::fabs(d - vx(3.2)) < vx(0.7))
      out.mat = MAT::GRAVEL;
    else
      out.mat = MAT::GRASS_LAWN;
    return;
  }
  if (kind == "cemetery") {
    cemetery_surface(space, x, y, hx, hy, out);
    return;
  }
  if (kind == "allotments") {
    allotment_surface(space, x, y, hx, hy, out);
    return;
  }
  if (kind == "garages") {
    // a garage cooperative's yard: worn asphalt, gravel and puddles of mud
    const uint32_t n = hash32(std::floor(hx / 24), std::floor(hy / 24), 931) & 15u;
    out.mat = n < 5 ? MAT::GRAVEL : n < 7 ? MAT::MUD : n < 8 ? MAT::ASPHALT_PATCH : MAT::ASPHALT_WORN;
    return;
  }
  if (kind == "wasteland") {
    // a vacant lot by the works: gravel, bare earth, rubble, dry grass and weeds
    const uint32_t a = hash32(std::floor(hx / 40), std::floor(hy / 40), 941) & 7u;
    const uint32_t b = hash32(std::floor(hx / 12), std::floor(hy / 12), 942) & 7u;
    out.mat = a < 2 ? (b < 3 ? MAT::GRAVEL : MAT::DIRT) : a < 4 ? (b < 2 ? MAT::CONCRETE_DARK : MAT::GRASS_DRY) : b < 2 ? MAT::MUD : b < 4 ? MAT::GRASS_DRY : MAT::GRASS;
    return;
  }
  if (kind == "tankFarm" || kind == "containerYard") {
    industry_surface(space, x, y, out.mat);
    return;
  }
  if (kind == "riverside") {
    // lawn with a paved promenade grid; the quay itself is cut by the river
    const double pu = wrap_mod(u, 96);
    const double pv = wrap_mod(v, 96);
    out.mat = pu < 12 || pv < 12 ? MAT::PLAZA_STONE : MAT::GRASS_LAWN;
    return;
  }
  // park, and any other kind
  park_surface(space, u, v, out, hx, hy);
}

uint16_t cobble(double hx, double hy) {
  const uint32_t k = hash32(std::floor(hx / 2), std::floor(hy / 2), 0xc0b) & 15u;
  return k < 3 ? MAT::COBBLE_DARK : k < 5 ? MAT::COBBLE_LIGHT : MAT::COBBLE;
}

const Frame& space_frame(const OpenSpace& space) {
  return space.frame.get([&] { return Frame(space.rect, space.front ? space.front : 'S'); });
}

}  // namespace svx::city
