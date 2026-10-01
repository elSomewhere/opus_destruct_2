// svx_city — voxel_city buildings/facade.js.
#include "buildings/facade.hpp"

#include "buildings/archetypes.hpp"
#include "core/math.hpp"
#include "svx/base/types.hpp"

namespace svx::city {

namespace {

int facade_cell_base(const BuildingLook& look, double len, double t, double zr, double H, double floor) {
  if (zr < 2) return WIN::SPANDREL;  // slab edge band
  if (floor == 0 && look.storefront) {
    if (zr < 3) return WIN::WALL;
    if (zr >= H - 4) return WIN::LINTEL;
    const double m = std::fmod(t, vx(3.0));
    if (m == 0 || t < 3 || t > len - 4) return WIN::FRAME;
    return WIN::STOREFRONT;
  }
  const double bay = look.bay;
  const double nb = js::max(1.0, std::floor(len / bay));
  const double margin = std::floor((len - nb * bay) / 2);
  const double tt = t - margin;
  if (tt < 0 || tt >= nb * bay) return WIN::WALL;
  const double in_bay = std::fmod(tt, bay);
  if (look.type == "open") {
    if (zr >= look.sill && zr < look.head) return in_bay < 3 ? WIN::FRAME : WIN::GLASS;
    if (zr == look.sill - 1) return WIN::SILL;
    return WIN::WALL;
  }
  if (look.type == "curtain") {
    if (zr >= H - 2) return WIN::SPANDREL;
    if (in_bay == 0) return WIN::FRAME;
    return WIN::GLASS;
  }
  const double w0 = std::floor((bay - look.win_w) / 2);
  const bool in_win = in_bay >= w0 && in_bay < w0 + look.win_w;
  // prefab panels: a joint line round every panel (one per bay and floor)
  const bool joint = look.joint != -1 && look.joint != 0;
  if (joint && !in_win && (in_bay == 0 || zr == 2)) return WIN::JOINT;
  if (joint && in_win && zr == 2) return WIN::JOINT;
  if (look.type == "ribbon") {
    if (zr >= look.sill && zr < look.head) return in_bay == 0 ? WIN::FRAME : WIN::GLASS;
    return WIN::WALL;
  }
  if (!in_win) {
    // casing: a trim surround beside the window, from the sill to the lintel
    if (look.casing && (in_bay == w0 - 1 || in_bay == w0 + look.win_w) && zr >= look.sill - 1 && zr <= look.head) return WIN::CASING;
    return WIN::WALL;
  }
  if (zr == look.sill - 1) return WIN::SILL;
  if (zr >= look.sill && zr < look.head) {
    const double mid = w0 + std::floor(look.win_w / 2);
    if (look.win_w >= 8 && in_bay == mid) return WIN::FRAME;
    if (look.transom && zr == look.sill + js::round((look.head - look.sill) * 0.62)) return WIN::FRAME;
    return WIN::GLASS;
  }
  if (zr == look.head && look.lintel) return WIN::LINTEL;
  return WIN::WALL;
}

}  // namespace

BuildingLook make_building_look(const Envelope& env, double seed) {
  Rng rng = Rng::from(seed, env.id, "look");
  BuildingLook look;
  look.style = resolve_style(env.style, rng);
  const StyleWindow& w = look.style.window;
  const double bay = js::max(vx(w.bay), vx(1.0));
  look.bay = bay;
  look.win_w = js::min(vx(w.width), bay - 2);
  look.sill = vx(w.sill) + 2;
  look.head = vx(w.head) + 2;
  look.type = w.type;
  look.lintel = w.lintel;
  look.storefront = env.program.ground == "retail" || env.program.ground == "officeLobby" || (env.extra && env.extra->storefront);
  look.lit_seed = rng.int_(0, 1 << 30);
  look.joint = look.style.joint;
  look.casing = w.casing;
  look.transom = w.transom;
  // any of the nordic wall treatments (seams, corner boards)
  look.boards = (look.style.seam != -1 && look.style.seam != 0) || look.style.corners;
  look.seed = seed;
  return look;
}

const BuildingLook& building_look(const Envelope& env, double seed) {
  const BuildingLook& look = env.look_cache.get([&] { return make_building_look(env, seed); });
  if (!(look.seed == seed)) SVX_FAIL("facade: an envelope's look asked for with two seeds");
  return look;
}

int facade_cell(const BuildingLook& look, double len, double t, double zr, double H, double floor) {
  const int cls = facade_cell_base(look, len, t, zr, H, floor);
  return cls == WIN::WALL ? wall_class(look, len, t, zr) : cls;
}

int wall_class(const BuildingLook& look, double len, double t, double zr) {
  if (!look.boards) return WIN::WALL;
  const ResolvedStyle& st = look.style;
  if (st.corners && (t < 2 || t >= len - 2)) return WIN::CORNER;
  if (st.seam != -1 && st.seam != 0 && std::fmod(st.seam_dir == 'h' ? zr : t, 3) == 0) return WIN::SEAM;
  return WIN::WALL;
}

int facade_material(const BuildingLook& look, int cls, double floor, double zr) {
  const ResolvedStyle& st = look.style;
  switch (cls) {
    case WIN::GLASS:
    case WIN::STOREFRONT:
      return st.glass;
    case WIN::FRAME:
      return floor == 0 && look.storefront ? st.storefront : st.frame;
    case WIN::SILL:
    case WIN::LINTEL:
      return st.trim;
    case WIN::JOINT:
      return st.joint != -1 ? st.joint : st.wall;
    case WIN::SEAM:
      if (floor == 0 && ((look.storefront && st.shop_base) || zr < st.base_h)) return st.base;
      return st.seam;
    case WIN::CASING:
      return st.trim;
    case WIN::CORNER:
      if (floor == 0 && zr < st.base_h) return st.base;
      return st.trim;
    case WIN::SPANDREL:
      if (st.accent_strips && floor > 0 && std::fmod(floor, 3) == 0 && zr == 1) return st.trim;
      return look.type == "curtain" ? st.frame : floor == 0 ? st.base : st.wall;
    default:
      if (floor == 0 && ((look.storefront && st.shop_base) || zr < st.base_h)) return st.base;
      return st.wall;
  }
}

}  // namespace svx::city
